#include "asyncdownload/client.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
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

struct CliOptions {
    bool pause_on_exit = false;
    std::optional<std::filesystem::path> summary_file;
    std::optional<std::filesystem::path> config_file;
    std::optional<std::size_t> connections_override;
};

[[nodiscard]] bool parse_cli_options(const int argc,
                                     char** argv,
                                     CliOptions& options) {
    bool connections_parsed = false;
    for (int index = 3; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--pause-on-exit") {
            options.pause_on_exit = true;
            continue;
        }

        if (argument == "--summary-file") {
            if (index + 1 >= argc) {
                return false;
            }

            options.summary_file = argv[++index];
            continue;
        }

        if (argument == "--config") {
            if (index + 1 >= argc) {
                return false;
            }

            options.config_file = argv[++index];
            continue;
        }

        std::size_t connections = 0;
        if (!connections_parsed && parse_connections(argument.c_str(), connections)) {
            options.connections_override = connections;
            connections_parsed = true;
            continue;
        }

        return false;
    }

    return true;
}

void print_usage() {
    std::cerr << "Usage: AsyncDownload <url> <output> [connections] "
                 "[--config <path>] [--pause-on-exit] [--summary-file <path>]\n";
}

[[nodiscard]] bool read_size_field(const nlohmann::json& object,
                                   const char* key,
                                   std::size_t& target,
                                   const bool allow_zero) {
    if (!object.contains(key)) {
        return true;
    }

    const auto& field = object.at(key);
    if (!field.is_number_integer() && !field.is_number_unsigned()) {
        return false;
    }

    const auto value = field.get<std::int64_t>();
    if (value < 0 || (!allow_zero && value == 0)) {
        return false;
    }

    target = static_cast<std::size_t>(value);
    return true;
}

[[nodiscard]] bool read_bool_field(const nlohmann::json& object,
                                   const char* key,
                                   bool& target) {
    if (!object.contains(key)) {
        return true;
    }

    const auto& field = object.at(key);
    if (!field.is_boolean()) {
        return false;
    }

    target = field.get<bool>();
    return true;
}

[[nodiscard]] bool read_duration_field(const nlohmann::json& object,
                                       const char* key,
                                       std::chrono::milliseconds& target) {
    if (!object.contains(key)) {
        return true;
    }

    const auto& field = object.at(key);
    if (!field.is_number_integer() && !field.is_number_unsigned()) {
        return false;
    }

    const auto value = field.get<std::int64_t>();
    if (value < 0) {
        return false;
    }

    target = std::chrono::milliseconds(value);
    return true;
}

[[nodiscard]] bool load_download_options_from_config(
    const std::filesystem::path& path,
    asyncdownload::DownloadOptions& options,
    std::string& error_message) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        error_message = "failed to open config file";
        return false;
    }

    const auto root = nlohmann::json::parse(stream, nullptr, false, true);
    if (root.is_discarded()) {
        error_message = "failed to parse config file as JSON";
        return false;
    }

    const auto* object = &root;
    if (root.contains("download_options")) {
        object = &root.at("download_options");
    }

    if (!object->is_object()) {
        error_message = "config root must be an object";
        return false;
    }

    if (!read_size_field(*object, "max_connections", options.max_connections, false) ||
        !read_size_field(*object, "queue_capacity_packets", options.queue_capacity_packets, false) ||
        !read_size_field(*object,
            "scheduler_window_bytes",
            options.scheduler_window_bytes,
            false) ||
        !read_size_field(*object,
            "backpressure_high_bytes",
            options.backpressure_high_bytes,
            false) ||
        !read_size_field(*object,
            "backpressure_low_bytes",
            options.backpressure_low_bytes,
            true) ||
        !read_size_field(*object, "block_size", options.block_size, false) ||
        !read_size_field(*object, "io_alignment", options.io_alignment, false) ||
        !read_size_field(*object, "max_gap_bytes", options.max_gap_bytes, false) ||
        !read_size_field(*object, "flush_threshold_bytes", options.flush_threshold_bytes, false) ||
        !read_duration_field(*object, "flush_interval_ms", options.flush_interval) ||
        !read_bool_field(*object, "overwrite_existing", options.overwrite_existing)) {
        error_message = "config file contains an invalid DownloadOptions field";
        return false;
    }

    return true;
}

