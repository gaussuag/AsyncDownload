#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace asyncdownload::performance {

// Performance metrics maintenance guide:
// 1. Add new direct metrics to the appropriate field group in this header, then
//    wire RuntimePerformanceMetrics, SummaryDirectPerformanceMetrics, and
//    copy_runtime_to_summary().
// 2. Add new derived-only summary metrics to
//    include/asyncdownload/types.hpp::PerformanceSummary and compute them in
//    src/download/download_engine.cpp::build_performance_summary().
// 3. Add or update runtime collection at the real producer/consumer hot path,
//    typically in src/download/download_engine.cpp or
//    src/persistence/persistence_thread.cpp.
// 4. Keep external summary keys in sync:
//    - src/main.cpp::write_summary()
//    - scripts/performance/performance_common.py::SUMMARY_SPECS
// 5. Update tests when the metric affects exported summary shape or runtime
//    sampling/counters, especially tests/persistence/persistence_thread_test.cpp.
// 6. If the metric changes benchmark interpretation or maintainability rules,
//    update docs/performance/performance_optimization_history_zh.md and the
//    current performance docs as needed.
// 7. Preserve existing summary key names unless there is an explicit benchmark
//    schema migration plan.
//
// Quick rule of thumb:
// - direct metric: define here, copy here, export in main.py/main.cpp
// - derived metric: define in PerformanceSummary, compute in download_engine.cpp
// - sampled metric: add runtime raw counters here, summarize in download_engine.cpp,
//   then export through main.cpp and performance_common.py

template <typename IntField>
struct TimeToFirstMetrics {
    IntField time_to_first_byte_ms{-1};
    IntField time_to_first_persist_ms{-1};
};

template <typename IntField>
struct ResumeMetrics {
    IntField resume_reused_bytes{0};
};

template <typename DoubleField>
struct PeakRateMetrics {
    DoubleField peak_network_bytes_per_second{0.0};
    DoubleField peak_disk_bytes_per_second{0.0};
};

template <typename SizeField, typename IntField>
struct ResourcePeakMetrics {
    SizeField max_memory_bytes{0};
    IntField max_inflight_bytes{0};
    SizeField max_queued_packets{0};
    IntField max_queued_bytes{0};
    SizeField max_active_requests{0};
};

template <typename SizeField>
struct PauseMetrics {
    SizeField memory_pause_count{0};
    SizeField queue_full_pause_count{0};
    SizeField queue_full_pause_capacity_reached_count{0};
    SizeField queue_full_pause_try_enqueue_failure_count{0};
    SizeField window_boundary_pause_count{0};
    SizeField gap_pause_count{0};
    SizeField max_queue_paused_handles{0};
    SizeField max_memory_paused_handles{0};
    SizeField queue_full_resume_count{0};
    SizeField memory_resume_count{0};
    SizeField queue_resume_blocked_by_memory_count{0};
    SizeField queue_pause_overlap_memory_count{0};
    SizeField queue_full_pause_start_queued_packets_total{0};
    SizeField memory_pause_start_queued_packets_total{0};
};

template <typename SizeField, typename IntField>
struct TransferCountMetrics {
    SizeField windows_total{0};
    SizeField ranges_stolen{0};
    SizeField write_callback_calls{0};
    SizeField packets_enqueued_total{0};
    SizeField max_packet_size_bytes{0};
    SizeField flush_count{0};
    SizeField metadata_save_count{0};
    SizeField file_write_calls_total{0};
    SizeField staged_write_flush_count{0};
    SizeField direct_append_packets_total{0};
    SizeField out_of_order_insert_packets_total{0};
    SizeField drained_ordered_packets_total{0};
    SizeField out_of_order_queue_peak_packets{0};
    SizeField crc_sample_blocks_total{0};
    IntField queue_full_pause_start_queued_bytes_total{0};
    IntField memory_pause_start_queued_bytes_total{0};
};

