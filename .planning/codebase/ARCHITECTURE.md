# Architecture

**Analysis Date:** 2026-03-21

## Pattern Overview

**Overall:** Layered CLI download engine with a split network/persistence pipeline

**Key Characteristics:**
- Single CLI executable drives the whole task flow through `src/main.cpp`
- Public library API in `include/asyncdownload/` fronts the internal engine
- Network fetch, scheduling, disk persistence, and resume validation are separated
- Runtime state is task-local and resumed through `.part` plus `.config.json` files

## Layers

**CLI and API Layer:**
- Purpose: Parse user input, load optional config, start a download, and print progress/summary
- Contains: `src/main.cpp`, `src/client.cpp`, `include/asyncdownload/client.hpp`, `include/asyncdownload/types.hpp`
- Depends on: Public request/result types, engine entry point, JSON config parsing, console I/O
- Used by: End users invoking `AsyncDownload` and tests that exercise the public API

**Orchestration Layer:**
- Purpose: Turn a request into a complete download session and coordinate all subsystems
- Contains: `src/download/download_engine.cpp`, `src/download/download_engine.hpp`
- Depends on: Probe, scheduler, bitmap, file writer, metadata store, persistence thread, curl, thread pool, queue
- Used by: `asyncdownload::DownloadClient::download`

**Transfer Planning Layer:**
- Purpose: Probe remote capabilities, build ranges, and keep requests aligned to resume and work stealing rules
- Contains: `src/download/http_probe.cpp`, `src/download/http_probe.hpp`, `src/download/range_scheduler.cpp`, `src/download/range_scheduler.hpp`
- Depends on: `src/core/models.hpp`, `src/core/block_bitmap.hpp`, `src/core/alignment.hpp`, libcurl
- Used by: Download engine during session setup and main event loop

**Persistence Layer:**
- Purpose: Serialize packets into ordered file writes, maintain VDL/bitmap progress, and save recovery metadata
- Contains: `src/persistence/persistence_thread.cpp`, `src/persistence/persistence_thread.hpp`, `src/storage/file_writer.cpp`, `src/storage/file_writer.hpp`, `src/metadata/metadata_store.cpp`, `src/metadata/metadata_store.hpp`
- Depends on: Session state, bitmap, file writer, metadata JSON, worker pool, queue
- Used by: Download engine as the only writer of persistent download state

**Core Model and Utility Layer:**
- Purpose: Define shared state, error codes, metrics, alignment helpers, checksum helpers, and memory accounting
- Contains: `src/core/models.hpp`, `src/core/error.cpp`, `src/core/block_bitmap.cpp`, `src/core/memory_accounting.cpp`, `src/core/path_utils.cpp`, `src/core/crc32.cpp`
- Depends on: Standard library only
- Used by: Every higher layer

## Data Flow

**Download Session Execution:**

1. User runs `AsyncDownload <url> <output> [connections] [--config <path>]`
2. `src/main.cpp` parses CLI arguments, merges optional JSON config, and builds `asyncdownload::DownloadRequest`
3. `asyncdownload::DownloadClient::download` forwards the request to `src/download/download_engine.cpp`
4. The engine probes the remote URL, builds `core::SessionState`, and derives `temporary_path` plus `metadata_path`
5. `src/metadata/metadata_store.cpp` loads any prior resume snapshot and `src/storage/file_writer.cpp` opens the `.part` file
6. `src/core/block_bitmap.cpp` restores and validates block state for resume, then `src/download/range_scheduler.cpp` builds initial ranges
7. The engine starts `src/persistence/persistence_thread.cpp`, then drives libcurl multi-handle transfers
8. Curl write callbacks enqueue `core::DataPacket` objects into `moodycamel::BlockingConcurrentQueue`
9. The persistence thread drains packets, writes ordered bytes through `FileWriter`, advances the bitmap, flushes, and saves metadata snapshots
10. When all work completes, the engine flushes, finalizes the file, removes stale metadata, and returns `DownloadResult`

**State Management:**
- Session state lives in `src/core/models.hpp::core::SessionState`
- Task-local progress uses atomics for byte counters, pause counters, and performance metrics
- Resume state is persisted in `.part` and `.config.json` files adjacent to the final output path
- `src/core/block_bitmap.cpp` is the canonical source for durable block completion status

## Key Abstractions

**DownloadRequest / DownloadResult:**
- Purpose: Public task input and output contract
- Examples: `asyncdownload::DownloadRequest`, `asyncdownload::DownloadResult`
- Pattern: Value objects with no ownership of internal workers

**SessionState / RangeContext / DataPacket:**
- Purpose: Runtime task graph for range scheduling, packet flow, and persistence
- Examples: `core::SessionState`, `core::RangeContext`, `core::DataPacket`
- Pattern: Shared mutable state with atomic coordination

**MetadataState / RangeStateSnapshot / BlockCrcSample:**
- Purpose: Compact recovery snapshot for restart and integrity validation
- Examples: `core::MetadataState`, `core::RangeStateSnapshot`, `core::BlockCrcSample`
- Pattern: Serialization-friendly data transfer objects

**AtomicBlockBitmap:**
- Purpose: Track which file blocks are empty, downloading, or finished
- Examples: `src/core/block_bitmap.hpp`
- Pattern: Atomic state machine over fixed-size blocks

**FileWriter / MetadataStore:**
- Purpose: Abstract file I/O and metadata persistence behind simple error-code APIs
- Examples: `storage::FileWriter`, `metadata::MetadataStore`
- Pattern: RAII resource wrappers

**PerformanceSummary / RuntimePerformanceMetrics:**
- Purpose: Capture runtime counters, peaks, and derived summary values
- Examples: `include/asyncdownload/performance_metrics.hpp`, `include/asyncdownload/types.hpp`
- Pattern: Structured metrics with direct copy plus derived aggregation

## Entry Points

**CLI Entry:**
- Location: `src/main.cpp`
- Triggers: Direct executable invocation
- Responsibilities: Parse args, load config, start download, stream progress, print summary, return exit code

**Library Entry:**
- Location: `src/client.cpp`, `include/asyncdownload/client.hpp`
- Triggers: Consumers calling `asyncdownload::DownloadClient::download`
- Responsibilities: Forward request into the orchestration layer without exposing internals

## Error Handling

**Strategy:** Return `std::error_code` from the public API and collapse unexpected exceptions to `internal_error` in the engine

**Patterns:**
- `src/core/error.cpp` maps `asyncdownload::DownloadErrc` to stable messages
- `src/download/download_engine.cpp` validates request, probe, resume, and file setup stages before entering the loop
- `src/main.cpp` reports failures to `stderr` and exits with status `1`
- Persistence and I/O helpers return codes instead of throwing

## Cross-Cutting Concerns

**Logging:**
- Console progress and summaries are emitted from `src/main.cpp`
- Errors are reported on `stderr`; progress updates use carriage-return style live output

**Validation:**
- CLI arguments are checked in `src/main.cpp`
- Config values are parsed from JSON and validated before the request is executed
- Resume compatibility is checked through URL, output paths, probe metadata, and snapshot data

**Metrics and Profiling:**
- Runtime counters live in `include/asyncdownload/performance_metrics.hpp`
- Summaries are copied and derived in `src/download/download_engine.cpp`
- Final presentation is produced in `src/main.cpp::write_summary`

**Persistence Conventions:**
- Final file is promoted from `*.part`
- Resume metadata is stored as `*.config.json`
- Helper path construction is centralized in `src/core/path_utils.cpp`

---

*Architecture analysis: 2026-03-21*
*Update when major patterns change*
