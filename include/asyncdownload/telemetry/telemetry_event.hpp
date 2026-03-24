#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace asyncdownload::telemetry {

using TelemetryClock = std::chrono::steady_clock;

enum class TelemetryEventType : std::uint16_t {
    task_started = 0,
    first_byte_received = 1,
    download_delta = 2,
    persist_delta = 3,
    queue_paused = 4,
    memory_sample = 5,
    task_completed = 6
};

enum class TelemetryPauseReason : std::uint8_t {
    none = 0,
    queue_full = 1,
    memory_pressure = 2,
    gap = 3
};

enum class TelemetryCompletionStatus : std::uint8_t {
    success = 0,
    cancelled = 1,
    failed = 2
};

struct TaskStartedPayload {
    std::uint8_t reserved{0};
};

struct FirstByteReceivedPayload {
    std::uint8_t reserved{0};
};

struct DownloadDeltaPayload {
    std::uint64_t bytes_count{0};
};

struct PersistDeltaPayload {
    std::uint64_t bytes_count{0};
};

struct QueuePausedPayload {
    TelemetryPauseReason reason{TelemetryPauseReason::none};
    bool is_queue_full{false};
};

struct MemorySamplePayload {
    std::uint64_t memory_bytes{0};
};

struct TaskCompletedPayload {
    TelemetryCompletionStatus status{TelemetryCompletionStatus::success};
};

union TelemetryPayloadStorage {
    TaskStartedPayload task_started;
    FirstByteReceivedPayload first_byte_received;
    DownloadDeltaPayload download_delta;
    PersistDeltaPayload persist_delta;
    QueuePausedPayload queue_paused;
    MemorySamplePayload memory_sample;
    TaskCompletedPayload task_completed;

    constexpr TelemetryPayloadStorage() noexcept
        : task_started{} {}
};

struct TelemetryPayload {
    TelemetryPayloadStorage storage{};

    [[nodiscard]] static constexpr TelemetryPayload task_started_payload() noexcept {
        return TelemetryPayload{};
    }

    [[nodiscard]] static constexpr TelemetryPayload first_byte_received_payload() noexcept {
        return TelemetryPayload{};
    }

    [[nodiscard]] static constexpr TelemetryPayload download_delta_payload(
        const std::uint64_t bytes_count) noexcept {
        TelemetryPayload payload;
        payload.storage.download_delta = DownloadDeltaPayload{bytes_count};
        return payload;
    }

    [[nodiscard]] static constexpr TelemetryPayload persist_delta_payload(
        const std::uint64_t bytes_count) noexcept {
        TelemetryPayload payload;
        payload.storage.persist_delta = PersistDeltaPayload{bytes_count};
        return payload;
    }

    [[nodiscard]] static constexpr TelemetryPayload queue_paused_payload(
        const TelemetryPauseReason reason,
        const bool is_queue_full) noexcept {
        TelemetryPayload payload;
        payload.storage.queue_paused = QueuePausedPayload{reason, is_queue_full};
        return payload;
    }

    [[nodiscard]] static constexpr TelemetryPayload memory_sample_payload(
        const std::uint64_t memory_bytes) noexcept {
        TelemetryPayload payload;
        payload.storage.memory_sample = MemorySamplePayload{memory_bytes};
        return payload;
    }

    [[nodiscard]] static constexpr TelemetryPayload task_completed_payload(
        const TelemetryCompletionStatus status) noexcept {
        TelemetryPayload payload;
        payload.storage.task_completed = TaskCompletedPayload{status};
        return payload;
    }

    [[nodiscard]] constexpr const TaskStartedPayload& as_task_started() const noexcept {
        return storage.task_started;
    }

    [[nodiscard]] constexpr const FirstByteReceivedPayload& as_first_byte_received() const noexcept {
        return storage.first_byte_received;
    }

    [[nodiscard]] constexpr const DownloadDeltaPayload& as_download_delta() const noexcept {
        return storage.download_delta;
    }

    [[nodiscard]] constexpr const PersistDeltaPayload& as_persist_delta() const noexcept {
        return storage.persist_delta;
    }

    [[nodiscard]] constexpr const QueuePausedPayload& as_queue_paused() const noexcept {
        return storage.queue_paused;
    }

    [[nodiscard]] constexpr const MemorySamplePayload& as_memory_sample() const noexcept {
        return storage.memory_sample;
    }

    [[nodiscard]] constexpr const TaskCompletedPayload& as_task_completed() const noexcept {
        return storage.task_completed;
    }
};

struct TelemetryEvent {
    TelemetryEventType type{TelemetryEventType::task_started};
    std::uint64_t timestamp_ns{0};
    TelemetryPayload payload{};
};

[[nodiscard]] inline std::uint64_t telemetry_timestamp_ns(
    const TelemetryClock::time_point timestamp) noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(timestamp.time_since_epoch()).count());
}

[[nodiscard]] inline std::uint64_t telemetry_now_ns() noexcept {
    return telemetry_timestamp_ns(TelemetryClock::now());
}

[[nodiscard]] inline TelemetryEvent make_telemetry_event(
    const TelemetryEventType type,
    const TelemetryPayload& payload,
    const TelemetryClock::time_point timestamp = TelemetryClock::now()) noexcept {
    return TelemetryEvent{type, telemetry_timestamp_ns(timestamp), payload};
}

static_assert(std::is_trivially_copyable_v<TelemetryPayloadStorage>);
static_assert(std::is_trivially_copyable_v<TelemetryPayload>);
static_assert(std::is_trivially_copyable_v<TelemetryEvent>);
static_assert(sizeof(TelemetryPayload) <= sizeof(std::uint64_t));
static_assert(sizeof(TelemetryEvent) <= 24);

} // namespace asyncdownload::telemetry
