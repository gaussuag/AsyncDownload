#pragma once

#include <curl/curl.h>

#include <cstdint>

#if defined(ASYNCDOWNLOAD_HTTP_TRANSFER_FAULT_TEST)
#include <atomic>
#endif

namespace asyncdownload::http::detail {

#if defined(ASYNCDOWNLOAD_HTTP_TRANSFER_FAULT_TEST)

struct CurlRuntimeFaultPlan {
    std::atomic<bool> fail_next_easy_init{false};
    std::atomic<bool> fail_next_easy_option{false};
    std::atomic<bool> fail_next_easy_info{false};
    std::atomic<bool> fail_next_easy_pause{false};
    std::atomic<bool> fail_next_multi_init{false};
    std::atomic<bool> fail_next_multi_option{false};
    std::atomic<bool> fail_next_add_handle{false};
    std::atomic<bool> fail_next_remove_handle{false};
    std::atomic<bool> fail_next_multi_perform{false};
    std::atomic<bool> fail_next_multi_wait{false};
    std::atomic<bool> fail_next_multi_cleanup{false};
    std::atomic<bool> fail_next_slist_append{false};
    std::atomic<bool> skip_next_easy_perform{false};
    std::atomic<bool> skip_next_multi_perform{false};
    std::atomic<bool> emit_next_done{false};
    std::atomic<void*> last_added_easy{nullptr};
    std::atomic<std::uint64_t> easy_info_calls{0};
    std::atomic<std::uint64_t> multi_perform_calls{0};
    std::atomic<std::uint64_t> multi_wait_calls{0};
    std::atomic<std::uint64_t> remove_handle_calls{0};
    std::atomic<std::uint64_t> multi_cleanup_calls{0};

