#include "http_transfer.hpp"

#include "asyncdownload/error.hpp"
#include "asyncdownload/telemetry/telemetry_event.hpp"
#include "asyncdownload/telemetry/telemetry_session.hpp"
#include "flow/packet_flow.hpp"
#include "curl_runtime_adapter.hpp"
#include "http_response_accumulator.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

bool configure_probe_common(
    CURL* easy,
    const std::string& url,
    HttpResponseAccumulator& response,
    curl_slist* identity_headers) noexcept {
    return detail::easy_setopt(
               easy,
               CURLOPT_URL,
               url.c_str()) == CURLE_OK &&
        detail::easy_setopt(
            easy,
            CURLOPT_FOLLOWLOCATION,
            1L) == CURLE_OK &&
        detail::easy_setopt(
            easy,
            CURLOPT_NOSIGNAL,
            1L) == CURLE_OK &&
        detail::easy_setopt(
            easy,
            CURLOPT_HTTP_VERSION,
            CURL_HTTP_VERSION_1_1) == CURLE_OK &&
        detail::easy_setopt(
            easy,
            CURLOPT_WRITEFUNCTION,
            discard_probe_body) == CURLE_OK &&
        detail::easy_setopt(
            easy,
            CURLOPT_WRITEDATA,
            &response) == CURLE_OK &&
        detail::easy_setopt(
            easy,
            CURLOPT_HEADERFUNCTION,
            capture_probe_header) == CURLE_OK &&
        detail::easy_setopt(
            easy,
            CURLOPT_HEADERDATA,
            &response) == CURLE_OK &&
        detail::easy_setopt(
            easy,
            CURLOPT_ACCEPT_ENCODING,
            static_cast<const char*>(
                nullptr)) == CURLE_OK &&
        detail::easy_setopt(
            easy,
            CURLOPT_HTTP_CONTENT_DECODING,
            0L) == CURLE_OK &&
        detail::easy_setopt(
            easy,
            CURLOPT_HTTPHEADER,
            identity_headers) == CURLE_OK;
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
    const HttpProbeRequest& request,
    curl_slist* identity_headers) noexcept {
    CURL* easy = detail::easy_init();
    if (easy == nullptr) {
        return failed_probe(
            make_failure(
                HttpFailureReason::easy_init_failed,
                DownloadErrc::http_init_failed));
    }

    HttpResponseAccumulator response;
    if (!configure_probe_common(
            easy,
            request.url,
            response,
            identity_headers) ||
        detail::easy_setopt(
            easy,
            CURLOPT_HTTPGET,
            1L) != CURLE_OK ||
        detail::easy_setopt(
            easy,
            CURLOPT_RANGE,
            "0-0") != CURLE_OK) {
        detail::easy_cleanup(easy);
        return failed_probe(
            make_failure(
                HttpFailureReason::
                    easy_option_failed,
                DownloadErrc::http_probe_failed));
    }

    const auto curl_result =
        detail::easy_perform(easy);
    long response_code = 0;
    const auto response_info =
        detail::easy_getinfo(
            easy,
            CURLINFO_RESPONSE_CODE,
            &response_code);
    curl_off_t response_length = -1;
    const auto length_info =
        detail::easy_getinfo(
            easy,
            CURLINFO_CONTENT_LENGTH_DOWNLOAD_T,
            &response_length);
    detail::easy_cleanup(easy);

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
    if (response_info != CURLE_OK ||
        length_info != CURLE_OK) {
        return failed_probe(
            make_failure(
                HttpFailureReason::
                    easy_info_failed,
                DownloadErrc::http_probe_failed),
            response_code);
    }
    if (!response.content_encoding_is_identity()) {
        return failed_probe(
            make_failure(
                HttpFailureReason::
                    content_encoding_invalid,
                DownloadErrc::http_probe_failed),
            response_code);
    }

    std::int64_t total = 0;
    bool accept_ranges = false;
    if (response_code == 206) {
        if (response.content_range_invalid() ||
            !response.content_range().has_value() ||
            response.content_range()->first != 0 ||
            response.content_range()->last != 0 ||
            response.content_range()->total <= 0 ||
            response.content_length_invalid() ||
            (response.content_length().has_value() &&
             *response.content_length() != 1)) {
            return failed_probe(
                make_failure(
                    HttpFailureReason::
                        content_range_mismatch,
                    DownloadErrc::http_probe_failed),
                response_code);
        }
        total = response.content_range()->total;
        accept_ranges = true;
    } else if (response_code == 200) {
        if (response.content_range().has_value() ||
            response.content_range_invalid() ||
            response.content_length_invalid() ||
            !response.content_length().has_value() ||
            *response.content_length() <= 0) {
            return failed_probe(
                make_failure(
                    HttpFailureReason::
                        content_length_malformed,
                    DownloadErrc::http_probe_failed),
                response_code);
        }
        total = *response.content_length();
    } else {
        return failed_probe(
            make_failure(
                HttpFailureReason::
                    response_status_invalid,
                DownloadErrc::http_probe_failed),
            response_code);
    }
    if (total <= 0 ||
        (response_length > 0 &&
         response_code == 206 &&
         response_length != 1)) {
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
            accept_ranges,
            response.etag(),
            response.last_modified()
        },
        {},
        response_code
    };
}