template <typename IntField>
struct DurationCountMetrics {
    IntField flush_time_ms_total{0};
    IntField metadata_save_time_ms_total{0};
    IntField staged_write_bytes_total{0};
    IntField out_of_order_queue_peak_bytes{0};
    IntField crc_sample_bytes_total{0};
};

template <typename CountField, typename TimeField>
struct LatencyRuntimeSampleMetrics {
    CountField sample_count{0};
    TimeField total_time_ns{0};
    TimeField max_time_ns{0};
};

template <typename CountField, typename FloatField>
struct LatencySummarySampleMetrics {
    CountField sample_count{0};
    FloatField avg_us{0.0};
    FloatField max_us{0.0};
};

template <typename CountField, typename TimeField>
struct RuntimeLatencyMetrics {
    LatencyRuntimeSampleMetrics<CountField, TimeField> handle_data_packet{};
    LatencyRuntimeSampleMetrics<CountField, TimeField> append_bytes{};
    LatencyRuntimeSampleMetrics<CountField, TimeField> file_write{};
    LatencyRuntimeSampleMetrics<CountField, TimeField> metadata_snapshot{};
    LatencyRuntimeSampleMetrics<CountField, TimeField> crc_sample_read{};
    LatencyRuntimeSampleMetrics<CountField, TimeField> flush_pending_write{};
};

template <typename CountField, typename TimeField>
struct RuntimePauseDurationMetrics {
    LatencyRuntimeSampleMetrics<CountField, TimeField> queue_full_pause_duration{};
    LatencyRuntimeSampleMetrics<CountField, TimeField> memory_pause_duration{};
    LatencyRuntimeSampleMetrics<CountField, TimeField> queue_resume_blocked_by_memory_duration{};
};

struct RuntimePerformanceMetrics
    : TimeToFirstMetrics<std::atomic<std::int64_t>>,
      ResumeMetrics<std::int64_t>,
      PeakRateMetrics<double>,
      ResourcePeakMetrics<std::atomic<std::size_t>, std::atomic<std::int64_t>>,
      PauseMetrics<std::atomic<std::size_t>>,
      TransferCountMetrics<std::atomic<std::size_t>, std::atomic<std::int64_t>>,
      DurationCountMetrics<std::atomic<std::int64_t>> {
    RuntimeLatencyMetrics<std::atomic<std::size_t>, std::atomic<std::int64_t>> latency{};
    RuntimePauseDurationMetrics<std::atomic<std::size_t>, std::atomic<std::int64_t>>
        pause_duration{};
};

