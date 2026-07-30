#include <array>
#include <initializer_list>
#include <memory>
#include <system_error>
#include <utility>

#include <gtest/gtest.h>

#include "download/download_policy.hpp"
#include "range/range_lifecycle.hpp"

namespace {

using asyncdownload::download::SchedulingPolicy;
using asyncdownload::range::ByteSpan;
using asyncdownload::range::CancelRequested;
using asyncdownload::range::CompletionId;
using asyncdownload::range::EventDisposition;
using asyncdownload::range::GapPauseChanged;
using asyncdownload::range::LeaseFailed;
using asyncdownload::range::LeaseId;
using asyncdownload::range::LeaseSucceeded;
using asyncdownload::range::PersistenceCommitted;
using asyncdownload::range::PersistedThrough;
using asyncdownload::range::PublishRangeCompleteEffect;
using asyncdownload::range::RangeId;
using asyncdownload::range::RangeLifecycle;
using asyncdownload::range::RangeLifecycleCreation;
using asyncdownload::range::RangePhase;
using asyncdownload::range::RegisterRangeEffect;
using asyncdownload::range::ResizeRangeEffect;

SchedulingPolicy range_policy() {
    return {4, 128, 64, true, false};
}

RangeLifecycleCreation create_lifecycle(
    const std::int64_t total_size,
    const std::initializer_list<ByteSpan> ranges,
    const SchedulingPolicy& policy = range_policy()) {
    return RangeLifecycle::create(
        total_size,
        policy,
        {ranges.begin(), ranges.size()});
}

std::unique_ptr<RangeLifecycle> require_lifecycle(
    RangeLifecycleCreation creation) {
    EXPECT_FALSE(creation.error);
    EXPECT_NE(creation.value, nullptr);
    return std::move(creation.value);
}

}

TEST(RangeLifecycleTest, CreatesDisjointInitialRanges) {
    auto creation = create_lifecycle(
        512,
        {{0, 128}, {256, 512}});

    ASSERT_FALSE(creation.error);
    ASSERT_NE(creation.value, nullptr);
    ASSERT_EQ(creation.effects.values.size(), 2U);
    ASSERT_TRUE(std::holds_alternative<RegisterRangeEffect>(
        creation.effects.values[0]));
    EXPECT_EQ(
        std::get<RegisterRangeEffect>(creation.effects.values[0]).bytes,
        (ByteSpan{0, 128}));
    const auto snapshot = creation.value->snapshot();
    ASSERT_FALSE(snapshot.error);
    ASSERT_EQ(snapshot.value.ranges.size(), 2U);
    EXPECT_EQ(snapshot.value.ranges[0].id, (RangeId{0}));
    EXPECT_EQ(snapshot.value.ranges[1].id, (RangeId{1}));
}

TEST(RangeLifecycleTest, RejectsEmptyOverlappingOrOutOfBoundsRanges) {
    const auto policy = range_policy();
    const std::array<ByteSpan, 0> none{};
    const auto empty =
        RangeLifecycle::create(512, policy, none);
    const auto zero = create_lifecycle(512, {{64, 64}});
    const auto overlapping = create_lifecycle(
        512,
        {{0, 256}, {128, 384}});
    const auto beyond = create_lifecycle(512, {{0, 576}});
    const auto unaligned = create_lifecycle(512, {{1, 128}});

    EXPECT_TRUE(empty.error);
    EXPECT_TRUE(zero.error);
    EXPECT_TRUE(overlapping.error);
    EXPECT_TRUE(beyond.error);
    EXPECT_TRUE(unaligned.error);
}

TEST(RangeLifecycleTest, AcquiresOneActiveLeasePerRange) {
    auto lifecycle = require_lifecycle(create_lifecycle(
        512,
        {{0, 256}, {256, 512}}));

    const auto first = lifecycle->acquire();
    const auto second = lifecycle->acquire();
    const auto exhausted = lifecycle->acquire();

    ASSERT_TRUE(first.lease.has_value());
    ASSERT_TRUE(second.lease.has_value());
    EXPECT_EQ(first.lease->id.range, (RangeId{0}));
    EXPECT_EQ(second.lease->id.range, (RangeId{1}));
    EXPECT_FALSE(exhausted.lease.has_value());
    EXPECT_FALSE(exhausted.error);
}

