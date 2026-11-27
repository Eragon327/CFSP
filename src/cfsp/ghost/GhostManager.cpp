#include <winsock2.h> // must stay above the Bedrock headers

#include "cfsp/ghost/GhostManager.h"

#include "cfsp/CFSP.h"
#include "cfsp/ghost/GhostActions.h"
#include "cfsp/ghost/GhostPeer.h"
#include "cfsp/ghost/GhostSession.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"
#include "ll/api/event/server/ServerStartedEvent.h"
#include "ll/api/event/server/ServerStoppingEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/Versions.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/common/SubClientId.h"
#include "mc/deps/certificates/WebToken.h"
#include "mc/deps/core/utility/Util.h"
#include "mc/deps/identity/edu_common/MessToken.h"
#include "mc/deps/input/InputMode.h"
#include "mc/deps/json/Reader.h"
#include "mc/deps/json/Value.h"
#include "mc/entity/components/ServerMovementAuthorityType.h"
#include "mc/entity/components/ServerPlayerMovementComponent.h"
#include "mc/network/ConnectionRequest.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/NetworkSystem.h"
#include "mc/network/Packet.h"
#include "mc/network/RemoteConnector.h"
#include "mc/network/ServerNetworkHandler.h"
#include "mc/network/VanillaServerNetworkHandler.h"
#include "mc/network/connection/DisconnectFailReason.h"
#include "mc/network/packet/LoginPacket.h"
#include "mc/network/packet/PlayerActionPacket.h"
#include "mc/network/packet/PlayerActionType.h"
#include "mc/network/packet/PlayerAuthInputPacket.h"
#include "mc/network/packet/PlayerListPacket.h"
#include "mc/network/packet/PlayerListPacketPayload.h"
#include "mc/network/packet/PlayerRespawnState.h"
#include "mc/network/packet/RequestChunkRadiusPacket.h"
#include "mc/network/packet/ResourcePackClientResponsePacket.h"
#include "mc/network/packet/RespawnPacket.h"
#include "mc/network/packet/ServerboundLoadingScreenPacket.h"
#include "mc/network/packet/SetActorDataPacket.h"
#include "mc/network/packet/SetLocalPlayerAsInitializedPacket.h"
#include "mc/network/packet/UpdateAbilitiesPacket.h"
#include "mc/platform/UUID.h"
#include "mc/server/ServerPlayer.h"
#include "mc/world/actor/player/PlayerListEntry.h"
#include "mc/world/actor/player/SerializedSkinImpl.h"
#include "mc/world/actor/player/SerializedSkinRef.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/material/Material.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace coral_fans::cfsp::ghost {

