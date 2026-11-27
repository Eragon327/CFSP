#include "SimPlayer.h"
#include "cfsp/core/simPlayer/SpDriver.h"
#include "ll/api/i18n/I18n.h"
#include "mc/world/actor/player/LayeredAbilities.h"

namespace coral_fans::cfsp::simulated_player {
base::OperateResult SimPlayer::sneaking(std::optional<bool> enable) {
    using ll::i18n_literals::operator""_tr;
    if (!this->mSimPlayer) [[unlikely]]
        return base::OperateResult::error("manager.error.loseSimplayer"_tr());
    if (this->mSimPlayer->isDead()) [[unlikely]]
        return base::OperateResult::error("manager.fail.spIsDead"_tr());
    driver::current().setSneaking(
        *this->mSimPlayer,
        enable.has_value() ? enable.value() : !this->mSimPlayer->getStatusFlag(ActorFlags::Sneaking)
    );
    return base::OperateResult::success("manager.success.operate"_tr());
}

base::OperateResult SimPlayer::swimming(std::optional<bool> enable) {
    using ll::i18n_literals::operator""_tr;
    if (!this->mSimPlayer) [[unlikely]]
        return base::OperateResult::error("manager.error.loseSimplayer"_tr());
    if (this->mSimPlayer->isDead()) [[unlikely]]
        return base::OperateResult::error("manager.fail.spIsDead"_tr());
    driver::current().setSwimming(
        *this->mSimPlayer,
        enable.has_value() ? enable.value() : !this->mSimPlayer->getStatusFlag(ActorFlags::Swimming)
    );
    return base::OperateResult::success("manager.success.operate"_tr());
}

base::OperateResult SimPlayer::flying(std::optional<bool> enable) {
    using ll::i18n_literals::operator""_tr;
    if (!this->mSimPlayer) [[unlikely]]
        return base::OperateResult::error("manager.error.loseSimplayer"_tr());
    if (this->mSimPlayer->isDead()) [[unlikely]]
        return base::OperateResult::error("manager.fail.spIsDead"_tr());
    auto& spAbilities = this->mSimPlayer->getAbilities();
    if (spAbilities.getAbility(AbilitiesIndex::MayFly).mValue->mBoolVal) [[likely]] {
        bool value =
            enable.has_value() ? enable.value() : !spAbilities.getAbility(AbilitiesIndex::Flying).mValue->mBoolVal;
        if (spAbilities.setAbility(AbilitiesIndex::Flying, value)) [[likely]] {
            driver::current().setFlying(*this->mSimPlayer, value);
            return base::OperateResult::success("manager.success.operate"_tr());
        }
    }
    return base::OperateResult::error("manager.fail.faileToSetFlying"_tr());
}

base::OperateResult SimPlayer::sprinting(std::optional<bool> enable) {
    using ll::i18n_literals::operator""_tr;
    if (!this->mSimPlayer) [[unlikely]]
        return base::OperateResult::error("manager.error.loseSimplayer"_tr());
    if (this->mSimPlayer->isDead()) [[unlikely]]
        return base::OperateResult::error("manager.fail.spIsDead"_tr());
    driver::current().setSprinting(
        *this->mSimPlayer,
        enable.has_value() ? enable.value() : !this->mSimPlayer->getStatusFlag(ActorFlags::Sprinting)
    );
    return base::OperateResult::success("manager.success.operate"_tr());
}
} // namespace coral_fans::cfsp::simulated_player