#include "http_probe.hpp"

#include "asyncdownload/error.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>

#include <curl/curl.h>

namespace asyncdownload::download {
namespace {

struct ProbeHeaders {
    bool accept_ranges = false;
    std::string etag;
    std::string last_modified;
    // 某些服务端 HEAD 不返回 Content-Length，但会在 206 的 Content-Range 里
    // 暴露总长度，所以这里额外保留一个备选来源。
    std::optional<std::int64_t> content_range_total;
};

[[nodiscard]] std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.pop_back();
    }
    return value;
}

[[nodiscard]] std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](const unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    return value;
}

size_t discard_body(char* data, const size_t size, const size_t nmemb, void* user_data) {
    static_cast<void>(data);
    static_cast<void>(user_data);
    return size * nmemb;
}

size_t capture_header(char* buffer, const size_t size, const size_t nmemb, void* user_data) {
    auto* headers = static_cast<ProbeHeaders*>(user_data);
    std::string line(buffer, size * nmemb);
    const auto separator = line.find(':');
    if (separator == std::string::npos) {
        return size * nmemb;
    }

    auto key = to_lower(trim(line.substr(0, separator)));
    auto value = trim(line.substr(separator + 1));
    if (!value.empty() && value.back() == '\r') {
        value.pop_back();
    }

    if (key == "accept-ranges") {
        headers->accept_ranges = to_lower(value).find("bytes") != std::string::npos;
    } else if (key == "etag") {
        headers->etag = value;
    } else if (key == "last-modified") {
        headers->last_modified = value;
    } else if (key == "content-range") {
        // fallback 到 Range GET 时，Content-Range 通常形如 "bytes 0-0/12345"。
        // 这里把最后的总长度部分抽出来，作为 HEAD 不可靠时的补充信息。
        const auto slash = value.rfind('/');
        if (slash != std::string::npos) {
            const auto total_value = value.substr(slash + 1);
            try {
                headers->content_range_total = std::stoll(total_value);
                headers->accept_ranges = true;
            } catch (...) {
            }
        }
    }

    return size * nmemb;
}

void configure_common(CURL* curl,
                      const std::string& url,
                      ProbeHeaders& headers) {
    // probe 的目标是“拿元信息而不是下载正文”，所以统一丢弃 body，只保留头信息。
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, capture_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &headers);
}

[[nodiscard]] core::RemoteProbeResult fallback_probe(const std::string& url) noexcept {
    core::RemoteProbeResult result{};
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        result.error = make_error_code(DownloadErrc::http_init_failed);
        return result;
    }

    ProbeHeaders headers;
    configure_common(curl, url, headers);
    // HEAD 在不少站点上要么被禁用，要么不返回完整长度。
    // 所以降级方案是发一个 0-0 的 Range GET，用最小代价换取总大小和 Range 能力。
    curl_easy_setopt(curl, CURLOPT_RANGE, "0-0");

    const auto code = curl_easy_perform(curl);
    if (code != CURLE_OK) {
        result.error = make_error_code(DownloadErrc::http_probe_failed);
        curl_easy_cleanup(curl);
        return result;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.response_code);

    curl_off_t length = -1;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length);
    // 优先使用 Content-Range 里的总长度，因为它通常比 206 场景下的
    // CONTENT_LENGTH_DOWNLOAD_T 更直接可靠。
    result.total_size = headers.content_range_total.value_or(length > 0 ? length : 0);
    result.accept_ranges = headers.accept_ranges;
    result.etag = headers.etag;
    result.last_modified = headers.last_modified;
    curl_easy_cleanup(curl);

    if (result.total_size <= 0) {
        result.error = make_error_code(DownloadErrc::http_probe_failed);
    }
    return result;
}

} // namespace

core::RemoteProbeResult HttpProbe::probe(const std::string& url) const noexcept {
    core::RemoteProbeResult result{};
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        result.error = make_error_code(DownloadErrc::http_init_failed);
        return result;
    }

    ProbeHeaders headers;
    configure_common(curl, url, headers);
    // 正常路径优先走 HEAD，因为它最省流量，也最符合“先探测再下载”的预期。
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);

    const auto code = curl_easy_perform(curl);
    if (code != CURLE_OK) {
        curl_easy_cleanup(curl);
        return fallback_probe(url);
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.response_code);
    curl_off_t length = -1;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length);

    result.total_size = length > 0 ? length : 0;
    result.accept_ranges = headers.accept_ranges;
    result.etag = headers.etag;
    result.last_modified = headers.last_modified;
    curl_easy_cleanup(curl);

    // 只要 HEAD 给出的关键信息不够用，就回退到 Range GET。
    // 这里宁可多一次探测请求，也不把一个长度未知的资源交给下载主链。
    if (result.response_code >= 400 || result.total_size <= 0) {
        return fallback_probe(url);
    }

    return result;
}

} // namespace asyncdownload::download
