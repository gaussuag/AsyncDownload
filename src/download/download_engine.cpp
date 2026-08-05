#include "download_engine.hpp"

#include "asyncdownload/error.hpp"
#include "download/download_engine_internal.hpp"
#include "core/block_bitmap.hpp"
#include "core/models.hpp"
#include "core/path_utils.hpp"
#include "download/download_policy.hpp"
#include "download/progress_snapshot_builder.hpp"
#include "download/range_scheduler.hpp"
#include "flow/packet_flow.hpp"
#include "http/http_transfer.hpp"
#include "persistence/persistence_thread.hpp"
#include "range/range_lifecycle.hpp"
#include "recovery/recovery_checkpoint.hpp"

#include <thread-pool/BS_thread_pool.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace asyncdownload::download {
namespace {

using Clock = std::chrono::steady_clock;
struct ExpectedGeometryAck {
    persistence::RangeRegistrationTicket ticket = 0;
    range::RangeId range{};
    std::uint64_t geometry_revision = 0;
    bool received = false;
};

struct PendingLease {
    range::RangeLease lease{};
    std::array<ExpectedGeometryAck, 2> acks{};
    std::size_t ack_count = 0;
};

struct ActiveTransfer {
    http::TransferToken token{};
    range::RangeLease lease{};
};

[[nodiscard]] std::optional<std::int64_t> merged_downloaded_bytes(
    const core::SessionState& session,
    const flow::PacketFlowSnapshot& flow_snapshot) noexcept {
    if (session.recovery_initial_trusted_bytes < 0 ||
        flow_snapshot.published_data_bytes >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max() -
                session.recovery_initial_trusted_bytes)) {
        return std::nullopt;
    }
    return session.recovery_initial_trusted_bytes +
        static_cast<std::int64_t>(
            flow_snapshot.published_data_bytes);
}

[[nodiscard]] std::error_code invoke_progress(
    core::SessionState& session,
    flow::PacketProducer& packet_producer,
    const http::HttpSessionSnapshot& http_snapshot)
        noexcept {
    if (!session.progress_callback) {
        return {};
    }

    const auto telemetry_snapshot =
        session.telemetry_session_.current_snapshot();
    const auto flow_snapshot =
        packet_producer.snapshot();
    ProgressSnapshotSources sources{};
    sources.total_bytes = session.total_size;
    sources.recovery_trusted_bytes =
        session.recovery_initial_trusted_bytes;
    sources.persisted_bytes =
        session.persisted_bytes.load(
            std::memory_order_relaxed);
    sources.vdl_offset =
        session.vdl_offset.load(
            std::memory_order_relaxed);
    sources.packet_flow = flow_snapshot;
    sources.http = http_snapshot;
    sources.telemetry = telemetry_snapshot;
    sources.resumed = session.resumed;
    const auto built = build_progress_snapshot(sources);
    if (built.error) {
        return built.error;
    }

    try {
        session.progress_callback(built.snapshot);
    } catch (...) {
    }
    return {};
}

[[nodiscard]] std::int64_t sum_finished_bytes(const core::AtomicBlockBitmap& bitmap,
                                              const std::size_t block_size,
                                              const std::int64_t total_size) noexcept {
    std::int64_t total = 0;
    // 这个统计是“所有 finished block 的总和”，用于展示和最终结果汇总。
    // 它和 VDL 不同，不要求从文件头开始连续。
    for (std::size_t index = 0; index < bitmap.block_count(); ++index) {
        if (bitmap.load(index) != core::BlockState::finished) {
            continue;
        }

        const auto block_begin = static_cast<std::int64_t>(index * block_size);
        const auto block_end = std::min(block_begin + static_cast<std::int64_t>(block_size),
            total_size);
        total += block_end - block_begin;
    }

    return total;
}

void rebuild_bitmap_from_ranges(
    core::AtomicBlockBitmap& bitmap,
    const std::vector<range::RangeSnapshot>& ranges,
    const std::size_t block_size,
    const std::int64_t total_size) noexcept {
    // 正常退出时也做同样的投影，确保最终结果以 Persistence 线程实际推进过的
    // persisted_offset 为准，而不是以中途某个旧快照为准。
    for (const auto& range : ranges) {
        if (range.persisted_through <= range.bytes.begin) {
            continue;
        }

        bitmap.mark_finished_range(range.bytes.begin,
            range.persisted_through,
            block_size,
            total_size);
    }
}

