#include "asyncdownload/client.hpp"
#include "core/block_bitmap.hpp"
#include "core/crc32.hpp"
#include "metadata/metadata_store.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <set>
#include <sstream>
#include <span>
#include <string_view>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace {

#ifdef _WIN32

std::wstring quote_arg(const std::wstring& value) {
    if (value.find_first_of(L" \t\"") == std::wstring::npos) {
        return value;
    }

    std::wstring result = L"\"";
    for (const auto ch : value) {
        if (ch == L'\"') {
            result += L'\\';
        }
        result += ch;
    }
    result += L"\"";
    return result;
}

void append_log(const std::filesystem::path& path, const std::string& message) {
    std::ofstream stream(path, std::ios::app);
    stream << message << std::endl;
}

struct ChildProcess {
    PROCESS_INFORMATION process_info{};
    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;

    ChildProcess() {
        process_info.hProcess = nullptr;
        process_info.hThread = nullptr;
    }

    ~ChildProcess() {
        close();
    }

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    void close() {
        if (stdout_read != nullptr) {
            CloseHandle(stdout_read);
            stdout_read = nullptr;
        }
        if (stdout_write != nullptr) {
            CloseHandle(stdout_write);
            stdout_write = nullptr;
        }
        if (process_info.hThread != nullptr) {
            CloseHandle(process_info.hThread);
            process_info.hThread = nullptr;
        }
        if (process_info.hProcess != nullptr) {
            CloseHandle(process_info.hProcess);
            process_info.hProcess = nullptr;
        }
    }

    [[nodiscard]] bool running() const {
        return process_info.hProcess != nullptr;
    }

    [[nodiscard]] DWORD wait(const DWORD timeout_ms) const {
        return WaitForSingleObject(process_info.hProcess, timeout_ms);
    }

    [[nodiscard]] DWORD exit_code() const {
        DWORD code = 0;
        if (process_info.hProcess != nullptr) {
            GetExitCodeProcess(process_info.hProcess, &code);
        }
        return code;
    }

    void terminate(const UINT exit_code) const {
        if (process_info.hProcess != nullptr) {
            TerminateProcess(process_info.hProcess, exit_code);
        }
    }
};

void stop_child(ChildProcess& child, const UINT exit_code) {
    if (!child.running()) {
        return;
    }
    if (child.wait(0) == WAIT_TIMEOUT) {
        child.terminate(exit_code);
        const auto wait_result = child.wait(5000);
        static_cast<void>(wait_result);
    }
    child.close();
}

std::filesystem::path get_test_executable_path() {
    std::array<wchar_t, 4096> buffer{};
    const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    return std::filesystem::path(std::wstring(buffer.data(), length));
}

std::filesystem::path get_workspace_root() {
    return get_test_executable_path().parent_path().parent_path().parent_path().parent_path();
}

std::filesystem::path get_cli_path() {
    const auto test_executable = get_test_executable_path();
    const auto configuration = test_executable.parent_path().filename();
    const auto build_root = test_executable.parent_path().parent_path().parent_path();
    const auto from_test_bin = build_root / "src" / configuration / "AsyncDownload.exe";
    if (std::filesystem::exists(from_test_bin)) {
        return from_test_bin;
    }

    const auto from_workspace = get_workspace_root() / "build" / "src" / configuration /
        "AsyncDownload.exe";
    if (std::filesystem::exists(from_workspace)) {
        return from_workspace;
    }

    const auto release_fallback = get_workspace_root() / "build" / "src" / "Release" /
        "AsyncDownload.exe";
    if (std::filesystem::exists(release_fallback)) {
        return release_fallback;
    }

    const auto debug_fallback = get_workspace_root() / "build" / "src" / "Debug" /
        "AsyncDownload.exe";
    if (std::filesystem::exists(debug_fallback)) {
        return debug_fallback;
    }

    return from_workspace;
}

std::filesystem::path make_unique_temp_root(const std::string_view prefix) {
    const auto ticks = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return std::filesystem::temp_directory_path() /
        (std::string(prefix) + "_" + std::to_string(GetCurrentProcessId()) + "_" +
            std::to_string(ticks));
}

DWORD g_last_start_process_error = 0;

