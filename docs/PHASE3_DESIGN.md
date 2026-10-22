# Phase 3 design: bounded broadcast SPMC

## Decision

Phase 3 changes the single-reader ring into a **single-publisher, multi-subscriber broadcast ring**. Each admitted subscriber receives every message in sequence. This is the market-data fanout model; it is deliberately not a work queue where consumers divide messages among themselves. There remains exactly one publisher, and there is no subscriber-to-subscriber ordering or delivery guarantee beyond each subscriber's own sequence.

Each subscriber has a stable, caller-chosen identity and a persistent acknowledged sequence. A subscriber restart using the same identity resumes at `acknowledged + 1`; a crash after processing but before ACK can redeliver. The publisher can reuse a slot only after every registered subscriber has acknowledged it. Thus one slow or disconnected subscriber applies backpressure to the whole fanout. This is the cost of a single bounded shared payload ring and no per-subscriber payload copies.

## Core invariants

Let `P` be the last published sequence, `A[i]` the last acknowledged sequence for registered subscriber `i`, and `R = min_i A[i]` the retention watermark.

1. Only the process holding the publisher OFD lock may mutate `P` or slots.
2. A slot for sequence `n` may be written only when `n - R <= capacity`.
3. The publisher writes slot bytes and then release-stores `P = n`.
4. Every subscriber advances only its own cursor, and only in order after receiving the matching pending sequence.
5. Slot `n` remains immutable until all registered members have `A[i] >= n`.
6. Membership changes cannot race a publish. While the roster is open, subscriber registration and the first publish serialize on one OFD registry lock. After the roster is sealed, adding or removing a member also requires the publisher role to be quiescent.
7. Existing identities retain their cursor across process restarts. A new identity admitted after publication starts at the then-current tail and does not receive historical messages.
8. No member is automatically evicted because of age, timeout, or process liveness. Explicit removal requires the publisher to be stopped and the subscriber role lock to be obtainable; it knowingly discards that member's remaining backlog.
9. Shared state contains no process-local pointers. Member state and cursors are lock-free atomics in a versioned Linux shared-memory ABI.

## Shared layout and cache behavior

The header advertises the maximum subscriber count and fixed offsets to the member table and slot array. Each fixed member record contains a publication state, exact stable identity bytes, and a cache-line-separated 64-bit ACK cursor. A separate publisher-only retention watermark is a conservative lower bound on the true minimum ACK. The publisher uses this cached lower bound for the common not-full check and scans member cursors only when the ring appears full; a stale watermark can cause conservative backpressure but cannot permit overwrite.

The shared data-generation futex must wake all subscribers after publication. Waking only one would strand the other broadcast readers. The space-generation futex still wakes the single publisher after an ACK. These words remain hints; per-member and published sequences are authoritative. Scanning `N` reader cursors on backpressure is O(N), which is intentional and auditable. Benchmark sweeps should quantify this before considering hierarchical minima or sharded rings.

## Admission, locking, and crash windows

The publisher lock uses byte 0 of the shared object. The short registry lock uses byte 1. Subscriber `i` owns a byte-range lock at a fixed offset derived from `i`, outside those control bytes. A member record moves `FREE -> INITIALIZING -> REGISTERED`; the last state store is release-published after identity and cursor initialization. A process that dies while initializing releases its OFD locks. The next registry-lock owner can clear an incomplete record before reuse because the roster cannot have been sealed while that registration held the lock.

Before the first publication, a new subscriber registers at sequence zero. The publisher's first publication acquires the registry lock, verifies at least one registered subscriber, seals membership, and then publishes. If no subscriber has registered, publish returns a structured `no_subscribers` error without consuming a sequence. This makes subscriber startup order explicit.

After the roster is sealed, attaching a known identity only acquires its member lock and resumes its cursor; this can happen while the publisher is running. Adding a new identity takes the publisher lock and registry lock, initializes its ACK cursor to the current `P`, then releases both. The late subscriber gets future traffic only. Explicit removal takes the same control locks plus the member lock; if the publisher or subscriber is live, the operation fails instead of guessing that it is stalled. This control-plane lock order is publisher, registry, member. The normal receive, ACK, and steady-state publish paths do not acquire the registry lock.

A crash after publishing but before wake leaves `P` committed; subscribers can recover it after restart or finite timeout. A crash after an ACK but before wake leaves that member's cursor committed; a restarted publisher recomputes its retention watermark. A crashed subscriber that has not ACKed holds no OFD role lock after process exit, but its persistent cursor still gates reuse until it restarts or an operator explicitly removes it.

## Rejected alternatives for this milestone

- **Competing-consumer queue:** consumers split sequences, which is useful for work distribution but does not deliver every market-data update to each strategy.
- **One payload ring per subscriber:** reduces slow-reader interference, but multiplies payload memory and publisher copy work by subscriber count.
- **Automatic stale-member expiry:** a slow but valid consumer is indistinguishable from a failed one without a failure detector; expiry could silently discard required data.
- **Dynamic lock-free registry mutation:** requires a proven membership epoch/reclamation scheme to avoid publishing across join/leave cutovers. The quiescent publisher lock keeps admission out of the hot path and is easier to audit.
- **MPMC or multiple publishers:** changes total ordering, slot ownership, and sequence reservation. It is not needed for broadcast fanout and remains out of scope.

## Acceptance gate

- Multiple independent subscriber processes each observe identical order and payload integrity.
- The slowest reader gates slot reuse; faster readers never cause premature overwrite.
- Full/empty/backpressure behavior and all wait policies remain bounded.
- Same-identity subscriber restart resumes and redelivers an unacknowledged message.
- A new late identity starts at the tail, while an existing identity resumes its prior cursor.
- Concurrent role claims and join/first-publish/forget cutovers are serialized or rejected; no half-initialized membership is accepted.
- Explicit forget fails while either the publisher or target subscriber is live and succeeds only with both quiescent.
- Release and sanitizer CI pass on native Linux x86-64 before Phase 3 is marked complete.
