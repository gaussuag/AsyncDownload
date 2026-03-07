#include "asyncdownload/client.hpp"
#include "asyncdownload/error.hpp"

#include <gtest/gtest.h>

TEST(DownloadClientTest, RejectsEmptyRequest) {
    asyncdownload::DownloadClient client;
    asyncdownload::DownloadRequest request{};

    const auto result = client.download(request);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error, asyncdownload::make_error_code(asyncdownload::DownloadErrc::invalid_request));
}
