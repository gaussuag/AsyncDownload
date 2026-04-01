#include "asyncdownload/telemetry/telemetry_collector.hpp"

#include <algorithm>
#include <chrono>

namespace asyncdownload::telemetry {

namespace {

constexpr double kEmaPreviousWeight = 0.8;
constexpr double kEmaCurrentWeight = 0.2;

[[nodiscard]] double compute_instantaneous_speed(const std::uint64_t bytes,
                                                 const TelemetryClock::duration delta) noexcept {
    const auto delta_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(delta).count();
    if (delta_ns <= 0) {
        return 0.0;
    }

    return static_cast<double>(bytes) * 1'000'000'000.0 / static_cast<double>(delta_ns);
}

[[nodiscard]] std::int64_t clamp_inflight_bytes(const std::uint64_t downloaded_bytes,
                                                const std::uint64_t persisted_bytes) noexcept {
    if (downloaded_bytes <= persisted_bytes) {
        return 0;
    }

    return static_cast<std::int64_t>(downloaded_bytes - persisted_bytes);
}

} // namespace

void TelemetryCollector::record_task_started(const TelemetryClock::time_point timestamp) noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    reset_state(timestamp);
}

void TelemetryCollector::record_first_byte_received(
    const TelemetryClock::time_point timestamp) noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!task_started_ || task_completed_) {
        return;
    }

    update_latest_event_timestamp(timestamp);
    if (!first_byte_received_at_.has_value() || timestamp < *first_byte_received_at_) {
        first_byte_received_at_ = timestamp;
    }
}

void TelemetryCollector::record_download_delta(const std::uint64_t bytes,
                                               const TelemetryClock::time_point timestamp) noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!task_started_ || task_completed_) {
        return;
    }

    update_latest_event_timestamp(timestamp);
    ++packets_enqueued_total_;
    total_packet_bytes_ += bytes;
    max_packet_size_bytes_ = std::max(max_packet_size_bytes_, static_cast<std::size_t>(bytes));
    if (last_network_sample_at_.has_value()) {
        const auto current_speed = compute_instantaneous_speed(bytes, timestamp - *last_network_sample_at_);
        if (network_speed_ema_ == 0.0) {
            network_speed_ema_ = current_speed;
        } else {
            network_speed_ema_ = kEmaPreviousWeight * network_speed_ema_ +
                                 kEmaCurrentWeight * current_speed;
        }
    }
    last_network_sample_at_ = timestamp;
    total_download_bytes_ += bytes;
    const auto inflight_bytes = clamp_inflight_bytes(total_download_bytes_, total_persist_bytes_);
    max_inflight_bytes_ = std::max(max_inflight_bytes_, inflight_bytes);
}

void TelemetryCollector::record_persist_delta(const std::uint64_t bytes,
                                              const TelemetryClock::time_point timestamp) noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!task_started_ || task_completed_) {
        return;
    }

    update_latest_event_timestamp(timestamp);
    if (last_disk_sample_at_.has_value()) {
        const auto current_speed = compute_instantaneous_speed(bytes, timestamp - *last_disk_sample_at_);
        if (disk_speed_ema_ == 0.0) {
            disk_speed_ema_ = current_speed;
        } else {
            disk_speed_ema_ = kEmaPreviousWeight * disk_speed_ema_ +
                              kEmaCurrentWeight * current_speed;
        }
    }
    last_disk_sample_at_ = timestamp;
    total_persist_bytes_ += bytes;
}

void TelemetryCollector::record_pause(const TelemetryPauseReason reason,
                                      const bool is_queue_full,
                                      const TelemetryClock::time_point timestamp) noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!task_started_ || task_completed_) {
        return;
    }

    update_latest_event_timestamp(timestamp);
    ++total_pause_count_;
    if (is_queue_full || reason == TelemetryPauseReason::queue_full) {
        ++queue_full_pause_count_;
    }
}

