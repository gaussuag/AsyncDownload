#include <algorithm>
#include <chrono>
#include <limits>

#include "asyncdownload/telemetry/telemetry_session.hpp"

namespace asyncdownload::telemetry {
namespace {

constexpr double EMA_PREVIOUS_WEIGHT = 0.8;
constexpr double EMA_CURRENT_WEIGHT = 0.2;

template <typename Value>
Value saturating_add(
    const Value lhs,
    const Value rhs) noexcept {
    const auto maximum =
        std::numeric_limits<Value>::max();
    return rhs > maximum - lhs
        ? maximum
        : lhs + rhs;
}

template <typename Value>
Value saturating_increment(
    const Value value) noexcept {
    return saturating_add<Value>(value, 1);
}

std::size_t saturating_size(
    const std::uint64_t value) noexcept {
    if constexpr (
        std::numeric_limits<std::size_t>::max() <
        std::numeric_limits<std::uint64_t>::max()) {
        if (value >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())) {
            return std::numeric_limits<std::size_t>::max();
        }
    }
    return static_cast<std::size_t>(value);
}

std::int64_t saturating_int64(
    const std::uint64_t value) noexcept {
    const auto maximum =
        static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max());
    return value > maximum
        ? std::numeric_limits<std::int64_t>::max()
        : static_cast<std::int64_t>(value);
}

double compute_instantaneous_speed(
    const std::uint64_t bytes,
    const TelemetryClock::duration delta) noexcept {
    const auto delta_ns =
        std::chrono::duration_cast<
            std::chrono::nanoseconds>(delta)
            .count();
    if (delta_ns <= 0) {
        return 0.0;
    }
    return static_cast<double>(bytes) *
        1'000'000'000.0 /
        static_cast<double>(delta_ns);
}

std::int64_t clamp_inflight_bytes(
    const std::uint64_t downloaded_bytes,
    const std::uint64_t persisted_bytes) noexcept {
    if (downloaded_bytes <= persisted_bytes) {
        return 0;
    }
    return saturating_int64(
        downloaded_bytes - persisted_bytes);
}

}

void TelemetrySession::record_task_started(
    const TelemetryClock::time_point timestamp) noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    reset_state(timestamp);
}

void TelemetrySession::record_first_byte_received(
    const TelemetryClock::time_point timestamp) noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!state_.task_started ||
        state_.task_completed) {
        return;
    }
    update_latest_event_timestamp(timestamp);
    if (!state_.first_byte_received_at.has_value() ||
        timestamp < *state_.first_byte_received_at) {
        state_.first_byte_received_at = timestamp;
    }
}

void TelemetrySession::record_download_delta(
    const std::uint64_t bytes) noexcept {
    record_download_delta_at(
        bytes,
        TelemetryClock::now());
}

void TelemetrySession::record_download_delta_at(
    const std::uint64_t bytes,
    const TelemetryClock::time_point timestamp) noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!state_.task_started ||
        state_.task_completed) {
        return;
    }
    update_latest_event_timestamp(timestamp);
    state_.packets_enqueued_total =
        saturating_increment(
            state_.packets_enqueued_total);
    state_.total_packet_bytes =
        saturating_add(
            state_.total_packet_bytes,
            bytes);
    state_.max_packet_size_bytes =
        std::max(
            state_.max_packet_size_bytes,
            saturating_size(bytes));
    if (state_.last_network_sample_at.has_value()) {
        const auto current_speed =
            compute_instantaneous_speed(
                bytes,
                timestamp -
                    *state_.last_network_sample_at);
        if (state_.network_speed_ema == 0.0) {
            state_.network_speed_ema =
                current_speed;
        } else {
            state_.network_speed_ema =
                EMA_PREVIOUS_WEIGHT *
                    state_.network_speed_ema +
                EMA_CURRENT_WEIGHT *
                    current_speed;
        }
    }
    state_.last_network_sample_at = timestamp;
    state_.total_download_bytes =
        saturating_add(
            state_.total_download_bytes,
            bytes);
    state_.max_inflight_bytes =
        std::max(
            state_.max_inflight_bytes,
            clamp_inflight_bytes(
                state_.total_download_bytes,
                state_.total_persist_bytes));
}

void TelemetrySession::record_persist_delta(
    const std::uint64_t bytes) noexcept {
    record_persist_delta_at(
        bytes,
        TelemetryClock::now());
}

void TelemetrySession::record_persist_delta_at(
    const std::uint64_t bytes,
    const TelemetryClock::time_point timestamp) noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!state_.task_started ||
        state_.task_completed) {
        return;
    }
    update_latest_event_timestamp(timestamp);
    if (state_.last_disk_sample_at.has_value()) {
        const auto current_speed =
            compute_instantaneous_speed(
                bytes,
                timestamp -
                    *state_.last_disk_sample_at);
        if (state_.disk_speed_ema == 0.0) {
            state_.disk_speed_ema =
                current_speed;
        } else {
            state_.disk_speed_ema =
                EMA_PREVIOUS_WEIGHT *
                    state_.disk_speed_ema +
                EMA_CURRENT_WEIGHT *
                    current_speed;
        }
    }
    state_.last_disk_sample_at = timestamp;
    state_.total_persist_bytes =
        saturating_add(
            state_.total_persist_bytes,
            bytes);
}

