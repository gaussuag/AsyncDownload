#include "asyncdownload/client.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

#ifdef _WIN32
#include <Windows.h>
#include <TlHelp32.h>
#endif

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
    std::optional<std::filesystem::path> diagnostic_file;
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

        if (argument == "--diagnostic-file") {
            if (index + 1 >= argc) {
                return false;
            }

            options.diagnostic_file = argv[++index];
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
                 "[--config <path>] [--pause-on-exit] [--summary-file <path>] "
                 "[--diagnostic-file <path>]\n";
}

struct ResourceDiagnostics {
    std::size_t sample_count = 0;
    double average_cpu_utilization_pct = 0.0;
    double peak_cpu_utilization_pct = 0.0;
    std::size_t peak_thread_count = 0;
    std::size_t peak_handle_count = 0;
};

#ifdef _WIN32

struct ProcessCpuTotals {
    unsigned long long kernel_100ns = 0;
    unsigned long long user_100ns = 0;
};

[[nodiscard]] unsigned long long combine_file_time(const FILETIME& file_time) {
    return (static_cast<unsigned long long>(file_time.dwHighDateTime) << 32ULL) |
        static_cast<unsigned long long>(file_time.dwLowDateTime);
}

[[nodiscard]] bool read_process_cpu_totals(ProcessCpuTotals& totals) {
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) == 0) {
        return false;
    }

    totals.kernel_100ns = combine_file_time(kernel);
    totals.user_100ns = combine_file_time(user);
    return true;
}

[[nodiscard]] std::size_t read_process_handle_count() {
    DWORD handle_count = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &handle_count) == 0) {
        return 0;
    }

    return static_cast<std::size_t>(handle_count);
}

[[nodiscard]] std::size_t read_process_thread_count() {
    const auto process_id = GetCurrentProcessId();
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    std::size_t count = 0;
    if (Thread32First(snapshot, &entry) == TRUE) {
        do {
            if (entry.th32OwnerProcessID == process_id) {
                ++count;
            }
            entry.dwSize = sizeof(entry);
        } while (Thread32Next(snapshot, &entry) == TRUE);
    }

    CloseHandle(snapshot);
    return count;
}

#endif

class ResourceDiagnosticsSampler {
public:
    void start() {
        sample();
    }

    void sample() {
#ifdef _WIN32
        ProcessCpuTotals current_cpu{};
        const auto now = std::chrono::steady_clock::now();
        const auto thread_count = read_process_thread_count();
        const auto handle_count = read_process_handle_count();
        diagnostics_.peak_thread_count = std::max(diagnostics_.peak_thread_count, thread_count);
        diagnostics_.peak_handle_count = std::max(diagnostics_.peak_handle_count, handle_count);

        if (read_process_cpu_totals(current_cpu)) {
            if (has_previous_cpu_) {
                const auto elapsed_seconds =
                    std::chrono::duration_cast<std::chrono::duration<double>>(now -
                        previous_sample_at_).count();
                if (elapsed_seconds > 0.0) {
                    const auto cpu_delta_100ns =
                        (current_cpu.kernel_100ns - previous_cpu_totals_.kernel_100ns) +
                        (current_cpu.user_100ns - previous_cpu_totals_.user_100ns);
                    const auto cpu_seconds = static_cast<double>(cpu_delta_100ns) / 10000000.0;
                    const auto core_count = std::max(1u, std::thread::hardware_concurrency());
                    const auto utilization = std::max(0.0,
                        (cpu_seconds / (elapsed_seconds * static_cast<double>(core_count))) *
                            100.0);
                    cpu_utilization_sum_pct_ += utilization;
                    ++cpu_sample_count_;
                    diagnostics_.peak_cpu_utilization_pct =
                        std::max(diagnostics_.peak_cpu_utilization_pct, utilization);
                }
            }

            previous_cpu_totals_ = current_cpu;
            previous_sample_at_ = now;
            has_previous_cpu_ = true;
        }
#else
        const auto now = std::chrono::steady_clock::now();
        static_cast<void>(now);
#endif
        ++diagnostics_.sample_count;
    }

