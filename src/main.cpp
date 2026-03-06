#include <iostream>
#include <curl/curl.h>
#include <concurrentqueue/concurrentqueue.h>
#include <7zip/C/7zVersion.h>
#include "7zip/C/LzmaLib.h" 
#include <vector>
#include <string>
#include <cstring>

static int zipApi() {
    std::string original_data = "Hello 7-Zip! Testing connection... 1234567890";
    size_t srcLen = original_data.size();

    // 1. 准备目标缓冲区
    // 压缩后的长度估算：源长度 + 1% + 12字节 (LZMA 头部)
    size_t destLen = srcLen + srcLen / 10 + 20;
    std::vector<unsigned char> outBuffer(destLen);

    // 2. 准备属性缓冲区 (LZMA 需要 5 字节来存储编码属性)
    unsigned char outProps[5];
    size_t outPropsSize = 5;

    std::cout << "--- 7-Zip (LZMA) 链接测试 ---" << std::endl;

    // 3. 调用压缩函数
    int result = LzmaCompress(
        outBuffer.data(), &destLen,             // 输出数据及长度
        (const unsigned char*)original_data.c_str(), srcLen, // 输入数据及长度
        outProps, &outPropsSize,                // 属性缓冲区
        5,        // 压缩等级 [0, 9]
        1 << 24,  // 字典大小 (DictSize)
        3,        // lc
        0,        // lp
        2,        // pb
        32,       // fb
        2         // numThreads
    );

    // 4. 验证结果
    if (result == 0) { // SZ_OK
        std::cout << "成功: 库函数调用正常！" << std::endl;
        std::cout << "原始大小: " << srcLen << " bytes" << std::endl;
        std::cout << "压缩后大小: " << destLen << " bytes" << std::endl;
    }
    else {
        std::cerr << "失败: 错误代码 = " << result << std::endl;
        return 1;
    }

    return 0;
}

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

    std::cout << "7zip Version: " << MY_VER_MAJOR << "." << MY_VER_MINOR << "." << MY_VER_BUILD << std::endl;
    zipApi();

    return 0;
}
