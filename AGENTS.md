# AGENTS.md - AsyncDownload Project Guide

## Project Overview

C++ project template using CMake + vcpkg + GoogleTest. A cross-platform async download library with HTTP support.

## Build Commands

### Basic Build
```batch
scripts\build.bat
```

### Release Build
```batch
scripts\build.bat release
```

### Clean Build
```batch
scripts\build.bat clean
```

### Run Executable
```batch
build\src\Debug\AsyncDownload.exe
```

### Run Tests
```batch
build\tests\Debug\AsyncDownload_tests.exe
```

### Run Single Test
```batch
build\tests\Debug\AsyncDownload_tests.exe --gtest_filter=SampleTest.BasicAssertion
```

### CMake Rebuild (after dependency changes)
```batch
rmdir /s /q build
scripts\build.bat
```

## Code Style Guidelines

### Language Standard
- C++20 required
- CMake 3.16+

### Compiler Options
- MSVC: `/utf-8 /W4 /permissive-`
- GCC/Clang: `-Wall -Wextra -pedantic`

### Naming Conventions
- Variables/functions: `snake_case`
- Classes: `PascalCase`
- Constants: `UPPER_SNAKE_CASE`
- File names: `snake_case.cpp`, `snake_case.hpp`

### Include Order
1. Standard library headers (`<iostream>`, `<vector>`, etc.)
2. Third-party headers (`<curl/curl.h>`, `<concurrentqueue/concurrentqueue.h>`)
3. Project headers (`"my_header.h"`)

### Formatting Rules
- Use 4 spaces for indentation (no tabs)
- Opening brace on same line
- Space around operators: `a + b`, not `a+b`
- No trailing whitespace
- Max line length: 100 characters

### Code Requirements
- **No comments** unless explicitly requested by user
- **No TODOs** or FIXME markers
- Use `const` wherever possible
- Prefer `enum class` over plain enums
- Use `std::optional` instead of sentinel values
- Use `[[nodiscard]]` for functions that must not be ignored

### Error Handling
- Return error codes (0 for success, non-zero for failure)
- Use `std::error_code` for complex error handling
- Never throw exceptions (project does not use RTTI)

### Dependencies

#### vcpkg Dependencies (vcpkg.json)
Add packages to `vcpkg.json`:
```json
{
  "dependencies": [
    "curl"
  ]
}
```

#### Header-only Libraries (libs/)
Place header-only libraries in `libs/<libname>/include/`.
Run `scripts\detect_libs.bat` to auto-detect.

Current libs:
- `concurrentqueue` - Lock-free queue
- `thread-pool` - Thread pool
- `googletest` - Unit testing

### Testing

#### Add Tests
1. Add test file to `tests/`
2. Modify `tests/CMakeLists.txt`

#### Test Framework
- GoogleTest
- Use `TEST(TestSuite, TestName)` format
- Include assertions: `EXPECT_EQ`, `EXPECT_TRUE`, `ASSERT_NE`, etc.

#### Example Test
```cpp
#include <gtest/gtest.h>

TEST(SampleTest, BasicAssertion) {
    EXPECT_EQ(1 + 1, 2);
}
```

### Project Structure
```
AsyncDownload/
├── CMakeLists.txt           # Root CMake config
├── vcpkg.json               # vcpkg dependencies
├── cmake/                   # CMake modules
│   ├── CompilerOptions.cmake
│   └── VcpkgDeps.cmake
├── src/                     # Source code
│   ├── CMakeLists.txt
│   └── main.cpp
├── include/                 # Public headers
├── libs/                   # Header-only libs
├── tests/                  # Unit tests
│   ├── CMakeLists.txt
│   └── main_test.cpp
└── scripts/                # Build scripts
```

### Important Notes
- Always build after code changes using `scripts\build.bat`
- Run the executable to verify functionality
- Run tests before committing changes
- Do not modify `.gitignore` or build artifacts
- Set `VCPKG_ROOT` environment variable before building

## Thread History Archive

- For any completed code change, documentation update, configuration edit, or AGENTS/rule update, use the local `history-archive` skill to append a record under `.agents/history/{thread_key}/`.
- If the user already provided a `thread_key` in the current thread, use it directly.
- If no `thread_key` is available yet, the first reply must ask: `本次会话的唯一标识是什么？`
- Do not invent a `thread_key`, and do not read or write another thread's archive unless the user explicitly requests it.
- Write both archive layers:
  - Index: `.agents/history/{thread_key}/index.json`
  - Detailed session archive: `.agents/history/{thread_key}/sessions/{timestamp}.md`
- Keep each archive record concise and high value. Include: `purpose`, `request_snapshot`, `summary`, `sources`, `changes`, `verification`, `next_step`, `session_archive`, and `status`.
- Merge multiple file edits from the same task into one archive record instead of writing fragmented entries.
- Perform the archive write as part of the normal workflow without interrupting the main task.
- After a successful archive write, end the reply with this status card:

---
代码改动已存档  
索引文件：`.agents/history/{thread_key}/index.json`  
详细存档：`.agents/history/{thread_key}/sessions/{timestamp}.md`  
本次记录：`{YYYY-MM-DD} {HH:mm:ss} - {purpose}`  
改动文件：`{file1}, {file2}, ...`  
存储结果：`success`
---

## Documentation Placement Rules

- These rules apply to newly created documents only. Existing documents do not need to be moved just to satisfy this rule.
- All newly created project documents must be placed under `docs/`.
- Do not place new topic-specific documents directly under `docs/`.
- When creating a new document under `docs/`, first check whether an existing topic directory already matches the document.
- If a matching topic directory already exists, place the new document in that directory.
- If no suitable topic directory exists, create a new topic directory first, then place the document inside it.
- Organize documentation by topic, not by date, temporary task name, or generic document type.
- Directory names under `docs/` should use lowercase English names.
- Document file names should prefer `snake_case`. Existing filename patterns such as `_zh.md` are allowed and may continue to be used.
- The only documents that may be placed directly under `docs/` are top-level overview documents:
  - README-style overviews
  - Navigation or index pages
  - Overall architecture overviews
- Example: performance-related documents should be placed under `docs/performance/` if that directory already exists; otherwise create it and place the new document there.
