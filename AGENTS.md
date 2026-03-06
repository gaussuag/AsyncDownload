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
- C++17 required
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
