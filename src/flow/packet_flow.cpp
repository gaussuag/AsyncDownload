#include "packet_flow.hpp"

#include "asyncdownload/error.hpp"
#include "asyncdownload/telemetry/telemetry_session.hpp"
#include "flow/packet_queue_adapter.hpp"

#include <atomic>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

namespace asyncdownload::flow {

class PacketFlow::Implementation {
public:
    Implementation(const download::FlowControlPolicy& policy,
                   telemetry::TelemetrySession& telemetry)
        : policy(policy),
          telemetry(telemetry),
          queue(policy.packet_budget) {}

    download::FlowControlPolicy policy;
    telemetry::TelemetrySession& telemetry;
    detail::MoodycamelPacketQueueAdapter queue;
    std::atomic<PacketFlowState> state{PacketFlowState::open};
    std::atomic<std::size_t> queued_packets{0};
    std::atomic<std::size_t> accounted_bytes{0};
    std::atomic<std::uint64_t> published_data_bytes{0};
    PacketSequence next_sequence = 1;
    std::mutex error_mutex;
    std::error_code error;
};

bool PacketAdmission::accepted() const noexcept {
    return code == PacketAdmissionCode::accepted;
}

bool PacketAdmission::must_pause() const noexcept {
    return active_pause_mask != 0;
}

ProducerLane::ProducerLane(ProducerLane&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      id_(std::exchange(other.id_, 0)) {}

ProducerLane& ProducerLane::operator=(ProducerLane&& other) noexcept {
    if (this != &other) {
        owner_ = std::exchange(other.owner_, nullptr);
        id_ = std::exchange(other.id_, 0);
    }
    return *this;
}

ProducerLane::~ProducerLane() = default;

PacketLaneId ProducerLane::id() const noexcept {
    return id_;
}

PacketLease::PacketLease(PacketLease&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      data_(std::move(other.data_)),
      control_(std::move(other.control_)),
      sequence_(std::exchange(other.sequence_, 0)),
      accounted_bytes_(std::exchange(other.accounted_bytes_, 0)),
      kind_(other.kind_),
      has_value_(std::exchange(other.has_value_, false)),
      reorder_node_accounted_(std::exchange(other.reorder_node_accounted_, false)) {}

PacketLease& PacketLease::operator=(PacketLease&& other) noexcept {
    if (this != &other) {
        complete();
        owner_ = std::exchange(other.owner_, nullptr);
        data_ = std::move(other.data_);
        control_ = std::move(other.control_);
        sequence_ = std::exchange(other.sequence_, 0);
        accounted_bytes_ = std::exchange(other.accounted_bytes_, 0);
        kind_ = other.kind_;
        has_value_ = std::exchange(other.has_value_, false);
        reorder_node_accounted_ = std::exchange(
            other.reorder_node_accounted_, false);
    }
    return *this;
}

PacketLease::~PacketLease() {
    complete();
}

bool PacketLease::has_value() const noexcept {
    return has_value_;
}

PacketKind PacketLease::kind() const noexcept {
    return kind_;
}

PacketSequence PacketLease::sequence() const noexcept {
    return sequence_;
}

const DataPacket* PacketLease::data() const noexcept {
    return has_value_ && kind_ == PacketKind::data ? &data_ : nullptr;
}

const ControlPacket* PacketLease::control() const noexcept {
    return has_value_ && kind_ == PacketKind::control ? &control_ : nullptr;
}

std::error_code PacketLease::account_reorder_node() noexcept {
    return make_error_code(DownloadErrc::internal_error);
}

void PacketLease::complete() noexcept {
    owner_ = nullptr;
    data_ = {};
    control_ = {};
    sequence_ = 0;
    accounted_bytes_ = 0;
    kind_ = PacketKind::data;
    has_value_ = false;
    reorder_node_accounted_ = false;
}

PacketProducer::PacketProducer(PacketFlow& owner) noexcept
    : owner_(owner) {}

std::error_code PacketProducer::open_lane(ProducerLane&) noexcept {
    return make_error_code(DownloadErrc::internal_error);
}

PacketAdmission PacketProducer::accept(ProducerLane&, DataChunk) noexcept {
    return {};
}

PacketAdmission PacketProducer::flush(ProducerLane&) noexcept {
    return {};
}

std::error_code PacketProducer::discard(ProducerLane&) noexcept {
    return make_error_code(DownloadErrc::internal_error);
}

PacketPublishResult PacketProducer::publish(ControlPacket) noexcept {
    return {};
}

PacketReconcileResult PacketProducer::reconcile(
    std::span<const PacketLaneObservation>,
    std::span<PacketPauseAction>) noexcept {
    return {.error = make_error_code(DownloadErrc::internal_error)};
}

bool PacketProducer::paused(const ProducerLane&) const noexcept {
    return false;
}

std::uint8_t PacketProducer::pause_mask(const ProducerLane&) const noexcept {
    return 0;
}

PacketFlowSnapshot PacketProducer::snapshot() const noexcept {
    auto& implementation = *owner_.implementation_;
    std::scoped_lock lock(implementation.error_mutex);
    return {
        implementation.state.load(std::memory_order_acquire),
        implementation.queued_packets.load(std::memory_order_acquire),
        implementation.accounted_bytes.load(std::memory_order_acquire),
        implementation.published_data_bytes.load(std::memory_order_acquire),
        implementation.error
    };
}

std::error_code PacketProducer::close() noexcept {
    auto& implementation = *owner_.implementation_;
    auto expected = PacketFlowState::open;
    if (!implementation.state.compare_exchange_strong(
            expected,
            PacketFlowState::closing,
            std::memory_order_acq_rel)) {
        return make_error_code(DownloadErrc::internal_error);
    }

    detail::PacketEnvelope envelope{};
    envelope.kind = detail::PacketEnvelopeKind::close;
    envelope.sequence = implementation.next_sequence;
    if (!implementation.queue.publish(envelope)) {
        implementation.state.store(PacketFlowState::failed, std::memory_order_release);
        std::scoped_lock lock(implementation.error_mutex);
        implementation.error = make_error_code(DownloadErrc::internal_error);
        return implementation.error;
    }
    ++implementation.next_sequence;
    return {};
}

PacketConsumer::PacketConsumer(PacketFlow& owner) noexcept
    : owner_(owner) {}

PacketReceiveResult PacketConsumer::receive(
    PacketLease& lease,
    const std::chrono::microseconds timeout) noexcept {
    if (lease.has_value()) {
        return {
            PacketReceiveCode::failed,
            make_error_code(DownloadErrc::internal_error)
        };
    }

    auto& implementation = *owner_.implementation_;
    detail::PacketEnvelope envelope{};
    if (implementation.queue.receive(envelope, timeout)) {
        if (envelope.kind == detail::PacketEnvelopeKind::close) {
            implementation.state.store(PacketFlowState::closed, std::memory_order_release);
            return {PacketReceiveCode::closed, {}};
        }
        return {
            PacketReceiveCode::failed,
            make_error_code(DownloadErrc::internal_error)
        };
    }

    if (implementation.state.load(std::memory_order_acquire) ==
        PacketFlowState::failed) {
        std::scoped_lock lock(implementation.error_mutex);
        return {PacketReceiveCode::failed, implementation.error};
    }
    return {PacketReceiveCode::timeout, {}};
}

std::error_code PacketConsumer::fail(const std::error_code error) noexcept {
    auto& implementation = *owner_.implementation_;
    {
        std::scoped_lock lock(implementation.error_mutex);
        if (!implementation.error) {
            implementation.error = error ?
                error : make_error_code(DownloadErrc::internal_error);
        }
    }
    implementation.state.store(PacketFlowState::failed, std::memory_order_release);
    return {};
}

std::error_code PacketFlow::create(
    const download::FlowControlPolicy& policy,
    telemetry::TelemetrySession& telemetry,
    std::unique_ptr<PacketFlow>& result) noexcept {
    result.reset();
    if (policy.packet_budget == 0 ||
        policy.packet_budget >
            std::numeric_limits<std::size_t>::max() - 31) {
        return std::make_error_code(std::errc::invalid_argument);
    }

    try {
        auto implementation =
            std::make_unique<Implementation>(policy, telemetry);
        result.reset(new PacketFlow(std::move(implementation)));
        return {};
    } catch (const std::bad_alloc&) {
        return std::make_error_code(std::errc::not_enough_memory);
    } catch (...) {
        return make_error_code(DownloadErrc::internal_error);
    }
}

PacketFlow::PacketFlow(std::unique_ptr<Implementation> implementation) noexcept
    : implementation_(std::move(implementation)),
      producer_(*this),
      consumer_(*this) {}

PacketFlow::~PacketFlow() = default;

PacketProducer& PacketFlow::producer() noexcept {
    return producer_;
}

PacketConsumer& PacketFlow::consumer() noexcept {
    return consumer_;
}

}
