#pragma once

#include "SpDriver.h"

namespace coral_fans::cfsp::simulated_player::driver {

// config.simPlayerImpl = protocol. Fake players are real ServerPlayers behind a fake
// client connection, driven by ghost::GhostManager. On a client build this only does
// anything inside a single-player world, where the integrated server provides the login
// flow (see cfsp/ghost).
class GhostDriver final : public SpDriver {
public:
    [[nodiscard]] config::SimPlayerImpl type() const noexcept override { return config::SimPlayerImpl::protocol; }

    void init() override;
    void shutdown() override;
    void forgetPlayers() override;

    bool login(std::string const& name, std::string const& xuid, std::optional<int64> uniqueId, LoginCallbacks callbacks)
        override;
    void logout(std::string const& name, ServerPlayer* player) override;

    [[nodiscard]] bool        isSimPlayer(Player const& player) const override;
    [[nodiscard]] std::string storageKey(ServerPlayer& player) const override;
    void                      purgeStorage(std::string const& key) override;

    bool attack(ServerPlayer& player) override;
    bool interact(ServerPlayer& player) override;
    bool jump(ServerPlayer& player) override;

    bool useItem(ServerPlayer& player) override;
    void stopUsingItem(ServerPlayer& player) override;

    bool destroyBlock(ServerPlayer& player, BlockPos const& pos, uchar face) override;
    void stopDestroyingBlock(ServerPlayer& player) override;

    void stopInteracting(ServerPlayer& player) override;
    void stopMoving(ServerPlayer& player) override;

    void lookAt(ServerPlayer& player, Vec3 const& pos) override;
    void moveTo(ServerPlayer& player, Vec3 const& pos, float speed) override;
    void navTo(ServerPlayer& player, Vec3 const& pos, float speed) override;

    void chat(ServerPlayer& player, std::string const& message) override;

    bool respawn(ServerPlayer& player) override;

    void setSneaking(ServerPlayer& player, bool value) override;
    void setSprinting(ServerPlayer& player, bool value) override;
    void setSwimming(ServerPlayer& player, bool value) override;
    void setFlying(ServerPlayer& player, bool value) override;
};

} // namespace coral_fans::cfsp::simulated_player::driver
