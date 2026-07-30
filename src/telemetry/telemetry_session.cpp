#include "asyncdownload/telemetry/telemetry_session.hpp"

namespace asyncdownload::telemetry {

void TelemetrySession::record_task_started(const TelemetryClock::time_point timestamp) noexcept {
    collector_.record_task_started(timestamp);
}

void TelemetrySession::record_first_byte_received(
    const TelemetryClock::time_point timestamp) noexcept {
    collector_.record_first_byte_received(timestamp);
}

void TelemetrySession::record_download_delta(const std::uint64_t bytes) noexcept {
    record_download_delta_at(bytes, TelemetryClock::now());
}

void TelemetrySession::record_persist_delta(const std::uint64_t bytes) noexcept {
    record_persist_delta_at(bytes, TelemetryClock::now());
}

void TelemetrySession::record_pause(const TelemetryPauseReason reason,
                                    const bool is_queue_full) noexcept {
    record_pause_at(reason, is_queue_full, TelemetryClock::now());
}

void TelemetrySession::record_memory_sample(const std::uint64_t memory_bytes) noexcept {
    record_memory_sample_at(memory_bytes, TelemetryClock::now());
}

void TelemetrySession::record_task_completed(const TelemetryClock::time_point timestamp) noexcept {
    collector_.record_task_completed(timestamp);
}

void TelemetrySession::record_download_delta_at(
    const std::uint64_t bytes,
    const TelemetryClock::time_point timestamp) noexcept {
    collector_.record_download_delta(bytes, timestamp);
}

void TelemetrySession::record_persist_delta_at(
    const std::uint64_t bytes,
    const TelemetryClock::time_point timestamp) noexcept {
    collector_.record_persist_delta(bytes, timestamp);
}

void TelemetrySession::record_pause_at(
    const TelemetryPauseReason reason,
    const bool is_queue_full,
    const TelemetryClock::time_point timestamp) noexcept {
    collector_.record_pause(reason, is_queue_full, timestamp);
}

void TelemetrySession::record_memory_sample_at(
    const std::uint64_t memory_bytes,
    const TelemetryClock::time_point timestamp) noexcept {
    collector_.record_memory_sample(memory_bytes, timestamp);
}

ProgressSnapshot TelemetrySession::current_snapshot() const noexcept {
    return collector_.current_snapshot();
}

PerformanceSummary TelemetrySession::final_summary(
    const TelemetryClock::time_point now) const noexcept {
    return collector_.final_summary(now);
}

} // namespace asyncdownload::telemetry