namespace {

constexpr int      kStageTimeout      = 200;  // ticks a login step may take before giving up
constexpr int      kPackStackFallback = 60;   // ticks to wait for ResourcePackStack
constexpr int      kReadyTimeout      = 2400; // ticks the server may take to finish loading the player
constexpr int      kStuckTimeout      = 100;  // ticks without progress before a walk is abandoned
constexpr float    kArriveDistance    = 0.15f;
constexpr float    kPassDistance      = 0.35f;
constexpr float    kObstacleReach     = 0.7f; // from the centre of the ghost: half its width plus a margin
constexpr float    kStepHeight        = 0.6f;
constexpr float    kDegToRad          = 0.017453292f;
constexpr uint64_t kXuidBase          = 900000000000ULL; // 12 digits after the "2535" prefix

ll::io::Logger& log() { return CFSP::getInstance().getSelf().getLogger(); }

char const* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string b64url(unsigned char const* data, size_t n) {
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = data[i] << 16;
        if (i + 1 < n) v |= data[i + 1] << 8;
        if (i + 2 < n) v |= data[i + 2];
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(i + 1 < n ? kB64[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < n ? kB64[v & 63] : '=');
    }
    while (!out.empty() && out.back() == '=') out.pop_back();
    return out;
}

// Names and xuids end up inside hand-built JSON.
std::string jsonEscape(std::string const& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        auto u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (u < 0x20) {
            char buf[8]{};
            std::snprintf(buf, sizeof(buf), "\\u%04x", u);
            out += buf;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string b64(unsigned char const* data, size_t n) { return Util::base64_encode(data, n, true); }

std::string b64(std::string const& s) {
    if (s.empty()) return {};
    return b64(reinterpret_cast<unsigned char const*>(s.data()), s.size());
}

std::string colorHex(mce::Color const& color) {
    auto channel = [](float v) {
        int i = static_cast<int>(std::lround(v * 255.0f));
        if (i < 0) i = 0;
        if (i > 255) i = 255;
        return i;
    };
    char buf[8]{};
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", channel(color.r), channel(color.g), channel(color.b));
    return buf;
}

// Vanilla humanoid geometry (geometry.humanoid.custom / customSlim). The trusted
// default SerializedSkin has no geometry data of its own; a real client needs this
// mesh when a custom skin is sent.
constexpr char const* kDefaultSkinGeometry = R"GEO(
{
   "format_version" : "1.12.0",
   "minecraft:geometry" : [
      {
         "bones" : [
            {"name":"body","parent":"waist","pivot":[0.0,24.0,0.0]},
            {"name":"waist","pivot":[0.0,12.0,0.0]},
            {"cubes":[{"origin":[-5.0,8.0,3.0],"size":[10,16,1],"uv":[0,0]}],"name":"cape","parent":"body","pivot":[0.0,24.0,3.0],"rotation":[0.0,180.0,0.0]}
         ],
         "description" : {"identifier":"geometry.cape","texture_height":32,"texture_width":64}
      },
      {
         "bones" : [
            {"name":"root","pivot":[0.0,0.0,0.0]},
            {"cubes":[{"origin":[-4.0,12.0,-2.0],"size":[8,12,4],"uv":[16,16]}],"name":"body","parent":"waist","pivot":[0.0,24.0,0.0]},
            {"name":"waist","parent":"root","pivot":[0.0,12.0,0.0]},
            {"cubes":[{"origin":[-4.0,24.0,-4.0],"size":[8,8,8],"uv":[0,0]}],"name":"head","parent":"body","pivot":[0.0,24.0,0.0]},
            {"name":"cape","parent":"body","pivot":[0.0,24,3.0]},
            {"cubes":[{"inflate":0.50,"origin":[-4.0,24.0,-4.0],"size":[8,8,8],"uv":[32,0]}],"name":"hat","parent":"head","pivot":[0.0,24.0,0.0]},
            {"cubes":[{"origin":[4.0,12.0,-2.0],"size":[4,12,4],"uv":[32,48]}],"name":"leftArm","parent":"body","pivot":[5.0,22.0,0.0]},
            {"cubes":[{"inflate":0.250,"origin":[4.0,12.0,-2.0],"size":[4,12,4],"uv":[48,48]}],"name":"leftSleeve","parent":"leftArm","pivot":[5.0,22.0,0.0]},
            {"name":"leftItem","parent":"leftArm","pivot":[6.0,15.0,1.0]},
            {"cubes":[{"origin":[-8.0,12.0,-2.0],"size":[4,12,4],"uv":[40,16]}],"name":"rightArm","parent":"body","pivot":[-5.0,22.0,0.0]},
            {"cubes":[{"inflate":0.250,"origin":[-8.0,12.0,-2.0],"size":[4,12,4],"uv":[40,32]}],"name":"rightSleeve","parent":"rightArm","pivot":[-5.0,22.0,0.0]},
            {"locators":{"lead_hold":[-6,15,1]},"name":"rightItem","parent":"rightArm","pivot":[-6,15,1]},
            {"cubes":[{"origin":[-0.10,0.0,-2.0],"size":[4,12,4],"uv":[16,48]}],"name":"leftLeg","parent":"root","pivot":[1.90,12.0,0.0]},
            {"cubes":[{"inflate":0.250,"origin":[-0.10,0.0,-2.0],"size":[4,12,4],"uv":[0,48]}],"name":"leftPants","parent":"leftLeg","pivot":[1.90,12.0,0.0]},
            {"cubes":[{"origin":[-3.90,0.0,-2.0],"size":[4,12,4],"uv":[0,16]}],"name":"rightLeg","parent":"root","pivot":[-1.90,12.0,0.0]},
            {"cubes":[{"inflate":0.250,"origin":[-3.90,0.0,-2.0],"size":[4,12,4],"uv":[0,32]}],"name":"rightPants","parent":"rightLeg","pivot":[-1.90,12.0,0.0]},
            {"cubes":[{"inflate":0.250,"origin":[-4.0,12.0,-2.0],"size":[8,12,4],"uv":[16,32]}],"name":"jacket","parent":"body","pivot":[0.0,24.0,0.0]}
         ],
         "description" : {"identifier":"geometry.humanoid.custom","texture_height":64,"texture_width":64,"visible_bounds_height":2,"visible_bounds_offset":[0,1,0],"visible_bounds_width":1}
      },
      {
         "bones" : [
            {"name":"root","pivot":[0.0,0.0,0.0]},
            {"name":"waist","parent":"root","pivot":[0.0,12.0,0.0]},
            {"cubes":[{"origin":[-4.0,12.0,-2.0],"size":[8,12,4],"uv":[16,16]}],"name":"body","parent":"waist","pivot":[0.0,24.0,0.0]},
            {"cubes":[{"origin":[-4.0,24.0,-4.0],"size":[8,8,8],"uv":[0,0]}],"name":"head","parent":"body","pivot":[0.0,24.0,0.0]},
            {"cubes":[{"inflate":0.50,"origin":[-4.0,24.0,-4.0],"size":[8,8,8],"uv":[32,0]}],"name":"hat","parent":"head","pivot":[0.0,24.0,0.0]},
            {"cubes":[{"origin":[-3.90,0.0,-2.0],"size":[4,12,4],"uv":[0,16]}],"name":"rightLeg","parent":"root","pivot":[-1.90,12.0,0.0]},
            {"cubes":[{"inflate":0.250,"origin":[-3.90,0.0,-2.0],"size":[4,12,4],"uv":[0,32]}],"name":"rightPants","parent":"rightLeg","pivot":[-1.90,12.0,0.0]},
            {"cubes":[{"origin":[-0.10,0.0,-2.0],"size":[4,12,4],"uv":[16,48]}],"mirror":true,"name":"leftLeg","parent":"root","pivot":[1.90,12.0,0.0]},
            {"cubes":[{"inflate":0.250,"origin":[-0.10,0.0,-2.0],"size":[4,12,4],"uv":[0,48]}],"name":"leftPants","parent":"leftLeg","pivot":[1.90,12.0,0.0]},
            {"cubes":[{"origin":[4.0,11.50,-2.0],"size":[3,12,4],"uv":[32,48]}],"name":"leftArm","parent":"body","pivot":[5.0,21.50,0.0]},
            {"cubes":[{"inflate":0.250,"origin":[4.0,11.50,-2.0],"size":[3,12,4],"uv":[48,48]}],"name":"leftSleeve","parent":"leftArm","pivot":[5.0,21.50,0.0]},
            {"name":"leftItem","parent":"leftArm","pivot":[6,14.50,1]},
            {"cubes":[{"origin":[-7.0,11.50,-2.0],"size":[3,12,4],"uv":[40,16]}],"name":"rightArm","parent":"body","pivot":[-5.0,21.50,0.0]},
            {"cubes":[{"inflate":0.250,"origin":[-7.0,11.50,-2.0],"size":[3,12,4],"uv":[40,32]}],"name":"rightSleeve","parent":"rightArm","pivot":[-5.0,21.50,0.0]},
            {"locators":{"lead_hold":[-6,14.50,1]},"name":"rightItem","parent":"rightArm","pivot":[-6,14.50,1]},
            {"cubes":[{"inflate":0.250,"origin":[-4.0,12.0,-2.0],"size":[8,12,4],"uv":[16,32]}],"name":"jacket","parent":"body","pivot":[0.0,24.0,0.0]},
            {"name":"cape","parent":"body","pivot":[0.0,24,-3.0]}
         ],
         "description" : {"identifier":"geometry.humanoid.customSlim","texture_height":64,"texture_width":64,"visible_bounds_height":2,"visible_bounds_offset":[0,1,0],"visible_bounds_width":1}
      }
   ]
}
)GEO";

// A real client crashes on a player-list entry whose SerializedSkin has no image
// (0x0 skin). The fake login therefore carries a real skin; BDS's trusted default
// skin is used as the template, with a generated 64x64 fallback image.
std::string makeSkinAuthJson(SerializedSkinRef const& skinRef) {
    auto& impl = skinRef.mSkinImpl.get()->mObject;

    auto const& skinImage = impl.mSkinImage.get();
    uint32_t    width     = skinImage.mWidth;
    uint32_t    height    = skinImage.mHeight;

    std::string skinB64;
    if (width > 0 && height > 0 && skinImage.mImageBytes.size() >= static_cast<size_t>(width) * height * 4) {
        skinB64 = b64(skinImage.mImageBytes.data(), static_cast<size_t>(width) * height * 4);
    } else {
        width  = 64;
        height = 64;
        std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
        for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
            pixels[i]     = 0x46;
            pixels[i + 1] = 0x8a;
            pixels[i + 2] = 0xd6;
            pixels[i + 3] = 0xff;
        }
        skinB64 = b64(pixels.data(), pixels.size());
    }

    std::string resourcePatch = impl.mResourcePatch.get();
    if (resourcePatch.empty()) resourcePatch = R"({"geometry":{"default":"geometry.humanoid.custom"}})";

    std::string geometry;
    if (!impl.mGeometryData.get().isNull()) geometry = impl.mGeometryData.get().toStyledString();
    if (geometry.empty()) geometry = kDefaultSkinGeometry;

    std::string geometryVersion = impl.mGeometryDataMinEngineVersion.get().mSemVersion.get().asString();
    if (geometryVersion.empty()) geometryVersion = "1.16.0";

    auto const& capeImage = impl.mCapeImage.get();
    std::string capeB64;
    if (capeImage.mWidth > 0 && capeImage.mHeight > 0
        && capeImage.mImageBytes.size() >= static_cast<size_t>(capeImage.mWidth) * capeImage.mHeight * 4) {
        capeB64 = b64(capeImage.mImageBytes.data(), static_cast<size_t>(capeImage.mWidth) * capeImage.mHeight * 4);
    }

    std::string skinId = impl.mId.get();
    if (skinId.empty()) skinId = "Standard_Custom";

    std::string out;
    out.reserve(skinB64.size() + geometry.size() + 512);
    out += R"(,"SkinId":")" + skinId + "\"";
    out += R"(,"SkinData":")" + skinB64 + "\"";
    out += R"(,"SkinImageWidth":)" + std::to_string(width);
    out += R"(,"SkinImageHeight":)" + std::to_string(height);
    out += R"(,"SkinResourcePatch":")" + b64(resourcePatch) + "\"";
    out += R"(,"SkinGeometryData":")" + b64(geometry) + "\"";
    out += R"(,"SkinGeometryDataEngineVersion":")" + geometryVersion + "\"";
    out += R"(,"CapeData":")" + capeB64 + "\"";
    out += R"(,"CapeImageWidth":)" + std::to_string(capeImage.mWidth);
    out += R"(,"CapeImageHeight":)" + std::to_string(capeImage.mHeight);
    out += R"(,"CapeId":")" + impl.mCapeId.get() + "\"";
    out += R"(,"ArmSize":"wide")";
    out += R"(,"SkinColor":")" + colorHex(impl.mSkinColor.get()) + "\"";
    out += R"(,"AnimatedImageData":[])";
    out += R"(,"PersonaPieces":[])";
    out += R"(,"PieceTintColors":{})";
    out += R"(,"TrustedSkin":true)";
    out += R"(,"PremiumSkin":false)";
    out += R"(,"PersonaSkin":false)";
    out += R"(,"CapeOnClassicSkin":false)";
    out += R"(,"PrimaryUser":false)";
    out += R"(,"OverrideSkin":false)";
    return out;
}

