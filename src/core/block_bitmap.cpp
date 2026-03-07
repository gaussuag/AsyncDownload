#include "block_bitmap.hpp"

#include <algorithm>

namespace asyncdownload::core {

AtomicBlockBitmap::AtomicBlockBitmap(const std::size_t block_count) {
    reset(block_count);
}

void AtomicBlockBitmap::reset(const std::size_t block_count) {
    block_count_ = block_count;
    states_ = std::make_unique<std::atomic<std::uint8_t>[]>(block_count_);
    // 位图是恢复、调度和进度统计共享的最小公共状态，所以初始化时统一从 empty 开始，
    // 后续再由恢复逻辑或 Persistence 线程把状态推进到 downloading/finished。
    for (std::size_t index = 0; index < block_count_; ++index) {
        states_[index].store(static_cast<std::uint8_t>(BlockState::empty),
            std::memory_order_relaxed);
    }
}

std::size_t AtomicBlockBitmap::block_count() const noexcept {
    return block_count_;
}

BlockState AtomicBlockBitmap::load(const std::size_t index) const noexcept {
    return static_cast<BlockState>(states_[index].load(std::memory_order_acquire));
}

void AtomicBlockBitmap::store(const std::size_t index, const BlockState state) noexcept {
    states_[index].store(static_cast<std::uint8_t>(state), std::memory_order_release);
}

bool AtomicBlockBitmap::compare_exchange(const std::size_t index,
                                         BlockState expected,
                                         const BlockState desired) noexcept {
    auto expected_raw = static_cast<std::uint8_t>(expected);
    return states_[index].compare_exchange_strong(
        expected_raw,
        static_cast<std::uint8_t>(desired),
        std::memory_order_acq_rel,
        std::memory_order_acquire);
}

std::vector<std::uint8_t> AtomicBlockBitmap::snapshot() const {
    std::vector<std::uint8_t> copy(block_count_);
    // metadata 不直接持久化原子对象，而是把当前状态拍平成普通字节数组。
    for (std::size_t index = 0; index < block_count_; ++index) {
        copy[index] = states_[index].load(std::memory_order_acquire);
    }
    return copy;
}

void AtomicBlockBitmap::restore(const std::vector<std::uint8_t>& states) noexcept {
    const auto count = std::min(block_count_, states.size());
    // restore 只负责把快照放回位图，不在这里判断状态是否可信；
    // 诸如“downloading 需要回滚”“CRC 需要复核”都交给更高层恢复逻辑处理。
    for (std::size_t index = 0; index < count; ++index) {
        states_[index].store(states[index], std::memory_order_release);
    }
}

void AtomicBlockBitmap::reset_transient_states() noexcept {
    // downloading 只表示“上次进程运行时触碰过这个 block”，并不代表它已经形成
    // 稳定可恢复的磁盘状态，所以重启后要先回到 empty。
    for (std::size_t index = 0; index < block_count_; ++index) {
        if (load(index) == BlockState::downloading) {
            store(index, BlockState::empty);
        }
    }
}

std::int64_t AtomicBlockBitmap::contiguous_finished_bytes(const std::size_t block_size,
                                                          const std::int64_t total_size) const noexcept {
    std::int64_t finished = 0;
    // VDL 的语义不是“finished 总和”，而是“从文件头开始连续 finished 的最长前缀”。
    // 一旦中间出现洞，后面即使也 finished，也不能算进安全连续前沿。
    for (std::size_t index = 0; index < block_count_; ++index) {
        if (load(index) != BlockState::finished) {
            break;
        }

        finished += static_cast<std::int64_t>(block_size);
        if (finished >= total_size) {
            return total_size;
        }
    }

    return finished;
}

void AtomicBlockBitmap::mark_downloading_range(const std::int64_t start_offset,
                                               const std::int64_t end_offset,
                                               const std::size_t block_size,
                                               const std::int64_t total_size) noexcept {
    if (end_offset <= start_offset || block_count_ == 0 || total_size <= 0) {
        return;
    }

    const auto clamped_start = std::max<std::int64_t>(0, start_offset);
    const auto clamped_end = std::min(end_offset, total_size);
    if (clamped_end <= clamped_start) {
        return;
    }

    const auto start_block = static_cast<std::size_t>(clamped_start /
        static_cast<std::int64_t>(block_size));
    const auto end_block = static_cast<std::size_t>((clamped_end - 1) /
        static_cast<std::int64_t>(block_size));

    for (std::size_t index = start_block; index <= end_block && index < block_count_; ++index) {
        // 这里只允许 empty -> downloading，避免把已经确认 finished 的块
        // 因为一次重复写入或恢复中的重试重新降级。
        const auto marked = compare_exchange(index, BlockState::empty, BlockState::downloading);
        static_cast<void>(marked);
    }
}

void AtomicBlockBitmap::mark_finished_range(const std::int64_t start_offset,
                                            const std::int64_t end_offset,
                                            const std::size_t block_size,
                                            const std::int64_t total_size) noexcept {
    if (end_offset <= start_offset || block_count_ == 0) {
        return;
    }

    const auto start_block = static_cast<std::size_t>(start_offset /
        static_cast<std::int64_t>(block_size));
    const auto end_block = static_cast<std::size_t>((end_offset - 1) /
        static_cast<std::int64_t>(block_size));

    for (std::size_t index = start_block; index <= end_block && index < block_count_; ++index) {
        const auto block_begin = static_cast<std::int64_t>(index * block_size);
        const auto block_end = std::min(block_begin + static_cast<std::int64_t>(block_size),
            total_size);
        // 只有当一个 block 的全部有效字节都被当前区间完整覆盖时，它才有资格升级为
        // finished；部分写入只能停留在 downloading。
        if (start_offset <= block_begin && end_offset >= block_end) {
            store(index, BlockState::finished);
        }
    }
}

std::size_t required_block_count(const std::int64_t total_size,
                                 const std::size_t block_size) noexcept {
    if (total_size <= 0 || block_size == 0) {
        return 0;
    }

    // 位图是按 block 向上取整覆盖整个文件的，最后一个 block 可以是部分有效数据。
    return static_cast<std::size_t>((total_size +
        static_cast<std::int64_t>(block_size) - 1) /
        static_cast<std::int64_t>(block_size));
}

} // namespace asyncdownload::core
