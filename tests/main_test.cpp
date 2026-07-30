#include <chrono>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "asyncdownload/client.hpp"
#include "asyncdownload/error.hpp"

namespace {

asyncdownload::DownloadRequest make_invalid_policy_request(const std::string& name) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    asyncdownload::DownloadRequest request{};
    request.url = "http://127.0.0.1:1/unreachable";
    request.output_path = std::filesystem::temp_directory_path() /
        (name + "_" + std::to_string(stamp) + ".bin");
    return request;
}

void expect_invalid_policy_without_artifacts(const asyncdownload::DownloadRequest& request,
                                             const asyncdownload::DownloadResult& result) {
    const auto temporary_path = std::filesystem::path(request.output_path.string() + ".part");
    const auto metadata_path =
        std::filesystem::path(request.output_path.string() + ".config.json");
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error,
        asyncdownload::make_error_code(asyncdownload::DownloadErrc::invalid_request));
    EXPECT_FALSE(std::filesystem::exists(temporary_path));
    EXPECT_FALSE(std::filesystem::exists(metadata_path));
}

}

TEST(DownloadClientTest, RejectsEmptyRequest) {
    asyncdownload::DownloadClient client;
    asyncdownload::DownloadRequest request{};

    const auto result = client.download(request);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error,
        asyncdownload::make_error_code(asyncdownload::DownloadErrc::invalid_request));
}

TEST(DownloadClientTest, RejectsInvalidPolicyBeforeHttpProbe) {
    auto request = make_invalid_policy_request("asyncdownload_invalid_connections");
    request.options.max_connections = 0;

    asyncdownload::DownloadClient client;
    const auto result = client.download(request);

    expect_invalid_policy_without_artifacts(request, result);
}

TEST(DownloadClientTest, RejectsUnsafeAlignmentBeforeCreatingArtifacts) {
    auto request = make_invalid_policy_request("asyncdownload_invalid_alignment");
    request.options.io_alignment = 8192;

    asyncdownload::DownloadClient client;
    const auto result = client.download(request);

    expect_invalid_policy_without_artifacts(request, result);
}

TEST(DownloadClientTest, RejectsInvalidWatermarksBeforeCreatingArtifacts) {
    auto request = make_invalid_policy_request("asyncdownload_invalid_watermarks");
    request.options.backpressure_low_bytes = request.options.backpressure_high_bytes + 1;

    asyncdownload::DownloadClient client;
    const auto result = client.download(request);

    expect_invalid_policy_without_artifacts(request, result);
}