// The port the server really listens on. A dedicated server is not necessarily on
// 19132, and a single-player world may have no remote connector at all.
uint16_t listeningPort(NetworkSystem& network) {
    if (auto* connector = network.mRemoteConnector.get()) {
        if (auto port = connector->getIPv4Port()) return port;
        if (auto port = connector->getIPv6Port()) return port;
    }
    return 19132;
}

// The version of the game this mod is running in, as a client reports it at login.
std::string gameVersion() {
    auto version = ll::getGameVersion();
    return std::to_string(version.major) + "." + std::to_string(version.minor) + "."
         + std::to_string(version.patch);
}

// The token is deliberately fake (no real signature); only its payload is read.
std::string makeConnectionJson(
    GhostSession const&      session,
    SerializedSkinRef const* skinTemplate,
    uint16_t                 serverPort,
    std::string&             tokenOut
) {
    std::string const header =
        R"({"alg":"ES384","x5u":"MHYwEAYHKoZIzj0CAQYFK4EEACIDYgAE8S0bHR0HhZhmwpLV4hrVPg+PvUVD8QV2gf02f3qrS+eG0iQxCW6TYP/naDQK1ACLe9FUhc6f1VJ+2iilNzwu3NuKCibXJ0pPWdBY07X3U6ckidcqhdm6UHq19eAA9c4Q"})";

    char mid[17]{};
    std::snprintf(
        mid,
        sizeof(mid),
        "%016llX",
        static_cast<unsigned long long>(std::hash<std::string>{}(session.xuid + "#mid"))
    );

    std::string skinJson = makeSkinAuthJson(*skinTemplate);
    std::string name     = jsonEscape(session.name);
    std::string xuid     = jsonEscape(session.xuid);

    std::string payload;
    payload.reserve(512 + skinJson.size());
    payload += R"({"ap":0,"aud":"api://auth-minecraft-services/multiplayer",)";
    payload +=
        R"("cpk":"MHYwEAYHKoZIzj0CAQYFK4EEACIDYgAE8S0bHR0HhZhmwpLV4hrVPg+PvUVD8QV2gf02f3qrS+eG0iQxCW6TYP/naDQK1ACLe9FUhc6f1VJ+2iilNzwu3NuKCibXJ0pPWdBY07X3U6ckidcqhdm6UHq19eAA9c4Q",)";
    payload += R"("exp":2000000000,"identity":")" + session.uuid + R"(","leguuid":")" + session.uuid + R"(",)"
             + R"("mid":")" + mid + R"(","nid":"","nname":"","pid":"","pname":"",)" + R"("xid":")" + xuid
             + R"(","xname":")" + name + R"(")";
    payload += R"(,"DeviceId":")" + session.uuid + R"(","SelfSignedId":")" + session.uuid + R"(",)"
             + R"("ClientRandomId":)" + std::to_string(std::hash<std::string>{}(session.xuid + "#random"))
             + R"(,"PlatformOnlineId":")" + mid + R"(","DeviceOS":4,"BuildPlatform":4,)"
             + R"("GameVersion":")" + gameVersion() + R"(","LanguageCode":"en_US",)"
             + R"("CurrentInputMode":1,"DefaultInputMode":1,)"
             + R"("GuiScale":0,"ServerAddress":"127.0.0.1:)" + std::to_string(serverPort)
             + R"(","ThirdPartyName":")" + name + R"(")";
    // ConnectionRequest reads the skin from the token payload, not from the outer
    // authentication object.
    payload += skinJson;
    payload += "}";

    unsigned char sig[96]{};
    std::string   token = b64url(reinterpret_cast<unsigned char const*>(header.data()), header.size()) + "."
                        + b64url(reinterpret_cast<unsigned char const*>(payload.data()), payload.size()) + "."
                        + b64url(sig, sizeof(sig));
    tokenOut            = token;

    return std::string(R"({"AuthenticationType":2,"Token":")") + token + "\"}";
}

