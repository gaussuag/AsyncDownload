#include "persistence_thread.hpp"

#include "asyncdownload/error.hpp"
#include "core/alignment.hpp"
#include "core/constants.hpp"
#include "core/crc32.hpp"
#include "range/range_fault_adapter.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

namespace asyncdownload::persistence {

namespace {

} // namespace

PersistenceThread::PersistenceThread(core::SessionState& session,
                                     download::PersistencePolicy policy,
                                     flow::PacketConsumer& packet_consumer,
                                     core::AtomicBlockBitmap& bitmap,
                                     storage::FileWriter& file_writer,
                                     metadata::MetadataStore& metadata_store,
                                     BS::thread_pool<>& workers,
                                     const std::size_t initial_range_count)
    : session_(session),
      policy_(std::move(policy)),
      packet_consumer_(packet_consumer),
      bitmap_(bitmap),
      file_writer_(file_writer),
      metadata_store_(metadata_store),
      workers_(workers),
      geometry_capacity_(std::max(
          initial_range_count,
          2 * session.effective_policy
                  .scheduling()
                  .connection_limit)) {}

PersistenceThread::~PersistenceThread() {
    stop();
    join();
}

RangeRegistrationSubmitResult
PersistenceThread::submit_range_geometry(
    RangeGeometryCommand command) noexcept {
    try {
        std::scoped_lock lock(geometry_mutex_);
        if (geometry_commands_.size() >= geometry_capacity_ ||
            next_geometry_ticket_ ==
                std::numeric_limits<
                    RangeRegistrationTicket>::max()) {
            return {
                0,
                make_error_code(DownloadErrc::internal_error)
            };
        }
#if defined(ASYNCDOWNLOAD_RANGE_LIFECYCLE_FAULT_TEST)
        range::detail::fail_if_requested(
            range::detail::range_fault_plan()
                .fail_next_geometry_submit_allocation);
#endif
        const auto ticket = next_geometry_ticket_;
        geometry_commands_.emplace_back(
            ticket,
            std::move(command));
        ++next_geometry_ticket_;
        return {ticket, {}};
    } catch (const std::bad_alloc&) {
        return {
            0,
            std::make_error_code(std::errc::not_enough_memory)
        };
    } catch (...) {
        return {
            0,
            make_error_code(DownloadErrc::internal_error)
        };
    }
}

RangeRegistrationPollResult
PersistenceThread::poll_range_geometry_ack() noexcept {
    try {
        std::scoped_lock lock(geometry_mutex_);
        if (geometry_acks_.empty()) {
            return {};
        }
        auto ack = std::move(geometry_acks_.front());
        geometry_acks_.pop_front();
        return {std::move(ack), {}};
    } catch (...) {
        return {
            {},
            make_error_code(DownloadErrc::internal_error)
        };
    }
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

void PersistenceThread::process_loop() {
    // 这个循环的职责不是“看到包就写盘”这么简单，而是把网络层吐出来的离散
    // DataPacket 收敛成一条严格有序、按对齐规则落盘、并能周期性生成恢复元数据
    // 的持久化流水线。
    while (true) {
        process_range_geometry();
        if (error()) {
            break;
        }
        flow::PacketLease packet;
        const auto received = packet_consumer_.receive(
            packet, std::chrono::microseconds(100000));
        if (received.code == flow::PacketReceiveCode::packet) {
            handle_packet(std::move(packet));
        } else if (received.code == flow::PacketReceiveCode::closed) {
            maybe_schedule_flush(true);
            break;
        } else if (received.code == flow::PacketReceiveCode::failed) {
            set_error(received.error);
            break;
        }

        poll_pending_flush();
        maybe_schedule_flush(false);
        if (error()) {
            break;
        }
    }

    // 主循环退出并不代表最后一轮 flush 已经完成，所以这里还要等待挂起中的
    // flush/meta 任务结束，确保退出时磁盘和 metadata 是同一个版本。
    drain_buffered_packets();
    wait_pending_flush();
}

void PersistenceThread::process_range_geometry() noexcept {
    while (true) {
        std::optional<std::pair<
            RangeRegistrationTicket,
            RangeGeometryCommand>> pending;
        try {
            {
                std::scoped_lock lock(geometry_mutex_);
                if (geometry_commands_.empty()) {
                    return;
                }
                pending.emplace(
                    std::move(geometry_commands_.front()));
                geometry_commands_.pop_front();
            }

            RangeRegistrationAck ack;
            ack.ticket = pending->first;
            std::visit(
                [this, &ack](const auto& effect) {
                    ack.range = effect.range;
                    ack.geometry_revision =
                        effect.geometry_revision;
                    if (effect.range.value >
                            std::numeric_limits<
                                std::size_t>::max()) {
                        ack.error = make_error_code(
                            DownloadErrc::internal_error);
                        return;
                    }
                    if constexpr (std::is_same_v<
                                      std::decay_t<
                                          decltype(effect)>,
                                      range::RegisterRangeEffect>) {
                        if (effect.geometry_revision != 0 ||
                            effect.bytes.begin < 0 ||
                            effect.bytes.begin >=
                                effect.bytes.end ||
                            effect.bytes.end >
                                session_.total_size) {
                            ack.error = make_error_code(
                                DownloadErrc::internal_error);
                            return;
                        }
                        const auto range_id =
                            static_cast<std::size_t>(
                                effect.range.value);
#if defined(ASYNCDOWNLOAD_RANGE_LIFECYCLE_FAULT_TEST)
                        range::detail::fail_if_requested(
                            range::detail::range_fault_plan()
                                .fail_next_write_state_allocation);
#endif
                        auto created =
                            std::make_unique<RangeWriteState>();
                        created->id = effect.range;
                        created->bytes = effect.bytes;
                        created->geometry_revision =
                            effect.geometry_revision;
                        created->observed_dispatch_through =
                            effect.bytes.begin;
                        created->persisted_through =
                            effect.bytes.begin;
                        created->facts = effect.facts;
                        if (range_id >= ranges_.size()) {
                            ranges_.resize(range_id + 1);
                        }
                        if (ranges_[range_id] != nullptr) {
                            ack.error = make_error_code(
                                DownloadErrc::internal_error);
                            return;
                        }
                        ranges_[range_id] =
                            std::move(created);
                    } else {
                        auto* state = lookup_range(
                            static_cast<std::size_t>(
                                effect.range.value));
                        if (state == nullptr) {
                            ack.error = make_error_code(
                                DownloadErrc::internal_error);
                            return;
                        }
                        if (effect.geometry_revision !=
                                state->geometry_revision + 1 ||
                            effect.new_end <=
                                state->bytes.begin ||
                            effect.new_end >
                                state->bytes.end ||
                            effect.new_end <
                                state->observed_dispatch_through) {
                            ack.error = make_error_code(
                                DownloadErrc::internal_error);
                            return;
                        }
                        state->bytes.end = effect.new_end;
                        state->geometry_revision =
                            effect.geometry_revision;
                    }
                },
                pending->second);

            {
                std::scoped_lock lock(geometry_mutex_);
                if (geometry_acks_.size() >=
                    geometry_capacity_) {
                    set_error(make_error_code(
                        DownloadErrc::internal_error));
                    return;
                }
                geometry_acks_.push_back(std::move(ack));
            }
        } catch (const std::bad_alloc&) {
            set_error(
                std::make_error_code(
                    std::errc::not_enough_memory));
            return;
        } catch (...) {
            set_error(make_error_code(
                DownloadErrc::internal_error));
            return;
        }
    }
}

void PersistenceThread::handle_packet(flow::PacketLease packet) {
    if (packet.kind() == flow::PacketKind::control) {
        const auto* control = packet.control();
        if (control == nullptr ||
            control->completion.range.value >
                std::numeric_limits<std::size_t>::max()) {
            packet.complete();
            set_error(make_error_code(DownloadErrc::internal_error));
            return;
        }
        const auto control_value = *control;
        packet.complete();
        handle_range_complete(control_value);
        return;
    }

    handle_data_packet(std::move(packet));
}

void PersistenceThread::handle_data_packet(flow::PacketLease packet) {
    const auto* data = packet.data();
    if (data == nullptr ||
        data->lease.range.value >
            std::numeric_limits<std::size_t>::max()) {
        packet.complete();
        set_error(make_error_code(DownloadErrc::internal_error));
        return;
    }
    const auto range_id =
        static_cast<std::size_t>(data->lease.range.value);
    auto* range = lookup_range(range_id);
    if (range == nullptr) {
        packet.complete();
        set_error(make_error_code(DownloadErrc::internal_error));
        return;
    }
    if (data->payload.empty() ||
        data->payload.size() >
            static_cast<std::size_t>(
                std::numeric_limits<
                    std::int64_t>::max()) ||
        data->offset < 0 ||
        data->offset >
            std::numeric_limits<std::int64_t>::max() -
                static_cast<std::int64_t>(
                    data->payload.size())) {
        packet.complete();
        set_error(make_error_code(
            DownloadErrc::internal_error));
        return;
    }
    const auto packet_end = data->offset +
        static_cast<std::int64_t>(data->payload.size());
    if (data->lease_span.begin <
            range->bytes.begin ||
        data->lease_span.begin >=
            data->lease_span.end ||
        data->lease_span.end > range->bytes.end ||
        data->offset < data->lease_span.begin ||
        packet_end > data->lease_span.end ||
        packet_end > range->bytes.end) {
        packet.complete();
        set_error(make_error_code(
            DownloadErrc::internal_error));
        return;
    }
    if (packet_end <= range->persisted_through) {
        packet.complete();
        return;
    }
    if (data->offset < range->persisted_through) {
        packet.complete();
        set_error(make_error_code(
            DownloadErrc::internal_error));
        return;
    }
    if (range->last_observed_lease_generation == 0 &&
        data->lease.generation != 1) {
        packet.complete();
        set_error(make_error_code(
            DownloadErrc::internal_error));
        return;
    }
    if (range->last_observed_lease_generation ==
            data->lease.generation &&
        range->last_observed_lease_span.has_value() &&
        *range->last_observed_lease_span !=
            data->lease_span) {
        packet.complete();
        set_error(make_error_code(
            DownloadErrc::internal_error));
        return;
    }
    if (data->lease.generation >
            range->last_observed_lease_generation &&
        data->lease.generation !=
            range->last_observed_lease_generation + 1) {
        packet.complete();
        set_error(make_error_code(
            DownloadErrc::internal_error));
        return;
    }
    if (data->lease.generation <
        range->last_observed_lease_generation) {
        packet.complete();
        set_error(make_error_code(
            DownloadErrc::internal_error));
        return;
    }
    if (data->lease.generation ==
            range->last_observed_lease_generation + 1 &&
        range->last_observed_lease_span.has_value() &&
        data->lease_span.begin <
            range->last_observed_lease_span->end) {
        packet.complete();
        set_error(make_error_code(
            DownloadErrc::internal_error));
        return;
    }
    range->observed_activity = true;
    if (data->lease.generation >
        range->last_observed_lease_generation) {
        range->last_observed_lease_generation =
            data->lease.generation;
        range->last_observed_lease_span =
            data->lease_span;
    }
    range->observed_dispatch_through = std::max(
        range->observed_dispatch_through,
        data->lease_span.end);
    if (data->offset == range->persisted_through) {
        // 命中当前 expected offset 时，说明这批数据正好可以接到已经落盘的前沿后面，
        // 于是直接写入，并尝试把 map 里后续连续片段一并 drain 掉。
        const auto packet_size = data->payload.size();
        const auto append_error = append_bytes(
            *range, data->offset, data->payload, false);
        packet.complete();
        if (append_error) {
            set_error(append_error);
            return;
        }
        range->persisted_through +=
            static_cast<std::int64_t>(packet_size);
        range->facts.publish_persisted_through(
            range->persisted_through);
        update_finished_blocks(*range);
        drain_ordered_packets(*range, false);
    } else {
        // 回调线程不能阻塞等待缺口补齐，所以乱序包先进入 map。
        // Persistence 线程只要等到 expected offset 到达，就能把后续连续片段一起链式写下去。
        const auto existing =
            range->out_of_order.find(data->offset);
        if (existing != range->out_of_order.end()) {
            const auto* existing_data =
                existing->second.data();
            if (existing_data != nullptr &&
                existing_data->lease == data->lease &&
                existing_data->payload.size() ==
                    data->payload.size()) {
                packet.complete();
                return;
            }
            packet.complete();
            set_error(make_error_code(
                DownloadErrc::internal_error));
            return;
        }
        try {
            if (const auto account_error =
                    packet.account_reorder_node();
                account_error) {
                packet.complete();
                set_error(account_error);
                return;
            }
#if defined(ASYNCDOWNLOAD_RANGE_LIFECYCLE_FAULT_TEST)
            range::detail::fail_if_requested(
                range::detail::range_fault_plan()
                    .fail_next_reorder_allocation);
#endif
            const auto offset = data->offset;
            const auto [position, inserted] =
                range->out_of_order.emplace(
                    offset,
                    std::move(packet));
            static_cast<void>(position);
            if (!inserted) {
                set_error(make_error_code(
                    DownloadErrc::internal_error));
                return;
            }
        } catch (const std::bad_alloc&) {
            packet.complete();
            set_error(std::make_error_code(
                std::errc::not_enough_memory));
            return;
        } catch (...) {
            packet.complete();
            set_error(make_error_code(
                DownloadErrc::internal_error));
            return;
        }
        update_gap_flag(*range);
    }

    maybe_schedule_flush(false);
}

void PersistenceThread::handle_range_complete(
    const flow::ControlPacket& control) {
    const auto range_id = static_cast<std::size_t>(
        control.completion.range.value);
    auto* range = lookup_range(range_id);
    if (range == nullptr) {
        set_error(make_error_code(DownloadErrc::internal_error));
        return;
    }
    if (control.completion.generation <
        range->last_observed_lease_generation) {
        return;
    }
    if (range->committed) {
        if (range->pending_completion ==
                control.completion &&
            control.expected_end == range->bytes.end &&
            range->persisted_through ==
                control.expected_end) {
            return;
        }
        set_error(make_error_code(
            DownloadErrc::internal_error));
        return;
    }
    if (control.completion.generation !=
            range->last_observed_lease_generation ||
        control.completion.generation == 0 ||
        control.expected_end != range->bytes.end ||
        range->persisted_through !=
            control.expected_end ||
        range->gap_blocked ||
        !range->out_of_order.empty()) {
        set_error(make_error_code(
            DownloadErrc::internal_error));
        return;
    }
    range->observed_activity = true;
    range->pending_completion = control.completion;
    range->observed_dispatch_through = std::max(
        range->observed_dispatch_through,
        control.expected_end);
    // 网络层认定一个 range 的 HTTP 请求已经全部结束后，Persistence 仍然要做
    // 两件事：把最后没凑满对齐块的 tail 刷掉，以及把状态正式推进到 finished。
    const auto flush_error = flush_tail(*range, false);
    if (flush_error) {
        set_error(flush_error);
        return;
    }
    if (range->tail.length != 0) {
        set_error(make_error_code(
            DownloadErrc::internal_error));
        return;
    }

    update_finished_blocks(*range);
    const auto publish_error =
        range->facts.publish_committed(
            control.completion,
            range->persisted_through);
    if (publish_error) {
        set_error(publish_error);
        return;
    }
    range->committed = true;
    maybe_schedule_flush(true);
}

RangeWriteState* PersistenceThread::lookup_range(
    const std::size_t range_id) const {
    if (range_id >= ranges_.size()) {
        return nullptr;
    }
    return ranges_[range_id].get();
}

std::error_code PersistenceThread::append_bytes(RangeWriteState& range,
                                                const std::int64_t offset,
                                                std::span<const std::uint8_t> bytes,
                                                const bool sample_timing) {
    static_cast<void>(sample_timing);
    auto cursor = offset;
    std::size_t index = 0;
    const auto alignment = policy_.io_alignment_bytes;

    while (index < bytes.size()) {
        if (range.tail.length > 0) {
            if (range.tail.offset +
                static_cast<std::int64_t>(range.tail.length) != cursor) {
                return make_error_code(DownloadErrc::internal_error);
            }

            // 先把前一个未对齐尾巴尽量补满；只有形成完整对齐块，或者已经碰到文件末尾，
            // 才会真正落盘。
            const auto writable = std::min(alignment - range.tail.length,
                bytes.size() - index);
            std::memcpy(range.tail.data.data() + range.tail.length,
                bytes.data() + index,
                writable);
            range.tail.length += writable;
            cursor += static_cast<std::int64_t>(writable);
            index += writable;

            const auto is_last_chunk = range.tail.offset +
                static_cast<std::int64_t>(range.tail.length) >= session_.total_size;
            if (range.tail.length == alignment || is_last_chunk) {
                const auto flush_error = flush_tail(range, sample_timing);
                if (flush_error) {
                    return flush_error;
                }
            }

            continue;
        }

        if ((cursor % static_cast<std::int64_t>(alignment)) != 0) {
            // range 可以从任意偏移开始，但磁盘写入要尽量按 4KB 对齐，所以先把头部
            // 零散字节放进 tail buffer，等后续数据把这个扇区补完整。
            range.tail.offset = cursor;
            const auto writable = std::min(core::bytes_to_alignment(cursor, alignment),
                bytes.size() - index);
            std::memcpy(range.tail.data.data(), bytes.data() + index, writable);
            range.tail.length = writable;
            cursor += static_cast<std::int64_t>(writable);
            index += writable;
            continue;
        }

        const auto aligned_bytes = core::full_aligned_prefix(bytes.size() - index, alignment);
        if (aligned_bytes == 0) {
            // 剩余数据不足一个完整对齐块时不直接写盘，避免每次都触发小块写入。
            range.tail.offset = cursor;
            std::memcpy(range.tail.data.data(), bytes.data() + index, bytes.size() - index);
            range.tail.length = bytes.size() - index;
            return {};
        }

        // 真正的磁盘写入只发生在 Persistence 线程里，这样相邻 range 在扇区边界上
        // 不会出现并发 RMW 竞争。
        const auto write_error = write_bytes(cursor,
            bytes.subspan(index, aligned_bytes),
            sample_timing,
            false);
        if (write_error) {
            return write_error;
        }

        bitmap_.mark_downloading_range(cursor,
            cursor + static_cast<std::int64_t>(aligned_bytes),
            policy_.block_bytes,
            session_.total_size);
        bytes_since_flush_ += aligned_bytes;
        session_.persisted_bytes.fetch_add(static_cast<std::int64_t>(aligned_bytes),
            std::memory_order_relaxed);
        session_.telemetry_session_.record_persist_delta(static_cast<std::uint64_t>(aligned_bytes));
        cursor += static_cast<std::int64_t>(aligned_bytes);
        index += aligned_bytes;
    }

    return {};
}

std::error_code PersistenceThread::write_bytes(const std::int64_t offset,
                                               const std::span<const std::uint8_t> bytes,
                                               const bool sample_timing,
                                               const bool tail_write) {
    static_cast<void>(sample_timing);
    static_cast<void>(tail_write);
    return file_writer_.write(offset, bytes);
}

std::error_code PersistenceThread::flush_tail(
    RangeWriteState& range,
    const bool sample_timing) {
    if (range.tail.length == 0) {
        return {};
    }

    const auto remaining = static_cast<std::size_t>(std::max<std::int64_t>(0,
        session_.total_size - range.tail.offset));
    const auto write_size = std::min(remaining,
        std::max(
            range.tail.length,
            std::min(policy_.io_alignment_bytes, remaining)));
    std::array<std::uint8_t, core::TAIL_BUFFER_CAPACITY_BYTES> bytes{};
    std::memcpy(
        bytes.data(),
        range.tail.data.data(),
        range.tail.length);

    // range 结束时即使不足 4KB 也要把尾巴刷掉，否则 metadata 看起来完成了，
    // 但磁盘上最后一个扇区还停留在内存里。
    const auto write_error = write_bytes(range.tail.offset,
        std::span<const std::uint8_t>(bytes.data(), write_size),
        sample_timing,
        true);
    if (write_error) {
        return write_error;
    }

    bitmap_.mark_downloading_range(range.tail.offset,
        range.tail.offset + static_cast<std::int64_t>(write_size),
        policy_.block_bytes,
        session_.total_size);
    bytes_since_flush_ += range.tail.length;
    session_.persisted_bytes.fetch_add(static_cast<std::int64_t>(range.tail.length),
        std::memory_order_relaxed);
    session_.telemetry_session_.record_persist_delta(
        static_cast<std::uint64_t>(range.tail.length));
    range.tail = {};
    return {};
}

void PersistenceThread::update_finished_blocks(
    const RangeWriteState& range) noexcept {
    // downloading 表示“这个 block 已经被触达并落盘过部分内容”，而 finished
    // 表示“这个 block 的全部有效字节都已经可恢复”。这里统一由 persisted_offset
    // 去决定哪些 block 可以升级到 finished。
    bitmap_.mark_finished_range(range.bytes.begin,
        range.persisted_through,
        policy_.block_bytes,
        session_.total_size);
}

void PersistenceThread::drain_ordered_packets(
    RangeWriteState& range,
    const bool sample_timing) {
    // 一旦 expected offset 对上，就尽量把 map 里后面连续的 packet 一次性清空。
    // 这样既能减少 map 常驻量，也能快速消除 gap pause。
    while (!range.out_of_order.empty()) {
        auto next = range.out_of_order.begin();
        if (next->first != range.persisted_through) {
            break;
        }

        auto packet = std::move(next->second);
        range.out_of_order.erase(next);
        const auto* data = packet.data();
        if (data == nullptr) {
            packet.complete();
            set_error(make_error_code(DownloadErrc::internal_error));
            return;
        }
        const auto packet_size = data->payload.size();
        const auto append_error = append_bytes(
            range, data->offset, data->payload, sample_timing);
        packet.complete();
        if (append_error) {
            set_error(append_error);
            return;
        }
        range.persisted_through +=
            static_cast<std::int64_t>(packet_size);
        range.facts.publish_persisted_through(
            range.persisted_through);
        update_finished_blocks(range);
    }

    update_gap_flag(range);
}

void PersistenceThread::drain_buffered_packets() noexcept {
    for (auto& range : ranges_) {
        if (range == nullptr) {
            continue;
        }
        while (!range->out_of_order.empty()) {
            auto packet = std::move(
                range->out_of_order.begin()->second);
            range->out_of_order.erase(
                range->out_of_order.begin());
            packet.complete();
        }
    }
}

void PersistenceThread::update_gap_flag(
    RangeWriteState& range) {
    if (range.out_of_order.empty()) {
        if (range.gap_blocked) {
            range.gap_blocked = false;
            range.facts.publish_gap_pause(false);
        }
        return;
    }

    // gap 太大说明这个 range 前面有长时间补不上的洞，再继续接收后续数据只会
    // 无限堆积内存，所以让 Orchestrator 暂停这个 handle，等缺口被补齐后再恢复。
    const auto gap = range.out_of_order.begin()->first -
        range.persisted_through;
    const auto blocked = gap > policy_.max_gap_bytes;
    if (blocked != range.gap_blocked) {
        range.gap_blocked = blocked;
        range.facts.publish_gap_pause(blocked);
    }
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
    const auto interval_elapsed =
        now - last_flush_time_ >= policy_.flush_interval;
    if (!force &&
        bytes_since_flush_ < policy_.flush_threshold_bytes &&
        !interval_elapsed) {
        return;
    }

    // Flush 和 metadata 保存都放到线程池异步做，Persistence 线程继续串行消费
    // 数据包，避免把网络到磁盘这条主链卡死在 FlushFileBuffers 上。
    auto snapshot = build_metadata_state();
    bytes_since_flush_ = 0;
    last_flush_time_ = now;

    pending_flush_ = workers_.submit_task([this, snapshot]() mutable {
        auto flush_error = file_writer_.flush();
        if (flush_error) {
            return flush_error;
        }

        // CRC 只对 VDL 之后仍被标成 finished 的块采样，因为 VDL 之前的数据已经由
        // “最长连续安全前沿”语义兜底，恢复时不需要再逐块复查。
        snapshot.vdl_offset = bitmap_.contiguous_finished_bytes(policy_.block_bytes,
            session_.total_size);
        snapshot.crc_samples = build_crc_samples(snapshot);
        auto metadata_error = metadata_store_.save(snapshot);
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
    session_.vdl_offset.store(bitmap_.contiguous_finished_bytes(policy_.block_bytes,
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
    session_.vdl_offset.store(bitmap_.contiguous_finished_bytes(policy_.block_bytes,
        session_.total_size), std::memory_order_release);
}

core::MetadataState PersistenceThread::build_metadata_state() const {
    core::MetadataState state{};
    state.url = session_.url;
    state.output_path = session_.paths.output_path;
    state.temporary_path = session_.paths.temporary_path;
    state.total_size = session_.total_size;
    state.accept_ranges =
        session_.effective_policy.remote_facts().accept_ranges;
    state.resumed = session_.resumed;
    state.etag = session_.etag;
    state.last_modified = session_.last_modified;
    state.block_size = policy_.block_bytes;
    state.io_alignment = policy_.io_alignment_bytes;

    core::AtomicBlockBitmap snapshot_bitmap(core::required_block_count(session_.total_size,
        policy_.block_bytes));
    snapshot_bitmap.restore(bitmap_.snapshot());

    // metadata 快照不能只信 bitmap 当前值，因为某些 range 的 persisted_offset
    // 可能已经推进了，但本轮 snapshot 还没来得及把这些推进反映到独立副本里。
    for (const auto& range : ranges_) {
        if (range == nullptr) {
            continue;
        }

        const auto status = range->local_failure ?
            core::RangeStatus::failed :
            range->committed ?
                core::RangeStatus::finished :
                range->gap_blocked ?
                    core::RangeStatus::paused :
                    range->observed_activity ?
                        core::RangeStatus::downloading :
                        core::RangeStatus::empty;
        state.ranges.push_back(core::RangeStateSnapshot{
            static_cast<std::size_t>(range->id.value),
            range->bytes.begin,
            range->bytes.end - 1,
            std::max(
                range->observed_dispatch_through,
                range->persisted_through),
            range->persisted_through,
            static_cast<std::uint8_t>(status)});

        if (range->persisted_through >
            range->bytes.begin) {
            snapshot_bitmap.mark_finished_range(
                range->bytes.begin,
                range->persisted_through,
                policy_.block_bytes,
                session_.total_size);
        }
    }

    state.bitmap_states = snapshot_bitmap.snapshot();
    state.vdl_offset = snapshot_bitmap.contiguous_finished_bytes(policy_.block_bytes,
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
        const auto read_error = file_writer_.read(offset, length, bytes);
        if (read_error) {
            continue;
        }

        samples.push_back(core::BlockCrcSample{offset, core::crc32(bytes), length});
    }

    return samples;
}

void PersistenceThread::set_error(const std::error_code error) {
    auto should_fail_flow = false;
    {
        std::scoped_lock lock(error_mutex_);
        if (!error_) {
            error_ = error;
            should_fail_flow = true;
        }
    }
    if (should_fail_flow) {
        const auto fail_error = packet_consumer_.fail(error);
        static_cast<void>(fail_error);
    }
}

} // namespace asyncdownload::persistence


