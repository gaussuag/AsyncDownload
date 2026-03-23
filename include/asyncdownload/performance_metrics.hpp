#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace asyncdownload::performance {

template <typename SizeField, typename IntField>
struct ResourcePeakMetrics {
    SizeField max_memory_bytes{0};
    IntField max_inflight_bytes{0};
};

template <typename SizeField>
struct PauseMetrics {
    SizeField total_pause_count{0};
    SizeField queue_full_pause_count{0};
};

template <typename SizeField>
struct TransferCountMetrics {
    SizeField packets_enqueued_total{0};
    SizeField max_packet_size_bytes{0};
};

template <typename FloatField, typename IntField>
struct DerivedPerformanceMetrics {
    FloatField average_network_bytes_per_second{0.0};
    FloatField average_disk_bytes_per_second{0.0};
    IntField time_to_first_byte_ms{0};
    FloatField average_packet_size_bytes{0.0};
};

struct RuntimePerformanceMetrics
    : ResourcePeakMetrics<std::atomic<std::size_t>, std::atomic<std::int64_t>>,
      PauseMetrics<std::atomic<std::size_t>>,
      TransferCountMetrics<std::atomic<std::size_t>> {};

struct SummaryDirectPerformanceMetrics
    : ResourcePeakMetrics<std::size_t, std::int64_t>,
      PauseMetrics<std::size_t>,
      TransferCountMetrics<std::size_t> {};

struct SummaryPerformanceMetrics
    : SummaryDirectPerformanceMetrics,
      DerivedPerformanceMetrics<double, std::int64_t> {};

template <typename T>
[[nodiscard]] T load_value(const T& value) noexcept {
    return value;
}

template <typename T>
[[nodiscard]] T load_value(const std::atomic<T>& value) noexcept {
    return value.load(std::memory_order_relaxed);
}

inline void copy_runtime_to_summary(SummaryDirectPerformanceMetrics& summary,
                                    const RuntimePerformanceMetrics& runtime) noexcept {
    summary.max_memory_bytes = load_value(runtime.max_memory_bytes);
    summary.max_inflight_bytes = load_value(runtime.max_inflight_bytes);
    summary.total_pause_count = load_value(runtime.total_pause_count);
    summary.queue_full_pause_count = load_value(runtime.queue_full_pause_count);
    summary.packets_enqueued_total = load_value(runtime.packets_enqueued_total);
    summary.max_packet_size_bytes = load_value(runtime.max_packet_size_bytes);
}

} // namespace asyncdownload::performance