bool start_process(ChildProcess& child,
                   const std::wstring& application_name,
                   const std::wstring& command_line,
                   const std::filesystem::path& working_directory,
                   const bool capture_stdout) {
    SECURITY_ATTRIBUTES security_attributes{};
    security_attributes.nLength = sizeof(security_attributes);
    security_attributes.bInheritHandle = TRUE;

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);

    if (capture_stdout) {
        if (!CreatePipe(&child.stdout_read, &child.stdout_write, &security_attributes, 0)) {
            g_last_start_process_error = GetLastError();
            return false;
        }
        SetHandleInformation(child.stdout_read, HANDLE_FLAG_INHERIT, 0);
        startup_info.dwFlags |= STARTF_USESTDHANDLES;
        startup_info.hStdOutput = child.stdout_write;
        startup_info.hStdError = child.stdout_write;
        startup_info.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }

    auto mutable_command = command_line;
    const wchar_t* application_ptr = application_name.empty() ? nullptr : application_name.c_str();
    const auto created = CreateProcessW(
        application_ptr,
        mutable_command.data(),
        nullptr,
        nullptr,
        capture_stdout ? TRUE : FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        working_directory.c_str(),
        &startup_info,
        &child.process_info);

    if (child.stdout_write != nullptr) {
        CloseHandle(child.stdout_write);
        child.stdout_write = nullptr;
    }

    if (created != TRUE) {
        g_last_start_process_error = GetLastError();
        child.close();
        return false;
    }

    g_last_start_process_error = 0;
    return true;
}

