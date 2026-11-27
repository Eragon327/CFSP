#pragma once

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/level/BlockPos.h"

#include <string>

class ServerPlayer;

namespace coral_fans::cfsp::ghost {

struct GhostSession;

// What a ghost can do. These mirror SimulatedPlayer's simulate* methods, but work on
// a plain ServerPlayer: one-off actions call into the same server code the vanilla
// simulated player uses, anything continuous is expressed through GhostControl and
// reaches the server as client input.
namespace actions {

// Runs once per tick before the input packet is sent (block breaking progress).
void tick(GhostSession& session, ServerPlayer& player);

bool attack(GhostSession& session, ServerPlayer& player);
bool interact(GhostSession& session, ServerPlayer& player);
bool jump(GhostSession& session, ServerPlayer& player);

bool useItem(GhostSession& session, ServerPlayer& player);
void stopUsingItem(ServerPlayer& player);

bool destroyBlock(GhostSession& session, ServerPlayer& player, BlockPos const& pos, uchar face);
void stopDestroyingBlock(GhostSession& session, ServerPlayer& player);

void lookAt(GhostSession& session, ServerPlayer& player, Vec3 const& target);

// Walks in a straight line to target. speed is in blocks per second; with
// hopObstacles the ghost jumps when it is blocked.
void moveTo(GhostSession& session, ServerPlayer& player, Vec3 const& target, float speed, bool hopObstacles);

// Walks to target along a path found by the game's own pathfinder, so it goes around
// obstacles. Falls back to a straight walk if no path is found.
void navigateTo(GhostSession& session, ServerPlayer& player, Vec3 const& target, float speed);

// Plans the next stretch of a navigation whose waypoints ran out before its target.
// Returns false, with the ghost stopped, if there is nothing more to walk.
bool continueNavigation(GhostSession& session, ServerPlayer& player);

void chat(GhostSession& session, ServerPlayer& player, std::string const& message);

// Asks the server to respawn a dead ghost. The respawn itself completes over the
// next few ticks, like it does for a real client.
bool respawn(GhostSession& session, ServerPlayer& player);

void setSneaking(GhostSession& session, ServerPlayer& player, bool value);
void setSprinting(GhostSession& session, ServerPlayer& player, bool value);
void setSwimming(GhostSession& session, ServerPlayer& player, bool value);
void setFlying(GhostSession& session, ServerPlayer& player, bool value);

} // namespace actions

} // namespace coral_fans::cfsp::ghost
