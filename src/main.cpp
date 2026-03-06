#include <iostream>
#include <curl/curl.h>
#include <concurrentqueue/concurrentqueue.h>
#include <vector>
#include <string>
#include <cstring>

int main() {
    std::cout << "Hello, World!" << std::endl;

    CURL* curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, "https://example.com");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);

        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK) {
            long response_code;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
            std::cout << "HTTP Response Code: " << response_code << std::endl;
        } else {
            std::cout << "curl_easy_perform failed: " << curl_easy_strerror(res) << std::endl;
        }

        curl_easy_cleanup(curl);
    }

    moodycamel::ConcurrentQueue<int> queue;
    queue.enqueue(1);
    queue.enqueue(2);
    queue.enqueue(3);

    int value;
    while (queue.try_dequeue(value)) {
        std::cout << "Dequeued: " << value << std::endl;
    }

    return 0;
}
