# Phase 1: Telemetry Skeleton - Plans

## Plan Index

| # | Plan | Description |
|---|------|-------------|
| 1 | [01-01-PLAN.md](01-01-PLAN.md) | Set up directory structure and CMake configuration |
| 2 | [01-02-PLAN.md](01-02-PLAN.md) | Define TelemetryEventType enum and TelemetryPayload structures |
| 3 | [01-03-PLAN.md](01-03-PLAN.md) | Implement TelemetrySink with moodycamel queue |
| 4 | [01-04-PLAN.md](01-04-PLAN.md) | Implement TelemetryCollector stub |
| 5 | [01-05-PLAN.md](01-05-PLAN.md) | Implement TelemetrySession facade |
| 6 | [01-06-PLAN.md](01-06-PLAN.md) | Add unit tests for skeleton components |

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
