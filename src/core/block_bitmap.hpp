#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace asyncdownload::core {

enum class BlockState : std::uint8_t {
    // 该块还没有任何可信落盘数据。
    empty = 0,
    // 该块已经被触达并部分写盘，但还不能保证完整可恢复。
    downloading = 1,
    // 该块的全部有效字节都已经稳定写盘，可直接参与恢复和 VDL 推进。
    finished = 2
};

class AtomicBlockBitmap {
public:
    AtomicBlockBitmap() = default;
    explicit AtomicBlockBitmap(std::size_t block_count);

    // 重新分配位图存储并清空状态。
    void reset(std::size_t block_count);

    [[nodiscard]] std::size_t block_count() const noexcept;
    [[nodiscard]] BlockState load(std::size_t index) const noexcept;
    void store(std::size_t index, BlockState state) noexcept;
    [[nodiscard]] bool compare_exchange(std::size_t index,
                                        BlockState expected,
                                        BlockState desired) noexcept;
    // 把当前位图拍平成普通字节数组，便于写入 metadata。
    [[nodiscard]] std::vector<std::uint8_t> snapshot() const;
    // 从 metadata 快照恢复位图原始状态，不做额外可信度判断。
    void restore(const std::vector<std::uint8_t>& states) noexcept;
    // 把只在进程运行期有效的 downloading 状态回滚成 empty。
    void reset_transient_states() noexcept;
    // 计算从文件头开始连续 finished 的安全字节数，也就是 VDL 候选值。
    [[nodiscard]] std::int64_t contiguous_finished_bytes(std::size_t block_size,
                                                         std::int64_t total_size) const noexcept;
    // 把某段已触达区间对应的块提升到 downloading，但不会覆盖 finished。
    void mark_downloading_range(std::int64_t start_offset,
                                std::int64_t end_offset,
                                std::size_t block_size,
                                std::int64_t total_size) noexcept;
    // 只有被完整覆盖的块才会升级成 finished。
    void mark_finished_range(std::int64_t start_offset,
                             std::int64_t end_offset,
                             std::size_t block_size,
                             std::int64_t total_size) noexcept;

private:
    std::size_t block_count_ = 0;
    std::unique_ptr<std::atomic<std::uint8_t>[]> states_;
};

// 根据文件总长度和块大小，计算位图需要覆盖多少个块。
[[nodiscard]] std::size_t required_block_count(std::int64_t total_size,
                                               std::size_t block_size) noexcept;

} // namespace asyncdownload::core
