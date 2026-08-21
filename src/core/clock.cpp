#include "ptl/core/clock.hpp"

#include <stdexcept>
#include <string>

namespace ptl {

void SimulatedClock::advance_to(Timestamp ts) {
    if (ts < now_) {
        // Loud rather than silent. An event feed that goes backwards means the
        // merge is broken or the data is unsorted, and either way every
        // downstream point-in-time guarantee is void. Clamping would hide it.
        throw std::logic_error(
            "SimulatedClock::advance_to moved backwards: from " + to_iso8601(now_) +
            " to " + to_iso8601(ts));
    }
    now_ = ts;
}

Timestamp WallClock::now() const noexcept {
    using namespace std::chrono;
    return time_point_cast<nanoseconds>(system_clock::now());
}

}  // namespace ptl