TEST(
    RangeLifecycleTest,
    SignsFirstLeaseAtGenerationOneAndAdvancesDispatchCursorToLeaseEnd) {
    auto lifecycle =
        require_lifecycle(create_lifecycle(256, {{0, 256}}));

    const auto acquired = lifecycle->acquire();
    const auto snapshot = lifecycle->snapshot();

    ASSERT_TRUE(acquired.lease.has_value());
    EXPECT_EQ(acquired.lease->id, (LeaseId{{0}, 1}));
    EXPECT_EQ(acquired.lease->bytes, (ByteSpan{0, 128}));
    ASSERT_FALSE(snapshot.error);
    EXPECT_EQ(snapshot.value.ranges[0].dispatch_cursor, 128);
    EXPECT_EQ(snapshot.value.ranges[0].phase, RangePhase::leased);
}

TEST(RangeLifecycleTest, IncrementsGenerationWithoutReuse) {
    auto lifecycle =
        require_lifecycle(create_lifecycle(256, {{0, 256}}));
    const auto first = lifecycle->acquire();
    ASSERT_TRUE(first.lease.has_value());
    ASSERT_FALSE(lifecycle->apply(
        LeaseSucceeded{first.lease->id, 128}).error);

    const auto second = lifecycle->acquire();

    ASSERT_TRUE(second.lease.has_value());
    EXPECT_EQ(second.lease->id.generation, 2U);
    EXPECT_NE(second.lease->id, first.lease->id);
}

TEST(RangeLifecycleTest, RequeuesRangeAfterNonfinalLeaseSuccess) {
    auto lifecycle =
        require_lifecycle(create_lifecycle(256, {{0, 256}}));
    const auto lease = lifecycle->acquire().lease;
    ASSERT_TRUE(lease.has_value());

    const auto applied = lifecycle->apply(
        LeaseSucceeded{lease->id, lease->bytes.end});
    const auto snapshot = lifecycle->snapshot();

    EXPECT_EQ(applied.disposition, EventDisposition::applied);
    EXPECT_TRUE(applied.scheduler_may_run);
    EXPECT_EQ(applied.effects.size, 0U);
    ASSERT_FALSE(snapshot.error);
    EXPECT_EQ(snapshot.value.ranges[0].phase, RangePhase::ready);
}

TEST(RangeLifecycleTest, WaitsForPersistenceAfterFinalLeaseSuccess) {
    auto lifecycle =
        require_lifecycle(create_lifecycle(128, {{0, 128}}));
    const auto lease = lifecycle->acquire().lease;
    ASSERT_TRUE(lease.has_value());

    const auto applied = lifecycle->apply(
        LeaseSucceeded{lease->id, lease->bytes.end});
    const auto snapshot = lifecycle->snapshot();

    ASSERT_EQ(applied.effects.size, 1U);
    EXPECT_TRUE(std::holds_alternative<PublishRangeCompleteEffect>(
        applied.effects.values[0]));
    EXPECT_EQ(
        snapshot.value.ranges[0].phase,
        RangePhase::awaiting_persistence);
    EXPECT_FALSE(snapshot.value.all_finished);
}

TEST(
    RangeLifecycleTest,
    MarksFinishedOnlyAfterMatchingPersistenceCommit) {
    auto lifecycle =
        require_lifecycle(create_lifecycle(128, {{0, 128}}));
    const auto lease = lifecycle->acquire().lease;
    ASSERT_TRUE(lease.has_value());
    const auto closed = lifecycle->apply(
        LeaseSucceeded{lease->id, 128});
    ASSERT_EQ(closed.effects.size, 1U);
    const auto completion = std::get<PublishRangeCompleteEffect>(
        closed.effects.values[0]).completion;
    ASSERT_FALSE(
        lifecycle->apply(PersistedThrough{{0}, 128}).error);

    const auto before = lifecycle->snapshot();
    const auto committed = lifecycle->apply(
        PersistenceCommitted{completion, 128});
    const auto after = lifecycle->snapshot();

    EXPECT_FALSE(before.value.all_finished);
    EXPECT_EQ(committed.disposition, EventDisposition::applied);
    EXPECT_TRUE(after.value.all_finished);
    EXPECT_EQ(after.value.finished_ranges, 1U);
    EXPECT_EQ(after.value.ranges[0].phase, RangePhase::finished);
}

