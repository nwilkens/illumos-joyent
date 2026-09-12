# ICE/E810 review working list

Review baseline: `ice-e810` at
`21286590789bdbf8b6ab5b85c42863a06261b31e`, reviewed 2026-09-11.
Integration baseline: `b61858015a2f4bee7e5ddb28ed5ee4ca4c5849ce`, merging
`origin/master` at `eab31c4851f56d244d91d2452beb2f209538fd5f`. The merge did
not change the ICE production sources.

This list covers correctness, architecture, performance, and test quality.
Security review remains a separate, deferred task. Items 1-8 describe source
findings; their hardware triggers and effects have not been reproduced on this
revision. Items 9-14 are maintenance and validation work. Keep IDs stable as
fixes land, and record both the implemented behavior and remaining validation.

| ID | Priority | Issue | Status |
| --- | --- | --- | --- |
| 1 | P1 | Terminal reset failure prevents filter retirement and can make unregister panic | Implemented; hardware validation pending |
| 2 | P1 | Detach releases DMA after an unchecked fallback reset | Implemented; hardware validation pending |
| 3 | P1 | Pending TX notifications can outlive MAC unregister | Implemented; hardware validation pending |
| 4 | P2 | RX alignment and VLAN header layout defeat IP fast paths | Implemented; hardware performance validation pending |
| 5 | P2 | One reset request can cause two complete resets | Implemented; hardware validation pending |
| 6 | P2 | Link refresh reports UP while the datapath remains failed | Implemented; hardware validation pending |
| 7 | P2 | RX descriptor DMA faults are checked late or missed | Implemented; hardware fault injection pending |
| 8 | P2 | Small-MSS LSO fallback retains the wrong checksum seed | Implemented; LSO disabled by default, hardware validation pending |
| 9 | Maintenance | Duplicate MAC filter constructors | Implemented; request equivalence tested |
| 10 | Architecture | Filter ownership and replay contract is incomplete | In progress; VSI failure ownership corrected |
| 11 | Architecture | Lifecycle callers conflate several kinds of quiescence | Completed; explicit lifecycle contracts documented |
| 12 | Documentation | Some comments promise stronger invariants than the code establishes | Completed; comments checked against current callers |
| 13 | Test repair | `tx_bind_threshold.py` has a stale exact-text assertion | Implemented with executable copy/bind regression |
| 14 | Test coverage | Most checks inspect source strings instead of executing behavior | Open; first behavioral test added for item 1 |

## 1. Terminal filter retirement

In [ice_gld.c](../../uts/common/io/ice/ice_gld.c),
`ice_gld_set_mac_locked()` requires firmware removal to succeed before
retiring `vi_macs`. Terminal reset failure has already shut down the control
queue. In [mac.c](../../uts/common/io/mac/mac.c),
`mac_remove_macaddr_vlan()` restores `ma_nusers` when removal fails; client
teardown drops its reference anyway, and `mac_fini_macaddr()` later verifies
that no users remain. Promiscuous disable has the same ownership consequence
when MAC uses promiscuous classification instead of a unicast filter.

The fix handles only `ICE_STATE_RESET_FAILED`, under `ice_rebuild_lock`:

- Tracked unicast and multicast removals retire the software record and return
  success without a firmware command. Missing records still return `ENOENT`.
- Promiscuous disable clears the desired state and succeeds without firmware.
- New filter and promiscuous enables return `EIO`, including duplicate adds.
- Ordinary `ICE_STATE_ERROR` retains the existing firmware/error behavior.

This is valid because terminal failure blocks both start and reset replay
until driver reload. Imported common-code bookkeeping remains owned by common
teardown. It does **not** establish DMA isolation or fix items 2 and 3.

Validation: `terminal_filters.py` compiles and executes the actual C callbacks
with controlled admin-queue results. It covers normal and terminal unicast,
multicast, and promiscuous cleanup, duplicate/missing entries, lock boundaries,
and allocation retirement. The original source fails the terminal removal
case; the changed source passes all six scenarios. Negative controls also fail
when the exception is broadened to ordinary error state or promiscuous
retirement is omitted.

Validation on 2026-09-12: a clean native ICE module build in an isolated
illumos x86 source snapshot passed all 20 GCC 10 compilations and all 20
configured smatch shadow checks, with no diagnostics. Module linking and CTF
generation completed. The source/callback suite passes 30 of 31 scripts; only
the existing item 13 assertion fails. C style and patch whitespace checks pass.

Hardware acceptance still needed: force terminal recovery failure with active
primary, multicast, and promiscuous clients; close clients and detach; verify
that MAC retains no address/promiscuous users and unregister does not assert.
Do this together with validation of the remaining detach defects.

## 2. DMA release after failed fallback reset

