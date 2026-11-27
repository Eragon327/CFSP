#include "cfsp/CFSP.h"
#include "cfsp/core/fix/CFSPFixManager.h"
#include "cfsp/core/helper/CFSPHelperManager.h"
#include "cfsp/core/manager/CFSPManager.h"
#include "cfsp/core/simPlayer/SpDriver.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/command/ServerCommandRegisterEvent.h"
#include "ll/api/event/server/ServerStartedEvent.h"
#include "ll/api/event/server/ServerStoppingEvent.h"
#include "ll/api/i18n/I18n.h"
#include "ll/api/mod/RegisterHelper.h"

#ifdef LL_PLAT_C
#include "ll/api/service/Bedrock.h"
#endif


namespace coral_fans::cfsp {

CFSP& CFSP::getInstance() {
    static CFSP instance;
    return instance;
}

bool CFSP::load() {
    auto& spManager = manager::CFSPManager::getInstance();
    if (!spManager.init()) return false;
    // How fake players get into the world is picked once, here (config.simPlayerImpl,
    // see SpDriver.h). On a client build either way only does something inside a
    // single-player world, where the integrated server is.
    auto& driver = simulated_player::driver::select(spManager.getConfig().simPlayerImpl);
    driver.init();
    if (driver.type() == config::SimPlayerImpl::simulated)
        getSelf().getLogger().warn(
            "simPlayerImpl is \"simulated\": fake players are Mojang SimulatedPlayers. Behavior packs that iterate "
            "world.getPlayers() may crash the server with them; switch to \"protocol\" if that happens."
        );
    // load i18n
    if (!ll::i18n::getInstance().load(getSelf().getLangDir())) getSelf().getLogger().error("Failed to load I18n");

    ll::event::EventBus::getInstance().emplaceListener<ll::event::command::ServerCommandRegisterEvent>([](auto&&) {
        manager::CFSPManager::getInstance().load();
    });
    ll::event::EventBus::getInstance().emplaceListener<ll::event::server::ServerStartedEvent>(
        [](ll::event::server::ServerStartedEvent&) {
            auto& manager = manager::CFSPManager::getInstance();
            if (manager.getAutoJoin()) manager.autoJoin();
        }
    );
    // The server removes every player itself while it stops; let go of the fake
    // players so nothing touches one that is being torn down. Nothing is saved here:
    // the main loop has ended and saving a player then crashes.
    ll::event::EventBus::getInstance().emplaceListener<ll::event::server::ServerStoppingEvent>(
        [](auto&&) {
            auto& manager = manager::CFSPManager::getInstance();
            manager.onServerStopping();
#ifdef LL_PLAT_C
            // A client leaves and enters single-player worlds without unloading the
            // mod, so the next world starts from an empty map.
            manager.clear();
#endif
        },
        ll::event::EventPriority::High
    );
    return true;
}

bool CFSP::enable() {
#ifdef LL_PLAT_C
    if (ll::service::getLevel()) {
        auto& manager = manager::CFSPManager::getInstance();
        manager.load();
        if (manager.getAutoJoin()) manager.autoJoin();
    }
#endif
    return true;
}

bool CFSP::disable() {
#ifdef LL_PLAT_C
    // Leaving a single-player world disables the mod (it is only unloaded when the game
    // exits). Persist the fake players' bookkeeping and let go of them here too, so the
    // next world does not start from stale state even if no stopping event arrives.
    // Nothing that touches a player is allowed: the world is going away.
    auto& manager = manager::CFSPManager::getInstance();
    manager.onServerStopping();
    manager.clear();
    simulated_player::driver::current().forgetPlayers();
#endif
    return true;
}

bool CFSP::unload() {
    manager::CFSPManager::getInstance().unload();
    simulated_player::driver::current().shutdown();
    helper::CFSPHelperManager::getInstance().SimPlayerHelperHook(false);
    fix::CFSPFixManager::getInstance().cfspBugFixHook(false);
    return true;
}

} // namespace coral_fans::cfsp

LL_REGISTER_MOD(coral_fans::cfsp::CFSP, coral_fans::cfsp::CFSP::getInstance());