void TelemetrySession::record_pause(
    const TelemetryPauseReason reason,
    const bool is_queue_full) noexcept {
    record_pause_at(
        reason,
        is_queue_full,
        TelemetryClock::now());
}

void TelemetrySession::record_pause_at(
    const TelemetryPauseReason reason,
    const bool is_queue_full,
    const TelemetryClock::time_point timestamp) noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!state_.task_started ||
        state_.task_completed) {
        return;
    }
    update_latest_event_timestamp(timestamp);
    state_.total_pause_count =
        saturating_increment(
            state_.total_pause_count);
    if (is_queue_full ||
        reason == TelemetryPauseReason::queue_full) {
        state_.queue_full_pause_count =
            saturating_increment(
                state_.queue_full_pause_count);
    }
}

void TelemetrySession::record_memory_sample(
    const std::uint64_t memory_bytes) noexcept {
    record_memory_sample_at(
        memory_bytes,
        TelemetryClock::now());
}

void TelemetrySession::record_memory_sample_at(
    const std::uint64_t memory_bytes,
    const TelemetryClock::time_point timestamp) noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!state_.task_started ||
        state_.task_completed) {
        return;
    }
    update_latest_event_timestamp(timestamp);
    state_.latest_memory_bytes =
        saturating_size(memory_bytes);
    state_.max_memory_bytes =
        std::max(
            state_.max_memory_bytes,
            state_.latest_memory_bytes);
}

void TelemetrySession::record_task_completed(
    const TelemetryClock::time_point timestamp) noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!state_.task_started ||
        state_.task_completed) {
        return;
    }
    state_.task_completed = true;
    state_.task_completed_at =
        std::max(
            timestamp,
            state_.latest_event_at);
    state_.latest_event_at =
        state_.task_completed_at;
}

ProgressSnapshot
TelemetrySession::current_snapshot() const noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    ProgressSnapshot snapshot{};
    snapshot.downloaded_bytes =
        saturating_int64(
            state_.total_download_bytes);
    snapshot.persisted_bytes =
        saturating_int64(
            state_.total_persist_bytes);
    snapshot.inflight_bytes =
        clamp_inflight_bytes(
            state_.total_download_bytes,
            state_.total_persist_bytes);
    snapshot.memory_bytes =
        state_.latest_memory_bytes;
    snapshot.network_bytes_per_second =
        state_.network_speed_ema;
    snapshot.disk_bytes_per_second =
        state_.disk_speed_ema;
    return snapshot;
}

PerformanceSummary TelemetrySession::final_summary(
    const TelemetryClock::time_point now) const noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    PerformanceSummary result{};
    result.max_memory_bytes =
        state_.max_memory_bytes;
    result.max_inflight_bytes =
        state_.max_inflight_bytes;
    result.total_pause_count =
        state_.total_pause_count;
    result.queue_full_pause_count =
        state_.queue_full_pause_count;
    result.packets_enqueued_total =
        state_.packets_enqueued_total;
    result.max_packet_size_bytes =
        state_.max_packet_size_bytes;
    result.average_packet_size_bytes =
        state_.packets_enqueued_total == 0U
        ? 0.0
        : static_cast<double>(
              state_.total_packet_bytes) /
            static_cast<double>(
                state_.packets_enqueued_total);
    if (state_.task_started &&
        state_.first_byte_received_at.has_value() &&
        *state_.first_byte_received_at >=
            state_.task_started_at) {
        result.time_to_first_byte_ms =
            static_cast<std::int64_t>(
                std::chrono::duration_cast<
                    std::chrono::milliseconds>(
                    *state_.first_byte_received_at -
                    state_.task_started_at)
                    .count());
    }

    const auto end_timestamp =
        state_.task_completed
        ? state_.task_completed_at
        : std::max(
              state_.latest_event_at,
              now);
    if (state_.task_started &&
        end_timestamp > state_.task_started_at) {
        const auto duration =
            end_timestamp -
            state_.task_started_at;
        result.average_network_bytes_per_second =
            compute_instantaneous_speed(
                state_.total_download_bytes,
                duration);
        result.average_disk_bytes_per_second =
            compute_instantaneous_speed(
                state_.total_persist_bytes,
                duration);
    }
    return result;
}

void TelemetrySession::reset_state(
    const TelemetryClock::time_point timestamp) noexcept {
    state_ = AggregationState{};
    state_.task_started = true;
    state_.task_started_at = timestamp;
    state_.task_completed_at = timestamp;
    state_.latest_event_at = timestamp;
}

void TelemetrySession::update_latest_event_timestamp(
    const TelemetryClock::time_point timestamp) noexcept {
    if (timestamp > state_.latest_event_at) {
        state_.latest_event_at = timestamp;
    }
}

}