void write_summary(std::ostream& stream, const asyncdownload::DownloadResult& result) {
    const auto& perf = result.performance;
    const auto avg_net_mb = perf.average_network_bytes_per_second / (1024.0 * 1024.0);
    const auto avg_disk_mb = perf.average_disk_bytes_per_second / (1024.0 * 1024.0);
    const auto peak_net_mb = perf.peak_network_bytes_per_second / (1024.0 * 1024.0);
    const auto peak_disk_mb = perf.peak_disk_bytes_per_second / (1024.0 * 1024.0);
    stream << std::fixed << std::setprecision(2);
    stream << "Summary\n";
    stream << "  status=" << (result.ok() ? "success" : "failed") << "\n";
    stream << "  total_bytes=" << result.total_bytes << "\n";
    stream << "  downloaded_bytes=" << result.downloaded_bytes << "\n";
    stream << "  persisted_bytes=" << result.persisted_bytes << "\n";
    stream << "  duration_ms=" << perf.total_duration_ms << "\n";
    stream << "  avg_network_speed=" << avg_net_mb << " MB/s\n";
    stream << "  avg_disk_speed=" << avg_disk_mb << " MB/s\n";
    stream << "  peak_network_speed=" << peak_net_mb << " MB/s\n";
    stream << "  peak_disk_speed=" << peak_disk_mb << " MB/s\n";
    stream << "  time_to_first_byte_ms=" << perf.time_to_first_byte_ms << "\n";
    stream << "  time_to_first_persist_ms=" << perf.time_to_first_persist_ms << "\n";
    stream << "  resumed=" << (result.resumed ? "true" : "false") << "\n";
    stream << "  resume_reused_bytes=" << perf.resume_reused_bytes << "\n";
    stream << "  max_memory_bytes=" << perf.max_memory_bytes << "\n";
    stream << "  max_inflight_bytes=" << perf.max_inflight_bytes << "\n";
    stream << "  max_queued_packets=" << perf.max_queued_packets << "\n";
    stream << "  max_active_requests=" << perf.max_active_requests << "\n";
    stream << "  memory_pause_count=" << perf.memory_pause_count << "\n";
    stream << "  queue_full_pause_count=" << perf.queue_full_pause_count << "\n";
    stream << "  window_boundary_pause_count=" << perf.window_boundary_pause_count << "\n";
    stream << "  gap_pause_count=" << perf.gap_pause_count << "\n";
    stream << "  windows_total=" << perf.windows_total << "\n";
    stream << "  ranges_total=" << perf.ranges_total << "\n";
    stream << "  ranges_stolen=" << perf.ranges_stolen << "\n";
    stream << "  write_callback_calls=" << perf.write_callback_calls << "\n";
    stream << "  packets_enqueued_total=" << perf.packets_enqueued_total << "\n";
    stream << "  avg_packet_size_bytes=" << perf.average_packet_size_bytes << "\n";
    stream << "  max_packet_size_bytes=" << perf.max_packet_size_bytes << "\n";
    stream << "  flush_count=" << perf.flush_count << "\n";
    stream << "  flush_time_ms_total=" << perf.flush_time_ms_total << "\n";
    stream << "  metadata_save_count=" << perf.metadata_save_count << "\n";
    stream << "  metadata_save_time_ms_total=" << perf.metadata_save_time_ms_total << "\n";
    stream << "  handle_data_packet_sample_count=" << perf.handle_data_packet.sample_count << "\n";
    stream << "  handle_data_packet_avg_us=" << perf.handle_data_packet.avg_us << "\n";
    stream << "  handle_data_packet_max_us=" << perf.handle_data_packet.max_us << "\n";
    stream << "  append_bytes_sample_count=" << perf.append_bytes.sample_count << "\n";
    stream << "  append_bytes_avg_us=" << perf.append_bytes.avg_us << "\n";
    stream << "  append_bytes_max_us=" << perf.append_bytes.max_us << "\n";
    stream << "  file_write_sample_count=" << perf.file_write.sample_count << "\n";
    stream << "  file_write_avg_us=" << perf.file_write.avg_us << "\n";
    stream << "  file_write_max_us=" << perf.file_write.max_us << "\n";
    stream << "  file_write_calls_total=" << perf.file_write_calls_total << "\n";
    stream << "  staged_write_flush_count=" << perf.staged_write_flush_count << "\n";
    stream << "  staged_write_bytes_total=" << perf.staged_write_bytes_total << "\n";
    if (!result.ok()) {
        stream << "  error=" << result.error.message() << "\n";
    }
    stream.unsetf(std::ios::floatfield);
}

void write_summary_file(const std::filesystem::path& path,
                        const asyncdownload::DownloadResult& result) {
    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            std::cerr << "Failed to create summary directory: " << ec.message() << "\n";
            return;
        }
    }

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        std::cerr << "Failed to open summary file: " << path.string() << "\n";
        return;
    }

    write_summary(stream, result);
}

void maybe_pause_on_exit(const bool enabled) {
    if (!enabled) {
        return;
    }

    std::cout << "Press Enter to exit...";
    std::cout.flush();
    std::string line;
    std::getline(std::cin, line);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        print_usage();
        return 1;
    }

    asyncdownload::DownloadRequest request{};
    request.url = argv[1];
    request.output_path = argv[2];
    CliOptions cli_options{};

    if (!parse_cli_options(argc, argv, cli_options)) {
        print_usage();
        return 1;
    }

    if (cli_options.config_file.has_value()) {
        std::string error_message;
        if (!load_download_options_from_config(*cli_options.config_file,
                request.options,
                error_message)) {
            std::cerr << "Failed to load config file '"
                      << cli_options.config_file->string()
                      << "': "
                      << error_message
                      << "\n";
            return 1;
        }
    }

    if (cli_options.connections_override.has_value()) {
        request.options.max_connections = *cli_options.connections_override;
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
    write_summary(std::cout, result);
    if (cli_options.summary_file.has_value()) {
        write_summary_file(*cli_options.summary_file, result);
    }

    if (!result.ok()) {
        std::cerr << "Download failed: " << result.error.message() << "\n";
        maybe_pause_on_exit(cli_options.pause_on_exit);
        return 1;
    }

    std::cout << "Download completed: " << request.output_path.string() << "\n";
    maybe_pause_on_exit(cli_options.pause_on_exit);
    return 0;
}
