# Testing Patterns

**Analysis Date:** 2026-03-21

## Test Framework

**Runner:**
- GoogleTest is the test framework.
- `tests/CMakeLists.txt` builds a single `asyncdownload_tests` executable and registers cases with
  `gtest_discover_tests(asyncdownload_tests)`.

**Assertion Library:**
- Use GoogleTest assertions such as `EXPECT_EQ`, `EXPECT_TRUE`, `ASSERT_FALSE`, and `ASSERT_NE`.

**Run Commands:**
```bat
scripts\build.bat
build\tests\Debug\AsyncDownload_tests.exe
build\tests\Debug\AsyncDownload_tests.exe --gtest_list_tests
build\tests\Debug\AsyncDownload_tests.exe --gtest_filter=DownloadIntegrationTest.ResumeAfterInterruptedCliDownload
```

## Test File Organization

**Location:**
- Tests live under `tests/` and are compiled recursively from that tree.
- Subsystem folders are used for grouping: `tests/core/`, `tests/download/`, `tests/persistence/`,
  `tests/storage/`, and `tests/support/`.
- Support code is kept beside the tests, such as `tests/support/range_server.py`.

**Naming:**
- Unit-style files use `*_test.cpp`, for example `tests/core/block_bitmap_test.cpp` and
  `tests/storage/file_writer_test.cpp`.
- End-to-end or scenario tests use `*_integration_test.cpp`, such as
  `tests/download/download_resume_integration_test.cpp`.

**Structure:**
```text
tests/
  core/
    block_bitmap_test.cpp
    block_bitmap_real_partition_test.cpp
    memory_accounting_test.cpp
  download/
    range_scheduler_test.cpp
    download_resume_integration_test.cpp
  persistence/
    persistence_thread_test.cpp
  storage/
    file_writer_test.cpp
    metadata_store_test.cpp
  support/
    range_server.py
```

## Test Structure

**Suite Organization:**
```cpp
TEST(BlockBitmapTest, MarksFullyCoveredBlocksFinished) {
    asyncdownload::core::AtomicBlockBitmap bitmap(4);
    bitmap.mark_finished_range(0, 128 * 1024, 64 * 1024, 256 * 1024);

    EXPECT_EQ(bitmap.load(0), asyncdownload::core::BlockState::finished);
}
```

**Patterns:**
- Most tests use one `TEST` per behavior and keep setup local to the test body.
- Shared helpers live in an anonymous namespace at the top of the file.
- `ASSERT_*` is used for setup that must succeed, and `EXPECT_*` is used for behavior checks.
- Tests that need platform gating use `#ifndef _WIN32` plus `GTEST_SKIP()`, as in
  `tests/download/download_resume_integration_test.cpp`.

## Mocking

**Framework:**
- No dedicated mocking framework is used.
- Tests prefer real components, temporary files, and actual helper processes over module mocks.

**Patterns:**
```cpp
asyncdownload::storage::FileWriter writer;
ASSERT_FALSE(writer.open(path, 128 * 1024, false, true));
writer.close();
```

**What to Mock:**
- External boundaries are exercised with real implementations where practical, including
  `FileWriter`, `MetadataStore`, and `DownloadClient`.
- The integration suite uses `tests/support/range_server.py` as a live HTTP server instead of
  stubbing network behavior.

**What NOT to Mock:**
- Core helpers such as `core/block_bitmap.cpp` and `core/memory_accounting.cpp` are tested
  directly.
- Pure data transformations are verified with real inputs and outputs, not synthetic mocks.

## Fixtures and Factories

**Test Data:**
```cpp
void write_test_file(const std::filesystem::path& path, std::size_t size_bytes);
bool wait_for_condition(const std::function<bool()>& predicate,
                        std::chrono::milliseconds timeout);
std::filesystem::path make_unique_temp_root(std::string_view prefix);
```

**Location:**
- Factory-style helpers are usually defined inside the test file near the tests that use them.
- Temporary directories come from `std::filesystem::temp_directory_path()`.
- Shared non-C++ fixtures are minimal; the notable reusable fixture is
  `tests/support/range_server.py`.

## Coverage

**Requirements:**
- No explicit coverage threshold or coverage gate was found in `CMakeLists.txt` or the test
  scripts.
- Coverage is practical/behavioral rather than percentage-driven.

**Configuration:**
- `tests/CMakeLists.txt` does not add coverage flags or a coverage target.
- Tests are intended to run from the built executable rather than through a separate harness.

**View Coverage:**
```bat
build\tests\Debug\AsyncDownload_tests.exe
```

## Test Types

**Unit Tests:**
- Most of `tests/core/`, `tests/storage/`, and `tests/download/range_scheduler_test.cpp` are
  narrow unit tests.
- These tests exercise `src/core/block_bitmap.cpp`, `src/core/memory_accounting.cpp`,
  `src/storage/file_writer.cpp`, and `src/metadata/metadata_store.cpp` directly.

**Integration Tests:**
- `tests/download/download_resume_integration_test.cpp` is the main integration file.
- It launches `build\src\Debug\AsyncDownload.exe` or the built CLI binary, starts
  `tests/support/range_server.py`, and verifies resume, config loading, port usage, and progress
  reporting.

**E2E Tests:**
- There is no separate E2E framework or directory.
- The Windows-only integration suite functions as the closest end-to-end coverage.

## Common Patterns

**Async Testing:**
```cpp
auto future = std::async(std::launch::async, [request]() mutable {
    asyncdownload::DownloadClient client;
    return client.download(request);
});
```

**Error Testing:**
```cpp
const auto result = client.download(request);
EXPECT_FALSE(result.ok());
EXPECT_EQ(result.error, asyncdownload::make_error_code(
    asyncdownload::DownloadErrc::invalid_request));
```

**Snapshot Testing:**
- Snapshot testing is not used.
- Tests prefer direct assertions on file contents, metadata state, and performance counters.

---

*Testing analysis: 2026-03-21*
*Update when test patterns change*
