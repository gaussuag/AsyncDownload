#include "persistence_thread.hpp"

#include "asyncdownload/error.hpp"
#include "core/alignment.hpp"
#include "core/constants.hpp"
#include "core/crc32.hpp"
#include "core/memory_accounting.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace asyncdownload::persistence {

namespace {

constexpr std::uint64_t kLatencySampleStride = 64;
constexpr std::uint64_t kLatencySampleMask = kLatencySampleStride - 1;

template <typename T>
void update_peak(std::atomic<T>& target, const T value) noexcept {
    auto current = target.load(std::memory_order_relaxed);
    while (current < value &&
        !target.compare_exchange_weak(current,
            value,
            std::memory_order_release,
            std::memory_order_relaxed)) {
    }
}

void maybe_record_first_persist(core::SessionState& session) noexcept {
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - session.task_started_at).count();
    auto unset = static_cast<std::int64_t>(-1);
    session.performance_metrics.time_to_first_persist_ms.compare_exchange_strong(unset,
        static_cast<std::int64_t>(std::max<std::int64_t>(0, elapsed)),
        std::memory_order_release,
        std::memory_order_relaxed);
}

void update_inflight_peak(core::SessionState& session) noexcept {
    const auto inflight = session.downloaded_bytes.load(std::memory_order_relaxed) -
        session.persisted_bytes.load(std::memory_order_relaxed);
    update_peak(session.performance_metrics.max_inflight_bytes,
        std::max<std::int64_t>(0, inflight));
}

[[nodiscard]] std::int64_t elapsed_ns(const std::chrono::steady_clock::time_point started_at) noexcept {
    return std::max<std::int64_t>(0,
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started_at).count());
}

void record_latency_sample(std::atomic<std::size_t>& sample_count,
                           std::atomic<std::int64_t>& total_ns,
                           std::atomic<std::int64_t>& max_ns,
                           const std::int64_t elapsed) noexcept {
    sample_count.fetch_add(1, std::memory_order_relaxed);
    total_ns.fetch_add(elapsed, std::memory_order_relaxed);
    update_peak(max_ns, elapsed);
}

} // namespace

PersistenceThread::PersistenceThread(core::SessionState& session,
                                     moodycamel::BlockingConcurrentQueue<core::DataPacket>& data_queue,
                                     core::AtomicBlockBitmap& bitmap,
                                     storage::FileWriter& file_writer,
                                     metadata::MetadataStore& metadata_store,
                                     BS::thread_pool<>& workers)
    : session_(session),
      data_queue_(data_queue),
      bitmap_(bitmap),
      file_writer_(file_writer),
      metadata_store_(metadata_store),
      workers_(workers) {}

PersistenceThread::~PersistenceThread() {
    stop();
    join();
}

void PersistenceThread::register_range(core::RangeContext* range) {
    // Orchestrator 可能在运行期追加被 steal 出来的新 range，所以这里不能假设
    // ranges_ 在启动时就固定不变。
    std::scoped_lock lock(ranges_mutex_);
    if (range->range_id >= ranges_.size()) {
        ranges_.resize(range->range_id + 1, nullptr);
    }
    ranges_[range->range_id] = range;
}

void PersistenceThread::start() {
    // Persistence 线程一旦启动，就成为唯一允许推进 persisted_offset、
    // 更新位图和触发 metadata 保存的执行者。
    worker_thread_ = std::thread(&PersistenceThread::process_loop, this);
}

void PersistenceThread::stop() {
    if (stopping_) {
        return;
    }

    stopping_ = true;
    // shutdown 也走同一条队列，这样可以保证它排在已有数据包之后，
    // 让线程先把前面已经入队的数据全部消费掉再退出。
    data_queue_.enqueue(core::DataPacket{.kind = core::PacketKind::shutdown});
}

