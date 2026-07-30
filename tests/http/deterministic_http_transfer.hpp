#pragma once

#include "http/http_transfer.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace asyncdownload::http::testing {

struct FakeProbeStep {
    HttpProbeRequest expected;
    HttpProbeResult result;
};

struct FakeDelivery {
    range::ByteOffset offset = 0;
    std::vector<std::uint8_t> bytes;
};

struct FakeTransferScript {
    range::RangeLease expected_lease;
    std::vector<FakeDelivery> deliveries;
    std::optional<HttpFailure> terminal_failure;
    long response_code = 206;
};

class DeterministicHttpTransferPort final : public HttpTransferPort {
public:
    struct State;

    DeterministicHttpTransferPort(
        std::vector<FakeProbeStep> probe_steps,
        std::vector<FakeTransferScript> transfer_scripts) noexcept;
    ~DeterministicHttpTransferPort() override;

    [[nodiscard]] HttpProbeResult probe(
        const HttpProbeRequest& request) noexcept override;

    [[nodiscard]] HttpSessionOpenResult open_session(
        const HttpSessionConfig& config,
        flow::PacketProducer& packet_producer,
        telemetry::TelemetrySession& telemetry) noexcept override;

    [[nodiscard]] std::size_t consumed_probe_steps() const noexcept;
    [[nodiscard]] std::size_t consumed_transfer_scripts() const noexcept;

private:
    std::shared_ptr<State> state_;
};

}
