# ICE lifecycle contracts

The illumos glue owns MAC callbacks, packet buffers, interrupt handlers, and
deferred work. The imported core owns firmware command encoding, control queues,
and scheduler/VSI bookkeeping. These lifetimes overlap; a stopped interface,
an ERROR bit, and a completed hardware reset establish different guarantees.

## Locks and execution context

`ice_rebuild_lock` is the outer adaptive mutex for start/stop, reset, detach
admission, and management operations that use firmware. Take it before
`ice_lock`, ring locks, `vi_mac_lock`, `ice_loopback_lock`, or `ice_lse_lock`.
The loopback path takes `ice_loopback_lock` before `ice_lse_lock`. No interrupt
handler takes `ice_rebuild_lock`: handlers latch causes and defer recovery.

`ice_lock` protects dispatch ownership and saved OICR causes. The OICR worker
drops it before waiting for the lifecycle lock; the reset worker acquires the
lifecycle lock first and briefly takes `ice_lock` when retiring dispatch
ownership. Never wait for a worker while holding a lock it needs.
`ice_lock` is an interrupt-priority mutex: never hold it across a firmware
command, which can poll for up to a second. Management paths hold only
`ice_rebuild_lock` across admin-queue commands; the core's queue lock
serializes them.

Each TX ring lock protects descriptors, backpressure, admission, and the
`mac_tx_ring_update()` call. Each RX ring lock protects pool ownership,
delivery admission, and the queue's interrupt routing register: the lifecycle
transitions and MAC's poll-mode callbacks change ring state, and one writer
composes `QINT_RQCTL` from it. `mac_rx_ring()` runs outside that lock with
`irxr_intr_busy` set; loan returns also acquire the ring lock. Free a delivered
or discarded loan chain outside the lock. The frame assembly error path is a
special case: it retires its unpublished loans before freeing their mblks.

The link lock serializes carrier-cache updates, operational publication, and
MAC-handle clearing. It does not by itself keep an unregistered MAC handle
alive: detach must first close callback paths and gate the workers.

## Helper boundaries

| Operation | Prerequisite | Guarantee on return |
| --- | --- | --- |
| `ice_tx_quiesce()` | Ring storage remains live; lifecycle caller serializes restart | New TX and completion notifications are gated; admitted TX calls and prior notifications have returned; blocked state is cleared. No DMA is released. |
| `ice_rx_quiesce()` | Ring storage remains live; restart is excluded | Every ring is closed; interrupt upcalls have returned. Loan waiting uses one shared deadline; FALSE means at least one loan remains. No pool is released. |
| `ice_queues_disable()` | Queue/core state and register mapping remain live | Attempts every queue and reports whether each confirmed disable. Failure permits no assumption that hardware has stopped accessing memory. |
| `ice_tx_reclaim()` | Software access is quiescent and queue disable or reset completed | Retires parked TCBs and clears descriptor ownership. It does not establish its own hardware barrier. |
| `ice_rx_reclaim()` | Software access is quiescent and hardware can no longer reach buffers | Frees only pools with zero loans; other pools remain intact and closed. |
| `ice_detach_quiesce()` | Lifecycle lock held, detaching gate set, STARTED clear | TRUE establishes callback/loan quiescence and packet DMA isolation before unregister. FALSE retains instance resources and MAC registration. |
| `ice_unconfigure()` | Successful detach isolation and unregister, or attach failed before exposing a datapath | Stops producers, drains workers, and releases resources in dependency order. Its final best-effort reset is cleanup, not the packet DMA barrier. |

The RX loan deadline does not bound an in-progress `mac_rx_ring()` upcall;
that callback must finish before its MAC/ring storage can be freed. TX likewise
waits for admitted calls and notifications to return. These waits are software
ownership fences, not hardware queue-stop acknowledgments.

The filter module owns accepted address/promiscuous policy and its list lock.
GLD setters acquire the lifecycle lock within that module; VSI replay passes
its already-held lock through the filter lifecycle interface. Imported filter
request types and list operations stay private to `ice_filter.c`.

## Hardware statistics

`ice_stats.c` owns the port/VSI caches, refresh timestamps, baseline validity,
and counter lock. MAC requests a scalar through `ice_stats_read()`; that entry
point takes the lifecycle lock before the statistics lock, maps supported
selectors, refreshes the port cache at most once per 10ms, and applies the MAC
access-fault policy. Unsupported selectors leave the output untouched and read
no registers. Private kstat readers retain their separate service-impact policy.

At the existing post-reset baseline invalidation point, lifecycle code calls
`ice_stats_reset()` with the outer lock held. The statistics owner takes its
inner lock and clears baseline validity while preserving accumulated counters
and the refresh deadline. Attach initializes baselines before publishing
kstats; teardown removes readers before destroying the statistics lock.

## TX copy-buffer pools

