#pragma once

#include "download/download_policy.hpp"
#include "range/range_types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <system_error>
#include <vector>

namespace asyncdownload::telemetry {
class TelemetrySession;
}

namespace asyncdownload::flow {

using PacketLaneId = std::uint32_t;
using PacketSequence = std::uint64_t;

enum class PacketKind : std::uint8_t {
    data = 0,
    control = 1
};

enum class ControlPacketKind : std::uint8_t {
    range_complete = 0
};

enum class PacketAdmissionCode : std::uint8_t {
    accepted = 0,
    packet_budget_exhausted,
    memory_budget_exhausted,
    backend_temporarily_unavailable,
    closed,
    failed
};

enum class PacketFlowState : std::uint8_t {
    open = 0,
    closing,
    closed,
    failed
};

enum class PacketPauseReason : std::uint8_t {
    none = 0,
    queue = 1,
    memory = 2
};

enum class PacketPauseActionKind : std::uint8_t {
    pause_receive = 0,
    resume_candidate
};

enum class PacketReceiveCode : std::uint8_t {
    packet = 0,
    timeout,
    closed,
    failed
};

enum class PacketPublishCode : std::uint8_t {
    published = 0,
    closed,
    failed
};

struct DataChunk {
    range::LeaseId lease{};
    range::ByteSpan lease_span{};
    range::ByteOffset offset = 0;
    std::span<const std::uint8_t> bytes{};
};

struct DataPacket {
    range::LeaseId lease{};
    range::ByteSpan lease_span{};
    range::ByteOffset offset = 0;
    std::vector<std::uint8_t> payload;
};

struct ControlPacket {
    ControlPacketKind kind = ControlPacketKind::range_complete;
    range::CompletionId completion{};
    range::ByteOffset expected_end = 0;
};

struct PacketAdmission {
    PacketAdmissionCode code = PacketAdmissionCode::failed;
    std::size_t consumed_bytes = 0;
    std::size_t published_bytes = 0;
    std::uint8_t active_pause_mask = 0;
    std::error_code error{};

    [[nodiscard]] bool accepted() const noexcept;
    [[nodiscard]] bool must_pause() const noexcept;
};

struct PacketFlowSnapshot {
    PacketFlowState state = PacketFlowState::open;
    std::size_t queued_packets = 0;
    std::size_t accounted_bytes = 0;
    std::uint64_t published_data_bytes = 0;
    std::error_code error{};
};

struct PacketLaneObservation {
    PacketLaneId lane_id = 0;
    double bytes_per_second = 0.0;
    bool eligible_for_memory_pause = false;
};

struct PacketPauseAction {
    PacketLaneId lane_id = 0;
    PacketPauseActionKind kind = PacketPauseActionKind::pause_receive;
    std::uint8_t active_pause_mask = 0;
};

struct PacketReceiveResult {
    PacketReceiveCode code = PacketReceiveCode::failed;
    std::error_code error{};
};

struct PacketPublishResult {
    PacketPublishCode code = PacketPublishCode::failed;
    std::error_code error{};
};

struct PacketReconcileResult {
    std::size_t action_count = 0;
    std::error_code error{};
};

class PacketFlow;

class ProducerLane {
public:
    ProducerLane() noexcept = default;
    ProducerLane(ProducerLane&& other) noexcept;
    ProducerLane& operator=(ProducerLane&& other) noexcept;
    ~ProducerLane();

    ProducerLane(const ProducerLane&) = delete;
    ProducerLane& operator=(const ProducerLane&) = delete;

    [[nodiscard]] PacketLaneId id() const noexcept;

private:
    PacketFlow* owner_ = nullptr;
    PacketLaneId id_ = 0;

    friend class PacketProducer;
    friend class PacketFlow;
};

class PacketLease {
public:
    PacketLease() noexcept = default;
    PacketLease(PacketLease&& other) noexcept;
    PacketLease& operator=(PacketLease&& other) noexcept;
    ~PacketLease();

