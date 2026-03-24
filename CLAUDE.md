<!-- GSD:project-start source:PROJECT.md -->
## Project

**AsyncDownload Telemetry Refactoring**

Refactoring the AsyncDownload C++ library's performance metrics system into a decoupled Telemetry module. Current implementation couples download logic with telemetry state and calculations. Target state: clean separation where download code emits semantic events, Telemetry module handles all metrics aggregation and export.

**Core Value:** Maintain 100% backward compatibility for `benchmark.py` and `profiler.py` while achieving clean architectural separation between download functionality and telemetry concerns.

### Constraints

- **C++20**: Must use modern C++ features
- **No RTTI/exceptions**: Project convention, no exceptions allowed
- **Backward compatibility**: benchmark.py and profiler.py must produce identical output
- **Thread safety**: Multi-producer queue, single consumer model
- **Memory bounded**: Event struct sizes must be controlled
<!-- GSD:project-end -->

<!-- GSD:stack-start source:codebase/STACK.md -->
## Technology Stack

## Languages
- C++20 - all production code under `src/` and public headers under `include/`
- CMake - build orchestration in `CMakeLists.txt`, `src/CMakeLists.txt`, `tests/CMakeLists.txt`
- JSON - configuration and metadata payloads in `configs/download_options.template.json` and `src/metadata/metadata_store.cpp`
- Python 3 - test HTTP range server in `tests/support/range_server.py`
- Batch scripting - build and run helpers under `scripts/`
## Runtime
- Native desktop CLI runtime built from `src/main.cpp`
- Windows is the primary verified environment for the current test harness
- Integration tests use Win32 process APIs in `tests/download/download_resume_integration_test.cpp`
- vcpkg manifest mode via `vcpkg.json`
- Lockfile: none checked in
## Frameworks
- None - this is a native C++ download library/CLI, not a web framework app
- GoogleTest - unit and integration tests in `tests/`
- CMake 3.16+ - configured in `CMakeLists.txt`
- MSVC or GCC/Clang - compiler options are set in `CMakeLists.txt`
- vcpkg toolchain integration - enabled when `VCPKG_ROOT` points to a valid vcpkg install
## Key Dependencies
- `curl` - HTTP probing and download transport via `libcurl` in `src/download/http_probe.cpp`
- `nlohmann-json` - CLI config parsing and metadata serialization in `src/main.cpp` and `src/metadata/metadata_store.cpp`
- `googletest` - test framework linked from `tests/CMakeLists.txt`
- `concurrentqueue` - header-only queue library available under `libs/concurrentqueue/include`
- `thread-pool` - header-only utility library available under `libs/thread-pool/include`
- `Windows API` - process control and pipe handling for integration tests in `tests/download/download_resume_integration_test.cpp`
## Configuration
- `VCPKG_ROOT` - required so `CMakeLists.txt` can locate `scripts/buildsystems/vcpkg.cmake`
- `python` on PATH - required by Windows integration tests that launch `tests/support/range_server.py`
- CLI configuration is file-driven through `--config <path>` and `--summary-file <path>` in `src/main.cpp`
- `CMakeLists.txt` - top-level project configuration, compiler flags, and vcpkg toolchain hookup
- `src/CMakeLists.txt` - library and CLI targets
- `tests/CMakeLists.txt` - test target and GoogleTest discovery
- `configs/download_options.template.json` - user-facing `DownloadOptions` template
## Platform Requirements
- Windows development environment is the clearest fit for the current repo state
- Visual Studio 2022 is the documented IDE in `README.md`
- Python 3 is needed for the local range-server test fixture
- Native CLI executable; no cloud deployment target is defined in the repository
- The code is structured as a cross-platform C++ library, but the current repository’s exercised path is Windows desktop CLI
<!-- GSD:stack-end -->

<!-- GSD:conventions-start source:CONVENTIONS.md -->
## Conventions

