# Plan 1: Create Telemetry Module Directory Structure

## Task Description

Set up the directory structure and CMake configuration for the new Telemetry module. Create the public header aggregation file that exposes all telemetry components.

## Context

Phase 1 of 5: Telemetry Skeleton. This plan establishes the file organization per D-01 through D-04 in the context:
- Public headers in `include/asyncdownload/telemetry/`
- Implementation in `src/telemetry/`
- Main public header: `include/asyncdownload/telemetry.hpp`

## Implementation Notes

1. Create directory `include/asyncdownload/telemetry/`
2. Create directory `src/telemetry/`
3. Create `include/asyncdownload/telemetry.hpp` that includes and re-exports all telemetry components
4. Modify `src/CMakeLists.txt` to include `src/telemetry/*.cpp` files

## Files to Create/Modify

- `include/asyncdownload/telemetry.hpp` (new)
- `src/CMakeLists.txt` (modify)

## Verification

1. Build succeeds with new header structure
2. `include/asyncdownload/telemetry.hpp` can be included without errors
