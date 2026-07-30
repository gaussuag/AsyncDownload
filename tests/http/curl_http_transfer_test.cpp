#include "http/http_transfer.hpp"

#include "asyncdownload/error.hpp"
#include "asyncdownload/telemetry/telemetry_session.hpp"
#include "flow/packet_flow.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <thread>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace {

#ifdef _WIN32

std::wstring quote_process_arg(
    const std::wstring& value) {
    return L"\"" + value + L"\"";
}

struct PythonServer {
    PROCESS_INFORMATION process{};
    HANDLE stdout_read = nullptr;

    ~PythonServer() {
        stop();
    }

    void stop() noexcept {
        if (process.hProcess != nullptr) {
            if (WaitForSingleObject(
                    process.hProcess,
                    0) == WAIT_TIMEOUT) {
                TerminateProcess(process.hProcess, 0);
                static_cast<void>(
                    WaitForSingleObject(
                        process.hProcess,
                        5000));
            }
            CloseHandle(process.hProcess);
            process.hProcess = nullptr;
        }
        if (process.hThread != nullptr) {
            CloseHandle(process.hThread);
            process.hThread = nullptr;
        }
        if (stdout_read != nullptr) {
            CloseHandle(stdout_read);
            stdout_read = nullptr;
        }
    }
};

std::filesystem::path test_workspace_root() {
    std::array<wchar_t, 4096> buffer{};
    const auto length = GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size()));
    return std::filesystem::path(
        std::wstring(buffer.data(), length))
        .parent_path()
        .parent_path()
        .parent_path()
        .parent_path();
}

bool start_python_server(
    PythonServer& server,
    const std::filesystem::path& source,
    const std::filesystem::path& request_log,
    const std::wstring& extra_arguments = L"") {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE stdout_write = nullptr;
    if (!CreatePipe(
            &server.stdout_read,
            &stdout_write,
            &attributes,
            0)) {
        return false;
    }
    SetHandleInformation(
        server.stdout_read,
        HANDLE_FLAG_INHERIT,
        0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = stdout_write;
    startup.hStdError = stdout_write;
    startup.hStdInput = GetStdHandle(
        STD_INPUT_HANDLE);

    const auto root = test_workspace_root();
    const auto script =
        root / "tests" / "support" /
        "range_server.py";
    auto command =
        quote_process_arg(L"python") + L" " +
        quote_process_arg(script.wstring()) + L" " +
        quote_process_arg(source.wstring()) +
        L" --port 0 --request-log " +
        quote_process_arg(request_log.wstring()) +
        extra_arguments;
    const auto started = CreateProcessW(
        nullptr,
        command.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        root.c_str(),
        &startup,
        &server.process);
    CloseHandle(stdout_write);
    return started == TRUE;
}

std::string read_server_port(
    const PythonServer& server) {
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    std::string value;
    while (std::chrono::steady_clock::now() <
           deadline) {
        DWORD available = 0;
        if (PeekNamedPipe(
                server.stdout_read,
                nullptr,
                0,
                nullptr,
                &available,
                nullptr) &&
            available != 0) {
            char character = 0;
            DWORD read = 0;
            if (!ReadFile(
                    server.stdout_read,
                    &character,
                    1,
                    &read,
                    nullptr) ||
                read == 0) {
                break;
            }
            if (character == '\n') {
                break;
            }
            if (character != '\r') {
                value.push_back(character);
            }
        } else {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(10));
        }
    }
    return value;
}

#endif

class CurlHttpTransferTest : public ::testing::Test {
protected:
    void SetUp() override {
        telemetry_.record_task_started();
        const asyncdownload::download::FlowControlPolicy policy{
            8,
            1024 * 1024,
            512 * 1024
        };
        ASSERT_FALSE(
            asyncdownload::flow::PacketFlow::create(
                policy,
                telemetry_,
                flow_));
        ASSERT_FALSE(
            asyncdownload::http::
                create_curl_http_transfer_port(port_));
        ASSERT_NE(port_, nullptr);
    }

    std::unique_ptr<
        asyncdownload::http::HttpTransferSession>
    open_session(
        const std::size_t capacity = 3) {
        return open_session(
            "http://127.0.0.1:1/object",
            1024,
            capacity);
    }

    std::unique_ptr<
        asyncdownload::http::HttpTransferSession>
    open_session(
        const std::string& url,
        const std::int64_t total_size,
        const std::size_t capacity) {
        auto opened = port_->open_session(
            {url, total_size, capacity},
            flow_->producer(),
            telemetry_);
        EXPECT_FALSE(opened.failure.error);
        EXPECT_NE(opened.session, nullptr);
        return std::move(opened.session);
    }

