#include "recovery_checkpoint.hpp"

#include "asyncdownload/error.hpp"
#include "core/block_bitmap.hpp"
#include "core/crc32.hpp"
#include "metadata/metadata_store.hpp"
#include "recovery_fault_adapter.hpp"
#include "storage/file_writer.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace asyncdownload::recovery {

class PreparedCheckpoint::Implementation {};

PreparedCheckpoint::PreparedCheckpoint(
    std::unique_ptr<Implementation>
        implementation) noexcept
    : implementation_(std::move(implementation)) {}

PreparedCheckpoint::PreparedCheckpoint(
    PreparedCheckpoint&&) noexcept = default;

PreparedCheckpoint& PreparedCheckpoint::operator=(
    PreparedCheckpoint&&) noexcept = default;

PreparedCheckpoint::~PreparedCheckpoint() = default;

class RecoveryCheckpoint::Implementation {
public:
    explicit Implementation(
        const RecoveryOpenRequest& request)
        : paths(request.paths),
          remote(request.remote),
          policy(request.policy),
          overwrite_existing(
              request.overwrite_existing),
          metadata_store(paths.metadata_path) {}

    core::SessionPaths paths;
    RemoteRecoveryIdentity remote;
    download::RecoveryIdentityPolicy policy;
    bool overwrite_existing = false;
    storage::FileWriter file_writer;
    metadata::MetadataStore metadata_store;
};

