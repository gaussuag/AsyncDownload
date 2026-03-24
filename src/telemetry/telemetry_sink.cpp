#include "asyncdownload/telemetry/telemetry_sink.hpp"

namespace asyncdownload::telemetry {

TelemetrySink::ProducerToken TelemetrySink::make_producer_token() {
    return ProducerToken(queue_);
}

bool TelemetrySink::enqueue(const TelemetryEvent& event) noexcept {
    return queue_.enqueue(event);
}

bool TelemetrySink::enqueue(TelemetryEvent&& event) noexcept {
    return queue_.enqueue(std::move(event));
}

bool TelemetrySink::enqueue_from_producer(const ProducerToken& token,
                                          const TelemetryEvent& event) noexcept {
    return queue_.enqueue(token, event);
}

bool TelemetrySink::enqueue_from_producer(const ProducerToken& token,
                                          TelemetryEvent&& event) noexcept {
    return queue_.enqueue(token, std::move(event));
}

void TelemetrySink::wait_dequeue(TelemetryEvent& event) noexcept {
    queue_.wait_dequeue(event);
}

bool TelemetrySink::wait_dequeue_timed(TelemetryEvent& event,
                                       const std::chrono::milliseconds timeout) noexcept {
    return queue_.wait_dequeue_timed(event, timeout);
}

bool TelemetrySink::try_dequeue(TelemetryEvent& event) noexcept {
    return queue_.try_dequeue(event);
}

std::size_t TelemetrySink::size_approx() const noexcept {
    return queue_.size_approx();
}

} // namespace asyncdownload::telemetry
