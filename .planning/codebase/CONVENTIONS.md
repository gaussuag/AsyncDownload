# Coding Conventions

**Analysis Date:** 2026-03-21

## Naming Patterns

**Files:**
- `snake_case.cpp` and `snake_case.hpp` are the dominant source patterns in `src/` and
  `include/asyncdownload/`, for example `src/core/block_bitmap.cpp` and
  `include/asyncdownload/client.hpp`.
- Test files use `snake_case_test.cpp` or `snake_case_integration_test.cpp`, such as
  `tests/core/block_bitmap_test.cpp` and
  `tests/download/download_resume_integration_test.cpp`.
- Helper scripts keep their native naming, such as `scripts/build.bat` and
  `tests/support/range_server.py`.

**Functions:**
- Use `snake_case` for free functions, member functions, and helpers, as seen in
  `src/main.cpp` and `src/core/block_bitmap.cpp`.
- Status-returning functions are commonly marked `[[nodiscard]]`, especially on public APIs such as
  `include/asyncdownload/client.hpp` and `src/storage/file_writer.hpp`.
- Anonymous-namespace helpers are common for file-local parsing and utility code.

**Variables:**
- Use `snake_case` for locals, parameters, and members.
- Private data members typically end with `_`, for example `block_count_`, `states_`, and
  `handle_` in `src/core/block_bitmap.hpp` and `src/storage/file_writer.hpp`.
- `constexpr` and `const` locals are preferred over mutable temporaries when possible.

**Types:**
- Use `PascalCase` for classes, structs, and type aliases, such as `DownloadClient`,
  `DownloadOptions`, and `RangeScheduler`.
- Prefer `enum class` over plain enums, as in `asyncdownload::core::BlockState` and
  `asyncdownload::DownloadErrc`.
- Enum values in this repository are written in lower snake case, for example
  `BlockState::downloading` and `DownloadErrc::invalid_request`.

## Code Style

**Formatting:**
- C++20 is the project standard, enforced in `CMakeLists.txt`.
- Indentation is 4 spaces, braces stay on the same line, and line wrapping is used for long
  declarations and output chains.
- Include order follows standard headers, then third-party headers, then project headers, as seen
  in `src/main.cpp`, `src/core/block_bitmap.cpp`, and `tests/core/block_bitmap_test.cpp`.
- No repo-local `.clang-format` or `.editorconfig` file was observed; style is driven by the
  existing source and CMake compiler settings.

**Linting:**
- `CMakeLists.txt` enables `/W4 /permissive-` on MSVC and `-Wall -Wextra -pedantic` elsewhere.
- `scripts/build.bat` is the primary build entry point and is the practical verification command
  after edits.
- No separate lint target or formatter task was found in the repository.

## Import Organization

**Order:**
1. Standard library headers.
2. Third-party headers such as `<nlohmann/json.hpp>`, `<gtest/gtest.h>`,
   `<concurrentqueue/blockingconcurrentqueue.h>`, and `<thread-pool/BS_thread_pool.hpp>`.
3. Project headers like `"asyncdownload/client.hpp"` and `"core/block_bitmap.hpp"`.

**Grouping:**
- Keep a blank line between the standard, third-party, and project groups.
- Within each group, the codebase mostly keeps related headers together rather than aggressively
  sorting alphabetically.

**Path Aliases:**
- No include alias system was found. Public headers live under `include/asyncdownload/`, while
  internal headers stay under `src/`.

## Error Handling

**Patterns:**
- Public operations return `std::error_code`, `bool`, or result objects instead of throwing.
- `include/asyncdownload/error.hpp` defines `DownloadErrc`, and
  `src/core/error.cpp` maps those values to a custom error category.
- `DownloadResult::ok()` is the normal success check, and callers branch on it in
  `src/main.cpp` and integration tests.
- `std::optional` is used for absent values, such as config paths and optional load results.

**Error Types:**
- Use `std::error_code` for recoverable failures at subsystem boundaries, as in
  `src/storage/file_writer.cpp` and `src/metadata/metadata_store.cpp`.
- Use `enum class` error codes for project-specific failures instead of exceptions.
- The CLI reports parsing and runtime errors through `std::cerr` and returns non-zero exit codes.

## Logging

**Framework:**
- There is no dedicated logging library in the current codebase.
- The CLI uses `std::cout` for progress and summary output and `std::cerr` for failures in
  `src/main.cpp`.

**Patterns:**
- Keep logging at boundaries such as the CLI and test harnesses.
- Core library code generally stays silent and reports state through return values instead of
  logging side effects.

## Comments

**When to Comment:**
- Existing code uses explanatory comments heavily in implementation files such as
  `src/core/block_bitmap.cpp`, `src/storage/file_writer.hpp`, and `src/download/range_scheduler.hpp`.
- Comments tend to explain recovery semantics, state transitions, or platform-specific behavior.
- Avoid comments that restate obvious code; only add them when the behavior is non-obvious.

**JSDoc/TSDoc:**
- No formal doc-comment style was observed. Public headers rely on terse inline comments where
  needed.

**TODO Comments:**
- No `TODO` or `FIXME` markers were observed in the current source tree.

## Function Design

**Size:**
- Prefer small helper functions and file-local utilities in anonymous namespaces.
- The codebase regularly breaks parsing and summary formatting into many focused helpers, as in
  `src/main.cpp`.

**Parameters:**
- Keep parameter lists short where possible; related inputs are grouped into structs such as
  `DownloadRequest` and `DownloadOptions`.

**Return Values:**
- Return early on validation failures.
- Use explicit success/failure return values rather than relying on exceptions.

## Module Design

**Exports:**
- Public API surface lives in `include/asyncdownload/`.
- Internal implementation headers remain under `src/` and are not treated as public exports.

**Barrel Files:**
- No `index.hpp` or similar barrel-file pattern was observed.
- Include the specific public header you need rather than relying on re-export layers.

---

*Convention analysis: 2026-03-21*
*Update when patterns change*
