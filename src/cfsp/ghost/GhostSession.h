#pragma once

#include "mc/certificates/identity/PlayerAuthenticationInfo.h"
#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/network/NetworkIdentifier.h"
#include "mc/world/level/BlockPos.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class ServerPlayer;

namespace coral_fans::cfsp::ghost {

// What the fake client is currently "pressing". GhostManager turns this into one
// PlayerAuthInputPacket per tick; the server's own movement simulation does the rest.
struct GhostControl {
    // Rotation (pitch, yaw) reported to the server every tick. Empty = keep whatever
    // rotation the server currently has.
    std::optional<Vec2> look;

    // Straight-line walking targets, visited in order.
    std::vector<Vec3> waypoints;
    size_t            waypointIndex = 0;
    float             moveScale     = 1.0f; // 0..1 fraction of walking speed
    bool              hopObstacles  = false;
    Vec3              lastFeetPos   = {0, 0, 0};
    int               stuckTicks    = 0;

    bool jumpOnce     = false;
    bool wasJumping   = false;
    bool sneaking     = false;
    bool wasSneaking  = false;
    bool sprinting    = false;
    bool wasSprinting = false;

    // One-shot state transitions requested since the last input packet.
    std::optional<bool> swimChange;
    std::optional<bool> flyChange;

    // Block breaking in progress (mirrors SimulatedPlayer's destroy intent).
    bool     destroying  = false;
    BlockPos destroyPos  = {0, 0, 0};
    uchar    destroyFace = 0;

    // Level tick of the last attack or item use, for their shared cooldown.
    uint64_t lastActionTick = 0;

    // Respawn handshake: the client has to acknowledge before the server respawns it.
    bool respawnRequested = false;
    bool respawnAckSent   = false;

    // Where a navigation is ultimately heading. Set while the waypoints are a path
    // that stops short of it, so that the next stretch is planned when they run out.
    std::optional<Vec3> navTarget;
    int                 navReplans = 0;

    void stopMoving() {
        waypoints.clear();
        waypointIndex = 0;
        stuckTicks    = 0;
        navTarget.reset();
        navReplans = 0;
    }
};

enum class GhostStage : int {
    WaitPacksInfo = 0, // login submitted, waiting for ResourcePacksInfo
    WaitPackStack,     // DownloadingFinished sent, waiting for ResourcePackStack
    WaitPlayer,        // ResourcePackStackFinished sent, waiting for the ServerPlayer
    WaitSpawned,       // chunk radius + SetLocalPlayerAsInitialized sent
    WaitReady,         // spawned; waiting for the server to finish loading the player
    Online,
    Failed,
};

// One fake client. The storage blocks are the reason this is a concrete type:
// NetworkIdentifier's default ctor and PlayerAuthenticationInfo's ctor/dtor are not
// linkable, so both objects are built from zeroed bytes.
struct GhostSession {
    alignas(NetworkIdentifier) std::byte idStorage[sizeof(NetworkIdentifier)]{};
    alignas(PlayerAuthenticationInfo) std::byte authInfoStorage[sizeof(PlayerAuthenticationInfo)]{};

    NetworkIdentifier*        id                 = nullptr;
    PlayerAuthenticationInfo* authInfo           = nullptr;
    bool                      stringsConstructed = false;

    std::string name;
    std::string xuid;
    std::string uuid; // canonical uuid string, also written into the fake token
    uint64_t    guid = 0;

    // Set by GhostPeer when the server sends the packets the handshake reacts to.
    std::atomic<bool> sawResourcePacksInfo{false};
    std::atomic<bool> sawResourcePackStack{false};

    GhostStage stage     = GhostStage::WaitPacksInfo;
    int        ticks     = 0;
    int        stageTick = 0;
    uint64_t   inputTick = 0;

    // The live player, refreshed every tick; null while logging in or after the
    // server dropped it.
    ServerPlayer* player    = nullptr;
    int64_t       uniqueId  = 0;
    uint64_t      runtimeId = 0;

    // The early player-list entry was dropped because its skin was empty; resend it
    // once the ghost has spawned.
    bool playerListSuppressed = false;

    GhostControl control;

    // Fired once, when the player is fully loaded and accepts input.
    std::function<void(ServerPlayer&)> onReady;
    // Fired once if the server drops the player behind our back (kick, failed
    // login). The argument is the player while it is still valid, else null.
    std::function<void(ServerPlayer*)> onLost;

    // PlayerAuthenticationInfo's destructor is not linkable, so only the strings
    // that initIdentity placement-news are destroyed here. The remaining
    // std::string members stay zeroed for the lifetime of this object.
    ~GhostSession() {
        if (stringsConstructed) {
            authInfo->Xuid.get().~basic_string();
            authInfo->PlayFabId.get().~basic_string();
            authInfo->XboxLiveName.get().~basic_string();
            authInfo->BestDisplayName.get().~basic_string();
        }
    }
};

} // namespace coral_fans::cfsp::ghost
