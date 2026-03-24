#include "asyncdownload/telemetry/telemetry_session.hpp"

namespace asyncdownload::telemetry {

TelemetrySession::TelemetrySession()
    : collector_(sink_) {
    collector_.start_consuming();
}

TelemetrySession::~TelemetrySession() {
    collector_.stop_consuming();
}

void TelemetrySession::record_task_started() noexcept {
    emit(TelemetryEventType::task_started, TelemetryPayload::task_started_payload());
}

void TelemetrySession::record_first_byte_received() noexcept {
    emit(TelemetryEventType::first_byte_received, TelemetryPayload::first_byte_received_payload());
}

void TelemetrySession::record_download_delta(const std::uint64_t bytes) noexcept {
    emit(TelemetryEventType::download_delta, TelemetryPayload::download_delta_payload(bytes));
}

void TelemetrySession::record_persist_delta(const std::uint64_t bytes) noexcept {
    emit(TelemetryEventType::persist_delta, TelemetryPayload::persist_delta_payload(bytes));
}

void TelemetrySession::record_pause(const TelemetryPauseReason reason,
                                    const bool is_queue_full) noexcept {
    emit(TelemetryEventType::queue_paused,
         TelemetryPayload::queue_paused_payload(reason, is_queue_full));
}

void TelemetrySession::record_memory_sample(const std::uint64_t memory_bytes) noexcept {
    emit(TelemetryEventType::memory_sample,
         TelemetryPayload::memory_sample_payload(memory_bytes));
}

void TelemetrySession::record_task_completed() noexcept {
    emit(TelemetryEventType::task_completed,
         TelemetryPayload::task_completed_payload(TelemetryCompletionStatus::success));
}

ProgressSnapshot TelemetrySession::current_snapshot() const noexcept {
    return collector_.current_snapshot();
}

PerformanceSummary TelemetrySession::final_summary() const noexcept {
    collector_.wait_until_drained();
    return collector_.final_summary();
}

void TelemetrySession::emit(const TelemetryEventType type, const TelemetryPayload& payload) noexcept {
    const bool enqueued = sink_.enqueue(make_telemetry_event(type, payload));
    static_cast<void>(enqueued);
}

} // namespace asyncdownload::telemetry
