# Architecture and invariants

## Scope

Phase 3 provides one publisher and a fixed roster of subscribers sharing one bounded broadcast ring. Every registered subscriber observes every sequence; this is not a competing-consumer work queue. Linux POSIX shared memory provides the mapping. Linux open-file-description (OFD) byte-range locks on the shared-memory file descriptor provide exclusive publisher and per-member roles and are released by the kernel when the owning open file description closes or its process exits.

The shared object stores offsets and fixed-layout values only. It contains no process-local pointers. Participants must use a compatible Linux ABI, endianness, compiler atomic representation, and structure layout. Attachment validates magic, version, header size, offsets, slot size, capacity, subscriber table bounds, and lock-free atomic support.

## Broadcast sequence and retention protocol

`published` is the highest completely published sequence. Each registered subscriber `i` has an independent `acknowledged[i]`. Both publisher sequence and initial subscriber cursors begin at zero. Let `retained = min(acknowledged[i])` across the current roster. For the next sequence `n = published + 1`:

1. The publisher may write only if `n - retained <= capacity`.
2. It writes the fixed slot at `(n - 1) & (capacity - 1)`.
3. It release-stores `published = n` only after the slot is complete.
4. Each subscriber acquire-loads `published`, copies its next unacknowledged slot, and checks the embedded sequence.
5. That subscriber release-stores `acknowledged[i] = n` only after processing succeeds.

The publisher acquire-loads a conservative retention watermark before reusing a slot and scans all registered cursors when the ring appears full. The watermark can lag the real minimum and cause conservative backpressure but cannot permit overwrite. This bounds memory and prevents overwriting an unacknowledged message. A slow or offline registered subscriber intentionally gates the whole broadcast ring. Waits have finite deadlines and return structured timeout errors; messages are not silently dropped.

## Membership and recovery

Before the first publish, new subscribers register at sequence zero. The first publisher operation serializes with registration, requires a nonempty roster, and seals membership. A new identity admitted later starts at current `published + 1` and receives future traffic only. An existing identity reattaches at `acknowledged[i] + 1`; a crash after processing but before ACK can redeliver. Delivery is at least once until ACK while the shared object survives. Exactly-once side effects are not provided.

The publisher role lock uses byte 0, the registry lock byte 1, and each subscriber member lock a fixed offset derived from its table index. Control-plane order is publisher, registry, member. The member record state transition is `FREE -> INITIALIZING -> REGISTERED`; identity and cursor initialization precede the release-store of `REGISTERED`. An initializer crash can be cleaned by the next registry owner. No membership cutover races publication: initial admission serializes with first publication; later addition and explicit removal require a quiescent publisher.

No member is automatically evicted for age, timeout, or presumed process death. OFD role locks distinguish an active role from an exited process, but cannot distinguish a dead subscriber from a temporarily unavailable subscriber. A dead subscriber's persistent cursor therefore continues to gate reuse until same-identity restart or explicit `forget`. Forget requires both publisher and target subscriber locks to be available and knowingly discards backlog. Unlink/destruction remains an operator action and must not race live users; unlink prevents future name-based attachment but does not invalidate existing mappings.

A publisher restart reacquires the exclusive publisher lock and continues at `published + 1` if the object remains. A crash after publication but before wake leaves the committed sequence authoritative. A crash after ACK but before wake leaves the member cursor authoritative; publisher restart recomputes retention.

## Memory ordering and platform assumptions

The design uses lock-free `std::atomic<uint64_t>` and `std::atomic<uint32_t>` in a shared `mmap`, with acquire/release ordering. ISO C++ does not guarantee inter-process synchronization semantics for `std::atomic`; this implementation relies on Linux implementations where lock-free atomics operate on coherent shared mappings. Creation and attachment reject platforms where required atomics are not lock-free. Intended deployment is Linux x86-64; a successful build elsewhere does not expand the supported ABI claim.

Message slots are fixed at 256 bytes: a 64-bit sequence, 32-bit length, 32-bit reserved field, and 240-byte payload. Producer and subscriber copy bytes into and out of shared memory. No zero-copy view is exposed, preventing slot reuse while application code retains a view. Publish/receive hot-path library operations do not allocate.

Published and per-subscriber ACK cursors occupy separate 64-byte-aligned cache lines. Member metadata and cursor are likewise separated. This is a target-specific false-sharing reduction, not a claim that 64 bytes is universal. Publisher-side minimum scanning is O(N) only when the cached watermark suggests the ring is full. The fixed member table bounds that cost; the benchmark sweeps fanout to quantify it.

## Wait strategies

Each endpoint selects a process-local `busy_poll`, `futex`, or `adaptive` policy before claiming its role. Published and per-member acknowledged sequences remain the predicates. Separate 32-bit data and space generation words are only sleep-transition hints, placed on their own cache lines and checked for lock-free operation on attachment. The publisher release-publishes a slot, increments data generation, and wakes **all** subscribers. Waking only one would strand other broadcast readers. An ACK release-stores its member cursor, increments space generation, and wakes the single publisher.

Waiters check the sequence predicate, sample the generation, check the predicate again, then wait on the sampled value. A generation change makes `FUTEX_WAIT` return rather than sleep through the event. Spurious wakeups and interrupts return to predicate checks. Futex operations are process-shared. Adaptive mode performs a finite configured number of predicate checks before sleeping. Public waits use a `steady_clock` deadline. Generation wrap cannot affect message correctness because the sequence cursors are authoritative.

Wakeups are not a crash detector. If a process stops while holding its role, the other endpoint observes a finite wait timeout; OFD locks release on process exit or descriptor closure. A process killed after committing a cursor but before waking its peer leaves the cursor committed. Automatic takeover, heartbeat-based health guesses, and wait-free wake guarantees are outside this milestone.

## Design alternatives to evaluate later

- Per-subscriber payload rings reduce head-of-line blocking but multiply shared memory and publisher copies by fanout.
- A hierarchical minimum-ACK tree could reduce scan work for much larger rosters, but introduces publication and reclamation state that must be proved against membership epochs.
- A work-queue mode has different semantics: consumers divide messages rather than each receiving all updates. It should be a separate abstraction, not a flag on this broadcast contract.
- Futex wake-all cost may dominate at high fanout. Any wake optimization must preserve the invariant that each sleeper rechecks authoritative sequence state; benchmark before considering a shared notification relay or polling partition.
- Automatic eviction requires an explicit loss policy or a trustworthy failure detector; liveness timeout alone is unsafe for required market-data delivery.
