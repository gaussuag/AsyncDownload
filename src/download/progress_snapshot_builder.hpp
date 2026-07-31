#pragma once

#include <cstdint>
#include <system_error>

#include "asyncdownload/types.hpp"
#include "flow/packet_flow.hpp"
#include "http/http_transfer.hpp"

namespace asyncdownload::download {

struct ProgressSnapshotSources {
    std::int64_t total_bytes = 0;
    std::int64_t recovery_trusted_bytes = 0;
    std::int64_t persisted_bytes = 0;
    std::int64_t vdl_offset = 0;
    flow::PacketFlowSnapshot packet_flow{};
    http::HttpSessionSnapshot http{};
    ProgressSnapshot telemetry{};
    bool resumed = false;
};

struct ProgressSnapshotBuildResult {
    ProgressSnapshot snapshot{};
    std::error_code error{};
};

[[nodiscard]] ProgressSnapshotBuildResult build_progress_snapshot(
    const ProgressSnapshotSources& sources) noexcept;

}
