#include "http/http_response_accumulator.hpp"

#include <gtest/gtest.h>

#include <string_view>

namespace {

void append_line(
    asyncdownload::http::HttpResponseAccumulator& response,
    const std::string_view line) {
    ASSERT_TRUE(response.append(line));
}

TEST(HttpResponseAccumulatorTest, KeepsOnlyFinalResponseBlock) {
    asyncdownload::http::HttpResponseAccumulator response;
    append_line(response, "HTTP/1.1 302 Found\r\n");
    append_line(response, "Content-Length: 0\r\n");
    append_line(response, "Accept-Ranges: bytes\r\n");
    append_line(response, "ETag: \"redirect\"\r\n");
    append_line(response, "\r\n");
    append_line(response, "HTTP/1.1 200 OK\r\n");
    append_line(response, "Content-Length: 128\r\n");
    append_line(response, "Accept-Ranges: none\r\n");
    append_line(response, "ETag: \"final\"\r\n");
    append_line(response, "\r\n");

    EXPECT_EQ(response.status(), 200);
    ASSERT_TRUE(response.content_length().has_value());
    EXPECT_EQ(*response.content_length(), 128);
    EXPECT_FALSE(response.accept_ranges());
    EXPECT_EQ(response.etag(), "\"final\"");
    EXPECT_TRUE(response.headers_complete());
}

TEST(HttpResponseAccumulatorTest, RejectsDecimalOverflow) {
    asyncdownload::http::HttpResponseAccumulator response;
    append_line(response, "HTTP/1.1 200 OK\r\n");
    append_line(
        response,
        "Content-Length: 9223372036854775808\r\n");

    EXPECT_TRUE(response.content_length_invalid());
    EXPECT_FALSE(response.content_length().has_value());
}

TEST(HttpResponseAccumulatorTest, ClassifiesDuplicateContentRangeAsInvalid) {
    asyncdownload::http::HttpResponseAccumulator response;
    append_line(response, "HTTP/1.1 206 Partial Content\r\n");
    append_line(response, "Content-Range: bytes 0-0/128\r\n");
    append_line(response, "Content-Range: bytes 0-0/128\r\n");

    EXPECT_TRUE(response.content_range_invalid());
}

TEST(HttpResponseAccumulatorTest, AllowsOnlyIdenticalContentLengths) {
    asyncdownload::http::HttpResponseAccumulator identical;
    append_line(identical, "HTTP/1.1 200 OK\r\n");
    append_line(identical, "Content-Length: 128\r\n");
    append_line(identical, "content-length:\t128 \r\n");
    EXPECT_FALSE(identical.content_length_invalid());

    asyncdownload::http::HttpResponseAccumulator conflicting;
    append_line(conflicting, "HTTP/1.1 200 OK\r\n");
    append_line(conflicting, "Content-Length: 128\r\n");
    append_line(conflicting, "Content-Length: 129\r\n");
    EXPECT_TRUE(conflicting.content_length_invalid());
}

TEST(HttpResponseAccumulatorTest, ParsesCaseInsensitiveContentRangeUnit) {
    asyncdownload::http::HttpResponseAccumulator response;
    append_line(response, "HTTP/1.1 206 Partial Content\r\n");
    append_line(response, "content-range: BYTES 7-9/10\r\n");

    ASSERT_TRUE(response.content_range().has_value());
    EXPECT_EQ(
        *response.content_range(),
        (asyncdownload::http::ParsedContentRange{
            7,
            9,
            10
        }));
}

TEST(HttpResponseAccumulatorTest, RejectsStarAndInclusiveOverflowRange) {
    asyncdownload::http::HttpResponseAccumulator star;
    append_line(star, "HTTP/1.1 206 Partial Content\r\n");
    append_line(star, "Content-Range: bytes 0-0/*\r\n");
    EXPECT_TRUE(star.content_range_invalid());

    asyncdownload::http::HttpResponseAccumulator overflow;
    append_line(overflow, "HTTP/1.1 206 Partial Content\r\n");
    append_line(
        overflow,
        "Content-Range: bytes 0-9223372036854775807/"
        "9223372036854775807\r\n");
    EXPECT_TRUE(overflow.content_range_invalid());
}

TEST(HttpResponseAccumulatorTest, IgnoresTrailersAfterBodyStarts) {
    asyncdownload::http::HttpResponseAccumulator response;
    append_line(response, "HTTP/1.1 200 OK\r\n");
    append_line(response, "Content-Length: 128\r\n");
    append_line(response, "ETag: \"header\"\r\n");
    append_line(response, "\r\n");
    response.begin_body();
    append_line(response, "Content-Length: 4\r\n");
    append_line(response, "ETag: \"trailer\"\r\n");

    ASSERT_TRUE(response.content_length().has_value());
    EXPECT_EQ(*response.content_length(), 128);
    EXPECT_EQ(response.etag(), "\"header\"");
}

}
