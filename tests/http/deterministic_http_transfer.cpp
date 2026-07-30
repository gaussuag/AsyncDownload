#include "deterministic_http_transfer.hpp"

#include "asyncdownload/error.hpp"
#include "asyncdownload/telemetry/telemetry_event.hpp"
#include "asyncdownload/telemetry/telemetry_session.hpp"
#include "flow/packet_flow.hpp"

#include <algorithm>
#include <new>
#include <utility>

namespace asyncdownload::http::testing {
namespace {

HttpFailure protocol_failure() noexcept {
    return {
        HttpFailureReason::protocol_order_invalid,
        make_error_code(DownloadErrc::internal_error)
    };
}

HttpFailure allocation_failure() noexcept {
    return {
        HttpFailureReason::allocation_failed,
        make_error_code(DownloadErrc::internal_error)
    };
}

}

struct DeterministicHttpTransferPort::State {
    std::vector<FakeProbeStep> probe_steps;
    std::vector<FakeTransferScript> transfer_scripts;
    std::size_t probe_index = 0;
    std::size_t transfer_index = 0;
    bool session_opened = false;
};

class DeterministicHttpTransferSession final : public HttpTransferSession {
public:
    struct Slot {
        flow::ProducerLane lane;
        std::optional<TransferToken> token;
        std::optional<HttpTransferEvent> pending_event;
        std::size_t script_index = 0;
        std::size_t delivery_index = 0;
        range::ByteOffset accepted_through = 0;
        std::uint64_t generation = 0;
        bool active = false;
        bool gap_paused = false;
        bool first_byte_recorded = false;
    };

    static HttpSessionOpenResult create(
        std::shared_ptr<DeterministicHttpTransferPort::State> state,
        const HttpSessionConfig& config,
        flow::PacketProducer& packet_producer,
        telemetry::TelemetrySession& telemetry) noexcept {
        HttpSessionOpenResult result{};
        if (config.total_size <= 0 ||
            config.max_active_transfers == 0 ||
            config.url.empty()) {
            result.failure = protocol_failure();
            return result;
        }

        try {
            auto session = std::unique_ptr<DeterministicHttpTransferSession>(
                new DeterministicHttpTransferSession(
                    std::move(state),
                    packet_producer,
                    telemetry));
            session->slots_.reserve(config.max_active_transfers);
            for (std::size_t index = 0;
                 index < config.max_active_transfers;
                 ++index) {
                auto slot = std::make_unique<Slot>();
                const auto error =
                    packet_producer.open_lane(slot->lane);
                if (error) {
                    result.failure = {
                        HttpFailureReason::callback_sink_failed,
                        error
                    };
                    return result;
                }
                session->slots_.push_back(std::move(slot));
            }
            result.session = std::move(session);
        } catch (const std::bad_alloc&) {
            result.failure = allocation_failure();
        } catch (...) {
            result.failure = protocol_failure();
        }
        return result;
    }