[[nodiscard]] std::error_code submit_geometry_effect(
    const range::RangeEffect& effect,
    persistence::PersistenceThread& persistence,
    ExpectedGeometryAck& expected) noexcept {
    persistence::RangeRegistrationSubmitResult submitted;
    if (const auto* registration =
            std::get_if<range::RegisterRangeEffect>(&effect)) {
        submitted = persistence.submit_range_geometry(
            persistence::RangeGeometryCommand{*registration});
        expected.range = registration->range;
        expected.geometry_revision =
            registration->geometry_revision;
    } else if (const auto* resize =
                   std::get_if<range::ResizeRangeEffect>(&effect)) {
        submitted = persistence.submit_range_geometry(
            persistence::RangeGeometryCommand{*resize});
        expected.range = resize->range;
        expected.geometry_revision =
            resize->geometry_revision;
    } else {
        return make_error_code(DownloadErrc::internal_error);
    }
    if (submitted.error || submitted.ticket == 0) {
        return submitted.error ?
            submitted.error :
            make_error_code(DownloadErrc::internal_error);
    }
    expected.ticket = submitted.ticket;
    return {};
}

[[nodiscard]] std::error_code apply_geometry_ack(
    const persistence::RangeRegistrationAck& ack,
    std::span<ExpectedGeometryAck> expected) noexcept {
    const auto found = std::find_if(
        expected.begin(),
        expected.end(),
        [&ack](const auto& candidate) {
            return candidate.ticket == ack.ticket;
        });
    if (found == expected.end() ||
        found->received ||
        found->range != ack.range ||
        found->geometry_revision != ack.geometry_revision ||
        ack.error) {
        return ack.error ?
            ack.error :
            make_error_code(DownloadErrc::internal_error);
    }
    found->received = true;
    return {};
}

[[nodiscard]] bool all_geometry_acks_received(
    const std::span<const ExpectedGeometryAck> expected) noexcept {
    return std::all_of(
        expected.begin(),
        expected.end(),
        [](const auto& ack) {
            return ack.received;
        });
}

