#include "SpDriver.h"

#include "SpDriverGhost.h"
#include "SpDriverSim.h"

#include <memory>

namespace coral_fans::cfsp::simulated_player::driver {

namespace {

std::unique_ptr<SpDriver>& instance() {
    static std::unique_ptr<SpDriver> driver;
    return driver;
}

std::unique_ptr<SpDriver> make(config::SimPlayerImpl impl) {
    switch (impl) {
    case config::SimPlayerImpl::simulated:
        return std::make_unique<SimulatedDriver>();
    case config::SimPlayerImpl::protocol:
    default:
        return std::make_unique<GhostDriver>();
    }
}

} // namespace

SpDriver& select(config::SimPlayerImpl impl) {
    auto& driver = instance();
    if (!driver || driver->type() != impl) driver = make(impl);
    return *driver;
}

SpDriver& current() {
    auto& driver = instance();
    if (!driver) driver = make(config::SimPlayerImpl::protocol);
    return *driver;
}

} // namespace coral_fans::cfsp::simulated_player::driver
