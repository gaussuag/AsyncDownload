#pragma once

#include "range/range_lifecycle.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <variant>

namespace asyncdownload::flow {
class PacketProducer;
}

namespace asyncdownload::telemetry {
class TelemetrySession;
}

namespace asyncdownload::http {

using TransferSlotId = std::uint32_t;

enum class HttpFailureReason : std::uint8_t {
    none = 0,
    global_init_failed,
    easy_init_failed,
    multi_init_failed,
    easy_option_failed,
    multi_option_failed,
    easy_info_failed,
    add_handle_failed,
    remove_handle_failed,
    multi_perform_failed,
    multi_wait_failed,
    pause_failed,
    transport_failed,
    callback_sink_failed,
    response_status_invalid,
    content_range_missing,
    content_range_malformed,
    content_range_mismatch,
    content_length_malformed,
    content_length_mismatch,
    content_encoding_invalid,
    body_too_short,
    body_too_long,
    header_callback_failed,
    cancelled,
    upstream_failed,
    protocol_order_invalid,
    allocation_failed
};

struct HttpFailure {
    HttpFailureReason reason = HttpFailureReason::none;
    std::error_code error{};
};

struct HttpProbeRequest {
    std::string url;

    friend bool operator==(const HttpProbeRequest&, const HttpProbeRequest&) = default;
};

struct HttpObjectFacts {
    std::int64_t total_size = 0;
    bool accept_ranges = false;
    std::string etag;
    std::string last_modified;

    friend bool operator==(const HttpObjectFacts&, const HttpObjectFacts&) = default;
};

struct HttpProbeResult {
    std::optional<HttpObjectFacts> facts;
    HttpFailure failure{};
    long response_code = 0;

    [[nodiscard]] bool ok() const noexcept {
        return facts.has_value() && !failure.error;
    }
};

enum class HttpSessionState : std::uint8_t {
    open = 0,
    cancelling,
    failed,
    closed
};

struct HttpSessionConfig {
    std::string url;
    std::int64_t total_size = 0;
    std::size_t max_active_transfers = 0;
};

struct TransferToken {
    TransferSlotId slot = 0;
    std::uint64_t slot_generation = 0;
    range::LeaseId lease{};

    friend bool operator==(const TransferToken&, const TransferToken&) = default;
};

enum class HttpStartCode : std::uint8_t {
    started = 0,
    no_capacity,
    closed,
    failed
};

struct HttpStartResult {
    HttpStartCode code = HttpStartCode::failed;
    std::optional<TransferToken> token;
    HttpFailure failure{};
};

struct HttpLeaseSucceeded {
    TransferToken token{};
    range::LeaseId lease{};
    range::ByteOffset received_through = 0;
    long response_code = 0;
};

struct HttpLeaseFailed {
    TransferToken token{};
    range::LeaseId lease{};
    range::ByteOffset accepted_through = 0;
    HttpFailure failure{};
};

using HttpTransferEvent = std::variant<
    HttpLeaseSucceeded,
    HttpLeaseFailed>;

enum class HttpPollCode : std::uint8_t {
    event = 0,
    idle,
    timed_out,
    failed,
    closed
};

struct HttpPollResult {
    HttpPollCode code = HttpPollCode::failed;
    std::optional<HttpTransferEvent> event;
    std::error_code error{};
};

enum class HttpCancelKind : std::uint8_t {
    task_cancelled = 0,
    upstream_failed
};

struct HttpCancelRequest {
    HttpCancelKind kind = HttpCancelKind::task_cancelled;
    std::error_code cause{};
};

struct HttpSessionSnapshot {
    HttpSessionState state = HttpSessionState::open;
    std::size_t active_transfers = 0;
    std::size_t available_slots = 0;
    std::size_t pending_events = 0;
    std::size_t paused_transfers = 0;
    std::error_code error{};
};

class HttpTransferSession {
public:
    virtual ~HttpTransferSession() = default;

    [[nodiscard]] virtual HttpStartResult start(
        const range::RangeLease& lease) noexcept = 0;

    [[nodiscard]] virtual std::error_code set_gap_paused(
        const TransferToken& token,
        bool active) noexcept = 0;

    [[nodiscard]] virtual HttpPollResult poll(
        std::chrono::milliseconds timeout) noexcept = 0;

    [[nodiscard]] virtual std::error_code cancel(
        const HttpCancelRequest& request) noexcept = 0;

    [[nodiscard]] virtual HttpSessionSnapshot snapshot() const noexcept = 0;

    [[nodiscard]] virtual std::error_code close() noexcept = 0;
};

struct HttpSessionOpenResult {
    std::unique_ptr<HttpTransferSession> session;
    HttpFailure failure{};
};

class HttpTransferPort {
public:
    virtual ~HttpTransferPort() = default;

    [[nodiscard]] virtual HttpProbeResult probe(
        const HttpProbeRequest& request) noexcept = 0;

    [[nodiscard]] virtual HttpSessionOpenResult open_session(
        const HttpSessionConfig& config,
        flow::PacketProducer& packet_producer,
        telemetry::TelemetrySession& telemetry) noexcept = 0;
};

[[nodiscard]] std::error_code create_curl_http_transfer_port(
    std::unique_ptr<HttpTransferPort>& result) noexcept;

}