## Naming Patterns
- `snake_case.cpp` and `snake_case.hpp` are the dominant source patterns in `src/` and
- Test files use `snake_case_test.cpp` or `snake_case_integration_test.cpp`, such as
- Helper scripts keep their native naming, such as `scripts/build.bat` and
- Use `snake_case` for free functions, member functions, and helpers, as seen in
- Status-returning functions are commonly marked `[[nodiscard]]`, especially on public APIs such as
- Anonymous-namespace helpers are common for file-local parsing and utility code.
- Use `snake_case` for locals, parameters, and members.
- Private data members typically end with `_`, for example `block_count_`, `states_`, and
- `constexpr` and `const` locals are preferred over mutable temporaries when possible.
- Use `PascalCase` for classes, structs, and type aliases, such as `DownloadClient`,
- Prefer `enum class` over plain enums, as in `asyncdownload::core::BlockState` and
- Enum values in this repository are written in lower snake case, for example
## Code Style
- C++20 is the project standard, enforced in `CMakeLists.txt`.
- Indentation is 4 spaces, braces stay on the same line, and line wrapping is used for long
- Include order follows standard headers, then third-party headers, then project headers, as seen
- No repo-local `.clang-format` or `.editorconfig` file was observed; style is driven by the
- `CMakeLists.txt` enables `/W4 /permissive-` on MSVC and `-Wall -Wextra -pedantic` elsewhere.
- `scripts/build.bat` is the primary build entry point and is the practical verification command
- No separate lint target or formatter task was found in the repository.
## Import Organization
- Keep a blank line between the standard, third-party, and project groups.
- Within each group, the codebase mostly keeps related headers together rather than aggressively
- No include alias system was found. Public headers live under `include/asyncdownload/`, while
## Error Handling
- Public operations return `std::error_code`, `bool`, or result objects instead of throwing.
- `include/asyncdownload/error.hpp` defines `DownloadErrc`, and
- `DownloadResult::ok()` is the normal success check, and callers branch on it in
- `std::optional` is used for absent values, such as config paths and optional load results.
- Use `std::error_code` for recoverable failures at subsystem boundaries, as in
- Use `enum class` error codes for project-specific failures instead of exceptions.
- The CLI reports parsing and runtime errors through `std::cerr` and returns non-zero exit codes.
## Logging
- There is no dedicated logging library in the current codebase.
- The CLI uses `std::cout` for progress and summary output and `std::cerr` for failures in
- Keep logging at boundaries such as the CLI and test harnesses.
- Core library code generally stays silent and reports state through return values instead of
## Comments
- Existing code uses explanatory comments heavily in implementation files such as
- Comments tend to explain recovery semantics, state transitions, or platform-specific behavior.
- Avoid comments that restate obvious code; only add them when the behavior is non-obvious.
- No formal doc-comment style was observed. Public headers rely on terse inline comments where
- No `TODO` or `FIXME` markers were observed in the current source tree.
## Function Design
- Prefer small helper functions and file-local utilities in anonymous namespaces.
- The codebase regularly breaks parsing and summary formatting into many focused helpers, as in
- Keep parameter lists short where possible; related inputs are grouped into structs such as
- Return early on validation failures.
- Use explicit success/failure return values rather than relying on exceptions.
## Module Design
- Public API surface lives in `include/asyncdownload/`.
- Internal implementation headers remain under `src/` and are not treated as public exports.
- No `index.hpp` or similar barrel-file pattern was observed.
- Include the specific public header you need rather than relying on re-export layers.
<!-- GSD:conventions-end -->

<!-- GSD:architecture-start source:ARCHITECTURE.md -->
## Architecture

