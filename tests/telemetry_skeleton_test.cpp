#include "asyncdownload/telemetry.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

namespace asyncdownload::telemetry {

struct TelemetrySessionTestAccess {
    static TelemetrySink& sink(TelemetrySession& session) noexcept {
        return session.sink_;
    }

    static TelemetryCollector& collector(TelemetrySession& session) noexcept {
        return session.collector_;
    }
};

} // namespace asyncdownload::telemetry

namespace {

using asyncdownload::telemetry::TelemetryCollector;
using asyncdownload::telemetry::TelemetryCompletionStatus;
using asyncdownload::telemetry::TelemetryEvent;
using asyncdownload::telemetry::TelemetryEventType;
using asyncdownload::telemetry::TelemetryPauseReason;
using asyncdownload::telemetry::TelemetryPayload;
using asyncdownload::telemetry::TelemetrySession;
using asyncdownload::telemetry::TelemetrySessionTestAccess;
using asyncdownload::telemetry::TelemetrySink;
using namespace std::chrono_literals;

TEST(TelemetrySkeletonTest, EventModelIsFixedSize) {
    const TelemetryEvent task_started{
        TelemetryEventType::task_started,
        1U,
        TelemetryPayload::task_started_payload(),
    };
    const TelemetryEvent first_byte{
        TelemetryEventType::first_byte_received,
        2U,
        TelemetryPayload::first_byte_received_payload(),
    };
    const TelemetryEvent download_delta{
        TelemetryEventType::download_delta,
        3U,
        TelemetryPayload::download_delta_payload(64U),
    };
    const TelemetryEvent persist_delta{
        TelemetryEventType::persist_delta,
        4U,
        TelemetryPayload::persist_delta_payload(32U),
    };
    const TelemetryEvent queue_paused{
        TelemetryEventType::queue_paused,
        5U,
        TelemetryPayload::queue_paused_payload(TelemetryPauseReason::queue_full, true),
    };
    const TelemetryEvent memory_sample{
        TelemetryEventType::memory_sample,
        6U,
        TelemetryPayload::memory_sample_payload(4096U),
    };
    const TelemetryEvent task_completed{
        TelemetryEventType::task_completed,
        7U,
        TelemetryPayload::task_completed_payload(TelemetryCompletionStatus::success),
    };

    EXPECT_LE(sizeof(TelemetryPayload), sizeof(std::uint64_t));
    EXPECT_LE(sizeof(TelemetryEvent), 24U);
    EXPECT_EQ(download_delta.payload.as_download_delta().bytes_count, 64U);
    EXPECT_EQ(persist_delta.payload.as_persist_delta().bytes_count, 32U);
    EXPECT_EQ(queue_paused.payload.as_queue_paused().reason, TelemetryPauseReason::queue_full);
    EXPECT_TRUE(queue_paused.payload.as_queue_paused().is_queue_full);
    EXPECT_EQ(memory_sample.payload.as_memory_sample().memory_bytes, 4096U);
    EXPECT_EQ(task_completed.payload.as_task_completed().status,
              TelemetryCompletionStatus::success);
    EXPECT_EQ(task_started.type, TelemetryEventType::task_started);
    EXPECT_EQ(first_byte.type, TelemetryEventType::first_byte_received);
}

TEST(TelemetrySkeletonTest, SinkPreservesFifoOrderIncludingProducerTokenEnqueue) {
    TelemetrySink sink;
    auto token = sink.make_producer_token();

    ASSERT_TRUE(sink.enqueue(TelemetryEvent{
        TelemetryEventType::download_delta,
        10U,
        TelemetryPayload::download_delta_payload(11U),
    }));
    ASSERT_TRUE(sink.enqueue_from_producer(token,
                                           TelemetryEvent{
                                               TelemetryEventType::persist_delta,
                                               20U,
                                               TelemetryPayload::persist_delta_payload(22U),
                                           }));

    TelemetryEvent first;
    TelemetryEvent second;

    ASSERT_TRUE(sink.wait_dequeue_timed(first, 50ms));
    ASSERT_TRUE(sink.wait_dequeue_timed(second, 50ms));
    EXPECT_FALSE(sink.try_dequeue(first));

    EXPECT_EQ(first.type, TelemetryEventType::download_delta);
    EXPECT_EQ(first.payload.as_download_delta().bytes_count, 11U);
    EXPECT_EQ(second.type, TelemetryEventType::persist_delta);
    EXPECT_EQ(second.payload.as_persist_delta().bytes_count, 22U);
}

TEST(TelemetrySkeletonTest, CollectorProcessesQueuedEventsAndStopsCleanly) {
    TelemetrySink sink;
    TelemetryCollector collector(sink);
    collector.start_consuming();

    ASSERT_TRUE(sink.enqueue(TelemetryEvent{
        TelemetryEventType::task_started,
        100U,
        TelemetryPayload::task_started_payload(),
    }));
    ASSERT_TRUE(sink.enqueue(TelemetryEvent{
        TelemetryEventType::task_completed,
        200U,
        TelemetryPayload::task_completed_payload(TelemetryCompletionStatus::success),
    }));

    const auto deadline = std::chrono::steady_clock::now() + 250ms;
    while (collector.processed_event_count() < 2U && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }

    collector.wait_until_drained();
    collector.stop_consuming();

    ASSERT_GE(collector.processed_event_count(), 2U);
    ASSERT_TRUE(collector.last_event_type().has_value());
    EXPECT_EQ(collector.last_event_type().value(), TelemetryEventType::task_completed);
}

TEST(TelemetrySkeletonTest, SessionRecordMethodsEmitExpectedEventsWithTimestamps) {
    TelemetrySession session;
    TelemetrySessionTestAccess::collector(session).stop_consuming();

    session.record_task_started();
    session.record_download_delta(128U);
    session.record_pause(TelemetryPauseReason::gap, false);
    session.record_task_completed();

    auto& sink = TelemetrySessionTestAccess::sink(session);
    TelemetryEvent first;
    TelemetryEvent second;
    TelemetryEvent third;
    TelemetryEvent fourth;

    ASSERT_TRUE(sink.wait_dequeue_timed(first, 50ms));
    ASSERT_TRUE(sink.wait_dequeue_timed(second, 50ms));
    ASSERT_TRUE(sink.wait_dequeue_timed(third, 50ms));
    ASSERT_TRUE(sink.wait_dequeue_timed(fourth, 50ms));

    EXPECT_EQ(first.type, TelemetryEventType::task_started);
    EXPECT_EQ(second.type, TelemetryEventType::download_delta);
    EXPECT_EQ(second.payload.as_download_delta().bytes_count, 128U);
    EXPECT_EQ(third.type, TelemetryEventType::queue_paused);
    EXPECT_EQ(third.payload.as_queue_paused().reason, TelemetryPauseReason::gap);
    EXPECT_FALSE(third.payload.as_queue_paused().is_queue_full);
    EXPECT_EQ(fourth.type, TelemetryEventType::task_completed);
    EXPECT_EQ(fourth.payload.as_task_completed().status, TelemetryCompletionStatus::success);
    EXPECT_GT(first.timestamp_ns, 0U);
    EXPECT_GE(second.timestamp_ns, first.timestamp_ns);
    EXPECT_GE(third.timestamp_ns, second.timestamp_ns);
    EXPECT_GE(fourth.timestamp_ns, third.timestamp_ns);
}

} // namespace
