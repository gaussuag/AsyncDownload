#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

#include "asyncdownload/telemetry/telemetry_session.hpp"

namespace asyncdownload::telemetry {

struct TelemetrySessionTestAccess {
    static void seed_counts(
        TelemetrySession& session,
        const std::size_t packets,
        const std::uint64_t packet_bytes,
        const std::size_t pauses,
        const std::size_t queue_pauses) {
        std::scoped_lock lock(session.state_mutex_);
        session.state_.packets_enqueued_total = packets;
        session.state_.total_packet_bytes = packet_bytes;
        session.state_.total_pause_count = pauses;
        session.state_.queue_full_pause_count =
            queue_pauses;
    }
};

}

namespace {

using asyncdownload::telemetry::TelemetryClock;
using asyncdownload::telemetry::TelemetryPauseReason;
using asyncdownload::telemetry::TelemetrySession;
using asyncdownload::telemetry::TelemetrySessionTestAccess;

TEST(TelemetrySaturationTest, SaturatesByteTotalsAndPublicProjections) {
    TelemetrySession session;
    const auto start =
        TelemetryClock::time_point{std::chrono::seconds(1)};
    session.record_task_started(start);

    session.record_download_delta_at(
        std::numeric_limits<std::uint64_t>::max(),
        start + std::chrono::seconds(1));
    session.record_download_delta_at(
        1,
        start + std::chrono::seconds(2));

    auto snapshot = session.current_snapshot();
    auto summary =
        session.final_summary(
            start + std::chrono::seconds(3));
    EXPECT_EQ(
        snapshot.downloaded_bytes,
        std::numeric_limits<std::int64_t>::max());
    EXPECT_EQ(
        snapshot.inflight_bytes,
        std::numeric_limits<std::int64_t>::max());
    EXPECT_EQ(
        summary.max_inflight_bytes,
        std::numeric_limits<std::int64_t>::max());
    EXPECT_EQ(summary.packets_enqueued_total, 2U);
    EXPECT_EQ(
        summary.max_packet_size_bytes,
        std::numeric_limits<std::size_t>::max());
    EXPECT_DOUBLE_EQ(
        summary.average_packet_size_bytes,
        static_cast<double>(
            std::numeric_limits<std::uint64_t>::max()) /
            2.0);

    session.record_persist_delta_at(
        std::numeric_limits<std::uint64_t>::max(),
        start + std::chrono::seconds(3));
    session.record_persist_delta_at(
        1,
        start + std::chrono::seconds(4));

    snapshot = session.current_snapshot();
    summary =
        session.final_summary(
            start + std::chrono::seconds(5));
    EXPECT_EQ(
        snapshot.persisted_bytes,
        std::numeric_limits<std::int64_t>::max());
    EXPECT_EQ(snapshot.inflight_bytes, 0);
    EXPECT_TRUE(std::isfinite(
        snapshot.network_bytes_per_second));
    EXPECT_TRUE(std::isfinite(
        snapshot.disk_bytes_per_second));
    EXPECT_TRUE(std::isfinite(
        summary.average_network_bytes_per_second));
    EXPECT_TRUE(std::isfinite(
        summary.average_disk_bytes_per_second));
    EXPECT_GE(snapshot.network_bytes_per_second, 0.0);
    EXPECT_GE(snapshot.disk_bytes_per_second, 0.0);
}

TEST(TelemetrySaturationTest, SaturatesMemoryNarrowingAndKeepsPeak) {
    TelemetrySession session;
    const auto start =
        TelemetryClock::time_point{std::chrono::seconds(1)};
    session.record_task_started(start);

    session.record_memory_sample_at(
        std::numeric_limits<std::uint64_t>::max(),
        start + std::chrono::seconds(1));
    session.record_memory_sample_at(
        1,
        start + std::chrono::seconds(2));

    const auto snapshot = session.current_snapshot();
    const auto summary = session.final_summary(
        start + std::chrono::seconds(3));
    EXPECT_EQ(snapshot.memory_bytes, 1U);
    EXPECT_EQ(
        summary.max_memory_bytes,
        std::numeric_limits<std::size_t>::max());
}

TEST(TelemetrySaturationTest, SaturatesSeededCountsAndStaysSaturated) {
    TelemetrySession session;
    const auto start =
        TelemetryClock::time_point{std::chrono::seconds(1)};
    session.record_task_started(start);
    TelemetrySessionTestAccess::seed_counts(
        session,
        std::numeric_limits<std::size_t>::max() - 1,
        std::numeric_limits<std::uint64_t>::max() - 1,
        std::numeric_limits<std::size_t>::max() - 1,
        std::numeric_limits<std::size_t>::max() - 1);

    session.record_download_delta_at(
        2,
        start + std::chrono::seconds(1));
    session.record_download_delta_at(
        2,
        start + std::chrono::seconds(2));
    session.record_pause_at(
        TelemetryPauseReason::queue_full,
        true,
        start + std::chrono::seconds(3));
    session.record_pause_at(
        TelemetryPauseReason::queue_full,
        true,
        start + std::chrono::seconds(4));

    const auto summary =
        session.final_summary(
            start + std::chrono::seconds(5));
    EXPECT_EQ(
        summary.packets_enqueued_total,
        std::numeric_limits<std::size_t>::max());
    EXPECT_EQ(
        summary.total_pause_count,
        std::numeric_limits<std::size_t>::max());
    EXPECT_EQ(
        summary.queue_full_pause_count,
        std::numeric_limits<std::size_t>::max());
    EXPECT_DOUBLE_EQ(
        summary.average_packet_size_bytes,
        1.0);
}

}
