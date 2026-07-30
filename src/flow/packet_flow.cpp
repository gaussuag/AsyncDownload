#include "packet_flow.hpp"

#include "asyncdownload/error.hpp"
#include "asyncdownload/telemetry/telemetry_session.hpp"
#include "core/constants.hpp"
#include "flow/packet_queue_adapter.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <utility>

namespace asyncdownload::flow {
namespace {

constexpr std::size_t AGGREGATED_PACKET_BYTES = 64 * 1024;
constexpr std::uint8_t QUEUE_PAUSE_MASK =
    static_cast<std::uint8_t>(PacketPauseReason::queue);
constexpr std::uint8_t MEMORY_PAUSE_MASK =
    static_cast<std::uint8_t>(PacketPauseReason::memory);

struct LaneState {
    PacketLaneId id = 0;
    bool active = false;
    range::LeaseId lease{};
    range::ByteSpan lease_span{};
    range::ByteOffset offset = 0;
    std::vector<std::uint8_t> payload;
    std::size_t accounted_bytes = 0;
    std::uint8_t pause_mask = 0;
};

[[nodiscard]] bool checked_add(const std::size_t lhs,
                               const std::size_t rhs,
                               std::size_t& result) noexcept {
    if (rhs > std::numeric_limits<std::size_t>::max() - lhs) {
        return false;
    }
    result = lhs + rhs;
    return true;
}

[[nodiscard]] bool checked_add(const range::ByteOffset lhs,
                               const std::size_t rhs,
                               range::ByteOffset& result) noexcept {
    if (lhs < 0 ||
        rhs > static_cast<std::size_t>(
            std::numeric_limits<range::ByteOffset>::max() - lhs)) {
        return false;
    }
    result = lhs + static_cast<range::ByteOffset>(rhs);
    return true;
}

}

class PacketFlow::Implementation {
public:
    Implementation(const download::FlowControlPolicy& flow_policy,
                   telemetry::TelemetrySession& telemetry_session)
        : policy(flow_policy),
          telemetry(telemetry_session),
          queue(flow_policy.packet_budget),
          producer_thread(std::this_thread::get_id()) {}

    [[nodiscard]] LaneState* find_lane(const ProducerLane& lane) noexcept {
        if (lane.owner_ != owner ||
            lane.id_ == 0 ||
            lane.id_ > lanes.size()) {
            return nullptr;
        }
        auto* lane_state = lanes[lane.id_ - 1].get();
        return lane_state != nullptr && lane_state->active ? lane_state : nullptr;
    }

    [[nodiscard]] const LaneState* find_lane(
        const ProducerLane& lane) const noexcept {
        if (lane.owner_ != owner ||
            lane.id_ == 0 ||
            lane.id_ > lanes.size()) {
            return nullptr;
        }
        const auto* lane_state = lanes[lane.id_ - 1].get();
        return lane_state != nullptr && lane_state->active ? lane_state : nullptr;
    }

    [[nodiscard]] bool producer_thread_matches() const noexcept {
        return producer_thread == std::this_thread::get_id();
    }

    [[nodiscard]] bool consumer_thread_matches_or_bind() noexcept {
        const auto current = std::this_thread::get_id();
        std::scoped_lock lock(consumer_thread_mutex);
        if (consumer_thread == std::thread::id{}) {
            consumer_thread = current;
        }
        return consumer_thread == current;
    }

    [[nodiscard]] std::error_code first_error() const noexcept {
        std::scoped_lock lock(error_mutex);
        return error;
    }

    void fail(const std::error_code value) noexcept {
        {
            std::scoped_lock lock(error_mutex);
            if (!error) {
                error = value ? value :
                    make_error_code(DownloadErrc::internal_error);
            }
        }
        state.store(PacketFlowState::failed, std::memory_order_release);
    }

