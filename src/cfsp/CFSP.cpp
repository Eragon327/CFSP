#include "cfsp/CFSP.h"
#include "cfsp/core/fix/CFSPFixManager.h"
#include "cfsp/core/helper/CFSPHelperManager.h"
#include "cfsp/core/manager/CFSPManager.h"
#include "cfsp/ghost/GhostManager.h"
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
    if (!manager::CFSPManager::getInstance().init()) return false;
#ifdef LL_PLAT_S
    // Fake players are real ServerPlayers behind a fake client; this drives them.
    ghost::GhostManager::getInstance().init();
#endif
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
#ifdef LL_PLAT_C
    ll::event::EventBus::getInstance().emplaceListener<ll::event::server::ServerStoppingEvent>([](auto&&) {
        manager::CFSPManager::getInstance().saveSps();
        manager::CFSPManager::getInstance().clear();
    });
#else
    // The server removes every player itself while it stops; let go of the fake
    // players so nothing touches one that is being torn down.
    ll::event::EventBus::getInstance().emplaceListener<ll::event::server::ServerStoppingEvent>(
        [](auto&&) { manager::CFSPManager::getInstance().onServerStopping(); },
        ll::event::EventPriority::High
    );
#endif
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

bool CFSP::disable() { return true; }

bool CFSP::unload() {
    manager::CFSPManager::getInstance().unload();
#ifdef LL_PLAT_S
    ghost::GhostManager::getInstance().shutdown();
#endif
    helper::CFSPHelperManager::getInstance().SimPlayerHelperHook(false);
    fix::CFSPFixManager::getInstance().cfspBugFixHook(false);
    return true;
}

} // namespace coral_fans::cfsp

LL_REGISTER_MOD(coral_fans::cfsp::CFSP, coral_fans::cfsp::CFSP::getInstance());
