#pragma once

#include "ll/api/base/StdInt.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/world/level/BlockPos.h"

#include <functional>
#include <optional>
#include <string>

class Player;
class ServerPlayer;

// The layer between SimPlayer and whatever actually puts a fake player into the
// world.
//
// Both builds do it the same way: a real ServerPlayer logs in through the server's own
// login flow behind a fake client connection, so it is
// indistinguishable from a human player for scripts and other mods. A dedicated server
// always has that server; the client build (LL_PLAT_C) only has it while a single-player
// world -- the integrated server -- is loaded, and that is the only place a client build
// can have fake players at all.
namespace coral_fans::cfsp::simulated_player::driver {

struct LoginCallbacks {
    // The player is in the world and can be controlled. May run inside login().
    std::function<void(ServerPlayer&)> onReady;
    // The player went away without logout() being called (kicked, login refused).
    // The argument is the player while it can still be saved, null once it is gone.
    std::function<void(ServerPlayer*)> onLost;
};

// Starts bringing the player online. Returns false if that could not even be
// started; otherwise exactly one of the callbacks runs later (or right away).
bool login(std::string const& name, std::string const& xuid, std::optional<int64> uniqueId, LoginCallbacks callbacks);

// Takes the player offline. Works while the login is still in progress
// (player == nullptr). No callback runs.
void logout(std::string const& name, ServerPlayer* player);

// True for players this driver brought online.
bool isSimPlayer(Player const& player);

// The key under which the server keeps its own copy of this player's data, or ""
// if it keeps none.
std::string storageKey(ServerPlayer& player);

// Deletes that copy, so a later player with the same identity starts from scratch.
void purgeStorage(std::string const& key);

bool attack(ServerPlayer& player);
bool interact(ServerPlayer& player);
bool jump(ServerPlayer& player);

bool useItem(ServerPlayer& player);
void stopUsingItem(ServerPlayer& player);

bool destroyBlock(ServerPlayer& player, BlockPos const& pos, uchar face);
void stopDestroyingBlock(ServerPlayer& player);

void stopInteracting(ServerPlayer& player);
void stopMoving(ServerPlayer& player);

void lookAt(ServerPlayer& player, Vec3 const& pos);
void moveTo(ServerPlayer& player, Vec3 const& pos, float speed);
void navTo(ServerPlayer& player, Vec3 const& pos, float speed);

void chat(ServerPlayer& player, std::string const& message);

// Returns false if the player cannot be respawned right now.
bool respawn(ServerPlayer& player);

void setSneaking(ServerPlayer& player, bool value);
void setSprinting(ServerPlayer& player, bool value);
void setSwimming(ServerPlayer& player, bool value);
// Called after the Flying ability itself has been changed.
void setFlying(ServerPlayer& player, bool value);

} // namespace coral_fans::cfsp::simulated_player::driver
