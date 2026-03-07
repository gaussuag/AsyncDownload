#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <span>
#include <system_error>
#include <vector>

namespace asyncdownload::storage {

class FileWriter {
public:
    FileWriter() = default;
    ~FileWriter();

    FileWriter(const FileWriter&) = delete;
    FileWriter& operator=(const FileWriter&) = delete;

    // 打开或创建底层文件，并按目标大小做预分配。
    // resume_existing=true 时保留现有内容，供断点续传继续写入。
    [[nodiscard]] std::error_code open(const std::filesystem::path& path,
                                       std::int64_t total_size,
                                       bool resume_existing,
                                       bool overwrite_existing) noexcept;
    // 在指定偏移写入一段字节，不依赖外部维护文件指针状态。
    [[nodiscard]] std::error_code write(std::int64_t offset,
                                        std::span<const std::uint8_t> bytes) noexcept;
    // 强制把页缓存内容刷到稳定介质，是 VDL 和 metadata 推进的前提。
    [[nodiscard]] std::error_code flush() noexcept;
    // 按偏移读取字节，主要供恢复时的 CRC 校验使用。
    [[nodiscard]] std::error_code read(std::int64_t offset,
                                       std::size_t length,
                                       std::vector<std::byte>& bytes) noexcept;
    // 把已完成的 .part 文件提升为正式文件。
    [[nodiscard]] std::error_code finalize(const std::filesystem::path& output_path,
                                           bool overwrite_existing) noexcept;
    // 幂等关闭底层句柄。
    void close() noexcept;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    // 按平台能力尝试更高效的预分配路径，并在失败时逐级降级。
    [[nodiscard]] std::error_code preallocate(std::int64_t total_size) noexcept;

#ifdef _WIN32
    // Windows 句柄统一擦成 void*，避免在头文件里暴露平台细节类型。
    void* handle_ = reinterpret_cast<void*>(-1);
#else
    int handle_ = -1;
#endif
    std::filesystem::path path_;
    std::mutex mutex_;
};

} // namespace asyncdownload::storage
