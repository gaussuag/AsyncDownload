#pragma once

#include "asyncdownload/telemetry/telemetry_session.hpp"
#include "asyncdownload/types.hpp"
#include "download/download_policy.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace asyncdownload::core {

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
    explicit SessionState(
        download::EffectiveDownloadPolicy policy) noexcept
        : effective_policy(std::move(policy)),
          total_size(effective_policy.remote_facts().total_size) {}

    // SessionState 是整个下载任务的共享上下文，网络层和持久化层都围绕它协作。
    SessionPaths paths;
    std::string url;
    std::string etag;
    std::string last_modified;
    download::EffectiveDownloadPolicy effective_policy;
    std::int64_t total_size = 0;
    bool resumed = false;
    std::int64_t recovery_initial_trusted_bytes = 0;
    std::atomic<std::int64_t> persisted_bytes{0};
    // vdl_offset 表示当前已经 flush 并进入恢复元数据的最长连续安全前沿。
    std::atomic<std::int64_t> vdl_offset{0};
    std::atomic<bool> stop_requested{false};
    std::chrono::steady_clock::time_point task_started_at{};
    ProgressCallback progress_callback{};
    asyncdownload::telemetry::TelemetrySession telemetry_session_{};
};

} // namespace asyncdownload::core
