#include "CFSPHelperManager.h"
#include "cfsp/core/manager/CFSPManager.h"
#include "glm/fwd.hpp"
#include "ll/api/memory/Hook.h"
#include "mc/world/level/block/Block.h"

#ifdef LL_PLAT_C
#include "ll/api/service/Bedrock.h"
#include "mc/server/ServerInstance.h"
#include "mc/server/SimulatedPlayer.h"
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

// Keeps a SimulatedPlayer looking the way it was told to. A fake player on a
// dedicated server reports its rotation with every input packet instead, so this is
// only needed for the client build.
#ifdef LL_PLAT_C
LL_TYPE_INSTANCE_HOOK(
    CFSPLookHelperHook,
    ll::memory::HookPriority::Normal,
    SimulatedPlayer,
    &SimulatedPlayer::tick,
    bool,
    ::BlockSource& region
) {
    if (auto serverInstance = ll::service::getServerInstance();
        !serverInstance
        || std::this_thread::get_id() != ll::service::getServerInstance()->mServerInstanceThread->get_id())
        return origin(region);
    bool ori  = origin(region);
    auto cfsp = manager::CFSPManager::getInstance().tryGetCFSP(this);
    if (cfsp.has_value() && std::holds_alternative<::sim::VoidMoveIntent>(this->mSimulatedMovement->mType.get())) {
        auto pos = cfsp.value()->mSaveData.lookAtOffSet + this->getHeadPos();
        if (std::holds_alternative<::sim::ContinuousLookAtPositionIntent>(this->mLookAtIntent->mType.get()))
            [[likely]] {
            std::get<::sim::ContinuousLookAtPositionIntent>(this->mLookAtIntent->mType.get()).mPosition =
                glm::vec3(pos.x, pos.y, pos.z);
        } else cfsp.value()->lookAt(pos);
    }
    return ori;
}
#endif

void CFSPHelperManager::operateHelperHook(bool enabled) {
    if (enabled) {
        CFSPBuildHelperHook ::hook();
#ifdef LL_PLAT_C
        CFSPLookHelperHook ::hook();
#endif
    } else {
        CFSPBuildHelperHook ::unhook();
#ifdef LL_PLAT_C
        CFSPLookHelperHook ::unhook();
#endif
    }
}
} // namespace coral_fans::cfsp::helper