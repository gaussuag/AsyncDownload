#pragma once

#include "asyncdownload/types.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace asyncdownload::core {

enum class PacketKind : std::uint8_t {
    // 真正的下载数据包。
    data = 0,
    // Orchestrator 告诉 Persistence：某个 range 的网络阶段已经结束。
    range_complete = 1,
    // 结束 Persistence 线程的控制消息。
    shutdown = 2
};

enum class RangeStatus : std::uint8_t {
    // 当前没有请求在处理这个 range，等待被调度。
    empty = 0,
    // 当前有请求正在为这个 range 拉取数据。
    downloading = 1,
    // 网络和持久化都已经确认这个 range 完成。
    finished = 2,
    // 因 gap 或 memory 背压而暂停。
    paused = 3,
    // 该 range 在本轮下载中出现错误。
    failed = 4
};

struct BlockCrcSample {
    // 采样块在文件中的起始偏移。
    std::int64_t offset = 0;
    std::uint32_t crc32 = 0;
    // 采样长度通常等于 block_size，只有最后一个块可能更短。
    std::size_t length = 0;
};

struct DataPacket {
    // 同一个结构同时承载数据包和控制包。
    PacketKind kind = PacketKind::data;
    std::size_t range_id = 0;
    std::int64_t offset = 0;
    std::vector<std::uint8_t> payload;
    // 用于全局内存会计，除了 payload 还会加上固定结构和 map 节点开销。
    std::size_t accounted_bytes = 0;

    [[nodiscard]] std::size_t size() const noexcept {
        return payload.size();
    }
};

struct TailBuffer {
    // Persistence 线程只会把完整对齐块直接写盘，不满 4KB 的尾巴先暂存在这里，
    // 等后续数据补齐或 range 结束时再一起刷到磁盘。
    std::array<std::uint8_t, 4096> data{};
    std::size_t length = 0;
    std::int64_t offset = 0;
};

struct RangeContext {
    RangeContext(const std::size_t id,
                 const std::int64_t start,
                 const std::int64_t end) noexcept
        : range_id(id),
          start_offset(start),
          end_offset(end),
          current_offset(start),
          persisted_offset(start) {}

    // range_id 在整个任务内唯一，用来把网络包、控制包、metadata 快照关联到同一 range。
    const std::size_t range_id;
    // 逻辑起点保持不变，主要用于恢复、位图推进和调试。
    const std::int64_t start_offset;
    // end_offset 在安全 steal 时可以被缩短，因此用原子维护。
    std::atomic<std::int64_t> end_offset;
    // current_offset 表示调度器已经租出去的前沿，而不是已经写盘的前沿。
    std::atomic<std::int64_t> current_offset;
    TailBuffer tail_buffer;
    // status 是给调度、进度展示和恢复快照看的粗粒度状态。
    std::atomic<std::uint8_t> status{static_cast<std::uint8_t>(RangeStatus::empty)};
    std::atomic<bool> pause_for_gap{false};
    std::atomic<bool> pause_for_memory{false};
    // completion_notified 防止同一个 range 被重复发送 range_complete 控制消息。
    std::atomic<bool> completion_notified{false};
    // marked_finished 代表 Persistence 已经确认该 range 的最终收尾完成。
    std::atomic<bool> marked_finished{false};
    // persisted_offset 只由 Persistence 线程推进，表示这个 range 已经按顺序
    // 物理写入到磁盘的逻辑前沿。
    std::int64_t persisted_offset;
    // 网络层可以乱序到达，但真正写盘必须严格按 offset 串行，因此先按 offset
    // 暂存在 map 里，等缺口补齐后再链式落盘。
    std::map<std::int64_t, DataPacket> out_of_order_queue;
};

struct RangeStateSnapshot {
    // 这是 metadata 里记录的“恢复最小状态集”，用于重启后重建调度现场。
    std::size_t range_id = 0;
    std::int64_t start_offset = 0;
    std::int64_t end_offset = 0;
    std::int64_t current_offset = 0;
    std::int64_t persisted_offset = 0;
    std::uint8_t status = 0;
};

