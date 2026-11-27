#include "CFSPHelperManager.h"
#include "cfsp/core/manager/CFSPManager.h"
#include "ll/api/memory/Hook.h"
#include "mc/world/level/block/Block.h"

#ifdef LL_PLAT_C
#include "ll/api/service/Bedrock.h"
#include "mc/server/ServerInstance.h"
#endif

namespace coral_fans::cfsp::helper {
LL_TYPE_INSTANCE_HOOK(
    CFSPBuildHelperHook,
    ll::memory::HookPriority::Normal,
    Block,
    &Block::use,
    bool,
    ::Player&               player,
    ::BlockPos const&       pos,
    uchar                   face,
    ::HandSlot              handSlot,
    ::std::optional<::Vec3> hit
) {
#ifdef LL_PLAT_C
    if (auto serverInstance = ll::service::getServerInstance();
        !serverInstance
        || std::this_thread::get_id() != ll::service::getServerInstance()->mServerInstanceThread->get_id())
        return origin(player, pos, face, handSlot, hit);
#endif
    if (CFSPHelperManager::getInstance().buildMutex) return false;
    return origin(player, pos, face, handSlot, hit);
}

void CFSPHelperManager::operateHelperHook(bool enabled) {
    if (enabled) {
        CFSPBuildHelperHook::hook();
    } else {
        CFSPBuildHelperHook::unhook();
    }
}
} // namespace coral_fans::cfsp::helper