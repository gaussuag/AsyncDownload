#include "http_response_accumulator.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>

namespace asyncdownload::http {
namespace {

std::string_view trim_ows(
    std::string_view value) noexcept {
    while (!value.empty() &&
           (value.front() == ' ' ||
            value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           (value.back() == ' ' ||
            value.back() == '\t' ||
            value.back() == '\r' ||
            value.back() == '\n')) {
        value.remove_suffix(1);
    }
    return value;
}

bool ascii_equal(
    const std::string_view left,
    const std::string_view right) noexcept {
    return left.size() == right.size() &&
        std::equal(
            left.begin(),
            left.end(),
            right.begin(),
            [](const char lhs, const char rhs) {
                return std::tolower(
                           static_cast<unsigned char>(lhs)) ==
                    std::tolower(
                           static_cast<unsigned char>(rhs));
            });
}

std::optional<std::int64_t> parse_decimal(
    const std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    const auto parsed = std::from_chars(
        text.data(),
        text.data() + text.size(),
        value);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != text.data() + text.size() ||
        value < 0) {
        return std::nullopt;
    }
    return value;
}

std::optional<long> parse_status(
    const std::string_view line) noexcept {
    if (!line.starts_with("HTTP/")) {
        return std::nullopt;
    }
    const auto separator = line.find(' ');
    if (separator == std::string_view::npos ||
        separator + 4 > line.size()) {
        return std::nullopt;
    }
    const auto code = line.substr(separator + 1, 3);
    if (!std::all_of(
            code.begin(),
            code.end(),
            [](const char value) {
                return value >= '0' && value <= '9';
            })) {
        return std::nullopt;
    }
    return static_cast<long>(
        (code[0] - '0') * 100 +
        (code[1] - '0') * 10 +
        code[2] - '0');
}

std::optional<ParsedContentRange>
parse_content_range(
    std::string_view value) noexcept {
    value = trim_ows(value);
    constexpr std::string_view prefix = "bytes ";
    if (value.size() <= prefix.size() ||
        !ascii_equal(
            value.substr(0, prefix.size()),
            prefix)) {
        return std::nullopt;
    }
    value.remove_prefix(prefix.size());
    const auto dash = value.find('-');
    const auto slash = value.find('/');
    if (dash == std::string_view::npos ||
        slash == std::string_view::npos ||
        dash == 0 ||
        slash <= dash + 1 ||
        slash + 1 >= value.size() ||
        value.find('-', dash + 1) !=
            std::string_view::npos ||
        value.find('/', slash + 1) !=
            std::string_view::npos) {
        return std::nullopt;
    }
    const auto first =
        parse_decimal(value.substr(0, dash));
    const auto last = parse_decimal(
        value.substr(dash + 1, slash - dash - 1));
    const auto total =
        parse_decimal(value.substr(slash + 1));
    if (!first.has_value() ||
        !last.has_value() ||
        !total.has_value() ||
        *first > *last ||
        *last >= *total) {
        return std::nullopt;
    }
    return ParsedContentRange{
        *first,
        *last,
        *total
    };
}

bool has_token(
    std::string_view value,
    const std::string_view expected) noexcept {
    while (!value.empty()) {
        const auto comma = value.find(',');
        const auto token = trim_ows(
            value.substr(0, comma));
        if (ascii_equal(token, expected)) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        value.remove_prefix(comma + 1);
    }
    return false;
}

}

bool HttpResponseAccumulator::append(
    const std::string_view line) noexcept {
    try {
        if (const auto parsed_status =
                parse_status(line);
            parsed_status.has_value()) {
            start_block(*parsed_status);
            return true;
        }
        if (body_started_) {
            return true;
        }
        if (line == "\r\n" ||
            line == "\n" ||
            line.empty()) {
            headers_complete_ = true;
            return true;
        }

        const auto separator = line.find(':');
        if (separator == std::string_view::npos) {
            parser_failed_ = true;
            return false;
        }
        const auto name = trim_ows(
            line.substr(0, separator));
        const auto value = trim_ows(
            line.substr(separator + 1));
        if (ascii_equal(name, "accept-ranges")) {
            accept_ranges_ = accept_ranges_ ||
                has_token(value, "bytes");
        } else if (ascii_equal(
                       name,
                       "content-length")) {
            const auto parsed = parse_decimal(value);
            if (!parsed.has_value() ||
                (content_length_.has_value() &&
                 *content_length_ != *parsed)) {
                content_length_invalid_ = true;
            } else {
                content_length_ = *parsed;
            }
        } else if (ascii_equal(
                       name,
                       "content-range")) {
            const auto parsed =
                parse_content_range(value);
            if (!parsed.has_value() ||
                content_range_.has_value()) {
                content_range_invalid_ = true;
            } else {
                content_range_ = *parsed;
            }
        } else if (ascii_equal(
                       name,
                       "content-encoding")) {
            const std::string parsed(value);
            if (!content_encoding_.empty() &&
                !ascii_equal(
                    content_encoding_,
                    parsed)) {
                content_encoding_invalid_ = true;
            } else {
                content_encoding_ = parsed;
            }
        } else if (ascii_equal(name, "etag")) {
            etag_ = value;
        } else if (ascii_equal(
                       name,
                       "last-modified")) {
            last_modified_ = value;
        }
        return true;
    } catch (...) {
        parser_failed_ = true;
        return false;
    }
}

void HttpResponseAccumulator::reset() noexcept {
    status_ = 0;
    headers_complete_ = false;
    body_started_ = false;
    parser_failed_ = false;
    accept_ranges_ = false;
    content_length_.reset();
    content_range_.reset();
    content_range_invalid_ = false;
    content_length_invalid_ = false;
    content_encoding_.clear();
    content_encoding_invalid_ = false;
    etag_.clear();
    last_modified_.clear();
}

void HttpResponseAccumulator::begin_body() noexcept {
    body_started_ = true;
}

long HttpResponseAccumulator::status() const noexcept {
    return status_;
}

bool HttpResponseAccumulator::headers_complete()
    const noexcept {
    return headers_complete_;
}

bool HttpResponseAccumulator::parser_failed()
    const noexcept {
    return parser_failed_;
}

bool HttpResponseAccumulator::accept_ranges()
    const noexcept {
    return accept_ranges_;
}

const std::optional<std::int64_t>&
HttpResponseAccumulator::content_length()
    const noexcept {
    return content_length_;
}

const std::optional<ParsedContentRange>&
HttpResponseAccumulator::content_range()
    const noexcept {
    return content_range_;
}

bool HttpResponseAccumulator::content_range_invalid()
    const noexcept {
    return content_range_invalid_;
}

bool HttpResponseAccumulator::content_length_invalid()
    const noexcept {
    return content_length_invalid_;
}

const std::string&
HttpResponseAccumulator::content_encoding()
    const noexcept {
    return content_encoding_;
}

bool HttpResponseAccumulator::content_encoding_invalid()
    const noexcept {
    return content_encoding_invalid_;
}

const std::string& HttpResponseAccumulator::etag()
    const noexcept {
    return etag_;
}

const std::string&
HttpResponseAccumulator::last_modified()
    const noexcept {
    return last_modified_;
}

void HttpResponseAccumulator::start_block(
    const long status) noexcept {
    status_ = status;
    headers_complete_ = false;
    body_started_ = false;
    accept_ranges_ = false;
    content_length_.reset();
    content_range_.reset();
    content_range_invalid_ = false;
    content_length_invalid_ = false;
    content_encoding_.clear();
    content_encoding_invalid_ = false;
    etag_.clear();
    last_modified_.clear();
}

}