std::string read_line_from_pipe(HANDLE pipe_handle, const DWORD timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::string line;
    char ch = 0;
    DWORD read = 0;

    while (std::chrono::steady_clock::now() < deadline) {
        DWORD available = 0;
        if (PeekNamedPipe(pipe_handle, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            if (!ReadFile(pipe_handle, &ch, 1, &read, nullptr) || read == 0) {
                break;
            }
            if (ch == '\n') {
                break;
            }
            if (ch != '\r') {
                line.push_back(ch);
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    return line;
}

std::string read_all_from_pipe(HANDLE pipe_handle) {
    std::string output;
    if (pipe_handle == nullptr) {
        return output;
    }

    std::array<char, 4096> buffer{};
    DWORD read = 0;
    while (ReadFile(pipe_handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) &&
           read > 0) {
        output.append(buffer.data(), read);
    }
    return output;
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return {};
    }

    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

void write_test_file(const std::filesystem::path& path, const std::size_t size_bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(stream.is_open());

    std::array<char, 4096> chunk{};
    for (std::size_t index = 0; index < chunk.size(); ++index) {
        chunk[index] = static_cast<char>(index % 251);
    }

    std::size_t remaining = size_bytes;
    while (remaining > 0) {
        const auto to_write = std::min<std::size_t>(remaining, chunk.size());
        stream.write(chunk.data(), static_cast<std::streamsize>(to_write));
        remaining -= to_write;
    }
}

std::vector<std::byte> read_file_bytes(const std::filesystem::path& path,
                                       const std::int64_t offset,
                                       const std::size_t length) {
    std::ifstream stream(path, std::ios::binary);
    EXPECT_TRUE(stream.is_open());
    stream.seekg(offset);

    std::vector<std::byte> bytes(length);
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(length));
    EXPECT_EQ(stream.gcount(), static_cast<std::streamsize>(length));
    return bytes;
}

void overwrite_bytes(const std::filesystem::path& path,
                     const std::int64_t offset,
                     const std::span<const std::byte> bytes) {
    std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(stream.is_open());
    stream.seekp(offset);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
}

bool wait_for_path(const std::filesystem::path& path, const std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::exists(path)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return std::filesystem::exists(path);
}

bool files_equal(const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
    if (!std::filesystem::exists(lhs) || !std::filesystem::exists(rhs)) {
        return false;
    }
    if (std::filesystem::file_size(lhs) != std::filesystem::file_size(rhs)) {
        return false;
    }

    std::ifstream left(lhs, std::ios::binary);
    std::ifstream right(rhs, std::ios::binary);
    if (!left.is_open() || !right.is_open()) {
        return false;
    }

    std::array<char, 8192> left_buffer{};
    std::array<char, 8192> right_buffer{};
    while (left && right) {
        left.read(left_buffer.data(), static_cast<std::streamsize>(left_buffer.size()));
        right.read(right_buffer.data(), static_cast<std::streamsize>(right_buffer.size()));
        const auto left_count = left.gcount();
        const auto right_count = right.gcount();
        if (left_count != right_count) {
            return false;
        }
        if (!std::equal(left_buffer.begin(), left_buffer.begin() + left_count, right_buffer.begin())) {
            return false;
        }
    }

    return true;
}

std::set<int> read_logged_ports(const std::filesystem::path& path) {
    std::set<int> ports;
    std::ifstream stream(path);
    if (!stream.is_open()) {
        return ports;
    }

    std::string line;
    while (std::getline(stream, line)) {
        std::istringstream parser(line);
        std::string method;
        int port = 0;
        parser >> method >> port;
        if (method == "GET" && port > 0) {
            ports.insert(port);
        }
    }

    return ports;
}

#endif

TEST(DownloadIntegrationTest, ResumeAfterInterruptedCliDownload) {
#ifndef _WIN32
    GTEST_SKIP() << "This integration test currently uses Windows process control.";
#else
    const auto workspace_root = get_workspace_root();
    const auto temp_root = std::filesystem::temp_directory_path() / "asyncdownload_resume_integration";
    std::error_code ec;
    const auto removed = std::filesystem::remove_all(temp_root, ec);
    static_cast<void>(removed);
    std::filesystem::create_directories(temp_root, ec);
    ASSERT_FALSE(ec);

    const auto trace_file = temp_root / "trace.log";
    append_log(trace_file, "start");

    const auto source_file = temp_root / "source.bin";
    const auto output_file = temp_root / "downloaded.bin";
    const auto part_file = std::filesystem::path(output_file.string() + ".part");
    const auto metadata_file = std::filesystem::path(output_file.string() + ".config.json");
    write_test_file(source_file, 64 * 1024 * 1024);
    append_log(trace_file, std::string("source_size=") +
        std::to_string(static_cast<unsigned long long>(std::filesystem::file_size(source_file))));
    append_log(trace_file, "source_ready");

    ChildProcess server;
    const auto script_path = workspace_root / "tests" / "support" / "range_server.py";
    const auto server_command = quote_arg(L"python") + L" " +
        quote_arg(script_path.wstring()) + L" " +
        quote_arg(source_file.wstring()) + L" --port 0 --chunk-size 65536 --delay-ms 20";
    append_log(trace_file, std::string("script_path=") + script_path.string());
    append_log(trace_file, "starting_server");
    ASSERT_TRUE(start_process(server, L"", server_command, workspace_root, true));
    append_log(trace_file, "server_started");

    const auto port_line = read_line_from_pipe(server.stdout_read, 5000);
    append_log(trace_file, std::string("port=") + port_line);
    if (port_line.empty()) {
        stop_child(server, 0);
        FAIL() << "range server did not report a port";
    }

    const auto url = std::string("http://127.0.0.1:") + port_line + "/source.bin";

    ChildProcess cli;
    const auto cli_path = get_cli_path();
    append_log(trace_file, std::string("cli_path=") + cli_path.string());
    append_log(trace_file, std::string("cli_exists=") +
        (std::filesystem::exists(cli_path) ? "true" : "false"));
    if (!std::filesystem::exists(cli_path)) {
        stop_child(server, 0);
        FAIL() << "CLI executable not found: " << cli_path.string();
    }

    const auto cli_command = quote_arg(cli_path.wstring()) + L" " +
        quote_arg(std::wstring(url.begin(), url.end())) + L" " +
        quote_arg(output_file.wstring()) + L" 4";
    append_log(trace_file, "starting_cli");
    if (!start_process(cli, cli_path.wstring(), cli_command, workspace_root, true)) {
        append_log(trace_file, std::string("cli_start_error=") +
            std::to_string(static_cast<unsigned long long>(g_last_start_process_error)));
        stop_child(server, 0);
        FAIL() << "failed to start CLI process, error=" << g_last_start_process_error;
    }
    append_log(trace_file, "cli_started");

    const auto metadata_ready = wait_for_path(metadata_file, std::chrono::milliseconds(6000));
    append_log(trace_file, std::string("metadata_ready=") + (metadata_ready ? "true" : "false"));
    append_log(trace_file, std::string("cli_wait_before_kill=") +
        std::to_string(static_cast<unsigned long long>(cli.wait(0))));
    append_log(trace_file, "waiting_before_kill");
    if (!metadata_ready) {
        append_log(trace_file, std::string("cli_exit_code=") +
            std::to_string(static_cast<unsigned long long>(cli.exit_code())));
        append_log(trace_file, std::string("cli_output=") + read_all_from_pipe(cli.stdout_read));
        stop_child(cli, 99);
        stop_child(server, 0);
        FAIL() << "metadata file was not persisted before interruption";
    }

    if (cli.wait(0) == WAIT_TIMEOUT) {
        append_log(trace_file, "terminating_cli");
        cli.terminate(99);
        const auto cli_wait_result = cli.wait(5000);
        static_cast<void>(cli_wait_result);
    }
    append_log(trace_file, std::string("cli_exit_code=") +
        std::to_string(static_cast<unsigned long long>(cli.exit_code())));
    append_log(trace_file, std::string("cli_output=") + read_all_from_pipe(cli.stdout_read));
    cli.close();
    append_log(trace_file, "cli_closed");

    append_log(trace_file, std::string("part_exists=") +
        (std::filesystem::exists(part_file) ? "true" : "false"));
    append_log(trace_file, std::string("metadata_exists=") +
        (std::filesystem::exists(metadata_file) ? "true" : "false"));
    if (!std::filesystem::exists(part_file) || !std::filesystem::exists(metadata_file)) {
        stop_child(server, 0);
        FAIL() << "interrupted CLI run did not leave resume artifacts";
    }

    asyncdownload::DownloadClient client;
    asyncdownload::DownloadRequest request{};
    request.url = url;
    request.output_path = output_file;
    request.options.max_connections = 4;

    append_log(trace_file, "starting_resume");
    const auto result = client.download(request);
    append_log(trace_file, std::string("resume_ok=") + (result.ok() ? "true" : "false") +
        ";error=" + result.error.message());

    append_log(trace_file, "stopping_server");
    stop_child(server, 0);

    ASSERT_TRUE(result.ok()) << result.error.message();
    EXPECT_TRUE(result.resumed);
    EXPECT_GT(result.performance.resume_reused_bytes, 0);
    EXPECT_TRUE(std::filesystem::exists(output_file));
    EXPECT_FALSE(std::filesystem::exists(part_file));
    EXPECT_FALSE(std::filesystem::exists(metadata_file));
    EXPECT_TRUE(files_equal(source_file, output_file));

    append_log(trace_file, "finished_asserts");
    const auto final_removed = std::filesystem::remove_all(temp_root, ec);
    static_cast<void>(final_removed);
#endif
}

TEST(DownloadIntegrationTest, LoadsDownloadOptionsFromConfigFile) {
#ifndef _WIN32
    GTEST_SKIP() << "This integration test currently uses Windows process control.";
#else
    const auto workspace_root = get_workspace_root();
    const auto temp_root = make_unique_temp_root("asyncdownload_cli_config_integration");
    std::error_code ec;
    const auto removed = std::filesystem::remove_all(temp_root, ec);
    static_cast<void>(removed);
    std::filesystem::create_directories(temp_root, ec);
    ASSERT_FALSE(ec);

    const auto source_file = temp_root / "source.bin";
    const auto output_file = temp_root / "downloaded.bin";
    const auto summary_file = temp_root / "summary.txt";
    const auto config_file = temp_root / "download_options.json";
    const auto cli_path = get_cli_path();
    write_test_file(source_file, 8 * 1024 * 1024);
    ASSERT_TRUE(std::filesystem::exists(cli_path));

    {
        std::ofstream stream(config_file, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(stream.is_open());
        stream << "{\n"
               << "  \"download_options\": {\n"
               << "    \"max_connections\": 1,\n"
               << "    \"scheduler_window_bytes\": 1048576,\n"
               << "    \"flush_threshold_bytes\": 1048576,\n"
               << "    \"flush_interval_ms\": 1000\n"
               << "  }\n"
               << "}\n";
    }

    ChildProcess server;
    const auto script_path = workspace_root / "tests" / "support" / "range_server.py";
    const auto server_command = quote_arg(L"python") + L" " +
        quote_arg(script_path.wstring()) + L" " +
        quote_arg(source_file.wstring()) + L" --port 0 --chunk-size 65536 --delay-ms 5";
    ASSERT_TRUE(start_process(server, L"", server_command, workspace_root, true));

    const auto port_line = read_line_from_pipe(server.stdout_read, 5000);
    ASSERT_FALSE(port_line.empty());

    ChildProcess cli;
    const auto url = std::string("http://127.0.0.1:") + port_line + "/source.bin";
    const auto cli_command = quote_arg(cli_path.wstring()) + L" " +
        quote_arg(std::wstring(url.begin(), url.end())) + L" " +
        quote_arg(output_file.wstring()) + L" --config " +
        quote_arg(config_file.wstring()) + L" --summary-file " +
        quote_arg(summary_file.wstring());
    ASSERT_TRUE(start_process(cli, cli_path.wstring(), cli_command, workspace_root, false));
    ASSERT_EQ(cli.wait(20000), WAIT_OBJECT_0);
    const auto exit_code = cli.exit_code();
    cli.close();
    stop_child(server, 0);

    ASSERT_EQ(exit_code, 0U);
    ASSERT_TRUE(std::filesystem::exists(output_file));
    ASSERT_TRUE(files_equal(source_file, output_file));

    const auto summary_text = read_text_file(summary_file);
    EXPECT_NE(summary_text.find("status=success"), std::string::npos);
    EXPECT_NE(summary_text.find("windows_total=8"), std::string::npos);
    EXPECT_NE(summary_text.find("ranges_total=1"), std::string::npos);

    const auto final_removed = std::filesystem::remove_all(temp_root, ec);
    static_cast<void>(final_removed);
#endif
}

TEST(DownloadIntegrationTest, ResumesAfterCrcRollbackPastVdl) {
#ifndef _WIN32
    GTEST_SKIP() << "This integration test currently uses Windows process control.";
#else
    const auto workspace_root = get_workspace_root();
    const auto temp_root =
        std::filesystem::temp_directory_path() / "asyncdownload_crc_resume_integration";
    std::error_code ec;
    const auto removed = std::filesystem::remove_all(temp_root, ec);
    static_cast<void>(removed);
    std::filesystem::create_directories(temp_root, ec);
    ASSERT_FALSE(ec);

    const auto trace_file = temp_root / "trace.log";
    append_log(trace_file, "start");

    constexpr std::size_t block_size = 64 * 1024;
    constexpr std::size_t total_size = 16 * block_size;

    const auto source_file = temp_root / "source.bin";
    const auto output_file = temp_root / "recovered.bin";
    const auto part_file = std::filesystem::path(output_file.string() + ".part");
    const auto metadata_file = std::filesystem::path(output_file.string() + ".config.json");
    write_test_file(source_file, total_size);
    std::filesystem::copy_file(source_file, part_file, std::filesystem::copy_options::overwrite_existing,
        ec);
    ASSERT_FALSE(ec);

    std::vector<std::byte> corrupt_block(block_size, std::byte{0x5A});
    overwrite_bytes(part_file, static_cast<std::int64_t>(block_size), corrupt_block);

    asyncdownload::metadata::MetadataStore metadata_store(metadata_file);
    asyncdownload::core::MetadataState state{};
    state.url = "placeholder";
    state.output_path = output_file;
    state.temporary_path = part_file;
    state.total_size = total_size;
    state.vdl_offset = block_size;
    state.accept_ranges = true;
    state.resumed = true;
    state.block_size = block_size;
    state.io_alignment = 4 * 1024;
    state.bitmap_states.assign(total_size / block_size,
        static_cast<std::uint8_t>(asyncdownload::core::BlockState::finished));
    state.ranges.push_back({0,
        0,
        static_cast<std::int64_t>(total_size - 1),
        static_cast<std::int64_t>(total_size),
        static_cast<std::int64_t>(total_size),
        static_cast<std::uint8_t>(asyncdownload::core::RangeStatus::finished)});

    for (std::size_t block = 1; block < total_size / block_size; ++block) {
        const auto offset = static_cast<std::int64_t>(block * block_size);
        const auto bytes = read_file_bytes(source_file, offset, block_size);
        state.crc_samples.push_back({offset, asyncdownload::core::crc32(bytes), block_size});
    }

    ASSERT_FALSE(metadata_store.save(state));
    append_log(trace_file, "metadata_ready");

    ChildProcess server;
    const auto script_path = workspace_root / "tests" / "support" / "range_server.py";
    const auto server_command = quote_arg(L"python") + L" " +
        quote_arg(script_path.wstring()) + L" " +
        quote_arg(source_file.wstring()) + L" --port 0 --chunk-size 65536 --delay-ms 5";
    ASSERT_TRUE(start_process(server, L"", server_command, workspace_root, true));

    const auto port_line = read_line_from_pipe(server.stdout_read, 5000);
    ASSERT_FALSE(port_line.empty());

    asyncdownload::DownloadRequest request{};
    request.url = std::string("http://127.0.0.1:") + port_line + "/source.bin";
    request.output_path = output_file;
    request.options.max_connections = 2;
    request.options.block_size = block_size;
    request.options.scheduler_window_bytes = block_size;
    request.options.io_alignment = 4 * 1024;

    state.url = request.url;
    ASSERT_FALSE(metadata_store.save(state));

    asyncdownload::DownloadClient client;
    const auto result = client.download(request);
    stop_child(server, 0);

    ASSERT_TRUE(result.ok()) << result.error.message();
    EXPECT_TRUE(result.resumed);
    EXPECT_TRUE(std::filesystem::exists(output_file));
    EXPECT_FALSE(std::filesystem::exists(part_file));
    EXPECT_FALSE(std::filesystem::exists(metadata_file));
    EXPECT_TRUE(files_equal(source_file, output_file));

    const auto final_removed = std::filesystem::remove_all(temp_root, ec);
    static_cast<void>(final_removed);
#endif
}

TEST(DownloadIntegrationTest, ResetsDownloadingBlocksToEmptyOnResume) {
#ifndef _WIN32
    GTEST_SKIP() << "This integration test currently uses Windows process control.";
#else
    const auto workspace_root = get_workspace_root();
    const auto temp_root =
        std::filesystem::temp_directory_path() / "asyncdownload_downloading_resume_integration";
    std::error_code ec;
    const auto removed = std::filesystem::remove_all(temp_root, ec);
    static_cast<void>(removed);
    std::filesystem::create_directories(temp_root, ec);
    ASSERT_FALSE(ec);

    constexpr std::size_t block_size = 64 * 1024;
    constexpr std::size_t total_size = 8 * block_size;

    const auto source_file = temp_root / "source.bin";
    const auto output_file = temp_root / "recovered.bin";
    const auto part_file = std::filesystem::path(output_file.string() + ".part");
    const auto metadata_file = std::filesystem::path(output_file.string() + ".config.json");
    write_test_file(source_file, total_size);
    std::filesystem::copy_file(source_file, part_file, std::filesystem::copy_options::overwrite_existing,
        ec);
    ASSERT_FALSE(ec);

    std::vector<std::byte> corrupt_block(block_size, std::byte{0x66});
    overwrite_bytes(part_file, static_cast<std::int64_t>(block_size), corrupt_block);

    asyncdownload::metadata::MetadataStore metadata_store(metadata_file);
    asyncdownload::core::MetadataState state{};
    state.url = "placeholder";
    state.output_path = output_file;
    state.temporary_path = part_file;
    state.total_size = total_size;
    state.vdl_offset = block_size;
    state.accept_ranges = true;
    state.resumed = true;
    state.block_size = block_size;
    state.io_alignment = 4 * 1024;
    state.bitmap_states.assign(total_size / block_size,
        static_cast<std::uint8_t>(asyncdownload::core::BlockState::empty));
    state.bitmap_states[0] = static_cast<std::uint8_t>(asyncdownload::core::BlockState::finished);
    state.bitmap_states[1] =
        static_cast<std::uint8_t>(asyncdownload::core::BlockState::downloading);
    state.ranges.push_back({0,
        0,
        static_cast<std::int64_t>(total_size - 1),
        static_cast<std::int64_t>(block_size),
        static_cast<std::int64_t>(block_size),
        static_cast<std::uint8_t>(asyncdownload::core::RangeStatus::paused)});
    ASSERT_FALSE(metadata_store.save(state));

    ChildProcess server;
    const auto script_path = workspace_root / "tests" / "support" / "range_server.py";
    const auto server_command = quote_arg(L"python") + L" " +
        quote_arg(script_path.wstring()) + L" " +
        quote_arg(source_file.wstring()) + L" --port 0 --chunk-size 65536 --delay-ms 5";
    ASSERT_TRUE(start_process(server, L"", server_command, workspace_root, true));

    const auto port_line = read_line_from_pipe(server.stdout_read, 5000);
    ASSERT_FALSE(port_line.empty());

    asyncdownload::DownloadRequest request{};
    request.url = std::string("http://127.0.0.1:") + port_line + "/source.bin";
    request.output_path = output_file;
    request.options.max_connections = 2;
    request.options.block_size = block_size;
    request.options.scheduler_window_bytes = block_size;
    request.options.io_alignment = 4 * 1024;

    state.url = request.url;
    ASSERT_FALSE(metadata_store.save(state));

    asyncdownload::DownloadClient client;
    const auto result = client.download(request);
    stop_child(server, 0);

    ASSERT_TRUE(result.ok()) << result.error.message();
    EXPECT_TRUE(result.resumed);
    EXPECT_TRUE(std::filesystem::exists(output_file));
    EXPECT_FALSE(std::filesystem::exists(part_file));
    EXPECT_FALSE(std::filesystem::exists(metadata_file));
    EXPECT_TRUE(files_equal(source_file, output_file));

    const auto final_removed = std::filesystem::remove_all(temp_root, ec);
    static_cast<void>(final_removed);
#endif
}

TEST(DownloadIntegrationTest, UsesDistinctClientPortsAcrossConcurrentRanges) {
#ifndef _WIN32
    GTEST_SKIP() << "This integration test currently uses Windows process control.";
#else
    const auto workspace_root = get_workspace_root();
    const auto temp_root =
        std::filesystem::temp_directory_path() / "asyncdownload_connection_isolation";
    std::error_code ec;
    const auto removed = std::filesystem::remove_all(temp_root, ec);
    static_cast<void>(removed);
    std::filesystem::create_directories(temp_root, ec);
    ASSERT_FALSE(ec);

    const auto source_file = temp_root / "source.bin";
    const auto output_file = temp_root / "downloaded.bin";
    const auto request_log = temp_root / "requests.log";
    write_test_file(source_file, 32 * 1024 * 1024);

    ChildProcess server;
    const auto script_path = workspace_root / "tests" / "support" / "range_server.py";
    const auto server_command = quote_arg(L"python") + L" " +
        quote_arg(script_path.wstring()) + L" " +
        quote_arg(source_file.wstring()) +
        L" --port 0 --chunk-size 65536 --delay-ms 20 --request-log " +
        quote_arg(request_log.wstring());
    ASSERT_TRUE(start_process(server, L"", server_command, workspace_root, true));

    const auto port_line = read_line_from_pipe(server.stdout_read, 5000);
    ASSERT_FALSE(port_line.empty());

    asyncdownload::DownloadRequest request{};
    request.url = std::string("http://127.0.0.1:") + port_line + "/source.bin";
    request.output_path = output_file;
    request.options.max_connections = 4;
    request.options.block_size = 64 * 1024;
    request.options.scheduler_window_bytes = 1 * 1024 * 1024;

    asyncdownload::DownloadClient client;
    const auto result = client.download(request);
    stop_child(server, 0);

    ASSERT_TRUE(result.ok()) << result.error.message();
    const auto ports = read_logged_ports(request_log);
    EXPECT_GE(ports.size(), 4U);
    EXPECT_TRUE(files_equal(source_file, output_file));

    const auto final_removed = std::filesystem::remove_all(temp_root, ec);
    static_cast<void>(final_removed);
#endif
}

TEST(DownloadIntegrationTest, ReportsDetailedProgressSnapshot) {
#ifndef _WIN32
    GTEST_SKIP() << "This integration test currently uses Windows process control.";
#else
    const auto workspace_root = get_workspace_root();
    const auto temp_root = make_unique_temp_root("asyncdownload_progress_snapshot_integration");
    std::error_code ec;
    const auto removed = std::filesystem::remove_all(temp_root, ec);
    static_cast<void>(removed);
    std::filesystem::create_directories(temp_root, ec);
    ASSERT_FALSE(ec);

    const auto source_file = temp_root / "source.bin";
    const auto output_file = temp_root / "progress.bin";
    write_test_file(source_file, 32 * 1024 * 1024);

    ChildProcess server;
    const auto script_path = workspace_root / "tests" / "support" / "range_server.py";
    const auto server_command = quote_arg(L"python") + L" " +
        quote_arg(script_path.wstring()) + L" " +
        quote_arg(source_file.wstring()) + L" --port 0 --chunk-size 65536 --delay-ms 0";
    ASSERT_TRUE(start_process(server, L"", server_command, workspace_root, true));

    const auto port_line = read_line_from_pipe(server.stdout_read, 5000);
    ASSERT_FALSE(port_line.empty());

    std::mutex snapshots_mutex;
    std::vector<asyncdownload::ProgressSnapshot> snapshots;

    asyncdownload::DownloadRequest request{};
    request.url = std::string("http://127.0.0.1:") + port_line + "/source.bin";
    request.output_path = output_file;
    request.options.max_connections = 4;
    request.options.scheduler_window_bytes = 1 * 1024 * 1024;
    request.options.flush_threshold_bytes = 64 * 1024;
    request.options.flush_interval = std::chrono::milliseconds(100);
    request.progress_callback = [&](const asyncdownload::ProgressSnapshot& snapshot) {
        std::scoped_lock lock(snapshots_mutex);
        snapshots.push_back(snapshot);
    };

    asyncdownload::DownloadClient client;
    const auto result = client.download(request);
    stop_child(server, 0);

    ASSERT_TRUE(result.ok()) << result.error.message();
    EXPECT_GT(result.performance.total_duration_ms, 0);
    EXPECT_GT(result.performance.average_network_bytes_per_second, 0.0);
    EXPECT_GT(result.performance.average_disk_bytes_per_second, 0.0);
    EXPECT_GT(result.performance.peak_network_bytes_per_second, 0.0);
    EXPECT_GT(result.performance.peak_disk_bytes_per_second, 0.0);
    EXPECT_GE(result.performance.peak_network_bytes_per_second,
        result.performance.average_network_bytes_per_second);
    EXPECT_GE(result.performance.peak_disk_bytes_per_second,
        result.performance.average_disk_bytes_per_second);
    EXPECT_GE(result.performance.time_to_first_byte_ms, 0);
    EXPECT_GE(result.performance.time_to_first_persist_ms, 0);
    EXPECT_GT(result.performance.max_memory_bytes, 0U);
    EXPECT_GT(result.performance.max_inflight_bytes, 0);
    EXPECT_GT(result.performance.max_queued_packets, 0U);
    EXPECT_GT(result.performance.max_queued_bytes, 0);
    EXPECT_GT(result.performance.max_active_requests, 0U);
    EXPECT_GE(result.performance.memory_pause_count, 0U);
    EXPECT_GE(result.performance.queue_full_pause_count, 0U);
    EXPECT_GE(result.performance.queue_full_pause_capacity_reached_count, 0U);
    EXPECT_GE(result.performance.queue_full_pause_try_enqueue_failure_count, 0U);
    EXPECT_GE(result.performance.max_queue_paused_handles, 0U);
    EXPECT_GE(result.performance.max_memory_paused_handles, 0U);
    EXPECT_GE(result.performance.queue_full_resume_count, 0U);
    EXPECT_GE(result.performance.memory_resume_count, 0U);
    EXPECT_GT(result.performance.windows_total, 0U);
    EXPECT_GT(result.performance.ranges_total, 0U);
    EXPECT_GT(result.performance.write_callback_calls, 0U);
    EXPECT_GT(result.performance.packets_enqueued_total, 0U);
    EXPECT_GT(result.performance.average_packet_size_bytes, 0.0);
    EXPECT_GT(result.performance.max_packet_size_bytes, 0U);
    EXPECT_GE(result.performance.queue_full_pause_start_queued_packets_total, 0U);
    EXPECT_GE(result.performance.queue_full_pause_start_queued_bytes_total, 0);
    EXPECT_GE(result.performance.memory_pause_start_queued_packets_total, 0U);
    EXPECT_GE(result.performance.memory_pause_start_queued_bytes_total, 0);
    EXPECT_GE(result.performance.queue_full_pause_avg_ms, 0.0);
    EXPECT_GE(result.performance.queue_full_pause_max_ms, 0.0);
    EXPECT_GE(result.performance.memory_pause_avg_ms, 0.0);
    EXPECT_GE(result.performance.memory_pause_max_ms, 0.0);
    EXPECT_GE(result.performance.queue_full_pause_start_queued_packets_avg, 0.0);
    EXPECT_GE(result.performance.queue_full_pause_start_queued_bytes_avg, 0.0);
    EXPECT_GE(result.performance.memory_pause_start_queued_packets_avg, 0.0);
    EXPECT_GE(result.performance.memory_pause_start_queued_bytes_avg, 0.0);
    EXPECT_GE(result.performance.queue_resume_blocked_by_memory_count, 0U);
    EXPECT_GE(result.performance.queue_pause_overlap_memory_count, 0U);
    EXPECT_GE(result.performance.queue_resume_blocked_by_memory_avg_ms, 0.0);
    EXPECT_GE(result.performance.queue_resume_blocked_by_memory_max_ms, 0.0);
    EXPECT_GE(result.performance.write_callback_calls,
        result.performance.packets_enqueued_total);
    EXPECT_GE(static_cast<double>(result.performance.max_packet_size_bytes),
        result.performance.average_packet_size_bytes);
    EXPECT_GT(result.performance.flush_count, 0U);
    EXPECT_GT(result.performance.metadata_save_count, 0U);

    std::vector<asyncdownload::ProgressSnapshot> captured;
    {
        std::scoped_lock lock(snapshots_mutex);
        captured = snapshots;
    }

    ASSERT_FALSE(captured.empty());
    EXPECT_TRUE(std::any_of(captured.begin(), captured.end(),
        [](const asyncdownload::ProgressSnapshot& snapshot) {
            return snapshot.active_requests > 0;
        }));
    EXPECT_TRUE(std::any_of(captured.begin(), captured.end(),
        [](const asyncdownload::ProgressSnapshot& snapshot) {
            return snapshot.network_bytes_per_second > 0.0;
        }));
    EXPECT_TRUE(std::any_of(captured.begin(), captured.end(),
        [](const asyncdownload::ProgressSnapshot& snapshot) {
            return snapshot.disk_bytes_per_second > 0.0;
        }));
    EXPECT_TRUE(std::any_of(captured.begin(), captured.end(),
        [](const asyncdownload::ProgressSnapshot& snapshot) {
            return snapshot.vdl_offset > 0;
        }));
    EXPECT_TRUE(std::all_of(captured.begin(), captured.end(),
        [](const asyncdownload::ProgressSnapshot& snapshot) {
            return snapshot.inflight_bytes >= 0;
        }));

    const auto final_removed = std::filesystem::remove_all(temp_root, ec);
    static_cast<void>(final_removed);
#endif
}

TEST(DownloadIntegrationTest, PreservesResumeArtifactsAfterServerFailure) {
#ifndef _WIN32
    GTEST_SKIP() << "This integration test currently uses Windows process control.";
#else
    const auto workspace_root = get_workspace_root();
    const auto temp_root =
        std::filesystem::temp_directory_path() / "asyncdownload_server_failure_integration";
    std::error_code ec;
    const auto removed = std::filesystem::remove_all(temp_root, ec);
    static_cast<void>(removed);
    std::filesystem::create_directories(temp_root, ec);
    ASSERT_FALSE(ec);

    const auto source_file = temp_root / "source.bin";
    const auto output_file = temp_root / "interrupted.bin";
    const auto part_file = std::filesystem::path(output_file.string() + ".part");
    const auto metadata_file = std::filesystem::path(output_file.string() + ".config.json");
    write_test_file(source_file, 128 * 1024 * 1024);

    ChildProcess server;
    const auto script_path = workspace_root / "tests" / "support" / "range_server.py";
    const auto server_command = quote_arg(L"python") + L" " +
        quote_arg(script_path.wstring()) + L" " +
        quote_arg(source_file.wstring()) + L" --port 0 --chunk-size 65536 --delay-ms 20";
    ASSERT_TRUE(start_process(server, L"", server_command, workspace_root, true));

    const auto port_line = read_line_from_pipe(server.stdout_read, 5000);
    ASSERT_FALSE(port_line.empty());

    asyncdownload::DownloadRequest request{};
    request.url = std::string("http://127.0.0.1:") + port_line + "/source.bin";
    request.output_path = output_file;
    request.options.max_connections = 4;

    auto future = std::async(std::launch::async, [request]() mutable {
        asyncdownload::DownloadClient client;
        return client.download(request);
    });

    ASSERT_TRUE(wait_for_path(metadata_file, std::chrono::milliseconds(8000)));
    stop_child(server, 77);

    ASSERT_EQ(future.wait_for(std::chrono::seconds(20)), std::future_status::ready);
    const auto result = future.get();

    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(std::filesystem::exists(part_file));
    EXPECT_TRUE(std::filesystem::exists(metadata_file));
    EXPECT_FALSE(std::filesystem::exists(output_file));

    const auto final_removed = std::filesystem::remove_all(temp_root, ec);
    static_cast<void>(final_removed);
#endif
}

} // namespace