uint64_t makeGuid(std::string const& xuid) {
    uint64_t guid = std::hash<std::string>{}(xuid + "#guid");
    return guid == 0 ? 1 : guid;
}

std::array<unsigned char, 16> makeUuid(std::string const& seed) {
    uint64_t                      a = std::hash<std::string>{}(seed + "#uuid-a");
    uint64_t                      b = std::hash<std::string>{}(seed + "#uuid-b");
    std::array<unsigned char, 16> uuid{};
    std::memcpy(uuid.data(), &a, sizeof(a));
    std::memcpy(uuid.data() + sizeof(a), &b, sizeof(b));
    uuid[6] = static_cast<unsigned char>((uuid[6] & 0x0F) | 0x40); // version 4
    uuid[8] = static_cast<unsigned char>((uuid[8] & 0x3F) | 0x80); // RFC 4122 variant
    return uuid;
}

std::string uuidToString(std::array<unsigned char, 16> const& u) {
    char buf[37]{};
    std::snprintf(
        buf,
        sizeof(buf),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        u[0],
        u[1],
        u[2],
        u[3],
        u[4],
        u[5],
        u[6],
        u[7],
        u[8],
        u[9],
        u[10],
        u[11],
        u[12],
        u[13],
        u[14],
        u[15]
    );
    return buf;
}

// PlayerAuthenticationInfo's constructor is not linkable; zero bytes + placement new
// for the two strings the login path actually reads. The remaining std::string members
// stay zeroed, which is a valid empty-string representation for MSVC's std::string.
void initIdentity(GhostSession& session) {
    std::memset(session.idStorage, 0, sizeof(session.idStorage));
    session.id           = reinterpret_cast<NetworkIdentifier*>(session.idStorage);
    session.id->mType    = NetworkIdentifier::Type::RakNet;
    session.guid         = makeGuid(session.xuid);
    session.id->mGuid    = RakNet::RakNetGUID{session.guid, 0};
    auto& addr           = session.id->mSock.as<sockaddr_in>();
    addr.sin_family      = AF_INET;
    addr.sin_port        = __builtin_bswap16(19132);
    addr.sin_addr.s_addr = __builtin_bswap32(INADDR_LOOPBACK);

    std::memset(session.authInfoStorage, 0, sizeof(session.authInfoStorage));
    session.authInfo = reinterpret_cast<PlayerAuthenticationInfo*>(session.authInfoStorage);
    auto& info       = *session.authInfo;
    char  pfid[17]{};
    std::snprintf(
        pfid,
        sizeof(pfid),
        "%016llX",
        static_cast<unsigned long long>(std::hash<std::string>{}(session.xuid + "#pfid"))
    );
    new (std::addressof(info.Xuid.get())) std::string(session.xuid);
    new (std::addressof(info.PlayFabId.get())) std::string(pfid);
    new (std::addressof(info.XboxLiveName.get())) std::string(session.name);
    new (std::addressof(info.BestDisplayName.get())) std::string(session.name);
    session.stringsConstructed = true;
    info.IsHost                = false;
    info.IsLocal               = false;

    auto uuid                    = makeUuid(session.xuid + ":" + session.name);
    session.uuid                 = uuidToString(uuid);
    info.AuthenticatedUuid.get() = mce::UUID::fromString(session.uuid);
}

// std::optional<MessToken>'s destructor is not linkable; pass a disengaged (all-zero)
// optional by raw storage reference. The login path only reads it.
std::optional<MessToken> const& hostMessToken() {
    alignas(std::optional<MessToken>) static std::byte storage[sizeof(std::optional<MessToken>)]{};
    std::memset(storage, 0, sizeof(storage));
    return *reinterpret_cast<std::optional<MessToken> const*>(storage);
}

// ResourcePackClientResponsePacket's default ctor is not linkable on either platform,
// but the payload ctor is. On a dedicated server the payload is filled in place; on the
// client build its default ctor is declared "prevent constructor by default", so the
// payload has to be built from the response enum instead. Dispatch must go through the
// game-specific callback, not through ServerNetworkHandler's base NetEventCallback.
void sendPackResponse(ServerNetworkHandler& handler, NetworkIdentifier const& id, bool stackFinished) {
#ifdef LL_PLAT_C
    ResourcePackClientResponsePacketPayload payload(
        stackFinished ? ResourcePackResponse::ResourcePackStackFinished : ResourcePackResponse::DownloadingFinished
    );
#else
    ResourcePackClientResponsePacketPayload payload;
    if (stackFinished) {
        payload.mResponse.get() = ResourcePackClientResponsePacketPayload::ResourcePackStackFinished{};
    } else {
        payload.mResponse.get() = ResourcePackClientResponsePacketPayload::DownloadingFinished{};
    }
#endif
    ResourcePackClientResponsePacket pkt{std::move(payload)};
    if (auto* gameSpecific = handler.getGameSpecificNetEventCallback()) {
        static_cast<VanillaServerNetworkHandler*>(gameSpecific)->handle(id, pkt);
    }
}

// Answers whatever the server is waiting for before it hands movement back to the
// client: the end of the loading screen, or a finished dimension change.
void acknowledgeAuthority(ServerNetworkHandler& handler, GhostSession& session, ServerPlayer& player) {
    auto authority = player.getEntityContext().tryGetComponent<ServerPlayerMovementComponent>();
    if (!authority) return;
    auto& bits = *authority->mServerHasMovementAuthority;

    if (bits.test(static_cast<size_t>(ServerMovementAuthorityType::DimensionTransfer))) {
        if (auto raw = MinecraftPackets::createPacket(MinecraftPacketIds::PlayerAction)) {
            auto& pkt      = static_cast<PlayerActionPacket&>(*raw);
            pkt.mAction    = PlayerActionType::ChangeDimensionAck;
            pkt.mRuntimeId = player.getRuntimeID();
            handler.handle(*session.id, pkt);
        }
    }
    if (bits.test(static_cast<size_t>(ServerMovementAuthorityType::LoadingScreen))) {
        if (auto raw = MinecraftPackets::createPacket(MinecraftPacketIds::ServerboundLoadingScreenPacket)) {
            auto& pkt                               = static_cast<ServerboundLoadingScreenPacket&>(*raw);
            pkt.mServerboundLoadingScreenPacketType = ServerboundLoadingScreenPacketType::EndLoadingScreen;
            handler.handle(*session.id, pkt);
        }
    }
}

} // namespace

