#include "recovery_checkpoint.hpp"

#include "asyncdownload/error.hpp"
#include "core/block_bitmap.hpp"
#include "metadata/metadata_store.hpp"
#include "storage/file_writer.hpp"

#include <limits>
#include <new>
#include <utility>

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
        std::error_code filesystem_error;
        const auto part_exists =
            std::filesystem::exists(
                request.paths.temporary_path,
                filesystem_error);
        if (filesystem_error) {
            result.error = make_error_code(
                DownloadErrc::open_file_failed);
            return result;
        }
        const auto metadata_exists =
            std::filesystem::exists(
                request.paths.metadata_path,
                filesystem_error);
        if (filesystem_error) {
            result.error = make_error_code(
                DownloadErrc::metadata_parse_failed);
            return result;
        }
        if (part_exists || metadata_exists) {
            result.error = internal_error();
            return result;
        }

        auto implementation =
            std::make_unique<Implementation>(request);
        const auto open_error =
            implementation->file_writer.open(
                request.paths.temporary_path,
                request.remote.total_size,
                false,
                request.overwrite_existing);
        if (open_error) {
            result.error = open_error;
            return result;
        }

        result.restored.bitmap_states.assign(
            core::required_block_count(
                request.remote.total_size,
                request.policy.block_bytes),
            std::uint8_t{0});
        result.checkpoint =
            std::unique_ptr<RecoveryCheckpoint>(
                new RecoveryCheckpoint(
                    std::move(implementation)));
    } catch (const std::bad_alloc&) {
        result.error = std::make_error_code(
            std::errc::not_enough_memory);
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

}
