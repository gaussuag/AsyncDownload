#include "range_lifecycle.hpp"

#include "asyncdownload/error.hpp"
#include "download/range_scheduler.hpp"
#include "range/range_fault_adapter.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <new>
#include <thread>
#include <utility>

namespace asyncdownload::range {
namespace {

struct LeaseOutcome {
    LeaseId lease{};
    LeaseOutcomeKind kind = LeaseOutcomeKind::succeeded;
    ByteOffset through = 0;
    std::error_code cause;
};

struct RangeRecord {
    RangeId id{};
    ByteSpan bytes{};
    ByteOffset dispatch_cursor = 0;
    ByteOffset persisted_through = 0;
    RangePhase phase = RangePhase::ready;
    bool gap_blocked = false;
    std::uint64_t geometry_revision = 0;
    std::uint64_t next_lease_generation = 1;
    std::optional<RangeLease> active_lease;
    std::optional<CompletionId> completion;
    std::optional<LeaseOutcome> last_lease_outcome;
    std::unique_ptr<RangeFactSlot> fact_slot;
    std::uint64_t last_fact_revision = 0;
    std::uint64_t observed_committed_generation = 0;
};

[[nodiscard]] std::error_code internal_error() noexcept {
    return make_error_code(DownloadErrc::internal_error);
}

[[nodiscard]] bool is_terminal(const RangePhase phase) noexcept {
    return phase == RangePhase::finished ||
        phase == RangePhase::failed ||
        phase == RangePhase::cancelled;
}

}

class RangeLifecycle::Implementation {
public:
    Implementation(
        const ByteOffset object_size,
        download::SchedulingPolicy scheduling_policy) noexcept
        : total_size(object_size),
          scheduling(std::move(scheduling_policy)),
          scheduler(scheduling, total_size),
          owner_thread(std::this_thread::get_id()) {}

    [[nodiscard]] bool owner_thread_matches() const noexcept {
        return owner_thread == std::this_thread::get_id();
    }

    [[nodiscard]] RangeRecord* find(const RangeId id) noexcept {
        const auto found = std::find_if(
            ranges.begin(),
            ranges.end(),
            [id](const auto& record) {
                return record != nullptr && record->id == id;
            });
        return found == ranges.end() ? nullptr : found->get();
    }

    [[nodiscard]] const RangeRecord* find(
        const RangeId id) const noexcept {
        const auto found = std::find_if(
            ranges.begin(),
            ranges.end(),
            [id](const auto& record) {
                return record != nullptr && record->id == id;
            });
        return found == ranges.end() ? nullptr : found->get();
    }

    void remember_error(const std::error_code value) noexcept {
        if (!first_error) {
            first_error = value ? value : internal_error();
        }
    }

    void fail_record(
        RangeRecord& record,
        const std::error_code error) noexcept {
        if (!is_terminal(record.phase)) {
            record.phase = RangePhase::failed;
            record.active_lease.reset();
        }
        remember_error(error);
    }

    void fail_unfinished(const std::error_code error) noexcept {
        remember_error(error);
        for (auto& record : ranges) {
            if (record != nullptr && !is_terminal(record->phase)) {
                record->phase = RangePhase::failed;
                record->active_lease.reset();
            }
        }
    }

    [[nodiscard]] ApplyResult reject(
        RangeRecord* record,
        const std::error_code error) noexcept {
        if (record == nullptr) {
            fail_unfinished(error);
        } else {
            fail_record(*record, error);
        }
        return {
            EventDisposition::rejected,
            first_error,
            false,
            true,
            {}
        };
    }

