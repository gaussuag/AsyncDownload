#pragma once

#include "asyncdownload/telemetry/telemetry_event.hpp"
#include "asyncdownload/telemetry/telemetry_sink.hpp"
#include "asyncdownload/types.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>

namespace asyncdownload::telemetry {

class TelemetryCollector {
public:
    explicit TelemetryCollector(TelemetrySink& sink) noexcept;
    ~TelemetryCollector();

    TelemetryCollector(const TelemetryCollector&) = delete;
    TelemetryCollector& operator=(const TelemetryCollector&) = delete;

    void start_consuming();
    void stop_consuming();
    void wait_until_drained() const;

    [[nodiscard]] ProgressSnapshot current_snapshot() const noexcept;
    [[nodiscard]] PerformanceSummary final_summary() const noexcept;
    [[nodiscard]] std::size_t processed_event_count() const noexcept;
    [[nodiscard]] std::optional<TelemetryEventType> last_event_type() const noexcept;

private:
    void consume_loop();
    void handle_event(const TelemetryEvent& event) noexcept;

    TelemetrySink& sink_;
    mutable std::atomic<bool> stop_requested_{false};
    std::atomic<bool> running_{false};
    mutable std::atomic<bool> processing_event_{false};
    std::thread worker_thread_{};
    std::atomic<std::size_t> processed_event_count_{0};
    std::atomic<std::uint16_t> last_event_type_{
        static_cast<std::uint16_t>(TelemetryEventType::task_started)};
    std::atomic<bool> has_last_event_{false};
    mutable std::mutex state_mutex_{};
    bool task_started_{false};
    bool task_completed_{false};
    std::uint64_t task_started_ns_{0};
    std::uint64_t task_completed_ns_{0};
    std::uint64_t latest_event_timestamp_ns_{0};
    std::optional<std::uint64_t> first_byte_received_ns_{};
    bool first_byte_received_set_{false};
    std::uint64_t last_network_timestamp_ns_{0};
    std::uint64_t last_disk_timestamp_ns_{0};
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
    ProgressSnapshot snapshot_{};
    PerformanceSummary summary_{};
};

} // namespace asyncdownload::telemetry
