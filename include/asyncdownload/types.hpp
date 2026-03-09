#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>

namespace asyncdownload {

struct DownloadOptions {
    // 最大并发连接数。仅在服务端支持 Range 时才会真正发挥多连接效果。
    std::size_t max_connections = 4;
    // 网络层到持久化线程之间的队列容量，单位是 packet 个数而不是字节数。
    std::size_t queue_capacity_packets = 4096;
    // 单个 HTTP 请求覆盖的窗口大小。逻辑 range 可以比它大，但会被拆成多个 window。
    std::size_t scheduler_window_bytes = 4 * 1024 * 1024;
    // 全局内存积压高水位，超过后开始主动暂停最快的一批连接。
    std::size_t backpressure_high_bytes = 256 * 1024 * 1024;
    // 低水位，只有回落到这里以下才恢复被内存背压暂停的连接。
    std::size_t backpressure_low_bytes = 128 * 1024 * 1024;
    // 位图颗粒度，也是调度切分和 steal 对齐的基本单位。
    std::size_t block_size = 64 * 1024;
    // 持久化写盘的目标对齐单位。
    std::size_t io_alignment = 4 * 1024;
    // 单个 range 允许的最大乱序缺口，超过后会触发 gap pause。
    std::size_t max_gap_bytes = 32 * 1024 * 1024;
    // 累积写盘达到该阈值后触发一次异步 flush + metadata 保存。
    std::size_t flush_threshold_bytes = 16 * 1024 * 1024;
    // 即使没有达到 flush_threshold_bytes，超过该时间间隔也会触发周期性 flush。
    std::chrono::milliseconds flush_interval{2000};
    // 下载完成 rename 正式文件时，是否允许覆盖已有目标文件。
    bool overwrite_existing = true;
};

struct ProgressSnapshot {
    // 远端文件总大小，若 probe 成功则在整个任务期间保持不变。
    std::int64_t total_bytes = 0;
    // 已从网络层接收并成功入队的数据总量。
    std::int64_t downloaded_bytes = 0;
    // 已经物理写入临时文件的数据总量。
    std::int64_t persisted_bytes = 0;
    // 从文件头开始连续安全落盘的长度，恢复时 VDL 之前的数据可直接信任。
    std::int64_t vdl_offset = 0;
    // 已下载但尚未物理落盘的字节数，可以直观看到网络与磁盘是否失衡。
    std::int64_t inflight_bytes = 0;
    std::size_t queued_packets = 0;
    std::size_t active_ranges = 0;
    std::size_t finished_ranges = 0;
    std::size_t active_requests = 0;
    std::size_t paused_ranges = 0;
    std::size_t gap_paused_ranges = 0;
    std::size_t memory_paused_ranges = 0;
    std::size_t memory_bytes = 0;
    // 基于相邻两次进度采样的平均网络接收速度。
    double network_bytes_per_second = 0.0;
    // 基于相邻两次进度采样的平均磁盘写入速度。
    double disk_bytes_per_second = 0.0;
    bool resumed = false;
};

using ProgressCallback = std::function<void(const ProgressSnapshot&)>;

struct PerformanceSummary {
    // 整个任务总耗时，包含 probe、恢复判定、下载和最终收尾。
    std::int64_t total_duration_ms = 0;
    // 首次收到网络数据包的时间点，单位是相对任务启动的毫秒数。
    std::int64_t time_to_first_byte_ms = -1;
    // 首次真正写盘的时间点，单位是相对任务启动的毫秒数。
    std::int64_t time_to_first_persist_ms = -1;
    // 基于任务总耗时计算的平均网络接收速度。
    double average_network_bytes_per_second = 0.0;
    // 基于任务总耗时计算的平均磁盘写入速度。
    double average_disk_bytes_per_second = 0.0;
    // 下载过程中的峰值网络接收速度。
    double peak_network_bytes_per_second = 0.0;
    // 下载过程中的峰值磁盘写入速度。
    double peak_disk_bytes_per_second = 0.0;
    // 恢复启动时直接复用的安全字节数。
    std::int64_t resume_reused_bytes = 0;
    // 下载过程中观测到的最大内存占用。
    std::size_t max_memory_bytes = 0;
    // downloaded 与 persisted 之间出现过的最大积压字节数。
    std::int64_t max_inflight_bytes = 0;
    // 队列中的 packet 数量峰值。
    std::size_t max_queued_packets = 0;
    // 并发活跃 HTTP 请求峰值。
    std::size_t max_active_requests = 0;
    // 真正因超过内存高水位触发的暂停次数。
    std::size_t memory_pause_count = 0;
    // 因队列 try_enqueue 失败触发的暂停次数。
    std::size_t queue_full_pause_count = 0;
    // 因当前 window 已经没有剩余额度而触发的暂停次数。
    std::size_t window_boundary_pause_count = 0;
    // 因 gap 熔断暂停 handle 的次数。
    std::size_t gap_pause_count = 0;
    // 实际发起的 window 请求总数。
    std::size_t windows_total = 0;
    // 任务期间总共存在过的逻辑 range 数量。
    std::size_t ranges_total = 0;
    // 运行期通过 stealing 新增出来的 range 数量。
    std::size_t ranges_stolen = 0;
    // libcurl 写回调被调用的次数。
    std::size_t write_callback_calls = 0;
    // 成功入队到持久化线程的数据 packet 数量。
    std::size_t packets_enqueued_total = 0;
    // 数据 packet 的平均负载大小。
    double average_packet_size_bytes = 0.0;
    // 观测到的最大数据 packet 负载大小。
    std::size_t max_packet_size_bytes = 0;
    // 实际执行的 flush 次数。
    std::size_t flush_count = 0;
    // flush 累积耗时。
    std::int64_t flush_time_ms_total = 0;
    // metadata 保存次数。
    std::size_t metadata_save_count = 0;
    // metadata 保存累积耗时。
    std::int64_t metadata_save_time_ms_total = 0;
};

struct DownloadRequest {
    // 目标资源 URL。
    std::string url;
    // 最终正式产物路径；运行中会派生出同目录的 .part 和 .config.json。
    std::filesystem::path output_path;
    DownloadOptions options{};
    // 进度回调由下载引擎线程周期性触发，不要求调用方自己做同步。
    ProgressCallback progress_callback{};
};

struct DownloadResult {
    // 整个任务的最终错误码；success 时为空。
    std::error_code error;
    std::int64_t total_bytes = 0;
    std::int64_t downloaded_bytes = 0;
    std::int64_t persisted_bytes = 0;
    // 已经由 Persistence 确认为完成态的 range 数量。
    std::size_t completed_ranges = 0;
    bool resumed = false;
    // 无论成功还是失败，调用方都能看到本次任务使用过的恢复文件路径。
    std::filesystem::path temporary_path;
    std::filesystem::path metadata_path;
    // 下载完成后用于性能分析的任务级汇总。
    PerformanceSummary performance{};

    [[nodiscard]] bool ok() const noexcept {
        return !error;
    }
};

} // namespace asyncdownload
