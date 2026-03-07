#include "file_writer.hpp"

#include "asyncdownload/error.hpp"

#include <fstream>

#ifdef _WIN32
#include <Windows.h>
#include <WinIoCtl.h>
#else
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace asyncdownload::storage {
namespace {

#ifdef _WIN32
[[nodiscard]] bool set_file_position(HANDLE handle, const std::int64_t offset) noexcept {
    LARGE_INTEGER position{};
    position.QuadPart = offset;
    return SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) == TRUE;
}

[[nodiscard]] bool extend_file_size(HANDLE handle, const std::int64_t total_size) noexcept {
    // 预分配的基础动作都是先把逻辑文件长度扩到目标大小。
    return set_file_position(handle, total_size) && SetEndOfFile(handle) == TRUE;
}

[[nodiscard]] bool try_fast_preallocate(HANDLE handle, const std::int64_t total_size) noexcept {
    // SetFileValidData 是最快路径：它直接把文件标记为具有目标长度的有效数据区，
    // 省掉逐块清零成本，但要求较高权限。
    if (!extend_file_size(handle, total_size)) {
        return false;
    }

    if (SetFileValidData(handle, total_size) != TRUE) {
        return false;
    }

    return set_file_position(handle, 0);
}

[[nodiscard]] bool try_sparse_preallocate(HANDLE handle, const std::int64_t total_size) noexcept {
    // sparse 路径比普通扩容更接近“先拿到逻辑空间，实际物理块按需分配”的语义，
    // 对断点续传场景很合适。
    DWORD bytes_returned = 0;
    if (DeviceIoControl(handle,
            FSCTL_SET_SPARSE,
            nullptr,
            0,
            nullptr,
            0,
            &bytes_returned,
            nullptr) != TRUE) {
        return false;
    }

    if (!extend_file_size(handle, total_size)) {
        return false;
    }

    return set_file_position(handle, 0);
}

[[nodiscard]] bool try_basic_preallocate(HANDLE handle, const std::int64_t total_size) noexcept {
    // 最后的兜底只是把文件长度扩出去，不再追求更激进的预分配能力。
    if (!extend_file_size(handle, total_size)) {
        return false;
    }

    return set_file_position(handle, 0);
}
#endif

} // namespace

FileWriter::~FileWriter() {
    close();
}

std::error_code FileWriter::open(const std::filesystem::path& path,
                                 const std::int64_t total_size,
                                 const bool resume_existing,
                                 const bool overwrite_existing) noexcept {
    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return make_error_code(DownloadErrc::create_directory_failed);
        }
    }

    // FileWriter 在语义上始终只绑定一个底层文件句柄，因此重新 open 前先关闭旧句柄。
    close();

    if (!resume_existing && std::filesystem::exists(path, ec) && overwrite_existing) {
        // 新任务允许覆盖时，先删除旧 .part，避免 CREATE_ALWAYS/rename 等平台差异
        // 把旧恢复产物悄悄保留下来。
        std::filesystem::remove(path, ec);
        ec.clear();
    }

#ifdef _WIN32
    // 恢复时保留现有文件内容，新任务则按 overwrite_existing 决定是覆盖还是独占创建。
    const auto creation_mode = resume_existing ? OPEN_ALWAYS :
        (overwrite_existing ? CREATE_ALWAYS : CREATE_NEW);
    HANDLE handle = CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        creation_mode,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return make_error_code(DownloadErrc::open_file_failed);
    }
    handle_ = handle;
#else
    int flags = resume_existing ? (O_RDWR | O_CREAT) : (O_RDWR | O_CREAT | O_TRUNC);
    if (!resume_existing && !overwrite_existing) {
        flags = O_RDWR | O_CREAT | O_EXCL;
    }
    handle_ = ::open(path.c_str(), flags, 0644);
    if (handle_ < 0) {
        return make_error_code(DownloadErrc::open_file_failed);
    }
#endif

    path_ = path;
    // 句柄一旦成功打开，就立即尝试把逻辑容量扩到目标大小。
    // 这样后续随机写入时不会不断触发文件扩展路径。
    const auto preallocate_error = preallocate(total_size);
    if (preallocate_error) {
        close();
        return preallocate_error;
    }

    return {};
}

std::error_code FileWriter::preallocate(const std::int64_t total_size) noexcept {
    if (total_size <= 0) {
        return {};
    }

    std::scoped_lock lock(mutex_);

#ifdef _WIN32
    // Windows 侧优先走最快路径；若当前进程没有 SeManageVolumePrivilege，
    // 就退化到 sparse + SetEndOfFile，最后再退到普通扩容。
    auto* handle = static_cast<HANDLE>(handle_);
    if (try_fast_preallocate(handle, total_size)) {
        return {};
    }

    if (try_sparse_preallocate(handle, total_size)) {
        return {};
    }

    if (!try_basic_preallocate(handle, total_size)) {
        return make_error_code(DownloadErrc::preallocate_failed);
    }
#else
    // POSIX 先尝试真正的预分配；如果文件系统不支持，再退到只扩展逻辑文件大小。
    if (posix_fallocate(handle_, 0, total_size) == 0) {
        return {};
    }

    if (::ftruncate(handle_, total_size) != 0) {
        return make_error_code(DownloadErrc::preallocate_failed);
    }
#endif

    return {};
}

