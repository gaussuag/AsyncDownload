#include "download_engine.hpp"

#include "asyncdownload/error.hpp"
#include "core/block_bitmap.hpp"
#include "core/crc32.hpp"
#include "core/models.hpp"
#include "core/path_utils.hpp"
#include "download/download_policy.hpp"
#include "download/http_probe.hpp"
#include "download/range_scheduler.hpp"
#include "flow/packet_flow.hpp"
#include "metadata/metadata_store.hpp"
#include "persistence/persistence_thread.hpp"
#include "range/range_lifecycle.hpp"
#include "storage/file_writer.hpp"

#include <thread-pool/BS_thread_pool.hpp>

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace asyncdownload::download {
namespace {

using Clock = std::chrono::steady_clock;
struct ExpectedGeometryAck {
    persistence::RangeRegistrationTicket ticket = 0;
    range::RangeId range{};
    std::uint64_t geometry_revision = 0;
    bool received = false;
};

struct PendingLease {
    range::RangeLease lease{};
    std::array<ExpectedGeometryAck, 2> acks{};
    std::size_t ack_count = 0;
};

struct TransferHandle {
    // TransferHandle 代表一个可复用的 easy handle 槽位。
    // 它既保存 libcurl 句柄，也保存当前绑定到哪个 range/window，以及暂停原因、
    // 响应码、速度等运行期状态。
    core::SessionState* session = nullptr;
    flow::PacketProducer* packet_producer = nullptr;
    flow::ProducerLane packet_lane;
    FlowControlPolicy flow_control{};
    CURL* easy = nullptr;
    core::RangeContext* range = nullptr;
    std::optional<range::RangeLease> lease;
    std::optional<PendingLease> pending_lease;
    std::string range_header;
    std::int64_t request_start = 0;
    std::int64_t request_end = -1;
    std::int64_t next_offset = 0;
    std::int64_t request_bytes = 0;
    long response_code = 0;
    double speed_bytes_per_second = 0.0;
    bool in_multi = false;
    bool paused_by_gap = false;
    bool paused_by_window_boundary = false;
    CURLcode curl_result = CURLE_OK;
    std::error_code packet_error;
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

void mark_range_status(core::RangeContext& range, const core::RangeStatus status) noexcept;

void update_transfer_pause_state(TransferHandle& transfer) noexcept {
    if (transfer.range == nullptr) {
        return;
    }

    const auto packet_paused =
        transfer.packet_producer != nullptr &&
        transfer.packet_producer->paused(transfer.packet_lane);
    if (packet_paused ||
        transfer.paused_by_gap ||
        transfer.paused_by_window_boundary) {
        mark_range_status(*transfer.range, core::RangeStatus::paused);
        return;
    }

    mark_range_status(*transfer.range, core::RangeStatus::downloading);
}

void record_first_network_byte(core::SessionState& session,
                               const std::size_t bytes) noexcept {
    if (bytes == 0) {
        return;
    }

    session.telemetry_session_.record_first_byte_received();
}

[[nodiscard]] std::optional<std::int64_t> merged_downloaded_bytes(
    const core::SessionState& session,
    const flow::PacketFlowSnapshot& flow_snapshot) noexcept {
    if (session.recovery_initial_trusted_bytes < 0 ||
        flow_snapshot.published_data_bytes >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max() -
                session.recovery_initial_trusted_bytes)) {
        return std::nullopt;
    }
    return session.recovery_initial_trusted_bytes +
        static_cast<std::int64_t>(
            flow_snapshot.published_data_bytes);
}

void invoke_progress(core::SessionState& session,
                     const std::vector<std::unique_ptr<core::RangeContext>>& ranges,
                     const std::vector<TransferHandle>& handles) noexcept {
    if (!session.progress_callback) {
        return;
    }

    auto snapshot = session.telemetry_session_.current_snapshot();
    const auto flow_snapshot = handles.empty() ||
            handles.front().packet_producer == nullptr ?
        flow::PacketFlowSnapshot{} :
        handles.front().packet_producer->snapshot();
    snapshot.total_bytes = session.total_size;
    snapshot.downloaded_bytes =
        merged_downloaded_bytes(session, flow_snapshot).value_or(
            std::numeric_limits<std::int64_t>::max());
    snapshot.persisted_bytes = session.persisted_bytes.load(std::memory_order_relaxed);
    snapshot.vdl_offset = session.vdl_offset.load(std::memory_order_relaxed);
    snapshot.inflight_bytes = std::max<std::int64_t>(0,
        snapshot.downloaded_bytes - snapshot.persisted_bytes);
    snapshot.queued_packets = flow_snapshot.queued_packets;
    snapshot.memory_bytes = flow_snapshot.accounted_bytes;
    snapshot.resumed = session.resumed;

    for (const auto& range : ranges) {
        const auto status = static_cast<core::RangeStatus>(
            range->status.load(std::memory_order_acquire));
        if (status == core::RangeStatus::paused) {
            ++snapshot.paused_ranges;
        }
    }

    snapshot.active_requests = static_cast<std::size_t>(std::count_if(handles.begin(),
        handles.end(),
        [](const TransferHandle& handle) {
            return handle.in_multi;
        }));

    try {
        session.progress_callback(snapshot);
    } catch (...) {
    }
}

[[nodiscard]] bool metadata_matches(const core::MetadataState& state,
                                    const std::string& url,
                                    const RecoveryIdentityPolicy& policy,
                                    const core::RemoteProbeResult& probe,
                                    const core::SessionPaths& paths) noexcept {
    // 恢复条件必须同时满足“本地任务身份一致”和“远端资源身份一致”。
    // 只要 URL、路径、总大小、对齐参数、ETag、Last-Modified 有冲突，就宁可重下，
    // 也不冒险把旧状态接到新资源上。
    if (state.url != url ||
        state.output_path != paths.output_path ||
        state.temporary_path != paths.temporary_path ||
        state.total_size != probe.total_size) {
        return false;
    }

    if (state.block_size != policy.block_bytes ||
        state.io_alignment != policy.io_alignment_bytes) {
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

[[nodiscard]] bool metadata_proves_complete(
    const core::MetadataState& state,
    const RecoveryIdentityPolicy& policy) noexcept {
    if (state.total_size <= 0 ||
        state.vdl_offset < state.total_size) {
        return false;
    }

    const auto expected_blocks = core::required_block_count(
        state.total_size,
        policy.block_bytes);
    return state.bitmap_states.size() == expected_blocks &&
        std::all_of(
            state.bitmap_states.begin(),
            state.bitmap_states.end(),
            [](const std::uint8_t value) {
                return value ==
                    static_cast<std::uint8_t>(core::BlockState::finished);
            });
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

[[nodiscard]] core::RangeContext* find_range_projection(
    const std::vector<std::unique_ptr<core::RangeContext>>& ranges,
    const range::RangeId id) noexcept {
    if (id.value > std::numeric_limits<std::size_t>::max()) {
        return nullptr;
    }
    const auto found = std::find_if(
        ranges.begin(),
        ranges.end(),
        [id](const auto& candidate) {
            return candidate != nullptr &&
                candidate->range_id ==
                    static_cast<std::size_t>(id.value);
        });
    return found == ranges.end() ? nullptr : found->get();
}

[[nodiscard]] std::error_code apply_geometry_effects(
    const range::EffectBatch<2>& effects,
    std::vector<std::unique_ptr<core::RangeContext>>& ranges,
    persistence::PersistenceThread& persistence) noexcept {
    try {
        for (std::size_t index = 0;
             index < effects.size;
             ++index) {
            if (const auto* resize =
                    std::get_if<range::ResizeRangeEffect>(
                        &effects.values[index])) {
                auto* projection =
                    find_range_projection(ranges, resize->range);
                if (projection == nullptr ||
                    resize->new_end <= projection->start_offset) {
                    return make_error_code(
                        DownloadErrc::internal_error);
                }
                projection->end_offset.store(
                    resize->new_end - 1,
                    std::memory_order_release);
                continue;
            }

            const auto* registration =
                std::get_if<range::RegisterRangeEffect>(
                    &effects.values[index]);
            if (registration == nullptr ||
                registration->range.value >
                    std::numeric_limits<std::size_t>::max() ||
                registration->bytes.begin >=
                    registration->bytes.end ||
                find_range_projection(
                    ranges,
                    registration->range) != nullptr) {
                return make_error_code(
                    DownloadErrc::internal_error);
            }
            auto projection =
                std::make_unique<core::RangeContext>(
                    static_cast<std::size_t>(
                        registration->range.value),
                    registration->bytes.begin,
                    registration->bytes.end - 1);
            persistence.register_range(projection.get());
            ranges.push_back(std::move(projection));
        }
        return {};
    } catch (const std::bad_alloc&) {
        return std::make_error_code(std::errc::not_enough_memory);
    } catch (...) {
        return make_error_code(DownloadErrc::internal_error);
    }
}

[[nodiscard]] std::error_code submit_geometry_effect(
    const range::RangeEffect& effect,
    persistence::PersistenceThread& persistence,
    ExpectedGeometryAck& expected) noexcept {
    persistence::RangeRegistrationSubmitResult submitted;
    if (const auto* registration =
            std::get_if<range::RegisterRangeEffect>(&effect)) {
        submitted = persistence.submit_range_geometry(
            persistence::RangeGeometryCommand{*registration});
        expected.range = registration->range;
        expected.geometry_revision =
            registration->geometry_revision;
    } else if (const auto* resize =
                   std::get_if<range::ResizeRangeEffect>(&effect)) {
        submitted = persistence.submit_range_geometry(
            persistence::RangeGeometryCommand{*resize});
        expected.range = resize->range;
        expected.geometry_revision =
            resize->geometry_revision;
    } else {
        return make_error_code(DownloadErrc::internal_error);
    }
    if (submitted.error || submitted.ticket == 0) {
        return submitted.error ?
            submitted.error :
            make_error_code(DownloadErrc::internal_error);
    }
    expected.ticket = submitted.ticket;
    return {};
}

[[nodiscard]] std::error_code apply_geometry_ack(
    const persistence::RangeRegistrationAck& ack,
    std::span<ExpectedGeometryAck> expected) noexcept {
    const auto found = std::find_if(
        expected.begin(),
        expected.end(),
        [&ack](const auto& candidate) {
            return candidate.ticket == ack.ticket;
        });
    if (found == expected.end() ||
        found->received ||
        found->range != ack.range ||
        found->geometry_revision != ack.geometry_revision ||
        ack.error) {
        return ack.error ?
            ack.error :
            make_error_code(DownloadErrc::internal_error);
    }
    found->received = true;
    return {};
}

[[nodiscard]] bool all_geometry_acks_received(
    const std::span<const ExpectedGeometryAck> expected) noexcept {
    return std::all_of(
        expected.begin(),
        expected.end(),
        [](const auto& ack) {
            return ack.received;
        });
}

[[nodiscard]] std::error_code wait_for_geometry_acks(
    persistence::PersistenceThread& persistence,
    std::vector<ExpectedGeometryAck>& expected) noexcept {
    while (!all_geometry_acks_received(expected)) {
        const auto polled =
            persistence.poll_range_geometry_ack();
        if (polled.error) {
            return polled.error;
        }
        if (polled.ack.has_value()) {
            if (const auto error = apply_geometry_ack(
                    *polled.ack,
                    expected);
                error) {
                return error;
            }
            continue;
        }
        if (const auto error = persistence.error(); error) {
            return error;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }
    return {};
}

[[nodiscard]] std::error_code drain_geometry_acks(
    persistence::PersistenceThread& persistence,
    std::vector<TransferHandle>& handles) noexcept {
    while (true) {
        const auto polled =
            persistence.poll_range_geometry_ack();
        if (polled.error) {
            return polled.error;
        }
        if (!polled.ack.has_value()) {
            return {};
        }

        bool matched = false;
        for (auto& handle : handles) {
            if (!handle.pending_lease.has_value()) {
                continue;
            }
            auto expected = std::span<ExpectedGeometryAck>(
                handle.pending_lease->acks.data(),
                handle.pending_lease->ack_count);
            const auto found = std::find_if(
                expected.begin(),
                expected.end(),
                [&polled](const auto& candidate) {
                    return candidate.ticket ==
                        polled.ack->ticket;
                });
            if (found == expected.end()) {
                continue;
            }
            matched = true;
            if (const auto error =
                    apply_geometry_ack(
                        *polled.ack,
                        expected);
                error) {
                return error;
            }
            break;
        }
        if (!matched) {
            return make_error_code(
                DownloadErrc::internal_error);
        }
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
    if (transfer.range == nullptr ||
        !transfer.lease.has_value()) {
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

[[nodiscard]] std::error_code publish_range_complete(
    flow::PacketProducer& producer,
    const range::PublishRangeCompleteEffect& effect) noexcept {
    if (effect.expected_end <= 0) {
        return std::make_error_code(std::errc::invalid_argument);
    }
    const auto published = producer.publish({
        flow::ControlPacketKind::range_complete,
        effect.completion,
        effect.expected_end
    });
    if (published.code != flow::PacketPublishCode::published) {
        return published.error ?
            published.error :
            make_error_code(DownloadErrc::internal_error);
    }
    return {};
}

void mark_range_status(core::RangeContext& range, const core::RangeStatus status) noexcept {
    range.status.store(static_cast<std::uint8_t>(status), std::memory_order_release);
}

[[nodiscard]] bool response_is_valid(const core::SessionState& session,
                                     const TransferHandle& transfer) noexcept {
    // Range 模式下，除非这个请求覆盖整个文件，否则必须看到 206。
    // 否则服务端可能忽略了 Range 头，继续下载会把数据布局全部打乱。
    if (session.effective_policy.scheduling().issue_range_requests) {
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

[[nodiscard]] flow::PacketAdmission flush_packet_lane(
    TransferHandle& transfer) noexcept {
    auto result =
        transfer.packet_producer->flush(transfer.packet_lane);
    if (result.code == flow::PacketAdmissionCode::failed) {
        transfer.packet_error = result.error;
    } else if (result.code == flow::PacketAdmissionCode::closed) {
        transfer.packet_error =
            make_error_code(DownloadErrc::internal_error);
    }
    return result;
}

[[nodiscard]] std::error_code drain_packet_lane(
    TransferHandle& transfer) noexcept {
    while (true) {
        const auto flushed = flush_packet_lane(transfer);
        if (flushed.accepted()) {
            return {};
        }
        if (flushed.code == flow::PacketAdmissionCode::failed ||
            flushed.code == flow::PacketAdmissionCode::closed) {
            return transfer.packet_error ?
                transfer.packet_error :
                make_error_code(DownloadErrc::internal_error);
        }
        const auto snapshot =
            transfer.packet_producer->snapshot();
        if (snapshot.state == flow::PacketFlowState::failed) {
            return snapshot.error ?
                snapshot.error :
                make_error_code(DownloadErrc::internal_error);
        }
        if (snapshot.state != flow::PacketFlowState::open) {
            return make_error_code(DownloadErrc::internal_error);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

[[nodiscard]] std::error_code arm_transfer(TransferHandle& transfer,
                                           CURLM* multi,
                                           const core::SessionState& session,
                                           core::RangeContext& projection,
                                           range::RangeLease lease) noexcept {
    if (lease.bytes.begin >= lease.bytes.end) {
        transfer.range = nullptr;
        return {};
    }

    // 派发 window 前先把逻辑租约登记到 range.current_offset。
    // 即使后面发生失败，也可以借助 rollback_inflight_window 把这段租约收回。
    projection.current_offset.store(
        lease.bytes.end,
        std::memory_order_release);
    mark_range_status(
        projection,
        core::RangeStatus::downloading);

    transfer.range = &projection;
    transfer.lease = lease;
    transfer.request_start = lease.bytes.begin;
    transfer.request_end = lease.bytes.end - 1;
    transfer.next_offset = lease.bytes.begin;
    transfer.request_bytes = 0;
    transfer.response_code = 0;
    transfer.speed_bytes_per_second = 0.0;
    transfer.paused_by_gap = false;
    transfer.paused_by_window_boundary = false;
    transfer.curl_result = CURLE_OK;
    transfer.packet_error = {};
    transfer.request_started = Clock::now();
    transfer.range_header.clear();

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
        if (bytes == 0 ||
            current == nullptr ||
            current->range == nullptr ||
            !current->lease.has_value()) {
            return 0;
        }

        // stop_requested 表示主线程已经决定收尾，回调这里直接返回失败，
        // 让 libcurl 尽快结束该请求。
        if (current->session->stop_requested.load(std::memory_order_acquire)) {
            return 0;
        }

        const auto remaining = current->request_end - current->next_offset + 1;
        if (remaining <= 0) {
            const auto flushed = flush_packet_lane(*current);
            if (!flushed.accepted()) {
                if (current->packet_error) {
                    return 0;
                }
                update_transfer_pause_state(*current);
                return CURL_WRITEFUNC_PAUSE;
            }

            // 如果服务端继续往回调里塞数据，但逻辑 window 已经没有剩余额度，
            // 就暂停接收，避免把后续字节算进错误的区间。
            if (!current->paused_by_window_boundary) {
                current->session->telemetry_session_.record_pause(
                    telemetry::TelemetryPauseReason::none, false);
            }
            current->paused_by_window_boundary = true;
            update_transfer_pause_state(*current);
            return CURL_WRITEFUNC_PAUSE;
        }

        const auto allowed = std::min<std::size_t>(bytes, static_cast<std::size_t>(remaining));
        if (allowed != bytes) {
            // window 化调度要求一个请求只能覆盖分配给它的那段字节。
            // 只要回调给出的数据超出当前 window，就把这次传输视为异常。
            return 0;
        }

        if (current->request_end == std::numeric_limits<std::int64_t>::max()) {
            current->packet_error =
                std::make_error_code(std::errc::invalid_argument);
            return 0;
        }
        const auto admission = current->packet_producer->accept(
            current->packet_lane,
            {
                current->lease->id,
                current->lease->bytes,
                current->next_offset,
                std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t*>(data),
                    allowed)
            });
        if (!admission.accepted()) {
            if (admission.code ==
                flow::PacketAdmissionCode::memory_budget_exhausted) {
                update_transfer_pause_state(*current);
                return CURL_WRITEFUNC_PAUSE;
            }
            if (admission.code ==
                    flow::PacketAdmissionCode::packet_budget_exhausted ||
                admission.code ==
                    flow::PacketAdmissionCode::backend_temporarily_unavailable) {
                update_transfer_pause_state(*current);
                return CURL_WRITEFUNC_PAUSE;
            }
            current->packet_error = admission.error ?
                admission.error :
                make_error_code(DownloadErrc::internal_error);
            return 0;
        }
        if ((admission.active_pause_mask &
             static_cast<std::uint8_t>(
                 flow::PacketPauseReason::memory)) != 0) {
            update_transfer_pause_state(*current);
        }
        if ((admission.active_pause_mask &
             static_cast<std::uint8_t>(
                 flow::PacketPauseReason::queue)) != 0) {
            update_transfer_pause_state(*current);
        }

        record_first_network_byte(*current->session, allowed);
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

    if (session.effective_policy.scheduling().issue_range_requests) {
        transfer.range_header = std::to_string(transfer.request_start) + "-" +
            std::to_string(transfer.request_end);
        curl_easy_setopt(transfer.easy, CURLOPT_RANGE, transfer.range_header.c_str());
    }

    if (curl_multi_add_handle(multi, transfer.easy) != CURLM_OK) {
        transfer.range = nullptr;
        transfer.lease.reset();
        return make_error_code(DownloadErrc::http_transfer_failed);
    }

    transfer.in_multi = true;
    return {};
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
            handle.session->telemetry_session_.record_pause(
                telemetry::TelemetryPauseReason::gap, false);
            handle.paused_by_gap = true;
            update_transfer_pause_state(handle);
            curl_easy_pause(handle.easy, CURLPAUSE_RECV);
        } else if (!pause_for_gap && handle.paused_by_gap) {
            handle.paused_by_gap = false;
            if (!handle.packet_producer->paused(
                    handle.packet_lane) &&
                !handle.paused_by_window_boundary) {
                update_transfer_pause_state(handle);
                curl_easy_pause(handle.easy, CURLPAUSE_CONT);
            }
        }
    }
}

[[nodiscard]] std::error_code reconcile_packet_flow(
    std::vector<TransferHandle>& handles,
    std::vector<flow::PacketLaneObservation>& observations,
    std::vector<flow::PacketPauseAction>& actions) noexcept {
    observations.clear();
    for (auto& handle : handles) {
        if (!handle.in_multi || handle.range == nullptr) {
            continue;
        }
        const auto eligible = !handle.paused_by_window_boundary;
        if (eligible) {
            update_speed(handle);
        }
        observations.push_back({
            handle.packet_lane.id(),
            handle.speed_bytes_per_second,
            eligible
        });
    }
    if (observations.empty()) {
        return {};
    }
    actions.resize(observations.size());
    const auto result = handles.front().packet_producer->reconcile(
        observations, actions);
    if (result.error) {
        return result.error;
    }
    for (std::size_t index = 0;
         index < result.action_count;
         ++index) {
        const auto& action = actions[index];
        const auto found = std::find_if(
            handles.begin(),
            handles.end(),
            [&action](const TransferHandle& handle) {
                return handle.packet_lane.id() == action.lane_id;
            });
        if (found == handles.end() ||
            !found->in_multi ||
            found->range == nullptr) {
            return make_error_code(DownloadErrc::internal_error);
        }
        update_transfer_pause_state(*found);
        if (action.kind ==
            flow::PacketPauseActionKind::pause_receive) {
            curl_easy_pause(found->easy, CURLPAUSE_RECV);
            continue;
        }
        if (!found->paused_by_gap &&
            !found->paused_by_window_boundary &&
            !found->packet_producer->paused(
                found->packet_lane)) {
            curl_easy_pause(found->easy, CURLPAUSE_CONT);
        }
    }
    return {};
}

[[nodiscard]] std::error_code finalize_completed_request(
    TransferHandle& transfer,
    core::SessionState& session,
    range::RangeLifecycle& lifecycle) noexcept {
    if (transfer.range == nullptr ||
        !transfer.lease.has_value()) {
        return {};
    }

    const auto fail_lease =
        [&transfer, &lifecycle](
            const std::error_code error) noexcept {
            rollback_inflight_window(transfer);
            mark_range_status(
                *transfer.range,
                core::RangeStatus::failed);
            const auto applied = lifecycle.apply(
                range::LeaseFailed{
                    transfer.lease->id,
                    transfer.next_offset,
                    error
                });
            return applied.error ? applied.error : error;
        };

    // 这里处理的是“一个 HTTP window 请求结束了”，不是“整个下载任务结束了”。
    // 所以它既负责校验这次请求，也负责决定后续应该继续调度还是宣告 range 完成。
    update_speed(transfer);
    if (const auto flush_error = drain_packet_lane(transfer); flush_error) {
        return fail_lease(flush_error);
    }

    if (transfer.curl_result != CURLE_OK) {
        return fail_lease(
            make_error_code(DownloadErrc::http_transfer_failed));
    }

    if (!response_is_valid(session, transfer)) {
        return fail_lease(
            make_error_code(DownloadErrc::http_invalid_response));
    }

    if (transfer.next_offset <= transfer.request_end) {
        return fail_lease(
            make_error_code(DownloadErrc::http_transfer_failed));
    }

    transfer.range->pause_for_gap.store(false, std::memory_order_release);
    transfer.paused_by_window_boundary = false;
    update_transfer_pause_state(transfer);

    const auto applied = lifecycle.apply(
        range::LeaseSucceeded{
            transfer.lease->id,
            transfer.next_offset
        });
    if (applied.error) {
        mark_range_status(
            *transfer.range,
            core::RangeStatus::failed);
        return applied.error;
    }
    if (applied.scheduler_may_run) {
        mark_range_status(*transfer.range, core::RangeStatus::empty);
    }
    for (std::size_t index = 0;
         index < applied.effects.size;
         ++index) {
        const auto* completion =
            std::get_if<range::PublishRangeCompleteEffect>(
                &applied.effects.values[index]);
        if (completion == nullptr) {
            return make_error_code(DownloadErrc::internal_error);
        }
        const auto publish_error = publish_range_complete(
            *transfer.packet_producer,
            *completion);
        if (publish_error) {
            return publish_error;
        }
    }

    transfer.range = nullptr;
    transfer.lease.reset();
    transfer.pending_lease.reset();
    transfer.range_header.clear();
    transfer.paused_by_gap = false;
    transfer.paused_by_window_boundary = false;
    return {};
}

void release_transfer(CURLM* multi, TransferHandle& transfer) noexcept {
    if (transfer.in_multi) {
        curl_multi_remove_handle(multi, transfer.easy);
        transfer.in_multi = false;
    }

    transfer.paused_by_window_boundary = false;
    // easy handle 会被复用给下一个 range/window，因此这里只清运行期状态，
    // 不销毁底层 easy 对象本身。
    transfer.range = nullptr;
    transfer.lease.reset();
    transfer.pending_lease.reset();
    transfer.range_header.clear();
    transfer.paused_by_gap = false;
    transfer.paused_by_window_boundary = false;
}

std::error_code stop_network_phase(core::SessionState& session,
                                   CURLM* multi,
                                   std::vector<TransferHandle>& handles) noexcept {
    // 退出红线的第一步是先停网络生产，避免 Persistence 在 drain 队列时又收到
    // 新数据，从而把收尾阶段拉回“边消费边生产”的竞态。
    session.stop_requested.store(true, std::memory_order_release);
    std::error_code first_error;
    for (auto& handle : handles) {
        if (handle.packet_producer == nullptr ||
            handle.packet_lane.id() == 0) {
            rollback_inflight_window(handle);
            release_transfer(multi, handle);
            continue;
        }
        const auto flush_error = drain_packet_lane(handle);
        if (flush_error && !first_error) {
            first_error = flush_error;
        }
        if (flush_error) {
            const auto discard_error =
                handle.packet_producer->discard(
                    handle.packet_lane);
            if (!first_error && discard_error) {
                first_error = discard_error;
            }
        }

        rollback_inflight_window(handle);
        release_transfer(multi, handle);
    }

    return first_error;
}

std::error_code stop_persistence_phase(flow::PacketProducer& producer,
                                       persistence::PersistenceThread& persistence,
                                       std::error_code failure) noexcept {
    // 网络停住之后再让 Persistence 做最终 drain 和 flush，这样 VDL/metadata
    // 的最终状态才能和磁盘内容一致。
    const auto close_error = producer.close();
    if (!failure && close_error) {
        failure = close_error;
    }
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
                                                           const Clock::time_point now) noexcept {
    return session.telemetry_session_.final_summary(now);
}

std::error_code finalize_storage_phase(storage::FileWriter& file_writer,
                                       metadata::MetadataStore& metadata_store,
                                       const core::SessionState& session,
                                       const core::AtomicBlockBitmap& bitmap,
                                       std::error_code failure) noexcept {
    const auto completed_bytes = bitmap.contiguous_finished_bytes(
        session.effective_policy.persistence().block_bytes,
        session.total_size);
    if (!failure && completed_bytes >= session.total_size) {
        auto finalize_error = file_writer.finalize(
            session.paths.output_path,
            session.effective_policy.persistence().overwrite_existing);
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
    result.temporary_path = core::make_temporary_path(request.output_path);
    result.metadata_path = core::make_metadata_path(request.output_path);

    try {
        // 第一层先做最基础的请求合法性校验，避免后面创建网络和文件资源后再回滚。
        if (request.url.empty() || request.output_path.empty()) {
            result.error = make_error_code(DownloadErrc::invalid_request);
            return result;
        }

        const auto validated_policy =
            validate_download_options(request.options);
        if (!validated_policy.ok()) {
            result.error = validated_policy.failure.error;
            return result;
        }

        const auto run_started = Clock::now();
        // libcurl 的全局初始化只需要做一次，但这里仍通过轻量 RAII 包装保证
        // 当前进程在真正进入下载主链前已经具备可工作的网络环境。
        CurlGlobal curl_global;
        if (!curl_global.ok()) {
            result.error = make_error_code(DownloadErrc::http_init_failed);
            return result;
        }

        // 先做远端探测，拿到文件大小、Range 能力、ETag、Last-Modified。
        // 后面的恢复判定、分片调度和完整性校验都依赖这一步的结果。
        HttpProbe probe;
        const auto probe_result = probe.probe(request.url);
        if (probe_result.error) {
            result.error = probe_result.error;
            return result;
        }
        const auto effective_policy_result = bind_remote_facts(
            *validated_policy.value,
            RemoteObjectFacts{
                probe_result.total_size,
                probe_result.accept_ranges
            });
        if (!effective_policy_result.ok()) {
            result.error = effective_policy_result.failure.error;
            return result;
        }
        core::SessionState session(
            std::move(*effective_policy_result.value));
        session.paths.output_path = request.output_path;
        session.paths.temporary_path = core::make_temporary_path(request.output_path);
        session.paths.metadata_path = core::make_metadata_path(request.output_path);
        session.url = request.url;
        session.etag = probe_result.etag;
        session.last_modified = probe_result.last_modified;
        session.progress_callback = request.progress_callback;
        session.task_started_at = run_started;
        session.telemetry_session_.record_task_started(run_started);

        metadata::MetadataStore metadata_store(session.paths.metadata_path);
        storage::FileWriter file_writer;

        // 先读历史 metadata，再结合远端 probe 结果判断这次到底是恢复下载还是重新开始。
        const auto [metadata_error, loaded_metadata] = metadata_store.load();
        if (metadata_error) {
            result.error = metadata_error;
            result.performance = build_performance_summary(session, Clock::now());
            return result;
        }

        const auto temp_exists =
            std::filesystem::exists(session.paths.temporary_path);
        auto can_resume = false;
        if (temp_exists &&
            loaded_metadata.has_value() &&
            metadata_matches(
                *loaded_metadata,
                request.url,
                session.effective_policy.recovery_identity(),
                probe_result,
                session.paths)) {
            can_resume =
                session.effective_policy.recovery_identity().
                    allow_sparse_resume ||
                metadata_proves_complete(
                    *loaded_metadata,
                    session.effective_policy.recovery_identity());
        }

        // FileWriter 总是绑定到 .part 文件；若可以恢复则保留现有临时文件，
        // 否则按新任务语义重新打开并预分配。
        auto open_error = file_writer.open(session.paths.temporary_path,
            session.total_size,
            can_resume,
            session.effective_policy.persistence().overwrite_existing);
        if (open_error) {
            result.error = open_error;
            result.performance = build_performance_summary(session, Clock::now());
            return result;
        }

        core::AtomicBlockBitmap bitmap(core::required_block_count(session.total_size,
            session.effective_policy.persistence().block_bytes));
        if (can_resume) {
            // 恢复路径的关键目标不是“完全信任旧状态”，而是先把旧状态还原成一个
            // 可验证的候选快照，再用 VDL 和 CRC 把不可信部分剔掉。
            bitmap.restore(loaded_metadata->bitmap_states);
            // DOWNLOADING 代表上次进程退出时还没形成可恢复的稳定状态，重启后必须
            // 先回到 EMPTY，再结合 VDL/CRC 重新判断哪些块可信。
            bitmap.reset_transient_states();
            rebuild_bitmap_from_snapshots(bitmap,
                loaded_metadata->ranges,
                session.effective_policy.persistence().block_bytes,
                session.total_size);
            const auto validation_error = validate_resumed_blocks(*loaded_metadata,
                file_writer,
                bitmap);
            if (validation_error) {
                result.error = validation_error;
                file_writer.close();
                result.performance = build_performance_summary(session, Clock::now());
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
            session.effective_policy.persistence().block_bytes,
            session.total_size);
        const auto safe_vdl = bitmap.contiguous_finished_bytes(
            session.effective_policy.persistence().block_bytes,
            session.total_size);
        session.recovery_initial_trusted_bytes = finished_bytes;
        session.persisted_bytes.store(finished_bytes, std::memory_order_relaxed);
        session.vdl_offset.store(safe_vdl, std::memory_order_relaxed);

        if (safe_vdl >= session.total_size) {
            // 恢复后如果发现整个文件其实已经完整可靠，就直接 finalize，
            // 不再走任何网络或持久化线程。
            auto finalize_error = file_writer.finalize(session.paths.output_path,
                session.effective_policy.persistence().overwrite_existing);
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
            session.telemetry_session_.record_task_completed(Clock::now());
            result.performance = build_performance_summary(session, Clock::now());
            return result;
        }

        // 调度器基于当前 bitmap 生成“还需要下载哪些区间”。
        // 对全新任务来说是整文件切片；对恢复任务来说则只会覆盖未完成区域。
        RangeScheduler scheduler(
            session.effective_policy.scheduling(),
            session.total_size);
        const auto initial_plan = scheduler.plan_initial(bitmap);
        if (initial_plan.error || initial_plan.ranges.empty()) {
            file_writer.close();
            result.error = initial_plan.error ?
                initial_plan.error :
                make_error_code(DownloadErrc::internal_error);
            result.performance =
                build_performance_summary(session, Clock::now());
            return result;
        }
        auto lifecycle_creation = range::RangeLifecycle::create(
            session.total_size,
            session.effective_policy.scheduling(),
            initial_plan.ranges);
        if (lifecycle_creation.error ||
            lifecycle_creation.value == nullptr) {
            file_writer.close();
            result.error = lifecycle_creation.error ?
                lifecycle_creation.error :
                make_error_code(DownloadErrc::internal_error);
            result.performance =
                build_performance_summary(session, Clock::now());
            return result;
        }
        auto lifecycle = std::move(lifecycle_creation.value);
        std::vector<std::unique_ptr<core::RangeContext>> ranges;
        ranges.reserve(initial_plan.ranges.size());
        for (std::size_t index = 0;
             index < initial_plan.ranges.size();
             ++index) {
            const auto span = initial_plan.ranges[index];
            ranges.push_back(
                std::make_unique<core::RangeContext>(
                    index,
                    span.begin,
                    span.end - 1));
        }

        std::unique_ptr<flow::PacketFlow> packet_flow;
        const auto packet_flow_error = flow::PacketFlow::create(
            session.effective_policy.flow_control(),
            session.telemetry_session_,
            packet_flow);
        if (packet_flow_error) {
            file_writer.close();
            result.error = packet_flow_error;
            result.performance =
                build_performance_summary(session, Clock::now());
            return result;
        }
        BS::thread_pool<> workers(
            session.effective_policy.scheduling().connection_limit);
        persistence::PersistenceThread persistence(session,
            session.effective_policy.persistence(),
            packet_flow->consumer(),
            bitmap,
            file_writer,
            metadata_store,
            workers,
            ranges.size());
        for (const auto& range : ranges) {
            persistence.register_range(range.get());
        }
        persistence.start();

        std::vector<ExpectedGeometryAck> initial_geometry_acks;
        initial_geometry_acks.reserve(
            lifecycle_creation.effects.values.size());
        std::error_code initial_geometry_error;
        for (const auto& effect :
             lifecycle_creation.effects.values) {
            ExpectedGeometryAck expected;
            initial_geometry_error =
                submit_geometry_effect(
                    effect,
                    persistence,
                    expected);
            if (initial_geometry_error) {
                break;
            }
            initial_geometry_acks.push_back(expected);
        }
        if (!initial_geometry_error) {
            initial_geometry_error = wait_for_geometry_acks(
                persistence,
                initial_geometry_acks);
        }
        if (initial_geometry_error) {
            const auto applied = lifecycle->apply(
                range::EffectApplicationFailed{
                    initial_geometry_error
                });
            const auto close_error =
                packet_flow->producer().close();
            static_cast<void>(close_error);
            persistence.stop();
            persistence.join();
            file_writer.close();
            result.error = applied.error ?
                applied.error :
                initial_geometry_error;
            result.performance =
                build_performance_summary(
                    session,
                    Clock::now());
            return result;
        }

        // multi handle 统一承载所有 easy handle 的事件驱动；后面的主循环通过
        // curl_multi_perform + curl_multi_wait 实现非阻塞调度。
        CURLM* multi = curl_multi_init();
        if (multi == nullptr) {
            const auto close_error = packet_flow->producer().close();
            static_cast<void>(close_error);
            persistence.stop();
            persistence.join();
            file_writer.close();
            result.error = make_error_code(DownloadErrc::http_init_failed);
            result.performance = build_performance_summary(session, Clock::now());
            return result;
        }

        curl_multi_setopt(multi, CURLMOPT_MAX_TOTAL_CONNECTIONS,
            static_cast<long>(
                session.effective_policy.scheduling().connection_limit));
        curl_multi_setopt(multi, CURLMOPT_MAX_HOST_CONNECTIONS,
            static_cast<long>(
                session.effective_policy.scheduling().connection_limit));

        std::vector<TransferHandle> handles(
            session.effective_policy.scheduling().connection_limit);
        std::vector<flow::PacketLaneObservation>
            packet_observations;
        std::vector<flow::PacketPauseAction> packet_actions;
        packet_observations.reserve(handles.size());
        packet_actions.reserve(handles.size());
        std::error_code failure;
        for (auto& handle : handles) {
            handle.session = &session;
            handle.packet_producer = &packet_flow->producer();
            handle.flow_control = session.effective_policy.flow_control();
            if (const auto lane_error =
                    packet_flow->producer().open_lane(
                        handle.packet_lane);
                lane_error) {
                failure = lane_error;
                break;
            }
            handle.easy = curl_easy_init();
            if (handle.easy == nullptr) {
                failure = make_error_code(DownloadErrc::http_init_failed);
                break;
            }
        }

        auto emit_progress_at = Clock::now();
        while (!failure && !session.stop_requested.load(std::memory_order_acquire)) {
            if (const auto geometry_error =
                    drain_geometry_acks(
                        persistence,
                        handles);
                geometry_error) {
                const auto applied = lifecycle->apply(
                    range::EffectApplicationFailed{
                        geometry_error
                    });
                failure = applied.error ?
                    applied.error :
                    geometry_error;
                session.stop_requested.store(
                    true,
                    std::memory_order_release);
                break;
            }

            // 主循环开始时先看 Persistence 是否已经报错。写盘或 metadata 失败后，
            // 网络层必须尽快停止继续生产数据。
            if (const auto persistence_error = persistence.error(); persistence_error) {
                failure = persistence_error;
                session.stop_requested.store(true, std::memory_order_release);
                break;
            }

            for (auto& handle : handles) {
                if (handle.range != nullptr || handle.in_multi || failure) {
                    continue;
                }

                if (handle.pending_lease.has_value()) {
                    const auto expected =
                        std::span<const ExpectedGeometryAck>(
                            handle.pending_lease->acks.data(),
                            handle.pending_lease->ack_count);
                    if (!all_geometry_acks_received(expected)) {
                        continue;
                    }
                    const auto lease =
                        handle.pending_lease->lease;
                    handle.pending_lease.reset();
                    auto* projection = find_range_projection(
                        ranges,
                        lease.id.range);
                    if (projection == nullptr) {
                        const auto error =
                            make_error_code(
                                DownloadErrc::internal_error);
                        const auto applied = lifecycle->apply(
                            range::EffectApplicationFailed{
                                error
                            });
                        failure = applied.error ?
                            applied.error :
                            error;
                        session.stop_requested.store(
                            true,
                            std::memory_order_release);
                        break;
                    }
                    const auto arm_error = arm_transfer(
                        handle,
                        multi,
                        session,
                        *projection,
                        lease);
                    if (arm_error) {
                        const auto applied = lifecycle->apply(
                            range::LeaseFailed{
                                lease.id,
                                lease.bytes.begin,
                                arm_error
                            });
                        failure = applied.error ?
                            applied.error :
                            arm_error;
                        session.stop_requested.store(
                            true,
                            std::memory_order_release);
                    }
                    continue;
                }

                const auto acquired = lifecycle->acquire();
                if (acquired.error) {
                    failure = acquired.error;
                    session.stop_requested.store(
                        true,
                        std::memory_order_release);
                    break;
                }
                if (!acquired.lease.has_value()) {
                    continue;
                }
                if (const auto geometry_error =
                        apply_geometry_effects(
                            acquired.effects,
                            ranges,
                            persistence);
                    geometry_error) {
                    const auto applied = lifecycle->apply(
                        range::EffectApplicationFailed{
                            geometry_error
                        });
                    failure = applied.error ?
                        applied.error :
                        geometry_error;
                    session.stop_requested.store(
                        true,
                        std::memory_order_release);
                    break;
                }

                const auto lease = *acquired.lease;
                if (acquired.effects.size > 0) {
                    PendingLease pending;
                    pending.lease = lease;
                    pending.ack_count =
                        acquired.effects.size;
                    for (std::size_t index = 0;
                         index < acquired.effects.size;
                         ++index) {
                        const auto submit_error =
                            submit_geometry_effect(
                                acquired.effects.values[index],
                                persistence,
                                pending.acks[index]);
                        if (submit_error) {
                            const auto applied =
                                lifecycle->apply(
                                    range::EffectApplicationFailed{
                                        submit_error
                                    });
                            failure = applied.error ?
                                applied.error :
                                submit_error;
                            session.stop_requested.store(
                                true,
                                std::memory_order_release);
                            break;
                        }
                    }
                    if (failure) {
                        break;
                    }
                    handle.pending_lease = pending;
                    continue;
                }

                auto* projection = find_range_projection(
                    ranges,
                    lease.id.range);
                if (projection == nullptr) {
                    const auto error =
                        make_error_code(
                            DownloadErrc::internal_error);
                    const auto applied = lifecycle->apply(
                        range::EffectApplicationFailed{error});
                    failure = applied.error ?
                        applied.error :
                        error;
                    session.stop_requested.store(
                        true,
                        std::memory_order_release);
                    break;
                }

                const auto arm_error = arm_transfer(
                    handle,
                    multi,
                    session,
                    *projection,
                    lease);
                if (arm_error) {
                    const auto applied = lifecycle->apply(
                        range::LeaseFailed{
                            lease.id,
                            lease.bytes.begin,
                            arm_error
                        });
                    failure = applied.error ?
                        applied.error :
                        arm_error;
                    session.stop_requested.store(
                        true,
                        std::memory_order_release);
                    break;
                }
            }

            if (failure) {
                break;
            }

            // gap pause 和 memory backpressure 都是在事件循环里集中执行，避免在
            // write callback 里直接操作其他 handle，保持控制流简单。
            apply_gap_pauses(handles);
            if (const auto reconcile_error =
                    reconcile_packet_flow(
                        handles,
                        packet_observations,
                        packet_actions);
                reconcile_error) {
                failure = reconcile_error;
                session.stop_requested.store(
                    true,
                    std::memory_order_release);
                break;
            }

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
                    session,
                    *lifecycle);
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

            const auto has_active = std::any_of(handles.begin(), handles.end(),
                [](const TransferHandle& handle) {
                    return handle.in_multi ||
                        handle.range != nullptr ||
                        handle.pending_lease.has_value();
                });
            if (!has_active) {
                const auto lifecycle_snapshot =
                    lifecycle->snapshot();
                if (lifecycle_snapshot.error) {
                    failure = lifecycle_snapshot.error;
                    session.stop_requested.store(
                        true,
                        std::memory_order_release);
                    break;
                }
                if (!lifecycle_snapshot.value.has_schedulable_work) {
                    break;
                }
            }

            int num_fds = 0;
            const auto wait_status = curl_multi_wait(multi, nullptr, 0, 100, &num_fds);
            if (wait_status != CURLM_OK) {
                failure = make_error_code(DownloadErrc::http_transfer_failed);
                session.stop_requested.store(true, std::memory_order_release);
                break;
            }
        }

        if (failure ||
            session.stop_requested.load(
                std::memory_order_acquire)) {
            const auto cancelled =
                lifecycle->apply(range::CancelRequested{});
            if (!failure && cancelled.error) {
                failure = cancelled.error;
            }
        }

        // 收尾严格按“先停网络、再停持久化、最后 finalize 文件”的顺序执行，
        // 这样 VDL、bitmap 和磁盘内容才能在退出时保持一致。
        if (!failure) {
            failure = stop_network_phase(session, multi, handles);
        } else {
            const auto ignored = stop_network_phase(session, multi, handles);
            static_cast<void>(ignored);
        }
        failure = stop_persistence_phase(
            packet_flow->producer(), persistence, failure);

        // Persistence 线程拥有每个 range 的真正写盘前沿；停下来之后再做一次 bitmap
        // 重建，可以把最终结果对齐到落盘状态。
        rebuild_bitmap_from_ranges(bitmap,
            ranges,
            session.effective_policy.persistence().block_bytes,
            session.total_size);

        invoke_progress(session, ranges, handles);

        cleanup_network_resources(multi, handles);
        failure = finalize_storage_phase(file_writer,
            metadata_store,
            session,
            bitmap,
            failure);

        // DownloadResult 主要面向调用方总结最终状态，因此在这里统一从 session 和
        // bitmap 回填一次，避免中途多个分支各自维护结果对象。
        const auto downloaded = merged_downloaded_bytes(
            session,
            packet_flow->producer().snapshot());
        if (!downloaded && !failure) {
            failure = make_error_code(DownloadErrc::internal_error);
        }
        result.error = failure;
        result.total_bytes = session.total_size;
        result.downloaded_bytes = downloaded.value_or(
            std::numeric_limits<std::int64_t>::max());
        result.persisted_bytes = sum_finished_bytes(bitmap,
            session.effective_policy.persistence().block_bytes,
            session.total_size);
        result.completed_ranges = static_cast<std::size_t>(std::count_if(ranges.begin(),
            ranges.end(),
            [](const std::unique_ptr<core::RangeContext>& range) {
                return range->marked_finished.load(std::memory_order_acquire);
            }));
        result.resumed = session.resumed;
        result.temporary_path = session.paths.temporary_path;
        result.metadata_path = session.paths.metadata_path;
        if (!failure) {
            session.telemetry_session_.record_task_completed(Clock::now());
        }
        result.performance = build_performance_summary(session, Clock::now());
        return result;
    } catch (...) {
        // 对外契约是不抛异常，所以任何未预期错误最终都折叠成 internal_error。
        result.error = make_error_code(DownloadErrc::internal_error);
        return result;
    }
}

} // namespace asyncdownload::download





