#include "download_engine.hpp"

#include "asyncdownload/error.hpp"
#include "core/block_bitmap.hpp"
#include "core/crc32.hpp"
#include "core/memory_accounting.hpp"
#include "core/models.hpp"
#include "core/path_utils.hpp"
#include "download/http_probe.hpp"
#include "download/range_scheduler.hpp"
#include "metadata/metadata_store.hpp"
#include "persistence/persistence_thread.hpp"
#include "storage/file_writer.hpp"

#include <concurrentqueue/blockingconcurrentqueue.h>
#include <thread-pool/BS_thread_pool.hpp>

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace asyncdownload::download {
namespace {

using Clock = std::chrono::steady_clock;

struct TransferHandle {
    // TransferHandle 代表一个可复用的 easy handle 槽位。
    // 它既保存 libcurl 句柄，也保存当前绑定到哪个 range/window，以及暂停原因、
    // 响应码、速度等运行期状态。
    core::SessionState* session = nullptr;
    moodycamel::BlockingConcurrentQueue<core::DataPacket>* data_queue = nullptr;
    CURL* easy = nullptr;
    core::RangeContext* range = nullptr;
    std::string range_header;
    std::int64_t request_start = 0;
    std::int64_t request_end = -1;
    std::int64_t next_offset = 0;
    std::int64_t request_bytes = 0;
    long response_code = 0;
    double speed_bytes_per_second = 0.0;
    bool in_multi = false;
    bool paused_by_memory = false;
    bool paused_by_gap = false;
    CURLcode curl_result = CURLE_OK;
    Clock::time_point request_started{};
};

class CurlGlobal {
public:
    CurlGlobal() noexcept {
        static const auto init_result = curl_global_init(CURL_GLOBAL_DEFAULT);
        initialized_ = init_result == CURLE_OK;
    }

    [[nodiscard]] bool ok() const noexcept {
        return initialized_;
    }

private:
    bool initialized_ = false;
};

[[nodiscard]] std::size_t packet_accounted_bytes(const std::size_t payload_size,
                                                 const bool include_map_overhead) noexcept {
    return core::global_packet_overhead(payload_size, include_map_overhead);
}

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

void maybe_record_relative_time_ms(std::atomic<std::int64_t>& target,
                                   const core::SessionState& session,
                                   const Clock::time_point now) noexcept {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - session.task_started_at).count();
    auto unset = static_cast<std::int64_t>(-1);
    const auto elapsed_ms = static_cast<std::int64_t>(std::max<std::int64_t>(0, elapsed));
    target.compare_exchange_strong(unset,
        elapsed_ms,
        std::memory_order_release,
        std::memory_order_relaxed);
}

void update_progress_rates(core::SessionState& session,
                           ProgressSnapshot& snapshot) noexcept {
    // 速度统一由调度线程基于相邻两次快照做差分计算，这样网络速率和磁盘速率
    // 使用同一个时间窗口，UI 看到的数据更容易对比。
    const auto now = Clock::now();
    if (session.last_progress_sample_at.time_since_epoch().count() == 0) {
        session.last_progress_sample_at = now;
        session.last_progress_downloaded_bytes = snapshot.downloaded_bytes;
        session.last_progress_persisted_bytes = snapshot.persisted_bytes;
        snapshot.network_bytes_per_second = session.last_network_bytes_per_second;
        snapshot.disk_bytes_per_second = session.last_disk_bytes_per_second;
        return;
    }

    const auto elapsed_seconds =
        std::chrono::duration_cast<std::chrono::duration<double>>(now -
            session.last_progress_sample_at).count();
    if (elapsed_seconds > 0.0) {
        const auto downloaded_delta =
            snapshot.downloaded_bytes - session.last_progress_downloaded_bytes;
        const auto persisted_delta =
            snapshot.persisted_bytes - session.last_progress_persisted_bytes;
        session.last_network_bytes_per_second =
            std::max(0.0, static_cast<double>(downloaded_delta) / elapsed_seconds);
        session.last_disk_bytes_per_second =
            std::max(0.0, static_cast<double>(persisted_delta) / elapsed_seconds);
    }

    session.last_progress_sample_at = now;
    session.last_progress_downloaded_bytes = snapshot.downloaded_bytes;
    session.last_progress_persisted_bytes = snapshot.persisted_bytes;
    snapshot.network_bytes_per_second = session.last_network_bytes_per_second;
    snapshot.disk_bytes_per_second = session.last_disk_bytes_per_second;
    session.peak_network_bytes_per_second = std::max(session.peak_network_bytes_per_second,
        snapshot.network_bytes_per_second);
    session.peak_disk_bytes_per_second = std::max(session.peak_disk_bytes_per_second,
        snapshot.disk_bytes_per_second);
}

void invoke_progress(core::SessionState& session,
                     const std::vector<std::unique_ptr<core::RangeContext>>& ranges,
                     const std::vector<TransferHandle>& handles) noexcept {
    if (!session.progress_callback) {
        return;
    }

    // ProgressSnapshot 的来源分成三类：
    // 1. SessionState 里的全局原子计数
    // 2. range 状态机里的逻辑状态
    // 3. 当前 easy handle 列表里的实时请求状态
    ProgressSnapshot snapshot{};
    snapshot.total_bytes = session.total_size;
    snapshot.downloaded_bytes = session.downloaded_bytes.load(std::memory_order_relaxed);
    snapshot.persisted_bytes = session.persisted_bytes.load(std::memory_order_relaxed);
    snapshot.vdl_offset = session.vdl_offset.load(std::memory_order_relaxed);
    snapshot.inflight_bytes = std::max<std::int64_t>(0,
        snapshot.downloaded_bytes - snapshot.persisted_bytes);
    snapshot.queued_packets = session.queued_packets.load(std::memory_order_relaxed);
    snapshot.memory_bytes = core::global_memory_accounting().current_bytes();
    snapshot.resumed = session.resumed;

    for (const auto& range : ranges) {
        const auto status = static_cast<core::RangeStatus>(
            range->status.load(std::memory_order_acquire));
        if (status == core::RangeStatus::downloading || status == core::RangeStatus::paused) {
            ++snapshot.active_ranges;
        }
        if (range->marked_finished.load(std::memory_order_acquire)) {
            ++snapshot.finished_ranges;
        }
        if (status == core::RangeStatus::paused) {
            ++snapshot.paused_ranges;
        }
        if (range->pause_for_gap.load(std::memory_order_acquire)) {
            ++snapshot.gap_paused_ranges;
        }
        if (range->pause_for_memory.load(std::memory_order_acquire)) {
            ++snapshot.memory_paused_ranges;
        }
    }

    for (const auto& handle : handles) {
        if (handle.in_multi) {
            ++snapshot.active_requests;
        }
    }

    update_peak(session.max_memory_bytes, snapshot.memory_bytes);
    update_peak(session.max_inflight_bytes, snapshot.inflight_bytes);
    update_peak(session.max_queued_packets, snapshot.queued_packets);
    update_peak(session.max_active_requests, snapshot.active_requests);
    update_progress_rates(session, snapshot);

    try {
        session.progress_callback(snapshot);
    } catch (...) {
    }
}

[[nodiscard]] bool metadata_matches(const core::MetadataState& state,
                                    const DownloadRequest& request,
                                    const core::RemoteProbeResult& probe,
                                    const core::SessionPaths& paths) noexcept {
    // 恢复条件必须同时满足“本地任务身份一致”和“远端资源身份一致”。
    // 只要 URL、路径、总大小、对齐参数、ETag、Last-Modified 有冲突，就宁可重下，
    // 也不冒险把旧状态接到新资源上。
    if (state.url != request.url ||
        state.output_path != paths.output_path ||
        state.temporary_path != paths.temporary_path ||
        state.total_size != probe.total_size) {
        return false;
    }

    if (state.block_size != request.options.block_size ||
        state.io_alignment != request.options.io_alignment) {
        return false;
    }

    if (!probe.etag.empty() && !state.etag.empty() && probe.etag != state.etag) {
        return false;
    }

    if (!probe.last_modified.empty() &&
        !state.last_modified.empty() &&
        probe.last_modified != state.last_modified) {
        return false;
    }

    return true;
}

[[nodiscard]] std::int64_t sum_finished_bytes(const core::AtomicBlockBitmap& bitmap,
                                              const std::size_t block_size,
                                              const std::int64_t total_size) noexcept {
    std::int64_t total = 0;
    // 这个统计是“所有 finished block 的总和”，用于展示和最终结果汇总。
    // 它和 VDL 不同，不要求从文件头开始连续。
    for (std::size_t index = 0; index < bitmap.block_count(); ++index) {
        if (bitmap.load(index) != core::BlockState::finished) {
            continue;
        }

        const auto block_begin = static_cast<std::int64_t>(index * block_size);
        const auto block_end = std::min(block_begin + static_cast<std::int64_t>(block_size),
            total_size);
        total += block_end - block_begin;
    }

    return total;
}

void rebuild_bitmap_from_snapshots(core::AtomicBlockBitmap& bitmap,
                                   const std::vector<core::RangeStateSnapshot>& ranges,
                                   const std::size_t block_size,
                                   const std::int64_t total_size) noexcept {
    // metadata 里的 range 快照记录了各个 range 已经推进到哪里。
    // 恢复时把这些 persisted_offset 再投影回 bitmap，可以补齐仅靠旧 bitmap 快照
    // 还未完全表达出来的 finished 区域。
    for (const auto& range : ranges) {
        if (range.persisted_offset <= range.start_offset) {
            continue;
        }

        bitmap.mark_finished_range(range.start_offset,
            range.persisted_offset,
            block_size,
            total_size);
    }
}

void rebuild_bitmap_from_ranges(
    core::AtomicBlockBitmap& bitmap,
    const std::vector<std::unique_ptr<core::RangeContext>>& ranges,
    const std::size_t block_size,
    const std::int64_t total_size) noexcept {
    // 正常退出时也做同样的投影，确保最终结果以 Persistence 线程实际推进过的
    // persisted_offset 为准，而不是以中途某个旧快照为准。
    for (const auto& range : ranges) {
        if (!range || range->persisted_offset <= range->start_offset) {
            continue;
        }

        bitmap.mark_finished_range(range->start_offset,
            range->persisted_offset,
            block_size,
            total_size);
    }
}

[[nodiscard]] std::error_code validate_resumed_blocks(const core::MetadataState& state,
                                                      storage::FileWriter& file_writer,
                                                      core::AtomicBlockBitmap& bitmap) noexcept {
    const auto block_size = static_cast<std::int64_t>(state.block_size);

    // 恢复时只复查 VDL 之后的 finished block：
    // VDL 之前已经由“最长连续安全前沿”兜底；VDL 之后则必须依赖 CRC 样本确认。
    for (std::size_t index = 0; index < bitmap.block_count(); ++index) {
        if (bitmap.load(index) != core::BlockState::finished) {
            continue;
        }

        const auto offset = static_cast<std::int64_t>(index) * block_size;
        if (offset < state.vdl_offset) {
            continue;
        }

        const auto sample_it = std::find_if(state.crc_samples.begin(), state.crc_samples.end(),
            [offset](const core::BlockCrcSample& sample) {
                return sample.offset == offset;
            });
        if (sample_it == state.crc_samples.end()) {
            bitmap.store(index, core::BlockState::empty);
            continue;
        }

        std::vector<std::byte> bytes;
        const auto read_error = file_writer.read(offset, sample_it->length, bytes);
        if (read_error) {
            return read_error;
        }

        if (core::crc32(bytes) != sample_it->crc32) {
            bitmap.store(index, core::BlockState::empty);
        }
    }

    return {};
}

void rollback_inflight_window(TransferHandle& transfer) noexcept {
    if (transfer.range == nullptr) {
        return;
    }

    // arm_transfer 在派发一个 window 前会先把 current_offset 推进到 window 末尾之后。
    // 如果这个请求中途失败，就需要把这段“尚未真正完成”的租约回滚回 next_offset，
    // 否则调度器会误以为这些字节已经被可靠处理过。
    const auto current = transfer.range->current_offset.load(std::memory_order_acquire);
    if (transfer.next_offset < current) {
        transfer.range->current_offset.store(transfer.next_offset, std::memory_order_release);
    }
}

void enqueue_control_packet(moodycamel::BlockingConcurrentQueue<core::DataPacket>& data_queue,
                            core::SessionState& session,
                            const core::PacketKind kind,
                            const std::size_t range_id) {
    // control packet 复用同一条队列，把“数据流”和“状态切换通知”串到同一个顺序里，
    // 这样 Persistence 看到的事件顺序就和 Orchestrator 发出的顺序一致。
    core::DataPacket packet{};
    packet.kind = kind;
    packet.range_id = range_id;
    data_queue.enqueue(std::move(packet));
    const auto queued = session.queued_packets.fetch_add(1, std::memory_order_relaxed) + 1;
    update_peak(session.max_queued_packets, queued);
}

void mark_range_status(core::RangeContext& range, const core::RangeStatus status) noexcept {
    range.status.store(static_cast<std::uint8_t>(status), std::memory_order_release);
}

[[nodiscard]] bool response_is_valid(const core::SessionState& session,
                                     const TransferHandle& transfer) noexcept {
    // Range 模式下，除非这个请求覆盖整个文件，否则必须看到 206。
    // 否则服务端可能忽略了 Range 头，继续下载会把数据布局全部打乱。
    if (session.accept_ranges) {
        if (transfer.request_start == 0 && transfer.request_end >= session.total_size - 1) {
            return transfer.response_code == 200 || transfer.response_code == 206;
        }

        return transfer.response_code == 206;
    }

    return transfer.response_code == 200 || transfer.response_code == 206;
}

void update_speed(TransferHandle& transfer) noexcept {
    // 先用 libcurl 自带的速度统计；若当前平台或当前时刻拿不到，就退回到
    // “本次 window 已接收字节 / 已运行时间”的粗略估算。
    curl_off_t speed = 0;
    if (curl_easy_getinfo(transfer.easy, CURLINFO_SPEED_DOWNLOAD_T, &speed) == CURLE_OK &&
        speed > 0) {
        transfer.speed_bytes_per_second = static_cast<double>(speed);
        return;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - transfer.request_started);
    if (elapsed.count() > 0) {
        transfer.speed_bytes_per_second =
            static_cast<double>(transfer.request_bytes) * 1000.0 / elapsed.count();
    }
}

[[nodiscard]] std::error_code arm_transfer(TransferHandle& transfer,
                                           CURLM* multi,
                                           const core::SessionState& session,
                                           core::RangeContext& range,
                                           const RangeScheduler& scheduler) noexcept {
    const auto window = scheduler.next_window(range);
    if (window.first > window.second) {
        transfer.range = nullptr;
        return {};
    }

    // 派发 window 前先把逻辑租约登记到 range.current_offset。
    // 即使后面发生失败，也可以借助 rollback_inflight_window 把这段租约收回。
    range.current_offset.store(window.second + 1, std::memory_order_release);
    mark_range_status(range, core::RangeStatus::downloading);

    transfer.range = &range;
    transfer.request_start = window.first;
    transfer.request_end = window.second;
    transfer.next_offset = window.first;
    transfer.request_bytes = 0;
    transfer.response_code = 0;
    transfer.speed_bytes_per_second = 0.0;
    transfer.paused_by_memory = false;
    transfer.paused_by_gap = false;
    transfer.curl_result = CURLE_OK;
    transfer.request_started = Clock::now();
    transfer.range_header.clear();
    transfer.session->windows_total.fetch_add(1, std::memory_order_relaxed);

    curl_easy_reset(transfer.easy);
    curl_easy_setopt(transfer.easy, CURLOPT_URL, session.url.c_str());
    curl_easy_setopt(transfer.easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(transfer.easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(transfer.easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(transfer.easy, CURLOPT_WRITEFUNCTION, +[](char* data,
                                                               const size_t size,
                                                               const size_t nmemb,
                                                               void* user_data) -> size_t {
        auto* current = static_cast<TransferHandle*>(user_data);
        const auto bytes = size * nmemb;
        if (bytes == 0 || current == nullptr || current->range == nullptr) {
            return 0;
        }

        current->session->write_callback_calls.fetch_add(1, std::memory_order_relaxed);

        // stop_requested 表示主线程已经决定收尾，回调这里直接返回失败，
        // 让 libcurl 尽快结束该请求。
        if (current->session->stop_requested.load(std::memory_order_acquire)) {
            return 0;
        }

        const auto remaining = current->request_end - current->next_offset + 1;
        if (remaining <= 0) {
            // 如果服务端继续往回调里塞数据，但逻辑 window 已经没有剩余额度，
            // 就暂停接收，避免把后续字节算进错误的区间。
            if (!current->paused_by_memory) {
                current->session->window_boundary_pause_count.fetch_add(1,
                    std::memory_order_relaxed);
            }
            current->paused_by_memory = true;
            current->range->pause_for_memory.store(true, std::memory_order_release);
            mark_range_status(*current->range, core::RangeStatus::paused);
            return CURL_WRITEFUNC_PAUSE;
        }

        const auto allowed = std::min<std::size_t>(bytes, static_cast<std::size_t>(remaining));
        if (allowed != bytes) {
            // window 化调度要求一个请求只能覆盖分配给它的那段字节。
            // 只要回调给出的数据超出当前 window，就把这次传输视为异常。
            return 0;
        }

        const auto accounted = packet_accounted_bytes(allowed, false);
        const auto current_bytes = core::global_memory_accounting().current_bytes();
        if (core::should_pause_for_backpressure(current_bytes,
                accounted,
                current->session->options.backpressure_high_bytes)) {
            // 这里不阻塞等待队列腾空间，而是立刻 pause 当前 easy handle，
            // 把“什么时候恢复”交回给事件循环中的统一背压逻辑。
            if (!current->paused_by_memory) {
                current->session->memory_pause_count.fetch_add(1, std::memory_order_relaxed);
            }
            current->paused_by_memory = true;
            current->range->pause_for_memory.store(true, std::memory_order_release);
            mark_range_status(*current->range, core::RangeStatus::paused);
            return CURL_WRITEFUNC_PAUSE;
        }

        core::DataPacket packet{};
        packet.kind = core::PacketKind::data;
        packet.range_id = current->range->range_id;
        packet.offset = current->next_offset;
        packet.payload.assign(reinterpret_cast<const std::uint8_t*>(data),
            reinterpret_cast<const std::uint8_t*>(data) + allowed);
        packet.accounted_bytes = accounted;
        update_peak(current->session->max_packet_size_bytes, allowed);

        const auto queued_memory = core::global_memory_accounting().add(packet.accounted_bytes);
        update_peak(current->session->max_memory_bytes, queued_memory);
        if (!current->data_queue->try_enqueue(std::move(packet))) {
            const auto released_memory = core::global_memory_accounting().subtract(accounted);
            static_cast<void>(released_memory);
            // 队列入队失败和高水位触发一样，都采用“非阻塞暂停”策略，
            // 不在 libcurl 回调里做任何等待。
            if (!current->paused_by_memory) {
                current->session->queue_full_pause_count.fetch_add(1,
                    std::memory_order_relaxed);
            }
            current->paused_by_memory = true;
            current->range->pause_for_memory.store(true, std::memory_order_release);
            mark_range_status(*current->range, core::RangeStatus::paused);
            return CURL_WRITEFUNC_PAUSE;
        }

        // downloaded_bytes 统计的是“已经从网络收到并交给持久化链路处理”的字节数，
        // 它不等同于 persisted_bytes。
        current->session->packets_enqueued_total.fetch_add(1, std::memory_order_relaxed);
        const auto queued = current->session->queued_packets.fetch_add(1,
            std::memory_order_relaxed) + 1;
        update_peak(current->session->max_queued_packets, queued);
        const auto downloaded = current->session->downloaded_bytes.fetch_add(
            static_cast<std::int64_t>(allowed), std::memory_order_relaxed) +
            static_cast<std::int64_t>(allowed);
        const auto inflight = downloaded -
            current->session->persisted_bytes.load(std::memory_order_relaxed);
        update_peak(current->session->max_inflight_bytes, std::max<std::int64_t>(0, inflight));
        maybe_record_relative_time_ms(current->session->time_to_first_byte_ms,
            *current->session,
            Clock::now());
        current->next_offset += static_cast<std::int64_t>(allowed);
        current->request_bytes += static_cast<std::int64_t>(allowed);
        return allowed;
    });
    curl_easy_setopt(transfer.easy, CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(transfer.easy, CURLOPT_PRIVATE, &transfer);
    curl_easy_setopt(transfer.easy, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(transfer.easy, CURLOPT_ACCEPT_ENCODING, "");
    // 每个 window 请求都强制走新的连接，避免 multi 的连接缓存把并发 range
    // 折叠到同一条 TCP 连接上。
    curl_easy_setopt(transfer.easy, CURLOPT_FRESH_CONNECT, 1L);
    curl_easy_setopt(transfer.easy, CURLOPT_FORBID_REUSE, 1L);
    curl_easy_setopt(transfer.easy, CURLOPT_PIPEWAIT, 0L);

    if (session.accept_ranges) {
        transfer.range_header = std::to_string(transfer.request_start) + "-" +
            std::to_string(transfer.request_end);
        curl_easy_setopt(transfer.easy, CURLOPT_RANGE, transfer.range_header.c_str());
    }

    if (curl_multi_add_handle(multi, transfer.easy) != CURLM_OK) {
        transfer.range = nullptr;
        return make_error_code(DownloadErrc::http_transfer_failed);
    }

    transfer.in_multi = true;
    return {};
}

void resume_paused_transfers(std::vector<TransferHandle>& handles,
                             const std::size_t low_watermark) noexcept {
    if (core::global_memory_accounting().current_bytes() > low_watermark) {
        return;
    }

    // 只有内存真正回落到低水位以下才统一恢复，避免高低水位附近的抖动导致
    // handle 在 pause/continue 之间来回震荡。
    for (auto& handle : handles) {
        if (!handle.in_multi || !handle.paused_by_memory) {
            continue;
        }

        handle.paused_by_memory = false;
        if (handle.range != nullptr) {
            handle.range->pause_for_memory.store(false, std::memory_order_release);
            if (handle.range->pause_for_gap.load(std::memory_order_acquire)) {
                continue;
            }

            mark_range_status(*handle.range, core::RangeStatus::downloading);
        }

        curl_easy_pause(handle.easy, CURLPAUSE_CONT);
    }
}

void apply_gap_pauses(std::vector<TransferHandle>& handles) noexcept {
    // gap pause 的信号来自 Persistence 线程，它比网络层更早知道某个 range 前面
    // 是否已经堆出了过大的洞。
    for (auto& handle : handles) {
        if (!handle.in_multi || handle.range == nullptr) {
            continue;
        }

        const auto pause_for_gap = handle.range->pause_for_gap.load(std::memory_order_acquire);
        if (pause_for_gap && !handle.paused_by_gap) {
            handle.session->gap_pause_count.fetch_add(1, std::memory_order_relaxed);
            handle.paused_by_gap = true;
            mark_range_status(*handle.range, core::RangeStatus::paused);
            curl_easy_pause(handle.easy, CURLPAUSE_RECV);
        } else if (!pause_for_gap && handle.paused_by_gap) {
            handle.paused_by_gap = false;
            if (!handle.paused_by_memory) {
                mark_range_status(*handle.range, core::RangeStatus::downloading);
                curl_easy_pause(handle.easy, CURLPAUSE_CONT);
            }
        }
    }
}

void apply_memory_backpressure(std::vector<TransferHandle>& handles,
                               const std::size_t high_watermark) noexcept {
    if (core::global_memory_accounting().current_bytes() <= high_watermark) {
        return;
    }

    std::vector<TransferHandle*> active;
    active.reserve(handles.size());
    // 内存红线被踩中后，不是把所有连接一刀切暂停，而是优先暂停当前最快的那部分，
    // 让整体积压回落得更快。
    for (auto& handle : handles) {
        if (!handle.in_multi || handle.paused_by_memory || handle.range == nullptr) {
            continue;
        }

        update_speed(handle);
        active.push_back(&handle);
    }

    if (active.empty()) {
        return;
    }

    std::sort(active.begin(), active.end(), [](const TransferHandle* lhs, const TransferHandle* rhs) {
        return lhs->speed_bytes_per_second > rhs->speed_bytes_per_second;
    });

    const auto pause_count = std::max<std::size_t>(1, (active.size() + 4) / 5);
    for (std::size_t index = 0; index < pause_count && index < active.size(); ++index) {
        auto* handle = active[index];
        if (!handle->paused_by_memory) {
            handle->session->memory_pause_count.fetch_add(1, std::memory_order_relaxed);
        }
        handle->paused_by_memory = true;
        handle->range->pause_for_memory.store(true, std::memory_order_release);
        mark_range_status(*handle->range, core::RangeStatus::paused);
        curl_easy_pause(handle->easy, CURLPAUSE_RECV);
    }
}

[[nodiscard]] std::error_code finalize_completed_request(
    TransferHandle& transfer,
    moodycamel::BlockingConcurrentQueue<core::DataPacket>& data_queue,
    core::SessionState& session,
    std::deque<core::RangeContext*>& pending_ranges) noexcept {
    if (transfer.range == nullptr) {
        return {};
    }

    // 这里处理的是“一个 HTTP window 请求结束了”，不是“整个下载任务结束了”。
    // 所以它既负责校验这次请求，也负责决定后续应该继续调度还是宣告 range 完成。
    update_speed(transfer);
    if (transfer.curl_result != CURLE_OK) {
        rollback_inflight_window(transfer);
        mark_range_status(*transfer.range, core::RangeStatus::failed);
        return make_error_code(DownloadErrc::http_transfer_failed);
    }

    if (!response_is_valid(session, transfer)) {
        rollback_inflight_window(transfer);
        mark_range_status(*transfer.range, core::RangeStatus::failed);
        return make_error_code(DownloadErrc::http_invalid_response);
    }

    if (transfer.next_offset <= transfer.request_end) {
        rollback_inflight_window(transfer);
        mark_range_status(*transfer.range, core::RangeStatus::failed);
        return make_error_code(DownloadErrc::http_transfer_failed);
    }

    transfer.range->pause_for_memory.store(false, std::memory_order_release);
    transfer.range->pause_for_gap.store(false, std::memory_order_release);

    const auto next_start = transfer.range->current_offset.load(std::memory_order_acquire);
    const auto end = transfer.range->end_offset.load(std::memory_order_acquire);
    if (next_start <= end) {
        // 这个 range 还有尾巴没下载完，就把它重新放回待调度队列，
        // 后面会继续为它派发下一个 window。
        mark_range_status(*transfer.range, core::RangeStatus::empty);
        pending_ranges.push_back(transfer.range);
    } else if (!transfer.range->completion_notified.exchange(true, std::memory_order_acq_rel)) {
        // 真正的 finished 由 Persistence 线程在 flush tail 后确认；
        // Orchestrator 这里只负责发一个“网络阶段已完成”的控制消息。
        enqueue_control_packet(data_queue, session, core::PacketKind::range_complete,
            transfer.range->range_id);
    }

    transfer.range = nullptr;
    transfer.range_header.clear();
    transfer.paused_by_gap = false;
    transfer.paused_by_memory = false;
    return {};
}

void release_transfer(CURLM* multi, TransferHandle& transfer) noexcept {
    if (transfer.in_multi) {
        curl_multi_remove_handle(multi, transfer.easy);
        transfer.in_multi = false;
    }

    // easy handle 会被复用给下一个 range/window，因此这里只清运行期状态，
    // 不销毁底层 easy 对象本身。
    transfer.range = nullptr;
    transfer.range_header.clear();
    transfer.paused_by_gap = false;
    transfer.paused_by_memory = false;
}

void stop_network_phase(core::SessionState& session,
                        CURLM* multi,
                        std::vector<TransferHandle>& handles) noexcept {
    // 退出红线的第一步是先停网络生产，避免 Persistence 在 drain 队列时又收到
    // 新数据，从而把收尾阶段拉回“边消费边生产”的竞态。
    session.stop_requested.store(true, std::memory_order_release);
    for (auto& handle : handles) {
        rollback_inflight_window(handle);
        release_transfer(multi, handle);
    }
}

std::error_code stop_persistence_phase(persistence::PersistenceThread& persistence,
                                       std::error_code failure) noexcept {
    // 网络停住之后再让 Persistence 做最终 drain 和 flush，这样 VDL/metadata
    // 的最终状态才能和磁盘内容一致。
    persistence.stop();
    persistence.join();
    if (!failure) {
        failure = persistence.error();
    }
    return failure;
}

void cleanup_network_resources(CURLM* multi, std::vector<TransferHandle>& handles) noexcept {
    // 到这里网络层已经停止生产，所以可以安全销毁 easy/multi 资源。
    for (auto& handle : handles) {
        if (handle.easy != nullptr) {
            curl_easy_cleanup(handle.easy);
            handle.easy = nullptr;
        }
    }

    if (multi != nullptr) {
        curl_multi_cleanup(multi);
    }
}

[[nodiscard]] PerformanceSummary build_performance_summary(const core::SessionState& session,
                                                           const std::size_t ranges_total,
                                                           const Clock::time_point now) noexcept {
    PerformanceSummary summary{};
    summary.total_duration_ms = std::max<std::int64_t>(0,
        std::chrono::duration_cast<std::chrono::milliseconds>(now -
            session.task_started_at).count());
    summary.time_to_first_byte_ms = session.time_to_first_byte_ms.load(std::memory_order_relaxed);
    summary.time_to_first_persist_ms =
        session.time_to_first_persist_ms.load(std::memory_order_relaxed);
    const auto effective_downloaded = std::max<std::int64_t>(0,
        session.downloaded_bytes.load(std::memory_order_relaxed) - session.resume_reused_bytes);
    const auto effective_persisted = std::max<std::int64_t>(0,
        session.persisted_bytes.load(std::memory_order_relaxed) - session.resume_reused_bytes);
    if (summary.total_duration_ms > 0) {
        const auto seconds = static_cast<double>(summary.total_duration_ms) / 1000.0;
        summary.average_network_bytes_per_second =
            static_cast<double>(effective_downloaded) / seconds;
        summary.average_disk_bytes_per_second =
            static_cast<double>(effective_persisted) / seconds;
    }
    summary.peak_network_bytes_per_second = session.peak_network_bytes_per_second;
    summary.peak_disk_bytes_per_second = session.peak_disk_bytes_per_second;
    summary.resume_reused_bytes = session.resume_reused_bytes;
    summary.max_memory_bytes = session.max_memory_bytes.load(std::memory_order_relaxed);
    summary.max_inflight_bytes = session.max_inflight_bytes.load(std::memory_order_relaxed);
    summary.max_queued_packets = session.max_queued_packets.load(std::memory_order_relaxed);
    summary.max_active_requests = session.max_active_requests.load(std::memory_order_relaxed);
    summary.memory_pause_count = session.memory_pause_count.load(std::memory_order_relaxed);
    summary.queue_full_pause_count =
        session.queue_full_pause_count.load(std::memory_order_relaxed);
    summary.window_boundary_pause_count =
        session.window_boundary_pause_count.load(std::memory_order_relaxed);
    summary.gap_pause_count = session.gap_pause_count.load(std::memory_order_relaxed);
    summary.windows_total = session.windows_total.load(std::memory_order_relaxed);
    summary.ranges_total = ranges_total;
    summary.ranges_stolen = session.ranges_stolen.load(std::memory_order_relaxed);
    summary.write_callback_calls = session.write_callback_calls.load(std::memory_order_relaxed);
    summary.packets_enqueued_total =
        session.packets_enqueued_total.load(std::memory_order_relaxed);
    if (summary.packets_enqueued_total > 0) {
        summary.average_packet_size_bytes =
            static_cast<double>(effective_downloaded) /
            static_cast<double>(summary.packets_enqueued_total);
    }
    summary.max_packet_size_bytes =
        session.max_packet_size_bytes.load(std::memory_order_relaxed);
    summary.flush_count = session.flush_count.load(std::memory_order_relaxed);
    summary.flush_time_ms_total = session.flush_time_ms_total.load(std::memory_order_relaxed);
    summary.metadata_save_count =
        session.metadata_save_count.load(std::memory_order_relaxed);
    summary.metadata_save_time_ms_total =
        session.metadata_save_time_ms_total.load(std::memory_order_relaxed);
    return summary;
}

std::error_code finalize_storage_phase(storage::FileWriter& file_writer,
                                       metadata::MetadataStore& metadata_store,
                                       const core::SessionState& session,
                                       const core::AtomicBlockBitmap& bitmap,
                                       const bool overwrite_existing,
                                       std::error_code failure) noexcept {
    const auto completed_bytes = bitmap.contiguous_finished_bytes(session.options.block_size,
        session.total_size);
    if (!failure && completed_bytes >= session.total_size) {
        auto finalize_error = file_writer.finalize(session.paths.output_path, overwrite_existing);
        if (finalize_error) {
            return finalize_error;
        }

        const auto remove_metadata_error = metadata_store.remove();
        static_cast<void>(remove_metadata_error);
        return {};
    }

    // 如果任务失败，就保留 .part 和 metadata 供下次恢复使用，不在这里做破坏性清理。
    file_writer.close();
    if (!failure) {
        return make_error_code(DownloadErrc::http_transfer_failed);
    }

    return failure;
}

} // namespace

DownloadResult DownloadEngine::run(const DownloadRequest& request) noexcept {
    DownloadResult result{};
    const auto run_started = Clock::now();
    result.temporary_path = core::make_temporary_path(request.output_path);
    result.metadata_path = core::make_metadata_path(request.output_path);

    try {
        // 第一层先做最基础的请求合法性校验，避免后面创建网络和文件资源后再回滚。
        if (request.url.empty() || request.output_path.empty()) {
            result.error = make_error_code(DownloadErrc::invalid_request);
            return result;
        }

        // libcurl 的全局初始化只需要做一次，但这里仍通过轻量 RAII 包装保证
        // 当前进程在真正进入下载主链前已经具备可工作的网络环境。
        CurlGlobal curl_global;
        if (!curl_global.ok()) {
            result.error = make_error_code(DownloadErrc::http_init_failed);
            return result;
        }

        // 每次新任务开始前都重置全局内存会计，避免上一次任务异常退出后的残留统计
        // 污染当前这次背压判断。
        core::global_memory_accounting().reset();

        // 先做远端探测，拿到文件大小、Range 能力、ETag、Last-Modified。
        // 后面的恢复判定、分片调度和完整性校验都依赖这一步的结果。
        HttpProbe probe;
        const auto probe_result = probe.probe(request.url);
        if (probe_result.error) {
            result.error = probe_result.error;
            return result;
        }
        if (probe_result.total_size <= 0) {
            result.error = make_error_code(DownloadErrc::http_probe_failed);
            return result;
        }

        core::SessionState session{};
        session.paths.output_path = request.output_path;
        session.paths.temporary_path = core::make_temporary_path(request.output_path);
        session.paths.metadata_path = core::make_metadata_path(request.output_path);
        session.url = request.url;
        session.etag = probe_result.etag;
        session.last_modified = probe_result.last_modified;
        session.options = request.options;
        session.total_size = probe_result.total_size;
        session.accept_ranges = probe_result.accept_ranges;
        session.progress_callback = request.progress_callback;
        session.task_started_at = run_started;
        if (!session.accept_ranges) {
            // 不支持 Range 的服务端无法安全做多连接和窗口化调度，所以这里主动
            // 退化到单连接整文件下载，保证行为正确性优先。
            session.options.max_connections = 1;
            session.options.scheduler_window_bytes = static_cast<std::size_t>(session.total_size);
        }

        metadata::MetadataStore metadata_store(session.paths.metadata_path);
        storage::FileWriter file_writer;

        // 先读历史 metadata，再结合远端 probe 结果判断这次到底是恢复下载还是重新开始。
        const auto [metadata_error, loaded_metadata] = metadata_store.load();
        if (metadata_error) {
            result.error = metadata_error;
            result.performance = build_performance_summary(session, 0, Clock::now());
            return result;
        }

        const auto temp_exists = std::filesystem::exists(session.paths.temporary_path);
        const auto can_resume = temp_exists && loaded_metadata.has_value() &&
            metadata_matches(*loaded_metadata, request, probe_result, session.paths);

        // FileWriter 总是绑定到 .part 文件；若可以恢复则保留现有临时文件，
        // 否则按新任务语义重新打开并预分配。
        auto open_error = file_writer.open(session.paths.temporary_path,
            session.total_size,
            can_resume,
            request.options.overwrite_existing);
        if (open_error) {
            result.error = open_error;
            result.performance = build_performance_summary(session, 0, Clock::now());
            return result;
        }

        core::AtomicBlockBitmap bitmap(core::required_block_count(session.total_size,
            session.options.block_size));
        if (can_resume) {
            // 恢复路径的关键目标不是“完全信任旧状态”，而是先把旧状态还原成一个
            // 可验证的候选快照，再用 VDL 和 CRC 把不可信部分剔掉。
            bitmap.restore(loaded_metadata->bitmap_states);
            // DOWNLOADING 代表上次进程退出时还没形成可恢复的稳定状态，重启后必须
            // 先回到 EMPTY，再结合 VDL/CRC 重新判断哪些块可信。
            bitmap.reset_transient_states();
            rebuild_bitmap_from_snapshots(bitmap,
                loaded_metadata->ranges,
                session.options.block_size,
                session.total_size);
            const auto validation_error = validate_resumed_blocks(*loaded_metadata,
                file_writer,
                bitmap);
            if (validation_error) {
                result.error = validation_error;
                file_writer.close();
                result.performance = build_performance_summary(session, 0, Clock::now());
                return result;
            }

            session.resumed = true;
        } else {
            // 如果不能恢复，就把旧 metadata 清掉，避免后面把一个全新的下载任务
            // 和历史状态混在一起。
            const auto remove_metadata_error = metadata_store.remove();
            static_cast<void>(remove_metadata_error);
        }

        // 这里把“已经确认安全存在于磁盘上的字节数”重新投影回 SessionState，
        // 这样进度回调、背压显示和最终结果都会从正确的恢复点继续。
        const auto finished_bytes = sum_finished_bytes(bitmap,
            session.options.block_size,
            session.total_size);
        const auto safe_vdl = bitmap.contiguous_finished_bytes(session.options.block_size,
            session.total_size);
        session.resume_reused_bytes = finished_bytes;
        session.downloaded_bytes.store(finished_bytes, std::memory_order_relaxed);
        session.persisted_bytes.store(finished_bytes, std::memory_order_relaxed);
        session.vdl_offset.store(safe_vdl, std::memory_order_relaxed);

        if (safe_vdl >= session.total_size) {
            // 恢复后如果发现整个文件其实已经完整可靠，就直接 finalize，
            // 不再走任何网络或持久化线程。
            auto finalize_error = file_writer.finalize(session.paths.output_path,
                request.options.overwrite_existing);
            if (finalize_error) {
                result.error = finalize_error;
                return result;
            }

            const auto remove_metadata_error = metadata_store.remove();
            static_cast<void>(remove_metadata_error);
            result.total_bytes = session.total_size;
            result.downloaded_bytes = finished_bytes;
            result.persisted_bytes = finished_bytes;
            result.completed_ranges = bitmap.block_count();
            result.resumed = session.resumed;
            result.performance = build_performance_summary(session,
                bitmap.block_count(),
                Clock::now());
            return result;
        }

        // 调度器基于当前 bitmap 生成“还需要下载哪些区间”。
        // 对全新任务来说是整文件切片；对恢复任务来说则只会覆盖未完成区域。
        RangeScheduler scheduler(session.options, session.total_size, session.accept_ranges);
        auto ranges = scheduler.build_initial_ranges(bitmap);
        std::deque<core::RangeContext*> pending_ranges;
        for (const auto& range : ranges) {
            pending_ranges.push_back(range.get());
        }

        // 网络层只负责产出 DataPacket；真正写盘、重排、flush 和 metadata 更新
        // 全都交给独占的 PersistenceThread。
        moodycamel::BlockingConcurrentQueue<core::DataPacket> data_queue(
            session.options.queue_capacity_packets);
        BS::thread_pool<> workers(std::max<std::size_t>(1, session.options.max_connections));
        persistence::PersistenceThread persistence(session,
            data_queue,
            bitmap,
            file_writer,
            metadata_store,
            workers);
        for (const auto& range : ranges) {
            persistence.register_range(range.get());
        }
        persistence.start();

        // multi handle 统一承载所有 easy handle 的事件驱动；后面的主循环通过
        // curl_multi_perform + curl_multi_wait 实现非阻塞调度。
        CURLM* multi = curl_multi_init();
        if (multi == nullptr) {
            persistence.stop();
            persistence.join();
            file_writer.close();
            result.error = make_error_code(DownloadErrc::http_init_failed);
            result.performance = build_performance_summary(session, ranges.size(), Clock::now());
            return result;
        }

        curl_multi_setopt(multi, CURLMOPT_MAX_TOTAL_CONNECTIONS,
            static_cast<long>(session.options.max_connections));
        curl_multi_setopt(multi, CURLMOPT_MAX_HOST_CONNECTIONS,
            static_cast<long>(session.options.max_connections));

        std::vector<TransferHandle> handles(std::max<std::size_t>(1, session.options.max_connections));
        std::error_code failure;
        for (auto& handle : handles) {
            handle.session = &session;
            handle.data_queue = &data_queue;
            handle.easy = curl_easy_init();
            if (handle.easy == nullptr) {
                failure = make_error_code(DownloadErrc::http_init_failed);
                break;
            }
        }

        auto emit_progress_at = Clock::now();
        while (!failure && !session.stop_requested.load(std::memory_order_acquire)) {
            // 主循环开始时先看 Persistence 是否已经报错。写盘或 metadata 失败后，
            // 网络层必须尽快停止继续生产数据。
            if (const auto persistence_error = persistence.error(); persistence_error) {
                failure = persistence_error;
                session.stop_requested.store(true, std::memory_order_release);
                break;
            }

            // 空闲 handle 会优先消费 pending_ranges；如果暂时没有待派发 range，
            // 调度器再尝试从最大未分发尾部里做一次安全窃取。
            for (auto& handle : handles) {
                if (handle.range != nullptr || handle.in_multi || failure) {
                    continue;
                }

                while (true) {
                    if (pending_ranges.empty()) {
                        auto stolen = scheduler.steal_largest_range(ranges);
                        if (!stolen) {
                            break;
                        }

                        session.ranges_stolen.fetch_add(1, std::memory_order_relaxed);
                        persistence.register_range(stolen.get());
                        pending_ranges.push_back(stolen.get());
                        ranges.push_back(std::move(stolen));
                    }

                    auto* range = pending_ranges.front();
                    pending_ranges.pop_front();
                    if (range == nullptr || range->marked_finished.load(std::memory_order_acquire)) {
                        continue;
                    }

                    if (range->current_offset.load(std::memory_order_acquire) >
                        range->end_offset.load(std::memory_order_acquire)) {
                        // 某些 range 在调度阶段可能已经被逻辑推进到完成态，但还没来得及
                        // 通知 Persistence，这里补发 completion control packet。
                        if (!range->completion_notified.exchange(true, std::memory_order_acq_rel)) {
                            enqueue_control_packet(data_queue,
                                session,
                                core::PacketKind::range_complete,
                                range->range_id);
                        }
                        continue;
                    }

                    const auto arm_error = arm_transfer(handle, multi, session, *range, scheduler);
                    if (arm_error) {
                        failure = arm_error;
                        session.stop_requested.store(true, std::memory_order_release);
                    }
                    break;
                }
            }

            if (failure) {
                break;
            }

            // gap pause 和 memory backpressure 都是在事件循环里集中执行，避免在
            // write callback 里直接操作其他 handle，保持控制流简单。
            apply_gap_pauses(handles);
            apply_memory_backpressure(handles, session.options.backpressure_high_bytes);
            resume_paused_transfers(handles, session.options.backpressure_low_bytes);
            update_peak(session.max_active_requests,
                static_cast<std::size_t>(std::count_if(handles.begin(),
                    handles.end(),
                    [](const TransferHandle& handle) {
                        return handle.in_multi;
                    })));

            int running_handles = 0;
            const auto perform_status = curl_multi_perform(multi, &running_handles);
            if (perform_status != CURLM_OK) {
                failure = make_error_code(DownloadErrc::http_transfer_failed);
                session.stop_requested.store(true, std::memory_order_release);
                break;
            }

            int pending_messages = 0;
            while (auto* message = curl_multi_info_read(multi, &pending_messages)) {
                if (message->msg != CURLMSG_DONE) {
                    continue;
                }

                void* private_data = nullptr;
                curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &private_data);
                auto* transfer = static_cast<TransferHandle*>(private_data);
                if (transfer == nullptr) {
                    failure = make_error_code(DownloadErrc::internal_error);
                    session.stop_requested.store(true, std::memory_order_release);
                    break;
                }

                transfer->curl_result = message->data.result;
                curl_easy_getinfo(message->easy_handle, CURLINFO_RESPONSE_CODE,
                    &transfer->response_code);
                curl_multi_remove_handle(multi, message->easy_handle);
                transfer->in_multi = false;

                // 一个 window 请求完成后，不代表整个 range 完成。
                // finalize_completed_request 会决定是把 range 放回待调度队列，
                // 还是发出 range_complete 控制消息交给 Persistence 做最终收尾。
                const auto finalize_error = finalize_completed_request(*transfer,
                    data_queue,
                    session,
                    pending_ranges);
                if (finalize_error) {
                    failure = finalize_error;
                    session.stop_requested.store(true, std::memory_order_release);
                    break;
                }
            }

            const auto now = Clock::now();
            if (now >= emit_progress_at) {
                invoke_progress(session, ranges, handles);
                emit_progress_at = now + std::chrono::milliseconds(200);
            }

            // 只有当没有活动句柄且没有待派发 range 时，网络调度阶段才算真正完成。
            const auto has_active = std::any_of(handles.begin(), handles.end(),
                [](const TransferHandle& handle) {
                    return handle.in_multi || handle.range != nullptr;
                });
            if (!has_active && pending_ranges.empty()) {
                break;
            }

            int num_fds = 0;
            const auto wait_status = curl_multi_wait(multi, nullptr, 0, 100, &num_fds);
            if (wait_status != CURLM_OK) {
                failure = make_error_code(DownloadErrc::http_transfer_failed);
                session.stop_requested.store(true, std::memory_order_release);
                break;
            }
        }

        // 收尾严格按“先停网络、再停持久化、最后 finalize 文件”的顺序执行，
        // 这样 VDL、bitmap 和磁盘内容才能在退出时保持一致。
        stop_network_phase(session, multi, handles);
        failure = stop_persistence_phase(persistence, failure);

        // Persistence 线程拥有每个 range 的真正写盘前沿；停下来之后再做一次 bitmap
        // 重建，可以把最终结果对齐到落盘状态。
        rebuild_bitmap_from_ranges(bitmap,
            ranges,
            session.options.block_size,
            session.total_size);

        invoke_progress(session, ranges, handles);

        cleanup_network_resources(multi, handles);
        failure = finalize_storage_phase(file_writer,
            metadata_store,
            session,
            bitmap,
            request.options.overwrite_existing,
            failure);

        // DownloadResult 主要面向调用方总结最终状态，因此在这里统一从 session 和
        // bitmap 回填一次，避免中途多个分支各自维护结果对象。
        result.error = failure;
        result.total_bytes = session.total_size;
        result.downloaded_bytes = session.downloaded_bytes.load(std::memory_order_relaxed);
        result.persisted_bytes = sum_finished_bytes(bitmap,
            session.options.block_size,
            session.total_size);
        result.completed_ranges = static_cast<std::size_t>(std::count_if(ranges.begin(),
            ranges.end(),
            [](const std::unique_ptr<core::RangeContext>& range) {
                return range->marked_finished.load(std::memory_order_acquire);
            }));
        result.resumed = session.resumed;
        result.temporary_path = session.paths.temporary_path;
        result.metadata_path = session.paths.metadata_path;
        result.performance = build_performance_summary(session, ranges.size(), Clock::now());
        return result;
    } catch (...) {
        // 对外契约是不抛异常，所以任何未预期错误最终都折叠成 internal_error。
        result.error = make_error_code(DownloadErrc::internal_error);
        return result;
    }
}

} // namespace asyncdownload::download