struct SummaryDirectPerformanceMetrics
    : TimeToFirstMetrics<std::int64_t>,
      ResumeMetrics<std::int64_t>,
      PeakRateMetrics<double>,
      ResourcePeakMetrics<std::size_t, std::int64_t>,
      PauseMetrics<std::size_t>,
      TransferCountMetrics<std::size_t, std::int64_t>,
      DurationCountMetrics<std::int64_t> {};

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
    summary.time_to_first_byte_ms = load_value(runtime.time_to_first_byte_ms);
    summary.time_to_first_persist_ms = load_value(runtime.time_to_first_persist_ms);
    summary.resume_reused_bytes = load_value(runtime.resume_reused_bytes);
    summary.peak_network_bytes_per_second = load_value(runtime.peak_network_bytes_per_second);
    summary.peak_disk_bytes_per_second = load_value(runtime.peak_disk_bytes_per_second);
    summary.max_memory_bytes = load_value(runtime.max_memory_bytes);
    summary.max_inflight_bytes = load_value(runtime.max_inflight_bytes);
    summary.max_queued_packets = load_value(runtime.max_queued_packets);
    summary.max_queued_bytes = load_value(runtime.max_queued_bytes);
    summary.max_active_requests = load_value(runtime.max_active_requests);
    summary.memory_pause_count = load_value(runtime.memory_pause_count);
    summary.queue_full_pause_count = load_value(runtime.queue_full_pause_count);
    summary.queue_full_pause_capacity_reached_count =
        load_value(runtime.queue_full_pause_capacity_reached_count);
    summary.queue_full_pause_try_enqueue_failure_count =
        load_value(runtime.queue_full_pause_try_enqueue_failure_count);
    summary.window_boundary_pause_count = load_value(runtime.window_boundary_pause_count);
    summary.gap_pause_count = load_value(runtime.gap_pause_count);
    summary.max_queue_paused_handles = load_value(runtime.max_queue_paused_handles);
    summary.max_memory_paused_handles = load_value(runtime.max_memory_paused_handles);
    summary.queue_full_resume_count = load_value(runtime.queue_full_resume_count);
    summary.memory_resume_count = load_value(runtime.memory_resume_count);
    summary.queue_resume_blocked_by_memory_count =
        load_value(runtime.queue_resume_blocked_by_memory_count);
    summary.queue_pause_overlap_memory_count =
        load_value(runtime.queue_pause_overlap_memory_count);
    summary.queue_full_pause_start_queued_packets_total =
        load_value(runtime.queue_full_pause_start_queued_packets_total);
    summary.memory_pause_start_queued_packets_total =
        load_value(runtime.memory_pause_start_queued_packets_total);
    summary.windows_total = load_value(runtime.windows_total);
    summary.ranges_stolen = load_value(runtime.ranges_stolen);
    summary.write_callback_calls = load_value(runtime.write_callback_calls);
    summary.packets_enqueued_total = load_value(runtime.packets_enqueued_total);
    summary.max_packet_size_bytes = load_value(runtime.max_packet_size_bytes);
    summary.flush_count = load_value(runtime.flush_count);
    summary.flush_time_ms_total = load_value(runtime.flush_time_ms_total);
    summary.metadata_save_count = load_value(runtime.metadata_save_count);
    summary.metadata_save_time_ms_total = load_value(runtime.metadata_save_time_ms_total);
    summary.file_write_calls_total = load_value(runtime.file_write_calls_total);
    summary.staged_write_flush_count = load_value(runtime.staged_write_flush_count);
    summary.staged_write_bytes_total = load_value(runtime.staged_write_bytes_total);
    summary.direct_append_packets_total = load_value(runtime.direct_append_packets_total);
    summary.out_of_order_insert_packets_total = load_value(runtime.out_of_order_insert_packets_total);
    summary.drained_ordered_packets_total = load_value(runtime.drained_ordered_packets_total);
    summary.out_of_order_queue_peak_packets = load_value(runtime.out_of_order_queue_peak_packets);
    summary.out_of_order_queue_peak_bytes = load_value(runtime.out_of_order_queue_peak_bytes);
    summary.crc_sample_blocks_total = load_value(runtime.crc_sample_blocks_total);
    summary.crc_sample_bytes_total = load_value(runtime.crc_sample_bytes_total);
    summary.queue_full_pause_start_queued_bytes_total =
        load_value(runtime.queue_full_pause_start_queued_bytes_total);
    summary.memory_pause_start_queued_bytes_total =
        load_value(runtime.memory_pause_start_queued_bytes_total);
}

template <typename CountField, typename FloatField, typename RuntimeCountField, typename RuntimeTimeField>
inline void summarize_latency_sample(
    LatencySummarySampleMetrics<CountField, FloatField>& summary,
    const LatencyRuntimeSampleMetrics<RuntimeCountField, RuntimeTimeField>& runtime) noexcept {
    summary.sample_count = load_value(runtime.sample_count);
    if (summary.sample_count == 0) {
        summary.avg_us = 0.0;
        summary.max_us = 0.0;
        return;
    }
    const auto total_ns = load_value(runtime.total_time_ns);
    const auto max_ns = load_value(runtime.max_time_ns);
    summary.avg_us = static_cast<double>(total_ns) /
        static_cast<double>(summary.sample_count) / 1000.0;
    summary.max_us = static_cast<double>(max_ns > 0 ? max_ns : 0) / 1000.0;
}

} // namespace asyncdownload::performance
