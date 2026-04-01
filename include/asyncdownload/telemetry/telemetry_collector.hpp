#pragma once

#include "asyncdownload/telemetry/telemetry_event.hpp"
#include "asyncdownload/types.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

namespace asyncdownload::telemetry {

class TelemetryCollector {
public:
    TelemetryCollector() = default;

    TelemetryCollector(const TelemetryCollector&) = delete;
    TelemetryCollector& operator=(const TelemetryCollector&) = delete;

    void record_task_started(TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;
    void record_first_byte_received(
        TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;
    void record_download_delta(std::uint64_t bytes,
                               TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;
    void record_persist_delta(std::uint64_t bytes,
                              TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;
    void record_pause(TelemetryPauseReason reason,
                      bool is_queue_full,
                      TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;
    void record_memory_sample(std::uint64_t memory_bytes,
                              TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;
    void record_task_completed(
        TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;

    [[nodiscard]] ProgressSnapshot current_snapshot() const noexcept;
    [[nodiscard]] PerformanceSummary final_summary(
        TelemetryClock::time_point now = TelemetryClock::now()) const noexcept;

private:
    void reset_state(TelemetryClock::time_point timestamp) noexcept;
    void update_latest_event_timestamp(TelemetryClock::time_point timestamp) noexcept;

    mutable std::mutex state_mutex_{};
    bool task_started_{false};
    bool task_completed_{false};
    TelemetryClock::time_point task_started_at_{};
    TelemetryClock::time_point task_completed_at_{};
    TelemetryClock::time_point latest_event_at_{};
    std::optional<TelemetryClock::time_point> first_byte_received_at_{};
    std::optional<TelemetryClock::time_point> last_network_sample_at_{};
    std::optional<TelemetryClock::time_point> last_disk_sample_at_{};
    double network_speed_ema_{0.0};
    double disk_speed_ema_{0.0};
    std::uint64_t total_download_bytes_{0};
    std::uint64_t total_persist_bytes_{0};
    std::size_t packets_enqueued_total_{0};
    std::uint64_t total_packet_bytes_{0};
    std::size_t max_packet_size_bytes_{0};
    std::size_t max_memory_bytes_{0};
    std::int64_t max_inflight_bytes_{0};
    std::size_t total_pause_count_{0};
    std::size_t queue_full_pause_count_{0};
    std::size_t latest_memory_bytes_{0};
};

} // namespace asyncdownload::telemetry