TEST(RangeLifecycleTest, ReturnsUnpersistedSuffixAfterLeaseFailure) {
    auto lifecycle =
        require_lifecycle(create_lifecycle(256, {{0, 256}}));
    const auto lease = lifecycle->acquire().lease;
    ASSERT_TRUE(lease.has_value());
    ASSERT_FALSE(
        lifecycle->apply(PersistedThrough{{0}, 64}).error);
    const auto cause =
        std::make_error_code(std::errc::connection_reset);

    const auto failed = lifecycle->apply(
        LeaseFailed{lease->id, 96, cause});
    const auto snapshot = lifecycle->snapshot();

    EXPECT_EQ(failed.disposition, EventDisposition::applied);
    EXPECT_EQ(failed.error, cause);
    EXPECT_TRUE(failed.task_should_stop);
    EXPECT_EQ(snapshot.value.ranges[0].dispatch_cursor, 64);
    EXPECT_EQ(snapshot.value.ranges[0].persisted_through, 64);
    EXPECT_EQ(snapshot.value.ranges[0].phase, RangePhase::failed);
}

TEST(RangeLifecycleTest, IgnoresStaleLeaseSuccess) {
    auto lifecycle =
        require_lifecycle(create_lifecycle(384, {{0, 384}}));
    const auto first = lifecycle->acquire().lease;
    ASSERT_TRUE(first.has_value());
    ASSERT_FALSE(
        lifecycle->apply(LeaseSucceeded{first->id, 128}).error);
    const auto second = lifecycle->acquire().lease;
    ASSERT_TRUE(second.has_value());
    ASSERT_FALSE(
        lifecycle->apply(LeaseSucceeded{second->id, 256}).error);
    ASSERT_TRUE(lifecycle->acquire().lease.has_value());

    const auto stale = lifecycle->apply(
        LeaseSucceeded{first->id, 128});

    EXPECT_EQ(stale.disposition, EventDisposition::stale);
    EXPECT_FALSE(stale.error);
}

TEST(
    RangeLifecycleTest,
    TreatsIdenticalLeaseSuccessAsDuplicate) {
    auto lifecycle =
        require_lifecycle(create_lifecycle(256, {{0, 256}}));
    const auto lease = lifecycle->acquire().lease;
    ASSERT_TRUE(lease.has_value());
    ASSERT_FALSE(lifecycle->apply(
        LeaseSucceeded{lease->id, 128}).error);

    const auto duplicate = lifecycle->apply(
        LeaseSucceeded{lease->id, 128});

    EXPECT_EQ(
        duplicate.disposition,
        EventDisposition::duplicate);
    EXPECT_FALSE(duplicate.error);
}

TEST(RangeLifecycleTest, RejectsConflictingDuplicateOutcome) {
    auto lifecycle =
        require_lifecycle(create_lifecycle(256, {{0, 256}}));
    const auto lease = lifecycle->acquire().lease;
    ASSERT_TRUE(lease.has_value());
    ASSERT_FALSE(lifecycle->apply(
        LeaseSucceeded{lease->id, 128}).error);

    const auto conflicting = lifecycle->apply(
        LeaseFailed{
            lease->id,
            64,
            std::make_error_code(std::errc::io_error)
        });

    EXPECT_EQ(
        conflicting.disposition,
        EventDisposition::rejected);
    EXPECT_TRUE(conflicting.error);
    EXPECT_TRUE(conflicting.task_should_stop);
}