    asyncdownload::range::RangeLease lease(
        const std::uint64_t range = 1,
        const std::uint64_t generation = 1) {
        return {
            {{range}, generation},
            {0, 1024},
            true
        };
    }

    asyncdownload::http::HttpPollResult
    poll_until_event(
        asyncdownload::http::HttpTransferSession& session,
        const std::chrono::milliseconds timeout) {
        const auto deadline =
            std::chrono::steady_clock::now() +
            timeout;
        asyncdownload::http::HttpPollResult result{};
        while (std::chrono::steady_clock::now() <
               deadline) {
            result = session.poll(
                std::chrono::milliseconds(50));
            if (result.code ==
                asyncdownload::http::
                    HttpPollCode::event ||
                result.code ==
                asyncdownload::http::
                    HttpPollCode::failed ||
                result.code ==
                asyncdownload::http::
                    HttpPollCode::closed) {
                return result;
            }
        }
        return result;
    }

    asyncdownload::telemetry::TelemetrySession
        telemetry_;
    std::unique_ptr<asyncdownload::flow::PacketFlow>
        flow_;
    std::unique_ptr<
        asyncdownload::http::HttpTransferPort>
        port_;
};

TEST_F(CurlHttpTransferTest, CreatesFixedStableSlots) {
    auto session = open_session(3);
    const auto snapshot = session->snapshot();
    EXPECT_EQ(
        snapshot.state,
        asyncdownload::http::HttpSessionState::open);
    EXPECT_EQ(snapshot.active_transfers, 0U);
    EXPECT_EQ(snapshot.available_slots, 3U);
    EXPECT_EQ(snapshot.pending_events, 0U);
    EXPECT_FALSE(session->close());
}

TEST_F(
    CurlHttpTransferTest,
    RejectsWrongThreadWithoutCurlSideEffect) {
    auto session = open_session(2);
    auto result = std::async(
        std::launch::async,
        [&session, this]() {
            return session->start(lease());
        });

    const auto wrong_thread = result.get();
    EXPECT_EQ(
        wrong_thread.code,
        asyncdownload::http::HttpStartCode::failed);
    EXPECT_EQ(
        wrong_thread.failure.reason,
        asyncdownload::http::
            HttpFailureReason::protocol_order_invalid);
    const auto snapshot = session->snapshot();
    EXPECT_EQ(snapshot.active_transfers, 0U);
    EXPECT_EQ(snapshot.available_slots, 2U);
    EXPECT_FALSE(session->close());
}

TEST_F(
    CurlHttpTransferTest,
    PendingEventMakesAvailableSlotsZero) {
    auto session = open_session(2);
    const auto started =
        session->start(lease());
    ASSERT_EQ(
        started.code,
        asyncdownload::http::HttpStartCode::started);
    ASSERT_FALSE(
        session->cancel(
            {
                asyncdownload::http::
                    HttpCancelKind::task_cancelled,
                {}
            }));

    const auto pending = session->snapshot();
    EXPECT_EQ(pending.active_transfers, 0U);
    EXPECT_EQ(pending.pending_events, 1U);
    EXPECT_EQ(pending.available_slots, 0U);
    EXPECT_EQ(
        session->start(lease(2, 1)).code,
        asyncdownload::http::
            HttpStartCode::no_capacity);
    EXPECT_TRUE(session->close());

    const auto event =
        session->poll(std::chrono::milliseconds(0));
    EXPECT_EQ(
        event.code,
        asyncdownload::http::HttpPollCode::event);
    EXPECT_FALSE(session->close());
}

TEST_F(
    CurlHttpTransferTest,
    StartFailureReturnsSynchronouslyWithoutPendingEvent) {
    auto session = open_session(2);
    auto invalid = lease();
    invalid.bytes.end = invalid.bytes.begin;

    const auto start = session->start(invalid);
    EXPECT_EQ(
        start.code,
        asyncdownload::http::HttpStartCode::failed);
    EXPECT_FALSE(start.token.has_value());
    EXPECT_EQ(
        start.failure.reason,
        asyncdownload::http::
            HttpFailureReason::protocol_order_invalid);
    const auto snapshot = session->snapshot();
    EXPECT_EQ(snapshot.active_transfers, 0U);
    EXPECT_EQ(snapshot.pending_events, 0U);
    EXPECT_EQ(snapshot.available_slots, 2U);
    EXPECT_FALSE(session->close());
}

