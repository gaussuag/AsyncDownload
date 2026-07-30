#include "http_transfer.hpp"

#include "asyncdownload/error.hpp"
#include "http_response_accumulator.hpp"

#include <curl/curl.h>

#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace asyncdownload::http {
namespace {

HttpFailure make_failure(
    const HttpFailureReason reason,
    const DownloadErrc error) noexcept {
    return {reason, make_error_code(error)};
}

std::optional<std::size_t> callback_bytes(
    const std::size_t size,
    const std::size_t count) noexcept {
    if (size != 0 &&
        count >
            std::numeric_limits<std::size_t>::max() /
                size) {
        return std::nullopt;
    }
    return size * count;
}

std::size_t discard_probe_body(
    char* data,
    const std::size_t size,
    const std::size_t count,
    void* user_data) noexcept {
    static_cast<void>(data);
    const auto bytes = callback_bytes(size, count);
    if (!bytes.has_value()) {
        return CURL_WRITEFUNC_ERROR;
    }
    auto* response =
        static_cast<HttpResponseAccumulator*>(user_data);
    response->begin_body();
    return *bytes;
}

std::size_t capture_probe_header(
    char* data,
    const std::size_t size,
    const std::size_t count,
    void* user_data) noexcept {
    const auto bytes = callback_bytes(size, count);
    if (!bytes.has_value()) {
        return CURL_WRITEFUNC_ERROR;
    }
    auto* response =
        static_cast<HttpResponseAccumulator*>(user_data);
    return response->append(
        std::string_view(data, *bytes))
        ? *bytes
        : CURL_WRITEFUNC_ERROR;
}

void configure_probe_common(
    CURL* easy,
    const std::string& url,
    HttpResponseAccumulator& response) noexcept {
    static_cast<void>(
        curl_easy_setopt(
            easy,
            CURLOPT_URL,
            url.c_str()));
    static_cast<void>(
        curl_easy_setopt(
            easy,
            CURLOPT_FOLLOWLOCATION,
            1L));
    static_cast<void>(
        curl_easy_setopt(
            easy,
            CURLOPT_NOSIGNAL,
            1L));
    static_cast<void>(
        curl_easy_setopt(
            easy,
            CURLOPT_HTTP_VERSION,
            CURL_HTTP_VERSION_1_1));
    static_cast<void>(
        curl_easy_setopt(
            easy,
            CURLOPT_WRITEFUNCTION,
            discard_probe_body));
    static_cast<void>(
        curl_easy_setopt(
            easy,
            CURLOPT_WRITEDATA,
            &response));
    static_cast<void>(
        curl_easy_setopt(
            easy,
            CURLOPT_HEADERFUNCTION,
            capture_probe_header));
    static_cast<void>(
        curl_easy_setopt(
            easy,
            CURLOPT_HEADERDATA,
            &response));
}

HttpProbeResult failed_probe(
    const HttpFailure failure,
    const long response_code = 0) noexcept {
    return {
        std::nullopt,
        failure,
        response_code
    };
}

HttpProbeResult fallback_probe(
    const HttpProbeRequest& request) noexcept {
    CURL* easy = curl_easy_init();
    if (easy == nullptr) {
        return failed_probe(
            make_failure(
                HttpFailureReason::easy_init_failed,
                DownloadErrc::http_init_failed));
    }

    HttpResponseAccumulator response;
    configure_probe_common(
        easy,
        request.url,
        response);
    static_cast<void>(
        curl_easy_setopt(
            easy,
            CURLOPT_HTTPGET,
            1L));
    static_cast<void>(
        curl_easy_setopt(
            easy,
            CURLOPT_RANGE,
            "0-0"));

    const auto curl_result =
        curl_easy_perform(easy);
    long response_code = 0;
    static_cast<void>(
        curl_easy_getinfo(
            easy,
            CURLINFO_RESPONSE_CODE,
            &response_code));
    curl_off_t response_length = -1;
    static_cast<void>(
        curl_easy_getinfo(
            easy,
            CURLINFO_CONTENT_LENGTH_DOWNLOAD_T,
            &response_length));
    curl_easy_cleanup(easy);

    if (curl_result != CURLE_OK ||
        response.parser_failed()) {
        return failed_probe(
            make_failure(
                response.parser_failed()
                    ? HttpFailureReason::
                        header_callback_failed
                    : HttpFailureReason::
                        transport_failed,
                DownloadErrc::http_probe_failed),
            response_code);
    }

    const auto total =
        response.content_range().has_value()
        ? response.content_range()->total
        : response_length > 0
            ? static_cast<std::int64_t>(
                response_length)
            : 0;
    if (total <= 0) {
        return failed_probe(
            make_failure(
                HttpFailureReason::
                    content_length_malformed,
                DownloadErrc::http_probe_failed),
            response_code);
    }

    return {
        HttpObjectFacts{
            total,
            response.accept_ranges() ||
                response.content_range().has_value(),
            response.etag(),
            response.last_modified()
        },
        {},
        response_code
    };
}

class CurlHttpTransferPort final :
    public HttpTransferPort {
public:
    HttpProbeResult probe(
        const HttpProbeRequest& request) noexcept override {
        if (request.url.empty()) {
            return failed_probe(
                make_failure(
                    HttpFailureReason::
                        protocol_order_invalid,
                    DownloadErrc::http_probe_failed));
        }

        CURL* easy = curl_easy_init();
        if (easy == nullptr) {
            return failed_probe(
                make_failure(
                    HttpFailureReason::easy_init_failed,
                    DownloadErrc::http_init_failed));
        }

        HttpResponseAccumulator response;
        configure_probe_common(
            easy,
            request.url,
            response);
        static_cast<void>(
            curl_easy_setopt(
                easy,
                CURLOPT_NOBODY,
                1L));
        static_cast<void>(
            curl_easy_setopt(
                easy,
                CURLOPT_RANGE,
                nullptr));

        const auto curl_result =
            curl_easy_perform(easy);
        long response_code = 0;
        static_cast<void>(
            curl_easy_getinfo(
                easy,
                CURLINFO_RESPONSE_CODE,
                &response_code));
        curl_off_t response_length = -1;
        static_cast<void>(
            curl_easy_getinfo(
                easy,
                CURLINFO_CONTENT_LENGTH_DOWNLOAD_T,
                &response_length));
        curl_easy_cleanup(easy);

        if (curl_result != CURLE_OK) {
            return fallback_probe(request);
        }
        if (response.parser_failed()) {
            return failed_probe(
                make_failure(
                    HttpFailureReason::
                        header_callback_failed,
                    DownloadErrc::http_probe_failed),
                response_code);
        }
        if (response_code >= 400 ||
            response_length <= 0) {
            return fallback_probe(request);
        }

        return {
            HttpObjectFacts{
                static_cast<std::int64_t>(
                    response_length),
                response.accept_ranges(),
                response.etag(),
                response.last_modified()
            },
            {},
            response_code
        };
    }

    HttpSessionOpenResult open_session(
        const HttpSessionConfig& config,
        flow::PacketProducer& packet_producer,
        telemetry::TelemetrySession& telemetry)
            noexcept override {
        static_cast<void>(config);
        static_cast<void>(packet_producer);
        static_cast<void>(telemetry);
        return {
            nullptr,
            make_failure(
                HttpFailureReason::multi_init_failed,
                DownloadErrc::http_init_failed)
        };
    }
};

CURLcode curl_runtime_result() noexcept {
    static const auto result =
        curl_global_init(CURL_GLOBAL_DEFAULT);
    return result;
}

}

std::error_code create_curl_http_transfer_port(
    std::unique_ptr<HttpTransferPort>& result) noexcept {
    result.reset();
    if (curl_runtime_result() != CURLE_OK) {
        return make_error_code(
            DownloadErrc::http_init_failed);
    }
    try {
        result =
            std::make_unique<CurlHttpTransferPort>();
        return {};
    } catch (const std::bad_alloc&) {
        return std::make_error_code(
            std::errc::not_enough_memory);
    } catch (...) {
        return make_error_code(
            DownloadErrc::internal_error);
    }
}

}