    [[nodiscard]] AcquireResult issue_lease(
        RangeRecord& record) noexcept {
#if defined(ASYNCDOWNLOAD_RANGE_LIFECYCLE_FAULT_TEST)
        const auto forced_generation =
            detail::range_fault_plan()
                .force_next_lease_generation.exchange(
                    0,
                    std::memory_order_acq_rel);
        if (forced_generation != 0) {
            record.next_lease_generation =
                forced_generation;
        }
#endif
        if (record.next_lease_generation ==
            std::numeric_limits<std::uint64_t>::max()) {
            fail_record(record, internal_error());
            return {{}, {}, first_error};
        }

        const download::RangeCandidate candidate{
            record.id,
            record.bytes,
            record.dispatch_cursor,
            record.phase
        };
        const auto window = scheduler.next_window(candidate);
        if (window.begin < record.bytes.begin ||
            window.begin != record.dispatch_cursor ||
            window.begin >= window.end ||
            window.end > record.bytes.end) {
            fail_record(record, internal_error());
            return {{}, {}, first_error};
        }

        const RangeLease lease{
            {record.id, record.next_lease_generation},
            window,
            scheduling.issue_range_requests
        };
        ++record.next_lease_generation;
        record.active_lease = lease;
        record.dispatch_cursor = window.end;
        record.phase = RangePhase::leased;
        return {lease, {}, {}};
    }

    [[nodiscard]] AcquireResult steal_and_issue() noexcept {
        try {
            candidate_scratch.clear();
            candidate_scratch.reserve(ranges.size());
            for (const auto& record : ranges) {
                if (record == nullptr ||
                    record->gap_blocked ||
                    is_terminal(record->phase)) {
                    continue;
                }
                candidate_scratch.push_back({
                    record->id,
                    record->bytes,
                    record->dispatch_cursor,
                    record->phase
                });
            }
            const auto plan =
                scheduler.choose_steal(candidate_scratch);
            if (!plan.has_value()) {
                return {};
            }

            auto* donor = find(plan->donor);
            if (donor == nullptr ||
                plan->split <= donor->dispatch_cursor ||
                plan->split >= donor->bytes.end ||
                plan->split % scheduling.block_bytes != 0 ||
                next_range_id ==
                    std::numeric_limits<std::uint64_t>::max()) {
                fail_unfinished(internal_error());
                return {{}, {}, first_error};
            }

            const auto old_end = donor->bytes.end;
            const RangeId new_id{next_range_id};
            auto added = std::make_unique<RangeRecord>();
            added->id = new_id;
            added->bytes = {plan->split, old_end};
            added->dispatch_cursor = plan->split;
            added->persisted_through = plan->split;
            added->fact_slot =
                std::make_unique<RangeFactSlot>(
                    new_id,
                    plan->split);
            ranges.push_back(std::move(added));

            ++next_range_id;
            donor->bytes.end = plan->split;
            ++donor->geometry_revision;
            auto result = issue_lease(*ranges.back());
            if (result.error) {
                return result;
            }
            result.effects.values[0] = ResizeRangeEffect{
                donor->id,
                donor->bytes.end,
                donor->geometry_revision
            };
            result.effects.values[1] = RegisterRangeEffect{
                new_id,
                ranges.back()->bytes,
                ranges.back()->geometry_revision,
                ranges.back()->fact_slot->publisher()
            };
            result.effects.size = 2;
            return result;
        } catch (const std::bad_alloc&) {
            fail_unfinished(
                std::make_error_code(std::errc::not_enough_memory));
            return {{}, {}, first_error};
        } catch (...) {
            fail_unfinished(internal_error());
            return {{}, {}, first_error};
        }
    }