[[nodiscard]] std::error_code wait_for_geometry_acks(
    persistence::PersistenceThread& persistence,
    std::vector<ExpectedGeometryAck>& expected) noexcept {
    while (!all_geometry_acks_received(expected)) {
        const auto polled =
            persistence.poll_range_geometry_ack();
        if (polled.error) {
            return polled.error;
        }
        if (polled.ack.has_value()) {
            if (const auto error = apply_geometry_ack(
                    *polled.ack,
                    expected);
                error) {
                return error;
            }
            continue;
        }
        if (const auto error = persistence.error(); error) {
            return error;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }
    return {};
}

[[nodiscard]] std::error_code drain_geometry_acks(
    persistence::PersistenceThread& persistence,
    std::vector<PendingLease>& pending_leases)
        noexcept {
    while (true) {
        const auto polled =
            persistence.poll_range_geometry_ack();
        if (polled.error) {
            return polled.error;
        }
        if (!polled.ack.has_value()) {
            return {};
        }

        bool matched = false;
        for (auto& pending : pending_leases) {
            auto expected = std::span<ExpectedGeometryAck>(
                pending.acks.data(),
                pending.ack_count);
            const auto found = std::find_if(
                expected.begin(),
                expected.end(),
                [&polled](const auto& candidate) {
                    return candidate.ticket ==
                        polled.ack->ticket;
                });
            if (found == expected.end()) {
                continue;
            }
            matched = true;
            if (const auto error =
                    apply_geometry_ack(
                        *polled.ack,
                        expected);
                error) {
                return error;
            }
            break;
        }
        if (!matched) {
            return make_error_code(
                DownloadErrc::internal_error);
        }
    }
}

[[nodiscard]] std::error_code publish_range_complete(
    flow::PacketProducer& producer,
    const range::PublishRangeCompleteEffect& effect) noexcept {
    if (effect.expected_end <= 0) {
        return std::make_error_code(std::errc::invalid_argument);
    }
    const auto published = producer.publish({
        flow::ControlPacketKind::range_complete,
        effect.completion,
        effect.expected_end
    });
    if (published.code != flow::PacketPublishCode::published) {
        return published.error ?
            published.error :
            make_error_code(DownloadErrc::internal_error);
    }
    return {};
}

[[nodiscard]] std::error_code apply_range_effects(
    const range::ApplyResult& applied,
    range::RangeLifecycle& lifecycle,
    flow::PacketProducer& producer) noexcept {
    if (applied.error) {
        return applied.error;
    }
    for (std::size_t index = 0;
         index < applied.effects.size;
         ++index) {
        const auto* completion =
            std::get_if<range::PublishRangeCompleteEffect>(
                &applied.effects.values[index]);
        if (completion == nullptr) {
            return make_error_code(
                DownloadErrc::internal_error);
        }
        const auto error =
            publish_range_complete(
                producer,
                *completion);
        if (error) {
            const auto failed = lifecycle.apply(
                range::EffectApplicationFailed{error});
            return failed.error ? failed.error : error;
        }
    }
    return {};
}

[[nodiscard]] std::error_code apply_http_event(
    const http::HttpTransferEvent& event,
    range::RangeLifecycle& lifecycle,
    flow::PacketProducer& producer,
    std::vector<ActiveTransfer>& active) noexcept {
    http::TransferToken token;
    range::ApplyResult applied;
    std::error_code transfer_error;
    if (const auto* succeeded =
            std::get_if<http::HttpLeaseSucceeded>(
                &event)) {
        token = succeeded->token;
        applied = lifecycle.apply(
            range::LeaseSucceeded{
                succeeded->lease,
                succeeded->received_through
            });
    } else {
        const auto& failed =
            std::get<http::HttpLeaseFailed>(event);
        token = failed.token;
        transfer_error = failed.failure.error
            ? failed.failure.error
            : make_error_code(
                DownloadErrc::http_transfer_failed);
        applied = lifecycle.apply(
            range::LeaseFailed{
                failed.lease,
                failed.accepted_through,
                transfer_error
            });
    }

    const auto found = std::find_if(
        active.begin(),
        active.end(),
        [&token](const ActiveTransfer& current) {
            return current.token == token;
        });
    if (found == active.end()) {
        return make_error_code(
            DownloadErrc::internal_error);
    }
    const auto event_lease =
        std::visit(
            [](const auto& current) {
                return current.lease;
            },
            event);
    if (event_lease != found->lease.id ||
        token.lease != found->lease.id) {
        return make_error_code(
            DownloadErrc::internal_error);
    }
    active.erase(found);

    const auto effect_error =
        apply_range_effects(
            applied,
            lifecycle,
            producer);
    if (effect_error) {
        return effect_error;
    }
    return transfer_error;
}

[[nodiscard]] std::error_code feed_gap_pauses(
    http::HttpTransferSession& http_session,
    range::RangeLifecycle& lifecycle,
    const std::vector<ActiveTransfer>& active)
        noexcept {
    const auto snapshot = lifecycle.snapshot();
    if (snapshot.error) {
        return snapshot.error;
    }
    for (const auto& transfer : active) {
        const auto found = std::find_if(
            snapshot.value.ranges.begin(),
            snapshot.value.ranges.end(),
            [&transfer](
                const range::RangeSnapshot& current) {
                return current.id ==
                    transfer.lease.id.range;
            });
        if (found == snapshot.value.ranges.end()) {
            return make_error_code(
                DownloadErrc::internal_error);
        }
        const auto error =
            http_session.set_gap_paused(
                transfer.token,
                found->gap_blocked);
        if (error) {
            return error;
        }
    }
    return {};
}

[[nodiscard]] std::error_code drain_http_events(
    http::HttpTransferSession& http_session,
    range::RangeLifecycle& lifecycle,
    flow::PacketProducer& producer,
    std::vector<ActiveTransfer>& active)
        noexcept {
    while (true) {
        const auto polled =
            http_session.poll(
                std::chrono::milliseconds(0));
        if (polled.code ==
            http::HttpPollCode::event) {
            if (!polled.event.has_value()) {
                return make_error_code(
                    DownloadErrc::internal_error);
            }
            const auto error = apply_http_event(
                *polled.event,
                lifecycle,
                producer,
                active);
            if (error) {
                return error;
            }
            continue;
        }
        if (polled.code ==
                http::HttpPollCode::idle ||
            polled.code ==
                http::HttpPollCode::timed_out) {
            return {};
        }
        return polled.error
            ? polled.error
            : make_error_code(
                DownloadErrc::http_transfer_failed);
    }
}

[[nodiscard]] std::error_code stop_http_session(
    http::HttpTransferSession& http_session,
    range::RangeLifecycle& lifecycle,
    flow::PacketProducer& producer,
    std::vector<ActiveTransfer>& active,
    const bool upstream_failed) noexcept {
    const auto cancel_error =
        http_session.cancel(
            upstream_failed
                ? http::HttpCancelRequest{
                    http::HttpCancelKind::
                        upstream_failed,
                    producer.snapshot().error
                }
                : http::HttpCancelRequest{
                    http::HttpCancelKind::
                        task_cancelled,
                    {}
                });
    std::error_code first_error =
        cancel_error;
    while (true) {
        const auto polled =
            http_session.poll(
                std::chrono::milliseconds(0));
        if (polled.code ==
            http::HttpPollCode::event) {
            if (!polled.event.has_value()) {
                if (!first_error) {
                    first_error = make_error_code(
                        DownloadErrc::internal_error);
                }
                break;
            }
            const auto applied = apply_http_event(
                *polled.event,
                lifecycle,
                producer,
                active);
            if (!first_error && applied) {
                first_error = applied;
            }
            continue;
        }
        if (polled.code ==
                http::HttpPollCode::idle ||
            polled.code ==
                http::HttpPollCode::failed) {
            break;
        }
        if (!first_error) {
            first_error = polled.error
                ? polled.error
                : make_error_code(
                    DownloadErrc::internal_error);
        }
        break;
    }
    const auto close_error =
        http_session.close();
    if (!first_error && close_error) {
        first_error = close_error;
    }
    return first_error;
}

class PersistencePhase {
public:
    PersistencePhase(
        flow::PacketProducer& producer,
        persistence::PersistenceThread& persistence) noexcept
        : producer_(producer),
          persistence_(persistence) {}

    ~PersistencePhase() {
        if (armed_) {
            static_cast<void>(finish(make_error_code(
                DownloadErrc::internal_error)));
        }
    }

    void start() {
        persistence_.start();
        armed_ = true;
    }

    [[nodiscard]] std::error_code finish(
        std::error_code first_error) noexcept {
        if (!armed_) {
            return first_error;
        }
        armed_ = false;
        const auto close_error = producer_.close();
        if (!first_error && close_error) {
            first_error = close_error;
        }
        persistence_.join();
        if (!first_error) {
            first_error = persistence_.error();
        }
        return first_error;
    }

private:
    flow::PacketProducer& producer_;
    persistence::PersistenceThread& persistence_;
    bool armed_ = false;
};

[[nodiscard]] PerformanceSummary build_performance_summary(const core::SessionState& session,
                                                           const Clock::time_point now) noexcept {
    return session.telemetry_session_.final_summary(now);
}

} // namespace