    HttpStartResult start(
        const range::RangeLease& lease) noexcept override {
        if (state_value_ == HttpSessionState::closed) {
            return {HttpStartCode::closed, std::nullopt, {}};
        }
        if (state_value_ != HttpSessionState::open) {
            return {
                HttpStartCode::failed,
                std::nullopt,
                {HttpFailureReason::protocol_order_invalid, error_}
            };
        }
        if (pending_count() != 0) {
            return {HttpStartCode::no_capacity, std::nullopt, {}};
        }

        const auto found = std::find_if(
            slots_.begin(),
            slots_.end(),
            [](const std::unique_ptr<Slot>& slot) {
                return !slot->active &&
                    !slot->pending_event.has_value();
            });
        if (found == slots_.end()) {
            return {HttpStartCode::no_capacity, std::nullopt, {}};
        }
        if (state_->transfer_index >=
                state_->transfer_scripts.size() ||
            state_->transfer_scripts[state_->transfer_index]
                    .expected_lease != lease ||
            lease.bytes.begin < 0 ||
            lease.bytes.end <= lease.bytes.begin) {
            const auto failure = protocol_failure();
            fail_session(failure.error);
            return {
                HttpStartCode::failed,
                std::nullopt,
                failure
            };
        }

        auto& slot = **found;
        ++slot.generation;
        if (slot.generation == 0) {
            const auto failure = protocol_failure();
            fail_session(failure.error);
            return {
                HttpStartCode::failed,
                std::nullopt,
                failure
            };
        }
        const auto slot_index = static_cast<std::size_t>(
            std::distance(slots_.begin(), found));
        slot.token = TransferToken{
            static_cast<TransferSlotId>(slot_index),
            slot.generation,
            lease.id
        };
        slot.script_index = state_->transfer_index;
        slot.delivery_index = 0;
        slot.accepted_through = lease.bytes.begin;
        slot.active = true;
        slot.gap_paused = false;
        slot.first_byte_recorded = false;
        ++state_->transfer_index;

        return {
            HttpStartCode::started,
            slot.token,
            {}
        };
    }

