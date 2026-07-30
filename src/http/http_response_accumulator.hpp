#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace asyncdownload::http {

struct ParsedContentRange {
    std::int64_t first = 0;
    std::int64_t last = 0;
    std::int64_t total = 0;

    friend bool operator==(
        const ParsedContentRange&,
        const ParsedContentRange&) = default;
};

class HttpResponseAccumulator {
public:
    [[nodiscard]] bool append(
        std::string_view line) noexcept;
    void begin_body() noexcept;

    [[nodiscard]] long status() const noexcept;
    [[nodiscard]] bool headers_complete() const noexcept;
    [[nodiscard]] bool parser_failed() const noexcept;
    [[nodiscard]] bool accept_ranges() const noexcept;
    [[nodiscard]] const std::optional<std::int64_t>&
    content_length() const noexcept;
    [[nodiscard]] const std::optional<ParsedContentRange>&
    content_range() const noexcept;
    [[nodiscard]] bool content_range_invalid() const noexcept;
    [[nodiscard]] bool content_length_invalid() const noexcept;
    [[nodiscard]] const std::string& content_encoding() const noexcept;
    [[nodiscard]] bool content_encoding_invalid() const noexcept;
    [[nodiscard]] const std::string& etag() const noexcept;
    [[nodiscard]] const std::string& last_modified() const noexcept;

private:
    void start_block(long status) noexcept;

    long status_ = 0;
    bool headers_complete_ = false;
    bool body_started_ = false;
    bool parser_failed_ = false;
    bool accept_ranges_ = false;
    std::optional<std::int64_t> content_length_;
    std::optional<ParsedContentRange> content_range_;
    bool content_range_invalid_ = false;
    bool content_length_invalid_ = false;
    std::string content_encoding_;
    bool content_encoding_invalid_ = false;
    std::string etag_;
    std::string last_modified_;
};

}
