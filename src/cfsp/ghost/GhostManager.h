#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

class Packet;
class Player;
class PlayerListPacket;
class ServerPlayer;

namespace coral_fans::cfsp::ghost {

struct GhostSession;

// Brings "ghost players" online by feeding a fake client connection through the
// server's own login flow. The result is a real ServerPlayer (isSimulated() ==
// false), driven every tick by a PlayerAuthInputPacket built from GhostControl.
//
// Both builds use it. A dedicated server always has the server the login flow lives
// in; a client build only has it inside a single-player world (the integrated server),
// which login() checks with ll::service::getLevel().
//
// See docs/GhostPlayer-PoC.md for the technical background and constraints.
class GhostManager {
public:
    struct LoginResult {
        bool                          ok = false;
        std::string                   message;
        std::shared_ptr<GhostSession> session;
        // Non-null when the server created the player synchronously during login.
        ServerPlayer* player = nullptr;
    };

    static GhostManager& getInstance();

    GhostManager();
    ~GhostManager();
    GhostManager(GhostManager const&)            = delete;
    GhostManager& operator=(GhostManager const&) = delete;

    // Registers the tick driver and the packet filters. Safe to call more than once.
    void init();

    // Drops the local session bookkeeping (does not kick players; used on shutdown).
    void shutdown();

    // Forgets every session without touching the players. Used when a client leaves a
    // single-player world: the world and its players are going away, and the mod is
    // disabled rather than unloaded, so the hooks stay installed for the next world.
    void forgetSessions();

    // Submits the login. Empty name/xuid pick a generated unique identity.
    // onReady fires once the player is fully loaded. onLost fires if the server
    // drops the ghost on its own; its argument is the player while that is still
    // valid, null once it is gone.
    LoginResult login(
        std::string                        name,
        std::string                        xuid,
        std::function<void(ServerPlayer&)> onReady = {},
        std::function<void(ServerPlayer*)> onLost  = {}
    );

    // Disconnects the ghost and forgets its session. No callbacks are fired.
    bool logout(std::string const& name);

    [[nodiscard]] std::shared_ptr<GhostSession> find(std::string const& name) const;
    [[nodiscard]] std::shared_ptr<GhostSession> find(Player const& player) const;
    [[nodiscard]] bool                          isGhost(Player const& player) const;

    // Called from the NetworkSystem::send hook. The server broadcasts the ghost's
    // player-list entry before its skin is applied, which makes real clients drop
    // the connection with a data error; such an entry is suppressed here and resent
    // once the ghost has spawned with a valid skin.
    bool shouldDropPlayerListAdd(PlayerListPacket const& packet);

    // Holds back a ghost's abilities / synched actor data packets while it is still
    // logging in; real clients were seen dropping their connection over them.
    bool shouldDropGhostPacket(Packet const& packet);

private:
    void onTick();
    bool stepLogin(GhostSession& session);
    void driveOnline(GhostSession& session, ServerPlayer& player);
    void sendInput(GhostSession& session, ServerPlayer& player);
    void sendPlayerListAdd(GhostSession& session, ServerPlayer& player);
    void fail(GhostSession& session, char const* why);
    void lose(std::shared_ptr<GhostSession> const& session, ServerPlayer* player);

    std::vector<std::shared_ptr<GhostSession>> mSessions;
    bool                                       mTickHooked = false;
    bool                                       mSendHooked = false;
    bool                                       mStopping   = false;
};

} // namespace coral_fans::cfsp::ghost
