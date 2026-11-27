#ifdef LL_PLAT_C

#include "SpDriver.h"

#include "cfsp/core/fix/CFSPFixManager.h"

#include "ll/api/service/Bedrock.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/scripting/modules/gametest/ScriptNavigationResult.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/server/sim/LookDuration.h"
#include "mc/world/Minecraft.h"

#include <utility>

// Client-build driver: fake players are vanilla SimulatedPlayers, exactly as before.
namespace coral_fans::cfsp::simulated_player::driver {

namespace {

// Every ServerPlayer handed to this driver was created by login() below.
SimulatedPlayer& sim(ServerPlayer& player) { return static_cast<SimulatedPlayer&>(player); }

} // namespace

bool login(std::string const& name, std::string const& xuid, std::optional<int64> uniqueId, LoginCallbacks callbacks) {
    auto mc = ll::service::getMinecraft();
    if (!mc) return false;
    auto handler = mc->getServerNetworkHandler();
    if (!handler) return false;

    fix::CFSPFixManager::getInstance().createSpMutex = true;
    auto* simPlayer                                  = SimulatedPlayer::create(
        name,
        {0, 0, 0},
        {0, 0, 0},
        {0, 0},
        false,
        0,
        handler,
        xuid,
        uniqueId.has_value() ? std::optional<ActorUniqueID>(ActorUniqueID(uniqueId.value())) : std::nullopt
    );
    fix::CFSPFixManager::getInstance().createSpMutex = false;

    if (!simPlayer) return false;
    if (callbacks.onReady) callbacks.onReady(*simPlayer);
    return true;
}

void logout(std::string const&, ServerPlayer* player) {
    if (!player) return;
    auto& simPlayer = sim(*player);
    simPlayer.disconnect();
    simPlayer.remove();
    simPlayer.setGameTestHelper(nullptr);
}

bool isSimPlayer(Player const& player) { return player.isSimulatedPlayer(); }

// Nothing but our own save is used for a SimulatedPlayer.
std::string storageKey(ServerPlayer&) { return {}; }

void purgeStorage(std::string const&) {}

bool attack(ServerPlayer& player) { return sim(player).simulateAttack(); }

bool interact(ServerPlayer& player) { return sim(player).simulateInteract(); }

bool jump(ServerPlayer& player) { return sim(player).simulateJump(); }

bool useItem(ServerPlayer& player) { return sim(player).simulateUseItem(); }

void stopUsingItem(ServerPlayer& player) { sim(player).simulateStopUsingItem(); }

bool destroyBlock(ServerPlayer& player, BlockPos const& pos, uchar face) {
    return sim(player).simulateDestroyBlock(pos, static_cast<ScriptModuleMinecraft::ScriptFacing>(face));
}

void stopDestroyingBlock(ServerPlayer& player) { sim(player).simulateStopDestroyingBlock(); }

void stopInteracting(ServerPlayer& player) { sim(player).simulateStopInteracting(); }

void stopMoving(ServerPlayer& player) { sim(player).simulateStopMoving(); }

void lookAt(ServerPlayer& player, Vec3 const& pos) { sim(player).simulateLookAt(pos, sim::LookDuration::Instant); }

void moveTo(ServerPlayer& player, Vec3 const& pos, float speed) {
    sim(player).simulateMoveToLocation(pos, speed, false);
}

void navTo(ServerPlayer& player, Vec3 const& pos, float speed) { sim(player).simulateNavigateToLocation(pos, speed); }

void chat(ServerPlayer& player, std::string const& message) { sim(player).simulateChat(message); }

bool respawn(ServerPlayer& player) { return sim(player).simulateRespawn(); }

void setSneaking(ServerPlayer& player, bool value) { player.setSneaking(value); }

void setSprinting(ServerPlayer& player, bool value) { player.setSprinting(value); }

void setSwimming(ServerPlayer& player, bool value) {
    if (value) player.startSwimming();
    else player.stopSwimming();
}

// The ability change alone is enough for a SimulatedPlayer.
void setFlying(ServerPlayer&, bool) {}

} // namespace coral_fans::cfsp::simulated_player::driver

#endif // LL_PLAT_C
