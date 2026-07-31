#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

#include "asyncdownload/telemetry/telemetry_event.hpp"
#include "asyncdownload/types.hpp"

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

    void record_download_delta_at(
        std::uint64_t bytes,
        TelemetryClock::time_point timestamp) noexcept;
    void record_persist_delta_at(
        std::uint64_t bytes,
        TelemetryClock::time_point timestamp) noexcept;
    void record_pause_at(
        TelemetryPauseReason reason,
        bool is_queue_full,
        TelemetryClock::time_point timestamp) noexcept;
    void record_memory_sample_at(
        std::uint64_t memory_bytes,
        TelemetryClock::time_point timestamp) noexcept;

    [[nodiscard]] ProgressSnapshot current_snapshot() const noexcept;
    [[nodiscard]] PerformanceSummary final_summary(
        TelemetryClock::time_point now = TelemetryClock::now()) const noexcept;

private:
    struct AggregationState {
        bool task_started = false;
        bool task_completed = false;
        TelemetryClock::time_point task_started_at{};
        TelemetryClock::time_point task_completed_at{};
        TelemetryClock::time_point latest_event_at{};
        std::optional<TelemetryClock::time_point>
            first_byte_received_at{};
        std::optional<TelemetryClock::time_point>
            last_network_sample_at{};
        std::optional<TelemetryClock::time_point>
            last_disk_sample_at{};
        double network_speed_ema = 0.0;
        double disk_speed_ema = 0.0;
        std::uint64_t total_download_bytes = 0;
        std::uint64_t total_persist_bytes = 0;
        std::size_t packets_enqueued_total = 0;
        std::uint64_t total_packet_bytes = 0;
        std::size_t max_packet_size_bytes = 0;
        std::size_t max_memory_bytes = 0;
        std::int64_t max_inflight_bytes = 0;
        std::size_t total_pause_count = 0;
        std::size_t queue_full_pause_count = 0;
        std::size_t latest_memory_bytes = 0;
    };

    void reset_state(TelemetryClock::time_point timestamp) noexcept;
    void update_latest_event_timestamp(
        TelemetryClock::time_point timestamp) noexcept;

    mutable std::mutex state_mutex_{};
    AggregationState state_{};
};

class TelemetryCollector {
public:
    TelemetryCollector() = default;

    TelemetryCollector(const TelemetryCollector&) = delete;
    TelemetryCollector& operator=(const TelemetryCollector&) = delete;

    void record_task_started(
        TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;
    void record_first_byte_received(
        TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;
    void record_download_delta(
        std::uint64_t bytes,
        TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;
    void record_persist_delta(
        std::uint64_t bytes,
        TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;
    void record_pause(
        TelemetryPauseReason reason,
        bool is_queue_full,
        TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;
    void record_memory_sample(
        std::uint64_t memory_bytes,
        TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;
    void record_task_completed(
        TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept;

    [[nodiscard]] ProgressSnapshot current_snapshot() const noexcept;
    [[nodiscard]] PerformanceSummary final_summary(
        TelemetryClock::time_point now = TelemetryClock::now()) const noexcept;

private:
    TelemetrySession session_{};
};

}