    std::error_code set_gap_paused(
        const TransferToken& token,
        const bool active) noexcept override {
        auto* slot = find_active(token);
        if (slot == nullptr ||
            state_value_ == HttpSessionState::closed) {
            return make_error_code(DownloadErrc::internal_error);
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
        const std::chrono::milliseconds timeout) noexcept override {
        if (timeout.count() < 0) {
            const auto error =
                make_error_code(DownloadErrc::internal_error);
            fail_session(error);
            return {HttpPollCode::failed, std::nullopt, error};
        }
        if (state_value_ == HttpSessionState::closed) {
            return {HttpPollCode::closed, std::nullopt, {}};
        }

        if (const auto pending = take_pending_event();
            pending.has_value()) {
            return {
                HttpPollCode::event,
                std::move(*pending),
                {}
            };
        }

        auto* slot = first_active();
        if (slot == nullptr) {
            return {HttpPollCode::idle, std::nullopt, {}};
        }
        if (slot->gap_paused) {
            return {HttpPollCode::timed_out, std::nullopt, {}};
        }

        auto& script =
            state_->transfer_scripts[slot->script_index];
        if (slot->delivery_index <
            script.deliveries.size()) {
            auto& delivery =
                script.deliveries[slot->delivery_index];
            if (delivery.offset != slot->accepted_through ||
                !slot->token.has_value()) {
                return fail_active_slot(
                    *slot,
                    protocol_failure());
            }
            const range::ByteSpan lease_span =
                script.expected_lease.bytes;
            const flow::DataChunk chunk{
                script.expected_lease.id,
                lease_span,
                delivery.offset,
                delivery.bytes
            };
            const auto admission =
                packet_producer_.accept(slot->lane, chunk);
            if (admission.code ==
                flow::PacketAdmissionCode::accepted) {
                if (admission.consumed_bytes !=
                    delivery.bytes.size()) {
                    return fail_active_slot(
                        *slot,
                        protocol_failure());
                }
                if (!slot->first_byte_recorded &&
                    admission.consumed_bytes != 0) {
                    telemetry_.record_first_byte_received();
                    slot->first_byte_recorded = true;
                }
                slot->accepted_through +=
                    static_cast<range::ByteOffset>(
                        admission.consumed_bytes);
                ++slot->delivery_index;
                return {
                    HttpPollCode::timed_out,
                    std::nullopt,
                    {}
                };
            }
            if (admission.must_pause()) {
                if (admission.consumed_bytes != 0) {
                    return fail_active_slot(
                        *slot,
                        protocol_failure());
                }
                return {
                    HttpPollCode::timed_out,
                    std::nullopt,
                    {}
                };
            }
            return fail_active_slot(
                *slot,
                {
                    HttpFailureReason::callback_sink_failed,
                    admission.error
                        ? admission.error
                        : make_error_code(
                            DownloadErrc::internal_error)
                });
        }

        const auto flushed =
            packet_producer_.flush(slot->lane);
        if (flushed.code !=
            flow::PacketAdmissionCode::accepted) {
            if (flushed.must_pause()) {
                return {
                    HttpPollCode::timed_out,
                    std::nullopt,
                    {}
                };
            }
            return fail_active_slot(
                *slot,
                {
                    HttpFailureReason::callback_sink_failed,
                    flushed.error
                        ? flushed.error
                        : make_error_code(
                            DownloadErrc::internal_error)
                });
        }

        if (script.terminal_failure.has_value()) {
            return fail_active_slot(
                *slot,
                *script.terminal_failure);
        }
        if (slot->accepted_through !=
                script.expected_lease.bytes.end ||
            !slot->token.has_value()) {
            return fail_active_slot(
                *slot,
                {
                    HttpFailureReason::body_too_short,
                    make_error_code(
                        DownloadErrc::http_invalid_response)
                });
        }

        slot->pending_event = HttpLeaseSucceeded{
            *slot->token,
            script.expected_lease.id,
            slot->accepted_through,
            script.response_code
        };
        slot->active = false;
        return {
            HttpPollCode::event,
            take_pending_event(),
            {}
        };
    }

    std::error_code cancel(
        const HttpCancelRequest& request) noexcept override {
        if (state_value_ == HttpSessionState::closed) {
            return {};
        }
        if ((request.kind ==
                HttpCancelKind::task_cancelled &&
                request.cause) ||
            (request.kind ==
                HttpCancelKind::upstream_failed &&
                !request.cause)) {
            return make_error_code(DownloadErrc::internal_error);
        }

        state_value_ = HttpSessionState::cancelling;
        for (auto& slot_pointer : slots_) {
            auto& slot = *slot_pointer;
            if (!slot.active || !slot.token.has_value()) {
                continue;
            }
            const auto discard_error =
                packet_producer_.discard(slot.lane);
            if (discard_error && !error_) {
                error_ = discard_error;
            }
            const auto cause = request.kind ==
                    HttpCancelKind::upstream_failed
                ? request.cause
                : make_error_code(DownloadErrc::cancelled);
            slot.pending_event = HttpLeaseFailed{
                *slot.token,
                slot.token->lease,
                slot.accepted_through,
                {
                    request.kind ==
                            HttpCancelKind::upstream_failed
                        ? HttpFailureReason::upstream_failed
                        : HttpFailureReason::cancelled,
                    cause
                }
            };
            slot.active = false;
        }
        return error_;
    }

    HttpSessionSnapshot snapshot() const noexcept override {
        const auto active = active_count();
        const auto pending = pending_count();
        std::size_t paused = 0;
        for (const auto& slot : slots_) {
            if (slot->active &&
                (slot->gap_paused ||
                 packet_producer_.paused(slot->lane))) {
                ++paused;
            }
        }
        return {
            state_value_,
            active,
            pending == 0 && state_value_ ==
                    HttpSessionState::open
                ? slots_.size() - active
                : 0,
            pending,
            paused,
            error_
        };
    }

    std::error_code close() noexcept override {
        if (state_value_ == HttpSessionState::closed) {
            return {};
        }
        if (active_count() != 0 ||
            pending_count() != 0) {
            return make_error_code(DownloadErrc::internal_error);
        }
        state_value_ = HttpSessionState::closed;
        return error_;
    }

private:
    DeterministicHttpTransferSession(
        std::shared_ptr<DeterministicHttpTransferPort::State> state,
        flow::PacketProducer& packet_producer,
        telemetry::TelemetrySession& telemetry) noexcept
        : state_(std::move(state)),
          packet_producer_(packet_producer),
          telemetry_(telemetry) {}

    void fail_session(const std::error_code error) noexcept {
        state_value_ = HttpSessionState::failed;
        if (!error_) {
            error_ = error;
        }
    }

    HttpPollResult fail_active_slot(
        Slot& slot,
        const HttpFailure failure) noexcept {
        if (!slot.token.has_value()) {
            fail_session(
                make_error_code(DownloadErrc::internal_error));
            return {
                HttpPollCode::failed,
                std::nullopt,
                error_
            };
        }
        slot.pending_event = HttpLeaseFailed{
            *slot.token,
            slot.token->lease,
            slot.accepted_through,
            failure
        };
        slot.active = false;
        fail_session(failure.error);
        return {
            HttpPollCode::event,
            take_pending_event(),
            {}
        };
    }

    Slot* find_active(
        const TransferToken& token) noexcept {
        if (token.slot >= slots_.size()) {
            return nullptr;
        }
        auto& slot = *slots_[token.slot];
        if (!slot.active ||
            !slot.token.has_value() ||
            *slot.token != token) {
            return nullptr;
        }
        return &slot;
    }

    Slot* first_active() noexcept {
        const auto found = std::find_if(
            slots_.begin(),
            slots_.end(),
            [](const std::unique_ptr<Slot>& slot) {
                return slot->active;
            });
        return found == slots_.end()
            ? nullptr
            : found->get();
    }

    std::optional<HttpTransferEvent>
    take_pending_event() noexcept {
        for (auto& slot : slots_) {
            if (!slot->pending_event.has_value()) {
                continue;
            }
            auto event =
                std::move(slot->pending_event);
            slot->pending_event.reset();
            slot->token.reset();
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
                    return slot->active;
                }));
    }