void PersistenceThread::join() {
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

std::error_code PersistenceThread::error() const noexcept {
    std::scoped_lock lock(error_mutex_);
    return error_;
}

core::MetadataState PersistenceThread::current_metadata_state() const {
    return build_metadata_state();
}

bool PersistenceThread::all_ranges_completed() const noexcept {
    std::scoped_lock lock(ranges_mutex_);
    return std::all_of(ranges_.begin(), ranges_.end(), [](const auto* range) {
        return range == nullptr || range->marked_finished.load(std::memory_order_acquire);
    });
}

void PersistenceThread::process_loop() {
    // 这个循环的职责不是“看到包就写盘”这么简单，而是把网络层吐出来的离散
    // DataPacket 收敛成一条严格有序、按对齐规则落盘、并能周期性生成恢复元数据
    // 的持久化流水线。
    while (true) {
        core::DataPacket packet;
        if (data_queue_.wait_dequeue_timed(packet, 100000)) {
            const auto kind = packet.kind;
            handle_packet(std::move(packet));
            if (kind == core::PacketKind::shutdown) {
                break;
            }
        }

        poll_pending_flush();
        maybe_schedule_flush(false);
        if (error()) {
            break;
        }
    }

    // 主循环退出并不代表最后一轮 flush 已经完成，所以这里还要等待挂起中的
    // flush/meta 任务结束，确保退出时磁盘和 metadata 是同一个版本。
    wait_pending_flush();
}

void PersistenceThread::handle_packet(core::DataPacket packet) {
    if (packet.kind == core::PacketKind::shutdown) {
        // shutdown 不携带数据，但它会强制触发最后一次 flush，
        // 从而把退出前最后那批写入推进到 metadata 和 VDL 里。
        maybe_schedule_flush(true);
        return;
    }

    // 只有真正开始处理这个 packet 时，它才算离开“网络未落盘积压”集合。
    session_.queued_packets.fetch_sub(1, std::memory_order_relaxed);
    if (packet.kind == core::PacketKind::data) {
        const auto queued_bytes = session_.queued_bytes.fetch_sub(
            static_cast<std::int64_t>(packet.accounted_bytes), std::memory_order_relaxed) -
            static_cast<std::int64_t>(packet.accounted_bytes);
        if (queued_bytes < 0) {
            session_.queued_bytes.store(0, std::memory_order_relaxed);
        }
    }

    if (packet.kind == core::PacketKind::range_complete) {
        handle_range_complete(packet.range_id);
        return;
    }

    handle_data_packet(std::move(packet));
}

void PersistenceThread::handle_data_packet(core::DataPacket packet) {
    const auto sample_timing = (sampled_data_packet_counter_++ & kLatencySampleMask) == 0;
    const auto started_at = sample_timing ? std::chrono::steady_clock::now() :
        std::chrono::steady_clock::time_point{};
    const auto finish_sample = [&]() noexcept {
        if (!sample_timing) {
            return;
        }
        record_latency_sample(session_.performance_metrics.latency.handle_data_packet.sample_count,
            session_.performance_metrics.latency.handle_data_packet.total_time_ns,
            session_.performance_metrics.latency.handle_data_packet.max_time_ns,
            elapsed_ns(started_at));
    };

    auto* range = lookup_range(packet.range_id);
    if (range == nullptr) {
        release_packet_memory(packet);
        set_error(make_error_code(DownloadErrc::internal_error));
        finish_sample();
        return;
    }

    if (packet.offset == range->persisted_offset) {
        // 命中当前 expected offset 时，说明这批数据正好可以接到已经落盘的前沿后面，
        // 于是直接写入，并尝试把 map 里后续连续片段一并 drain 掉。
        const auto append_error = append_bytes(*range, packet.offset, packet.payload, sample_timing);
        release_packet_memory(packet);
        if (append_error) {
            set_error(append_error);
            finish_sample();
            return;
        }

        session_.performance_metrics.direct_append_packets_total.fetch_add(
            1, std::memory_order_relaxed);
        range->persisted_offset += static_cast<std::int64_t>(packet.size());
        update_finished_blocks(*range);
        drain_ordered_packets(*range, sample_timing);
    } else {
        // 回调线程不能阻塞等待缺口补齐，所以乱序包先进入 map。
        // Persistence 线程只要等到 expected offset 到达，就能把后续连续片段一起链式写下去。
        packet.accounted_bytes += core::kMapNodeOverheadBytes;
        const auto queued_memory = core::global_memory_accounting().add(core::kMapNodeOverheadBytes);
        update_peak(session_.performance_metrics.max_memory_bytes, queued_memory);
        ++current_out_of_order_packets_;
        current_out_of_order_bytes_ += static_cast<std::int64_t>(packet.accounted_bytes);
        session_.performance_metrics.out_of_order_insert_packets_total.fetch_add(
            1, std::memory_order_relaxed);
        update_peak(session_.performance_metrics.out_of_order_queue_peak_packets,
            current_out_of_order_packets_);
        update_peak(session_.performance_metrics.out_of_order_queue_peak_bytes,
            current_out_of_order_bytes_);
        range->out_of_order_queue.emplace(packet.offset, std::move(packet));
        update_gap_flag(*range);
    }

    maybe_schedule_flush(false);
    finish_sample();
}

void PersistenceThread::handle_range_complete(const std::size_t range_id) {
    auto* range = lookup_range(range_id);
    if (range == nullptr) {
        set_error(make_error_code(DownloadErrc::internal_error));
        return;
    }

    // 网络层认定一个 range 的 HTTP 请求已经全部结束后，Persistence 仍然要做
    // 两件事：把最后没凑满对齐块的 tail 刷掉，以及把状态正式推进到 finished。
    const auto flush_error = flush_tail(*range, false);
    if (flush_error) {
        set_error(flush_error);
        return;
    }

    update_finished_blocks(*range);
    range->marked_finished.store(true, std::memory_order_release);
    range->status.store(static_cast<std::uint8_t>(core::RangeStatus::finished),
        std::memory_order_release);
    maybe_schedule_flush(true);
}

core::RangeContext* PersistenceThread::lookup_range(const std::size_t range_id) const {
    std::scoped_lock lock(ranges_mutex_);
    if (range_id >= ranges_.size()) {
        return nullptr;
    }
    return ranges_[range_id];
}

std::error_code PersistenceThread::append_bytes(core::RangeContext& range,
                                                const std::int64_t offset,
                                                std::span<const std::uint8_t> bytes,
                                                const bool sample_timing) {
    const auto started_at = sample_timing ? std::chrono::steady_clock::now() :
        std::chrono::steady_clock::time_point{};
    const auto finish_sample = [&]() noexcept {
        if (!sample_timing) {
            return;
        }
        record_latency_sample(session_.performance_metrics.latency.append_bytes.sample_count,
            session_.performance_metrics.latency.append_bytes.total_time_ns,
            session_.performance_metrics.latency.append_bytes.max_time_ns,
            elapsed_ns(started_at));
    };
    auto cursor = offset;
    std::size_t index = 0;
    const auto alignment = session_.options.io_alignment;

    while (index < bytes.size()) {
        if (range.tail_buffer.length > 0) {
            if (range.tail_buffer.offset +
                static_cast<std::int64_t>(range.tail_buffer.length) != cursor) {
                return make_error_code(DownloadErrc::internal_error);
            }

            // 先把前一个未对齐尾巴尽量补满；只有形成完整对齐块，或者已经碰到文件末尾，
            // 才会真正落盘。
            const auto writable = std::min(alignment - range.tail_buffer.length,
                bytes.size() - index);
            std::memcpy(range.tail_buffer.data.data() + range.tail_buffer.length,
                bytes.data() + index,
                writable);
            range.tail_buffer.length += writable;
            cursor += static_cast<std::int64_t>(writable);
            index += writable;

            const auto is_last_chunk = range.tail_buffer.offset +
                static_cast<std::int64_t>(range.tail_buffer.length) >= session_.total_size;
            if (range.tail_buffer.length == alignment || is_last_chunk) {
                const auto flush_error = flush_tail(range, sample_timing);
                if (flush_error) {
                    finish_sample();
                    return flush_error;
                }
            }

            continue;
        }

        if ((cursor % static_cast<std::int64_t>(alignment)) != 0) {
            // range 可以从任意偏移开始，但磁盘写入要尽量按 4KB 对齐，所以先把头部
            // 零散字节放进 tail buffer，等后续数据把这个扇区补完整。
            range.tail_buffer.offset = cursor;
            const auto writable = std::min(core::bytes_to_alignment(cursor, alignment),
                bytes.size() - index);
            std::memcpy(range.tail_buffer.data.data(), bytes.data() + index, writable);
            range.tail_buffer.length = writable;
            cursor += static_cast<std::int64_t>(writable);
            index += writable;
            continue;
        }

        const auto aligned_bytes = core::full_aligned_prefix(bytes.size() - index, alignment);
        if (aligned_bytes == 0) {
            // 剩余数据不足一个完整对齐块时不直接写盘，避免每次都触发小块写入。
            range.tail_buffer.offset = cursor;
            std::memcpy(range.tail_buffer.data.data(), bytes.data() + index, bytes.size() - index);
            range.tail_buffer.length = bytes.size() - index;
            finish_sample();
            return {};
        }

        // 真正的磁盘写入只发生在 Persistence 线程里，这样相邻 range 在扇区边界上
        // 不会出现并发 RMW 竞争。
        const auto write_error = write_bytes(cursor, bytes.subspan(index, aligned_bytes), sample_timing);
        if (write_error) {
            finish_sample();
            return write_error;
        }

        bitmap_.mark_downloading_range(cursor,
            cursor + static_cast<std::int64_t>(aligned_bytes),
            session_.options.block_size,
            session_.total_size);
        bytes_since_flush_ += aligned_bytes;
        session_.persisted_bytes.fetch_add(static_cast<std::int64_t>(aligned_bytes),
            std::memory_order_relaxed);
        maybe_record_first_persist(session_);
        update_inflight_peak(session_);
        cursor += static_cast<std::int64_t>(aligned_bytes);
        index += aligned_bytes;
    }

    finish_sample();
    return {};
}

std::error_code PersistenceThread::write_bytes(const std::int64_t offset,
                                               const std::span<const std::uint8_t> bytes,
                                               const bool sample_timing) {
    const auto flush_pending = pending_flush_.valid() &&
        pending_flush_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready;
    const auto write_started_at = sample_timing ? std::chrono::steady_clock::now() :
        std::chrono::steady_clock::time_point{};
    const auto flush_pending_started_at = flush_pending ? std::chrono::steady_clock::now() :
        std::chrono::steady_clock::time_point{};
    const auto write_error = file_writer_.write(offset, bytes);
    session_.performance_metrics.file_write_calls_total.fetch_add(
        1, std::memory_order_relaxed);
    if (sample_timing) {
        record_latency_sample(session_.performance_metrics.latency.file_write.sample_count,
            session_.performance_metrics.latency.file_write.total_time_ns,
            session_.performance_metrics.latency.file_write.max_time_ns,
            elapsed_ns(write_started_at));
    }
    if (flush_pending) {
        record_latency_sample(session_.performance_metrics.latency.flush_pending_write.sample_count,
            session_.performance_metrics.latency.flush_pending_write.total_time_ns,
            session_.performance_metrics.latency.flush_pending_write.max_time_ns,
            elapsed_ns(flush_pending_started_at));
    }
    return write_error;
}

std::error_code PersistenceThread::flush_tail(core::RangeContext& range, const bool sample_timing) {
    if (range.tail_buffer.length == 0) {
        return {};
    }

    const auto remaining = static_cast<std::size_t>(std::max<std::int64_t>(0,
        session_.total_size - range.tail_buffer.offset));
    const auto write_size = std::min(remaining,
        std::max(range.tail_buffer.length, std::min(session_.options.io_alignment, remaining)));
    std::array<std::uint8_t, 4096> bytes{};
    std::memcpy(bytes.data(), range.tail_buffer.data.data(), range.tail_buffer.length);

    // range 结束时即使不足 4KB 也要把尾巴刷掉，否则 metadata 看起来完成了，
    // 但磁盘上最后一个扇区还停留在内存里。
    const auto write_error = write_bytes(range.tail_buffer.offset,
        std::span<const std::uint8_t>(bytes.data(), write_size),
        sample_timing);
    if (write_error) {
        return write_error;
    }

    bitmap_.mark_downloading_range(range.tail_buffer.offset,
        range.tail_buffer.offset + static_cast<std::int64_t>(write_size),
        session_.options.block_size,
        session_.total_size);
    bytes_since_flush_ += range.tail_buffer.length;
    session_.persisted_bytes.fetch_add(static_cast<std::int64_t>(range.tail_buffer.length),
        std::memory_order_relaxed);
    maybe_record_first_persist(session_);
    update_inflight_peak(session_);
    range.tail_buffer = {};
    return {};
}

void PersistenceThread::update_finished_blocks(const core::RangeContext& range) noexcept {
    // downloading 表示“这个 block 已经被触达并落盘过部分内容”，而 finished
    // 表示“这个 block 的全部有效字节都已经可恢复”。这里统一由 persisted_offset
    // 去决定哪些 block 可以升级到 finished。
    bitmap_.mark_finished_range(range.start_offset,
        range.persisted_offset,
        session_.options.block_size,
        session_.total_size);
}

void PersistenceThread::drain_ordered_packets(core::RangeContext& range, const bool sample_timing) {
    // 一旦 expected offset 对上，就尽量把 map 里后面连续的 packet 一次性清空。
    // 这样既能减少 map 常驻量，也能快速消除 gap pause。
    while (!range.out_of_order_queue.empty()) {
        auto next = range.out_of_order_queue.begin();
        if (next->first != range.persisted_offset) {
            break;
        }

        auto packet = std::move(next->second);
        range.out_of_order_queue.erase(next);
        if (current_out_of_order_packets_ > 0) {
            --current_out_of_order_packets_;
        }
        current_out_of_order_bytes_ = std::max<std::int64_t>(0,
            current_out_of_order_bytes_ - static_cast<std::int64_t>(packet.accounted_bytes));

        const auto append_error = append_bytes(range, packet.offset, packet.payload, sample_timing);
        release_packet_memory(packet);
        if (append_error) {
            set_error(append_error);
            return;
        }

        session_.performance_metrics.drained_ordered_packets_total.fetch_add(
            1, std::memory_order_relaxed);
        range.persisted_offset += static_cast<std::int64_t>(packet.size());
        update_finished_blocks(range);
    }

    update_gap_flag(range);
}

void PersistenceThread::update_gap_flag(core::RangeContext& range) {
    if (range.out_of_order_queue.empty()) {
        range.pause_for_gap.store(false, std::memory_order_release);
        return;
    }

    // gap 太大说明这个 range 前面有长时间补不上的洞，再继续接收后续数据只会
    // 无限堆积内存，所以让 Orchestrator 暂停这个 handle，等缺口被补齐后再恢复。
    const auto gap = range.out_of_order_queue.begin()->first - range.persisted_offset;
    range.pause_for_gap.store(gap > static_cast<std::int64_t>(session_.options.max_gap_bytes),
        std::memory_order_release);
}

void PersistenceThread::maybe_schedule_flush(const bool force) {
    if (error()) {
        return;
    }

    // 同一时刻最多只允许一个异步 flush 在跑，避免 file_writer_ 的 flush/read/save
    // 和下一轮快照生成互相打架。
    if (pending_flush_.valid() &&
        pending_flush_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto interval_elapsed = now - last_flush_time_ >= session_.options.flush_interval;
    if (!force && bytes_since_flush_ < session_.options.flush_threshold_bytes && !interval_elapsed) {
        return;
    }

    // Flush 和 metadata 保存都放到线程池异步做，Persistence 线程继续串行消费
    // 数据包，避免把网络到磁盘这条主链卡死在 FlushFileBuffers 上。
    const auto snapshot_started_at = std::chrono::steady_clock::now();
    auto snapshot = build_metadata_state();
    record_latency_sample(session_.performance_metrics.latency.metadata_snapshot.sample_count,
        session_.performance_metrics.latency.metadata_snapshot.total_time_ns,
        session_.performance_metrics.latency.metadata_snapshot.max_time_ns,
        elapsed_ns(snapshot_started_at));
    bytes_since_flush_ = 0;
    last_flush_time_ = now;

    pending_flush_ = workers_.submit_task([this, snapshot]() mutable {
        const auto flush_started_at = std::chrono::steady_clock::now();
        auto flush_error = file_writer_.flush();
        const auto flush_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - flush_started_at).count();
        session_.performance_metrics.flush_count.fetch_add(1, std::memory_order_relaxed);
        session_.performance_metrics.flush_time_ms_total.fetch_add(
            static_cast<std::int64_t>(std::max<std::int64_t>(0, flush_elapsed_ms)),
            std::memory_order_relaxed);
        if (flush_error) {
            return flush_error;
        }

        // CRC 只对 VDL 之后仍被标成 finished 的块采样，因为 VDL 之前的数据已经由
        // “最长连续安全前沿”语义兜底，恢复时不需要再逐块复查。
        snapshot.vdl_offset = bitmap_.contiguous_finished_bytes(session_.options.block_size,
            session_.total_size);
        snapshot.crc_samples = build_crc_samples(snapshot);
        const auto metadata_started_at = std::chrono::steady_clock::now();
        auto metadata_error = metadata_store_.save(snapshot);
        const auto metadata_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - metadata_started_at).count();
        session_.performance_metrics.metadata_save_count.fetch_add(
            1, std::memory_order_relaxed);
        session_.performance_metrics.metadata_save_time_ms_total.fetch_add(
            static_cast<std::int64_t>(std::max<std::int64_t>(0, metadata_elapsed_ms)),
            std::memory_order_relaxed);
        return metadata_error;
    });
}