The old `ice_unconfigure()` in [ice.c](../../uts/common/io/ice/ice.c)
discarded the fallback reset result and released packet DMA after unregister.
`ice_detach_quiesce()` now performs the fallible work first, under
`ice_rebuild_lock`:

- A started interface refuses detach without disturbing its datapath; the
  detach gate prevents a concurrent MAC start.
- TX submissions and RX upcalls/loans are quiesced without freeing buffers.
  Detach shares the bounded RX fence instead of a separate wait-only drain.
- Every queue must confirm disable, or a fallback PF reset must succeed.
  A faulted register access or an overlapping FMA observer also prevents
  accepting the polling result. Error epochs and active-clear accounting
  preserve evidence even when an interrupt observer consumes a shared fault.
  Normal successful FMA checks add no atomic operations and clear no latch.
- Failure retains MAC, pools, mappings, rings, interrupts, and task queues.
  A reset latches recovery work before it invalidates cached configuration;
  if unregister refuses a control client, gate rollback redispatches recovery.
- Unconfigure releases already-isolated packet resources. Attach failure has
  never enabled a datapath queue. Its final PF reset is best-effort cleanup,
  not the DMA barrier.

`detach_quiesce.py` executes the actual detach, FMA observer, redispatch, and
start functions in 14 controlled scenarios. The original detach fails before
proven isolation. Coverage includes disable/reset failure, MMIO faults,
consumed or delayed FMA clears, loan timeout, active-interface refusal,
unregister refusal, subsequent start, and terminal recovery suppression.
Atomic/barrier stubs exercise interleaving states, not CPU memory ordering.

Hardware acceptance remains: inject queue-disable failure followed by reset
timeout; verify that no mapping or backing memory is released, and retry
unload after recovery. Continued DMA/corruption was not demonstrated during
source review. Item 3 still covers the broader TX stop-time notification gate.

## 3. TX notification lifetime

`ice_tx_recycle()` now returns before descriptor reads, frees, or MAC wakeups
when `itxr_quiesce` is set. `ice_tx_quiesce()` takes the same ring lock held
across each `mac_tx_ring_update()`, so its acquisition waits out an earlier
notification. It then drains admitted transmit calls and clears
`itxr_blocked`: a builder may have rearmed backpressure while the condition
wait released the lock. Later completion interrupts cannot notify MAC or
reclaim buffers from the quiesced ring.

Quiescence itself releases no DMA. Explicit reclaim still requires a
successful queue-disable or reset barrier. `tx_quiesce.py` executes the real
recycle/quiesce/interrupt functions and checks five scenarios, including
pthread handshakes for a late builder and an in-flight MAC notification.
The old implementation fails the late-empty, late-completed, and builder
regressions at runtime; healthy wakeups remain covered.

Hardware acceptance remains: hold a blocked TX ring, fail queue stop, and
release a delayed completion across stop/detach. Verify no MAC callback after
quiescence, no callback crossing unregister, and no premature DMA release.

## 4. RX header layout

The receive path reserves six bytes before each frame: two align the IP
header after Ethernet, and four allow a stripped VLAN tag to be restored in
place. DMA allocations include the entire 2048-byte hardware write extent
plus that headroom; descriptor addresses and packet synchronization use the
same offset, while allocation metadata stays unchanged for teardown. Copy
and loan mblks both retain this headroom. VLAN reinsertion moves only the
address pair, preserving the original first-block IP and transport headers.

The E810 datasheet section 10.4.2.1 defines receive packet addresses in byte
units; section 3.1.2.4.1 imposes no software 4K alignment requirement. The
layout uses the same IP-alignment principle as illumos i40e.

`rx_layout.py` compiles the actual receive functions with controlled
DDI/STREAMS boundaries. Sixteen cases exercise copy/loan, tagged/untagged,
128/1500/2048/9216-byte frames, full DMA write bounds and synchronization
range, contiguous IPv6/TCP headers, exact frame bytes, and loan retirement.
The original source fails; negative controls removing allocation headroom
or copy alignment also fail. These host tests do not establish hardware
throughput, software fanout, or CPU/copy-count improvements.

Hardware acceptance remains: measure copy/loan, tagged/untagged, and jumbo
CPU, copy counts, software fanout, and throughput. Hardware RSS distribution
is distinct from this software-path correction.


## 5. Duplicate reset work

`ice_reset_pending` now covers queued, waiting, and running reset work.
Under `ice_rebuild_lock`, the worker atomically claims the reset-owed bits and
passes that mask to `ice_rebuild()` to choose a global-reset wait or PF reset.
A stale callback with no owed request does not prepare or rebuild the device.

Successful rebuilds no longer clear all owed bits near interrupt rearm. A
request arriving after the claim remains owed. Completion uses a CAS to leave
its fail-closed state intact and skips restarting the datapath when another
reset is pending. The worker releases its dispatch ownership at the end and
requeues later requests. Attach/detach gates retain requests until their owner
lifts the gate; terminal failure still retires unserviceable requests.