TEST(RangeLifecycleTest, RejectsFutureGeneration) {
    auto lifecycle =
        require_lifecycle(create_lifecycle(256, {{0, 256}}));
    ASSERT_TRUE(lifecycle->acquire().lease.has_value());

    const auto future = lifecycle->apply(
        LeaseSucceeded{{{0}, 2}, 128});

    EXPECT_EQ(future.disposition, EventDisposition::rejected);
    EXPECT_TRUE(future.error);
    EXPECT_TRUE(future.task_should_stop);
}

TEST(RangeLifecycleTest, AcceptsShortFinalWindow) {
    auto lifecycle =
        require_lifecycle(create_lifecycle(192, {{0, 192}}));
    const auto first = lifecycle->acquire().lease;
    ASSERT_TRUE(first.has_value());
    ASSERT_FALSE(
        lifecycle->apply(LeaseSucceeded{first->id, 128}).error);

    const auto final = lifecycle->acquire().lease;

    ASSERT_TRUE(final.has_value());
    EXPECT_EQ(final->bytes, (ByteSpan{128, 192}));
    const auto closed = lifecycle->apply(
        LeaseSucceeded{final->id, 192});
    EXPECT_FALSE(closed.error);
    EXPECT_EQ(closed.effects.size, 1U);
}

TEST(RangeLifecycleTest, AppliesGapFactWithoutChangingPhase) {
    auto lifecycle =
        require_lifecycle(create_lifecycle(256, {{0, 256}}));
    ASSERT_TRUE(lifecycle->acquire().lease.has_value());

    const auto applied =
        lifecycle->apply(GapPauseChanged{{0}, true});
    const auto snapshot = lifecycle->snapshot();

    EXPECT_EQ(applied.disposition, EventDisposition::applied);
    EXPECT_EQ(snapshot.value.ranges[0].phase, RangePhase::leased);
    EXPECT_TRUE(snapshot.value.ranges[0].gap_blocked);
}

TEST(RangeLifecycleTest, IgnoresDuplicateGapTransition) {
    auto lifecycle =
        require_lifecycle(create_lifecycle(256, {{0, 256}}));
    ASSERT_EQ(
        lifecycle->apply(GapPauseChanged{{0}, true}).disposition,
        EventDisposition::applied);

    const auto duplicate =
        lifecycle->apply(GapPauseChanged{{0}, true});

    EXPECT_EQ(
        duplicate.disposition,
        EventDisposition::duplicate);
}

TEST(RangeLifecycleTest, KeepsPersistedFrontierMonotonic) {
    auto lifecycle =
        require_lifecycle(create_lifecycle(256, {{0, 256}}));
    ASSERT_TRUE(lifecycle->acquire().lease.has_value());
    ASSERT_FALSE(
        lifecycle->apply(PersistedThrough{{0}, 96}).error);

    const auto stale =
        lifecycle->apply(PersistedThrough{{0}, 64});
    const auto snapshot = lifecycle->snapshot();

    EXPECT_EQ(stale.disposition, EventDisposition::stale);
    EXPECT_EQ(snapshot.value.ranges[0].persisted_through, 96);
}

TEST(RangeLifecycleTest, RejectsPersistedFrontierPastRangeEnd) {
    auto lifecycle =
        require_lifecycle(create_lifecycle(128, {{0, 128}}));
    ASSERT_TRUE(lifecycle->acquire().lease.has_value());

    const auto invalid =
        lifecycle->apply(PersistedThrough{{0}, 129});

    EXPECT_EQ(invalid.disposition, EventDisposition::rejected);
    EXPECT_TRUE(invalid.error);
    EXPECT_TRUE(invalid.task_should_stop);
}