class CurlHttpTransferSession final :
    public HttpTransferSession {
public:
    struct Slot {
        CurlHttpTransferSession* owner = nullptr;
        CURL* easy = nullptr;
        flow::ProducerLane lane;
        TransferSlotId id = 0;
        std::uint64_t generation = 0;
        std::optional<range::RangeLease> lease;
        std::optional<HttpTransferEvent> pending_event;
        HttpResponseAccumulator response;
        range::ByteOffset accepted_through = 0;
        std::uint64_t body_bytes = 0;
        double bytes_per_second = 0.0;
        std::uint8_t packet_pause_mask = 0;
        bool gap_paused = false;
        bool curl_receive_paused = false;
        bool in_multi = false;
        bool callback_active = false;
        bool cancelling = false;
        bool first_byte_recorded = false;
        HttpFailure callback_failure{};
        std::string range_header;
    };

    static HttpSessionOpenResult create(
        const HttpSessionConfig& config,
        flow::PacketProducer& packet_producer,
        telemetry::TelemetrySession& telemetry) noexcept {
        if (config.url.empty() ||
            config.total_size <= 0 ||
            config.max_active_transfers == 0) {
            return {
                nullptr,
                make_failure(
                    HttpFailureReason::
                        protocol_order_invalid,
                    DownloadErrc::internal_error)
            };
        }

        try {
            auto session =
                std::unique_ptr<CurlHttpTransferSession>(
                    new CurlHttpTransferSession(
                        config,
                        packet_producer,
                        telemetry));
            session->multi_ =
                detail::multi_init();
            if (session->multi_ == nullptr) {
                return {
                    nullptr,
                    make_failure(
                        HttpFailureReason::
                            multi_init_failed,
                        DownloadErrc::http_init_failed)
                };
            }
            session->identity_headers_ =
                detail::slist_append(
                    nullptr,
                    "Accept-Encoding: identity");
            if (session->identity_headers_ == nullptr) {
                return {
                    nullptr,
                    make_failure(
                        HttpFailureReason::
                            allocation_failed,
                        DownloadErrc::
                            http_init_failed)
                };
            }
            if (detail::multi_setopt(
                    session->multi_,
                    CURLMOPT_MAX_TOTAL_CONNECTIONS,
                    static_cast<long>(
                        config.max_active_transfers)) !=
                    CURLM_OK ||
                detail::multi_setopt(
                    session->multi_,
                    CURLMOPT_MAX_HOST_CONNECTIONS,
                    static_cast<long>(
                        config.max_active_transfers)) !=
                    CURLM_OK) {
                return {
                    nullptr,
                    make_failure(
                        HttpFailureReason::
                            multi_option_failed,
                        DownloadErrc::http_init_failed)
                };
            }

            session->slots_.reserve(
                config.max_active_transfers);
            for (std::size_t index = 0;
                 index < config.max_active_transfers;
                 ++index) {
                auto slot = std::make_unique<Slot>();
                slot->owner = session.get();
                slot->id =
                    static_cast<TransferSlotId>(index);
                slot->easy =
                    detail::easy_init();
                if (slot->easy == nullptr) {
                    return {
                        nullptr,
                        make_failure(
                            HttpFailureReason::
                                easy_init_failed,
                            DownloadErrc::
                                http_init_failed)
                    };
                }
                const auto lane_error =
                    packet_producer.open_lane(
                        slot->lane);
                if (lane_error) {
                    return {
                        nullptr,
                        {
                            HttpFailureReason::
                                callback_sink_failed,
                            lane_error
                        }
                    };
                }
                session->slots_.push_back(
                    std::move(slot));
            }
            return {std::move(session), {}};
        } catch (const std::bad_alloc&) {
            return {
                nullptr,
                make_failure(
                    HttpFailureReason::
                        allocation_failed,
                    DownloadErrc::internal_error)
            };
        } catch (...) {
            return {
                nullptr,
                make_failure(
                    HttpFailureReason::
                        protocol_order_invalid,
                    DownloadErrc::internal_error)
            };
        }
    }

    ~CurlHttpTransferSession() override {
        static_cast<void>(cleanup());
    }

    HttpStartResult start(
        const range::RangeLease& lease) noexcept override {
        if (!owner_thread_matches()) {
            return failed_start(
                make_failure(
                    HttpFailureReason::
                        protocol_order_invalid,
                    DownloadErrc::internal_error));
        }
        if (state_ == HttpSessionState::closed) {
            return {
                HttpStartCode::closed,
                std::nullopt,
                {}
            };
        }
        if (pending_count() != 0) {
            return {
                HttpStartCode::no_capacity,
                std::nullopt,
                {}
            };
        }
        if (state_ != HttpSessionState::open) {
            return failed_start(
                {
                    HttpFailureReason::
                        protocol_order_invalid,
                    error_
                        ? error_
                        : make_error_code(
                            DownloadErrc::
                                http_transfer_failed)
                });
        }
        if (lease.bytes.begin < 0 ||
            lease.bytes.end <= lease.bytes.begin ||
            lease.bytes.end > config_.total_size) {
            return failed_start(
                make_failure(
                    HttpFailureReason::
                        protocol_order_invalid,
                    DownloadErrc::internal_error));
        }

        const auto found = std::find_if(
            slots_.begin(),
            slots_.end(),
            [](const std::unique_ptr<Slot>& slot) {
                return !slot->lease.has_value() &&
                    !slot->pending_event.has_value() &&
                    !slot->in_multi;
            });
        if (found == slots_.end()) {
            return {
                HttpStartCode::no_capacity,
                std::nullopt,
                {}
            };
        }

        auto& slot = **found;
        try {
            ++slot.generation;
            if (slot.generation == 0) {
                return failed_start(
                    make_failure(
                        HttpFailureReason::
                            protocol_order_invalid,
                        DownloadErrc::internal_error));
            }
            slot.lease = lease;
            slot.accepted_through =
                lease.bytes.begin;
            slot.body_bytes = 0;
            slot.bytes_per_second = 0.0;
            slot.packet_pause_mask = 0;
            slot.gap_paused = false;
            slot.curl_receive_paused = false;
            slot.callback_active = false;
            slot.cancelling = false;
            slot.first_byte_recorded = false;
            slot.callback_failure = {};
            slot.response.reset();
            slot.range_header.clear();

            detail::easy_reset(slot.easy);
            if (!configure_slot(slot)) {
                slot.lease.reset();
                return failed_start(
                    make_failure(
                        HttpFailureReason::
                            easy_option_failed,
                        DownloadErrc::
                            http_transfer_failed));
            }
            if (detail::multi_add_handle(
                    multi_,
                    slot.easy) != CURLM_OK) {
                slot.lease.reset();
                return failed_start(
                    make_failure(
                        HttpFailureReason::
                            add_handle_failed,
                        DownloadErrc::
                            http_transfer_failed));
            }
            slot.in_multi = true;
            const TransferToken token{
                slot.id,
                slot.generation,
                lease.id
            };
            return {
                HttpStartCode::started,
                token,
                {}
            };
        } catch (const std::bad_alloc&) {
            slot.lease.reset();
            return failed_start(
                make_failure(
                    HttpFailureReason::
                        allocation_failed,
                    DownloadErrc::internal_error));
        } catch (...) {
            slot.lease.reset();
            return failed_start(
                make_failure(
                    HttpFailureReason::
                        protocol_order_invalid,
                    DownloadErrc::internal_error));
        }
    }

    std::error_code set_gap_paused(
        const TransferToken& token,
        const bool active) noexcept override {
        if (!owner_thread_matches()) {
            return make_error_code(
                DownloadErrc::internal_error);
        }
        auto* slot = find_active(token);
        if (slot == nullptr) {
            return make_error_code(
                DownloadErrc::internal_error);
        }
        if (active && !slot->gap_paused) {
            telemetry_.record_pause(
                telemetry::TelemetryPauseReason::gap,
                false);
        }
        slot->gap_paused = active;
        return {};
    }

    HttpPollResult poll(
        const std::chrono::milliseconds timeout)
            noexcept override {
        if (!owner_thread_matches()) {
            const auto error =
                make_error_code(
                    DownloadErrc::internal_error);
            return {
                HttpPollCode::failed,
                std::nullopt,
                error
            };
        }
        if (timeout.count() < 0) {
            const auto failure =
                make_failure(
                    HttpFailureReason::
                        protocol_order_invalid,
                    DownloadErrc::internal_error);
            stop_active_for_failure(
                failure,
                false);
            return {
                HttpPollCode::failed,
                std::nullopt,
                failure.error
            };
        }
        if (state_ == HttpSessionState::closed) {
            return {
                HttpPollCode::closed,
                std::nullopt,
                {}
            };
        }
        if (const auto event = take_event();
            event.has_value()) {
            return {
                HttpPollCode::event,
                std::move(*event),
                {}
            };
        }
        if (state_ == HttpSessionState::failed) {
            return {
                HttpPollCode::failed,
                std::nullopt,
                error_
            };
        }

        const auto reconcile_failure =
            reconcile_pauses();
        if (reconcile_failure.error) {
            stop_active_for_failure(
                reconcile_failure,
                reconcile_failure.reason ==
                    HttpFailureReason::
                        upstream_failed);
            return {
                HttpPollCode::failed,
                std::nullopt,
                reconcile_failure.error
            };
        }

        const auto first_drive =
            perform_and_drain();
        if (first_drive.error) {
            stop_active_for_failure(
                first_drive.failure,
                false);
            return first_drive;
        }
        if (const auto event = take_event();
            event.has_value()) {
            return {
                HttpPollCode::event,
                std::move(*event),
                {}
            };
        }
        if (active_count() == 0) {
            if (state_ == HttpSessionState::failed) {
                return {
                    HttpPollCode::failed,
                    std::nullopt,
                    error_
                };
            }
            return {
                HttpPollCode::idle,
                std::nullopt,
                {}
            };
        }

        int descriptor_count = 0;
        const auto wait_result =
            detail::multi_wait(
                multi_,
                nullptr,
                0,
                static_cast<int>(timeout.count()),
                &descriptor_count);
        static_cast<void>(descriptor_count);
        if (wait_result != CURLM_OK) {
            const auto failure =
                make_failure(
                    HttpFailureReason::
                        multi_wait_failed,
                    DownloadErrc::
                        http_transfer_failed);
            stop_active_for_failure(
                failure,
                false);
            return {
                HttpPollCode::failed,
                std::nullopt,
                failure.error
            };
        }

        const auto second_drive =
            perform_and_drain();
        if (second_drive.error) {
            stop_active_for_failure(
                second_drive.failure,
                false);
            return second_drive;
        }
        if (const auto event = take_event();
            event.has_value()) {
            return {
                HttpPollCode::event,
                std::move(*event),
                {}
            };
        }
        return {
            HttpPollCode::timed_out,
            std::nullopt,
            {}
        };
    }

    std::error_code cancel(
        const HttpCancelRequest& request)
            noexcept override {
        if (!owner_thread_matches()) {
            return make_error_code(
                DownloadErrc::internal_error);
        }
        if ((request.kind ==
                HttpCancelKind::task_cancelled &&
             request.cause) ||
            (request.kind ==
                HttpCancelKind::upstream_failed &&
             !request.cause)) {
            return make_error_code(
                DownloadErrc::internal_error);
        }
        if (state_ == HttpSessionState::closed) {
            return {};
        }
        state_ = HttpSessionState::cancelling;
        if (request.kind ==
                HttpCancelKind::upstream_failed &&
            !error_) {
            error_ = request.cause;
            primary_failure_ = {
                HttpFailureReason::
                    upstream_failed,
                request.cause
            };
        }

        for (auto& slot_pointer : slots_) {
            auto& slot = *slot_pointer;
            if (!slot.lease.has_value() ||
                slot.pending_event.has_value()) {
                continue;
            }
            slot.cancelling = true;
            if (slot.in_multi) {
                if (detail::multi_remove_handle(
                        multi_,
                        slot.easy) == CURLM_OK) {
                    slot.in_multi = false;
                } else if (!error_) {
                    const auto failure =
                        make_failure(
                            HttpFailureReason::
                                remove_handle_failed,
                            DownloadErrc::
                                http_transfer_failed);
                    error_ = failure.error;
                    primary_failure_ = failure;
                }
            }
            const auto lane_error =
                settle_lane(
                    slot,
                    request.kind ==
                        HttpCancelKind::
                            upstream_failed);
            if (lane_error && !error_) {
                error_ = lane_error;
                primary_failure_ = {
                    HttpFailureReason::
                        callback_sink_failed,
                    lane_error
                };
            }
            const auto cause = request.kind ==
                    HttpCancelKind::upstream_failed
                ? request.cause
                : make_error_code(
                    DownloadErrc::cancelled);
            slot.pending_event = HttpLeaseFailed{
                token_for(slot),
                slot.lease->id,
                slot.accepted_through,
                {
                    request.kind ==
                            HttpCancelKind::
                                upstream_failed
                        ? HttpFailureReason::
                            upstream_failed
                        : HttpFailureReason::
                            cancelled,
                    cause
                }
            };
        }
        return error_;
    }

    HttpSessionSnapshot snapshot()
        const noexcept override {
        const auto active = active_count();
        const auto pending = pending_count();
        std::size_t paused = 0;
        for (const auto& slot : slots_) {
            if (slot->lease.has_value() &&
                !slot->pending_event.has_value() &&
                (slot->gap_paused ||
                 slot->curl_receive_paused)) {
                ++paused;
            }
        }
        return {
            state_,
            active,
            pending == 0 &&
                    state_ == HttpSessionState::open
                ? slots_.size() - active
                : 0,
            pending,
            paused,
            error_
        };
    }

    std::error_code close() noexcept override {
        if (!owner_thread_matches()) {
            return make_error_code(
                DownloadErrc::internal_error);
        }
        if (state_ == HttpSessionState::closed) {
            return {};
        }
        if (active_count() != 0 ||
            pending_count() != 0) {
            return make_error_code(
                DownloadErrc::internal_error);
        }
        const auto cleanup_error = cleanup();
        state_ = HttpSessionState::closed;
        return error_
            ? error_
            : cleanup_error;
    }

private:
    struct DriveResult : HttpPollResult {
        HttpFailure failure{};
    };

    CurlHttpTransferSession(
        HttpSessionConfig config,
        flow::PacketProducer& packet_producer,
        telemetry::TelemetrySession& telemetry) noexcept
        : config_(std::move(config)),
          packet_producer_(packet_producer),
          telemetry_(telemetry),
          owner_thread_(std::this_thread::get_id()) {}

    static std::size_t write_callback(
        char* data,
        const std::size_t size,
        const std::size_t count,
        void* user_data) noexcept {
        auto* slot = static_cast<Slot*>(user_data);
        const auto bytes = callback_bytes(
            size,
            count);
        if (slot == nullptr ||
            !bytes.has_value()) {
            return CURL_WRITEFUNC_ERROR;
        }
        return slot->owner->accept_body(
            *slot,
            data,
            *bytes);
    }

    static std::size_t header_callback(
        char* data,
        const std::size_t size,
        const std::size_t count,
        void* user_data) noexcept {
        auto* slot = static_cast<Slot*>(user_data);
        const auto bytes = callback_bytes(
            size,
            count);
        if (slot == nullptr ||
            !bytes.has_value()) {
            return CURL_WRITEFUNC_ERROR;
        }
        if (!slot->response.append(
                std::string_view(data, *bytes))) {
            slot->callback_failure =
                make_failure(
                    HttpFailureReason::
                        header_callback_failed,
                    DownloadErrc::
                        http_invalid_response);
            return CURL_WRITEFUNC_ERROR;
        }
        return *bytes;
    }

    std::size_t accept_body(
        Slot& slot,
        char* data,
        const std::size_t bytes) noexcept {
        slot.callback_active = true;
        const auto finish_callback =
            [&slot]() noexcept {
                slot.callback_active = false;
            };
        if (bytes == 0) {
            finish_callback();
            return 0;
        }
        if (!slot.lease.has_value() ||
            slot.cancelling) {
            slot.callback_failure =
                make_failure(
                    slot.cancelling
                        ? HttpFailureReason::cancelled
                        : HttpFailureReason::
                            protocol_order_invalid,
                    slot.cancelling
                        ? DownloadErrc::cancelled
                        : DownloadErrc::internal_error);
            finish_callback();
            return CURL_WRITEFUNC_ERROR;
        }
        const auto response_status =
            slot.response.status();
        if ((response_status >= 100 &&
             response_status < 200) ||
            (response_status >= 300 &&
             response_status < 400)) {
            slot.response.begin_body();
            finish_callback();
            return bytes;
        }
        const auto response_failure =
            validate_response(slot);
        if (response_failure.error) {
            slot.callback_failure =
                response_failure;
            finish_callback();
            return CURL_WRITEFUNC_ERROR;
        }
        slot.response.begin_body();
        const auto remaining =
            slot.lease->bytes.end -
            slot.accepted_through;
        if (remaining < 0 ||
            bytes > static_cast<std::uint64_t>(
                        remaining)) {
            slot.callback_failure =
                make_failure(
                    HttpFailureReason::
                        body_too_long,
                    DownloadErrc::
                        http_invalid_response);
            finish_callback();
            return CURL_WRITEFUNC_ERROR;
        }
        const flow::DataChunk chunk{
            slot.lease->id,
            slot.lease->bytes,
            slot.accepted_through,
            {
                reinterpret_cast<
                    const std::uint8_t*>(data),
                bytes
            }
        };
        const auto admission =
            packet_producer_.accept(
                slot.lane,
                chunk);
        slot.packet_pause_mask =
            admission.active_pause_mask;
        if (admission.code ==
            flow::PacketAdmissionCode::accepted) {
            if (admission.consumed_bytes != bytes) {
                slot.callback_failure =
                    make_failure(
                        HttpFailureReason::
                            protocol_order_invalid,
                        DownloadErrc::internal_error);
                finish_callback();
                return CURL_WRITEFUNC_ERROR;
            }
            if (!slot.first_byte_recorded) {
                telemetry_.
                    record_first_byte_received();
                slot.first_byte_recorded = true;
            }
            slot.accepted_through +=
                static_cast<range::ByteOffset>(
                    bytes);
            slot.body_bytes += bytes;
            finish_callback();
            return bytes;
        }
        if (admission.must_pause() &&
            admission.consumed_bytes == 0) {
            slot.curl_receive_paused = true;
            finish_callback();
            return CURL_WRITEFUNC_PAUSE;
        }
        slot.callback_failure = {
            HttpFailureReason::
                callback_sink_failed,
            admission.error
                ? admission.error
                : make_error_code(
                    DownloadErrc::
                        http_transfer_failed)
        };
        finish_callback();
        return CURL_WRITEFUNC_ERROR;
    }

    bool configure_slot(Slot& slot) noexcept {
        if (!slot.lease.has_value()) {
            return false;
        }
        const auto set = [&slot](
            const CURLoption option,
            const auto value) noexcept {
            return detail::easy_setopt(
                slot.easy,
                option,
                value) == CURLE_OK;
        };
        if (!set(CURLOPT_URL, config_.url.c_str()) ||
            !set(CURLOPT_FOLLOWLOCATION, 1L) ||
            !set(CURLOPT_NOSIGNAL, 1L) ||
            !set(
                CURLOPT_HTTP_VERSION,
                CURL_HTTP_VERSION_1_1) ||
            !set(
                CURLOPT_WRITEFUNCTION,
                write_callback) ||
            !set(CURLOPT_WRITEDATA, &slot) ||
            !set(
                CURLOPT_HEADERFUNCTION,
                header_callback) ||
            !set(CURLOPT_HEADERDATA, &slot) ||
            !set(CURLOPT_PRIVATE, &slot) ||
            !set(CURLOPT_TCP_KEEPALIVE, 1L) ||
            !set(
                CURLOPT_ACCEPT_ENCODING,
                static_cast<const char*>(nullptr)) ||
            !set(
                CURLOPT_HTTP_CONTENT_DECODING,
                0L) ||
            !set(
                CURLOPT_HTTPHEADER,
                identity_headers_) ||
            !set(CURLOPT_FRESH_CONNECT, 1L) ||
            !set(CURLOPT_FORBID_REUSE, 1L) ||
            !set(CURLOPT_PIPEWAIT, 0L)) {
            return false;
        }
        if (slot.lease->use_http_range) {
            const auto inclusive_end =
                slot.lease->bytes.end - 1;
            slot.range_header =
                std::to_string(
                    slot.lease->bytes.begin) +
                "-" +
                std::to_string(inclusive_end);
            return set(
                CURLOPT_RANGE,
                slot.range_header.c_str());
        }
        return set(CURLOPT_RANGE, nullptr);
    }

    HttpFailure reconcile_pauses() noexcept {
        observations_.clear();
        actions_.clear();
        try {
            for (const auto& slot : slots_) {
                if (!slot->lease.has_value() ||
                    slot->pending_event.has_value()) {
                    continue;
                }
                observations_.push_back({
                    slot->lane.id(),
                    slot->bytes_per_second,
                    true
                });
            }
            actions_.resize(
                observations_.size());
        } catch (...) {
            return make_failure(
                HttpFailureReason::
                    allocation_failed,
                DownloadErrc::internal_error);
        }
        const auto reconciled =
            packet_producer_.reconcile(
                observations_,
                actions_);
        if (reconciled.error ||
            reconciled.action_count >
                actions_.size()) {
            return reconciled.error
                ? HttpFailure{
                    HttpFailureReason::
                        upstream_failed,
                    reconciled.error
                }
                : make_failure(
                    HttpFailureReason::
                        protocol_order_invalid,
                    DownloadErrc::internal_error);
        }
        for (std::size_t index = 0;
             index < reconciled.action_count;
             ++index) {
            const auto& action = actions_[index];
            const auto found = std::find_if(
                slots_.begin(),
                slots_.end(),
                [&action](
                    const std::unique_ptr<Slot>& slot) {
                    return slot->lease.has_value() &&
                        slot->lane.id() ==
                            action.lane_id;
                });
            if (found != slots_.end()) {
                (*found)->packet_pause_mask =
                    action.active_pause_mask;
            }
        }
        for (auto& slot : slots_) {
            if (!slot->lease.has_value() ||
                slot->pending_event.has_value() ||
                !slot->in_multi) {
                continue;
            }
            const auto should_pause =
                slot->packet_pause_mask != 0 ||
                slot->gap_paused;
            if (should_pause &&
                !slot->curl_receive_paused) {
                if (detail::easy_pause(
                        slot->easy,
                        CURLPAUSE_RECV) != CURLE_OK) {
                    return make_failure(
                        HttpFailureReason::
                            pause_failed,
                        DownloadErrc::
                            http_transfer_failed);
                }
                slot->curl_receive_paused = true;
            } else if (!should_pause &&
                       slot->curl_receive_paused) {
                slot->curl_receive_paused = false;
                if (detail::easy_pause(
                        slot->easy,
                        CURLPAUSE_CONT) != CURLE_OK) {
                    return make_failure(
                        HttpFailureReason::
                            pause_failed,
                        DownloadErrc::
                            http_transfer_failed);
                }
            }
        }
        return {};
    }

    DriveResult perform_and_drain() noexcept {
        int running_handles = 0;
        const auto perform_result =
            detail::multi_perform(
                multi_,
                &running_handles);
        static_cast<void>(running_handles);
        if (perform_result != CURLM_OK) {
            const auto failure =
                make_failure(
                    HttpFailureReason::
                        multi_perform_failed,
                    DownloadErrc::
                        http_transfer_failed);
            return {
                {
                    HttpPollCode::failed,
                    std::nullopt,
                    failure.error
                },
                failure
            };
        }
        int pending_messages = 0;
        while (auto* message =
                   detail::multi_info_read(
                       multi_,
                       &pending_messages)) {
            if (message->msg != CURLMSG_DONE) {
                continue;
            }
            auto* slot = find_slot(
                message->easy_handle);
            if (slot == nullptr) {
                const auto failure =
                    make_failure(
                        HttpFailureReason::
                            protocol_order_invalid,
                        DownloadErrc::
                            internal_error);
                return {
                    {
                        HttpPollCode::failed,
                        std::nullopt,
                        failure.error
                    },
                    failure
                };
            }
            finalize_done(
                *slot,
                message->data.result);
        }
        return {
            {
                HttpPollCode::idle,
                std::nullopt,
                {}
            },
            {}
        };
    }

    void finalize_done(
        Slot& slot,
        const CURLcode curl_result) noexcept {
        long response_code = 0;
        const auto response_info =
            detail::easy_getinfo(
                slot.easy,
                CURLINFO_RESPONSE_CODE,
                &response_code);
        curl_off_t speed = 0;
        const auto speed_info =
            detail::easy_getinfo(
                slot.easy,
                CURLINFO_SPEED_DOWNLOAD_T,
                &speed);
        if (speed_info == CURLE_OK &&
            speed > 0) {
            slot.bytes_per_second =
                static_cast<double>(speed);
        }
        auto remove_result = CURLM_OK;
        if (slot.in_multi) {
            remove_result =
                detail::multi_remove_handle(
                    multi_,
                    slot.easy);
            if (remove_result == CURLM_OK) {
                slot.in_multi = false;
            }
        }
        HttpFailure failure =
            slot.callback_failure;
        if (!failure.error) {
            if (response_info != CURLE_OK ||
                speed_info != CURLE_OK) {
                failure = make_failure(
                    HttpFailureReason::
                        easy_info_failed,
                    DownloadErrc::
                        http_transfer_failed);
            } else if (remove_result != CURLM_OK) {
                failure = make_failure(
                    HttpFailureReason::
                        remove_handle_failed,
                    DownloadErrc::
                        http_transfer_failed);
            }
        }
        const auto flush_error =
            flush_lane(slot);
        if (!failure.error && flush_error) {
            failure = {
                HttpFailureReason::
                    callback_sink_failed,
                flush_error
            };
        }
        if (!failure.error &&
            curl_result != CURLE_OK) {
            failure = make_failure(
                HttpFailureReason::
                    transport_failed,
                DownloadErrc::
                    http_transfer_failed);
        }
        if (!failure.error) {
            failure =
                validate_response(
                    slot,
                    response_code);
        }
        if (!failure.error &&
            slot.lease.has_value() &&
            slot.accepted_through !=
                slot.lease->bytes.end) {
            failure = make_failure(
                HttpFailureReason::body_too_short,
                DownloadErrc::
                    http_invalid_response);
        }

        if (failure.error) {
            slot.pending_event = HttpLeaseFailed{
                token_for(slot),
                slot.lease->id,
                slot.accepted_through,
                failure
            };
            fail_session(failure);
        } else {
            slot.pending_event =
                HttpLeaseSucceeded{
                    token_for(slot),
                    slot.lease->id,
                    slot.accepted_through,
                    response_code
                };
        }
    }

    HttpFailure validate_response(
        const Slot& slot,
        const long response_code = 0) const noexcept {
        if (!slot.lease.has_value()) {
            return make_failure(
                HttpFailureReason::
                    protocol_order_invalid,
                DownloadErrc::internal_error);
        }
        const auto& response = slot.response;
        const auto status = response.status();
        if (!response.headers_complete() ||
            status == 0 ||
            (response_code != 0 &&
             status != response_code)) {
            return make_failure(
                HttpFailureReason::
                    response_status_invalid,
                DownloadErrc::
                    http_invalid_response);
        }
        const auto whole_object =
            slot.lease->bytes.begin == 0 &&
            slot.lease->bytes.end ==
                config_.total_size;
        const auto partial_range =
            slot.lease->use_http_range &&
            !whole_object;
        if ((partial_range && status != 206) ||
            (!slot.lease->use_http_range &&
             status != 200) ||
            (slot.lease->use_http_range &&
             whole_object &&
             status != 200 &&
             status != 206)) {
            return make_failure(
                HttpFailureReason::
                    response_status_invalid,
                DownloadErrc::
                    http_invalid_response);
        }
        if (response.content_range_invalid()) {
            return make_failure(
                HttpFailureReason::
                    content_range_malformed,
                DownloadErrc::
                    http_invalid_response);
        }
        if (!response.content_encoding_is_identity()) {
            return make_failure(
                HttpFailureReason::
                    content_encoding_invalid,
                DownloadErrc::
                    http_invalid_response);
        }
        const auto requires_content_range =
            status == 206;
        if (requires_content_range &&
            !response.content_range().has_value()) {
            return make_failure(
                HttpFailureReason::
                    content_range_missing,
                DownloadErrc::
                    http_invalid_response);
        }
        if (!requires_content_range &&
            response.content_range().has_value()) {
            return make_failure(
                HttpFailureReason::
                    content_range_mismatch,
                DownloadErrc::
                    http_invalid_response);
        }
        if (requires_content_range) {
            const auto& content_range =
                *response.content_range();
            if (content_range.first !=
                    slot.lease->bytes.begin ||
                content_range.last !=
                    slot.lease->bytes.end - 1 ||
                content_range.total !=
                    config_.total_size) {
                return make_failure(
                    HttpFailureReason::
                        content_range_mismatch,
                    DownloadErrc::
                        http_invalid_response);
            }
        }
        if (response.content_length_invalid()) {
            return make_failure(
                HttpFailureReason::
                    content_length_malformed,
                DownloadErrc::
                    http_invalid_response);
        }
        const auto expected_length =
            slot.lease->bytes.end -
            slot.lease->bytes.begin;
        if (response.content_length().has_value() &&
            *response.content_length() !=
                expected_length) {
            return make_failure(
                HttpFailureReason::
                    content_length_mismatch,
                DownloadErrc::
                    http_invalid_response);
        }
        return {};
    }

    std::error_code flush_lane(
        Slot& slot) noexcept {
        while (true) {
            const auto admission =
                packet_producer_.flush(slot.lane);
            if (admission.code ==
                flow::PacketAdmissionCode::accepted) {
                return {};
            }
            if (admission.must_pause()) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(1));
                continue;
            }
            return admission.error
                ? admission.error
                : make_error_code(
                    DownloadErrc::
                        http_transfer_failed);
        }
    }

    Slot* find_slot(CURL* easy) noexcept {
        const auto found = std::find_if(
            slots_.begin(),
            slots_.end(),
            [easy](
                const std::unique_ptr<Slot>& slot) {
                return slot->easy == easy;
            });
        return found == slots_.end()
            ? nullptr
            : found->get();
    }

    Slot* find_active(
        const TransferToken& token) noexcept {
        if (token.slot >= slots_.size()) {
            return nullptr;
        }
        auto& slot = *slots_[token.slot];
        return slot.lease.has_value() &&
               !slot.pending_event.has_value() &&
               token == token_for(slot)
            ? &slot
            : nullptr;
    }

    TransferToken token_for(
        const Slot& slot) const noexcept {
        return {
            slot.id,
            slot.generation,
            slot.lease.has_value()
                ? slot.lease->id
                : range::LeaseId{}
        };
    }

    std::optional<HttpTransferEvent>
    take_event() noexcept {
        for (auto& slot : slots_) {
            if (!slot->pending_event.has_value()) {
                continue;
            }
            auto event =
                std::move(slot->pending_event);
            slot->pending_event.reset();
            slot->lease.reset();
            slot->range_header.clear();
            slot->callback_failure = {};
            return event;
        }
        return std::nullopt;
    }

    std::size_t active_count() const noexcept {
        return static_cast<std::size_t>(
            std::count_if(
                slots_.begin(),
                slots_.end(),
                [](const std::unique_ptr<Slot>& slot) {
                    return slot->lease.has_value() &&
                        !slot->pending_event.has_value();
                }));
    }

    std::size_t pending_count() const noexcept {
        return static_cast<std::size_t>(
            std::count_if(
                slots_.begin(),
                slots_.end(),
                [](const std::unique_ptr<Slot>& slot) {
                    return slot->pending_event.
                        has_value();
                }));
    }

    bool owner_thread_matches() const noexcept {
        return owner_thread_ ==
            std::this_thread::get_id();
    }

    HttpStartResult failed_start(
        const HttpFailure failure) noexcept {
        return {
            HttpStartCode::failed,
            std::nullopt,
            failure
        };
    }

    void fail_session(
        const HttpFailure failure) noexcept {
        state_ = HttpSessionState::failed;
        if (!error_ && failure.error) {
            error_ = failure.error;
            primary_failure_ = failure;
        }
    }

    std::error_code settle_lane(
        Slot& slot,
        const bool discard) noexcept {
        if (discard) {
            return packet_producer_.discard(
                slot.lane);
        }
        const auto flush_error =
            flush_lane(slot);
        if (!flush_error) {
            return {};
        }
        const auto discard_error =
            packet_producer_.discard(
                slot.lane);
        return flush_error
            ? flush_error
            : discard_error;
    }

    void stop_active_for_failure(
        const HttpFailure failure,
        const bool discard) noexcept {
        fail_session(failure);
        for (auto& slot_pointer : slots_) {
            auto& slot = *slot_pointer;
            if (!slot.lease.has_value() ||
                slot.pending_event.has_value()) {
                continue;
            }
            slot.cancelling = true;
            if (slot.in_multi) {
                if (detail::multi_remove_handle(
                        multi_,
                        slot.easy) == CURLM_OK) {
                    slot.in_multi = false;
                }
            }
            static_cast<void>(
                settle_lane(
                    slot,
                    discard));
            slot.pending_event = HttpLeaseFailed{
                token_for(slot),
                slot.lease->id,
                slot.accepted_through,
                failure
            };
        }
    }

    std::error_code cleanup() noexcept {
        std::error_code cleanup_error;
        for (auto& slot : slots_) {
            if (slot->in_multi &&
                multi_ != nullptr) {
                if (detail::multi_remove_handle(
                        multi_,
                        slot->easy) != CURLM_OK &&
                    !cleanup_error) {
                    cleanup_error =
                        make_error_code(
                            DownloadErrc::
                                http_transfer_failed);
                }
                slot->in_multi = false;
            }
            if (slot->easy != nullptr) {
                detail::easy_cleanup(
                    slot->easy);
                slot->easy = nullptr;
            }
        }
        if (multi_ != nullptr) {
            if (detail::multi_cleanup(
                    multi_) != CURLM_OK &&
                !cleanup_error) {
                cleanup_error =
                    make_error_code(
                        DownloadErrc::
                            http_transfer_failed);
            }
            multi_ = nullptr;
        }
        if (identity_headers_ != nullptr) {
            detail::slist_free_all(
                identity_headers_);
            identity_headers_ = nullptr;
        }
        if (!error_ && cleanup_error) {
            error_ = cleanup_error;
        }
        return cleanup_error;
    }

    HttpSessionConfig config_;
    flow::PacketProducer& packet_producer_;
    telemetry::TelemetrySession& telemetry_;
    std::thread::id owner_thread_;
    CURLM* multi_ = nullptr;
    curl_slist* identity_headers_ = nullptr;
    std::vector<std::unique_ptr<Slot>> slots_;
    std::vector<flow::PacketLaneObservation>
        observations_;
    std::vector<flow::PacketPauseAction> actions_;
    HttpSessionState state_ = HttpSessionState::open;
    std::error_code error_{};
    HttpFailure primary_failure_{};
};

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

        curl_slist* identity_headers =
            detail::slist_append(
                nullptr,
                "Accept-Encoding: identity");
        if (identity_headers == nullptr) {
            return failed_probe(
                make_failure(
                    HttpFailureReason::
                        allocation_failed,
                    DownloadErrc::http_probe_failed));
        }
        const auto free_identity_headers =
            [&identity_headers]() noexcept {
                detail::slist_free_all(
                    identity_headers);
            };
        CURL* easy = detail::easy_init();
        if (easy == nullptr) {
            free_identity_headers();
            return failed_probe(
                make_failure(
                    HttpFailureReason::easy_init_failed,
                    DownloadErrc::http_init_failed));
        }

        HttpResponseAccumulator response;
        if (!configure_probe_common(
                easy,
                request.url,
                response,
                identity_headers) ||
            detail::easy_setopt(
                easy,
                CURLOPT_NOBODY,
                1L) != CURLE_OK ||
            detail::easy_setopt(
                easy,
                CURLOPT_RANGE,
                static_cast<const char*>(
                    nullptr)) != CURLE_OK) {
            detail::easy_cleanup(easy);
            free_identity_headers();
            return failed_probe(
                make_failure(
                    HttpFailureReason::
                        easy_option_failed,
                    DownloadErrc::
                        http_probe_failed));
        }

        const auto curl_result =
            detail::easy_perform(easy);
        long response_code = 0;
        const auto response_info =
            detail::easy_getinfo(
                easy,
                CURLINFO_RESPONSE_CODE,
                &response_code);
        curl_off_t response_length = -1;
        const auto length_info =
            detail::easy_getinfo(
                easy,
                CURLINFO_CONTENT_LENGTH_DOWNLOAD_T,
                &response_length);
        detail::easy_cleanup(easy);

        if (curl_result != CURLE_OK) {
            const auto result =
                fallback_probe(
                    request,
                    identity_headers);
            free_identity_headers();
            return result;
        }
        if (response.parser_failed()) {
            free_identity_headers();
            return failed_probe(
                make_failure(
                    HttpFailureReason::
                        header_callback_failed,
                    DownloadErrc::http_probe_failed),
                response_code);
        }
        if (response_info != CURLE_OK ||
            length_info != CURLE_OK) {
            free_identity_headers();
            return failed_probe(
                make_failure(
                    HttpFailureReason::
                        easy_info_failed,
                    DownloadErrc::
                        http_probe_failed),
                response_code);
        }
        if (!response.content_encoding_is_identity()) {
            free_identity_headers();
            return failed_probe(
                make_failure(
                    HttpFailureReason::
                        content_encoding_invalid,
                    DownloadErrc::http_probe_failed),
                response_code);
        }
        if (response_code != 200 ||
            response_length <= 0 ||
            response.content_length_invalid() ||
            !response.content_length().has_value() ||
            *response.content_length() <= 0) {
            const auto result =
                fallback_probe(
                    request,
                    identity_headers);
            free_identity_headers();
            return result;
        }

        free_identity_headers();
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
        return CurlHttpTransferSession::create(
            config,
            packet_producer,
            telemetry);
    }
};

CURLcode curl_runtime_result() noexcept {
    static const auto result =
        detail::global_init(
            CURL_GLOBAL_DEFAULT);
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