    [[nodiscard]] bool add_accounting(const std::size_t bytes) noexcept {
        auto current = accounted_bytes.load(std::memory_order_acquire);
        while (true) {
            if (bytes > std::numeric_limits<std::size_t>::max() - current) {
                fail(make_error_code(DownloadErrc::internal_error));
                return false;
            }
            if (accounted_bytes.compare_exchange_weak(
                    current,
                    current + bytes,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                telemetry.record_memory_sample(current + bytes);
                return true;
            }
        }
    }

    [[nodiscard]] bool release_accounting(const std::size_t bytes) noexcept {
        auto current = accounted_bytes.load(std::memory_order_acquire);
        while (true) {
            if (bytes > current) {
                fail(make_error_code(DownloadErrc::internal_error));
                return false;
            }
            if (accounted_bytes.compare_exchange_weak(
                    current,
                    current - bytes,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return true;
            }
        }
    }

    void enter_pause(LaneState& lane, const std::uint8_t reason) noexcept {
        if ((lane.pause_mask & reason) != 0) {
            return;
        }
        lane.pause_mask |= reason;
        if (reason == QUEUE_PAUSE_MASK) {
            telemetry.record_pause(
                telemetry::TelemetryPauseReason::queue_full, true);
        } else {
            telemetry.record_pause(
                telemetry::TelemetryPauseReason::memory_pressure, false);
        }
    }

    [[nodiscard]] bool reserve_data_credit() noexcept {
        auto current = queued_packets.load(std::memory_order_acquire);
        while (true) {
            if (current >= policy.packet_budget) {
                return false;
            }
            if (queued_packets.compare_exchange_weak(
                    current,
                    current + 1,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return true;
            }
        }
    }

    [[nodiscard]] bool reserve_control_credit() noexcept {
        auto current = queued_packets.load(std::memory_order_acquire);
        while (true) {
            if (current == std::numeric_limits<std::size_t>::max()) {
                fail(make_error_code(DownloadErrc::internal_error));
                return false;
            }
            if (queued_packets.compare_exchange_weak(
                    current,
                    current + 1,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return true;
            }
        }
    }

    [[nodiscard]] bool release_credit() noexcept {
        auto current = queued_packets.load(std::memory_order_acquire);
        while (true) {
            if (current == 0) {
                fail(make_error_code(DownloadErrc::internal_error));
                return false;
            }
            if (queued_packets.compare_exchange_weak(
                    current,
                    current - 1,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return true;
            }
        }
    }

    [[nodiscard]] PacketAdmission publish_draft(LaneState& lane) noexcept {
        if (lane.payload.empty()) {
            return {
                PacketAdmissionCode::accepted,
                0,
                0,
                lane.pause_mask,
                {}
            };
        }

        if (state.load(std::memory_order_acquire) != PacketFlowState::open) {
            const auto current_state =
                state.load(std::memory_order_acquire);
            return {
                current_state == PacketFlowState::failed ?
                    PacketAdmissionCode::failed :
                    PacketAdmissionCode::closed,
                0,
                0,
                lane.pause_mask,
                current_state == PacketFlowState::failed ?
                    first_error() : std::error_code{}
            };
        }
        if (next_sequence ==
            std::numeric_limits<PacketSequence>::max()) {
            fail(make_error_code(DownloadErrc::internal_error));
            return {
                PacketAdmissionCode::failed,
                0,
                0,
                lane.pause_mask,
                first_error()
            };
        }
        const auto published = published_data_bytes.load(
            std::memory_order_acquire);
        if (lane.payload.size() >
            std::numeric_limits<std::uint64_t>::max() - published) {
            fail(make_error_code(DownloadErrc::internal_error));
            return {
                PacketAdmissionCode::failed,
                0,
                0,
                lane.pause_mask,
                first_error()
            };
        }
        if (!reserve_data_credit()) {
            enter_pause(lane, QUEUE_PAUSE_MASK);
            return {
                PacketAdmissionCode::packet_budget_exhausted,
                0,
                0,
                lane.pause_mask,
                {}
            };
        }

        detail::PacketEnvelope envelope{};
        envelope.kind = detail::PacketEnvelopeKind::data;
        envelope.sequence = next_sequence;
        envelope.data.lease = lane.lease;
        envelope.data.lease_span = lane.lease_span;
        envelope.data.offset = lane.offset;
        envelope.data.payload = std::move(lane.payload);
        envelope.accounted_bytes = lane.accounted_bytes;
        const auto published_bytes = envelope.data.payload.size();

        if (!queue.try_publish(envelope)) {
            static_cast<void>(release_credit());
            lane.payload = std::move(envelope.data.payload);
            enter_pause(lane, QUEUE_PAUSE_MASK);
            return {
                PacketAdmissionCode::backend_temporarily_unavailable,
                0,
                0,
                lane.pause_mask,
                {}
            };
        }

        ++next_sequence;
        published_data_bytes.store(
            published + static_cast<std::uint64_t>(published_bytes),
            std::memory_order_release);
        telemetry.record_download_delta(
            static_cast<std::uint64_t>(published_bytes));
        lane.lease = {};
        lane.lease_span = {};
        lane.offset = 0;
        lane.accounted_bytes = 0;
        lane.payload.clear();
        return {
            PacketAdmissionCode::accepted,
            0,
            published_bytes,
            lane.pause_mask,
            {}
        };
    }

    PacketFlow* owner = nullptr;
    download::FlowControlPolicy policy;
    telemetry::TelemetrySession& telemetry;
    detail::MoodycamelPacketQueueAdapter queue;
    std::vector<std::unique_ptr<LaneState>> lanes;
    std::atomic<PacketFlowState> state{PacketFlowState::open};
    std::atomic<std::size_t> queued_packets{0};
    std::atomic<std::size_t> accounted_bytes{0};
    std::atomic<std::uint64_t> published_data_bytes{0};
    PacketSequence next_sequence = 1;
    std::thread::id producer_thread;
    std::mutex consumer_thread_mutex;
    std::thread::id consumer_thread{};
    mutable std::mutex error_mutex;
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
        if (owner_ != nullptr) {
            static_cast<void>(owner_->producer().discard(*this));
        }
        owner_ = std::exchange(other.owner_, nullptr);
        id_ = std::exchange(other.id_, 0);
    }
    return *this;
}

ProducerLane::~ProducerLane() {
    if (owner_ != nullptr) {
        static_cast<void>(owner_->producer().discard(*this));
    }
}

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
    if (!has_value_ ||
        kind_ != PacketKind::data ||
        owner_ == nullptr ||
        reorder_node_accounted_) {
        const auto error =
            make_error_code(DownloadErrc::internal_error);
        if (owner_ != nullptr) {
            owner_->implementation_->fail(error);
        }
        return error;
    }

    if (!owner_->implementation_->add_accounting(
            core::kMapNodeOverheadBytes)) {
        return owner_->implementation_->first_error();
    }
    accounted_bytes_ += core::kMapNodeOverheadBytes;
    reorder_node_accounted_ = true;
    return {};
}

void PacketLease::complete() noexcept {
    if (has_value_ && owner_ != nullptr && accounted_bytes_ != 0) {
        static_cast<void>(
            owner_->implementation_->release_accounting(accounted_bytes_));
    }
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

std::error_code PacketProducer::open_lane(ProducerLane& lane) noexcept {
    auto& implementation = *owner_.implementation_;
    if (!implementation.producer_thread_matches() ||
        lane.owner_ != nullptr ||
        implementation.state.load(std::memory_order_acquire) !=
            PacketFlowState::open) {
        return make_error_code(DownloadErrc::internal_error);
    }

    try {
        auto state = std::make_unique<LaneState>();
        state->id = static_cast<PacketLaneId>(
            implementation.lanes.size() + 1);
        state->active = true;
        state->payload.reserve(AGGREGATED_PACKET_BYTES);
        lane.owner_ = &owner_;
        lane.id_ = state->id;
        implementation.lanes.push_back(std::move(state));
        return {};
    } catch (const std::bad_alloc&) {
        implementation.fail(
            std::make_error_code(std::errc::not_enough_memory));
        return implementation.first_error();
    } catch (...) {
        implementation.fail(make_error_code(DownloadErrc::internal_error));
        return implementation.first_error();
    }
}

PacketAdmission PacketProducer::accept(
    ProducerLane& lane,
    const DataChunk chunk) noexcept {
    auto& implementation = *owner_.implementation_;
    auto* lane_state = implementation.find_lane(lane);
    if (lane_state == nullptr) {
        return {
            PacketAdmissionCode::failed,
            0,
            0,
            0,
            make_error_code(DownloadErrc::internal_error)
        };
    }

    const auto flow_state =
        implementation.state.load(std::memory_order_acquire);
    if (flow_state != PacketFlowState::open) {
        return {
            flow_state == PacketFlowState::failed ?
                PacketAdmissionCode::failed :
                PacketAdmissionCode::closed,
            0,
            0,
            lane_state->pause_mask,
            flow_state == PacketFlowState::failed ?
                implementation.first_error() : std::error_code{}
        };
    }

    range::ByteOffset chunk_end = 0;
    if (chunk.bytes.empty() ||
        chunk.bytes.size() > AGGREGATED_PACKET_BYTES ||
        chunk.lease_span.begin < 0 ||
        chunk.lease_span.begin >= chunk.lease_span.end ||
        chunk.offset < chunk.lease_span.begin ||
        !checked_add(chunk.offset, chunk.bytes.size(), chunk_end) ||
        chunk_end > chunk.lease_span.end) {
        const auto error =
            std::make_error_code(std::errc::invalid_argument);
        implementation.fail(error);
        return {
            PacketAdmissionCode::failed,
            0,
            0,
            lane_state->pause_mask,
            error
        };
    }

    if (!lane_state->payload.empty()) {
        range::ByteOffset expected_offset = 0;
        if (lane_state->lease != chunk.lease ||
            lane_state->lease_span != chunk.lease_span ||
            !checked_add(
                lane_state->offset,
                lane_state->payload.size(),
                expected_offset) ||
            expected_offset != chunk.offset) {
            const auto error =
                std::make_error_code(std::errc::invalid_argument);
            implementation.fail(error);
            return {
                PacketAdmissionCode::failed,
                0,
                0,
                lane_state->pause_mask,
                error
            };
        }
    }

    std::size_t projected_payload = 0;
    if (!checked_add(
            lane_state->payload.size(),
            chunk.bytes.size(),
            projected_payload)) {
        const auto error =
            std::make_error_code(std::errc::invalid_argument);
        implementation.fail(error);
        return {
            PacketAdmissionCode::failed,
            0,
            0,
            lane_state->pause_mask,
            error
        };
    }

    std::size_t published_bytes = 0;
    if (projected_payload > AGGREGATED_PACKET_BYTES) {
        auto publication = implementation.publish_draft(*lane_state);
        if (!publication.accepted()) {
            return publication;
        }
        published_bytes = publication.published_bytes;
        projected_payload = chunk.bytes.size();
    }

    const auto projected_accounted =
        sizeof(DataPacket) + projected_payload;
    const auto delta = projected_accounted - lane_state->accounted_bytes;
    const auto current = implementation.accounted_bytes.load(
        std::memory_order_acquire);
    if (delta != 0 &&
        current != 0 &&
        (delta > implementation.policy.memory_high_bytes ||
         current > implementation.policy.memory_high_bytes - delta)) {
        implementation.enter_pause(*lane_state, MEMORY_PAUSE_MASK);
        return {
            PacketAdmissionCode::memory_budget_exhausted,
            0,
            published_bytes,
            lane_state->pause_mask,
            {}
        };
    }

    if (!implementation.add_accounting(delta)) {
        return {
            PacketAdmissionCode::failed,
            0,
            published_bytes,
            lane_state->pause_mask,
            implementation.first_error()
        };
    }
    try {
        lane_state->payload.insert(
            lane_state->payload.end(),
            chunk.bytes.begin(),
            chunk.bytes.end());
    } catch (const std::bad_alloc&) {
        static_cast<void>(implementation.release_accounting(delta));
        implementation.fail(
            std::make_error_code(std::errc::not_enough_memory));
        return {
            PacketAdmissionCode::failed,
            0,
            published_bytes,
            lane_state->pause_mask,
            implementation.first_error()
        };
    } catch (...) {
        static_cast<void>(implementation.release_accounting(delta));
        implementation.fail(make_error_code(DownloadErrc::internal_error));
        return {
            PacketAdmissionCode::failed,
            0,
            published_bytes,
            lane_state->pause_mask,
            implementation.first_error()
        };
    }
    if (lane_state->payload.size() == chunk.bytes.size()) {
        lane_state->lease = chunk.lease;
        lane_state->lease_span = chunk.lease_span;
        lane_state->offset = chunk.offset;
    }
    lane_state->accounted_bytes = projected_accounted;

    if (lane_state->payload.size() == AGGREGATED_PACKET_BYTES) {
        const auto publication =
            implementation.publish_draft(*lane_state);
        published_bytes += publication.published_bytes;
    }

    return {
        PacketAdmissionCode::accepted,
        chunk.bytes.size(),
        published_bytes,
        lane_state->pause_mask,
        {}
    };
}

PacketAdmission PacketProducer::flush(ProducerLane& lane) noexcept {
    auto& implementation = *owner_.implementation_;
    auto* lane_state = implementation.find_lane(lane);
    if (lane_state == nullptr) {
        return {
            PacketAdmissionCode::failed,
            0,
            0,
            0,
            make_error_code(DownloadErrc::internal_error)
        };
    }
    return implementation.publish_draft(*lane_state);
}

std::error_code PacketProducer::discard(ProducerLane& lane) noexcept {
    auto& implementation = *owner_.implementation_;
    if (!implementation.producer_thread_matches()) {
        return make_error_code(DownloadErrc::internal_error);
    }
    auto* lane_state = implementation.find_lane(lane);
    if (lane_state == nullptr) {
        return make_error_code(DownloadErrc::internal_error);
    }

    if (lane_state->accounted_bytes != 0) {
        static_cast<void>(
            implementation.release_accounting(
                lane_state->accounted_bytes));
    }
    lane_state->lease = {};
    lane_state->lease_span = {};
    lane_state->offset = 0;
    lane_state->payload.clear();
    lane_state->accounted_bytes = 0;
    lane_state->pause_mask = 0;
    lane_state->active = false;
    lane.owner_ = nullptr;
    lane.id_ = 0;
    return {};
}

PacketPublishResult PacketProducer::publish(
    ControlPacket packet) noexcept {
    auto& implementation = *owner_.implementation_;
    if (!implementation.producer_thread_matches()) {
        return {
            PacketPublishCode::failed,
            make_error_code(DownloadErrc::internal_error)
        };
    }
    const auto flow_state =
        implementation.state.load(std::memory_order_acquire);
    if (flow_state != PacketFlowState::open) {
        return {
            flow_state == PacketFlowState::failed ?
                PacketPublishCode::failed :
                PacketPublishCode::closed,
            flow_state == PacketFlowState::failed ?
                implementation.first_error() : std::error_code{}
        };
    }
    if (packet.expected_end <= 0) {
        const auto error =
            std::make_error_code(std::errc::invalid_argument);
        implementation.fail(error);
        return {
            PacketPublishCode::failed,
            error
        };
    }
    for (const auto& lane : implementation.lanes) {
        if (lane != nullptr &&
            lane->active &&
            !lane->payload.empty() &&
            lane->lease.range == packet.completion.range) {
            const auto error =
                make_error_code(DownloadErrc::internal_error);
            implementation.fail(error);
            return {
                PacketPublishCode::failed,
                error
            };
        }
    }

    detail::PacketEnvelope envelope{};
    if (implementation.next_sequence ==
        std::numeric_limits<PacketSequence>::max()) {
        implementation.fail(
            make_error_code(DownloadErrc::internal_error));
        return {
            PacketPublishCode::failed,
            implementation.first_error()
        };
    }
    envelope.kind = detail::PacketEnvelopeKind::control;
    envelope.sequence = implementation.next_sequence;
    envelope.control = packet;
    if (!implementation.reserve_control_credit()) {
        return {
            PacketPublishCode::failed,
            implementation.first_error()
        };
    }
    if (!implementation.queue.publish(envelope)) {
        static_cast<void>(implementation.release_credit());
        implementation.fail(make_error_code(DownloadErrc::internal_error));
        return {
            PacketPublishCode::failed,
            implementation.first_error()
        };
    }
    ++implementation.next_sequence;
    return {PacketPublishCode::published, {}};
}

PacketReconcileResult PacketProducer::reconcile(
    const std::span<const PacketLaneObservation> observations,
    const std::span<PacketPauseAction> actions) noexcept {
    auto& implementation = *owner_.implementation_;
    if (!implementation.producer_thread_matches() ||
        actions.size() < observations.size()) {
        return {
            0,
            make_error_code(DownloadErrc::internal_error)
        };
    }

    std::vector<const PacketLaneObservation*> eligible;
    try {
        eligible.reserve(observations.size());
    } catch (...) {
        return {
            0,
            std::make_error_code(std::errc::not_enough_memory)
        };
    }

    std::vector<bool> seen(implementation.lanes.size() + 1, false);
    for (const auto& observation : observations) {
        if (observation.lane_id == 0 ||
            observation.lane_id > implementation.lanes.size() ||
            !std::isfinite(observation.bytes_per_second) ||
            observation.bytes_per_second < 0.0 ||
            seen[observation.lane_id]) {
            return {
                0,
                std::make_error_code(std::errc::invalid_argument)
            };
        }
        seen[observation.lane_id] = true;
        auto* lane = implementation.lanes[
            observation.lane_id - 1].get();
        if (lane == nullptr || !lane->active) {
            return {
                0,
                std::make_error_code(std::errc::invalid_argument)
            };
        }
        if (observation.eligible_for_memory_pause &&
            (lane->pause_mask & MEMORY_PAUSE_MASK) == 0) {
            eligible.push_back(&observation);
        }
    }

    std::sort(
        eligible.begin(),
        eligible.end(),
        [](const auto* lhs, const auto* rhs) {
            if (lhs->bytes_per_second != rhs->bytes_per_second) {
                return lhs->bytes_per_second > rhs->bytes_per_second;
            }
            return lhs->lane_id < rhs->lane_id;
        });

    std::size_t action_count = 0;
    const auto current = implementation.accounted_bytes.load(
        std::memory_order_acquire);
    if (current > implementation.policy.memory_high_bytes &&
        !eligible.empty()) {
        const auto pause_count =
            std::max<std::size_t>(1, (eligible.size() + 4) / 5);
        for (std::size_t index = 0;
             index < pause_count && index < eligible.size();
             ++index) {
            auto& lane = *implementation.lanes[
                eligible[index]->lane_id - 1];
            implementation.enter_pause(lane, MEMORY_PAUSE_MASK);
            actions[action_count++] = {
                lane.id,
                PacketPauseActionKind::pause_receive,
                lane.pause_mask
            };
        }
    }

    if (current <= implementation.policy.memory_low_bytes) {
        for (const auto& observation : observations) {
            auto& lane =
                *implementation.lanes[observation.lane_id - 1];
            const auto previous = lane.pause_mask;
            lane.pause_mask &= static_cast<std::uint8_t>(
                ~MEMORY_PAUSE_MASK);
            if (implementation.queued_packets.load(
                    std::memory_order_acquire) <
                implementation.policy.packet_budget) {
                lane.pause_mask &= static_cast<std::uint8_t>(
                    ~QUEUE_PAUSE_MASK);
            }
            if (previous != 0 && lane.pause_mask == 0) {
                actions[action_count++] = {
                    lane.id,
                    PacketPauseActionKind::resume_candidate,
                    0
                };
            }
        }
    }
    return {action_count, {}};
}

bool PacketProducer::paused(const ProducerLane& lane) const noexcept {
    return pause_mask(lane) != 0;
}

std::uint8_t PacketProducer::pause_mask(
    const ProducerLane& lane) const noexcept {
    const auto* state = owner_.implementation_->find_lane(lane);
    return state == nullptr ? 0 : state->pause_mask;
}

PacketFlowSnapshot PacketProducer::snapshot() const noexcept {
    auto& implementation = *owner_.implementation_;
    return {
        implementation.state.load(std::memory_order_acquire),
        implementation.queued_packets.load(std::memory_order_acquire),
        implementation.accounted_bytes.load(std::memory_order_acquire),
        implementation.published_data_bytes.load(std::memory_order_acquire),
        implementation.first_error()
    };
}

std::error_code PacketProducer::close() noexcept {
    auto& implementation = *owner_.implementation_;
    if (!implementation.producer_thread_matches()) {
        return make_error_code(DownloadErrc::internal_error);
    }
    for (const auto& lane : implementation.lanes) {
        if (lane != nullptr &&
            lane->active &&
            !lane->payload.empty()) {
            return make_error_code(DownloadErrc::internal_error);
        }
    }

    auto expected = PacketFlowState::open;
    if (!implementation.state.compare_exchange_strong(
            expected,
            PacketFlowState::closing,
            std::memory_order_acq_rel)) {
        return make_error_code(DownloadErrc::internal_error);
    }

    detail::PacketEnvelope envelope{};
    if (implementation.next_sequence ==
        std::numeric_limits<PacketSequence>::max()) {
        implementation.fail(
            make_error_code(DownloadErrc::internal_error));
        return implementation.first_error();
    }
    envelope.kind = detail::PacketEnvelopeKind::close;
    envelope.sequence = implementation.next_sequence;
    if (!implementation.queue.publish(envelope)) {
        implementation.fail(make_error_code(DownloadErrc::internal_error));
        return implementation.first_error();
    }
    ++implementation.next_sequence;
    return {};
}

PacketConsumer::PacketConsumer(PacketFlow& owner) noexcept
    : owner_(owner) {}

PacketReceiveResult PacketConsumer::receive(
    PacketLease& lease,
    const std::chrono::microseconds timeout) noexcept {
    auto& implementation = *owner_.implementation_;
    if (lease.has_value() ||
        !implementation.consumer_thread_matches_or_bind()) {
        return {
            PacketReceiveCode::failed,
            make_error_code(DownloadErrc::internal_error)
        };
    }

    detail::PacketEnvelope envelope{};
    if (implementation.queue.receive(envelope, timeout)) {
        if (envelope.kind == detail::PacketEnvelopeKind::close) {
            implementation.state.store(
                PacketFlowState::closed,
                std::memory_order_release);
            return {PacketReceiveCode::closed, {}};
        }

        if (!implementation.release_credit()) {
            if (envelope.accounted_bytes != 0) {
                static_cast<void>(
                    implementation.release_accounting(
                        envelope.accounted_bytes));
            }
            return {
                PacketReceiveCode::failed,
                implementation.first_error()
            };
        }
        lease.owner_ = &owner_;
        lease.sequence_ = envelope.sequence;
        lease.accounted_bytes_ = envelope.accounted_bytes;
        lease.has_value_ = true;
        if (envelope.kind == detail::PacketEnvelopeKind::data) {
            lease.kind_ = PacketKind::data;
            lease.data_ = std::move(envelope.data);
        } else {
            lease.kind_ = PacketKind::control;
            lease.control_ = std::move(envelope.control);
        }
        return {PacketReceiveCode::packet, {}};
    }

    if (implementation.state.load(std::memory_order_acquire) ==
        PacketFlowState::failed) {
        return {
            PacketReceiveCode::failed,
            implementation.first_error()
        };
    }
    return {PacketReceiveCode::timeout, {}};
}

std::error_code PacketConsumer::fail(
    const std::error_code error) noexcept {
    auto& implementation = *owner_.implementation_;
    if (!implementation.consumer_thread_matches_or_bind()) {
        return make_error_code(DownloadErrc::internal_error);
    }
    implementation.fail(error);

    detail::PacketEnvelope envelope{};
    while (implementation.queue.receive(
        envelope, std::chrono::microseconds(0))) {
        if (envelope.kind == detail::PacketEnvelopeKind::close) {
            continue;
        }
        static_cast<void>(implementation.release_credit());
        if (envelope.accounted_bytes != 0) {
            static_cast<void>(
                implementation.release_accounting(
                    envelope.accounted_bytes));
        }
    }
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
        result->implementation_->owner = result.get();
        return {};
    } catch (const std::bad_alloc&) {
        return std::make_error_code(std::errc::not_enough_memory);
    } catch (...) {
        return make_error_code(DownloadErrc::internal_error);
    }
}

PacketFlow::PacketFlow(
    std::unique_ptr<Implementation> implementation) noexcept
    : implementation_(std::move(implementation)),
      producer_(*this),
      consumer_(*this) {}

PacketFlow::~PacketFlow() {
    if (!implementation_) {
        return;
    }
    for (auto& lane : implementation_->lanes) {
        if (lane != nullptr && lane->accounted_bytes != 0) {
            static_cast<void>(
                implementation_->release_accounting(
                    lane->accounted_bytes));
            lane->accounted_bytes = 0;
        }
        if (lane != nullptr) {
            lane->active = false;
        }
    }
    detail::PacketEnvelope envelope{};
    while (implementation_->queue.receive(
        envelope, std::chrono::microseconds(0))) {
        if (envelope.kind != detail::PacketEnvelopeKind::close) {
            static_cast<void>(
                implementation_->release_credit());
            if (envelope.accounted_bytes != 0) {
                static_cast<void>(
                    implementation_->release_accounting(
                        envelope.accounted_bytes));
            }
        }
    }
}

PacketProducer& PacketFlow::producer() noexcept {
    return producer_;
}

PacketConsumer& PacketFlow::consumer() noexcept {
    return consumer_;
}

}
