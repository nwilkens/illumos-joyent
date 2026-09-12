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
| 3 | P1 | Pending TX notifications can outlive MAC unregister | Open |
| 4 | P2 | RX alignment and VLAN header layout defeat IP fast paths | Open |
| 5 | P2 | One reset request can cause two complete resets | Open |
| 6 | P2 | Link refresh reports UP while the datapath remains failed | Open |
| 7 | P2 | RX descriptor DMA faults are checked late or missed | Open |
| 8 | P2 | Small-MSS LSO fallback retains the wrong checksum seed | Open; LSO disabled by default |
| 9 | Maintenance | Duplicate MAC filter constructors | Open |
| 10 | Architecture | Filter ownership and replay contract is incomplete | Open |
| 11 | Architecture | Lifecycle callers conflate several kinds of quiescence | Open |
| 12 | Documentation | Some comments promise stronger invariants than the code establishes | Open |
| 13 | Test repair | `tx_bind_threshold.py` has a stale exact-text assertion | Open; known baseline failure |
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

`ice_tx_quiesce()` in [ice_tx.c](../../uts/common/io/ice/ice_tx.c) stops
submissions but leaves `itxr_blocked` armed. Interrupt recycling can still call
`mac_tx_ring_update()`. `ice_detach()` unregisters MAC before interrupt removal
provides a handler fence. Include notifications in the quiescence contract and
drain in-flight callbacks before unregister.

Acceptance: hold a blocked TX ring, fail queue stop, and release a delayed
completion across detach. Verify that no MAC callback can cross unregister.

## 4. RX header layout

The copy and loan paths in [ice_rx.c](../../uts/common/io/ice/ice_rx.c) start
Ethernet at an aligned allocation base. The IP header after 14 bytes is then
unaligned, causing MAC fast-path rejection and IP pullup/copy. Reserve two
bytes of headroom while accounting for the full DMA write extent. VLAN
reinsertion also creates an L2-only head; preserve the headers required by the
MAC fast path in its first block.

Acceptance: check copy/loan, tagged/untagged, and jumbo header alignment and
first-block layout; measure CPU, copy counts, software fanout, and throughput.
Hardware RSS distribution is distinct from this software-path problem.

## 5. Duplicate reset work

`ice_reset_task()` in [ice.c](../../uts/common/io/ice/ice.c) clears its pending
flag before acquiring `ice_rebuild_lock`. Another worker can enqueue the same
still-owed request. The queued worker does not recheck owed work after the
first rebuild completes. Make request ownership coherent and recheck under
the lifecycle lock.

Acceptance: execute that interleaving for one request and count exactly one
reset, while preserving genuinely new reset requests.

## 6. Operational link state

`ice_rebuild()` in [ice.c](../../uts/common/io/ice/ice.c) can leave
`ICE_STATE_ERROR` and report DOWN after datapath restart fails. The periodic
link refresh in [ice_intr.c](../../uts/common/io/ice/ice_intr.c) subsequently
reports physical carrier as UP, although TX still discards traffic. Separate
carrier from operational readiness and retain the DOWN override until recovery.

Acceptance: fail a nonterminal datapath restart with carrier present, advance
the admin periodic, and verify operational DOWN until a successful restart.

## 7. Descriptor DMA fault timing

The receive walk in [ice_rx.c](../../uts/common/io/ice/ice_rx.c) reads
descriptor writeback after an unchecked synchronization. Its handle check is
deferred until repost, and the empty-ring return bypasses it. Check before
consumption and suppress delivery from a faulted operation.

Acceptance: inject descriptor sync/handle faults with DD clear and set; verify
fault reporting and no delivery. Packet corruption was not demonstrated.

## 8. Small-MSS LSO downgrade

In [ice_tx.c](../../uts/common/io/ice/ice_tx.c), a below-minimum MSS request
that fits the MTU can switch from TSO to ordinary transmission without repairing
the TCP checksum seed. Native LSO prepares a seed excluding TCP length, while
ordinary checksum offload requires that length. Reject unsupported requests
or perform a complete software fallback. This remains a gate for enabling LSO.

Acceptance: exercise below-minimum MSS requests and verify checksums on a wire
capture, including IPv4/IPv6 and fallback failure cases.

## 9. Shared filter construction

`ice_gld_fltr_init()` in [ice_gld.c](../../uts/common/io/ice/ice_gld.c) and
`ice_fltr_entry_init()` in [ice_vsi.c](../../uts/common/io/ice/ice_vsi.c)
duplicate the firmware filter constructor. Introduce one small internal helper
and verify identical add, remove, attach, and replay requests.

## 10. Filter ownership contract

Document ownership across MAC users, `vi_macs` desired state, imported-core
bookkeeping, and hardware rules. Define how failures create divergence and how
replay/teardown resolves it. Item 1 defines terminal retirement only; partial
programming, rollback, and normal replay still need a complete contract.

## 11. Lifecycle contract

Document submission quiescence, callback quiescence, DMA quiescence, reset-work
ownership, operational readiness, and lock order separately. Give helpers
explicit prerequisites and guarantees; use items 2, 3, 5, and 6 as concrete
acceptance cases. Prefer small corrections to a broad lifecycle rewrite.

## 12. Comments and proven invariants

Audit comments against the checks that establish their claims. For example,
the detach fallback reset is described as a DMA barrier despite an unchecked
result. Update such comments with the corresponding behavior fixes and check
the remaining glue once the lifecycle changes settle.

## 13. Stale TX source check

`tx_bind_threshold.py` requires the exact condition
`if (res == ICE_TX_BUILD_DROP)`, while production also checks
`|| msglen < ICE_TX_MIN_LEN`. Preserve validation of both DROP handling and runt
padding without requiring the obsolete spelling. Baseline: 29 of 30 original
scripts pass; this failure predates the merge and item 1.

## 14. Behavioral coverage

Keep useful source invariants, but add executable tests of the actual C
behavior for each fixed lifecycle or descriptor defect. Include failing
controls so a test demonstrates the old bug and rejects an incomplete fix.
Track kernel/hardware acceptance separately from host stubs and native
compilation. `terminal_filters.py` begins this work; concurrency, DMA, wire
behavior, and throughput remain outside its scope.