The small, ordinary, and LSO copy pools share one stack implementation. Each
buffer records its owning pool, so completion returns it without inferring
ownership from its size or the TX operation. Ordinary and LSO buffers can
have the same size and still belong to separate reserves. Their existing
shared lock and the small-pool lock protect only allocation/return on the
live stacks.

Pool construction precedes MAC registration. Destruction follows TX
quiescence, descriptor reclaim, and packet DMA isolation. Neither operation
holds a pool lock across memory allocation or DMA release. Partial construction
tracks successfully allocated buffers separately from array capacity; cleanup
requires all initialized buffers to be on the free stack and is repeatable.
The teardown order in `ice_unconfigure()` frees pools before ring/TCB storage.
It relies on `ice_detach_quiesce()` having reclaimed TX descriptors and returned
their pooled buffers before unregister. Attach failure has no admitted TX.

## Start, stop, and reset

`ice.c` owns start/stop admission and queue orchestration alongside reset and
detach. GLD callbacks submit intent through `ice_start()` and `ice_stop()`,
which acquire the lifecycle lock. Queue programming, interrupt association,
and the shared startup primitive are private to that owner. Reset calls the
private startup operation with the lock held and retains its own request
completion and RX-ring resume sequence; it does not re-enter MAC start.

`ICE_STATE_STARTED` records a successful softc start and whether reset should
restore that datapath. It is not evidence that every per-ring MAC start has
completed, that carrier is present, or that hardware DMA has stopped.

MAC start refuses detach, terminal failure, and owed reset work. It clears an
ordinary datapath ERROR before trying to reprogram the queues, relatches ERROR
on failure, and publishes the resulting operational link state. Clearing before
the attempt preserves a new error that arrives during startup. Per-ring RX
start supplies the MAC generation and posts buffers. A surviving loan pool
must drain before a later start can replace it.

MAC stop clears STARTED, dissociates queue interrupt causes, and attempts queue
disable. Successful disable permits software quiescence and reclaim. Failed
disable only closes software paths and requests a PF reset; buffers remain
owned. The void stop callback cannot report failure to MAC or free live DMA
merely to make teardown complete.

Reset preparation closes a running datapath and retains packet storage. The
rebuild must complete the claimed PF reset or global-reset wait before reclaim.
Control-queue objects, the DDP copy, port information, and VSI contexts survive
preparation; the core reinitializes their reset-sensitive state. Hardware or
firmware reconstruction failure is terminal until reload. Failure to restart
the datapath after reconstruction leaves ordinary ERROR so a later stop/start
can recover after outstanding software ownership drains.

`ice_reset_pending`, under `ice_lock`, spans queued, waiting, and running work.
It is distinct from atomic RESET_PENDING/PFR_REQ cause bits. Under the lifecycle
lock the worker claims those bits and passes the claim into the rebuild; a
callback with no claim performs no reset. Requests arriving after the claim
remain owed. Completion clears ERROR only if no later request is owed, and the
worker redispatches after releasing its dispatch ownership. A lifetime gate
leaves requests unclaimed; the gate owner redispatches when lifting it, and the
admin periodic retries failed dispatches. Terminal failure retires requests
that cannot be serviced until reload.

Carrier and operational readiness are separate. Firmware queries and loopback
update the carrier cache. ERROR, owed reset, or terminal failure forces DOWN
for both MAC publication and MAC_PROP_STATUS without destroying that cache.
Lifecycle publication is serialized with start/reset; a successful restart
can republish the retained carrier immediately.

## Detach and failure rollback

Detach refuses a started interface without disturbing it. Under the lifecycle
lock it sets the detaching gate, closes software paths, waits for RX loans,
and requires queue-disable confirmation or a successful fallback PF reset.
An access-error epoch and active-clear count prevent another FMA observer from
consuming a register fault and making an uncertain polling result look valid.

Only then may MAC unregister run. A control client can still refuse unregister;
detach lifts its gate and redispatches any recovery owed after a reset changed
the hardware configuration. Failed isolation likewise retains pools, rings,
mappings, handlers, workers, and registration. Handle clearing alone is not a
substitute for this rollback and callback protocol.

After unregister, unconfigure removes interrupt handlers, stops the periodic,
and drains the OICR taskq before the reset taskq. Those producers cannot then
queue new work against freed rings or the VSI. No taskq destruction runs while
holding the lifecycle lock. Attach failure uses progress bits for partial
cleanup and has never enabled packet queues through MAC start.

## Validation boundary

The portable regressions in `usr/src/test/ice-tests` execute the actual detach,
FMA observer, TX notification, reset worker/rebuild, and link callback bodies
against controlled boundaries. They cover failure and interleaving decisions;
they do not prove CPU memory ordering, device reset completion, or interrupt
delivery behavior. Native GCC/smatch compilation checks the real kernel
headers and module integration. The review worklist retains separate E810
fault-injection, wire, and performance acceptance requirements.
