#include <chrono>
#include <cstdint>
#include <type_traits>

#include <gtest/gtest.h>

#include "asyncdownload/telemetry.hpp"

namespace {

using asyncdownload::PerformanceSummary;
using asyncdownload::ProgressSnapshot;
using asyncdownload::telemetry::TelemetryClock;
using asyncdownload::telemetry::TelemetryCollector;
using asyncdownload::telemetry::TelemetryPauseReason;
using asyncdownload::telemetry::TelemetrySession;

using SessionStartedMember =
    void (TelemetrySession::*)(TelemetryClock::time_point) noexcept;
using SessionFirstByteMember =
    void (TelemetrySession::*)(TelemetryClock::time_point) noexcept;
using SessionDownloadMember =
    void (TelemetrySession::*)(std::uint64_t) noexcept;
using SessionPersistMember =
    void (TelemetrySession::*)(std::uint64_t) noexcept;
using SessionPauseMember =
    void (TelemetrySession::*)(TelemetryPauseReason, bool) noexcept;
using SessionMemoryMember =
    void (TelemetrySession::*)(std::uint64_t) noexcept;
using SessionCompletedMember =
    void (TelemetrySession::*)(TelemetryClock::time_point) noexcept;
using SessionSnapshotMember =
    ProgressSnapshot (TelemetrySession::*)() const noexcept;
using SessionSummaryMember =
    PerformanceSummary (TelemetrySession::*)(TelemetryClock::time_point)
        const noexcept;

using CollectorStartedMember =
    void (TelemetryCollector::*)(TelemetryClock::time_point) noexcept;
using CollectorFirstByteMember =
    void (TelemetryCollector::*)(TelemetryClock::time_point) noexcept;
using CollectorDownloadMember =
    void (TelemetryCollector::*)(
        std::uint64_t,
        TelemetryClock::time_point) noexcept;
using CollectorPersistMember =
    void (TelemetryCollector::*)(
        std::uint64_t,
        TelemetryClock::time_point) noexcept;
using CollectorPauseMember =
    void (TelemetryCollector::*)(
        TelemetryPauseReason,
        bool,
        TelemetryClock::time_point) noexcept;
using CollectorMemoryMember =
    void (TelemetryCollector::*)(
        std::uint64_t,
        TelemetryClock::time_point) noexcept;
using CollectorCompletedMember =
    void (TelemetryCollector::*)(TelemetryClock::time_point) noexcept;
using CollectorSnapshotMember =
    ProgressSnapshot (TelemetryCollector::*)() const noexcept;
using CollectorSummaryMember =
    PerformanceSummary (TelemetryCollector::*)(TelemetryClock::time_point)
        const noexcept;

static_assert(std::is_same_v<
    decltype(static_cast<SessionStartedMember>(
        &TelemetrySession::record_task_started)),
    SessionStartedMember>);
static_assert(std::is_same_v<
    decltype(static_cast<SessionFirstByteMember>(
        &TelemetrySession::record_first_byte_received)),
    SessionFirstByteMember>);
static_assert(std::is_same_v<
    decltype(static_cast<SessionDownloadMember>(
        &TelemetrySession::record_download_delta)),
    SessionDownloadMember>);
static_assert(std::is_same_v<
    decltype(static_cast<SessionPersistMember>(
        &TelemetrySession::record_persist_delta)),
    SessionPersistMember>);
static_assert(std::is_same_v<
    decltype(static_cast<SessionPauseMember>(
        &TelemetrySession::record_pause)),
    SessionPauseMember>);
static_assert(std::is_same_v<
    decltype(static_cast<SessionMemoryMember>(
        &TelemetrySession::record_memory_sample)),
    SessionMemoryMember>);
static_assert(std::is_same_v<
    decltype(static_cast<SessionCompletedMember>(
        &TelemetrySession::record_task_completed)),
    SessionCompletedMember>);
static_assert(std::is_same_v<
    decltype(static_cast<SessionSnapshotMember>(
        &TelemetrySession::current_snapshot)),
    SessionSnapshotMember>);
static_assert(std::is_same_v<
    decltype(static_cast<SessionSummaryMember>(
        &TelemetrySession::final_summary)),
    SessionSummaryMember>);

static_assert(std::is_same_v<
    decltype(static_cast<CollectorStartedMember>(
        &TelemetryCollector::record_task_started)),
    CollectorStartedMember>);
static_assert(std::is_same_v<
    decltype(static_cast<CollectorFirstByteMember>(
        &TelemetryCollector::record_first_byte_received)),
    CollectorFirstByteMember>);
static_assert(std::is_same_v<
    decltype(static_cast<CollectorDownloadMember>(
        &TelemetryCollector::record_download_delta)),
    CollectorDownloadMember>);
static_assert(std::is_same_v<
    decltype(static_cast<CollectorPersistMember>(
        &TelemetryCollector::record_persist_delta)),
    CollectorPersistMember>);
static_assert(std::is_same_v<
    decltype(static_cast<CollectorPauseMember>(
        &TelemetryCollector::record_pause)),
    CollectorPauseMember>);
static_assert(std::is_same_v<
    decltype(static_cast<CollectorMemoryMember>(
        &TelemetryCollector::record_memory_sample)),
    CollectorMemoryMember>);
static_assert(std::is_same_v<
    decltype(static_cast<CollectorCompletedMember>(
        &TelemetryCollector::record_task_completed)),
    CollectorCompletedMember>);
static_assert(std::is_same_v<
    decltype(static_cast<CollectorSnapshotMember>(
        &TelemetryCollector::current_snapshot)),
    CollectorSnapshotMember>);
static_assert(std::is_same_v<
    decltype(static_cast<CollectorSummaryMember>(
        &TelemetryCollector::final_summary)),
    CollectorSummaryMember>);

static_assert(
    std::is_same_v<
        std::underlying_type_t<TelemetryPauseReason>,
        std::uint8_t>);
static_assert(
    static_cast<std::uint8_t>(TelemetryPauseReason::none) == 0);
static_assert(
    static_cast<std::uint8_t>(TelemetryPauseReason::queue_full) == 1);
static_assert(
    static_cast<std::uint8_t>(TelemetryPauseReason::memory_pressure) == 2);
static_assert(
    static_cast<std::uint8_t>(TelemetryPauseReason::gap) == 3);

TEST(TelemetryPublicCompatibilityTest, DefaultConstructsBothPublicClasses) {
    TelemetrySession session;
    TelemetryCollector collector;

    EXPECT_EQ(session.final_summary().total_pause_count, 0U);
    EXPECT_EQ(collector.final_summary().total_pause_count, 0U);
}

TEST(TelemetryPublicCompatibilityTest, KeepsTimestampConversionContract) {
    const auto timestamp =
        TelemetryClock::time_point{std::chrono::nanoseconds(123456)};

    EXPECT_EQ(
        asyncdownload::telemetry::telemetry_timestamp_ns(timestamp),
        123456U);
}

}
