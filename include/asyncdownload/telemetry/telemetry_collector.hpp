#pragma once

#include "asyncdownload/telemetry/telemetry_event.hpp"
#include "asyncdownload/telemetry/telemetry_sink.hpp"
#include "asyncdownload/types.hpp"

#include <atomic>
#include <cstddef>
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
    std::thread worker_thread_{};
    std::atomic<std::size_t> processed_event_count_{0};
    std::atomic<std::uint16_t> last_event_type_{
        static_cast<std::uint16_t>(TelemetryEventType::task_started)};
    std::atomic<bool> has_last_event_{false};
    ProgressSnapshot snapshot_{};
    PerformanceSummary summary_{};
};

} // namespace asyncdownload::telemetry