## Pattern Overview
- Single CLI executable drives the whole task flow through `src/main.cpp`
- Public library API in `include/asyncdownload/` fronts the internal engine
- Network fetch, scheduling, disk persistence, and resume validation are separated
- Runtime state is task-local and resumed through `.part` plus `.config.json` files
## Layers
- Purpose: Parse user input, load optional config, start a download, and print progress/summary
- Contains: `src/main.cpp`, `src/client.cpp`, `include/asyncdownload/client.hpp`, `include/asyncdownload/types.hpp`
- Depends on: Public request/result types, engine entry point, JSON config parsing, console I/O
- Used by: End users invoking `AsyncDownload` and tests that exercise the public API
- Purpose: Turn a request into a complete download session and coordinate all subsystems
- Contains: `src/download/download_engine.cpp`, `src/download/download_engine.hpp`
- Depends on: Probe, scheduler, bitmap, file writer, metadata store, persistence thread, curl, thread pool, queue
- Used by: `asyncdownload::DownloadClient::download`
- Purpose: Probe remote capabilities, build ranges, and keep requests aligned to resume and work stealing rules
- Contains: `src/download/http_probe.cpp`, `src/download/http_probe.hpp`, `src/download/range_scheduler.cpp`, `src/download/range_scheduler.hpp`
- Depends on: `src/core/models.hpp`, `src/core/block_bitmap.hpp`, `src/core/alignment.hpp`, libcurl
- Used by: Download engine during session setup and main event loop
- Purpose: Serialize packets into ordered file writes, maintain VDL/bitmap progress, and save recovery metadata
- Contains: `src/persistence/persistence_thread.cpp`, `src/persistence/persistence_thread.hpp`, `src/storage/file_writer.cpp`, `src/storage/file_writer.hpp`, `src/metadata/metadata_store.cpp`, `src/metadata/metadata_store.hpp`
- Depends on: Session state, bitmap, file writer, metadata JSON, worker pool, queue
- Used by: Download engine as the only writer of persistent download state
- Purpose: Define shared state, error codes, metrics, alignment helpers, checksum helpers, and memory accounting
- Contains: `src/core/models.hpp`, `src/core/error.cpp`, `src/core/block_bitmap.cpp`, `src/core/memory_accounting.cpp`, `src/core/path_utils.cpp`, `src/core/crc32.cpp`
- Depends on: Standard library only
- Used by: Every higher layer
## Data Flow
- Session state lives in `src/core/models.hpp::core::SessionState`
- Task-local progress uses atomics for byte counters, pause counters, and performance metrics
- Resume state is persisted in `.part` and `.config.json` files adjacent to the final output path
- `src/core/block_bitmap.cpp` is the canonical source for durable block completion status
## Key Abstractions
- Purpose: Public task input and output contract
- Examples: `asyncdownload::DownloadRequest`, `asyncdownload::DownloadResult`
- Pattern: Value objects with no ownership of internal workers
- Purpose: Runtime task graph for range scheduling, packet flow, and persistence
- Examples: `core::SessionState`, `core::RangeContext`, `core::DataPacket`
- Pattern: Shared mutable state with atomic coordination
- Purpose: Compact recovery snapshot for restart and integrity validation
- Examples: `core::MetadataState`, `core::RangeStateSnapshot`, `core::BlockCrcSample`
- Pattern: Serialization-friendly data transfer objects
- Purpose: Track which file blocks are empty, downloading, or finished
- Examples: `src/core/block_bitmap.hpp`
- Pattern: Atomic state machine over fixed-size blocks
- Purpose: Abstract file I/O and metadata persistence behind simple error-code APIs
- Examples: `storage::FileWriter`, `metadata::MetadataStore`
- Pattern: RAII resource wrappers
- Purpose: Capture runtime counters, peaks, and derived summary values
- Examples: `include/asyncdownload/performance_metrics.hpp`, `include/asyncdownload/types.hpp`
- Pattern: Structured metrics with direct copy plus derived aggregation
## Entry Points
- Location: `src/main.cpp`
- Triggers: Direct executable invocation
- Responsibilities: Parse args, load config, start download, stream progress, print summary, return exit code
- Location: `src/client.cpp`, `include/asyncdownload/client.hpp`
- Triggers: Consumers calling `asyncdownload::DownloadClient::download`
- Responsibilities: Forward request into the orchestration layer without exposing internals
## Error Handling
- `src/core/error.cpp` maps `asyncdownload::DownloadErrc` to stable messages
- `src/download/download_engine.cpp` validates request, probe, resume, and file setup stages before entering the loop
- `src/main.cpp` reports failures to `stderr` and exits with status `1`
- Persistence and I/O helpers return codes instead of throwing
## Cross-Cutting Concerns
- Console progress and summaries are emitted from `src/main.cpp`
- Errors are reported on `stderr`; progress updates use carriage-return style live output
- CLI arguments are checked in `src/main.cpp`
- Config values are parsed from JSON and validated before the request is executed
- Resume compatibility is checked through URL, output paths, probe metadata, and snapshot data
- Runtime counters live in `include/asyncdownload/performance_metrics.hpp`
- Summaries are copied and derived in `src/download/download_engine.cpp`
- Final presentation is produced in `src/main.cpp::write_summary`
- Final file is promoted from `*.part`
- Resume metadata is stored as `*.config.json`
- Helper path construction is centralized in `src/core/path_utils.cpp`
<!-- GSD:architecture-end -->

<!-- GSD:workflow-start source:GSD defaults -->
## GSD Workflow Enforcement

Before using Edit, Write, or other file-changing tools, start work through a GSD command so planning artifacts and execution context stay in sync.

Use these entry points:
- `/gsd:quick` for small fixes, doc updates, and ad-hoc tasks
- `/gsd:debug` for investigation and bug fixing
- `/gsd:execute-phase` for planned phase work

Do not make direct repo edits outside a GSD workflow unless the user explicitly asks to bypass it.
<!-- GSD:workflow-end -->



<!-- GSD:profile-start -->
## Developer Profile

> Profile not yet configured. Run `/gsd:profile-user` to generate your developer profile.
> This section is managed by `generate-claude-profile` -- do not edit manually.
<!-- GSD:profile-end -->