    std::size_t pending_count() const noexcept {
        return static_cast<std::size_t>(
            std::count_if(
                slots_.begin(),
                slots_.end(),
                [](const std::unique_ptr<Slot>& slot) {
                    return slot->pending_event.has_value();
                }));
    }

    std::shared_ptr<DeterministicHttpTransferPort::State> state_;
    flow::PacketProducer& packet_producer_;
    telemetry::TelemetrySession& telemetry_;
    std::vector<std::unique_ptr<Slot>> slots_;
    HttpSessionState state_value_ = HttpSessionState::open;
    std::error_code error_{};
};

DeterministicHttpTransferPort::DeterministicHttpTransferPort(
    std::vector<FakeProbeStep> probe_steps,
    std::vector<FakeTransferScript> transfer_scripts) noexcept {
    try {
        state_ = std::make_shared<State>(
            State{
                std::move(probe_steps),
                std::move(transfer_scripts)
            });
    } catch (...) {
        state_.reset();
    }
}

DeterministicHttpTransferPort::~DeterministicHttpTransferPort() =
    default;

HttpProbeResult DeterministicHttpTransferPort::probe(
    const HttpProbeRequest& request) noexcept {
    if (!state_ ||
        state_->probe_index >= state_->probe_steps.size()) {
        return {
            std::nullopt,
            protocol_failure(),
            0
        };
    }
    const auto& step =
        state_->probe_steps[state_->probe_index];
    if (step.expected != request) {
        return {
            std::nullopt,
            protocol_failure(),
            0
        };
    }
    ++state_->probe_index;
    return step.result;
}

HttpSessionOpenResult
DeterministicHttpTransferPort::open_session(
    const HttpSessionConfig& config,
    flow::PacketProducer& packet_producer,
    telemetry::TelemetrySession& telemetry) noexcept {
    if (!state_) {
        return {nullptr, allocation_failure()};
    }
    if (state_->session_opened) {
        return {nullptr, protocol_failure()};
    }
    state_->session_opened = true;
    return DeterministicHttpTransferSession::create(
        state_,
        config,
        packet_producer,
        telemetry);
}

std::size_t
DeterministicHttpTransferPort::consumed_probe_steps()
    const noexcept {
    return state_ ? state_->probe_index : 0;
}

std::size_t
DeterministicHttpTransferPort::consumed_transfer_scripts()
    const noexcept {
    return state_ ? state_->transfer_index : 0;
}

}