Validation: `reset_requests.py` compiles the actual dispatch, worker, request
claim/completion helpers, and full rebuild body. Controlled interleavings cover
a duplicate dispatch while waiting for the lifecycle lock, a stale callback,
requests after the hardware barrier and interrupt rearm, CAS retries, both
reset types, attach/detach deferral, failed dispatch, and terminal failure. The
reviewed source fails the duplicate-dispatch case. Controls that restore the
late unconditional request clear or unconditional rebuilding also fail.
Hardware/control-queue boundaries are stubs; reset timing and physical recovery
still require device validation.

## 6. Operational link state

MAC publication and `MAC_PROP_STATUS` now use one effective-state helper:
`ICE_STATE_ERROR`, a terminal reset failure, or an owed reset forces DOWN.
Physical and loopback updates retain their carrier cache, speed, and duplex;
operational DOWN reports no longer overwrite that cache. A periodic physical
UP therefore cannot make a failed datapath appear usable.

`ice_m_start()` relatches ERROR if datapath start fails and publishes its result
while holding `ice_rebuild_lock`. Successful start can republish the cached
carrier without waiting for another event. The pre-start error clear stays
before the attempt so it cannot erase a new failure arriving during startup.
Initial registration publication and the final attach query now also hold the
lifecycle lock; the final query precedes lifting the attach gate and reset
redispatch, keeping the query and its publication out of concurrent recovery.

Validation: `link_operational.py` executes the actual carrier/loopback updates,
publication functions, MAC start, and property getter. It covers repeated
carrier UP under each failure state, cache preservation, pre-registration
caching, failed/successful start, a fresh startup fault, and loopback changes.
The baseline fails operational DOWN. Controls removing the publication gate,
failed-start ERROR latch, or effective property state each fail. The actual
rebuild regression additionally verifies DOWN and nonterminal ERROR after
both datapath-start and RX-resume failure; source checks cover attach locking.

Hardware acceptance still needed: fail a datapath restart with carrier
present, advance the admin periodic, and verify DOWN until a successful
restart, including loopback and externally queried link status.

## 7. Descriptor DMA fault timing

Every descriptor synchronization now checks both the synchronization result
and the DMA handle before reading DD or other writeback fields. This includes
DD-clear empty rings, continuation descriptors, and the interrupt-limit peek.
A fault leaves the unconsumed descriptor in place for recovery and reports
DDI_SERVICE_DEGRADED with ICE_STATE_ERROR.

A faulted drain does not publish a new RX tail after a descriptor/data DMA
fault, update delivered packet/byte counters, or return packets to MAC.
Frames accumulated before the fault are discarded after releasing the ring
lock, allowing loan-recycle callbacks to acquire that lock. Descriptor repost
and register-access errors likewise suppress delivery; a register error can
only be detected after the attempted doorbell write.

`rx_dma_faults.py` compiles the actual descriptor/frame/drain and interrupt/poll
entry points against controlled DDI/STREAMS boundaries. Its 108 cases cover
sync/handle faults with DD clear/set, first and later frames, incomplete jumbo
chains, cap peeks, repost, data-buffer faults, register faults, and healthy
copy/loan delivery. The pre-fix source fails on DD-clear fault detection;
negative controls omitting the peek check or poll-chain discard also fail.
Checks include zero writeback decode from faulted reads, no delivery/counters,
no unexpected tail write, and no leaked loans or recursive ring locking.

Hardware acceptance remains: inject descriptor sync/handle faults with DD
clear and set and verify FMA reporting, no packet delivery, and recovery.
Host regressions do not reproduce a physical DMA failure or prove the
review's unconfirmed packet-corruption hypothesis.


## 8. Small-MSS LSO downgrade

In [ice_tx.c](../../uts/common/io/ice/ice_tx.c), LSO requests with an MSS
below the controller's 64-byte minimum now return `ICE_TX_BUILD_DROP`, even
when the packet fits the MTU. The LSO marker stays set so `ice_tx_one()` counts
an LSO drop and consumes the packet before building descriptors. TCP's native
LSO checksum seed excludes TCP length, so simply turning off TSO is invalid;
no software segmentation or checksum fallback is attempted.

Validation: `lso_context.py` compiles and executes the actual offload-validation
function with controlled MAC metadata. IPv4 and IPv6 cases cover MSS 0, 1,
63, 64, 9668, and 9669; frame lengths below, at, and above the former MTU
boundary; valid TSO context fields; ordinary checksum requests; and invalid
metadata. The reviewed source fails the small-MSS rejection case. This
portable test checks driver decisions, not checksums produced by hardware.

