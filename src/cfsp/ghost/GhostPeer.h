#pragma once

#include "mc/network/NetworkPeer.h"

#include <memory>

namespace coral_fans::cfsp::ghost {

struct GhostSession;

// A NetworkPeer that never touches a socket. Every virtual is overridden here so the
// vtable contains only this translation unit's functions -- NetworkPeer's own methods
// live in BDS and are not linkable.
class GhostPeer final : public NetworkPeer {
public:
    explicit GhostPeer(std::shared_ptr<GhostSession> session);
    ~GhostPeer() override = default;

    void sendPacket(std::string const& data, Reliability reliability, Compressibility compressible) override;

    NetworkStatus getNetworkStatus() const override;

    void update() override {}

    void flush(std::function<void()>&& callback) override {
        if (callback) {
            callback();
        }
    }

    [[nodiscard]] bool isLocal() const override { return true; }
    [[nodiscard]] bool isEncrypted() const override { return false; }
    [[nodiscard]] bool isLan() const override { return false; }

    DataStatus _receivePacket(
        std::string&                                                  outData,
        std::shared_ptr<std::chrono::steady_clock::time_point> const& timepointPtr
    ) override {
        (void)outData;
        (void)timepointPtr;
        return DataStatus::NoData;
    }

private:
    std::weak_ptr<GhostSession> mSession;
};

} // namespace coral_fans::cfsp::ghost
