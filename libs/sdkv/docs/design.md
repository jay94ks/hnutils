# sdkv contracts

## Scope and local semantics

The library operates on opaque, owned byte strings and supplies an in-process
thread-safe table implementation. It neither serializes application data nor
implements a network, peer server, persistence, consensus, or distributed
conflict policy. IRemoteTable is an abstract extension point for applications.
An adapter can delegate its current cache and subscriptions to ITable::create().

All local CRUD and value-based CAS operations are linearized by a table mutex.
Each key has an independent monotonically incrementing revision, including
removal and recreation. A separate table mutation counter is available through
ITable::revision(). Neither counter is a globally comparable remote version.
Revisions on two synchronized tables can differ if their local histories differ.
CAS compares presence and bytes, not revisions, so it is not an ABA guard.

Callbacks report immutable owned prev/now images and their revisions. Initial
replays have absent prev, the current now, and isReplay() == true. Replay does
not count as a mutation or advance a key revision. Observers can copy and retain
CTableChange safely; its private process-local propagation metadata is not a
public network protocol.

## Snapshot and live delivery

Registration captures a consistent snapshot and installs the live subscription
under the same data mutex. Replay entries are queued before subsequent live
changes for that registration. There is no scan-then-subscribe gap.

Each registration owns an ordered, serialized delivery queue. Callbacks run
outside both table and queue locks. Different registrations can run concurrently.
A callback may read/mutate the same table or unregister itself. A reentrant or
concurrent mutation appends to an already-draining queue rather than waiting on
user code. Consequently, a successful local mutation or addSyncFrom is not a
barrier that waits for every downstream callback to finish.

The listener and application remain responsible for cross-registration thread
safety. Do not wait from a callback for another callback on the same registration
to complete. Applications should keep callbacks short or hand owned event copies
to their own executor.

The listener is held weakly by a subscription. The application retains the
listener and either its explicit CTableSubscription or a listener-owned
addSyncFrom registration. reset/removeSyncFrom cancels queued delivery; one
callback already selected for delivery can still complete. There is no blocking
join that could deadlock self-removal.

Exceptions from listener callbacks do not roll back committed state and do not
stop other listeners. They increment CTableSubscription::errorCount() and invoke
the listener's noexcept onError hook. Override the hook to surface application
failures. Allocation failures throw; no cross-table transaction is promised.

## Following tables

addSyncFrom overlays the source's present entries without deleting local-only
keys, then follows subsequent changes. Source additions/updates are local
upserts; removals remove the corresponding local entry if present. The target's
actual before/after images and key-local revisions describe its own mutation.
An unchanged imported payload produces no revision/event.

removeSyncFrom detaches without rolling state back. Sources are weakly held,
and table-forwarding listeners hold their targets weakly, so following cycles
do not create shared-ownership cycles.

Private origin identities and source sequence watermarks per key suppress
replayed changes, feedback loops, stale duplicate delivery, and diamond-path
duplicates within the in-process graph. Origin identities never wrap and are
not stable across process restarts. These guards are not a distributed version
vector or conflict-resolution protocol.

Multiple independently writing sources are applied in arrival order at each
table. No convergence guarantee is made for conflicting concurrent writers.
A transport/distributed architecture must define its own authority, ordering,
resynchronization, and partition policy before representing a remote table.
isOnline/isSynced belong to that adapter's policy; CRUD always uses its current
view even if offline.

Deleted-key revision counters and received origin/key watermarks are retained
until the table is destroyed to avoid version reset and stale resurrection.
There is no background history compaction or bounded delivery queue. Capacity,
backpressure, and long-lived tombstone policy require application-specific
decisions rather than silent dropping of notifications.

## Coroutines

Async entry points capture shared table ownership and owned input buffers before
returning a lazy asyncpp task. With an optional dispatcher, operations resume on
that executor; otherwise they execute when awaited on the current thread.
No implicit worker pool is created. An explicitly supplied dispatcher always
queues the operation, even when called from one of that dispatcher's callbacks.
This avoids relying on header-only dispatcher TLS being shared across DLLs.
Do not block on the returned future from inside that dispatcher's callback.

The dispatcher and any active task must survive until completion; drive the
dispatcher while waiting. Application-owned dispatcher exceptions are propagated
through the task. Do not destroy an actively suspended task while its dispatcher
still has a continuation queued. Async CRUD has exactly the same local atomicity,
limit, revision, and notification rules as synchronous CRUD.
