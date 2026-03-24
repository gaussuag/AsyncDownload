#pragma once

#include "asyncdownload/telemetry/telemetry_event.hpp"

#include <concurrentqueue/blockingconcurrentqueue.h>

#include <chrono>
#include <cstddef>

namespace asyncdownload::telemetry {

class TelemetrySink {
public:
    using Queue = moodycamel::BlockingConcurrentQueue<TelemetryEvent>;
    using ProducerToken = Queue::producer_token_t;

    [[nodiscard]] ProducerToken make_producer_token();
    [[nodiscard]] bool enqueue(const TelemetryEvent& event) noexcept;
    [[nodiscard]] bool enqueue(TelemetryEvent&& event) noexcept;
    [[nodiscard]] bool enqueue_from_producer(const ProducerToken& token,
                                             const TelemetryEvent& event) noexcept;
    [[nodiscard]] bool enqueue_from_producer(const ProducerToken& token,
                                             TelemetryEvent&& event) noexcept;
    void wait_dequeue(TelemetryEvent& event) noexcept;
    [[nodiscard]] bool wait_dequeue_timed(TelemetryEvent& event,
                                          std::chrono::milliseconds timeout) noexcept;
    [[nodiscard]] bool try_dequeue(TelemetryEvent& event) noexcept;
    [[nodiscard]] std::size_t size_approx() const noexcept;

private:
    Queue queue_{};
};

} // namespace asyncdownload::telemetry
