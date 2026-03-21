# Technology Stack

**Analysis Date:** 2026-03-21

## Languages

**Primary:**
- C++20 - all production code under `src/` and public headers under `include/`

**Secondary:**
- CMake - build orchestration in `CMakeLists.txt`, `src/CMakeLists.txt`, `tests/CMakeLists.txt`
- JSON - configuration and metadata payloads in `configs/download_options.template.json` and `src/metadata/metadata_store.cpp`
- Python 3 - test HTTP range server in `tests/support/range_server.py`
- Batch scripting - build and run helpers under `scripts/`

## Runtime

**Environment:**
- Native desktop CLI runtime built from `src/main.cpp`
- Windows is the primary verified environment for the current test harness
- Integration tests use Win32 process APIs in `tests/download/download_resume_integration_test.cpp`

**Package Manager:**
- vcpkg manifest mode via `vcpkg.json`
- Lockfile: none checked in

## Frameworks

**Core:**
- None - this is a native C++ download library/CLI, not a web framework app

**Testing:**
- GoogleTest - unit and integration tests in `tests/`

**Build/Dev:**
- CMake 3.16+ - configured in `CMakeLists.txt`
- MSVC or GCC/Clang - compiler options are set in `CMakeLists.txt`
- vcpkg toolchain integration - enabled when `VCPKG_ROOT` points to a valid vcpkg install

## Key Dependencies

**Critical:**
- `curl` - HTTP probing and download transport via `libcurl` in `src/download/http_probe.cpp`
- `nlohmann-json` - CLI config parsing and metadata serialization in `src/main.cpp` and `src/metadata/metadata_store.cpp`
- `googletest` - test framework linked from `tests/CMakeLists.txt`

**Infrastructure:**
- `concurrentqueue` - header-only queue library available under `libs/concurrentqueue/include`
- `thread-pool` - header-only utility library available under `libs/thread-pool/include`
- `Windows API` - process control and pipe handling for integration tests in `tests/download/download_resume_integration_test.cpp`

## Configuration

**Environment:**
- `VCPKG_ROOT` - required so `CMakeLists.txt` can locate `scripts/buildsystems/vcpkg.cmake`
- `python` on PATH - required by Windows integration tests that launch `tests/support/range_server.py`
- CLI configuration is file-driven through `--config <path>` and `--summary-file <path>` in `src/main.cpp`

**Build:**
- `CMakeLists.txt` - top-level project configuration, compiler flags, and vcpkg toolchain hookup
- `src/CMakeLists.txt` - library and CLI targets
- `tests/CMakeLists.txt` - test target and GoogleTest discovery
- `configs/download_options.template.json` - user-facing `DownloadOptions` template

## Platform Requirements

**Development:**
- Windows development environment is the clearest fit for the current repo state
- Visual Studio 2022 is the documented IDE in `README.md`
- Python 3 is needed for the local range-server test fixture

**Production:**
- Native CLI executable; no cloud deployment target is defined in the repository
- The code is structured as a cross-platform C++ library, but the current repository’s exercised path is Windows desktop CLI

---

*Stack analysis: 2026-03-21*
*Update after major dependency changes*