TEST_F(
    CurlHttpTransferTest,
    IncrementsSlotGenerationOnReuse) {
#ifndef _WIN32
    GTEST_SKIP() << "This test currently uses Windows process control.";
#else
    const auto root =
        std::filesystem::temp_directory_path() /
        ("asyncdownload_curl_session_" +
         std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    ASSERT_FALSE(ec);
    const auto source = root / "source.bin";
    const auto request_log = root / "requests.log";
    {
        std::ofstream stream(
            source,
            std::ios::binary |
                std::ios::trunc);
        ASSERT_TRUE(stream.is_open());
        const std::array<char, 4> bytes{
            1,
            2,
            3,
            4
        };
        stream.write(
            bytes.data(),
            static_cast<std::streamsize>(
                bytes.size()));
    }

    PythonServer server;
    ASSERT_TRUE(start_python_server(
        server,
        source,
        request_log));
    const auto port = read_server_port(server);
    ASSERT_FALSE(port.empty());
    auto session = open_session(
        "http://127.0.0.1:" + port +
            "/source.bin",
        4,
        1);
    const asyncdownload::range::RangeLease first{
        {{1}, 1},
        {0, 4},
        true
    };
    const auto first_start =
        session->start(first);
    ASSERT_EQ(
        first_start.code,
        asyncdownload::http::
            HttpStartCode::started);
    ASSERT_TRUE(first_start.token.has_value());

    auto first_event =
        session->poll(
            std::chrono::milliseconds(100));
    while (first_event.code ==
           asyncdownload::http::
               HttpPollCode::timed_out) {
        first_event = session->poll(
            std::chrono::milliseconds(100));
    }
    ASSERT_EQ(
        first_event.code,
        asyncdownload::http::HttpPollCode::event);
    ASSERT_TRUE(first_event.event.has_value());
    const auto* first_success =
        std::get_if<
            asyncdownload::http::
                HttpLeaseSucceeded>(
            &*first_event.event);
    ASSERT_NE(first_success, nullptr);
    EXPECT_EQ(first_success->lease, first.id);
    EXPECT_EQ(first_success->received_through, 4);

    asyncdownload::flow::PacketLease packet;
    ASSERT_EQ(
        flow_->consumer().receive(
            packet,
            std::chrono::milliseconds(1)).code,
        asyncdownload::flow::
            PacketReceiveCode::packet);
    packet.complete();

    auto second = first;
    second.id.generation = 2;
    const auto second_start =
        session->start(second);
    ASSERT_EQ(
        second_start.code,
        asyncdownload::http::
            HttpStartCode::started);
    ASSERT_TRUE(second_start.token.has_value());
    EXPECT_EQ(
        second_start.token->slot,
        first_start.token->slot);
    EXPECT_GT(
        second_start.token->slot_generation,
        first_start.token->slot_generation);

    auto second_event =
        session->poll(
            std::chrono::milliseconds(100));
    while (second_event.code ==
           asyncdownload::http::
               HttpPollCode::timed_out) {
        second_event = session->poll(
            std::chrono::milliseconds(100));
    }
    ASSERT_EQ(
        second_event.code,
        asyncdownload::http::HttpPollCode::event);
    ASSERT_TRUE(second_event.event.has_value());
    const auto* second_success =
        std::get_if<
            asyncdownload::http::
                HttpLeaseSucceeded>(
            &*second_event.event);
    ASSERT_NE(second_success, nullptr);
    EXPECT_EQ(second_success->lease, second.id);
    ASSERT_EQ(
        flow_->consumer().receive(
            packet,
            std::chrono::milliseconds(1)).code,
        asyncdownload::flow::
            PacketReceiveCode::packet);
    packet.complete();
    EXPECT_FALSE(session->close());
    server.stop();

    std::ifstream requests(request_log);
    std::size_t get_count = 0;
    std::string line;
    while (std::getline(requests, line)) {
        if (line.find("\tGET\t") !=
            std::string::npos) {
            ++get_count;
        }
    }
    EXPECT_EQ(get_count, 2U);
    const auto removed =
        std::filesystem::remove_all(root, ec);
    static_cast<void>(removed);
#endif
}

TEST_F(
    CurlHttpTransferTest,
    ClassifiesCleanShortBodyAsProtocolFailure) {
#ifndef _WIN32
    GTEST_SKIP() << "This test currently uses Windows process control.";
#else
    const auto root =
        std::filesystem::temp_directory_path() /
        ("asyncdownload_curl_short_" +
         std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    ASSERT_FALSE(ec);
    const auto source = root / "source.bin";
    const auto request_log = root / "requests.log";
    {
        std::ofstream stream(
            source,
            std::ios::binary |
                std::ios::trunc);
        ASSERT_TRUE(stream.is_open());
        const std::array<char, 5> bytes{
            1,
            2,
            3,
            4,
            5
        };
        stream.write(
            bytes.data(),
            static_cast<std::streamsize>(
                bytes.size()));
    }

    PythonServer server;
    ASSERT_TRUE(start_python_server(
        server,
        source,
        request_log,
        L" --omit-get-content-length"
        L" --body-length-delta -1"));
    const auto server_port =
        read_server_port(server);
    ASSERT_FALSE(server_port.empty());
    auto session = open_session(
        "http://127.0.0.1:" + server_port +
            "/source.bin",
        5,
        1);
    const asyncdownload::range::RangeLease
        requested{
            {{1}, 1},
            {0, 4},
            true
        };
    ASSERT_EQ(
        session->start(requested).code,
        asyncdownload::http::
            HttpStartCode::started);

    const auto result =
        poll_until_event(
            *session,
            std::chrono::seconds(2));
    ASSERT_EQ(
        result.code,
        asyncdownload::http::
            HttpPollCode::event);
    ASSERT_TRUE(result.event.has_value());
    const auto* failure =
        std::get_if<
            asyncdownload::http::
                HttpLeaseFailed>(
            &*result.event);
    ASSERT_NE(failure, nullptr);
    EXPECT_EQ(
        failure->failure.reason,
        asyncdownload::http::
            HttpFailureReason::body_too_short);
    EXPECT_EQ(
        failure->failure.error,
        asyncdownload::make_error_code(
            asyncdownload::DownloadErrc::
                http_invalid_response));
    EXPECT_EQ(failure->accepted_through, 3);
    EXPECT_TRUE(session->close());
    server.stop();

    const auto removed =
        std::filesystem::remove_all(root, ec);
    static_cast<void>(removed);
#endif
}

TEST_F(
    CurlHttpTransferTest,
    TerminatesLongBodyAsProtocolFailure) {
#ifndef _WIN32
    GTEST_SKIP() << "This test currently uses Windows process control.";
#else
    const auto root =
        std::filesystem::temp_directory_path() /
        ("asyncdownload_curl_long_" +
         std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    ASSERT_FALSE(ec);
    const auto source = root / "source.bin";
    const auto request_log = root / "requests.log";
    {
        std::ofstream stream(
            source,
            std::ios::binary |
                std::ios::trunc);
        ASSERT_TRUE(stream.is_open());
        const std::array<char, 5> bytes{
            1,
            2,
            3,
            4,
            5
        };
        stream.write(
            bytes.data(),
            static_cast<std::streamsize>(
                bytes.size()));
    }

    PythonServer server;
    ASSERT_TRUE(start_python_server(
        server,
        source,
        request_log,
        L" --chunk-size 4"
        L" --omit-get-content-length"
        L" --body-length-delta 1"));
    const auto server_port =
        read_server_port(server);
    ASSERT_FALSE(server_port.empty());
    auto session = open_session(
        "http://127.0.0.1:" + server_port +
            "/source.bin",
        5,
        1);
    const asyncdownload::range::RangeLease
        requested{
            {{1}, 1},
            {0, 4},
            true
        };
    ASSERT_EQ(
        session->start(requested).code,
        asyncdownload::http::
            HttpStartCode::started);

    const auto result =
        poll_until_event(
            *session,
            std::chrono::seconds(1));
    EXPECT_EQ(
        result.code,
        asyncdownload::http::
            HttpPollCode::event);
    if (result.code ==
            asyncdownload::http::
                HttpPollCode::event &&
        result.event.has_value()) {
        const auto* failure =
            std::get_if<
                asyncdownload::http::
                    HttpLeaseFailed>(
                &*result.event);
        ASSERT_NE(failure, nullptr);
        EXPECT_EQ(
            failure->failure.reason,
            asyncdownload::http::
                HttpFailureReason::body_too_long);
        EXPECT_EQ(
            failure->failure.error,
            asyncdownload::make_error_code(
                asyncdownload::DownloadErrc::
                    http_invalid_response));
        EXPECT_EQ(failure->accepted_through, 4);
    } else {
        static_cast<void>(
            session->cancel({
                asyncdownload::http::
                    HttpCancelKind::task_cancelled,
                {}
            }));
        static_cast<void>(
            session->poll(
                std::chrono::milliseconds(0)));
    }
    EXPECT_TRUE(session->close());
    server.stop();

    const auto removed =
        std::filesystem::remove_all(root, ec);
    static_cast<void>(removed);
#endif
}

TEST_F(
    CurlHttpTransferTest,
    PreservesTransportFailureForTruncatedConnection) {
#ifndef _WIN32
    GTEST_SKIP() << "This test currently uses Windows process control.";
#else
    const auto root =
        std::filesystem::temp_directory_path() /
        ("asyncdownload_curl_transport_" +
         std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    ASSERT_FALSE(ec);
    const auto source = root / "source.bin";
    const auto request_log = root / "requests.log";
    {
        std::ofstream stream(
            source,
            std::ios::binary |
                std::ios::trunc);
        ASSERT_TRUE(stream.is_open());
        const std::array<char, 5> bytes{
            1,
            2,
            3,
            4,
            5
        };
        stream.write(
            bytes.data(),
            static_cast<std::streamsize>(
                bytes.size()));
    }

    PythonServer server;
    ASSERT_TRUE(start_python_server(
        server,
        source,
        request_log,
        L" --chunk-size 2"
        L" --close-after-bytes 2"));
    const auto server_port =
        read_server_port(server);
    ASSERT_FALSE(server_port.empty());
    auto session = open_session(
        "http://127.0.0.1:" + server_port +
            "/source.bin",
        5,
        1);
    const asyncdownload::range::RangeLease
        requested{
            {{1}, 1},
            {0, 4},
            true
        };
    ASSERT_EQ(
        session->start(requested).code,
        asyncdownload::http::
            HttpStartCode::started);

    const auto result =
        poll_until_event(
            *session,
            std::chrono::seconds(2));
    ASSERT_EQ(
        result.code,
        asyncdownload::http::
            HttpPollCode::event);
    ASSERT_TRUE(result.event.has_value());
    const auto* failure =
        std::get_if<
            asyncdownload::http::
                HttpLeaseFailed>(
            &*result.event);
    ASSERT_NE(failure, nullptr);
    EXPECT_EQ(
        failure->failure.reason,
        asyncdownload::http::
            HttpFailureReason::transport_failed);
    EXPECT_EQ(
        failure->failure.error,
        asyncdownload::make_error_code(
            asyncdownload::DownloadErrc::
                http_transfer_failed));
    EXPECT_EQ(failure->accepted_through, 2);
    EXPECT_TRUE(session->close());
    server.stop();

    const auto removed =
        std::filesystem::remove_all(root, ec);
    static_cast<void>(removed);
#endif
}

TEST_F(
    CurlHttpTransferTest,
    PreservesCallbackCauseOverWriteError) {
#ifndef _WIN32
    GTEST_SKIP() << "This test currently uses Windows process control.";
#else
    const auto root =
        std::filesystem::temp_directory_path() /
        ("asyncdownload_curl_callback_cause_" +
         std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    ASSERT_FALSE(ec);
    const auto source = root / "source.bin";
    const auto request_log = root / "requests.log";
    {
        std::ofstream stream(
            source,
            std::ios::binary |
                std::ios::trunc);
        ASSERT_TRUE(stream.is_open());
        const std::array<char, 5> bytes{
            1,
            2,
            3,
            4,
            5
        };
        stream.write(
            bytes.data(),
            static_cast<std::streamsize>(
                bytes.size()));
    }

    PythonServer server;
    ASSERT_TRUE(start_python_server(
        server,
        source,
        request_log,
        L" --force-get-content-encoding gzip"));
    const auto server_port =
        read_server_port(server);
    ASSERT_FALSE(server_port.empty());
    auto session = open_session(
        "http://127.0.0.1:" + server_port +
            "/source.bin",
        5,
        1);
    const asyncdownload::range::RangeLease
        requested{
            {{1}, 1},
            {0, 4},
            true
        };
    ASSERT_EQ(
        session->start(requested).code,
        asyncdownload::http::
            HttpStartCode::started);

    const auto result =
        poll_until_event(
            *session,
            std::chrono::seconds(2));
    ASSERT_EQ(
        result.code,
        asyncdownload::http::
            HttpPollCode::event);
    ASSERT_TRUE(result.event.has_value());
    const auto* failure =
        std::get_if<
            asyncdownload::http::
                HttpLeaseFailed>(
            &*result.event);
    ASSERT_NE(failure, nullptr);
    EXPECT_EQ(
        failure->failure.reason,
        asyncdownload::http::
            HttpFailureReason::
                content_encoding_invalid);
    EXPECT_EQ(
        failure->failure.error,
        asyncdownload::make_error_code(
            asyncdownload::DownloadErrc::
                http_invalid_response));
    EXPECT_EQ(failure->accepted_through, 0);
    EXPECT_TRUE(session->close());
    server.stop();

    const auto removed =
        std::filesystem::remove_all(root, ec);
    static_cast<void>(removed);
#endif
}

}
