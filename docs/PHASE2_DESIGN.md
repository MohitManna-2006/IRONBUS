# Phase 2: wait and wake design

## Scope

Phase 2 changes only how an endpoint waits for work or ring space. The Phase 1 single-publisher/single-subscriber sequence and acknowledgement contract remains authoritative. No message is dropped, and no wakeup event itself represents a message.

Three process-local policies will be available:

- `busy`: repeatedly recheck the acquire-loaded sequence predicate and issue an architecture-specific CPU relax hint. It minimizes wake latency at the cost of consuming a core.
- `futex`: use Linux process-shared futex waits after checking the sequence predicate.
- `adaptive`: spin for a bounded number of predicate checks, then sleep on a futex. A futex wake returns to the predicate check; wakeups may be spurious.

The API and CLI will allow a per-endpoint strategy. Benchmarks will state both endpoint strategies, spin budgets, CPU affinity, scheduling, kernel and architecture. The library keeps a deterministic default and makes no claim that one policy is universally fastest.

## Shared futex words

The segment gains two 32-bit generation atomics on separate cache lines: a data generation and a space generation. The data generation changes after a publisher release-publishes a slot. The space generation changes after a subscriber release-acknowledges a slot. They are hints for sleeping waiters; the 64-bit published and acknowledged sequences remain the source of truth.

Changing the header requires a shared-layout version increment. Attach rejects the previous version and verifies the futex atomic is lock-free, naturally aligned, and the mapped layout matches the advertised size.

## Lost-wakeup prevention

For an empty subscriber, the wait loop is:

1. Acquire-load `published`; return to consume if the target sequence exists.
2. Acquire-load the data generation.
3. Acquire-load `published` again; return to consume if the target sequence exists.
4. `FUTEX_WAIT` on the observed generation value with the remaining deadline.
5. Recheck the sequence predicate after every wake, `EAGAIN`, interrupt, or timeout.

Publisher backpressure uses the symmetric protocol with the acknowledged sequence and space generation. The publisher release-stores the new published sequence before incrementing the data generation and waking one subscriber. The subscriber release-stores the ACK before incrementing the space generation and waking one publisher. A wake between steps 2 and 4 changes the futex word, so the kernel returns `EAGAIN` instead of sleeping through the event.

Futex calls use process-shared operations, never `FUTEX_PRIVATE_FLAG`. Relative wait duration is recomputed from a `steady_clock` deadline after every retry. `EINTR` and `EAGAIN` retry the predicate; expiration returns the existing timeout error. Unexpected syscall failures are reported with operation context.

## Boundedness and lifecycle

Adaptive spinning has a finite configurable budget per wait operation. Once consumed, that wait blocks in the kernel until an event or deadline. Busy mode remains bounded by its caller-provided timeout, but may consume CPU for the full interval. Stalled endpoints do not acquire automatic takeover; Phase 1 role locks still define ownership.

A 32-bit generation can wrap. Correctness does not depend on it being globally unique: the waiter always rechecks the 64-bit sequence predicate before sleeping and after waking. At the generation wrap boundary, the futex comparison still protects the immediate wait transition; sequence exhaustion remains separately rejected by the ring protocol.

## CPU affinity

The benchmark may pin each endpoint to an explicitly selected logical CPU. Affinity is optional and errors are reported instead of silently ignored. The process can only select CPUs allowed by its current cpuset. Pinning is an experiment setting, not an implicit library behavior.

## Acceptance checks

- Existing Phase 1 tests pass with each policy selected independently at both endpoints.
- Full and empty endpoints time out without hanging for busy, futex, and adaptive modes.
- A message published or acknowledged around the sleep transition is eventually observed; no lost wake can corrupt ordering or overwrite an unacknowledged slot.
- Crash/redelivery and both exclusive-role checks remain valid with each policy.
- The benchmark reports policy, spin budget, affinity, environment, throughput, observed publish-to-ACK latency percentiles, and CPU time when available.
- Release and sanitizer builds pass in native Linux x86-64 CI. Docker results are for functional validation and are not used as bare-metal performance claims.
