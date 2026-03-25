#include "asyncdownload/telemetry.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace {

using asyncdownload::telemetry::TelemetryClock;
using asyncdownload::telemetry::TelemetryCollector;
using asyncdownload::telemetry::TelemetryPauseReason;
using asyncdownload::telemetry::TelemetrySession;

TEST(TelemetrySkeletonTest, TimestampHelperUsesNanoseconds) {
    const auto timestamp = TelemetryClock::time_point{std::chrono::seconds(3)};
    EXPECT_EQ(asyncdownload::telemetry::telemetry_timestamp_ns(timestamp), 3000000000ULL);
}

TEST(TelemetrySkeletonTest, CollectorCanBeUsedWithoutBackgroundWorker) {
    TelemetryCollector collector;
    collector.record_task_started(TelemetryClock::time_point{std::chrono::seconds(1)});
    collector.record_download_delta(128U, TelemetryClock::time_point{std::chrono::seconds(2)});
    collector.record_pause(TelemetryPauseReason::gap,
        false,
        TelemetryClock::time_point{std::chrono::seconds(2)});
    collector.record_task_completed(TelemetryClock::time_point{std::chrono::seconds(3)});

    const auto summary = collector.final_summary();
    EXPECT_EQ(summary.packets_enqueued_total, 1U);
    EXPECT_EQ(summary.total_pause_count, 1U);
    EXPECT_EQ(summary.queue_full_pause_count, 0U);
}

TEST(TelemetrySkeletonTest, SessionRecordMethodsUpdateCollectorStateDirectly) {
    TelemetrySession session;

    session.record_task_started(TelemetryClock::time_point{std::chrono::seconds(1)});
    session.record_download_delta(128U);
    session.record_pause(TelemetryPauseReason::gap, false);
    session.record_task_completed(TelemetryClock::time_point{std::chrono::seconds(2)});

    const auto summary = session.final_summary();
    EXPECT_EQ(summary.packets_enqueued_total, 1U);
    EXPECT_EQ(summary.total_pause_count, 1U);
    EXPECT_EQ(summary.queue_full_pause_count, 0U);
}

} // namespace