TEST(
    RangeLifecycleTest,
    RecordsLatePersistedFactAfterFailureWithoutRetry) {
    auto lifecycle =
        require_lifecycle(create_lifecycle(256, {{0, 256}}));
    const auto lease = lifecycle->acquire().lease;
    ASSERT_TRUE(lease.has_value());
    ASSERT_TRUE(lifecycle->apply(
        LeaseFailed{
            lease->id,
            64,
            std::make_error_code(std::errc::io_error)
        }).error);

    const auto persisted =
        lifecycle->apply(PersistedThrough{{0}, 96});
    const auto snapshot = lifecycle->snapshot();

    EXPECT_FALSE(persisted.error);
    EXPECT_EQ(snapshot.value.ranges[0].persisted_through, 96);
    EXPECT_EQ(snapshot.value.ranges[0].dispatch_cursor, 96);
    EXPECT_EQ(snapshot.value.ranges[0].phase, RangePhase::failed);
    EXPECT_FALSE(lifecycle->acquire().lease.has_value());
}

TEST(RangeLifecycleTest, TreatsDuplicateCompletionAsIdempotent) {
    auto lifecycle =
        require_lifecycle(create_lifecycle(128, {{0, 128}}));
    const auto lease = lifecycle->acquire().lease;
    ASSERT_TRUE(lease.has_value());
    const auto closed = lifecycle->apply(
        LeaseSucceeded{lease->id, 128});
    const auto completion = std::get<PublishRangeCompleteEffect>(
        closed.effects.values[0]).completion;
    ASSERT_FALSE(
        lifecycle->apply(PersistedThrough{{0}, 128}).error);
    ASSERT_FALSE(lifecycle->apply(
        PersistenceCommitted{completion, 128}).error);

    const auto duplicate = lifecycle->apply(
        PersistenceCommitted{completion, 128});
    const auto snapshot = lifecycle->snapshot();

    EXPECT_EQ(
        duplicate.disposition,
        EventDisposition::duplicate);
    EXPECT_FALSE(duplicate.error);
    EXPECT_EQ(snapshot.value.finished_ranges, 1U);
}

TEST(RangeLifecycleTest, RejectsCompletionBeforeNetworkClose) {
    auto lifecycle =
        require_lifecycle(create_lifecycle(128, {{0, 128}}));
    ASSERT_TRUE(lifecycle->acquire().lease.has_value());
    ASSERT_FALSE(
        lifecycle->apply(PersistedThrough{{0}, 128}).error);

    const auto premature = lifecycle->apply(
        PersistenceCommitted{{{0}, 1}, 128});

    EXPECT_EQ(
        premature.disposition,
        EventDisposition::rejected);
    EXPECT_TRUE(premature.error);
}

TEST(
    RangeLifecycleTest,
    DoesNotFinishCancelledRangeFromLateCompletion) {
    auto lifecycle =
        require_lifecycle(create_lifecycle(128, {{0, 128}}));
    const auto lease = lifecycle->acquire().lease;
    ASSERT_TRUE(lease.has_value());
    const auto closed = lifecycle->apply(
        LeaseSucceeded{lease->id, 128});
    const auto completion = std::get<PublishRangeCompleteEffect>(
        closed.effects.values[0]).completion;
    ASSERT_FALSE(
        lifecycle->apply(PersistedThrough{{0}, 128}).error);
    ASSERT_TRUE(
        lifecycle->apply(CancelRequested{}).task_should_stop);

    const auto late = lifecycle->apply(
        PersistenceCommitted{completion, 128});
    const auto snapshot = lifecycle->snapshot();

    EXPECT_EQ(late.disposition, EventDisposition::stale);
    EXPECT_EQ(snapshot.value.ranges[0].phase, RangePhase::cancelled);
    EXPECT_FALSE(snapshot.value.all_finished);
}