DownloadResult detail::run_download(
    const DownloadRequest& request,
    const DownloadEngineDependencies& dependencies) noexcept {
    DownloadResult result{};
    result.temporary_path = core::make_temporary_path(request.output_path);
    result.metadata_path = core::make_metadata_path(request.output_path);

    try {
        // 第一层先做最基础的请求合法性校验，避免后面创建网络和文件资源后再回滚。
        if (request.url.empty() || request.output_path.empty()) {
            result.error = make_error_code(DownloadErrc::invalid_request);
            return result;
        }

        const auto validated_policy =
            validate_download_options(request.options);
        if (!validated_policy.ok()) {
            result.error = validated_policy.failure.error;
            return result;
        }

        const auto run_started = Clock::now();
        std::unique_ptr<http::HttpTransferPort>
            http_transfer_port;
        if (dependencies.create_http_transfer_port == nullptr) {
            result.error = make_error_code(
                DownloadErrc::internal_error);
            return result;
        }
        const auto http_port_error =
            dependencies.create_http_transfer_port(
                http_transfer_port);
        if (http_port_error ||
            http_transfer_port == nullptr) {
            result.error = http_port_error
                ? http_port_error
                : make_error_code(
                    DownloadErrc::http_init_failed);
            return result;
        }

        const auto probe_result =
            http_transfer_port->probe(
                {request.url});
        if (!probe_result.ok()) {
            result.error = probe_result.failure.error
                ? probe_result.failure.error
                : make_error_code(
                    DownloadErrc::http_probe_failed);
            return result;
        }
        const auto& remote_facts =
            *probe_result.facts;
        const auto effective_policy_result = bind_remote_facts(
            *validated_policy.value,
            RemoteObjectFacts{
                remote_facts.total_size,
                remote_facts.accept_ranges
            });
        if (!effective_policy_result.ok()) {
            result.error = effective_policy_result.failure.error;
            return result;
        }
        core::SessionState session(
            std::move(*effective_policy_result.value));
        session.paths.output_path = request.output_path;
        session.paths.temporary_path = core::make_temporary_path(request.output_path);
        session.paths.metadata_path = core::make_metadata_path(request.output_path);
        session.url = request.url;
        session.etag = remote_facts.etag;
        session.last_modified =
            remote_facts.last_modified;
        session.progress_callback = request.progress_callback;
        session.task_started_at = run_started;
        session.telemetry_session_.record_task_started(run_started);

        recovery::RecoveryOpenRequest recovery_request{};
        recovery_request.paths = session.paths;
        recovery_request.remote.url = request.url;
        recovery_request.remote.total_size =
            remote_facts.total_size;
        recovery_request.remote.accept_ranges =
            remote_facts.accept_ranges;
        recovery_request.remote.etag =
            remote_facts.etag;
        recovery_request.remote.last_modified =
            remote_facts.last_modified;
        recovery_request.policy =
            session.effective_policy.recovery_identity();
        recovery_request.overwrite_existing =
            session.effective_policy.persistence().
                overwrite_existing;
        auto recovery_open =
            recovery::RecoveryCheckpoint::open(
                recovery_request);
        if (recovery_open.error ||
            recovery_open.checkpoint == nullptr) {
            result.error = recovery_open.error ?
                recovery_open.error :
                make_error_code(
                    DownloadErrc::internal_error);
            result.performance =
                build_performance_summary(
                    session,
                    Clock::now());
            return result;
        }
        auto recovery_checkpoint =
            std::move(recovery_open.checkpoint);
        core::AtomicBlockBitmap bitmap(
            recovery_open.restored.bitmap_states.size());
        bitmap.restore(
            recovery_open.restored.bitmap_states);
        session.resumed =
            recovery_open.restored.disposition !=
            recovery::RecoveryDisposition::fresh;
        const auto finished_bytes =
            recovery_open.restored.trusted_bytes;
        const auto safe_vdl =
            recovery_open.restored.safe_vdl;
        session.recovery_initial_trusted_bytes = finished_bytes;
        session.persisted_bytes.store(finished_bytes, std::memory_order_relaxed);
        session.vdl_offset.store(safe_vdl, std::memory_order_relaxed);

        if (safe_vdl >= session.total_size) {
            // 恢复后如果发现整个文件其实已经完整可靠，就直接 finalize，
            // 不再走任何网络或持久化线程。
            const auto finalize_result =
                recovery_checkpoint->finalize();
            if (finalize_result.error) {
                result.error = finalize_result.error;
                return result;
            }
            result.total_bytes = session.total_size;
            result.downloaded_bytes = finished_bytes;
            result.persisted_bytes = finished_bytes;
            result.completed_ranges =
                recovery_open.restored.completed_ranges;
            result.resumed = session.resumed;
            const auto completed_at = Clock::now();
            session.telemetry_session_.
                record_task_completed(completed_at);
            result.performance =
                build_performance_summary(
                    session,
                    completed_at);
            return result;
        }

        // 调度器基于当前 bitmap 生成“还需要下载哪些区间”。
        // 对全新任务来说是整文件切片；对恢复任务来说则只会覆盖未完成区域。
        RangeScheduler scheduler(
            session.effective_policy.scheduling(),
            session.total_size);
        const auto initial_plan = scheduler.plan_initial(bitmap);
        if (initial_plan.error || initial_plan.ranges.empty()) {
            recovery_checkpoint->
                close_preserving_artifacts();
            result.error = initial_plan.error ?
                initial_plan.error :
                make_error_code(DownloadErrc::internal_error);
            result.performance =
                build_performance_summary(session, Clock::now());
            return result;
        }
        auto lifecycle_creation = range::RangeLifecycle::create(
            session.total_size,
            session.effective_policy.scheduling(),
            initial_plan.ranges);
        if (lifecycle_creation.error ||
            lifecycle_creation.value == nullptr) {
            recovery_checkpoint->
                close_preserving_artifacts();
            result.error = lifecycle_creation.error ?
                lifecycle_creation.error :
                make_error_code(DownloadErrc::internal_error);
            result.performance =
                build_performance_summary(session, Clock::now());
            return result;
        }
        auto lifecycle = std::move(lifecycle_creation.value);
        std::unique_ptr<flow::PacketFlow> packet_flow;
        const auto packet_flow_error = flow::PacketFlow::create(
            session.effective_policy.flow_control(),
            session.telemetry_session_,
            packet_flow);
        if (packet_flow_error) {
            recovery_checkpoint->
                close_preserving_artifacts();
            result.error = packet_flow_error;
            result.performance =
                build_performance_summary(session, Clock::now());
            return result;
        }
        BS::thread_pool<> workers(
            session.effective_policy.scheduling().connection_limit);
        persistence::PersistenceThread persistence(session,
            session.effective_policy.persistence(),
            packet_flow->consumer(),
            bitmap,
            *recovery_checkpoint,
            workers,
            initial_plan.ranges.size());
        const auto connection_limit =
            session.effective_policy.scheduling().
                connection_limit;
        std::vector<ExpectedGeometryAck> initial_geometry_acks;
        initial_geometry_acks.reserve(
            lifecycle_creation.effects.values.size());
        std::vector<PendingLease> pending_leases;
        std::vector<ActiveTransfer> active_transfers;
        pending_leases.reserve(connection_limit);
        active_transfers.reserve(connection_limit);
        PersistencePhase persistence_phase(
            packet_flow->producer(),
            persistence);
        persistence_phase.start();
        if (dependencies.post_persistence_start_check !=
            nullptr) {
            const auto start_error =
                dependencies.post_persistence_start_check();
            if (start_error) {
                result.error =
                    persistence_phase.finish(start_error);
                recovery_checkpoint->
                    close_preserving_artifacts();
                result.performance =
                    build_performance_summary(
                        session,
                        Clock::now());
                return result;
            }
        }

        std::error_code initial_geometry_error;
        for (const auto& effect :
             lifecycle_creation.effects.values) {
            ExpectedGeometryAck expected;
            initial_geometry_error =
                submit_geometry_effect(
                    effect,
                    persistence,
                    expected);
            if (initial_geometry_error) {
                break;
            }
            initial_geometry_acks.push_back(expected);
        }
        if (!initial_geometry_error) {
            initial_geometry_error = wait_for_geometry_acks(
                persistence,
                initial_geometry_acks);
        }
        if (initial_geometry_error) {
            const auto applied = lifecycle->apply(
                range::EffectApplicationFailed{
                    initial_geometry_error
                });
            const auto first_error = applied.error ?
                applied.error :
                initial_geometry_error;
            const auto phase_error =
                persistence_phase.finish(first_error);
            recovery_checkpoint->
                close_preserving_artifacts();
            result.error = phase_error;
            result.performance =
                build_performance_summary(
                    session,
                    Clock::now());
            return result;
        }

        auto opened_http_session =
            http_transfer_port->open_session(
                {
                    session.url,
                    session.total_size,
                    connection_limit
                },
                packet_flow->producer(),
                session.telemetry_session_);
        if (opened_http_session.failure.error ||
            opened_http_session.session == nullptr) {
            const auto first_error =
                opened_http_session.failure.error
                ? opened_http_session.failure.error
                : make_error_code(
                    DownloadErrc::http_init_failed);
            const auto phase_error =
                persistence_phase.finish(first_error);
            recovery_checkpoint->
                close_preserving_artifacts();
            result.error = phase_error;
            result.performance =
                build_performance_summary(
                    session,
                    Clock::now());
            return result;
        }
        auto http_session =
            std::move(opened_http_session.session);
        std::error_code failure;

        auto emit_progress_at = Clock::now();
        while (!failure &&
               !session.stop_requested.load(
                   std::memory_order_acquire)) {
            failure = drain_http_events(
                *http_session,
                *lifecycle,
                packet_flow->producer(),
                active_transfers);
            if (failure) {
                session.stop_requested.store(
                    true,
                    std::memory_order_release);
                break;
            }

            if (const auto geometry_error =
                    drain_geometry_acks(
                        persistence,
                        pending_leases);
                geometry_error) {
                const auto applied = lifecycle->apply(
                    range::EffectApplicationFailed{
                        geometry_error
                    });
                failure = applied.error ?
                    applied.error :
                    geometry_error;
                session.stop_requested.store(
                    true,
                    std::memory_order_release);
                break;
            }

            const auto facts =
                lifecycle->drain_persistence_facts();
            if (facts.error) {
                failure = facts.error;
                session.stop_requested.store(
                    true,
                    std::memory_order_release);
                break;
            }

            if (const auto persistence_error =
                    persistence.error();
                persistence_error) {
                const auto applied = lifecycle->apply(
                    range::PersistenceFailed{
                        std::nullopt,
                        persistence_error
                    });
                failure = applied.error ?
                    applied.error :
                    persistence_error;
                session.stop_requested.store(true, std::memory_order_release);
                break;
            }

            if (const auto gap_error =
                    feed_gap_pauses(
                        *http_session,
                        *lifecycle,
                        active_transfers);
                gap_error) {
                failure = gap_error;
                session.stop_requested.store(
                    true,
                    std::memory_order_release);
                break;
            }

            for (std::size_t index = 0;
                 index < pending_leases.size();) {
                auto& pending = pending_leases[index];
                const auto expected =
                    std::span<const ExpectedGeometryAck>(
                        pending.acks.data(),
                        pending.ack_count);
                if (!all_geometry_acks_received(expected)) {
                    ++index;
                    continue;
                }
                const auto started =
                    http_session->start(pending.lease);
                if (started.code ==
                    http::HttpStartCode::no_capacity) {
                    ++index;
                    continue;
                }
                const auto lease = pending.lease;
                pending_leases.erase(
                    pending_leases.begin() +
                    static_cast<std::ptrdiff_t>(index));
                if (started.code !=
                        http::HttpStartCode::started ||
                    !started.token.has_value()) {
                    const auto start_error =
                        started.failure.error
                        ? started.failure.error
                        : make_error_code(
                            DownloadErrc::
                                http_transfer_failed);
                    const auto applied = lifecycle->apply(
                        range::LeaseFailed{
                            lease.id,
                            lease.bytes.begin,
                            start_error
                        });
                    failure = applied.error
                        ? applied.error
                        : start_error;
                    session.stop_requested.store(
                        true,
                        std::memory_order_release);
                    break;
                }
                active_transfers.push_back({
                    *started.token,
                    lease
                });
            }
            if (failure) {
                break;
            }

            while (active_transfers.size() +
                       pending_leases.size() <
                   connection_limit) {
                const auto http_snapshot =
                    http_session->snapshot();
                if (http_snapshot.error ||
                    http_snapshot.pending_events != 0 ||
                    http_snapshot.available_slots <=
                        pending_leases.size()) {
                    if (http_snapshot.error) {
                        failure = http_snapshot.error;
                    }
                    break;
                }

                const auto acquired =
                    lifecycle->acquire();
                if (acquired.error) {
                    failure = acquired.error;
                    session.stop_requested.store(
                        true,
                        std::memory_order_release);
                    break;
                }
                if (!acquired.lease.has_value()) {
                    break;
                }

                const auto lease = *acquired.lease;
                if (acquired.effects.size > 0) {
                    PendingLease pending{};
                    pending.lease = lease;
                    pending.ack_count =
                        acquired.effects.size;
                    for (std::size_t index = 0;
                         index < acquired.effects.size;
                         ++index) {
                        const auto submit_error =
                            submit_geometry_effect(
                                acquired.effects.values[index],
                                persistence,
                                pending.acks[index]);
                        if (submit_error) {
                            const auto applied =
                                lifecycle->apply(
                                    range::EffectApplicationFailed{
                                        submit_error
                                    });
                            failure = applied.error ?
                                applied.error :
                                submit_error;
                            session.stop_requested.store(
                                true,
                                std::memory_order_release);
                            break;
                        }
                    }
                    if (failure) {
                        break;
                    }
                    pending_leases.push_back(
                        std::move(pending));
                    continue;
                }

                const auto started =
                    http_session->start(lease);
                if (started.code ==
                    http::HttpStartCode::no_capacity) {
                    PendingLease pending{};
                    pending.lease = lease;
                    pending_leases.push_back(
                        std::move(pending));
                    break;
                }
                if (started.code !=
                        http::HttpStartCode::started ||
                    !started.token.has_value()) {
                    const auto start_error =
                        started.failure.error
                        ? started.failure.error
                        : make_error_code(
                            DownloadErrc::
                                http_transfer_failed);
                    const auto applied = lifecycle->apply(
                        range::LeaseFailed{
                            lease.id,
                            lease.bytes.begin,
                            start_error
                        });
                    failure = applied.error
                        ? applied.error
                        : start_error;
                    session.stop_requested.store(
                        true,
                        std::memory_order_release);
                    break;
                }
                active_transfers.push_back({
                    *started.token,
                    lease
                });
            }

            if (failure) {
                break;
            }

            if (!active_transfers.empty()) {
                const auto polled =
                    http_session->poll(
                        std::chrono::milliseconds(100));
                if (polled.code ==
                    http::HttpPollCode::event) {
                    if (!polled.event.has_value()) {
                        failure = make_error_code(
                            DownloadErrc::internal_error);
                    } else {
                        failure = apply_http_event(
                            *polled.event,
                            *lifecycle,
                            packet_flow->producer(),
                            active_transfers);
                    }
                } else if (polled.code !=
                               http::HttpPollCode::
                                   timed_out &&
                           polled.code !=
                               http::HttpPollCode::idle) {
                    failure = polled.error
                        ? polled.error
                        : make_error_code(
                            DownloadErrc::
                                http_transfer_failed);
                }
                if (failure) {
                    session.stop_requested.store(
                        true,
                        std::memory_order_release);
                    break;
                }
            }

            const auto now = Clock::now();
            if (now >= emit_progress_at) {
                if (const auto progress_error =
                        invoke_progress(
                            session,
                            packet_flow->producer(),
                            http_session->snapshot());
                    progress_error) {
                    failure = progress_error;
                    session.stop_requested.store(
                        true,
                        std::memory_order_release);
                    break;
                }
                emit_progress_at = now + std::chrono::milliseconds(200);
            }

            if (active_transfers.empty() &&
                pending_leases.empty()) {
                const auto lifecycle_snapshot =
                    lifecycle->snapshot();
                if (lifecycle_snapshot.error) {
                    failure = lifecycle_snapshot.error;
                    session.stop_requested.store(
                        true,
                        std::memory_order_release);
                    break;
                }
                if (!lifecycle_snapshot.value.has_schedulable_work) {
                    break;
                }
            }
        }

        if (failure ||
            session.stop_requested.load(
                std::memory_order_acquire)) {
            const auto cancelled =
                lifecycle->apply(range::CancelRequested{});
            if (!failure && cancelled.error) {
                failure = cancelled.error;
            }
        }

        session.stop_requested.store(
            true,
            std::memory_order_release);
        if (failure) {
            const auto flow_snapshot =
                packet_flow->producer().snapshot();
            const auto stop_error =
                stop_http_session(
                    *http_session,
                    *lifecycle,
                    packet_flow->producer(),
                    active_transfers,
                    flow_snapshot.state ==
                        flow::PacketFlowState::failed);
            static_cast<void>(stop_error);
        } else {
            const auto close_error =
                http_session->close();
            if (close_error) {
                failure = close_error;
            }
        }
        const auto final_http_snapshot =
            http_session->snapshot();
        http_session.reset();
        failure = persistence_phase.finish(failure);
        const auto final_facts =
            lifecycle->drain_persistence_facts();
        if (!failure && final_facts.error) {
            failure = final_facts.error;
        }
        if (const auto persistence_error =
                persistence.error();
            persistence_error) {
            const auto applied = lifecycle->apply(
                range::PersistenceFailed{
                    std::nullopt,
                    persistence_error
                });
            if (!failure) {
                failure = applied.error ?
                    applied.error :
                    persistence_error;
            }
        }
        const auto final_lifecycle_snapshot =
            lifecycle->snapshot();
        if (!failure && final_lifecycle_snapshot.error) {
            failure = final_lifecycle_snapshot.error;
        }
        if (!failure &&
            !final_lifecycle_snapshot.value.all_finished) {
            failure = make_error_code(
                DownloadErrc::internal_error);
        }

        // Persistence 线程拥有每个 range 的真正写盘前沿；停下来之后再做一次 bitmap
        // 重建，可以把最终结果对齐到落盘状态。
        rebuild_bitmap_from_ranges(bitmap,
            final_lifecycle_snapshot.value.ranges,
            session.effective_policy.persistence().block_bytes,
            session.total_size);

        if (const auto progress_error =
                invoke_progress(
                    session,
                    packet_flow->producer(),
                    final_http_snapshot);
            !failure && progress_error) {
            failure = progress_error;
        }

        if (!failure) {
            const auto completed_bytes =
                bitmap.contiguous_finished_bytes(
                    session.effective_policy.
                        persistence().block_bytes,
                    session.total_size);
            if (completed_bytes < session.total_size) {
                failure = make_error_code(
                    DownloadErrc::
                        http_transfer_failed);
            } else {
                failure =
                    recovery_checkpoint->finalize().error;
            }
        }
        if (failure) {
            recovery_checkpoint->
                close_preserving_artifacts();
        }

        // DownloadResult 主要面向调用方总结最终状态，因此在这里统一从 session 和
        // bitmap 回填一次，避免中途多个分支各自维护结果对象。
        const auto downloaded = merged_downloaded_bytes(
            session,
            packet_flow->producer().snapshot());
        if (!downloaded && !failure) {
            failure = make_error_code(DownloadErrc::internal_error);
        }
        result.error = failure;
        result.total_bytes = session.total_size;
        result.downloaded_bytes = downloaded.value_or(
            std::numeric_limits<std::int64_t>::max());
        result.persisted_bytes = sum_finished_bytes(bitmap,
            session.effective_policy.persistence().block_bytes,
            session.total_size);
        result.completed_ranges =
            final_lifecycle_snapshot.value.finished_ranges;
        result.resumed = session.resumed;
        result.temporary_path = session.paths.temporary_path;
        result.metadata_path = session.paths.metadata_path;
        if (!failure) {
            const auto completed_at = Clock::now();
            session.telemetry_session_.
                record_task_completed(completed_at);
            result.performance =
                build_performance_summary(
                    session,
                    completed_at);
        } else {
            const auto failed_at = Clock::now();
            result.performance =
                build_performance_summary(
                    session,
                    failed_at);
        }
        return result;
    } catch (...) {
        // 对外契约是不抛异常，所以任何未预期错误最终都折叠成 internal_error。
        result.error = make_error_code(DownloadErrc::internal_error);
        return result;
    }
}

DownloadResult DownloadEngine::run(
    const DownloadRequest& request) noexcept {
    return detail::run_download(
        request,
        {http::create_curl_http_transfer_port, nullptr});
}

} // namespace asyncdownload::download





