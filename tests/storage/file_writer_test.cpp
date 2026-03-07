#include "storage/file_writer.hpp"

#include <gtest/gtest.h>

#include <filesystem>

namespace {

TEST(FileWriterTest, OpenPreallocatesTargetSize) {
    const auto temp_root = std::filesystem::temp_directory_path() / "asyncdownload_file_writer_size";
    std::error_code ec;
    const auto removed = std::filesystem::remove_all(temp_root, ec);
    static_cast<void>(removed);
    std::filesystem::create_directories(temp_root, ec);
    ASSERT_FALSE(ec);

    const auto path = temp_root / "output.bin.part";
    asyncdownload::storage::FileWriter writer;
    ASSERT_FALSE(writer.open(path, 128 * 1024, false, true));
    writer.close();

    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_EQ(std::filesystem::file_size(path), 128 * 1024);

    const auto cleaned = std::filesystem::remove_all(temp_root, ec);
    static_cast<void>(cleaned);
}

TEST(FileWriterTest, OpenFailsWhenOverwriteIsDisabledAndFileExists) {
    const auto temp_root =
        std::filesystem::temp_directory_path() / "asyncdownload_file_writer_overwrite";
    std::error_code ec;
    const auto removed = std::filesystem::remove_all(temp_root, ec);
    static_cast<void>(removed);
    std::filesystem::create_directories(temp_root, ec);
    ASSERT_FALSE(ec);

    const auto path = temp_root / "output.bin.part";
    asyncdownload::storage::FileWriter first_writer;
    ASSERT_FALSE(first_writer.open(path, 64 * 1024, false, true));
    first_writer.close();

    asyncdownload::storage::FileWriter second_writer;
    const auto error = second_writer.open(path, 64 * 1024, false, false);
    EXPECT_TRUE(static_cast<bool>(error));

    const auto cleaned = std::filesystem::remove_all(temp_root, ec);
    static_cast<void>(cleaned);
}

} // namespace
