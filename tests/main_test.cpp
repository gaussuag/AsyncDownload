#include <gtest/gtest.h>
#include <concurrentqueue/concurrentqueue.h>
#include <thread-pool/BS_thread_pool.hpp>
#include <curl/curl.h>

TEST(SampleTest, BasicAssertion) {
    EXPECT_EQ(1 + 1, 2);
}

TEST(SampleTest, FalseAssertion) {
    EXPECT_NE(1, 2);
}

TEST(ConcurrentQueueTest, PushPop) {
    moodycamel::ConcurrentQueue<int> queue;
    
    queue.enqueue(1);
    queue.enqueue(2);
    queue.enqueue(3);
    
    int value;
    EXPECT_TRUE(queue.try_dequeue(value));
    EXPECT_EQ(value, 1);
    
    EXPECT_TRUE(queue.try_dequeue(value));
    EXPECT_EQ(value, 2);
    
    EXPECT_TRUE(queue.try_dequeue(value));
    EXPECT_EQ(value, 3);
    
    EXPECT_FALSE(queue.try_dequeue(value));
}

TEST(ThreadPoolTest, SubmitTask) {
    BS::thread_pool pool(2);
    
    auto result = pool.submit_task([]() {
        return 42;
    });
    
    EXPECT_EQ(result.get(), 42);
    pool.wait();
}

TEST(CurlTest, Version) {
    curl_version_info_data* version = curl_version_info(CURLVERSION_NOW);
    EXPECT_NE(version, nullptr);
    
    if (version) {
        EXPECT_STRNE(version->version, "");
    }
}

TEST(CurlTest, EasyInit) {
    CURL* curl = curl_easy_init();
    EXPECT_NE(curl, nullptr);
    if (curl) {
        curl_easy_cleanup(curl);
    }
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