TEST(
    RangeLifecycleTest,
    NeverReopensFailedCancelledOrFinishedRange) {
    auto failed =
        require_lifecycle(create_lifecycle(128, {{0, 128}}));
    const auto failed_lease = failed->acquire().lease;
    ASSERT_TRUE(failed_lease.has_value());
    ASSERT_TRUE(failed->apply(
        LeaseFailed{
            failed_lease->id,
            0,
            std::make_error_code(std::errc::io_error)
        }).error);

    auto cancelled =
        require_lifecycle(create_lifecycle(128, {{0, 128}}));
    ASSERT_TRUE(cancelled->acquire().lease.has_value());
    ASSERT_TRUE(
        cancelled->apply(CancelRequested{}).task_should_stop);

    auto finished =
        require_lifecycle(create_lifecycle(128, {{0, 128}}));
    const auto finished_lease = finished->acquire().lease;
    ASSERT_TRUE(finished_lease.has_value());
    const auto closed = finished->apply(
        LeaseSucceeded{finished_lease->id, 128});
    ASSERT_EQ(closed.effects.size, 1U);
    ASSERT_FALSE(finished->apply(PersistedThrough{{0}, 128}).error);
    const auto completion =
        std::get<PublishRangeCompleteEffect>(
            closed.effects.values[0]).completion;
    ASSERT_FALSE(finished->apply(
        PersistenceCommitted{completion, 128}).error);

    EXPECT_FALSE(failed->acquire().lease.has_value());
    EXPECT_FALSE(cancelled->acquire().lease.has_value());
    EXPECT_FALSE(finished->acquire().lease.has_value());
    EXPECT_EQ(
        failed->snapshot().value.ranges[0].phase,
        RangePhase::failed);
    EXPECT_EQ(
        cancelled->snapshot().value.ranges[0].phase,
        RangePhase::cancelled);
    EXPECT_EQ(
        finished->snapshot().value.ranges[0].phase,
        RangePhase::finished);
}

TEST(RangeLifecycleTest, EmitsGeometryEffectsBeforeStolenLease) {
    auto policy = range_policy();
    policy.connection_limit = 16;
    policy.allow_work_stealing = true;
    auto lifecycle = require_lifecycle(
        create_lifecycle(1024, {{0, 1024}}, policy));
    const auto first = lifecycle->acquire();
    ASSERT_TRUE(first.lease.has_value());

    const auto stolen = lifecycle->acquire();

    ASSERT_TRUE(stolen.lease.has_value());
    EXPECT_EQ(stolen.lease->id.range, (RangeId{1}));
    EXPECT_EQ(stolen.effects.size, 2U);
    EXPECT_TRUE(std::holds_alternative<ResizeRangeEffect>(
        stolen.effects.values[0]));
    EXPECT_TRUE(std::holds_alternative<RegisterRangeEffect>(
        stolen.effects.values[1]));
    const auto resized =
        std::get<ResizeRangeEffect>(stolen.effects.values[0]);
    const auto registered =
        std::get<RegisterRangeEffect>(stolen.effects.values[1]);
    EXPECT_EQ(resized.new_end, registered.bytes.begin);
    EXPECT_EQ(stolen.lease->bytes.begin, registered.bytes.begin);
}

TEST(
    RangeLifecycleTest,
    DrainsPersistedGapAndCompletionFactsInProtocolOrder) {
    auto creation = create_lifecycle(128, {{0, 128}});
    ASSERT_FALSE(creation.error);
    ASSERT_EQ(creation.effects.values.size(), 1U);
    auto facts = std::get<RegisterRangeEffect>(
        creation.effects.values[0]).facts;
    auto lifecycle = std::move(creation.value);
    ASSERT_NE(lifecycle, nullptr);
    const auto lease = lifecycle->acquire().lease;
    ASSERT_TRUE(lease.has_value());
    const auto closed = lifecycle->apply(
        LeaseSucceeded{lease->id, 128});
    ASSERT_FALSE(closed.error);

    facts.publish_gap_pause(true);
    facts.publish_gap_pause(false);
    facts.publish_persisted_through(128);
    ASSERT_FALSE(facts.publish_committed(
        {lease->id.range, lease->id.generation},
        128));
    const auto drained =
        lifecycle->drain_persistence_facts();
    const auto snapshot = lifecycle->snapshot();

    EXPECT_FALSE(drained.error);
    EXPECT_FALSE(snapshot.value.ranges[0].gap_blocked);
    EXPECT_EQ(
        snapshot.value.ranges[0].persisted_through,
        128);
    EXPECT_EQ(
        snapshot.value.ranges[0].phase,
        RangePhase::finished);
    EXPECT_TRUE(snapshot.value.all_finished);
}