// The server sends the player-list Add entry for the ghost before its skin has been
// applied, so the entry carries a 0x0 SerializedSkin. Real clients treat that as a
// malformed packet and drop the connection with a data error ("Block"). Suppress that
// entry and resend a correct one after the ghost has spawned.
LL_TYPE_INSTANCE_HOOK(
    GhostPlayerListHook,
    ll::memory::HookPriority::High,
    NetworkSystem,
    &NetworkSystem::send,
    void,
    NetworkIdentifier const& id,
    Packet const&            packet,
    SubClientId              subId
) {
    auto& manager = coral_fans::cfsp::ghost::GhostManager::getInstance();
    if (manager.shouldDropGhostPacket(packet)) return;
    if (packet.getId() == MinecraftPacketIds::PlayerList
        && manager.shouldDropPlayerListAdd(static_cast<PlayerListPacket const&>(packet))) {
        return;
    }
    origin(id, packet, subId);
}

LL_TYPE_INSTANCE_HOOK(
    GhostPlayerListMultiHook,
    ll::memory::HookPriority::High,
    NetworkSystem,
    &NetworkSystem::sendToMultiple,
    void,
    std::vector<NetworkIdentifierWithSubId> const& ids,
    Packet const&                                  packet
) {
    auto& manager = coral_fans::cfsp::ghost::GhostManager::getInstance();
    if (manager.shouldDropGhostPacket(packet)) return;
    if (packet.getId() == MinecraftPacketIds::PlayerList
        && manager.shouldDropPlayerListAdd(static_cast<PlayerListPacket const&>(packet))) {
        return;
    }
    origin(ids, packet);
}

GhostManager& GhostManager::getInstance() {
    static GhostManager instance;
    return instance;
}

GhostManager::GhostManager()  = default;
GhostManager::~GhostManager() = default;

void GhostManager::init() {
    if (mTickHooked) return;
    mTickHooked = true;

    GhostPlayerListHook::hook();
    GhostPlayerListMultiHook::hook();
    mSendHooked = true;

    auto& bus = ll::event::EventBus::getInstance();
    bus.emplaceListener<ll::event::world::ServerLevelTickEvent>([](ll::event::world::ServerLevelTickEvent&) {
        GhostManager::getInstance().onTick();
    });
    // The server is about to take every player down itself; that is not a ghost
    // being lost, so stop watching them.
    bus.emplaceListener<ll::event::server::ServerStoppingEvent>([](ll::event::server::ServerStoppingEvent&) {
        auto& self     = GhostManager::getInstance();
        self.mStopping = true;
        self.mSessions.clear();
    });
    // A client can leave and enter single-player worlds without unloading the mod, so
    // a previous world's shutdown must not refuse logins in the next one.
    bus.emplaceListener<ll::event::server::ServerStartedEvent>([](ll::event::server::ServerStartedEvent&) {
        GhostManager::getInstance().mStopping = false;
    });
    // A ghost the server removes on its own (kick, duplicate login) is reported
    // while its player still exists, so the owner can save it one last time. During a
    // shutdown that is not a loss, and saving a player once the main loop has ended
    // crashes, so ignore those.
    bus.emplaceListener<ll::event::player::PlayerDisconnectEvent>([](ll::event::player::PlayerDisconnectEvent& ev) {
        auto& self = GhostManager::getInstance();
        if (self.mStopping) return;
        if (auto session = self.find(ev.self())) self.lose(session, &ev.self());
    });
}

// Forgets a session the server dropped and tells its owner. player is non-null while
// the ServerPlayer is still valid.
void GhostManager::lose(std::shared_ptr<GhostSession> const& session, ServerPlayer* player) {
    auto it = std::find(mSessions.begin(), mSessions.end(), session);
    if (it == mSessions.end()) return; // already logged out
    mSessions.erase(it);

    session->stage   = GhostStage::Failed;
    session->player  = nullptr;
    session->onReady = nullptr;
    if (auto lost = std::exchange(session->onLost, nullptr)) lost(player);
}

void GhostManager::shutdown() {
    mSessions.clear();
    if (mSendHooked) {
        GhostPlayerListMultiHook::unhook();
        GhostPlayerListHook::unhook();
        mSendHooked = false;
    }
}

void GhostManager::forgetSessions() {
    mSessions.clear();
    mStopping = true;
}

std::shared_ptr<GhostSession> GhostManager::find(std::string const& name) const {
    for (auto const& s : mSessions)
        if (s->name == name) return s;
    return nullptr;
}

std::shared_ptr<GhostSession> GhostManager::find(Player const& player) const {
    for (auto const& s : mSessions)
        if (s->player == &player) return s;
    return nullptr;
}

bool GhostManager::isGhost(Player const& player) const { return find(player) != nullptr; }

bool GhostManager::logout(std::string const& name) {
    auto it = std::find_if(mSessions.begin(), mSessions.end(), [&](auto const& s) { return s->name == name; });
    if (it == mSessions.end()) return false;

    auto session = *it;
    mSessions.erase(it);
    session->onReady = nullptr;
    session->onLost  = nullptr;
    session->player  = nullptr;
    session->stage   = GhostStage::Failed;
    if (auto handler = ll::service::bedrock::getServerNetworkHandler()) {
        if (handler->mClients.get().contains(*session->id)) {
            handler->disconnectClient(
                *session->id,
                SubClientId::PrimaryClient,
                Connection::DisconnectFailReason::Disconnected
            );
        }
    }
    return true;
}

bool GhostManager::shouldDropGhostPacket(Packet const& packet) {
    if (mSessions.empty()) return false;

    // Once a ghost is online these packets are what shows other players its pose
    // (sneaking, swimming), its abilities and that it is alive again after a
    // respawn, so they are only held back while it is still logging in.
    auto drops = [](GhostSession const& session) { return session.player && session.stage != GhostStage::Online; };

    if (packet.getId() == MinecraftPacketIds::UpdateAbilitiesPacket) {
        auto& abilities = static_cast<UpdateAbilitiesPacket const&>(packet);
        auto  target    = static_cast<int64_t>(abilities.mData.get().mTargetPlayer.get().rawID);
        for (auto const& session : mSessions)
            if (session->uniqueId == target && drops(*session)) return true;
    } else if (packet.getId() == MinecraftPacketIds::SetActorData) {
        auto&    data = static_cast<SetActorDataPacket const&>(packet);
        uint64_t id   = data.mId.get().rawID;
        for (auto const& session : mSessions)
            if (session->runtimeId == id && drops(*session)) return true;
    }
    return false;
}

