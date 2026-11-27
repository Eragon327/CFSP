#include "SimPlayer.h"
#include "cfsp/base/OperateResult.h"
#include "cfsp/base/Schedule.h"
#include "cfsp/base/Utils.h"
#include "cfsp/core/fix/CFSPFixManager.h"
#include "cfsp/core/manager/CFSPManager.h"
#include "cfsp/core/simPlayer/SpDriver.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/i18n/I18n.h"
#include "ll/api/service/Bedrock.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/world/level/Level.h"


// #include "mc/world/actor/provider/ActorAttribute.h"
#include <optional>


namespace coral_fans::cfsp::simulated_player {
void SimPlayer::cancelTask() { base::Schedule::getInstance().getSchedule()->cancel(this->mTaskid); }

void SimPlayer::cancelScript() { base::Schedule::getInstance().getSchedule()->cancel(this->mScriptid); }

base::OperateResult SimPlayer::stop() {
    using ll::i18n_literals::operator""_tr;
    if (base::Schedule::getInstance().getSchedule()->isRunning(this->mTaskid)) this->cancelTask();
    if (base::Schedule::getInstance().getSchedule()->isRunning(this->mScriptid)) this->cancelScript();

    if (!this->mSimPlayer) [[unlikely]]
        return base::OperateResult::error("manager.error.loseSimplayer"_tr());

    driver::stopDestroyingBlock(*this->mSimPlayer);

    driver::stopInteracting(*this->mSimPlayer);

    driver::stopMoving(*this->mSimPlayer);

    driver::stopUsingItem(*this->mSimPlayer);

    return base::OperateResult::success("manager.success.operate"_tr());
}

std::shared_ptr<SimPlayer> SimPlayer::create(
    const Player*      player,
    std::string const& spname,
    Vec3 const&        pos,
    DimensionType      dim,
    bool               lockUniqueId
) {
    auto uuid = player ? player->getUuid().asString() : "";
    auto xuid = "-" + std::to_string(std::hash<std::string>()(spname));

    SimPlayerSaveData saveData;
    saveData.name            = spname;
    saveData.xuid            = xuid;
    saveData.ownerUuid       = uuid;
    saveData.lastSpawnerUuid = uuid;
    if (player) saveData.mGameType = player->getPlayerGameType();

    auto cfsp         = std::make_shared<simulated_player::SimPlayer>(saveData, nullptr);
    cfsp->mConnecting = true;

    // The player may only show up a few ticks from now, so remember where it should
    // go and which way it should face.
    Vec3 lookDir = player ? Vec3::directionFromRotation(player->getRotation()) : Vec3{0, 0, 0};
    bool started = driver::login(
        spname,
        xuid,
        std::nullopt,
        {
            .onReady =
                [weak = std::weak_ptr(cfsp), pos, dim, lockUniqueId, lookDir](ServerPlayer& sp) {
                    if (auto self = weak.lock()) self->onCreated(sp, pos, dim, lockUniqueId, lookDir);
                },
            .onLost = [spname](ServerPlayer* sp) { manager::CFSPManager::getInstance().onSpLost(spname, sp); },
        }
    );
    if (!started) [[unlikely]]
        return nullptr;

    return cfsp;
}

void SimPlayer::onCreated(
    ServerPlayer& sp,
    Vec3 const&   pos,
    DimensionType dim,
    bool          lockUniqueId,
    Vec3 const&   lookDir
) {
    this->mSimPlayer           = &sp;
    this->mConnecting          = false;
    this->mSaveData.storageKey = driver::storageKey(sp);

    sp.teleport(pos, dim);
    sp.mPlayerRespawnPoint->mPlayerPosition = pos;
    sp.mPlayerRespawnPoint->mDimension      = dim;

    if (lockUniqueId) this->mSaveData.uniqueId = sp.getOrCreateUniqueID().rawID;
    sp.setPlayerGameType(this->mSaveData.mGameType);

    // Face the way the creator was facing; a console-created player keeps its rotation.
    if (lookDir != Vec3{0, 0, 0}) this->lookAt(sp.getHeadPos() + lookDir);
    this->mShouldSave = true;
}

base::OperateResult SimPlayer::spawn(std::optional<const Player*> player) {
    using ll::i18n_literals::operator""_tr;
    if (this->mSimPlayer || this->mConnecting) return base::OperateResult::error("manager.fail.spHasOnline"_tr());

    this->forgetUnlockedIdentity();
    this->mConnecting = true;
    bool started      = driver::login(
        this->mSaveData.name,
        this->mSaveData.xuid,
        this->mSaveData.uniqueId,
        {
            .onReady =
                [weak = this->weak_from_this()](ServerPlayer& sp) {
                    if (auto self = weak.lock()) self->onSpawned(sp);
                },
            .onLost = [spname = this->mSaveData.name](
                          ServerPlayer* sp
                      ) { manager::CFSPManager::getInstance().onSpLost(spname, sp); },
        }
    );
    if (!started) [[unlikely]] {
        this->mConnecting = false;
        return base::OperateResult::error("manager.error.failedtocreate"_tr());
    }

    if (player.has_value())
        this->mSaveData.lastSpawnerUuid = player.value() ? player.value()->getUuid().asString() : "";
    this->mSaveData.isOnline = true;
    this->mShouldSave        = true;
    return base::OperateResult::success("manager.success.operate"_tr());
}

void SimPlayer::onSpawned(ServerPlayer& sp) {
    this->mSimPlayer  = &sp;
    this->mConnecting = false;
    if (auto key = driver::storageKey(sp); key != this->mSaveData.storageKey) {
        this->mSaveData.storageKey = key;
        this->mShouldSave          = true;
    }

    this->loadSpNbt();
    sp.teleport(sp.getFeetPos(), sp.getDimensionId());
    sp.mPlayerRespawnPoint->mPlayerPosition = sp.getFeetPos();
    sp.mPlayerRespawnPoint->mDimension      = sp.getDimensionId();
    this->lookAt(sp.getHeadPos() + this->mSaveData.lookAtOffSet);
    sp.setPlayerGameType(this->mSaveData.mGameType);

    if (sp.isDead())
        ll::command::CommandRegistrar::getInstance(false).addSoftEnumValues("cfspDeadSp", {this->mSaveData.name});
}

base::OperateResult SimPlayer::despawn() {
    using ll::i18n_literals::operator""_tr;
    if (!this->mSimPlayer && !this->mConnecting) [[unlikely]]
        return base::OperateResult::error("manager.error.loseSimplayer"_tr());
    this->stop();
    this->mSaveData.isOnline = false;
    this->mShouldSave        = true;
    this->save();

    if (this->mSimPlayer) {
        auto& removingRecord = fix::CFSPFixManager::getInstance().mRemovingRecord;
        auto  currentTick    = this->mSimPlayer->getLevel().getCurrentTick();
        if (currentTick.tickID - removingRecord.mLastTick.tickID > 2) {
            removingRecord.mRemovingSpList.clear();
        }
        removingRecord.mLastTick = currentTick;
        removingRecord.mRemovingSpList.emplace(this->mSimPlayer->mName);
    }

    this->disconnect();
    this->forgetUnlockedIdentity();
    return base::OperateResult::success("manager.success.operate"_tr());
}

// A player whose unique id is not locked gets a new one every time it comes online.
// The server takes the id from its own copy of the player's data, so that copy has
// to go; everything else about the player is restored from our save.
void SimPlayer::forgetUnlockedIdentity() {
    if (this->mSaveData.uniqueId.has_value()) return;
    driver::purgeStorage(this->mSaveData.storageKey.value_or(""));
}

void SimPlayer::disconnect() {
    if (!this->mSimPlayer && !this->mConnecting) return;
    driver::logout(this->mSaveData.name, this->mSimPlayer);
    this->mSimPlayer  = nullptr;
    this->mConnecting = false;
}

void SimPlayer::onLost(ServerPlayer* player) {
    if (base::Schedule::getInstance().getSchedule()->isRunning(this->mTaskid)) this->cancelTask();
    if (base::Schedule::getInstance().getSchedule()->isRunning(this->mScriptid)) this->cancelScript();
    this->mSimPlayer         = player; // null once the player is gone: only data.json is written then
    this->mConnecting        = false;
    this->mSaveData.isOnline = false;
    this->mShouldSave        = true;
    this->save();
    this->mSimPlayer = nullptr;
}

void SimPlayer::onServerStopping() {
    if (base::Schedule::getInstance().getSchedule()->isRunning(this->mTaskid)) this->cancelTask();
    if (base::Schedule::getInstance().getSchedule()->isRunning(this->mScriptid)) this->cancelScript();
    // Nothing is saved here: this runs after the server's main loop has ended, when
    // the player can no longer be serialized. The stop command and the server's own
    // player save have already written our copy. isOnline stays set, so the player
    // joins again on the next start.
    this->mSimPlayer  = nullptr;
    this->mConnecting = false;
}

base::OperateResult SimPlayer::respawn() {
    using ll::i18n_literals::operator""_tr;
    if (!this->mSimPlayer) [[unlikely]]
        return base::OperateResult::error("manager.error.loseSimplayer"_tr());
    if (this->mSimPlayer->isAlive()) return base::OperateResult::error("manager.fail.spIsAlive"_tr());
    if (!driver::respawn(*this->mSimPlayer)) return base::OperateResult::error("manager.fail.failToRespawn"_tr());
    return base::OperateResult::success("manager.success.operate"_tr());
}

base::OperateResult SimPlayer::info() {
    using ll::i18n_literals::operator""_tr;
    std::string res  = "\n  " + "manager.info.spname"_tr() + this->mSaveData.name + "\n  ";
    res             += "manager.info.spOwner"_tr() + base::utils::tryGetPlayerName(this->mSaveData.ownerUuid) + "\n  ";
    res += "manager.info.spStatus"_tr()
         + (this->mSimPlayer ? this->mSimPlayer->isAlive() ? "base.spstatus.alive"_tr() : "base.spstatus.dead"_tr()
                             : "base.spstatus.offline"_tr())
         + "\n  ";
    if (this->mSimPlayer) {
        res += "manager.info.spPos"_tr() + base::utils::getDimName(this->mSimPlayer->getDimensionId()) + " "
             + this->mSimPlayer->getPosition().toString() + "\n  ";
        res += "manager.info.spRespawnpos"_tr()
             + base::utils::getDimName(this->mSimPlayer->mPlayerRespawnPoint->mDimension->mValue) + " "
             + this->mSimPlayer->mPlayerRespawnPoint->mPlayerPosition->toString() + "\n  ";
        res += "manager.info.spGamemode"_tr() + base::utils::getGameModeStr((int)this->mSimPlayer->getPlayerGameType())
             + "\n  ";
        res += "manager.info.spHealth"_tr() + std::to_string(mSimPlayer->getHealth()) + " / "
             + std::to_string(this->mSimPlayer->getMaxHealth()) + "\n  ";
        res += "manager.info.spIsFree"_tr() + (this->isFree() ? "base.yesOrNo.yes"_tr() : "base.yesOrNo.no"_tr());
    }
    return base::OperateResult::success(res);
}

base::OperateResult SimPlayer::lookAt(Vec3 const& pos) {
    using ll::i18n_literals::operator""_tr;
    if (!this->mSimPlayer) [[unlikely]]
        return base::OperateResult::error("manager.error.loseSimplayer"_tr());
    if (this->mSimPlayer->isDead()) [[unlikely]]
        return base::OperateResult::error("manager.fail.spIsDead"_tr());
    this->mSaveData.lookAtOffSet = pos - this->mSimPlayer->getHeadPos();
    driver::lookAt(*this->mSimPlayer, pos);
    return base::OperateResult::success("manager.success.operate"_tr());
}

base::OperateResult SimPlayer::lookAt(Direction direction) {
    using ll::i18n_literals::operator""_tr;
    if (!this->mSimPlayer) [[unlikely]]
        return base::OperateResult::error("manager.error.loseSimplayer"_tr());
    if (this->mSimPlayer->isDead()) [[unlikely]]
        return base::OperateResult::error("manager.fail.spIsDead"_tr());

    Vec3 offSet;
    switch (direction) {
    case Direction::North:
        offSet = {0, 0, -1};
        break;
    case Direction::South:
        offSet = {0, 0, 1};
        break;
    case Direction::West:
        offSet = {-1, 0, 0};
        break;
    case Direction::East:
        offSet = {1, 0, 0};
        break;
    case Direction::Up:
        offSet = {0, 1, 0};
        break;
    case Direction::Down:
        offSet = {0, -1, 0};
        break;
    default:
        return base::OperateResult::error("manager.fail.invalidDirection"_tr());
    }

    Vec3 pos = this->mSimPlayer->getHeadPos() + offSet;
    return this->lookAt(pos);
}
} // namespace coral_fans::cfsp::simulated_player