struct MetadataState {
    // 资源身份信息，用于判断恢复文件是否还能接着当前请求使用。
    std::string url;
    std::filesystem::path output_path;
    std::filesystem::path temporary_path;
    std::int64_t total_size = 0;
    std::int64_t vdl_offset = 0;
    bool accept_ranges = false;
    bool resumed = false;
    std::string etag;
    std::string last_modified;
    std::size_t block_size = 0;
    std::size_t io_alignment = 0;
    // 位图、range 快照和 CRC 样本共同描述“当前哪些数据可信、哪些要重下”。
    std::vector<std::uint8_t> bitmap_states;
    std::vector<RangeStateSnapshot> ranges;
    std::vector<BlockCrcSample> crc_samples;
};

struct RemoteProbeResult {
    // probe 自己的错误与 HTTP 响应码拆开保存，方便区分“网络失败”和“服务端响应无效”。
    std::error_code error;
    long response_code = 0;
    std::int64_t total_size = 0;
    bool accept_ranges = false;
    std::string etag;
    std::string last_modified;
};

struct SessionPaths {
    // output_path 是最终产物路径；其余两个是恢复阶段使用的中间文件。
    std::filesystem::path output_path;
    std::filesystem::path temporary_path;
    std::filesystem::path metadata_path;
};

struct SessionState {
    // SessionState 是整个下载任务的共享上下文，网络层和持久化层都围绕它协作。
    SessionPaths paths;
    std::string url;
    std::string etag;
    std::string last_modified;
    DownloadOptions options;
    std::int64_t total_size = 0;
    bool accept_ranges = false;
    bool resumed = false;
    // downloaded_bytes 统计已从网络接收的数据，persisted_bytes 统计已物理落盘的数据。
    std::atomic<std::int64_t> downloaded_bytes{0};
    std::atomic<std::int64_t> persisted_bytes{0};
    // vdl_offset 表示当前已经 flush 并进入恢复元数据的最长连续安全前沿。
    std::atomic<std::int64_t> vdl_offset{0};
    // queued_packets 用于进度和背压观测，表示还有多少包在持久化链路里等待处理。
    std::atomic<std::size_t> queued_packets{0};
    // queued_bytes 表示 network -> persistence 队列里按 accounted_bytes 统计的积压体量。
    std::atomic<std::int64_t> queued_bytes{0};
    std::atomic<std::int64_t> queued_payload_bytes{0};
    std::atomic<std::int64_t> active_buffered_accounted_bytes{0};
    std::atomic<std::size_t> queue_paused_handles{0};
    std::atomic<std::size_t> memory_paused_handles{0};
    std::atomic<bool> cancel_requested{false};
    std::atomic<bool> stop_requested{false};
    ProgressCallback progress_callback{};
    // 整个任务的性能测量都以 task_started_at 为相对时间零点。
    std::chrono::steady_clock::time_point task_started_at{};
    // 进度回调由 Orchestrator 线程统一采样，避免在网络回调或持久化线程里直接
    // 计算速度，减少跨线程统计带来的噪声。
    std::chrono::steady_clock::time_point last_progress_sample_at{};
    std::int64_t last_progress_downloaded_bytes = 0;
    std::int64_t last_progress_persisted_bytes = 0;
    double last_network_bytes_per_second = 0.0;
    double last_disk_bytes_per_second = 0.0;
    bool memory_watermark_episode_active = false;
    std::size_t memory_watermark_episode_start_active_requests = 0;
    std::int64_t memory_watermark_episode_start_active_window_bytes = 0;
    std::int64_t memory_watermark_episode_start_queued_payload_bytes = 0;
    std::int64_t memory_watermark_episode_start_inflight_bytes = 0;
    std::int64_t memory_watermark_episode_start_memory_bytes = 0;
    performance::RuntimePerformanceMetrics performance_metrics{};
};

} // namespace asyncdownload::core