    ByteOffset total_size = 0;
    download::SchedulingPolicy scheduling{};
    download::RangeScheduler scheduler;
    std::vector<std::unique_ptr<RangeRecord>> ranges;
    std::vector<download::RangeCandidate> candidate_scratch;
    std::uint64_t next_range_id = 0;
    std::size_t finished_ranges = 0;
    std::error_code first_error;
    std::thread::id owner_thread;
};

RangeLifecycleCreation RangeLifecycle::create(
    const ByteOffset total_size,
    const download::SchedulingPolicy& scheduling,
    const std::span<const ByteSpan> initial_ranges) noexcept {
    RangeLifecycleCreation creation;
    if (total_size <= 0 ||
        scheduling.connection_limit == 0 ||
        scheduling.transfer_window_bytes <= 0 ||
        scheduling.block_bytes <= 0 ||
        initial_ranges.empty()) {
        creation.error =
            std::make_error_code(std::errc::invalid_argument);
        return creation;
    }

    try {
#if defined(ASYNCDOWNLOAD_RANGE_LIFECYCLE_FAULT_TEST)
        detail::fail_if_requested(
            detail::range_fault_plan()
                .fail_next_create_allocation);
#endif
        auto implementation =
            std::make_unique<Implementation>(
                total_size, scheduling);
        implementation->ranges.reserve(initial_ranges.size());
        creation.effects.values.reserve(initial_ranges.size());
        for (std::size_t index = 0;
             index < initial_ranges.size();
             ++index) {
            const auto span = initial_ranges[index];
            if (span.begin < 0 ||
                span.begin >= span.end ||
                span.end > total_size ||
                span.begin % scheduling.block_bytes != 0 ||
                (span.end != total_size &&
                 span.end % scheduling.block_bytes != 0)) {
                creation.error =
                    std::make_error_code(std::errc::invalid_argument);
                return creation;
            }
            for (std::size_t previous = 0;
                 previous < index;
                 ++previous) {
                const auto other = initial_ranges[previous];
                if (span.begin < other.end &&
                    other.begin < span.end) {
                    creation.error =
                        std::make_error_code(
                            std::errc::invalid_argument);
                    return creation;
                }
            }

            auto record = std::make_unique<RangeRecord>();
            record->id = {
                static_cast<std::uint64_t>(index)
            };
            record->bytes = span;
            record->dispatch_cursor = span.begin;
            record->persisted_through = span.begin;
            record->fact_slot =
                std::make_unique<RangeFactSlot>(
                    record->id,
                    span.begin);
            creation.effects.values.push_back(
                RegisterRangeEffect{
                    record->id,
                    span,
                    record->geometry_revision,
                    record->fact_slot->publisher()
                });
            implementation->ranges.push_back(
                std::move(record));
        }
        if (!scheduling.issue_range_requests &&
            (initial_ranges.size() != 1 ||
             initial_ranges.front() != ByteSpan{0, total_size})) {
            creation.error =
                std::make_error_code(std::errc::invalid_argument);
            creation.effects.values.clear();
            return creation;
        }
        implementation->next_range_id =
            static_cast<std::uint64_t>(initial_ranges.size());
        creation.value.reset(
            new RangeLifecycle(std::move(implementation)));
        return creation;
    } catch (const std::bad_alloc&) {
        creation.value.reset();
        creation.effects.values.clear();
        creation.error =
            std::make_error_code(std::errc::not_enough_memory);
        return creation;
    } catch (...) {
        creation.value.reset();
        creation.effects.values.clear();
        creation.error = internal_error();
        return creation;
    }
}

RangeLifecycle::RangeLifecycle(
    std::unique_ptr<Implementation> implementation) noexcept
    : implementation_(std::move(implementation)) {}

RangeLifecycle::~RangeLifecycle() = default;

AcquireResult RangeLifecycle::acquire() noexcept {
    auto& implementation = *implementation_;
    assert(implementation.owner_thread_matches());
    if (!implementation.owner_thread_matches()) {
        return {{}, {}, internal_error()};
    }
    if (implementation.first_error) {
        return {{}, {}, implementation.first_error};
    }

    for (auto& record : implementation.ranges) {
        if (record != nullptr &&
            record->phase == RangePhase::ready &&
            !record->gap_blocked &&
            record->dispatch_cursor < record->bytes.end) {
            return implementation.issue_lease(*record);
        }
    }
    return implementation.steal_and_issue();
}

ApplyResult RangeLifecycle::apply(const RangeEvent& event) noexcept {
    auto& implementation = *implementation_;
    assert(implementation.owner_thread_matches());
    if (!implementation.owner_thread_matches()) {
        return implementation.reject(nullptr, internal_error());
    }

    if (const auto* succeeded =
            std::get_if<LeaseSucceeded>(&event)) {
        auto* record = implementation.find(succeeded->lease.range);
        if (record == nullptr) {
            return implementation.reject(nullptr, internal_error());
        }
        if (record->phase == RangePhase::failed ||
            record->phase == RangePhase::cancelled ||
            record->phase == RangePhase::finished) {
            return {EventDisposition::stale, {}, false, false, {}};
        }
        if (record->active_lease.has_value() &&
            record->active_lease->id == succeeded->lease) {
            if (succeeded->received_through !=
                record->active_lease->bytes.end) {
                return implementation.reject(
                    record, internal_error());
            }
            const auto completed_lease = *record->active_lease;
            record->last_lease_outcome = LeaseOutcome{
                succeeded->lease,
                LeaseOutcomeKind::succeeded,
                succeeded->received_through,
                {}
            };
            record->active_lease.reset();
            if (completed_lease.bytes.end < record->bytes.end) {
                record->phase = RangePhase::ready;
                return {
                    EventDisposition::applied,
                    {},
                    true,
                    false,
                    {}
                };
            }
            record->phase = RangePhase::awaiting_persistence;
            record->completion = CompletionId{
                record->id,
                succeeded->lease.generation
            };
            ApplyResult result;
            result.effects.values[0] =
                PublishRangeCompleteEffect{
                    *record->completion,
                    record->bytes.end
                };
            result.effects.size = 1;
            return result;
        }
        if (record->last_lease_outcome.has_value() &&
            record->last_lease_outcome->lease ==
                succeeded->lease) {
            if (record->last_lease_outcome->kind ==
                    LeaseOutcomeKind::succeeded &&
                record->last_lease_outcome->through ==
                    succeeded->received_through) {
                return {
                    EventDisposition::duplicate,
                    {},
                    false,
                    false,
                    {}
                };
            }
            return implementation.reject(record, internal_error());
        }
        if (succeeded->lease.generation <
            record->next_lease_generation) {
            return {EventDisposition::stale, {}, false, false, {}};
        }
        return implementation.reject(record, internal_error());
    }

    if (const auto* failed = std::get_if<LeaseFailed>(&event)) {
        auto* record = implementation.find(failed->lease.range);
        if (record == nullptr) {
            return implementation.reject(nullptr, internal_error());
        }
        if (record->phase == RangePhase::failed ||
            record->phase == RangePhase::cancelled ||
            record->phase == RangePhase::finished) {
            return {EventDisposition::stale, {}, false, false, {}};
        }
        if (record->active_lease.has_value() &&
            record->active_lease->id == failed->lease) {
            const auto span = record->active_lease->bytes;
            if (failed->accepted_through < span.begin ||
                failed->accepted_through > span.end) {
                return implementation.reject(
                    record, internal_error());
            }
            const auto cause =
                failed->cause ? failed->cause : internal_error();
            record->last_lease_outcome = LeaseOutcome{
                failed->lease,
                LeaseOutcomeKind::failed,
                failed->accepted_through,
                cause
            };
            record->active_lease.reset();
            record->dispatch_cursor = record->persisted_through;
            implementation.fail_record(*record, cause);
            return {
                EventDisposition::applied,
                implementation.first_error,
                false,
                true,
                {}
            };
        }
        if (record->last_lease_outcome.has_value() &&
            record->last_lease_outcome->lease == failed->lease) {
            const auto cause =
                failed->cause ? failed->cause : internal_error();
            if (record->last_lease_outcome->kind ==
                    LeaseOutcomeKind::failed &&
                record->last_lease_outcome->through ==
                    failed->accepted_through &&
                record->last_lease_outcome->cause == cause) {
                return {
                    EventDisposition::duplicate,
                    implementation.first_error,
                    false,
                    true,
                    {}
                };
            }
            return implementation.reject(record, internal_error());
        }
        if (failed->lease.generation <
            record->next_lease_generation) {
            return {EventDisposition::stale, {}, false, false, {}};
        }
        return implementation.reject(record, internal_error());
    }

    if (const auto* persisted =
            std::get_if<PersistedThrough>(&event)) {
        auto* record = implementation.find(persisted->range);
        if (record == nullptr) {
            return implementation.reject(nullptr, internal_error());
        }
        if (persisted->offset < record->persisted_through) {
            return {EventDisposition::stale, {}, false, false, {}};
        }
        if (persisted->offset == record->persisted_through) {
            return {
                EventDisposition::duplicate,
                {},
                false,
                false,
                {}
            };
        }
        if (persisted->offset > record->bytes.end ||
            (!is_terminal(record->phase) &&
             persisted->offset > record->dispatch_cursor)) {
            return implementation.reject(
                is_terminal(record->phase) ? nullptr : record,
                internal_error());
        }
        record->persisted_through = persisted->offset;
        if (record->phase == RangePhase::failed ||
            record->phase == RangePhase::cancelled) {
            record->dispatch_cursor = std::max(
                record->dispatch_cursor,
                record->persisted_through);
        }
        return {};
    }

    if (const auto* gap = std::get_if<GapPauseChanged>(&event)) {
        auto* record = implementation.find(gap->range);
        if (record == nullptr) {
            return implementation.reject(nullptr, internal_error());
        }
        if (record->gap_blocked == gap->active) {
            return {
                EventDisposition::duplicate,
                {},
                false,
                false,
                {}
            };
        }
        record->gap_blocked = gap->active;
        return {
            EventDisposition::applied,
            {},
            !gap->active && !is_terminal(record->phase),
            false,
            {}
        };
    }

    if (const auto* committed =
            std::get_if<PersistenceCommitted>(&event)) {
        auto* record =
            implementation.find(committed->completion.range);
        if (record == nullptr) {
            return implementation.reject(nullptr, internal_error());
        }
        if (record->phase == RangePhase::failed ||
            record->phase == RangePhase::cancelled) {
            return {EventDisposition::stale, {}, false, false, {}};
        }
        if (record->phase == RangePhase::finished) {
            if (record->completion == committed->completion &&
                committed->persisted_through ==
                    record->bytes.end) {
                return {
                    EventDisposition::duplicate,
                    {},
                    false,
                    false,
                    {}
                };
            }
            return implementation.reject(nullptr, internal_error());
        }
        if (!record->completion.has_value()) {
            if (record->last_lease_outcome.has_value() &&
                committed->completion.generation <
                    record->last_lease_outcome
                        ->lease.generation) {
                return {
                    EventDisposition::stale,
                    {},
                    false,
                    false,
                    {}
                };
            }
            return implementation.reject(record, internal_error());
        }
        if (committed->completion.generation <
            record->completion->generation) {
            return {EventDisposition::stale, {}, false, false, {}};
        }
        if (record->phase !=
                RangePhase::awaiting_persistence ||
            record->completion != committed->completion ||
            committed->persisted_through != record->bytes.end ||
            record->persisted_through != record->bytes.end) {
            return implementation.reject(record, internal_error());
        }
        record->phase = RangePhase::finished;
        ++implementation.finished_ranges;
        return {};
    }

    if (const auto* persistence_failure =
            std::get_if<PersistenceFailed>(&event)) {
        const auto error = persistence_failure->cause ?
            persistence_failure->cause : internal_error();
        if (persistence_failure->range.has_value()) {
            auto* record =
                implementation.find(*persistence_failure->range);
            if (record == nullptr) {
                return implementation.reject(
                    nullptr, internal_error());
            }
            if (record->phase != RangePhase::finished) {
                implementation.fail_record(*record, error);
            } else {
                implementation.remember_error(error);
            }
        } else {
            implementation.fail_unfinished(error);
        }
        return {
            EventDisposition::applied,
            implementation.first_error,
            false,
            true,
            {}
        };
    }

    if (std::holds_alternative<CancelRequested>(event)) {
        bool changed = false;
        for (auto& record : implementation.ranges) {
            if (record != nullptr &&
                !is_terminal(record->phase)) {
                record->phase = RangePhase::cancelled;
                record->active_lease.reset();
                changed = true;
            }
        }
        return {
            changed ? EventDisposition::applied :
                EventDisposition::duplicate,
            {},
            false,
            changed,
            {}
        };
    }

    const auto& effect_failure =
        std::get<EffectApplicationFailed>(event);
    const auto error = effect_failure.cause ?
        effect_failure.cause : internal_error();
    implementation.fail_unfinished(error);
    return {
        EventDisposition::applied,
        implementation.first_error,
        false,
        true,
        {}
    };
}

ApplyResult RangeLifecycle::drain_persistence_facts() noexcept {
    auto& implementation = *implementation_;
    assert(implementation.owner_thread_matches());
    if (!implementation.owner_thread_matches()) {
        return implementation.reject(nullptr, internal_error());
    }

    ApplyResult combined;
    for (auto& record_ptr : implementation.ranges) {
        auto* record = record_ptr.get();
        if (record == nullptr ||
            record->fact_slot == nullptr) {
            return implementation.reject(
                record,
                internal_error());
        }
        const auto fact = record->fact_slot->read_since(
            record->last_fact_revision);
        if (!fact.has_value()) {
            continue;
        }
        record->last_fact_revision = fact->revision;

        const auto merge =
            [&combined](const ApplyResult& applied) {
                combined.disposition =
                    applied.disposition;
                combined.scheduler_may_run =
                    combined.scheduler_may_run ||
                    applied.scheduler_may_run;
                combined.task_should_stop =
                    combined.task_should_stop ||
                    applied.task_should_stop;
                if (!combined.error && applied.error) {
                    combined.error = applied.error;
                }
            };

        if (fact->persisted_through >
            record->persisted_through) {
            const auto applied = apply(
                PersistedThrough{
                    record->id,
                    fact->persisted_through
                });
            merge(applied);
            if (applied.error) {
                return combined;
            }
        }
        if (fact->gap_paused !=
            record->gap_blocked) {
            const auto applied = apply(
                GapPauseChanged{
                    record->id,
                    fact->gap_paused
                });
            merge(applied);
            if (applied.error) {
                return combined;
            }
        }
        if (fact->committed_generation != 0 &&
            fact->committed_generation !=
                record->observed_committed_generation) {
            const auto applied = apply(
                PersistenceCommitted{
                    {
                        record->id,
                        fact->committed_generation
                    },
                    fact->persisted_through
                });
            merge(applied);
            if (applied.error) {
                return combined;
            }
            record->observed_committed_generation =
                fact->committed_generation;
        }
    }
    return combined;
}

SnapshotResult RangeLifecycle::snapshot() const noexcept {
    const auto& implementation = *implementation_;
    assert(implementation.owner_thread_matches());
    SnapshotResult result;
    if (!implementation.owner_thread_matches()) {
        result.error = internal_error();
        return result;
    }
    try {
#if defined(ASYNCDOWNLOAD_RANGE_LIFECYCLE_FAULT_TEST)
        detail::fail_if_requested(
            detail::range_fault_plan()
                .fail_next_snapshot_allocation);
#endif
        result.value.ranges.reserve(
            implementation.ranges.size());
        for (const auto& record : implementation.ranges) {
            if (record == nullptr) {
                result.error = internal_error();
                result.value = {};
                return result;
            }
            result.value.ranges.push_back({
                record->id,
                record->bytes,
                record->dispatch_cursor,
                record->persisted_through,
                record->phase,
                record->gap_blocked,
                record->active_lease.has_value() ?
                    std::optional<LeaseId>{
                        record->active_lease->id
                    } :
                    std::nullopt
            });
            if (!implementation.first_error &&
                !record->gap_blocked &&
                !is_terminal(record->phase) &&
                record->dispatch_cursor < record->bytes.end) {
                result.value.has_schedulable_work = true;
            }
        }
        result.value.finished_ranges =
            implementation.finished_ranges;
        result.value.all_finished =
            !implementation.ranges.empty() &&
            implementation.finished_ranges ==
                implementation.ranges.size();
        return result;
    } catch (const std::bad_alloc&) {
        result.value = {};
        result.error =
            std::make_error_code(std::errc::not_enough_memory);
        return result;
    } catch (...) {
        result.value = {};
        result.error = internal_error();
        return result;
    }
}

}