bool GhostManager::shouldDropPlayerListAdd(PlayerListPacket const& packet) {
    if (mSessions.empty()) return false;

    bool hasGhost = false;
    bool hasOther = false;
    bool drop     = false;

    for (auto const& entry : packet.mEntries.get()) {
        if (entry.index() != 1) { // 0 = RemoveEntry, 1 = AddEntry
            hasOther = true;
            continue;
        }
        auto const& add  = std::get<1>(entry);
        std::string uuid = add.mUUID.get().asString();
        auto it = std::find_if(mSessions.begin(), mSessions.end(), [&](auto const& s) { return s->uuid == uuid; });
        if (it == mSessions.end() || (*it)->stage >= GhostStage::WaitReady) {
            hasOther = true;
            continue;
        }
        hasGhost = true;

        bool  empty = true;
        auto& skin  = add.mSkin.get();
        auto* impl  = skin.mSkinImpl.get();
        if (impl) {
            auto& obj = impl->mObject;
            empty     = obj.mSkinImage.get().mWidth == 0 || obj.mSkinImage.get().mImageBytes.size() == 0;
        }
        if (empty) {
            drop                        = true;
            (*it)->playerListSuppressed = true;
        }
    }
    return drop && hasGhost && !hasOther;
}

void GhostManager::sendPlayerListAdd(GhostSession&, ServerPlayer& player) {
    auto handlerOpt = ll::service::bedrock::getServerNetworkHandler();
    auto networkOpt = ll::service::bedrock::getNetworkSystem();
    if (!handlerOpt || !networkOpt) return;

    PlayerListEntry  entry(player);
    PlayerListPacket packet;
    packet.mEntries.get().emplace_back(PlayerListPacketPayload::AddEntry{entry});

    for (auto const& [id, client] : handlerOpt->mClients.get()) {
        networkOpt->send(id, packet, SubClientId::PrimaryClient);
    }
}

GhostManager::LoginResult GhostManager::login(
    std::string                        name,
    std::string                        xuid,
    std::function<void(ServerPlayer&)> onReady,
    std::function<void(ServerPlayer*)> onLost
) {
    // A client can leave and enter single-player worlds in one process, and the
    // shutdown of the previous world must not refuse logins in the next one. Commands
    // cannot run while a server is actually stopping, so a login attempt with a level
    // loaded means we are in a live world again.
    if (mStopping && ll::service::getLevel()) mStopping = false;
    if (mStopping) return {false, "the server is stopping"};
#ifdef LL_PLAT_C
    // The fake login feeds the server's own login flow, so it needs the integrated
    // server of a single-player world. getLevel() is the cheap way to tell: it is only
    // set while such a world is loaded (main menu and remote multiplayer have none).
    if (!ll::service::getLevel()) {
        log().warn("[ghost] a fake player can only be created in a single-player world");
        return {false, "no single-player world is loaded"};
    }
#endif
    if (name.empty()) {
        uint64_t i = 1;
        do {
            name = "Ghost_" + std::to_string(i++);
        } while (find(name));
    }
    if (xuid.empty()) {
        uint64_t i = 1;
        do {
            xuid = "2535" + std::to_string(kXuidBase + i++);
        } while (std::any_of(mSessions.begin(), mSessions.end(), [&](auto const& s) { return s->xuid == xuid; }));
    }

    if (find(name)) return {false, "a ghost with that name already exists"};
    if (std::any_of(mSessions.begin(), mSessions.end(), [&](auto const& s) { return s->xuid == xuid; })) {
        return {false, "a ghost with that xuid already exists"};
    }

    auto handlerOpt = ll::service::bedrock::getServerNetworkHandler();
    auto networkOpt = ll::service::bedrock::getNetworkSystem();
    if (!handlerOpt || !networkOpt) {
        log().warn("[ghost] the server network is not ready yet");
        return {false, "server network is not ready"};
    }
    auto& handler = *handlerOpt;
    auto& network = *networkOpt;

    auto session     = std::make_shared<GhostSession>();
    session->name    = std::move(name);
    session->xuid    = std::move(xuid);
    session->onReady = std::move(onReady);
    session->onLost  = std::move(onLost);
    initIdentity(*session);

    // The client rejects a player whose SerializedSkin is empty. Always use BDS's
    // trusted default 64x64 humanoid skin: an online player's skin may be a 256x256
    // persona/custom skin whose geometry does not survive this fake-login path.
    SerializedSkinRef defaultSkin = SerializedSkinRef::createTrustedDefaultSerializedSkin();

    std::string tokenString;
    std::string blob = makeConnectionJson(*session, &defaultSkin, listeningPort(network), tokenString);

    Json::Value  authData;
    Json::Reader reader;
    if (!reader.parse(blob, authData, false)) {
        return {false, "failed to build the connection request JSON"};
    }

    // From here on the session is peer-owned too, so it stays alive even if the
    // login immediately starts sending packets.
    auto peer = std::make_shared<GhostPeer>(session);
    if (!network.onNewIncomingConnection(*session->id, std::move(peer))) {
        return {false, "onNewIncomingConnection refused the fake peer"};
    }
    mSessions.push_back(session);

    // _processValidatedLoginPacket expects a Client record for this id, otherwise it
    // returns early and nothing happens.
    auto request = std::make_unique<ConnectionRequest>(WebToken::parse(tokenString, 1u << 20), authData);
    handler.mClients.get().insert_or_assign(
        *session->id,
        std::make_unique<ServerNetworkHandler::Client>(std::move(request), *session->authInfo)
    );

    auto loginPacket                   = std::make_shared<LoginPacket>();
    loginPacket->mClientNetworkVersion = ll::getNetworkProtocolVersion(); // what a matching client reports
    loginPacket->mConnectionRequest =
        std::make_unique<ConnectionRequest>(WebToken::parse(tokenString, 1u << 20), authData);

    handler._processValidatedLoginPacket(*session->id, loginPacket, *session->authInfo, hostMessToken());

    // The server answers each handshake step synchronously, so the whole exchange
    // normally completes right here and the ServerPlayer already exists on return.
    for (int i = 0; i < 8 && stepLogin(*session); ++i) {}

    if (session->stage == GhostStage::Failed) {
        logout(session->name);
        return {false, "the server rejected the ghost login"};
    }
    return {
        true,
        "ghost login submitted: " + session->name + (session->player ? " (player created)" : " (player pending)"),
        session,
        session->player
    };
}