    PacketLease(const PacketLease&) = delete;
    PacketLease& operator=(const PacketLease&) = delete;

    [[nodiscard]] bool has_value() const noexcept;
    [[nodiscard]] PacketKind kind() const noexcept;
    [[nodiscard]] PacketSequence sequence() const noexcept;
    [[nodiscard]] const DataPacket* data() const noexcept;
    [[nodiscard]] const ControlPacket* control() const noexcept;
    [[nodiscard]] std::error_code account_reorder_node() noexcept;
    void complete() noexcept;

private:
    PacketFlow* owner_ = nullptr;
    DataPacket data_{};
    ControlPacket control_{};
    PacketSequence sequence_ = 0;
    std::size_t accounted_bytes_ = 0;
    PacketKind kind_ = PacketKind::data;
    bool has_value_ = false;
    bool reorder_node_accounted_ = false;

    friend class PacketConsumer;
    friend class PacketFlow;
};

class PacketProducer {
public:
    PacketProducer(const PacketProducer&) = delete;
    PacketProducer& operator=(const PacketProducer&) = delete;
    PacketProducer(PacketProducer&&) = delete;
    PacketProducer& operator=(PacketProducer&&) = delete;

    [[nodiscard]] std::error_code open_lane(ProducerLane& lane) noexcept;
    [[nodiscard]] PacketAdmission accept(ProducerLane& lane, DataChunk chunk) noexcept;
    [[nodiscard]] PacketAdmission flush(ProducerLane& lane) noexcept;
    [[nodiscard]] std::error_code discard(ProducerLane& lane) noexcept;
    [[nodiscard]] PacketPublishResult publish(ControlPacket packet) noexcept;
    [[nodiscard]] PacketReconcileResult reconcile(
        std::span<const PacketLaneObservation> observations,
        std::span<PacketPauseAction> actions) noexcept;
    [[nodiscard]] bool paused(const ProducerLane& lane) const noexcept;
    [[nodiscard]] std::uint8_t pause_mask(const ProducerLane& lane) const noexcept;
    [[nodiscard]] PacketFlowSnapshot snapshot() const noexcept;
    [[nodiscard]] std::error_code close() noexcept;

private:
    explicit PacketProducer(PacketFlow& owner) noexcept;

    PacketFlow& owner_;

    friend class PacketFlow;
};

class PacketConsumer {
public:
    PacketConsumer(const PacketConsumer&) = delete;
    PacketConsumer& operator=(const PacketConsumer&) = delete;
    PacketConsumer(PacketConsumer&&) = delete;
    PacketConsumer& operator=(PacketConsumer&&) = delete;

    [[nodiscard]] PacketReceiveResult receive(
        PacketLease& lease,
        std::chrono::microseconds timeout) noexcept;
    [[nodiscard]] std::error_code fail(std::error_code error) noexcept;

private:
    explicit PacketConsumer(PacketFlow& owner) noexcept;

    PacketFlow& owner_;

    friend class PacketFlow;
};

class PacketFlow {
public:
    [[nodiscard]] static std::error_code create(
        const download::FlowControlPolicy& policy,
        telemetry::TelemetrySession& telemetry,
        std::unique_ptr<PacketFlow>& result) noexcept;

    ~PacketFlow();

    PacketFlow(const PacketFlow&) = delete;
    PacketFlow& operator=(const PacketFlow&) = delete;

    [[nodiscard]] PacketProducer& producer() noexcept;
    [[nodiscard]] PacketConsumer& consumer() noexcept;

private:
    class Implementation;

    explicit PacketFlow(std::unique_ptr<Implementation> implementation) noexcept;

    std::unique_ptr<Implementation> implementation_;
    PacketProducer producer_;
    PacketConsumer consumer_;

    friend class PacketProducer;
    friend class PacketConsumer;
    friend class PacketLease;
    friend class ProducerLane;
};

}
