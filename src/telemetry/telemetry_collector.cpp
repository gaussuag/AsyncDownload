#include "asyncdownload/telemetry/telemetry_collector.hpp"

namespace asyncdownload::telemetry {

void TelemetryCollector::record_task_started(
    const TelemetryClock::time_point timestamp) noexcept {
    session_.record_task_started(timestamp);
}

void TelemetryCollector::record_first_byte_received(
    const TelemetryClock::time_point timestamp) noexcept {
    session_.record_first_byte_received(timestamp);
}

void TelemetryCollector::record_download_delta(
    const std::uint64_t bytes,
    const TelemetryClock::time_point timestamp) noexcept {
    session_.record_download_delta_at(
        bytes,
        timestamp);
}

void TelemetryCollector::record_persist_delta(
    const std::uint64_t bytes,
    const TelemetryClock::time_point timestamp) noexcept {
    session_.record_persist_delta_at(
        bytes,
        timestamp);
}

void TelemetryCollector::record_pause(
    const TelemetryPauseReason reason,
    const bool is_queue_full,
    const TelemetryClock::time_point timestamp) noexcept {
    session_.record_pause_at(
        reason,
        is_queue_full,
        timestamp);
}

void TelemetryCollector::record_memory_sample(
    const std::uint64_t memory_bytes,
    const TelemetryClock::time_point timestamp) noexcept {
    session_.record_memory_sample_at(
        memory_bytes,
        timestamp);
}

void TelemetryCollector::record_task_completed(
    const TelemetryClock::time_point timestamp) noexcept {
    session_.record_task_completed(timestamp);
}

ProgressSnapshot
TelemetryCollector::current_snapshot() const noexcept {
    return session_.current_snapshot();
}

PerformanceSummary TelemetryCollector::final_summary(
    const TelemetryClock::time_point now) const noexcept {
    return session_.final_summary(now);
}

}
