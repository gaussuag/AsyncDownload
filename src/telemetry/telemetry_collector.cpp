#include "asyncdownload/telemetry/telemetry_collector.hpp"

#include <chrono>
#include <thread>

namespace asyncdownload::telemetry {

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
    while (sink_.size_approx() != 0U) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

ProgressSnapshot TelemetryCollector::current_snapshot() const noexcept {
    return snapshot_;
}

PerformanceSummary TelemetryCollector::final_summary() const noexcept {
    return summary_;
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
            handle_event(event);
            continue;
        }

        if (stop_requested_.load(std::memory_order_acquire) && sink_.size_approx() == 0U) {
            break;
        }
    }
}

void TelemetryCollector::handle_event(const TelemetryEvent& event) noexcept {
    last_event_type_.store(static_cast<std::uint16_t>(event.type), std::memory_order_release);
    has_last_event_.store(true, std::memory_order_release);
    processed_event_count_.fetch_add(1, std::memory_order_acq_rel);
}

} // namespace asyncdownload::telemetry
