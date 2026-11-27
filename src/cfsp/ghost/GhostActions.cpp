#include "cfsp/ghost/GhostActions.h"

#include "cfsp/ghost/GhostSession.h"

#include "ll/api/service/Bedrock.h"

#include "mc/deps/shared_types/legacy/actor/ActorDamageCause.h"
#include "mc/entity/components_json_legacy/NavigationComponent.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/ServerNetworkHandler.h"
#include "mc/network/ServerPlayerBlockUseHandler.h"
#include "mc/network/packet/AnimatePacket.h"
#include "mc/network/packet/TextPacket.h"
#include "mc/network/packet/TextPacketPayload.h"
#include "mc/server/ServerPlayer.h"
#include "mc/world/actor/ActorHurtResult.h"
#include "mc/world/actor/ActorSwingSource.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/gamemode/InteractionResult.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/pathfinder/ActorPathingData.h"
#include "mc/world/level/pathfinder/Path.h"
#include "mc/world/level/pathfinder/PathCompletionType.h"
#include "mc/world/level/pathfinder/PathFinder.h"
#include "mc/world/phys/HitResult.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>

namespace coral_fans::cfsp::ghost::actions {

namespace {

// Ticks between two attacks or item uses, shared by both like in SimulatedPlayer.
// It is not cosmetic: an attack on a target that is still invulnerable from the last
// hit does no damage and restarts that invulnerability, so a ghost attacking every
// tick would land its first hit and then never another. Holding an item in use
// (drawing a bow, eating) needs it too, or every tick would start over.
constexpr uint64_t kActionCooldown = 10;
constexpr float    kWalkSpeed      = 4.317f;
constexpr float    kRadToDeg       = 57.29578f;

uint64_t now(ServerPlayer& player) { return player.getLevel().getCurrentTick().tickID; }

bool onCooldown(GhostSession& session, ServerPlayer& player) {
    return now(player) - session.control.lastActionTick < kActionCooldown;
}

bool isUsingItem(ServerPlayer& player) { return !player.mItemInUse->mItem->isNull(); }

// A real client announces its arm swing with an AnimatePacket, which the server
// relays to everyone nearby.
void swing(GhostSession& session, ServerPlayer& player, ActorSwingSource source) {
    auto handler = ll::service::bedrock::getServerNetworkHandler();
    auto raw     = MinecraftPackets::createPacket(MinecraftPacketIds::Animate);
    if (!handler || !raw) return;
    auto& pkt        = static_cast<AnimatePacket&>(*raw);
    pkt.mRuntimeId   = player.getRuntimeID();
    pkt.mAction      = AnimatePacketPayload::Action::Swing;
    pkt.mSwingSource = source;
    handler->handle(*session.id, pkt);
}

} // namespace

void tick(GhostSession& session, ServerPlayer& player) {
    auto& control = session.control;
    if (!control.destroying) return;

    if (isUsingItem(player)) {
        stopDestroyingBlock(session, player);
        return;
    }
    swing(session, player, ActorSwingSource::Mine);
    if (player.mDestroyingBlock) {
        ServerPlayerBlockUseHandler::serverTickBlockBreaking(player, control.destroyPos, control.destroyFace);
    } else {
        player.mDestroyingBlock = true;
        ServerPlayerBlockUseHandler::onStartDestroyBlock(player, control.destroyPos, control.destroyFace);
    }
    if (player.getDimensionBlockSource().getBlock(control.destroyPos).isAir()) stopDestroyingBlock(session, player);
}

bool attack(GhostSession& session, ServerPlayer& player) {
    if (!player.isAlive() || onCooldown(session, player)) return false;

    // Blocks are traced too, so a wall between the ghost and its target stops the hit.
    auto hit = player.traceRay(player.isCreative() ? 5.0f : 3.0f, true, true);
    swing(session, player, ActorSwingSource::Attack);
    session.control.lastActionTick = now(player);

    if (hit.mType != HitResultType::Entity) return false;
    auto* target = hit.getEntity();
    if (!target || target == &player) return false;
    player.attack(*target, SharedTypes::Legacy::ActorDamageCause::EntityAttack);
    return true;
}

bool interact(GhostSession&, ServerPlayer& player) {
    if (!player.isAlive()) return false;

    auto hit = player.traceRay(5.0f, true, true);
    if (hit.mType == HitResultType::Entity) {
        auto* target = hit.getEntity();
        if (!target || target == &player) return false;
        player.interact(*target, Vec3::ZERO());
        return true;
    }
    if (hit.mType == HitResultType::Tile) {
        BlockPos pos = hit.mBlock;
        return player.getDimensionBlockSource()
            .getBlock(pos)
            .use(player, pos, static_cast<uchar>(hit.mFacing), HandSlot::Mainhand, std::nullopt);
    }
    return false;
}

bool jump(GhostSession& session, ServerPlayer& player) {
    if (!player.isAlive() || !player.isOnGround()) return false;
    session.control.jumpOnce = true;
    return true;
}

bool useItem(GhostSession& session, ServerPlayer& player) {
    if (!player.isAlive()) return false;
    auto& item = const_cast<ItemStack&>(player.getSelectedItem());
    if (item.isNull() || onCooldown(session, player)) return false;
    player.mGameMode->useItem(item, HandSlot::Mainhand);
    session.control.lastActionTick = now(player);
    return true;
}

void stopUsingItem(ServerPlayer& player) {
    if (player.isAlive()) player.mGameMode->releaseUsingItem();
}

bool destroyBlock(GhostSession& session, ServerPlayer& player, BlockPos const& pos, uchar face) {
    if (!player.isAlive()) return false;
    auto& control = session.control;
    if (control.destroying) {
        if (control.destroyPos == pos && control.destroyFace == face) return true;
        stopDestroyingBlock(session, player);
    }
    if (player.getDimensionBlockSource().getBlock(pos).isAir() || isUsingItem(player)) return false;
    control.destroying  = true;
    control.destroyPos  = pos;
    control.destroyFace = face;
    return true;
}

void stopDestroyingBlock(GhostSession& session, ServerPlayer& player) {
    auto& control           = session.control;
    player.mDestroyingBlock = false;
    if (!control.destroying) return;
    if (player.isAlive()) player.mGameMode->stopDestroyBlock(control.destroyPos);
    control.destroying = false;
}

void lookAt(GhostSession& session, ServerPlayer& player, Vec3 const& target) {
    Vec3  d     = target - player.getHeadPos();
    float horiz = std::sqrt(d.x * d.x + d.z * d.z);
    if (horiz < 1e-4f && std::abs(d.y) < 1e-4f) return;

    Vec2 rot = player.getRotation();
    if (horiz >= 1e-4f) rot.y = std::atan2(-d.x, d.z) * kRadToDeg;
    rot.x = -std::atan2(d.y, horiz) * kRadToDeg;

    session.control.look = rot;
    // Also apply it now, so anything that runs before the next input packet sees it.
    player.mBuiltInComponents->mActorRotationComponent->mRot = rot;
}

void moveTo(GhostSession& session, ServerPlayer& player, Vec3 const& target, float speed, bool hopObstacles) {
    auto& control = session.control;
    control.stopMoving();
    control.waypoints.push_back(target);
    control.moveScale    = std::clamp(speed / kWalkSpeed, 0.05f, 1.0f);
    control.hopObstacles = hopObstacles;
    control.lastFeetPos  = player.getFeetPos();
}

namespace {

constexpr float kPathSearchMargin = 16.0f; // how far past the target's distance the search may wander
constexpr float kPathSearchMax    = 96.0f;
constexpr int   kMaxReplans       = 16;   // stretches a navigation may be planned in
constexpr float kMinProgress      = 1.0f; // a stretch that gets less closer than this is not worth walking

float horizontalDistance(Vec3 const& a, Vec3 const& b) {
    float dx = a.x - b.x;
    float dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

// Asks the game's pathfinder for a way from the player to target. The result is a
// list of positions to walk through in order; complete tells whether it reaches the
// target or is only the best stretch found towards it.
std::vector<Vec3> findPath(ServerPlayer& player, Vec3 const& target, bool& complete) {
    complete = false;

    // The pathfinder takes its rules from a NavigationComponent, which a player does
    // not have and whose constructor is not linkable. It only reads the flags and the
    // (empty) avoid list, so zeroed storage with the flags set is a valid stand-in.
    alignas(NavigationComponent) std::byte storage[sizeof(NavigationComponent)];
    std::memset(storage, 0, sizeof(storage));
    auto& rules              = *reinterpret_cast<NavigationComponent*>(storage);
    rules.mAvoidDamageBlocks = true;
    rules.mCanJump           = true;
    rules.mCanFloat          = true;
    rules.mCanPassDoors      = true;

    Vec3  feet = player.getFeetPos();
    float maxDist =
        std::min(horizontalDistance(feet, target) + std::abs(feet.y - target.y) + kPathSearchMargin, kPathSearchMax);

    PathFinder finder(player.getDimensionBlockSource(), rules);
    auto       data = PathFinder::extractPathingData(player, PathFinder::ExtractionPurpose::Pathing, target);
    // The pathfinder returns an empty path for anything that is not a "walker", a
    // category that comes from an entity's definition and that players lack. The
    // field has no name in the headers; it sits between the door-related flags.
    data.mUnke6b511.as<bool>() = true;
    auto path                  = finder._findPath(data, target.x, target.y, target.z, maxDist);
    if (!path || path->mCompletionType == PathCompletionType::Empty) return {};

    complete = path->mCompletionType == PathCompletionType::Full;

    std::vector<Vec3> out;
    auto const&       nodes = *path->mNodes;
    // The first node is the block the player already stands in.
    for (size_t i = nodes.size() > 1 ? 1 : 0; i < nodes.size(); ++i) {
        BlockPos const& pos = nodes[i].pos;
        out.emplace_back(static_cast<float>(pos.x) + 0.5f, static_cast<float>(pos.y), static_cast<float>(pos.z) + 0.5f);
    }
    return out;
}

} // namespace

void navigateTo(GhostSession& session, ServerPlayer& player, Vec3 const& target, float speed) {
    auto& control = session.control;
    control.stopMoving();
    control.moveScale    = std::clamp(speed / kWalkSpeed, 0.05f, 1.0f);
    control.hopObstacles = true;
    control.lastFeetPos  = player.getFeetPos();
    control.navTarget    = target;

    // No way found at all: at least try the direct line.
    if (!continueNavigation(session, player)) control.waypoints.push_back(target);
}

bool continueNavigation(GhostSession& session, ServerPlayer& player) {
    auto& control = session.control;
    if (!control.navTarget) {
        control.stopMoving();
        return false;
    }
    Vec3 target  = *control.navTarget;
    int  replans = control.navReplans;
    control.stopMoving();

    bool complete = false;
    auto path     = replans < kMaxReplans ? findPath(player, target, complete) : std::vector<Vec3>{};
    if (path.empty()) return false;

    if (complete) {
        // The path ends in the target's block; finish on the exact spot.
        path.push_back(target);
    } else {
        // Only worth walking if it actually gets closer.
        float now  = horizontalDistance(player.getFeetPos(), target);
        float then = horizontalDistance(path.back(), target);
        if (now - then < kMinProgress) return false;
        control.navTarget  = target;
        control.navReplans = replans + 1;
    }
    control.waypoints   = std::move(path);
    control.lastFeetPos = player.getFeetPos();
    return true;
}

void chat(GhostSession& session, ServerPlayer& player, std::string const& message) {
    auto handler = ll::service::bedrock::getServerNetworkHandler();
    if (!handler) return;
    auto pkt = TextPacketPayload::createChat(*player.mName, message, std::nullopt, session.xuid, "");
    handler->handle(*session.id, pkt);
}

bool respawn(GhostSession& session, ServerPlayer& player) {
    if (player.isAlive()) return false;
    session.control.respawnRequested = true;
    return true;
}

void setSneaking(GhostSession& session, ServerPlayer& player, bool value) {
    session.control.sneaking = value;
    player.setSneaking(value);
}

void setSprinting(GhostSession& session, ServerPlayer& player, bool value) {
    session.control.sprinting = value;
    player.setSprinting(value);
}

void setSwimming(GhostSession& session, ServerPlayer& player, bool value) {
    session.control.swimChange = value;
    if (value) player.startSwimming();
    else player.stopSwimming();
}

void setFlying(GhostSession& session, ServerPlayer&, bool value) { session.control.flyChange = value; }

} // namespace coral_fans::cfsp::ghost::actions