    [[nodiscard]] ResourceDiagnostics finish() {
        sample();
        if (cpu_sample_count_ > 0) {
            diagnostics_.average_cpu_utilization_pct =
                cpu_utilization_sum_pct_ / static_cast<double>(cpu_sample_count_);
        }
        return diagnostics_;
    }

private:
    ResourceDiagnostics diagnostics_{};
    double cpu_utilization_sum_pct_ = 0.0;
    std::size_t cpu_sample_count_ = 0;
#ifdef _WIN32
    bool has_previous_cpu_ = false;
    ProcessCpuTotals previous_cpu_totals_{};
    std::chrono::steady_clock::time_point previous_sample_at_{};
#endif
};

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
    stream << std::fixed << std::setprecision(4);
    stream << "Summary\n";
    stream << "  status=" << (result.ok() ? "success" : "failed") << "\n";
    stream << "  total_bytes=" << result.total_bytes << "\n";
    stream << "  downloaded_bytes=" << result.downloaded_bytes << "\n";
    stream << "  persisted_bytes=" << result.persisted_bytes << "\n";
    stream << "  avg_network_speed=" << avg_net_mb << " MB/s\n";
    stream << "  avg_disk_speed=" << avg_disk_mb << " MB/s\n";
    stream << "  time_to_first_byte_ms=" << perf.time_to_first_byte_ms << "\n";
    stream << "  resumed=" << (result.resumed ? "true" : "false") << "\n";
    stream << "  max_memory_bytes=" << perf.max_memory_bytes << "\n";
    stream << "  max_inflight_bytes=" << perf.max_inflight_bytes << "\n";
    stream << "  total_pause_count=" << perf.total_pause_count << "\n";
    stream << "  queue_full_pause_count=" << perf.queue_full_pause_count << "\n";
    stream << "  packets_enqueued_total=" << perf.packets_enqueued_total << "\n";
    stream << "  avg_packet_size_bytes=" << perf.average_packet_size_bytes << "\n";
    stream << "  max_packet_size_bytes=" << perf.max_packet_size_bytes << "\n";
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

void write_diagnostic_file(const std::filesystem::path& path,
                           const asyncdownload::DownloadResult& result,
                           const ResourceDiagnostics& diagnostics) {
    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            std::cerr << "Failed to create diagnostic directory: " << ec.message() << "\n";
            return;
        }
    }

    const auto& perf = result.performance;
    nlohmann::json payload = {
        {"run_summary",
            {
                {"status", result.ok() ? "success" : "failed"},
                {"total_bytes", result.total_bytes},
                {"downloaded_bytes", result.downloaded_bytes},
                {"persisted_bytes", result.persisted_bytes},
                {"resumed", result.resumed},
                {"error", result.ok() ? "" : result.error.message()},
            }},
        {"performance_summary",
            {
                {"avg_network_speed_mb_s",
                    perf.average_network_bytes_per_second / (1024.0 * 1024.0)},
                {"avg_disk_speed_mb_s",
                    perf.average_disk_bytes_per_second / (1024.0 * 1024.0)},
                {"time_to_first_byte_ms", perf.time_to_first_byte_ms},
                {"max_memory_bytes", perf.max_memory_bytes},
                {"max_inflight_bytes", perf.max_inflight_bytes},
                {"total_pause_count", perf.total_pause_count},
                {"queue_full_pause_count", perf.queue_full_pause_count},
                {"packets_enqueued_total", perf.packets_enqueued_total},
                {"avg_packet_size_bytes", perf.average_packet_size_bytes},
                {"max_packet_size_bytes", perf.max_packet_size_bytes},
            }},
        {"resource_diagnostics",
            {
                {"sample_count", diagnostics.sample_count},
                {"average_cpu_utilization_pct", diagnostics.average_cpu_utilization_pct},
                {"peak_cpu_utilization_pct", diagnostics.peak_cpu_utilization_pct},
                {"peak_thread_count", diagnostics.peak_thread_count},
                {"peak_handle_count", diagnostics.peak_handle_count},
            }},
    };

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        std::cerr << "Failed to open diagnostic file: " << path.string() << "\n";
        return;
    }

    stream << payload.dump(2) << "\n";
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

    ResourceDiagnosticsSampler diagnostics_sampler;
    diagnostics_sampler.start();
    request.progress_callback = [&](const asyncdownload::ProgressSnapshot& snapshot) {
        diagnostics_sampler.sample();
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
    const auto diagnostics = diagnostics_sampler.finish();
    std::cout << "\n";
    write_summary(std::cout, result);
    if (cli_options.summary_file.has_value()) {
        write_summary_file(*cli_options.summary_file, result);
    }
    if (cli_options.diagnostic_file.has_value()) {
        write_diagnostic_file(*cli_options.diagnostic_file, result, diagnostics);
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