namespace {

[[nodiscard]] std::error_code internal_error() noexcept {
    return make_error_code(
        DownloadErrc::internal_error);
}

[[nodiscard]] bool request_is_valid(
    const RecoveryOpenRequest& request) noexcept {
    return
        !request.paths.output_path.empty() &&
        !request.paths.temporary_path.empty() &&
        !request.paths.metadata_path.empty() &&
        !request.remote.url.empty() &&
        request.remote.total_size > 0 &&
        request.policy.block_bytes > 0 &&
        request.policy.io_alignment_bytes > 0;
}

[[nodiscard]] bool metadata_matches(
    const core::MetadataState& state,
    const RecoveryOpenRequest& request) noexcept {
    if (state.url != request.remote.url ||
        state.output_path != request.paths.output_path ||
        state.temporary_path !=
            request.paths.temporary_path ||
        state.total_size !=
            request.remote.total_size ||
        state.block_size !=
            request.policy.block_bytes ||
        state.io_alignment !=
            request.policy.io_alignment_bytes) {
        return false;
    }
    if (!request.remote.etag.empty() &&
        !state.etag.empty() &&
        request.remote.etag != state.etag) {
        return false;
    }
    if (!request.remote.last_modified.empty() &&
        !state.last_modified.empty() &&
        request.remote.last_modified !=
            state.last_modified) {
        return false;
    }
    return true;
}

[[nodiscard]] bool metadata_proves_complete(
    const core::MetadataState& state,
    const download::RecoveryIdentityPolicy& policy)
    noexcept {
    if (state.total_size <= 0 ||
        state.vdl_offset < state.total_size) {
        return false;
    }
    const auto required_blocks =
        core::required_block_count(
            state.total_size,
            policy.block_bytes);
    return
        state.bitmap_states.size() == required_blocks &&
        std::all_of(
            state.bitmap_states.begin(),
            state.bitmap_states.end(),
            [](const std::uint8_t value) {
                return value ==
                    static_cast<std::uint8_t>(
                        core::BlockState::finished);
            });
}

void project_legacy_ranges(
    core::AtomicBlockBitmap& bitmap,
    const std::vector<core::RangeStateSnapshot>& ranges,
    const std::size_t block_size,
    const std::int64_t total_size) noexcept {
    for (const auto& range : ranges) {
        if (range.persisted_offset <=
            range.start_offset) {
            continue;
        }
        bitmap.mark_finished_range(
            range.start_offset,
            range.persisted_offset,
            block_size,
            total_size);
    }
}

[[nodiscard]] bool vdl_prefix_is_finished(
    const core::AtomicBlockBitmap& bitmap,
    const std::int64_t serialized_vdl,
    const std::size_t block_size,
    const std::int64_t total_size) noexcept {
    if (serialized_vdl <= 0) {
        return true;
    }
    const auto prefix_end =
        std::min(serialized_vdl, total_size);
    for (std::size_t index = 0;
         index < bitmap.block_count();
         ++index) {
        const auto block_begin =
            static_cast<std::int64_t>(
                index * block_size);
        if (block_begin >= prefix_end) {
            break;
        }
        if (bitmap.load(index) !=
            core::BlockState::finished) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::error_code validate_crc_samples(
    const core::MetadataState& state,
    storage::FileWriter& file_writer,
    core::AtomicBlockBitmap& bitmap) noexcept {
    const auto block_size =
        static_cast<std::int64_t>(state.block_size);
    for (std::size_t index = 0;
         index < bitmap.block_count();
         ++index) {
        if (bitmap.load(index) !=
            core::BlockState::finished) {
            continue;
        }
        const auto offset =
            static_cast<std::int64_t>(index) *
            block_size;
        if (offset < state.vdl_offset) {
            continue;
        }
        const auto sample = std::find_if(
            state.crc_samples.begin(),
            state.crc_samples.end(),
            [offset](
                const core::BlockCrcSample& candidate) {
                return candidate.offset == offset;
            });
        if (sample == state.crc_samples.end()) {
            bitmap.store(
                index,
                core::BlockState::empty);
            continue;
        }
        std::vector<std::byte> bytes;
        const auto read_error = file_writer.read(
            offset,
            sample->length,
            bytes);
        if (read_error) {
            return read_error;
        }
        if (core::crc32(bytes) != sample->crc32) {
            bitmap.store(
                index,
                core::BlockState::empty);
        }
    }
    return {};
}

[[nodiscard]] std::int64_t sum_finished_bytes(
    const core::AtomicBlockBitmap& bitmap,
    const std::size_t block_size,
    const std::int64_t total_size) noexcept {
    std::int64_t total = 0;
    for (std::size_t index = 0;
         index < bitmap.block_count();
         ++index) {
        if (bitmap.load(index) !=
            core::BlockState::finished) {
            continue;
        }
        const auto begin =
            static_cast<std::int64_t>(
                index * block_size);
        const auto end = std::min(
            begin +
                static_cast<std::int64_t>(block_size),
            total_size);
        total += end - begin;
    }
    return total;
}

}

RecoveryCheckpoint::RecoveryCheckpoint(
    std::unique_ptr<Implementation>
        implementation) noexcept
    : implementation_(std::move(implementation)) {}

RecoveryCheckpoint::~RecoveryCheckpoint() {
    close_preserving_artifacts();
}

RecoveryOpenResult RecoveryCheckpoint::open(
    const RecoveryOpenRequest& request) noexcept {
    RecoveryOpenResult result{};
    if (!request_is_valid(request)) {
        result.error = make_error_code(
            DownloadErrc::invalid_request);
        return result;
    }

    try {
        auto implementation =
            std::make_unique<Implementation>(request);
        const auto [metadata_error, loaded] =
            implementation->metadata_store.load();
        if (metadata_error) {
            result.error = metadata_error;
            return result;
        }

        const auto part_exists =
            std::filesystem::exists(
                request.paths.temporary_path);
        const auto can_resume =
            part_exists &&
            loaded.has_value() &&
            metadata_matches(*loaded, request) &&
            (request.policy.allow_sparse_resume ||
             metadata_proves_complete(
                 *loaded,
                 request.policy));
        if (!can_resume) {
#if defined(ASYNCDOWNLOAD_RECOVERY_FAULT_TEST)
            if (detail::recovery_fault_plan().
                    fail_next_metadata_invalidate.exchange(
                        false,
                        std::memory_order_acq_rel)) {
                result.error = make_error_code(
                    DownloadErrc::
                        metadata_save_failed);
                return result;
            }
#endif
            const auto remove_error =
                implementation->metadata_store.remove();
            if (remove_error) {
                result.error = remove_error;
                return result;
            }
            result.stale_cleanup.status =
                loaded.has_value() ?
                    CleanupStatus::removed :
                    CleanupStatus::not_found;
#if defined(ASYNCDOWNLOAD_RECOVERY_FAULT_TEST)
            if (detail::recovery_fault_plan().
                    stop_after_metadata_invalidation.
                        exchange(
                            false,
                            std::memory_order_acq_rel)) {
                result.error = internal_error();
                return result;
            }
#endif
        }
        const auto open_error =
            implementation->file_writer.open(
                request.paths.temporary_path,
                request.remote.total_size,
                can_resume,
                can_resume ?
                    request.overwrite_existing :
                    true);
        if (open_error) {
            result.error = open_error;
            return result;
        }
#if defined(ASYNCDOWNLOAD_RECOVERY_FAULT_TEST)
        if (!can_resume &&
            detail::recovery_fault_plan().
                stop_after_part_reset.exchange(
                    false,
                    std::memory_order_acq_rel)) {
            result.error = internal_error();
            return result;
        }
#endif

        const auto block_count =
            core::required_block_count(
                request.remote.total_size,
                request.policy.block_bytes);
        core::AtomicBlockBitmap bitmap(block_count);
        if (can_resume) {
            bitmap.restore(loaded->bitmap_states);
            bitmap.reset_transient_states();
            project_legacy_ranges(
                bitmap,
                loaded->ranges,
                request.policy.block_bytes,
                request.remote.total_size);
            if (!vdl_prefix_is_finished(
                    bitmap,
                    loaded->vdl_offset,
                    request.policy.block_bytes,
                    request.remote.total_size)) {
                result.error = make_error_code(
                    DownloadErrc::
                        metadata_parse_failed);
                return result;
            }
            const auto validation_error =
                validate_crc_samples(
                    *loaded,
                    implementation->file_writer,
                    bitmap);
            if (validation_error) {
                result.error = validation_error;
                return result;
            }
            result.restored.bitmap_states =
                bitmap.snapshot();
            result.restored.trusted_bytes =
                sum_finished_bytes(
                    bitmap,
                    request.policy.block_bytes,
                    request.remote.total_size);
            result.restored.safe_vdl =
                bitmap.contiguous_finished_bytes(
                    request.policy.block_bytes,
                    request.remote.total_size);
            if (result.restored.safe_vdl >=
                request.remote.total_size) {
                result.restored.disposition =
                    RecoveryDisposition::complete;
                result.restored.completed_ranges =
                    bitmap.block_count();
            } else {
                result.restored.disposition =
                    RecoveryDisposition::resumed;
            }
        } else {
            result.restored.bitmap_states =
                bitmap.snapshot();
        }
        result.checkpoint =
            std::unique_ptr<RecoveryCheckpoint>(
                new RecoveryCheckpoint(
                    std::move(implementation)));
    } catch (const std::bad_alloc&) {
        result.error = internal_error();
    } catch (...) {
        result.error = internal_error();
    }

    if (result.error) {
        result.checkpoint.reset();
    }
    return result;
}

std::error_code RecoveryCheckpoint::write(
    const std::int64_t offset,
    const std::span<const std::uint8_t> bytes) noexcept {
    if (implementation_ == nullptr ||
        offset < 0 ||
        bytes.size() >
            static_cast<std::size_t>(
                std::numeric_limits<
                    std::int64_t>::max())) {
        return std::make_error_code(
            std::errc::invalid_argument);
    }
    const auto length =
        static_cast<std::int64_t>(bytes.size());
    if (offset >
        implementation_->remote.total_size - length) {
        return std::make_error_code(
            std::errc::invalid_argument);
    }
    return implementation_->file_writer.write(
        offset,
        bytes);
}

PrepareCheckpointResult RecoveryCheckpoint::prepare(
    const std::span<const std::uint8_t> bitmap_states,
    const std::span<const RecoveryRangeFact> ranges)
    noexcept {
    static_cast<void>(bitmap_states);
    static_cast<void>(ranges);
    return {nullptr, internal_error()};
}

CheckpointCommitResult RecoveryCheckpoint::commit(
    std::unique_ptr<PreparedCheckpoint> checkpoint)
    noexcept {
    static_cast<void>(checkpoint);
    return {0, 0, internal_error()};
}

FinalizeResult RecoveryCheckpoint::finalize() noexcept {
    return {
        false,
        {},
        internal_error()
    };
}

void RecoveryCheckpoint::close_preserving_artifacts()
    noexcept {
    if (implementation_ != nullptr) {
        implementation_->file_writer.close();
    }
}

const core::SessionPaths& RecoveryCheckpoint::paths()
    const noexcept {
    return implementation_->paths;
}

CleanupResult RecoveryCheckpoint::discard_candidate(
    const DiscardReason reason) noexcept {
    static_cast<void>(reason);
    return {
        CleanupStatus::failed,
        internal_error()
    };
}

storage::FileWriter&
RecoveryCheckpoint::legacy_file_writer() noexcept {
    return implementation_->file_writer;
}

metadata::MetadataStore&
RecoveryCheckpoint::legacy_metadata_store() noexcept {
    return implementation_->metadata_store;
}

std::error_code RecoveryCheckpoint::legacy_flush_part()
    noexcept {
    return implementation_->file_writer.flush();
}

std::error_code RecoveryCheckpoint::legacy_read_part(
    const std::int64_t offset,
    const std::size_t length,
    std::vector<std::byte>& output) noexcept {
    return implementation_->file_writer.read(
        offset,
        length,
        output);
}

std::error_code RecoveryCheckpoint::legacy_save_metadata(
    const core::MetadataState& state) noexcept {
    return implementation_->metadata_store.save(state);
}

}
