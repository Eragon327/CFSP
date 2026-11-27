#ifdef LL_PLAT_S

#include "cfsp/ghost/GhostPeer.h"

#include "cfsp/ghost/GhostSession.h"

#include <utility>

// NetworkPeer's destructor is declared but defined in BDS, which is not linkable.
// Defining it here gives our vtable a local target; only our own objects ever reach
// it, so forwarding to the real implementation is unnecessary.
NetworkPeer::~NetworkPeer() = default;

namespace coral_fans::cfsp::ghost {

GhostPeer::GhostPeer(std::shared_ptr<GhostSession> session) : mSession(std::move(session)) { mPeer = nullptr; }

void GhostPeer::sendPacket(std::string const& data, Reliability, Compressibility) {
    auto session = mSession.lock();
    if (!session || data.empty()) {
        return;
    }

    // Single (unbatched) packets start with their id. The handshake packets the
    // login driver waits for -- ResourcePacksInfo / ResourcePackStack -- are sent as
    // single frames by BDS, so the first byte is the real id there. Everything else
    // the server streams to this peer is simply discarded.
    unsigned char first = static_cast<unsigned char>(data[0]);
    if (first == 0x06) session->sawResourcePacksInfo = true;
    if (first == 0x07) session->sawResourcePackStack = true;
}

NetworkPeer::NetworkStatus GhostPeer::getNetworkStatus() const {
    NetworkStatus st{};
    st.mLoad                   = NetworkLoad::Unrestricted;
    st.mCurrentPing            = std::chrono::milliseconds(0);
    st.mAveragePing            = std::chrono::milliseconds(0);
    st.mApproximateMaxBps      = 1000000;
    st.mCurrentPacketLoss      = 0.0f;
    st.mAveragePacketLoss      = 0.0f;
    st.mTotalBytesReceived     = 0;
    st.mTotalBytesSent         = 0;
    st.mCurrentBytesSendBuffer = 0;
    st.mICEState               = 0;
    st.mUsingRelays            = false;
    return st;
}

} // namespace coral_fans::cfsp::ghost

#endif // LL_PLAT_S