    void reset() noexcept {
        fail_next_easy_init.store(false);
        fail_next_easy_option.store(false);
        fail_next_easy_info.store(false);
        fail_next_easy_pause.store(false);
        fail_next_multi_init.store(false);
        fail_next_multi_option.store(false);
        fail_next_add_handle.store(false);
        fail_next_remove_handle.store(false);
        fail_next_multi_perform.store(false);
        fail_next_multi_wait.store(false);
        fail_next_multi_cleanup.store(false);
        fail_next_slist_append.store(false);
        skip_next_easy_perform.store(false);
        skip_next_multi_perform.store(false);
        emit_next_done.store(false);
        last_added_easy.store(nullptr);
        easy_info_calls.store(0);
        multi_perform_calls.store(0);
        multi_wait_calls.store(0);
        remove_handle_calls.store(0);
        multi_cleanup_calls.store(0);
    }
};

inline CurlRuntimeFaultPlan&
curl_runtime_fault_plan() noexcept {
    static CurlRuntimeFaultPlan plan;
    return plan;
}

#endif

inline CURLcode global_init(
    const long flags) noexcept {
    return curl_global_init(flags);
}

inline CURL* easy_init() noexcept {
#if defined(ASYNCDOWNLOAD_HTTP_TRANSFER_FAULT_TEST)
    if (curl_runtime_fault_plan().
            fail_next_easy_init.exchange(false)) {
        return nullptr;
    }
#endif
    return curl_easy_init();
}

template <typename Value>
CURLcode easy_setopt(
    CURL* easy,
    const CURLoption option,
    const Value value) noexcept {
#if defined(ASYNCDOWNLOAD_HTTP_TRANSFER_FAULT_TEST)
    if (curl_runtime_fault_plan().
            fail_next_easy_option.exchange(false)) {
        return CURLE_UNKNOWN_OPTION;
    }
#endif
    return curl_easy_setopt(
        easy,
        option,
        value);
}

inline CURLcode easy_perform(
    CURL* easy) noexcept {
#if defined(ASYNCDOWNLOAD_HTTP_TRANSFER_FAULT_TEST)
    if (curl_runtime_fault_plan().
            skip_next_easy_perform.exchange(false)) {
        return CURLE_OK;
    }
#endif
    return curl_easy_perform(easy);
}

template <typename Value>
CURLcode easy_getinfo(
    CURL* easy,
    const CURLINFO info,
    Value* value) noexcept {
#if defined(ASYNCDOWNLOAD_HTTP_TRANSFER_FAULT_TEST)
    auto& plan = curl_runtime_fault_plan();
    plan.easy_info_calls.fetch_add(1);
    if (plan.fail_next_easy_info.exchange(false)) {
        return CURLE_BAD_FUNCTION_ARGUMENT;
    }
#endif
    return curl_easy_getinfo(
        easy,
        info,
        value);
}

inline CURLcode easy_pause(
    CURL* easy,
    const int bitmask) noexcept {
#if defined(ASYNCDOWNLOAD_HTTP_TRANSFER_FAULT_TEST)
    if (curl_runtime_fault_plan().
            fail_next_easy_pause.exchange(false)) {
        return CURLE_BAD_FUNCTION_ARGUMENT;
    }
#endif
    return curl_easy_pause(
        easy,
        bitmask);
}

inline void easy_reset(CURL* easy) noexcept {
    curl_easy_reset(easy);
}

inline void easy_cleanup(CURL* easy) noexcept {
    curl_easy_cleanup(easy);
}

inline CURLM* multi_init() noexcept {
#if defined(ASYNCDOWNLOAD_HTTP_TRANSFER_FAULT_TEST)
    if (curl_runtime_fault_plan().
            fail_next_multi_init.exchange(false)) {
        return nullptr;
    }
#endif
    return curl_multi_init();
}

template <typename Value>
CURLMcode multi_setopt(
    CURLM* multi,
    const CURLMoption option,
    const Value value) noexcept {
#if defined(ASYNCDOWNLOAD_HTTP_TRANSFER_FAULT_TEST)
    if (curl_runtime_fault_plan().
            fail_next_multi_option.exchange(false)) {
        return CURLM_BAD_FUNCTION_ARGUMENT;
    }
#endif
    return curl_multi_setopt(
        multi,
        option,
        value);
}

inline CURLMcode multi_add_handle(
    CURLM* multi,
    CURL* easy) noexcept {
#if defined(ASYNCDOWNLOAD_HTTP_TRANSFER_FAULT_TEST)
    if (curl_runtime_fault_plan().
            fail_next_add_handle.exchange(false)) {
        return CURLM_INTERNAL_ERROR;
    }
#endif
    const auto result =
        curl_multi_add_handle(
            multi,
            easy);
#if defined(ASYNCDOWNLOAD_HTTP_TRANSFER_FAULT_TEST)
    if (result == CURLM_OK) {
        curl_runtime_fault_plan().
            last_added_easy.store(easy);
    }
#endif
    return result;
}

inline CURLMcode multi_remove_handle(
    CURLM* multi,
    CURL* easy) noexcept {
#if defined(ASYNCDOWNLOAD_HTTP_TRANSFER_FAULT_TEST)
    auto& plan = curl_runtime_fault_plan();
    plan.remove_handle_calls.fetch_add(1);
    const auto result =
        curl_multi_remove_handle(
            multi,
            easy);
    return plan.fail_next_remove_handle.exchange(false)
        ? CURLM_INTERNAL_ERROR
        : result;
#else
    return curl_multi_remove_handle(
        multi,
        easy);
#endif
}

inline CURLMcode multi_perform(
    CURLM* multi,
    int* running_handles) noexcept {
#if defined(ASYNCDOWNLOAD_HTTP_TRANSFER_FAULT_TEST)
    auto& plan = curl_runtime_fault_plan();
    plan.multi_perform_calls.fetch_add(1);
    if (plan.fail_next_multi_perform.exchange(false)) {
        return CURLM_INTERNAL_ERROR;
    }
    if (plan.skip_next_multi_perform.exchange(false)) {
        *running_handles = 1;
        return CURLM_OK;
    }
#endif
    return curl_multi_perform(
        multi,
        running_handles);
}

inline CURLMcode multi_wait(
    CURLM* multi,
    curl_waitfd* extra_fds,
    const unsigned int extra_count,
    const int timeout_ms,
    int* descriptor_count) noexcept {
#if defined(ASYNCDOWNLOAD_HTTP_TRANSFER_FAULT_TEST)
    auto& plan = curl_runtime_fault_plan();
    plan.multi_wait_calls.fetch_add(1);
    if (plan.fail_next_multi_wait.exchange(false)) {
        return CURLM_INTERNAL_ERROR;
    }
#endif
    return curl_multi_wait(
        multi,
        extra_fds,
        extra_count,
        timeout_ms,
        descriptor_count);
}

inline CURLMsg* multi_info_read(
    CURLM* multi,
    int* pending_messages) noexcept {
#if defined(ASYNCDOWNLOAD_HTTP_TRANSFER_FAULT_TEST)
    auto& plan = curl_runtime_fault_plan();
    if (plan.emit_next_done.exchange(false)) {
        static thread_local CURLMsg message{};
        message.msg = CURLMSG_DONE;
        message.easy_handle =
            static_cast<CURL*>(
                plan.last_added_easy.load());
        message.data.result = CURLE_RECV_ERROR;
        *pending_messages = 0;
        return &message;
    }
#endif
    return curl_multi_info_read(
        multi,
        pending_messages);
}

inline CURLMcode multi_cleanup(
    CURLM* multi) noexcept {
#if defined(ASYNCDOWNLOAD_HTTP_TRANSFER_FAULT_TEST)
    auto& plan = curl_runtime_fault_plan();
    plan.multi_cleanup_calls.fetch_add(1);
    const auto result =
        curl_multi_cleanup(multi);
    return plan.fail_next_multi_cleanup.exchange(false)
        ? CURLM_INTERNAL_ERROR
        : result;
#else
    return curl_multi_cleanup(multi);
#endif
}

inline curl_slist* slist_append(
    curl_slist* list,
    const char* value) noexcept {
#if defined(ASYNCDOWNLOAD_HTTP_TRANSFER_FAULT_TEST)
    if (curl_runtime_fault_plan().
            fail_next_slist_append.exchange(false)) {
        return nullptr;
    }
#endif
    return curl_slist_append(
        list,
        value);
}

inline void slist_free_all(
    curl_slist* list) noexcept {
    curl_slist_free_all(list);
}

}
