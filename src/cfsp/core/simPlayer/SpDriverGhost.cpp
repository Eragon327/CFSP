#include "SpDriverGhost.h"

#include "cfsp/ghost/GhostActions.h"
#include "cfsp/ghost/GhostManager.h"
#include "cfsp/ghost/GhostSession.h"

#include "ll/api/service/Bedrock.h"
#include "mc/server/ServerPlayer.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/storage/LevelStorage.h"
#include "mc/world/level/storage/PlayerDataSystem.h"
#include "mc/world/level/storage/db_helpers/Category.h"

#include <utility>

namespace coral_fans::cfsp::simulated_player::driver {

namespace {

ghost::GhostManager& ghosts() { return ghost::GhostManager::getInstance(); }

// Runs fn(session) if player is one of ours.
template <class Fn, class R = std::invoke_result_t<Fn, ghost::GhostSession&>>
R withSession(ServerPlayer& player, Fn&& fn, R fallback = R()) {
    if (auto session = ghosts().find(player)) return fn(*session);
    return fallback;
}

template <class Fn>
void withSessionVoid(ServerPlayer& player, Fn&& fn) {
    if (auto session = ghosts().find(player)) fn(*session);
}

} // namespace

void GhostDriver::init() { ghosts().init(); }

void GhostDriver::shutdown() { ghosts().shutdown(); }

void GhostDriver::forgetPlayers() { ghosts().forgetSessions(); }

// The server assigns a real player's unique id itself, so uniqueId cannot be forced
// here; the id stays stable across sessions through the player's own save data.
bool GhostDriver::login(std::string const& name, std::string const& xuid, std::optional<int64>, LoginCallbacks callbacks) {
    return ghosts().login(name, xuid, std::move(callbacks.onReady), std::move(callbacks.onLost)).ok;
}

void GhostDriver::logout(std::string const& name, ServerPlayer*) { ghosts().logout(name); }

bool GhostDriver::isSimPlayer(Player const& player) const { return ghosts().isGhost(player); }

// A real player is saved into the world by the server itself, next to our own copy.
std::string GhostDriver::storageKey(ServerPlayer& player) const {
    return PlayerDataSystem::playerKey(player.mUniqueName.get());
}

void GhostDriver::purgeStorage(std::string const& key) {
    if (key.empty()) return;
    if (auto level = ll::service::getLevel()) level->getLevelStorage().deleteData(key, DBHelpers::Category::Player);
}

bool GhostDriver::attack(ServerPlayer& player) {
    return withSession(player, [&](auto& s) { return ghost::actions::attack(s, player); });
}

bool GhostDriver::interact(ServerPlayer& player) {
    return withSession(player, [&](auto& s) { return ghost::actions::interact(s, player); });
}

bool GhostDriver::jump(ServerPlayer& player) {
    return withSession(player, [&](auto& s) { return ghost::actions::jump(s, player); });
}

bool GhostDriver::useItem(ServerPlayer& player) {
    return withSession(player, [&](auto& s) { return ghost::actions::useItem(s, player); });
}

void GhostDriver::stopUsingItem(ServerPlayer& player) { ghost::actions::stopUsingItem(player); }

bool GhostDriver::destroyBlock(ServerPlayer& player, BlockPos const& pos, uchar face) {
    return withSession(player, [&](auto& s) { return ghost::actions::destroyBlock(s, player, pos, face); });
}

void GhostDriver::stopDestroyingBlock(ServerPlayer& player) {
    withSessionVoid(player, [&](auto& s) { ghost::actions::stopDestroyingBlock(s, player); });
}

// Interactions are one-shot calls here; there is nothing left running to stop.
void GhostDriver::stopInteracting(ServerPlayer&) {}

void GhostDriver::stopMoving(ServerPlayer& player) {
    withSessionVoid(player, [](auto& s) { s.control.stopMoving(); });
}

void GhostDriver::lookAt(ServerPlayer& player, Vec3 const& pos) {
    withSessionVoid(player, [&](auto& s) { ghost::actions::lookAt(s, player, pos); });
}

// A player cannot walk up a full block, so even a plain move hops over what is in
// its way.
void GhostDriver::moveTo(ServerPlayer& player, Vec3 const& pos, float speed) {
    withSessionVoid(player, [&](auto& s) { ghost::actions::moveTo(s, player, pos, speed, true); });
}

void GhostDriver::navTo(ServerPlayer& player, Vec3 const& pos, float speed) {
    withSessionVoid(player, [&](auto& s) { ghost::actions::navigateTo(s, player, pos, speed); });
}

void GhostDriver::chat(ServerPlayer& player, std::string const& message) {
    withSessionVoid(player, [&](auto& s) { ghost::actions::chat(s, player, message); });
}

bool GhostDriver::respawn(ServerPlayer& player) {
    return withSession(player, [&](auto& s) { return ghost::actions::respawn(s, player); });
}

void GhostDriver::setSneaking(ServerPlayer& player, bool value) {
    withSessionVoid(player, [&](auto& s) { ghost::actions::setSneaking(s, player, value); });
}

void GhostDriver::setSprinting(ServerPlayer& player, bool value) {
    withSessionVoid(player, [&](auto& s) { ghost::actions::setSprinting(s, player, value); });
}

void GhostDriver::setSwimming(ServerPlayer& player, bool value) {
    withSessionVoid(player, [&](auto& s) { ghost::actions::setSwimming(s, player, value); });
}

void GhostDriver::setFlying(ServerPlayer& player, bool value) {
    withSessionVoid(player, [&](auto& s) { ghost::actions::setFlying(s, player, value); });
}

} // namespace coral_fans::cfsp::simulated_player::driver