Hardware acceptance still needed: verify that below-minimum MSS requests
increment LSO drops without emitting a frame, and capture IPv4/IPv6 traffic at
supported MSS boundaries to validate wire checksums. LSO remains disabled by
default pending its existing hardware acceptance work.

## 9. Shared filter construction

The identical `ice_gld_fltr_init()` and `ice_fltr_entry_init()` constructors
are consolidated in the existing `ice_vsi.c` helper. GLD calls it through one
internal declaration in `ice.h`; attach, replay, and teardown retain their
existing calls. The helper initializes only caller-owned, unlinked storage.
It allocates nothing, takes no lock, performs no firmware operation, and does
not change caller ownership, locking, request ordering, or failure handling.

`filter_requests.py` runs the actual callers with captured imported-core
requests. Baseline and consolidated code preserve unicast/multicast/broadcast
fields across GLD add/remove, attach, replay, teardown, and attach rollback.
A mutated constructor missing `ICE_FLTR_TX` fails at runtime. The existing
terminal cleanup regressions and the updated constructor-wiring check pass.
This refactor does not establish hardware programming or rollback success.

## 10. Filter ownership contract

Document ownership across MAC users, `vi_macs` desired state, imported-core
bookkeeping, and hardware rules. Define how failures create divergence and how
replay/teardown resolves it. Item 1 defines terminal retirement only; partial
programming, rollback, and normal replay still need a complete contract.

VSI setup no longer releases its caller's desired filters on validation or
scheduler failure. Attach's existing failure label owns full teardown; rebuild
failure preserves `vi_macs` and any partial VSI state for terminal client
retirement and eventual detach. Successful replay continues to preserve the
same list. This changes no imported-core routine or queue programming.

`filter_requests.py` compiles actual setup, attach initialization, GLD callbacks,
replay, and teardown bodies. The four formerly destructive setup failures all
fail on the original implementation and pass after removing the inner teardown
calls. Tests verify terminal unicast/multicast retirement without firmware
commands, seven attach failure points (including RSS after filters exist), and
unchanged successful request/replay behavior. Hardware programming remains
outside this controlled host test.

## 11. Lifecycle contract

[LIFECYCLE.md](../../uts/common/io/ice/LIFECYCLE.md) records lock order,
submission and callback fences, RX loan ownership, hardware isolation,
reclamation prerequisites, reset request ownership, operational readiness,
and detach rollback. The helper table states what each operation establishes
and what it requires from its caller. It explicitly distinguishes the bounded
RX loan wait from the required completion of in-flight MAC upcalls.

The contract is grounded in the implemented item 2, 3, 5, and 6 fixes and their
actual-C regressions. Source review checked producer shutdown and taskq drain
ordering, the shared notification lock, and MAC's deferred link notification.
Those regressions, C style, and native module compilation provide software
validation; hardware ordering and recovery remain separate acceptance work.

## 12. Comments and proven invariants

Reviewed the glue comments against current helper bodies and callers. The
behavior fixes carry their local contracts: detach isolation precedes
unregister, TX notification gating is separate from DMA reclamation, request
claims preserve later resets, and VSI setup retains caller-owned filters.
The remaining corrections describe the admin periodic's retry role, cached
link getters versus blocking control callbacks, reset's retained control-queue
objects, actual RX queue-start and quiesce ordering, in-place VLAN insertion,
and the TX builder's enum result. Source-test commentary now describes the
current request protocol too.

Validation: the source and actual-C regressions remain unchanged in behavior;
C style and patch whitespace checks pass. This is a documentation audit, not a
proof of every driver invariant or hardware behavior.

## 13. Stale TX source check

`tx_bind_threshold.py` now compiles the actual `ice_tx_build_tcbs()` and
`ice_tx_copy_packet()` with controlled pool and DMA-allocation boundaries.
Its 12 cases cover the copy threshold, zero-filled runt padding, retry without
binding when runt padding is unavailable, binding at the minimum frame length,
permanent copy failures, descriptor-budget fallback and retirement of partial
bindings, and transient/permanent fallback copy failures. The driver behavior
is unchanged by this test repair.

Validation: the executable regression passes the reviewed production source.
Controls that omit either the DROP guard, the runt guard, or zeroing of the
pad fail their corresponding behavioral assertions. The original source-only
script's obsolete exact-condition assertion was reproduced before replacing
it. Pool and DMA boundaries are stubs; this is not a hardware DMA test.

## 14. Behavioral coverage

Keep useful source invariants, but add executable tests of the actual C
behavior for each fixed lifecycle or descriptor defect. Include failing
controls so a test demonstrates the old bug and rejects an incomplete fix.
Track kernel/hardware acceptance separately from host stubs and native
compilation. `terminal_filters.py` begins this work; concurrency, DMA, wire
behavior, and throughput remain outside its scope.
