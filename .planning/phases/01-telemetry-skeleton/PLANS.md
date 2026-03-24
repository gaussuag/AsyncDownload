# Phase 1: Telemetry Skeleton - Plans

## Plan Index

| # | Plan | Description |
|---|------|-------------|
| 1 | [01-create-directory-structure.md](plans/01-create-directory-structure.md) | Set up directory structure and CMake configuration |
| 2 | [02-implement-telemetry-event.md](plans/02-implement-telemetry-event.md) | Define TelemetryEventType enum and TelemetryPayload structures |
| 3 | [03-implement-telemetry-sink.md](plans/03-implement-telemetry-sink.md) | Implement TelemetrySink with moodycamel queue |
| 4 | [04-implement-telemetry-collector.md](plans/04-implement-telemetry-collector.md) | Implement TelemetryCollector stub |
| 5 | [05-implement-telemetry-session.md](plans/05-implement-telemetry-session.md) | Implement TelemetrySession facade |
| 6 | [06-add-unit-tests.md](plans/06-add-unit-tests.md) | Add unit tests for skeleton components |

## Coverage

| Requirement | Plan |
|-------------|------|
| TELE-01: TelemetryEvent enum | Plan 2 |
| TELE-02: TelemetryPayload fixed-size | Plan 2 |
| TELE-03: TelemetrySink with queue | Plan 3 |
| TELE-04: TelemetryCollector | Plan 4 |
| TELE-05: TelemetrySession facade | Plan 5 |
| TELE-06: steady_clock timestamps | Plans 2, 5 |

## Execution Order

1. Plan 1 (directory structure) must complete first
2. Plans 2-5 can proceed in parallel once Plan 1 is done
3. Plan 6 (tests) should run last after all implementation
