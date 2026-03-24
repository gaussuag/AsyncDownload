#include "asyncdownload/telemetry/telemetry_collector.hpp"

#include <chrono>
#include <cstdint>
#include <thread>

namespace asyncdownload::telemetry {

namespace {

constexpr double kEmaPreviousWeight = 0.8;
constexpr double kEmaCurrentWeight = 0.2;

[[nodiscard]] double compute_instantaneous_speed(const std::uint64_t bytes,
                                                 const std::uint64_t delta_ns) noexcept {
    if (delta_ns == 0U) {
        return 0.0;
    }

    return static_cast<double>(bytes) * 1'000'000'000.0 / static_cast<double>(delta_ns);
}

[[nodiscard]] std::int64_t clamp_inflight_bytes(const std::uint64_t downloaded_bytes,
                                                const std::uint64_t persisted_bytes) noexcept {
    if (downloaded_bytes <= persisted_bytes) {
        return 0;
    }

    const auto difference = downloaded_bytes - persisted_bytes;
    return static_cast<std::int64_t>(difference);
}

[[nodiscard]] std::int64_t snapshot_watermark_now() noexcept {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            TelemetryClock::now().time_since_epoch())
            .count());
}

} // namespace

TelemetryCollector::TelemetryCollector(TelemetrySink& sink) noexcept
    : sink_(sink) {}

TelemetryCollector::~TelemetryCollector() {
    stop_consuming();
}

void TelemetryCollector::start_consuming() {
    if (running_.exchange(true)) {
        return;
    }

    stop_requested_.store(false, std::memory_order_release);
    worker_thread_ = std::thread(&TelemetryCollector::consume_loop, this);
}

void TelemetryCollector::stop_consuming() {
    stop_requested_.store(true, std::memory_order_release);

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    running_.store(false, std::memory_order_release);
}

void TelemetryCollector::wait_until_drained() const {
    while (sink_.size_approx() != 0U ||
           processing_event_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

ProgressSnapshot TelemetryCollector::current_snapshot() const noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    ProgressSnapshot snapshot = snapshot_;
    snapshot.network_bytes_per_second = network_speed_ema_;
    snapshot.disk_bytes_per_second = disk_speed_ema_;
    snapshot.watermark_timestamp_ns = snapshot_watermark_now();
    return snapshot;
}

PerformanceSummary TelemetryCollector::final_summary() const noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    PerformanceSummary result = summary_;
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
    result.time_to_first_byte_ms =
        first_byte_received_ns_.has_value() && first_byte_received_ns_.value() >= task_started_ns_
            ? static_cast<std::int64_t>((first_byte_received_ns_.value() - task_started_ns_) /
                                        1'000'000U)
            : 0;

    const auto end_timestamp_ns =
        task_completed_ ? task_completed_ns_ : latest_event_timestamp_ns_;
    if (task_started_ && end_timestamp_ns > task_started_ns_) {
        const auto duration_ns = end_timestamp_ns - task_started_ns_;
        result.average_network_bytes_per_second =
            compute_instantaneous_speed(total_download_bytes_, duration_ns);
        result.average_disk_bytes_per_second =
            compute_instantaneous_speed(total_persist_bytes_, duration_ns);
    } else {
        result.average_network_bytes_per_second = 0.0;
        result.average_disk_bytes_per_second = 0.0;
    }

    return result;
}

std::size_t TelemetryCollector::processed_event_count() const noexcept {
    return processed_event_count_.load(std::memory_order_acquire);
}

std::optional<TelemetryEventType> TelemetryCollector::last_event_type() const noexcept {
    if (!has_last_event_.load(std::memory_order_acquire)) {
        return std::nullopt;
    }

    return static_cast<TelemetryEventType>(last_event_type_.load(std::memory_order_acquire));
}

void TelemetryCollector::consume_loop() {
    TelemetryEvent event;

    while (true) {
        if (sink_.wait_dequeue_timed(event, std::chrono::milliseconds(10))) {
            processing_event_.store(true, std::memory_order_release);
            handle_event(event);
            processing_event_.store(false, std::memory_order_release);
            continue;
        }

        if (stop_requested_.load(std::memory_order_acquire) && sink_.size_approx() == 0U) {
            break;
        }
    }
}

