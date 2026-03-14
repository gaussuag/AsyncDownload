#pragma once

#include "core/block_bitmap.hpp"
#include "core/models.hpp"
#include "metadata/metadata_store.hpp"
#include "storage/file_writer.hpp"

#include <concurrentqueue/blockingconcurrentqueue.h>
#include <thread-pool/BS_thread_pool.hpp>

#include <chrono>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

namespace asyncdownload::persistence {

class PersistenceThread {
public:
    // PersistenceThread 独占管理以下职责：
    // 1. RangeContext 的真实落盘前沿
    // 2. 乱序 packet 的重排
    // 3. 4KB 对齐写入与 tail buffer
    // 4. 位图推进
    // 5. flush / metadata / VDL 更新
    PersistenceThread(core::SessionState& session,
                      moodycamel::BlockingConcurrentQueue<core::DataPacket>& data_queue,
                      core::AtomicBlockBitmap& bitmap,
                      storage::FileWriter& file_writer,
                      metadata::MetadataStore& metadata_store,
                      BS::thread_pool<>& workers);
    ~PersistenceThread();

    PersistenceThread(const PersistenceThread&) = delete;
    PersistenceThread& operator=(const PersistenceThread&) = delete;

    // 注册一个可被该线程管理的 range。运行期 steal 出来的新 range 也会经过这里。
    void register_range(core::RangeContext* range);
    // 启动后台持久化线程。
    void start();
    // 通过 enqueue shutdown 控制包请求线程收尾退出。
    void stop();
    // 等待后台线程结束。
    void join();

    // 返回后台线程记录的首个错误。
    [[nodiscard]] std::error_code error() const noexcept;
    // 生成当前可用于恢复的 metadata 快照。
    [[nodiscard]] core::MetadataState current_metadata_state() const;
    // 判断所有已注册 range 是否都已进入 marked_finished。
    [[nodiscard]] bool all_ranges_completed() const noexcept;

private:
    // 主循环：消费 packet、轮询 flush 结果、按阈值发起新的 flush。
    void process_loop();
    // 按 packet.kind 分流到 data / range_complete / shutdown 三类处理路径。
    void handle_packet(core::DataPacket packet);
    void handle_data_packet(core::DataPacket packet);
    void handle_range_complete(std::size_t range_id);
    [[nodiscard]] core::RangeContext* lookup_range(std::size_t range_id) const;
    // 把一段逻辑字节按对齐规则写入磁盘，必要时借助 tail buffer 补齐。
    [[nodiscard]] std::error_code append_bytes(core::RangeContext& range,
                                               std::int64_t offset,
                                               std::span<const std::uint8_t> bytes,
                                               bool sample_timing);
    [[nodiscard]] std::error_code write_bytes(std::int64_t offset,
                                              std::span<const std::uint8_t> bytes,
                                              bool sample_timing,
                                              bool tail_write);
    // 强制把当前 range 的尾部残留刷到磁盘。
    [[nodiscard]] std::error_code flush_tail(core::RangeContext& range, bool sample_timing);
    // 根据 persisted_offset 推进位图 finished 状态。
    void update_finished_blocks(const core::RangeContext& range) noexcept;
    // 从乱序 map 中连续提取已经可以按序写盘的 packet。
    void drain_ordered_packets(core::RangeContext& range, bool sample_timing);
    // 根据当前缺口大小更新 pause_for_gap。
    void update_gap_flag(core::RangeContext& range);
    // 达到字节阈值或时间阈值后，异步提交 flush + metadata 保存任务。
    void maybe_schedule_flush(bool force);
    // 非阻塞轮询挂起中的 flush 任务是否完成。
    void poll_pending_flush();
    // 退出阶段阻塞等待最后一个 flush 完成。
    void wait_pending_flush();
    // 汇总当前位图、range 前沿和资源身份信息，构造 metadata 快照。
    [[nodiscard]] core::MetadataState build_metadata_state() const;
    // 为 VDL 之后仍 finished 的块生成 CRC 样本。
    [[nodiscard]] std::vector<core::BlockCrcSample>
    build_crc_samples(const core::MetadataState& state) const;
    // 在 packet 彻底被消费后回收其内存会计。
    void release_packet_memory(const core::DataPacket& packet) noexcept;
    // 只记录首个错误，后续错误当作连带症状忽略。
    void set_error(std::error_code error);

    core::SessionState& session_;
    moodycamel::BlockingConcurrentQueue<core::DataPacket>& data_queue_;
    core::AtomicBlockBitmap& bitmap_;
    storage::FileWriter& file_writer_;
    metadata::MetadataStore& metadata_store_;
    BS::thread_pool<>& workers_;
    mutable std::mutex ranges_mutex_;
    std::vector<core::RangeContext*> ranges_;
    std::thread worker_thread_;
    std::future<std::error_code> pending_flush_;
    std::chrono::steady_clock::time_point last_flush_time_{std::chrono::steady_clock::now()};
    std::size_t bytes_since_flush_ = 0;
    std::size_t current_out_of_order_packets_ = 0;
    std::int64_t current_out_of_order_bytes_ = 0;
    mutable std::mutex error_mutex_;
    std::error_code error_;
    bool stopping_ = false;
    std::uint64_t sampled_data_packet_counter_ = 0;
};

} // namespace asyncdownload::persistence