void PersistenceThread::poll_pending_flush() {
    if (!pending_flush_.valid()) {
        return;
    }

    if (pending_flush_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        return;
    }

    const auto flush_error = pending_flush_.get();
    if (flush_error) {
        set_error(flush_error);
        return;
    }

    // 只有 flush 和 metadata 都成功后，新的 VDL 才算真正对外可见。
    session_.vdl_offset.store(bitmap_.contiguous_finished_bytes(session_.options.block_size,
        session_.total_size), std::memory_order_release);
}

void PersistenceThread::wait_pending_flush() {
    if (!pending_flush_.valid()) {
        return;
    }

    const auto flush_error = pending_flush_.get();
    if (flush_error) {
        set_error(flush_error);
        return;
    }

    // 退出阶段也要按同样的顺序推进 VDL，避免最后一轮已写盘数据没有进入恢复元数据。
    session_.vdl_offset.store(bitmap_.contiguous_finished_bytes(session_.options.block_size,
        session_.total_size), std::memory_order_release);
}

core::MetadataState PersistenceThread::build_metadata_state() const {
    core::MetadataState state{};
    state.url = session_.url;
    state.output_path = session_.paths.output_path;
    state.temporary_path = session_.paths.temporary_path;
    state.total_size = session_.total_size;
    state.accept_ranges = session_.accept_ranges;
    state.resumed = session_.resumed;
    state.etag = session_.etag;
    state.last_modified = session_.last_modified;
    state.block_size = session_.options.block_size;
    state.io_alignment = session_.options.io_alignment;

    core::AtomicBlockBitmap snapshot_bitmap(core::required_block_count(session_.total_size,
        session_.options.block_size));
    snapshot_bitmap.restore(bitmap_.snapshot());

    // metadata 快照不能只信 bitmap 当前值，因为某些 range 的 persisted_offset
    // 可能已经推进了，但本轮 snapshot 还没来得及把这些推进反映到独立副本里。
    std::scoped_lock lock(ranges_mutex_);
    for (const auto* range : ranges_) {
        if (range == nullptr) {
            continue;
        }

        state.ranges.push_back(core::RangeStateSnapshot{
            range->range_id,
            range->start_offset,
            range->end_offset.load(std::memory_order_acquire),
            range->current_offset.load(std::memory_order_acquire),
            range->persisted_offset,
            range->status.load(std::memory_order_acquire)});

        if (range->persisted_offset > range->start_offset) {
            snapshot_bitmap.mark_finished_range(range->start_offset,
                range->persisted_offset,
                session_.options.block_size,
                session_.total_size);
        }
    }

    state.bitmap_states = snapshot_bitmap.snapshot();
    state.vdl_offset = snapshot_bitmap.contiguous_finished_bytes(session_.options.block_size,
        session_.total_size);
    return state;
}

