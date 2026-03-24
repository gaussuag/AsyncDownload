#pragma once

#include "asyncdownload/telemetry/telemetry_collector.hpp"
#include "asyncdownload/telemetry/telemetry_event.hpp"
#include "asyncdownload/telemetry/telemetry_sink.hpp"
#include "asyncdownload/types.hpp"

#include <cstdint>

namespace asyncdownload::telemetry {

struct TelemetrySessionTestAccess;

class TelemetrySession {
public:
    TelemetrySession();
    ~TelemetrySession();

    TelemetrySession(const TelemetrySession&) = delete;
    TelemetrySession& operator=(const TelemetrySession&) = delete;

    void record_task_started() noexcept;
    void record_first_byte_received() noexcept;
    void record_download_delta(std::uint64_t bytes) noexcept;
    void record_persist_delta(std::uint64_t bytes) noexcept;
    void record_pause(TelemetryPauseReason reason, bool is_queue_full) noexcept;
    void record_memory_sample(std::uint64_t memory_bytes) noexcept;
    void record_task_completed() noexcept;

    [[nodiscard]] ProgressSnapshot current_snapshot() const noexcept;
    [[nodiscard]] PerformanceSummary final_summary() const noexcept;

private:
    friend struct TelemetrySessionTestAccess;

    void emit(TelemetryEventType type, const TelemetryPayload& payload) noexcept;

    TelemetrySink sink_{};
    TelemetryCollector collector_;
};

} // namespace asyncdownload::telemetry