void GhostManager::fail(GhostSession& session, char const* why) {
    if (session.stage == GhostStage::Failed) return;
    session.stage  = GhostStage::Failed;
    session.player = nullptr;
    log().error("[ghost:{}] login failed: {}", session.name, why);
}

// Advances the client half of the login by one step. Returns true when a step was
// taken, so the caller can immediately try the next one.
bool GhostManager::stepLogin(GhostSession& session) {
    auto handlerOpt = ll::service::bedrock::getServerNetworkHandler();
    if (!handlerOpt) return false;
    auto& handler = *handlerOpt;
    auto& clients = handler.mClients.get();

    auto it = clients.find(*session.id);
    if (it == clients.end()) {
        fail(session, "client record disappeared");
        return false;
    }
    int  elapsed = session.ticks - session.stageTick;
    auto next    = [&](GhostStage stage) {
        session.stage     = stage;
        session.stageTick = session.ticks;
        return true;
    };

    switch (session.stage) {
    case GhostStage::WaitPacksInfo:
        if (!session.sawResourcePacksInfo) {
            if (elapsed > kStageTimeout) fail(session, "no ResourcePacksInfo");
            return false;
        }
        handler.completeHandshake(*session.id);
        sendPackResponse(handler, *session.id, false); // DownloadingFinished
        return next(GhostStage::WaitPackStack);

    case GhostStage::WaitPackStack:
        // Tolerate a missing ResourcePackStack: answer anyway after a while.
        if (!session.sawResourcePackStack && elapsed < kPackStackFallback) return false;
        sendPackResponse(handler, *session.id, true); // ResourcePackStackFinished
        return next(GhostStage::WaitPlayer);

    case GhostStage::WaitPlayer: {
        auto* sp = handler._getServerPlayer(*session.id, SubClientId::PrimaryClient);
        if (!sp) {
            if (elapsed > kStageTimeout) fail(session, "no ServerPlayer created");
            return false;
        }
        session.player    = sp;
        session.uniqueId  = sp->getOrCreateUniqueID().rawID;
        session.runtimeId = sp->getRuntimeID().rawID;

        RequestChunkRadiusPacket radius;
        radius.mChunkRadius    = 4;
        radius.mMaxChunkRadius = 4;
        handler.handle(*session.id, radius);

        SetLocalPlayerAsInitializedPacket initialized;
        initialized.mPlayerID = sp->getRuntimeID();
        handler.handle(*session.id, initialized);
        return next(GhostStage::WaitSpawned);
    }

    case GhostStage::WaitSpawned: {
        auto* sp = handler._getServerPlayer(*session.id, SubClientId::PrimaryClient);
        if (!sp) {
            fail(session, "ServerPlayer vanished during login");
            return false;
        }
        session.player = sp;
        if (static_cast<int>(it->second->mLoginState)
            < static_cast<int>(ServerNetworkHandler::Client::LoginState::PlayerSpawned)) {
            if (elapsed > kStageTimeout) fail(session, "login state never reached PlayerSpawned");
            return false;
        }
        if (session.playerListSuppressed) {
            session.playerListSuppressed = false;
            sendPlayerListAdd(session, *sp);
        }
        return next(GhostStage::WaitReady);
    }

    case GhostStage::WaitReady: {
        auto* sp = handler._getServerPlayer(*session.id, SubClientId::PrimaryClient);
        if (!sp) {
            fail(session, "ServerPlayer vanished while loading");
            return false;
        }
        session.player = sp;
        // The server keeps movement authority (and ignores our input) until the
        // client reports that its loading screen is gone.
        auto authority = sp->getEntityContext().tryGetComponent<ServerPlayerMovementComponent>();
        if (authority && authority->mServerHasMovementAuthority->any()) {
            if (elapsed % 10 == 0) acknowledgeAuthority(handler, session, *sp);
            if (elapsed > kReadyTimeout) fail(session, "the server never released movement authority");
            return false;
        }
        if (!sp->isPlayerInitialized()) {
            if (elapsed > kReadyTimeout) fail(session, "the player never finished loading");
            return false;
        }
        session.stage = GhostStage::Online;
        if (auto ready = std::exchange(session.onReady, nullptr)) ready(*sp);
        return false;
    }

    default:
        return false;
    }
}

void GhostManager::onTick() {
    if (mSessions.empty()) return;

    auto handlerOpt = ll::service::bedrock::getServerNetworkHandler();
    if (!handlerOpt) return;
    auto& handler = *handlerOpt;

    // Callbacks may log ghosts in or out, so walk a snapshot.
    auto sessions = mSessions;
    for (auto& session : sessions) {
        if (session->stage == GhostStage::Failed) continue;
        ++session->ticks;

        if (session->stage != GhostStage::Online) {
            while (stepLogin(*session)) {}
            continue;
        }

        auto* player = handler._getServerPlayer(*session->id, SubClientId::PrimaryClient);
        if (!player) {
            // Kicked or otherwise removed by the server behind our back.
            session->stage  = GhostStage::Failed;
            session->player = nullptr;
            continue;
        }
        session->player   = player;
        session->uniqueId = player->getOrCreateUniqueID().rawID; // loading saved data may change it
        driveOnline(*session, *player);
    }

    // Drop dead sessions and tell their owners. Their players are already gone.
    for (auto& session : sessions) {
        if (session->stage != GhostStage::Failed) continue;
        if (std::find(mSessions.begin(), mSessions.end(), session) == mSessions.end()) continue;
        if (handler.mClients.get().contains(*session->id)) {
            handler.disconnectClient(
                *session->id,
                SubClientId::PrimaryClient,
                Connection::DisconnectFailReason::Disconnected
            );
        }
        lose(session, nullptr);
    }
}

