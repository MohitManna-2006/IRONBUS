# IRONBUS Engineering Plan

## Phase 0: Foundation — complete (2026-10-22)

- Establish a C++20 CMake project with strict warnings, opt-in development sanitizers, release optimization, Linux CI, and a reproducible Linux container workflow.
- Keep the public API in `include/ironbus`, implementation in `src`, command-line tools in `apps`, correctness/integration checks in `tests`, and measurements in `bench`.
- Acceptance: Linux x86-64 release configure/build and automated tests passed in a Docker x86-64 container on 2026-10-22. The macOS host executes that container through emulation.

## Phase 1: Correct SPSC shared-memory IPC — complete (2026-10-22)

- One publisher and one subscriber communicate through fixed-size slots in a POSIX shared-memory object.
- Use monotonic published and acknowledged sequences, acquire/release atomics, bounded backpressure, role exclusion, validation, and restart from the last acknowledged sequence.
- Reject unknown, incomplete, or incompatible layouts. Explicitly state Linux ABI and lock-free atomic assumptions.
- Acceptance: ordering/integrity, wraparound/full/empty/backpressure, independent processes, crash/redelivery, publisher restart, truncated-layout rejection, and both role-contention checks passed in the bounded Linux x86-64 integration suite on 2026-10-22. CLI operations work between separately launched processes.

## Phase 2: Wakeup strategies and polling

- Add process-shared Linux futex wait/wake with separate data and space generations while keeping the 64-bit sequence cursors authoritative.
- Expose busy polling, futex waiting, and bounded-spin adaptive waiting per endpoint. Preserve finite timeout behavior and reject unsafe layouts.
- Add optional benchmark CPU affinity and comparisons that record throughput, publish-to-ACK p50/p99/p99.9, CPU time where available, and environment.
- Acceptance: all Phase 1 tests pass under each wait policy; futex race, timeout, recovery, and role tests pass; release and sanitizer CI pass on native Linux x86-64.

See [PHASE2_DESIGN.md](PHASE2_DESIGN.md) for the wait protocol and failure analysis. Mark Phase 2 complete only after the acceptance checks pass.

## Phase 3: Multi-consumer architecture

Specify per-subscriber cursors, admission, backpressure, and abandoned-subscriber reclamation before implementation.

## Phase 4: Failure recovery and lifecycle hardening

Add fault injection, initialization/lifecycle hardening, operational inspection, and recovery policy only after invariants are documented.

## Phase 5: Performance engineering

Measure cache-line contention, affinity, NUMA, and tail latency with reproducible bare-metal methodology. Avoid performance claims that exceed the evidence.

## Phase 6: Market-data integration

Add typed market-data examples and integrations after the transport contract and schema/version policy are stable.
