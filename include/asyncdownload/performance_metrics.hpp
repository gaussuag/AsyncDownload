#pragma once

#include <cstddef>
#include <cstdint>

namespace asyncdownload::performance {

struct SummaryPerformanceMetrics {
    double average_network_bytes_per_second{0.0};
    double average_disk_bytes_per_second{0.0};
    std::int64_t time_to_first_byte_ms{0};
    std::size_t max_memory_bytes{0};
    std::int64_t max_inflight_bytes{0};
    std::size_t total_pause_count{0};
    std::size_t queue_full_pause_count{0};
    std::size_t packets_enqueued_total{0};
    double average_packet_size_bytes{0.0};
    std::size_t max_packet_size_bytes{0};
};

} // namespace asyncdownload::performance
