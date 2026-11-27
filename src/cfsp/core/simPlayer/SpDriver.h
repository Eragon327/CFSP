#pragma once

#include "cfsp/Config.h"

#include "ll/api/base/StdInt.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/world/level/BlockPos.h"

#include <functional>
#include <optional>
#include <string>

class Player;
class ServerPlayer;

// The layer between SimPlayer and whatever actually puts a fake player into the
// world. SimPlayer (saving, permissions, scheduling, ...) is the same no matter how the
// player got there; only this part differs. There are two implementations, picked by
// config.simPlayerImpl when the mod loads:
//
//  - GhostDriver (protocol): a real ServerPlayer logs in through the server's own login
//    flow behind a fake client connection, so it is indistinguishable from a human
//    player for scripts and other mods. See cfsp/ghost and docs/GhostPlayer-PoC.md.
//  - SimulatedDriver (mojang): the vanilla gametest SimulatedPlayer.
//
// A dedicated server always has a server to put them in; the client build (LL_PLAT_C)
// only has one while a single-player world -- the integrated server -- is loaded, and
// that is the only place a client build can have fake players at all.
namespace coral_fans::cfsp::simulated_player::driver {

struct LoginCallbacks {
    // The player is in the world and can be controlled. May run inside login().
    std::function<void(ServerPlayer&)> onReady;
    // The player went away without logout() being called (kicked, login refused).
    // The argument is the player while it can still be saved, null once it is gone.
    std::function<void(ServerPlayer*)> onLost;
};

class SpDriver {
public:
    virtual ~SpDriver() = default;

    [[nodiscard]] virtual config::SimPlayerImpl type() const noexcept = 0;

    // Installs whatever the implementation needs (hooks, listeners). Called once from
    // the mod's load().
    virtual void init() {}
    // Undoes init(); the mod is being unloaded.
    virtual void shutdown() {}
    // A client build is leaving a single-player world: forget every player without
    // touching it, the world is going away. The mod stays loaded for the next world.
    virtual void forgetPlayers() {}

    // Starts bringing the player online. Returns false if that could not even be
    // started; otherwise exactly one of the callbacks runs later (or right away).
    virtual bool
    login(std::string const& name, std::string const& xuid, std::optional<int64> uniqueId, LoginCallbacks callbacks) = 0;

    // Takes the player offline. Works while the login is still in progress
    // (player == nullptr). No callback runs.
    virtual void logout(std::string const& name, ServerPlayer* player) = 0;

    // True for players this driver brought online.
    [[nodiscard]] virtual bool isSimPlayer(Player const& player) const = 0;

    // The key under which the server keeps its own copy of this player's data, or ""
    // if it keeps none.
    [[nodiscard]] virtual std::string storageKey(ServerPlayer& player) const = 0;

    // Deletes that copy, so a later player with the same identity starts from scratch.
    virtual void purgeStorage(std::string const& key) = 0;

    virtual bool attack(ServerPlayer& player)   = 0;
    virtual bool interact(ServerPlayer& player) = 0;
    virtual bool jump(ServerPlayer& player)     = 0;

    virtual bool useItem(ServerPlayer& player)       = 0;
    virtual void stopUsingItem(ServerPlayer& player) = 0;

    virtual bool destroyBlock(ServerPlayer& player, BlockPos const& pos, uchar face) = 0;
    virtual void stopDestroyingBlock(ServerPlayer& player)                           = 0;

    virtual void stopInteracting(ServerPlayer& player) = 0;
    virtual void stopMoving(ServerPlayer& player)      = 0;

    virtual void lookAt(ServerPlayer& player, Vec3 const& pos)             = 0;
    virtual void moveTo(ServerPlayer& player, Vec3 const& pos, float speed) = 0;
    virtual void navTo(ServerPlayer& player, Vec3 const& pos, float speed)  = 0;

    virtual void chat(ServerPlayer& player, std::string const& message) = 0;

    // Returns false if the player cannot be respawned right now.
    virtual bool respawn(ServerPlayer& player) = 0;

    virtual void setSneaking(ServerPlayer& player, bool value)  = 0;
    virtual void setSprinting(ServerPlayer& player, bool value) = 0;
    virtual void setSwimming(ServerPlayer& player, bool value)  = 0;
    // Called after the Flying ability itself has been changed.
    virtual void setFlying(ServerPlayer& player, bool value) = 0;
};

// Creates the driver for impl and makes it current(). Done once while the mod loads;
// switching while players are online would orphan them, so a change of
// config.simPlayerImpl takes effect on the next start.
SpDriver& select(config::SimPlayerImpl impl);

// The driver picked by select(). Before that, the protocol driver.
SpDriver& current();

} // namespace coral_fans::cfsp::simulated_player::driver
