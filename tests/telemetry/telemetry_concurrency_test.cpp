#include <atomic>
#include <chrono>
#include <cstdint>
#include <latch>
#include <thread>

#include <gtest/gtest.h>

#include "asyncdownload/telemetry/telemetry_session.hpp"

namespace {

using asyncdownload::telemetry::TelemetryClock;
using asyncdownload::telemetry::TelemetryPauseReason;
using asyncdownload::telemetry::TelemetrySession;

TEST(
    TelemetryConcurrencyTest,
    AggregatesConcurrentModuleRecordsExactly) {
    constexpr std::size_t EVENT_COUNT = 10'000;
    TelemetrySession session;
    TelemetrySession independent;
    const auto start =
        TelemetryClock::time_point{std::chrono::seconds(1)};
    session.record_task_started(start);
    independent.record_task_started(start);
    std::latch ready(4);
    std::latch begin(1);
    std::atomic<bool> producers_done{false};
    std::atomic<bool> query_consistent{true};

    std::thread packet_flow([&]() {
        ready.count_down();
        begin.wait();
        for (std::size_t index = 0;
             index < EVENT_COUNT;
             ++index) {
            session.record_download_delta_at(
                64,
                start + std::chrono::seconds(1));
            session.record_memory_sample_at(
                index,
                start + std::chrono::seconds(1));
        }
    });
    std::thread persistence([&]() {
        ready.count_down();
        begin.wait();
        for (std::size_t index = 0;
             index < EVENT_COUNT;
             ++index) {
            session.record_persist_delta_at(
                32,
                start + std::chrono::seconds(2));
        }
    });
    std::thread http([&]() {
        ready.count_down();
        begin.wait();
        session.record_first_byte_received(
            start + std::chrono::milliseconds(2));
        session.record_first_byte_received(
            start + std::chrono::milliseconds(1));
        for (std::size_t index = 0;
             index < EVENT_COUNT;
             ++index) {
            session.record_pause_at(
                TelemetryPauseReason::gap,
                false,
                start + std::chrono::seconds(3));
        }
    });
    std::thread query([&]() {
        ready.count_down();
        begin.wait();
        while (!producers_done.load(
            std::memory_order_acquire)) {
            const auto snapshot =
                session.current_snapshot();
            const auto summary =
                session.final_summary(
                    start + std::chrono::seconds(3));
            if (snapshot.downloaded_bytes < 0 ||
                snapshot.persisted_bytes < 0 ||
                snapshot.inflight_bytes < 0 ||
                summary.packets_enqueued_total >
                    EVENT_COUNT ||
                summary.total_pause_count >
                    EVENT_COUNT) {
                query_consistent.store(
                    false,
                    std::memory_order_release);
            }
        }
    });

    ready.wait();
    begin.count_down();
    packet_flow.join();
    persistence.join();
    http.join();
    producers_done.store(
        true,
        std::memory_order_release);
    query.join();
    EXPECT_TRUE(query_consistent.load(
        std::memory_order_acquire));

    const auto completed_at =
        start + std::chrono::seconds(4);
    session.record_task_completed(completed_at);
    const auto before = session.current_snapshot();
    const auto summary =
        session.final_summary(completed_at);
    EXPECT_EQ(
        before.downloaded_bytes,
        static_cast<std::int64_t>(
            EVENT_COUNT * 64));
    EXPECT_EQ(
        before.persisted_bytes,
        static_cast<std::int64_t>(
            EVENT_COUNT * 32));
    EXPECT_EQ(
        before.inflight_bytes,
        static_cast<std::int64_t>(
            EVENT_COUNT * 32));
    EXPECT_EQ(before.memory_bytes, EVENT_COUNT - 1);
    EXPECT_EQ(
        summary.packets_enqueued_total,
        EVENT_COUNT);
    EXPECT_EQ(summary.total_pause_count, EVENT_COUNT);
    EXPECT_EQ(summary.queue_full_pause_count, 0U);
    EXPECT_EQ(summary.max_memory_bytes, EVENT_COUNT - 1);
    EXPECT_EQ(summary.time_to_first_byte_ms, 1);

    std::thread late_packet([&]() {
        session.record_download_delta(999);
    });
    std::thread late_persistence([&]() {
        session.record_persist_delta(999);
    });
    std::thread late_http([&]() {
        session.record_pause(
            TelemetryPauseReason::queue_full,
            true);
    });
    late_packet.join();
    late_persistence.join();
    late_http.join();

    const auto after = session.current_snapshot();
    const auto after_summary =
        session.final_summary(
            start + std::chrono::seconds(100));
    EXPECT_EQ(after.downloaded_bytes, before.downloaded_bytes);
    EXPECT_EQ(after.persisted_bytes, before.persisted_bytes);
    EXPECT_EQ(
        after_summary.packets_enqueued_total,
        summary.packets_enqueued_total);
    EXPECT_EQ(
        after_summary.total_pause_count,
        summary.total_pause_count);

    independent.record_download_delta_at(
        7,
        start + std::chrono::seconds(1));
    EXPECT_EQ(
        independent.current_snapshot().downloaded_bytes,
        7);
    EXPECT_EQ(
        session.current_snapshot().downloaded_bytes,
        before.downloaded_bytes);
}

}