void GhostManager::driveOnline(GhostSession& session, ServerPlayer& player) {
    auto handlerOpt = ll::service::bedrock::getServerNetworkHandler();
    if (!handlerOpt) return;
    auto& handler = *handlerOpt;
    auto& control = session.control;

    auto authority = player.getEntityContext().tryGetComponent<ServerPlayerMovementComponent>();
    if (authority && authority->mServerHasMovementAuthority->any() && session.ticks % 10 == 0) {
        acknowledgeAuthority(handler, session, player);
    }

    if (!player.isAlive()) {
        control.stopMoving();
        control.destroying = false;
        if (!control.respawnRequested) return;
        if (!control.respawnAckSent) {
            // Tell the server the client is ready; it then picks a spawn position.
            if (auto raw = MinecraftPackets::createPacket(MinecraftPacketIds::Respawn)) {
                auto& pkt      = static_cast<RespawnPacket&>(*raw);
                pkt.mState     = PlayerRespawnState::ClientReadyToSpawn;
                pkt.mRuntimeId = player.getRuntimeID();
                handler.handle(*session.id, pkt);
            }
            control.respawnAckSent = true;
        } else if (player.mRespawnReady) {
            if (auto raw = MinecraftPackets::createPacket(MinecraftPacketIds::PlayerAction)) {
                auto& pkt      = static_cast<PlayerActionPacket&>(*raw);
                pkt.mAction    = PlayerActionType::Respawn;
                pkt.mRuntimeId = player.getRuntimeID();
                handler.handle(*session.id, pkt);
            }
        }
        return;
    }
    control.respawnRequested = false;
    control.respawnAckSent   = false;

    actions::tick(session, player);
    sendInput(session, player);
}

void GhostManager::sendInput(GhostSession& session, ServerPlayer& player) {
    using Input = PlayerAuthInputPacketPayload::InputData;

    auto handlerOpt = ll::service::bedrock::getServerNetworkHandler();
    auto raw        = MinecraftPackets::createPacket(MinecraftPacketIds::PlayerAuthInputPacket);
    if (!handlerOpt || !raw) return;

    auto& control = session.control;
    auto& input   = static_cast<PlayerAuthInputPacket&>(*raw);
    auto& flags   = *input.mInputData;

    Vec2 rot = control.look.value_or(player.getRotation());

    input.mPos                = player.getPosition();
    input.mRot                = rot;
    input.mYHeadRot           = rot.y;
    input.mInputMode          = InputMode::Mouse;
    input.mClientTick->mValue = ++session.inputTick;

    // The server has taken movement authority (teleport, dimension change, ...);
    // acknowledge it or it keeps rejecting our input.
    if (auto authority = player.getEntityContext().tryGetComponent<ServerPlayerMovementComponent>();
        authority && authority->mServerHasMovementAuthority->any()) {
        flags.insert(Input::HandledTeleport);
    }

    bool onGround = player.isOnGround();
    bool jump     = std::exchange(control.jumpOnce, false);

    // Walk towards the current waypoint.
    if (control.waypointIndex < control.waypoints.size()) {
        Vec3  feet   = player.getFeetPos();
        Vec3  target = control.waypoints[control.waypointIndex];
        float dx     = target.x - feet.x;
        float dz     = target.z - feet.z;
        float dist   = std::sqrt(dx * dx + dz * dz);

        // Waypoints along a path only need to be passed, the last one is stood on.
        bool last = control.waypointIndex + 1 == control.waypoints.size();
        if (dist < (last ? kArriveDistance : kPassDistance)) {
            if (++control.waypointIndex >= control.waypoints.size()) actions::continueNavigation(session, player);
        } else {
            float yaw     = rot.y * kDegToRad;
            float forward = (-std::sin(yaw) * dx + std::cos(yaw) * dz) / dist;
            float left    = (std::cos(yaw) * dx + std::sin(yaw) * dz) / dist;
            // Ease in over the last half block so the ghost does not overshoot.
            float scale = control.moveScale * (last ? std::clamp(dist / 0.5f, 0.3f, 1.0f) : 1.0f);

            Vec2 move{left * scale, forward * scale};
            input.mMove             = move;
            input.mAnalogMoveVector = move;
            input.mRawMoveVector    = move;
            if (forward > 0.01f) flags.insert(Input::Up);
            if (forward < -0.01f) flags.insert(Input::Down);
            if (left > 0.01f) flags.insert(Input::Left);
            if (left < -0.01f) flags.insert(Input::Right);

            // Not getting anywhere: hop if a block is in the way, give up after a while.
            // Being slow alone is not enough: the ghost is also slow while it speeds
            // up and while it eases into its target.
            float mx = feet.x - control.lastFeetPos.x;
            float mz = feet.z - control.lastFeetPos.z;
            if (mx * mx + mz * mz < 0.0004f) ++control.stuckTicks;
            else control.stuckTicks = 0;
            if (control.hopObstacles && control.stuckTicks >= 2 && onGround) {
                // Anything lower than this is stepped onto without jumping.
                BlockPos ahead{
                    Vec3{feet.x + dx / dist * kObstacleReach, feet.y + kStepHeight, feet.z + dz / dist * kObstacleReach}
                };
                jump = player.getDimensionBlockSource().getBlock(ahead).mBlockType->mMaterial.mBlocksMotion;
            }
            if (control.stuckTicks > kStuckTimeout) control.stopMoving();
            if (player.isInWater()) jump = true; // keep afloat
        }
        control.lastFeetPos = feet;
    }

    if (jump) {
        flags.insert(Input::Jumping);
        flags.insert(Input::JumpDown);
        flags.insert(Input::JumpCurrentRaw);
        if (!control.wasJumping) {
            flags.insert(Input::StartJumping);
            flags.insert(Input::JumpPressedRaw);
        }
    } else if (control.wasJumping) {
        flags.insert(Input::JumpReleasedRaw);
    }
    control.wasJumping = jump;

    if (control.sneaking) {
        flags.insert(Input::Sneaking);
        flags.insert(Input::SneakDown);
        flags.insert(Input::SneakCurrentRaw);
        if (!control.wasSneaking) {
            flags.insert(Input::StartSneaking);
            flags.insert(Input::SneakPressedRaw);
        }
    } else if (control.wasSneaking) {
        flags.insert(Input::StopSneaking);
        flags.insert(Input::SneakReleasedRaw);
    }
    control.wasSneaking = control.sneaking;

    if (control.sprinting) {
        flags.insert(Input::Sprinting);
        flags.insert(Input::SprintDown);
        if (!control.wasSprinting) flags.insert(Input::StartSprinting);
    } else if (control.wasSprinting) {
        flags.insert(Input::StopSprinting);
    }
    control.wasSprinting = control.sprinting;

    if (auto swim = std::exchange(control.swimChange, std::nullopt)) {
        flags.insert(*swim ? Input::StartSwimming : Input::StopSwimming);
    }
    if (auto fly = std::exchange(control.flyChange, std::nullopt)) {
        flags.insert(*fly ? Input::StartFlying : Input::StopFlying);
    }

    handlerOpt->handle(*session.id, input);
}

} // namespace coral_fans::cfsp::ghost
