#include "SpDriverSim.h"

#include "cfsp/core/fix/CFSPFixManager.h"

#include "ll/api/service/Bedrock.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/scripting/modules/gametest/ScriptNavigationResult.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/server/sim/LookDuration.h"
#include "mc/world/Minecraft.h"

#include <utility>

namespace coral_fans::cfsp::simulated_player::driver {

namespace {

// Every ServerPlayer handed to this driver was created by login() below.
SimulatedPlayer& sim(ServerPlayer& player) { return static_cast<SimulatedPlayer&>(player); }

} // namespace

bool SimulatedDriver::login(std::string const& name, std::string const& xuid, std::optional<int64> uniqueId, LoginCallbacks callbacks) {
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

void SimulatedDriver::logout(std::string const&, ServerPlayer* player) {
    if (!player) return;
    auto& simPlayer = sim(*player);
    simPlayer.disconnect();
    simPlayer.remove();
    simPlayer.setGameTestHelper(nullptr);
}

bool SimulatedDriver::isSimPlayer(Player const& player) const { return player.isSimulatedPlayer(); }

// Nothing but our own save is used for a SimulatedPlayer.
std::string SimulatedDriver::storageKey(ServerPlayer&) const { return {}; }

void SimulatedDriver::purgeStorage(std::string const&) {}

bool SimulatedDriver::attack(ServerPlayer& player) { return sim(player).simulateAttack(); }

bool SimulatedDriver::interact(ServerPlayer& player) { return sim(player).simulateInteract(); }

bool SimulatedDriver::jump(ServerPlayer& player) { return sim(player).simulateJump(); }

bool SimulatedDriver::useItem(ServerPlayer& player) { return sim(player).simulateUseItem(); }

void SimulatedDriver::stopUsingItem(ServerPlayer& player) { sim(player).simulateStopUsingItem(); }

bool SimulatedDriver::destroyBlock(ServerPlayer& player, BlockPos const& pos, uchar face) {
    return sim(player).simulateDestroyBlock(pos, static_cast<ScriptModuleMinecraft::ScriptFacing>(face));
}

void SimulatedDriver::stopDestroyingBlock(ServerPlayer& player) { sim(player).simulateStopDestroyingBlock(); }

void SimulatedDriver::stopInteracting(ServerPlayer& player) { sim(player).simulateStopInteracting(); }

void SimulatedDriver::stopMoving(ServerPlayer& player) { sim(player).simulateStopMoving(); }

void SimulatedDriver::lookAt(ServerPlayer& player, Vec3 const& pos) { sim(player).simulateLookAt(pos, sim::LookDuration::Instant); }

void SimulatedDriver::moveTo(ServerPlayer& player, Vec3 const& pos, float speed) {
    sim(player).simulateMoveToLocation(pos, speed, false);
}

void SimulatedDriver::navTo(ServerPlayer& player, Vec3 const& pos, float speed) { sim(player).simulateNavigateToLocation(pos, speed); }

void SimulatedDriver::chat(ServerPlayer& player, std::string const& message) { sim(player).simulateChat(message); }

bool SimulatedDriver::respawn(ServerPlayer& player) { return sim(player).simulateRespawn(); }

void SimulatedDriver::setSneaking(ServerPlayer& player, bool value) { player.setSneaking(value); }

void SimulatedDriver::setSprinting(ServerPlayer& player, bool value) { player.setSprinting(value); }

void SimulatedDriver::setSwimming(ServerPlayer& player, bool value) {
    if (value) player.startSwimming();
    else player.stopSwimming();
}

// The ability change alone is enough for a SimulatedPlayer.
void SimulatedDriver::setFlying(ServerPlayer&, bool) {}

} // namespace coral_fans::cfsp::simulated_player::driver

