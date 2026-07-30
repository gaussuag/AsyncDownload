#pragma once

#include "download/download_policy.hpp"
#include "range/range_types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <system_error>
#include <variant>
#include <vector>

namespace asyncdownload::range {

enum class LeaseOutcomeKind : std::uint8_t {
    succeeded = 0,
    failed = 1
};

struct LeaseSucceeded {
    LeaseId lease{};
    ByteOffset received_through = 0;
};

struct LeaseFailed {
    LeaseId lease{};
    ByteOffset accepted_through = 0;
    std::error_code cause;
};

struct PersistedThrough {
    RangeId range{};
    ByteOffset offset = 0;
};

struct GapPauseChanged {
    RangeId range{};
    bool active = false;
};

struct PersistenceCommitted {
    CompletionId completion{};
    ByteOffset persisted_through = 0;
};

struct PersistenceFailed {
    std::optional<RangeId> range;
    std::error_code cause;
};

struct CancelRequested {};

struct EffectApplicationFailed {
    std::error_code cause;
};

using RangeEvent = std::variant<
    LeaseSucceeded,
    LeaseFailed,
    PersistedThrough,
    GapPauseChanged,
    PersistenceCommitted,
    PersistenceFailed,
    CancelRequested,
    EffectApplicationFailed>;

struct RegisterRangeEffect {
    RangeId range{};
    ByteSpan bytes{};
    std::uint64_t geometry_revision = 0;
};

struct ResizeRangeEffect {
    RangeId range{};
    ByteOffset new_end = 0;
    std::uint64_t geometry_revision = 0;
};

struct PublishRangeCompleteEffect {
    CompletionId completion{};
    ByteOffset expected_end = 0;
};

using RangeEffect = std::variant<
    RegisterRangeEffect,
    ResizeRangeEffect,
    PublishRangeCompleteEffect>;

template <std::size_t Capacity>
struct EffectBatch {
    std::array<RangeEffect, Capacity> values{};
    std::size_t size = 0;
};

struct InitialEffectBatch {
    std::vector<RangeEffect> values;
};

enum class EventDisposition : std::uint8_t {
    applied = 0,
    duplicate = 1,
    stale = 2,
    rejected = 3
};

struct AcquireResult {
    std::optional<RangeLease> lease;
    EffectBatch<2> effects;
    std::error_code error;
};

struct ApplyResult {
    EventDisposition disposition = EventDisposition::applied;
    std::error_code error;
    bool scheduler_may_run = false;
    bool task_should_stop = false;
    EffectBatch<1> effects;
};

struct RangeSnapshot {
    RangeId id{};
    ByteSpan bytes{};
    ByteOffset dispatch_cursor = 0;
    ByteOffset persisted_through = 0;
    RangePhase phase = RangePhase::ready;
    bool gap_blocked = false;
    std::optional<LeaseId> active_lease;
};

struct LifecycleSnapshot {
    std::vector<RangeSnapshot> ranges;
    std::size_t finished_ranges = 0;
    bool has_schedulable_work = false;
    bool all_finished = false;
};

struct SnapshotResult {
    LifecycleSnapshot value;
    std::error_code error;
};

class RangeLifecycle;

struct RangeLifecycleCreation {
    std::unique_ptr<RangeLifecycle> value;
    InitialEffectBatch effects;
    std::error_code error;
};

class RangeLifecycle {
public:
    [[nodiscard]] static RangeLifecycleCreation create(
        ByteOffset total_size,
        const download::SchedulingPolicy& scheduling,
        std::span<const ByteSpan> initial_ranges) noexcept;

    ~RangeLifecycle();

    RangeLifecycle(const RangeLifecycle&) = delete;
    RangeLifecycle& operator=(const RangeLifecycle&) = delete;

    [[nodiscard]] AcquireResult acquire() noexcept;
    [[nodiscard]] ApplyResult apply(const RangeEvent& event) noexcept;
    [[nodiscard]] ApplyResult drain_persistence_facts() noexcept;
    [[nodiscard]] SnapshotResult snapshot() const noexcept;

private:
    class Implementation;

    explicit RangeLifecycle(
        std::unique_ptr<Implementation> implementation) noexcept;

    std::unique_ptr<Implementation> implementation_;
};

}
