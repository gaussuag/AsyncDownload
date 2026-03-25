#pragma once

#include "asyncdownload/telemetry/telemetry_collector.hpp"
#include "asyncdownload/telemetry/telemetry_event.hpp"
#include "asyncdownload/types.hpp"

#include <cstdint>

namespace asyncdownload::telemetry {

class TelemetrySession {
public:
    TelemetrySession() = default;

    TelemetrySession(const TelemetrySession&) = delete;
    TelemetrySession& operator=(const TelemetrySession&) = delete;

    void record_task_started(
        TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;
    void record_first_byte_received(
        TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;
    void record_download_delta(std::uint64_t bytes) noexcept;
    void record_persist_delta(std::uint64_t bytes) noexcept;
    void record_pause(TelemetryPauseReason reason, bool is_queue_full) noexcept;
    void record_memory_sample(std::uint64_t memory_bytes) noexcept;
    void record_task_completed(
        TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;

    [[nodiscard]] ProgressSnapshot current_snapshot() const noexcept;
    [[nodiscard]] PerformanceSummary final_summary(
        TelemetryClock::time_point now = TelemetryClock::now()) const noexcept;

private:
    TelemetryCollector collector_{};
};

} // namespace asyncdownload::telemetry
