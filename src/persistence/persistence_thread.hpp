#pragma once

#include "core/block_bitmap.hpp"
#include "core/models.hpp"
#include "download/download_policy.hpp"
#include "flow/packet_flow.hpp"
#include "persistence/range_write_state.hpp"
#include "range/range_lifecycle.hpp"
#include "recovery/recovery_types.hpp"

#include <thread-pool/BS_thread_pool.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace asyncdownload::recovery {
class RecoveryCheckpoint;
}

namespace asyncdownload::persistence {

using RangeRegistrationTicket = std::uint64_t;
using RangeGeometryCommand = std::variant<
    range::RegisterRangeEffect,
    range::ResizeRangeEffect>;

struct RangeRegistrationSubmitResult {
    RangeRegistrationTicket ticket = 0;
    std::error_code error;
};

struct RangeRegistrationAck {
    RangeRegistrationTicket ticket = 0;
    range::RangeId range{};
    std::uint64_t geometry_revision = 0;
    std::error_code error;
};

struct RangeRegistrationPollResult {
    std::optional<RangeRegistrationAck> ack;
    std::error_code error;
};

class PersistenceThread {
public:
    // PersistenceThread 独占管理以下职责：
    // 1. range 的真实落盘前沿
    // 2. 乱序 packet 的重排
    // 3. 4KB 对齐写入与 tail buffer
    // 4. 位图推进
    // 5. flush / metadata / VDL 更新
    PersistenceThread(core::SessionState& session,
                      download::PersistencePolicy policy,
                      flow::PacketConsumer& packet_consumer,
                      core::AtomicBlockBitmap& bitmap,
                      recovery::RecoveryCheckpoint& checkpoint,
                      BS::thread_pool<>& workers,
                      std::size_t initial_range_count = 0);
    ~PersistenceThread();

    PersistenceThread(const PersistenceThread&) = delete;
    PersistenceThread& operator=(const PersistenceThread&) = delete;

    [[nodiscard]] RangeRegistrationSubmitResult
    submit_range_geometry(RangeGeometryCommand command) noexcept;
    [[nodiscard]] RangeRegistrationPollResult
    poll_range_geometry_ack() noexcept;
    // 启动后台持久化线程。
    void start();
    // 通过 enqueue shutdown 控制包请求线程收尾退出。
    void stop();
    // 等待后台线程结束。
    void join();

    // 返回后台线程记录的首个错误。
    [[nodiscard]] std::error_code error() const noexcept;
private:
    // 主循环：消费 packet、轮询 flush 结果、按阈值发起新的 flush。
    void process_loop();
    void process_range_geometry() noexcept;
    // 按 packet.kind 分流到 data / range_complete / shutdown 三类处理路径。
    void handle_packet(flow::PacketLease packet);
    void handle_data_packet(flow::PacketLease packet);
    void handle_range_complete(const flow::ControlPacket& control);
    [[nodiscard]] RangeWriteState*
    lookup_range(std::size_t range_id) const;
    // 把一段逻辑字节按对齐规则写入磁盘，必要时借助 tail buffer 补齐。
    [[nodiscard]] std::error_code append_bytes(RangeWriteState& range,
                                               std::int64_t offset,
                                               std::span<const std::uint8_t> bytes,
                                               bool sample_timing);
    [[nodiscard]] std::error_code write_bytes(std::int64_t offset,
                                              std::span<const std::uint8_t> bytes,
                                              bool sample_timing,
                                              bool tail_write);
    [[nodiscard]] std::error_code record_persisted_bytes(
        std::size_t bytes) noexcept;
    // 强制把当前 range 的尾部残留刷到磁盘。
    [[nodiscard]] std::error_code flush_tail(
        RangeWriteState& range,
        bool sample_timing);
    // 根据 persisted_offset 推进位图 finished 状态。
    void update_finished_blocks(
        const RangeWriteState& range) noexcept;
    // 从乱序 map 中连续提取已经可以按序写盘的 packet。
    void drain_ordered_packets(
        RangeWriteState& range,
        bool sample_timing);
    void drain_buffered_packets() noexcept;
    // 根据当前缺口大小更新 gap pause。
    void update_gap_flag(RangeWriteState& range);
    // 达到字节阈值或时间阈值后，异步提交 flush + metadata 保存任务。
    void maybe_schedule_flush(bool force);
    // 非阻塞轮询挂起中的 flush 任务是否完成。
    void poll_pending_flush();
    // 退出阶段阻塞等待最后一个 flush 完成。
    void wait_pending_flush();
    [[nodiscard]] std::vector<
        recovery::RecoveryRangeFact>
    build_recovery_ranges() const;
    void publish_commit_result(
        const recovery::CheckpointCommitResult& result);
    // 只记录首个错误，后续错误当作连带症状忽略。
    void set_error(std::error_code error);

    core::SessionState& session_;
    const download::PersistencePolicy policy_;
    flow::PacketConsumer& packet_consumer_;
    core::AtomicBlockBitmap& bitmap_;
    recovery::RecoveryCheckpoint& checkpoint_;
    BS::thread_pool<>& workers_;
    std::vector<std::unique_ptr<RangeWriteState>>
        ranges_;
    const std::size_t geometry_capacity_;
    std::mutex geometry_mutex_;
    std::deque<std::pair<
        RangeRegistrationTicket,
        RangeGeometryCommand>> geometry_commands_;
    std::deque<RangeRegistrationAck> geometry_acks_;
    RangeRegistrationTicket next_geometry_ticket_ = 1;
    std::thread worker_thread_;
    std::future<recovery::CheckpointCommitResult>
        pending_flush_;
    recovery::CheckpointGeneration
        pending_generation_ = 0;
    recovery::CheckpointGeneration
        next_checkpoint_generation_ = 1;
    std::chrono::steady_clock::time_point last_flush_time_{std::chrono::steady_clock::now()};
    std::size_t bytes_since_flush_ = 0;
    mutable std::mutex error_mutex_;
    std::error_code error_;
    bool force_checkpoint_pending_ = false;
    bool stopping_ = false;
};

} // namespace asyncdownload::persistence
