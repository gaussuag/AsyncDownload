#pragma once

#include <chrono>
#include <cstdint>

namespace asyncdownload::telemetry {

using TelemetryClock = std::chrono::steady_clock;

enum class TelemetryPauseReason : std::uint8_t {
    none = 0,
    queue_full = 1,
    memory_pressure = 2,
    gap = 3
};

[[nodiscard]] inline std::uint64_t telemetry_timestamp_ns(
    const TelemetryClock::time_point timestamp) noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(timestamp.time_since_epoch()).count());
}

} // namespace asyncdownload::telemetry
