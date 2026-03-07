#include "metadata/metadata_store.hpp"

#include <gtest/gtest.h>

#include <filesystem>

TEST(MetadataStoreTest, SavesAndLoadsState) {
    const auto base = std::filesystem::temp_directory_path() / "asyncdownload_metadata_store_test.json";
    std::error_code ec;
    std::filesystem::remove(base, ec);

    asyncdownload::metadata::MetadataStore store(base);
    asyncdownload::core::MetadataState state{};
    state.url = "https://example.com/file.bin";
    state.output_path = "output.bin";
    state.temporary_path = "output.bin.part";
    state.total_size = 1024;
    state.vdl_offset = 512;
    state.accept_ranges = true;
    state.resumed = true;
    state.etag = "etag";
    state.last_modified = "last-modified";
    state.block_size = 64 * 1024;
    state.io_alignment = 4 * 1024;
    state.bitmap_states = {2, 1, 0};
    state.ranges.push_back({0, 0, 511, 512, 512, 2});
    state.crc_samples.push_back({512, 1234, 512});

    ASSERT_FALSE(store.save(state));

    const auto [load_error, loaded] = store.load();
    ASSERT_FALSE(load_error);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->url, state.url);
    EXPECT_EQ(loaded->vdl_offset, state.vdl_offset);
    EXPECT_EQ(loaded->bitmap_states, state.bitmap_states);
    ASSERT_EQ(loaded->ranges.size(), 1);
    EXPECT_EQ(loaded->ranges.front().range_id, 0U);
    ASSERT_EQ(loaded->crc_samples.size(), 1);
    EXPECT_EQ(loaded->crc_samples.front().crc32, 1234U);

    ASSERT_FALSE(store.remove());
}