std::error_code FileWriter::write(const std::int64_t offset,
                                  const std::span<const std::uint8_t> bytes) noexcept {
    if (bytes.empty()) {
        return {};
    }

    // FileWriter 对外暴露的是线程安全接口，所以 write/read/flush/finalize 共用同一把锁，
    // 保证异步 flush 和正常写入不会在同一个文件句柄上交叉打架。
    std::scoped_lock lock(mutex_);

#ifdef _WIN32
    // 这里显式使用系统页缓存，不走 NO_BUFFERING。对下载器来说，串行化的小批量
    // 对齐写入配合页缓存，比额外处理裸设备对齐约束更稳妥。
    if (!set_file_position(static_cast<HANDLE>(handle_), offset)) {
        return make_error_code(DownloadErrc::file_write_failed);
    }

    DWORD written = 0;
    if (!WriteFile(static_cast<HANDLE>(handle_), bytes.data(),
            static_cast<DWORD>(bytes.size()), &written, nullptr) ||
        written != bytes.size()) {
        return make_error_code(DownloadErrc::file_write_failed);
    }
#else
    const auto written = ::pwrite(handle_, bytes.data(), bytes.size(), offset);
    if (written < 0 || static_cast<std::size_t>(written) != bytes.size()) {
        return make_error_code(DownloadErrc::file_write_failed);
    }
#endif

    return {};
}

std::error_code FileWriter::flush() noexcept {
    std::scoped_lock lock(mutex_);

#ifdef _WIN32
    // flush 的语义是把系统页缓存里的脏页尽量推进到稳定介质，
    // 只有它成功后，Persistence 才会推进新的 VDL/metadata。
    if (!FlushFileBuffers(static_cast<HANDLE>(handle_))) {
        return make_error_code(DownloadErrc::file_flush_failed);
    }
#else
    if (::fsync(handle_) != 0) {
        return make_error_code(DownloadErrc::file_flush_failed);
    }
#endif

    return {};
}

std::error_code FileWriter::read(const std::int64_t offset,
                                 const std::size_t length,
                                 std::vector<std::byte>& bytes) noexcept {
    bytes.clear();
    if (length == 0) {
        return {};
    }

    bytes.resize(length);
    std::scoped_lock lock(mutex_);

#ifdef _WIN32
    // read 主要服务于恢复阶段的 CRC 校验，所以同样走显式 offset 读，
    // 不依赖外部维护文件指针状态。
    if (!set_file_position(static_cast<HANDLE>(handle_), offset)) {
        return make_error_code(DownloadErrc::file_read_failed);
    }

    DWORD read = 0;
    if (!ReadFile(static_cast<HANDLE>(handle_), bytes.data(),
            static_cast<DWORD>(bytes.size()), &read, nullptr) ||
        read != bytes.size()) {
        return make_error_code(DownloadErrc::file_read_failed);
    }
#else
    const auto read_bytes = ::pread(handle_, bytes.data(), bytes.size(), offset);
    if (read_bytes < 0 || static_cast<std::size_t>(read_bytes) != bytes.size()) {
        return make_error_code(DownloadErrc::file_read_failed);
    }
#endif

    return {};
}

std::error_code FileWriter::finalize(const std::filesystem::path& output_path,
                                     const bool overwrite_existing) noexcept {
    // finalize 的本质是“把一个已完成的 .part 提升为正式文件”，
    // 前提是先把所有缓存刷干净，再关闭句柄，最后 rename。
    auto ec = flush();
    if (ec) {
        return ec;
    }

    close();

    if (overwrite_existing) {
        std::filesystem::remove(output_path, ec);
        ec.clear();
    }

    // 最终产物通过 rename 提升，避免把“正在下载中的 .part 文件”暴露成正式文件名。
    std::filesystem::rename(path_, output_path, ec);
    if (ec) {
        return make_error_code(DownloadErrc::file_write_failed);
    }

    path_ = output_path;
    return {};
}

void FileWriter::close() noexcept {
    std::scoped_lock lock(mutex_);

#ifdef _WIN32
    // close 是幂等的：无论正常完成、失败回滚还是析构收尾，都可以重复调用。
    if (handle_ != reinterpret_cast<void*>(-1)) {
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = reinterpret_cast<void*>(-1);
    }
#else
    if (handle_ >= 0) {
        ::close(handle_);
        handle_ = -1;
    }
#endif
}

} // namespace asyncdownload::storage
