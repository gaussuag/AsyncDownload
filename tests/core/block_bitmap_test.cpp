#include "core/block_bitmap.hpp"

#include <gtest/gtest.h>

TEST(BlockBitmapTest, MarksFullyCoveredBlocksFinished) {
    asyncdownload::core::AtomicBlockBitmap bitmap(4);

    bitmap.mark_finished_range(0, 128 * 1024, 64 * 1024, 256 * 1024);

    EXPECT_EQ(bitmap.load(0), asyncdownload::core::BlockState::finished);
    EXPECT_EQ(bitmap.load(1), asyncdownload::core::BlockState::finished);
    EXPECT_EQ(bitmap.load(2), asyncdownload::core::BlockState::empty);
}

TEST(BlockBitmapTest, ContiguousFinishedBytesStopsAtFirstHole) {
    asyncdownload::core::AtomicBlockBitmap bitmap(4);
    bitmap.store(0, asyncdownload::core::BlockState::finished);
    bitmap.store(2, asyncdownload::core::BlockState::finished);

    EXPECT_EQ(bitmap.contiguous_finished_bytes(64 * 1024, 256 * 1024), 64 * 1024);
}

TEST(BlockBitmapTest, MarksTouchedBlocksAsDownloadingBeforeTheyFinish) {
    asyncdownload::core::AtomicBlockBitmap bitmap(4);

    bitmap.mark_downloading_range(32 * 1024, 96 * 1024, 64 * 1024, 256 * 1024);

    EXPECT_EQ(bitmap.load(0), asyncdownload::core::BlockState::downloading);
    EXPECT_EQ(bitmap.load(1), asyncdownload::core::BlockState::downloading);
    EXPECT_EQ(bitmap.load(2), asyncdownload::core::BlockState::empty);
}

TEST(BlockBitmapTest, DownloadingMarkDoesNotOverwriteFinishedBlocks) {
    asyncdownload::core::AtomicBlockBitmap bitmap(4);
    bitmap.store(1, asyncdownload::core::BlockState::finished);

    bitmap.mark_downloading_range(64 * 1024, 128 * 1024, 64 * 1024, 256 * 1024);

    EXPECT_EQ(bitmap.load(1), asyncdownload::core::BlockState::finished);
}

TEST(BlockBitmapTest, ResetTransientStatesClearsDownloadingOnly) {
    asyncdownload::core::AtomicBlockBitmap bitmap(4);
    bitmap.store(0, asyncdownload::core::BlockState::finished);
    bitmap.store(1, asyncdownload::core::BlockState::downloading);
    bitmap.store(2, asyncdownload::core::BlockState::empty);

    bitmap.reset_transient_states();

    EXPECT_EQ(bitmap.load(0), asyncdownload::core::BlockState::finished);
    EXPECT_EQ(bitmap.load(1), asyncdownload::core::BlockState::empty);
    EXPECT_EQ(bitmap.load(2), asyncdownload::core::BlockState::empty);
}