void TelemetryCollector::handle_event(const TelemetryEvent& event) noexcept {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);

        if (!task_completed_ || event.type == TelemetryEventType::task_completed) {
            if (event.timestamp_ns > latest_event_timestamp_ns_) {
                latest_event_timestamp_ns_ = event.timestamp_ns;
            }
        }

        if (task_completed_) {
            if (event.type != TelemetryEventType::task_completed) {
                last_event_type_.store(static_cast<std::uint16_t>(event.type),
                                       std::memory_order_release);
                has_last_event_.store(true, std::memory_order_release);
                processed_event_count_.fetch_add(1, std::memory_order_acq_rel);
                return;
            }
        }

        switch (event.type) {
        case TelemetryEventType::task_started:
            if (!task_started_) {
                task_started_ = true;
                task_completed_ = false;
                task_started_ns_ = event.timestamp_ns;
                task_completed_ns_ = 0U;
                latest_event_timestamp_ns_ = event.timestamp_ns;
                first_byte_received_ns_.reset();
                first_byte_received_set_ = false;
                last_network_timestamp_ns_ = 0U;
                last_disk_timestamp_ns_ = 0U;
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
                snapshot_ = ProgressSnapshot{};
                summary_ = PerformanceSummary{};
            }
            break;

        case TelemetryEventType::first_byte_received:
            if (task_started_ && !first_byte_received_set_ && event.timestamp_ns >= task_started_ns_) {
                first_byte_received_ns_ = event.timestamp_ns;
                first_byte_received_set_ = true;
                summary_.time_to_first_byte_ms =
                    static_cast<std::int64_t>((event.timestamp_ns - task_started_ns_) / 1'000'000U);
            }
            break;

        case TelemetryEventType::download_delta:
            if (task_started_) {
                const auto bytes = event.payload.as_download_delta().bytes_count;
                ++packets_enqueued_total_;
                total_packet_bytes_ += bytes;
                if (static_cast<std::size_t>(bytes) > max_packet_size_bytes_) {
                    max_packet_size_bytes_ = static_cast<std::size_t>(bytes);
                }

                if (last_network_timestamp_ns_ != 0U && event.timestamp_ns > last_network_timestamp_ns_) {
                    const double current_speed =
                        compute_instantaneous_speed(bytes, event.timestamp_ns - last_network_timestamp_ns_);
                    if (network_speed_ema_ == 0.0) {
                        network_speed_ema_ = current_speed;
                    } else {
                        network_speed_ema_ = kEmaPreviousWeight * network_speed_ema_ +
                                             kEmaCurrentWeight * current_speed;
                    }
                }
                last_network_timestamp_ns_ = event.timestamp_ns;
                total_download_bytes_ += bytes;

                snapshot_.downloaded_bytes = static_cast<std::int64_t>(total_download_bytes_);
                snapshot_.inflight_bytes =
                    clamp_inflight_bytes(total_download_bytes_, total_persist_bytes_);
                if (snapshot_.inflight_bytes > max_inflight_bytes_) {
                    max_inflight_bytes_ = snapshot_.inflight_bytes;
                }
            }
            break;

        case TelemetryEventType::persist_delta:
            if (task_started_) {
                const auto bytes = event.payload.as_persist_delta().bytes_count;
                if (last_disk_timestamp_ns_ != 0U && event.timestamp_ns > last_disk_timestamp_ns_) {
                    const double current_speed =
                        compute_instantaneous_speed(bytes, event.timestamp_ns - last_disk_timestamp_ns_);
                    if (disk_speed_ema_ == 0.0) {
                        disk_speed_ema_ = current_speed;
                    } else {
                        disk_speed_ema_ = kEmaPreviousWeight * disk_speed_ema_ +
                                          kEmaCurrentWeight * current_speed;
                    }
                }
                last_disk_timestamp_ns_ = event.timestamp_ns;
                total_persist_bytes_ += bytes;

                snapshot_.persisted_bytes = static_cast<std::int64_t>(total_persist_bytes_);
                snapshot_.inflight_bytes =
                    clamp_inflight_bytes(total_download_bytes_, total_persist_bytes_);
            }
            break;

        case TelemetryEventType::queue_paused:
            if (task_started_) {
                ++total_pause_count_;
                const auto pause = event.payload.as_queue_paused();
                if (pause.is_queue_full || pause.reason == TelemetryPauseReason::queue_full) {
                    ++queue_full_pause_count_;
                }
            }
            break;

        case TelemetryEventType::memory_sample:
            if (task_started_) {
                const auto memory_bytes = event.payload.as_memory_sample().memory_bytes;
                snapshot_.memory_bytes = static_cast<std::size_t>(memory_bytes);
                if (static_cast<std::size_t>(memory_bytes) > max_memory_bytes_) {
                    max_memory_bytes_ = static_cast<std::size_t>(memory_bytes);
                }
            }
            break;

        case TelemetryEventType::task_completed:
            if (task_started_ && !task_completed_) {
                task_completed_ = true;
                task_completed_ns_ = event.timestamp_ns;
                latest_event_timestamp_ns_ = event.timestamp_ns;
                summary_.max_memory_bytes = max_memory_bytes_;
                summary_.max_inflight_bytes = max_inflight_bytes_;
                summary_.total_pause_count = total_pause_count_;
                summary_.queue_full_pause_count = queue_full_pause_count_;
                summary_.packets_enqueued_total = packets_enqueued_total_;
                summary_.max_packet_size_bytes = max_packet_size_bytes_;
                summary_.average_packet_size_bytes = packets_enqueued_total_ == 0U
                                                        ? 0.0
                                                        : static_cast<double>(total_packet_bytes_) /
                                                              static_cast<double>(packets_enqueued_total_);
                summary_.time_to_first_byte_ms =
                    first_byte_received_ns_.has_value() &&
                            first_byte_received_ns_.value() >= task_started_ns_
                        ? static_cast<std::int64_t>((first_byte_received_ns_.value() -
                                                    task_started_ns_) /
                                                   1'000'000U)
                        : 0;
                if (task_completed_ns_ > task_started_ns_) {
                    const auto duration_ns = task_completed_ns_ - task_started_ns_;
                    summary_.average_network_bytes_per_second =
                        compute_instantaneous_speed(total_download_bytes_, duration_ns);
                    summary_.average_disk_bytes_per_second =
                        compute_instantaneous_speed(total_persist_bytes_, duration_ns);
                } else {
                    summary_.average_network_bytes_per_second = 0.0;
                    summary_.average_disk_bytes_per_second = 0.0;
                }
            }
            break;
        }
    }

    last_event_type_.store(static_cast<std::uint16_t>(event.type), std::memory_order_release);
    has_last_event_.store(true, std::memory_order_release);
    processed_event_count_.fetch_add(1, std::memory_order_acq_rel);
}

} // namespace asyncdownload::telemetry