std::vector<core::BlockCrcSample>
PersistenceThread::build_crc_samples(const core::MetadataState& state) const {
    std::vector<core::BlockCrcSample> samples;
    const auto block_size = static_cast<std::int64_t>(state.block_size);

    for (std::size_t index = 0; index < state.bitmap_states.size(); ++index) {
        if (state.bitmap_states[index] != static_cast<std::uint8_t>(core::BlockState::finished)) {
            continue;
        }

        const auto offset = static_cast<std::int64_t>(index) * block_size;
        if (offset < state.vdl_offset) {
            continue;
        }

        const auto length = static_cast<std::size_t>(std::min(block_size,
            state.total_size - offset));
        std::vector<std::byte> bytes;
        const auto read_started_at = std::chrono::steady_clock::now();
        const auto read_error = file_writer_.read(offset, length, bytes);
        record_latency_sample(session_.performance_metrics.latency.crc_sample_read.sample_count,
            session_.performance_metrics.latency.crc_sample_read.total_time_ns,
            session_.performance_metrics.latency.crc_sample_read.max_time_ns,
            elapsed_ns(read_started_at));
        if (read_error) {
            continue;
        }

        session_.performance_metrics.crc_sample_blocks_total.fetch_add(
            1, std::memory_order_relaxed);
        session_.performance_metrics.crc_sample_bytes_total.fetch_add(
            static_cast<std::int64_t>(length), std::memory_order_relaxed);
        samples.push_back(core::BlockCrcSample{offset, core::crc32(bytes), length});
    }

    return samples;
}

void PersistenceThread::release_packet_memory(const core::DataPacket& packet) noexcept {
    // 包的内存直到 Persistence 真正处理完成后才释放，这样全局内存会计能准确反映
    // “还没落盘的数据究竟占了多少内存”。
    const auto released_memory = core::global_memory_accounting().subtract(packet.accounted_bytes);
    static_cast<void>(released_memory);
}

void PersistenceThread::set_error(const std::error_code error) {
    std::scoped_lock lock(error_mutex_);
    if (!error_) {
        // 只保留第一个错误，后续错误通常只是连带症状；这样调用方看到的根因更稳定。
        error_ = error;
    }
}

} // namespace asyncdownload::persistence


