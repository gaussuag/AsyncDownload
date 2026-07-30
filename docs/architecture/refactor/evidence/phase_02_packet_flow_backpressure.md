# Phase 2 Evidence: Packet Flow / Backpressure

## Identity

| Item | Value |
| --- | --- |
| Stage | 2 — Packet Flow / Backpressure |
| Base commit | `eca01fc` |
| Branch | `codex/refactor-02-packet-flow` |
| First test commit | `55fa79a` |
| Last production fix | `15054e1` |
| Performance evidence | `8fefc18` |
| Change classes | S, C, test, perf |

The whole-stage rollback anchor is `eca01fc`. Dependent slices are reverted in reverse order.

## Slice ledger

| Commit | Class | Slice and rollback unit |
| --- | --- | --- |
| `55fa79a` | test | Characterize ownership, vendor capacity, ordering, pause, accounting, and replay |
| `de684b6` | S | Add dependency-neutral range values, Packet Flow interface, and private queue seam |
| `225c38a` | S | Centralize lane draft and move-only lease ownership |
| `868b6af` | S | Migrate the network callback to `PacketProducer` |
| `a890a41` | S | Migrate persistence to `PacketConsumer` and `PacketLease` |
| `285471c` | C | Map construction, arithmetic, allocation, and backend failures |
| `6f0de65` | C | Enforce request-policy logical Data Packet credits |
| `8fa0b17` | C | Replace global accounting with a request-local checked ledger |
| `e72300b` | C | Serialize completion/close and drain admitted drafts during stop |
| `006512c` | S | Centralize Queue/Memory pause state and low-watermark resume |
| `385ff4b` | S | Remove compatibility queues, counters, helpers, and shutdown packets |
| `878f9f4` | C | Linearize active producer operations with consumer failure |
| `3500166` | test | Exercise exact output replay with logical packet budget one |
| `2bab007` | test | Add the separately compiled, test-only deterministic fault adapter |
| `634353e` | test | Stress 300,000 packets across budgets 1, 2, and 33 |
| `15054e1` | C | Enforce owner-thread invariants and 100-round close/fail races |
| `9a1cef1` | test | Classify and clean up the Windows Debug elevation limitation |
| `8fefc18` | perf | Record the Release keeper and risk-probe result |

Each row is an independent Git rollback point. The four planned semantic correction units are
`285471c`, `6f0de65`, `8fa0b17`, and `e72300b`; the later `878f9f4` and `15054e1` harden the
failure linearization and owner-thread contract without changing policy defaults.

## Contract result

- The Packet Flow interface exposes no vendor types and is not included by public headers.
- Production has one concrete moodycamel adapter, one explicit producer token, one consumer token,
  and an implicit-producer hash size of zero.
- The deterministic fault adapter is compiled only into
  `asyncdownload_packet_flow_fault_tests`; it is not linked into the production library or CLI.
- Data admission uses a checked logical hard budget. Control and close use the same explicit
  producer stream and are not silently dropped because Data credits are exhausted.
- Vendor no-allocation failure is distinct from logical budget exhaustion.
- Each lane owns one contiguous draft with the existing 64 KiB aggregation target.
- `PacketLease` is move-only and returns request-local accounted bytes exactly once.
- Queue credits are returned at receive; payload and reorder accounting remain owned until lease
  completion.
- Reorder ownership adds exactly 48 bytes once.
- Completion follows all data for its lease, close follows all regular packets, and the consumer
  observes strictly increasing sequences.
- Consumer failure first closes admission, waits for active producer operations, drains all
  envelopes, and preserves the first terminal error.
- Queue and Memory pause bits are owned by Packet Flow. Episode counters advance only on the bit
  transition, and resume still requires the shared low-watermark condition.
- Progress uses recovery-trusted bytes plus `published_data_bytes`; no second downloaded-byte
  counter remains.
- Persistence no longer publishes shutdown packets and no production caller imports
  concurrentqueue.
- Public API, recovery metadata, connection/window policy, flush cadence, and the formal 10-key
  summary schema are unchanged.

## Correctness change evidence

