#include "asyncdownload/client.hpp"

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

[[nodiscard]] bool parse_connections(const char* value, std::size_t& result) {
    try {
        const auto parsed = std::stoull(value);
        if (parsed == 0) {
            return false;
        }

        result = static_cast<std::size_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3 || argc > 4) {
        std::cerr << "Usage: AsyncDownload <url> <output> [connections]\n";
        return 1;
    }

    asyncdownload::DownloadRequest request{};
    request.url = argv[1];
    request.output_path = argv[2];

    if (argc == 4 && !parse_connections(argv[3], request.options.max_connections)) {
        std::cerr << "Invalid connections value\n";
        return 1;
    }

    request.progress_callback = [](const asyncdownload::ProgressSnapshot& snapshot) {
        const auto network_mb_per_second =
            snapshot.network_bytes_per_second / (1024.0 * 1024.0);
        const auto disk_mb_per_second =
            snapshot.disk_bytes_per_second / (1024.0 * 1024.0);
        const auto progress = snapshot.total_bytes > 0
            ? (100.0 * static_cast<double>(snapshot.persisted_bytes) /
                static_cast<double>(snapshot.total_bytes))
            : 0.0;
        std::cout << "\rdownloaded=" << snapshot.downloaded_bytes
                  << " persisted=" << snapshot.persisted_bytes
                  << " vdl=" << snapshot.vdl_offset
                  << " inflight=" << snapshot.inflight_bytes
                  << " queued=" << snapshot.queued_packets
                  << " active=" << snapshot.active_requests
                  << " paused=" << snapshot.paused_ranges
                  << " net=" << network_mb_per_second
                  << "MB/s"
                  << " disk=" << disk_mb_per_second
                  << "MB/s"
                  << " memory=" << snapshot.memory_bytes
                  << " progress=" << progress << "%" << std::flush;
    };

    asyncdownload::DownloadClient client;
    const auto result = client.download(request);
    std::cout << "\n";

    if (!result.ok()) {
        std::cerr << "Download failed: " << result.error.message() << "\n";
        return 1;
    }

    std::cout << "Download completed: " << request.output_path.string() << "\n";
    return 0;
}
