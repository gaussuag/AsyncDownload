#include <cstdint>
#include <limits>

#include "asyncdownload/error.hpp"
#include "download/progress_snapshot_builder.hpp"

namespace asyncdownload::download {

ProgressSnapshotBuildResult build_progress_snapshot(
    const ProgressSnapshotSources& sources) noexcept {
    ProgressSnapshotBuildResult result{};
    if (sources.recovery_trusted_bytes < 0 ||
        sources.persisted_bytes < 0 ||
        sources.packet_flow.published_data_bytes >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max() -
                sources.recovery_trusted_bytes)) {
        result.error = make_error_code(DownloadErrc::internal_error);
        return result;
    }

    auto& snapshot = result.snapshot;
    snapshot.total_bytes = sources.total_bytes;
    snapshot.downloaded_bytes =
        sources.recovery_trusted_bytes +
        static_cast<std::int64_t>(
            sources.packet_flow.published_data_bytes);
    snapshot.persisted_bytes = sources.persisted_bytes;
    snapshot.vdl_offset = sources.vdl_offset;
    snapshot.inflight_bytes =
        snapshot.downloaded_bytes > snapshot.persisted_bytes
        ? snapshot.downloaded_bytes - snapshot.persisted_bytes
        : 0;
    snapshot.queued_packets =
        sources.packet_flow.queued_packets;
    snapshot.active_requests =
        sources.http.active_transfers;
    snapshot.paused_ranges =
        sources.http.paused_transfers;
    snapshot.memory_bytes =
        sources.packet_flow.accounted_bytes;
    snapshot.network_bytes_per_second =
        sources.telemetry.network_bytes_per_second;
    snapshot.disk_bytes_per_second =
        sources.telemetry.disk_bytes_per_second;
    snapshot.resumed = sources.resumed;
    return result;
}

}