| Fix | Behavior before | Red or characterization proof | Behavior after | Rollback |
| --- | --- | --- | --- | --- |
| `285471c` | Packet construction and allocation paths could terminate or collapse distinct backend failures into an ambiguous boolean | Packet Flow bounds tests plus deterministic constructor, payload, Data, Control, and close fault tests | Checked arithmetic has no partial mutation; OOM maps to `not_enough_memory`; Data backend rejection retains the draft; Control/close failures become terminal | Revert `285471c` after reverting dependent slices |
| `6f0de65` | Vendor constructor capacity and `try_enqueue` room were treated as if they were the business capacity | Vendor characterization proves constructor capacity is not a hard bound; budgets 1/2/33 and Control bypass tests | Data obtains credit only below `packet_budget`; receive or failed publish returns it; Control bypass remains ordered | Revert `6f0de65` after later accounting/close/pause slices |
| `8fa0b17` | A global ledger was reset at task start and could couple concurrent requests | Two simultaneous Packet Flow instances each retain their own exact accounting | Draft, queued lease, reorder node, discard, completion, and abort drain all use one request-local checked ledger | Revert `8fa0b17` after later close/pause slices |
| `e72300b` | Shutdown came from another producer stream and stop could abandon a queue-full draft | Cross-producer characterization plus queue-budget-one server-failure and output-equality integration | Completion/close share the network producer; open-flow drafts retry with the existing 1 ms wait before close; terminal upstream failure discards boundedly | Revert `e72300b` as one unit |

Supplementary concurrency proof:

- one producer and one consumer exchanged 100,000 one-byte packets for each logical budget
  `1`, `2`, and `33`;
- every sequence, offset, payload byte, and checksum was observed exactly once;
- a fixed-seed subset moved through reorder ownership;
- 100 close/fail races completed without deadlock;
- a deterministic blocked publish proved consumer failure waits and drains the late admitted
  envelope;
- every run ended with queue and accounted bytes equal to zero.

## Functional verification

| Command or group | Result |
| --- | --- |
| `scripts\build.bat` | pass |
| Production `PacketFlowTest.*` | 25/25 pass |
| Test-only `PacketFlowFaultTest.*` | 8/8 pass |
| Packet sequence/accounting stress | 300,000 packets pass |
| Budget-one non-Range HTTP output comparison | pass, source and output identical |
| Server failure with budget one | pass, artifacts retained and queue pause observed |
| `ctest --test-dir build -C Debug --output-on-failure` | 130/130 pass, 8 explicit skips |
| `scripts\build.bat release` | pass |
| `ctest --test-dir build -C Release --output-on-failure` | 130/130 pass, 2 architecture skips |
| Release CLI no-argument smoke | expected usage text and exit code 1 |

Debug skips consist of two architecture-impossible policy boundaries and six CLI elevation-limited
cases. The two historically failing Debug CLI tests now detect `ERROR_ELEVATION_REQUIRED`, stop
their local server, and report the same environment limitation as an explicit skip. Release
executes all six CLI paths successfully.

## Static and dependency audits

| Audit | Result |
| --- | --- |
| Vendor queue and tokens | production matches only in `src/flow/packet_queue_adapter.hpp` |
| Vendor test usage | only the explicit characterization contract test |
| Global memory symbols | zero matches |
| Shutdown packet and old enqueue/flush helpers | zero matches |
| Legacy Session/Range/Transfer packet-flow fields | zero matches |
| Public header import of Packet Flow | zero matches |
| Concurrentqueue import by download, persistence, or core | zero matches |
| Runtime adapter flag, virtual dispatch, or function-pointer seam | absent |

The locally vendored moodycamel implementation was read before choosing the adapter contract.
Constructor capacity is only a preallocation floor; `try_enqueue` is the non-allocating path and
can return false when its producer has no reusable room; allocating `enqueue` reports allocation
failure; timed dequeue timeout leaves its output untouched. The logical credit layer therefore
does not infer business capacity from vendor capacity or a raw `false` result.

## Performance verification

Detailed results are in
`evidence/phase_02_packet_flow_performance.md`.

| Protected case | Pre MB/s | Post MB/s | Change |
| --- | ---: | ---: | ---: |
| `baseline_default` | 514.88 | 568.89 | +10.49% |
| `balanced_candidate` | 509.63 | 505.54 | -0.80% |

All seven cases completed 20/20 runs. TTFB, low-memory shape, inflight bytes, pause episode
counts, packet totals, approximately 64 KiB average packet shape, 65,536-byte maximum packet, and
the 10-key schema remain stable. The keeper gate passes.

The required WPR command was attempted and failed at `wpr-start` with `0xc5585011` because this
environment could not enable the system-performance tracing policy. The failure artifact is
`build/profiles/20260731_005636_phase-2-profile`. No profiler number was substituted for the
benchmark gate.

## Gate result

Stage 2 passes its ownership, ordering, backpressure, failure, replay, concurrency, static,
Debug/Release, and performance gates. Stage 3 is unlocked.