void TelemetryCollector::record_memory_sample(const std::uint64_t memory_bytes,
                                              const TelemetryClock::time_point timestamp) noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!task_started_ || task_completed_) {
        return;
    }

    update_latest_event_timestamp(timestamp);
    latest_memory_bytes_ = static_cast<std::size_t>(memory_bytes);
    max_memory_bytes_ = std::max(max_memory_bytes_, latest_memory_bytes_);
}

void TelemetryCollector::record_task_completed(const TelemetryClock::time_point timestamp) noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!task_started_ || task_completed_) {
        return;
    }

    task_completed_ = true;
    task_completed_at_ = std::max(timestamp, latest_event_at_);
    latest_event_at_ = task_completed_at_;
}

ProgressSnapshot TelemetryCollector::current_snapshot() const noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    ProgressSnapshot snapshot{};
    snapshot.downloaded_bytes = static_cast<std::int64_t>(total_download_bytes_);
    snapshot.persisted_bytes = static_cast<std::int64_t>(total_persist_bytes_);
    snapshot.inflight_bytes = clamp_inflight_bytes(total_download_bytes_, total_persist_bytes_);
    snapshot.memory_bytes = latest_memory_bytes_;
    snapshot.network_bytes_per_second = network_speed_ema_;
    snapshot.disk_bytes_per_second = disk_speed_ema_;
    return snapshot;
}

PerformanceSummary TelemetryCollector::final_summary(const TelemetryClock::time_point now) const noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    PerformanceSummary result{};
    result.max_memory_bytes = max_memory_bytes_;
    result.max_inflight_bytes = max_inflight_bytes_;
    result.total_pause_count = total_pause_count_;
    result.queue_full_pause_count = queue_full_pause_count_;
    result.packets_enqueued_total = packets_enqueued_total_;
    result.max_packet_size_bytes = max_packet_size_bytes_;
    result.average_packet_size_bytes = packets_enqueued_total_ == 0U
                                           ? 0.0
                                           : static_cast<double>(total_packet_bytes_) /
                                                 static_cast<double>(packets_enqueued_total_);
    if (task_started_ && first_byte_received_at_.has_value() &&
        *first_byte_received_at_ >= task_started_at_) {
        result.time_to_first_byte_ms = static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                *first_byte_received_at_ - task_started_at_)
                .count());
    }

    const auto end_timestamp = task_completed_ ? task_completed_at_ : std::max(latest_event_at_, now);
    if (task_started_ && end_timestamp > task_started_at_) {
        const auto duration = end_timestamp - task_started_at_;
        result.average_network_bytes_per_second =
            compute_instantaneous_speed(total_download_bytes_, duration);
        result.average_disk_bytes_per_second =
            compute_instantaneous_speed(total_persist_bytes_, duration);
    }

    return result;
}

void TelemetryCollector::reset_state(const TelemetryClock::time_point timestamp) noexcept {
    task_started_ = true;
    task_completed_ = false;
    task_started_at_ = timestamp;
    task_completed_at_ = timestamp;
    latest_event_at_ = timestamp;
    first_byte_received_at_.reset();
    last_network_sample_at_.reset();
    last_disk_sample_at_.reset();
    network_speed_ema_ = 0.0;
    disk_speed_ema_ = 0.0;
    total_download_bytes_ = 0U;
    total_persist_bytes_ = 0U;
    packets_enqueued_total_ = 0U;
    total_packet_bytes_ = 0U;
    max_packet_size_bytes_ = 0U;
    max_memory_bytes_ = 0U;
    max_inflight_bytes_ = 0;
    total_pause_count_ = 0U;
    queue_full_pause_count_ = 0U;
    latest_memory_bytes_ = 0U;
}

void TelemetryCollector::update_latest_event_timestamp(
    const TelemetryClock::time_point timestamp) noexcept {
    if (timestamp > latest_event_at_) {
        latest_event_at_ = timestamp;
    }
}

} // namespace asyncdownload::telemetry
