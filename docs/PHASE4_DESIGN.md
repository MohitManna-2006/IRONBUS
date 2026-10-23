# Phase 4: Failure recovery and lifecycle hardening

## Goal

Make the shared-memory lifecycle inspectable and recoverable after process death at every persistent state transition. Preserve the Phase 3 contract: publication and ACK cursors are authoritative, subscribers are never evicted based on a timeout, and recovery never silently discards an acknowledged backlog.

## Failure model

Participants may exit or be killed at any instruction, including during shared-memory creation, subscriber registration, publication, ACK, membership changes, and teardown. The kernel releases an exited process's OFD locks. A process may also stop indefinitely while still alive; a timeout is not evidence that its role is safe to reclaim. The shared-memory object may be truncated or contain malformed metadata due to an interrupted creator or external damage. Power loss and persistence across reboot are outside the model because POSIX shared memory is volatile.

## Recovery invariants

1. A segment is attachable only after the creator release-publishes `READY` and the complete layout passes bounds and ABI validation.
2. A create attempt that fails before `READY` removes the name when cleanup runs. If the creator is killed and cleanup cannot run, attachment rejects the incomplete segment and tells the operator to unlink it before retrying.
3. An interrupted member registration can be reclaimed only while holding the registry lock and only from `INITIALIZING`; registered identities and their ACK cursors are never rewritten during ordinary recovery.
4. A published sequence is committed only by the release-store to `published`. Data in a slot whose sequence is above that cursor is uncommitted and may be overwritten on restart.
5. A subscriber ACK is committed only by its release-store to `acknowledged`. A crash before the store permits redelivery; a crash after the store does not.
6. A stale registered subscriber retains its cursor until that identity restarts or an operator explicitly forgets it while publisher and subscriber role locks are both available.
7. `unlink` removes the name only. Existing mappings remain valid and can still access the object; name reuse can create a second independent segment. Operators must not unlink while any process may continue using the old mapping.

## Operational inspection

Inspection must be read-only and report the object name, ABI/version, capacity, publication cursor, minimum ACK, roster seal state, and registered identities with their individual ACK cursors. Inspection must distinguish an active role lock from an inactive stale member where the platform can determine it; it must not treat lock availability as permission to forget that member automatically. Malformed or incomplete layouts must be reported as unsafe without attempting repair.

## Fault-injection and acceptance gates

Deterministic development-only pause/exit points are implemented through explicit environment variables at these transitions: after object creation, after sizing, before and after `READY`, after member `INITIALIZING`, after member `REGISTERED`, after slot write but before publication commit, after publication commit but before wake, and before and after ACK commit. The hook is private to the implementation, absent from the public API, and inactive unless an exact point is selected. `IRONBUS_FAULT_ACTION=pause` stops the process; otherwise it exits with status 86. The read-only `inspect` command reports registered IDs, ACK cursors, and lock activity without performing recovery or mutation.

For each point, verify the documented outcome with independently launched processes: incomplete creation is rejected; incomplete member initialization is reclaimed; committed publication is observed after publisher restart; uncommitted slot contents are not exposed; committed ACK is not redelivered; uncommitted ACK is redelivered; role locks become available after process exit; active-but-stopped roles remain protected; and explicit forget remains quiescent-only. Also verify malformed/truncated layouts fail safely and that inspection does not mutate them.

## Scope boundaries

This phase does not add automatic member eviction, heartbeat-based liveness decisions, a durable journal, power-loss recovery, zero-copy payload views, or a broker. Any future repair operation must be explicit, bounded, and designed around the same authoritative cursor invariants.
