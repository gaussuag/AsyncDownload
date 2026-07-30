#pragma once

#include "range/range_types.hpp"

#include <atomic>
#include <cstdint>
#include <optional>
#include <system_error>

namespace asyncdownload::range {

struct RangeFactSnapshot {
    ByteOffset persisted_through = 0;
    bool gap_paused = false;
    std::uint64_t committed_generation = 0;
    std::uint64_t revision = 0;
};

class RangeFactPublisher;

class RangeFactSlot {
public:
    RangeFactSlot(
        RangeId range,
        ByteOffset initial_offset) noexcept;

    [[nodiscard]] RangeFactPublisher publisher() noexcept;

    [[nodiscard]] std::optional<RangeFactSnapshot>
    read_since(std::uint64_t last_revision) const noexcept;

private:
    const RangeId range_;
    std::atomic<ByteOffset> persisted_through_;
    std::atomic<bool> gap_paused_{false};
    std::atomic<std::uint64_t> committed_generation_{0};
    std::atomic<std::uint64_t> revision_{0};

    friend class RangeFactPublisher;
};

class RangeFactPublisher {
public:
    RangeFactPublisher() noexcept = default;

    void publish_persisted_through(ByteOffset offset) noexcept;
    void publish_gap_pause(bool active) noexcept;
    [[nodiscard]] std::error_code publish_committed(
        CompletionId completion,
        ByteOffset offset) noexcept;

private:
    explicit RangeFactPublisher(RangeFactSlot& slot) noexcept;

    RangeFactSlot* slot_ = nullptr;

    friend class RangeFactSlot;
};

}
