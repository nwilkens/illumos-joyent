# ice driver checks

The [review working list](WORKLIST.md) tracks the open correctness,
architecture, performance, and test issues, their priorities, and acceptance
criteria. The driver [lifecycle contract](../../uts/common/io/ice/LIFECYCLE.md)
records the lock, callback, DMA, and recovery boundaries exercised here.

## Filter callback and recovery regression

```
python3 usr/src/test/ice-tests/terminal_filters.py
```

The runner extracts the actual filter callbacks from `ice_gld.c`, the shared
request constructor from `ice_vsi.c`, and the state enum from `ice.h`, then
compiles their bodies unchanged with boundary stubs in `terminal_filters.c`.
Thirty scenarios cover accepted ownership, failed unicast/multicast commands,
promiscuous rollback and retirement, owed/terminal recovery, duplicate/missing
entries, and reset requests arriving during the address-list check. Assertions
check errno, allocations, command counts, recovery dispatch, and lock boundaries.
A successful recorded-rule rollback still requests reset because an AQ error
can leave an unrecorded hardware rule. Direct replay must program an accepted
enabled policy even when its boolean is unchanged.

Use `--source /path/to/ice_gld.c` and `--vsi-source /path/to/ice_vsi.c` for paired
source revisions. The pre-recovery source at `79bd14d475` compiles with this
fixture and fails at runtime because a failed add does not request recovery.
This test does not load the driver or establish hardware isolation. The
[filter contract](../../uts/common/io/ice/FILTERS.md) records these ownership
and recovery rules.

## Shared MAC filter request regression

```
python3 usr/src/test/ice-tests/filter_requests.py
```

The runner compiles the actual GLD filter callbacks and VSI attach, replay,
and teardown functions. It reuses the terminal-filter fixtures and captures
requests at the imported-core boundary. Unicast, multicast, and broadcast
requests retain the same TX direction, MAC lookup, VSI forwarding/source,
software handle, and address fields across add, remove, attach, replay, and
teardown. An attach-failure case verifies the same rollback request and no
new desired-state ownership. Replay preserves the existing desired list.

The runner also executes the real VSI setup and attach initialization bodies.
Four rebuild failures (invalid handle, out-of-range hardware VSI, missing cached
context, and scheduler failure) preserve desired records for terminal client
removal without firmware calls. Seven attach failures verify that the existing
attach owner still destroys partial VSI state, lists, and locks, including
retiring desired records after RSS setup fails.

The `recovery_replay` scenario executes actual VSI rebuild after failed add,
failed multicast removal, failed promiscuous enable/rollback, and failed
promiscuous disable. Only accepted, unretired addresses are replayed; an
unaccepted or retired promiscuous policy is not restored. Later successful
recovery admits new ownership again. Core calls are controlled boundaries;
this checks the driver's replay decisions, not hardware cleanup.

The `requests` scenario checks constructor equivalence and current callback
policy. Select paired source files with `--gld-source` and `--vsi-source`, and
a single scenario with `--scenario`. Before the VSI setup ownership fix, each
`rebuild_*` scenario fails because setup drains the desired list. Mutations
that omit TX direction or replay an unowned promiscuous policy fail request
checks at runtime. Firmware encoding and device programming remain outside
these controlled tests.

## Detach lifecycle regression

```
python3 usr/src/test/ice-tests/detach_quiesce.py
```

This compiles the actual detach, FMA observer, reset redispatch, and MAC-start
functions. Fourteen scenarios cover the hardware barrier before unregister,
resource retention on failure, the bounded RX fence, start admission, and FMA
errors consumed by another observer. `--source` selects an older detach body
while retaining the current FMA boundary; `--gld-source` selects the start
callback. The test stubs hardware and atomics and does not prove live DMA or
CPU memory ordering.

## TX notification quiescence regression

```
python3 usr/src/test/ice-tests/tx_quiesce.py
```

The runner compiles the actual TX recycle, quiesce, and interrupt functions
with small DMA/MAC boundary stubs. Five named scenarios cover empty and
completed blocked rings after quiescence, both healthy wakeup paths, a builder
rearming backpressure during the active-call drain, and a prior notification
holding the ring lock while quiescence waits. The last two use pthread mutex
and condition-variable handshakes without sleeps. Assertions check callback
counts, retained descriptors/control blocks, and the completed quiescence gate.
This validates software callback ownership; hardware DMA isolation remains a
separate queue-disable/reset requirement.

Use `--source /path/to/ice_tx.c` for a baseline and `--case NAME` for one
scenario. The baseline fails `empty_late`, `completed_late`, and
`active_builder` at runtime.

## LSO context regression

Run `python3 usr/src/test/ice-tests/lso_context.py` with Python 3 and a C99
compiler (`CC` defaults to `cc`). It extracts the actual `ice_tx_context()` and
its metadata types and descriptor constants, then compiles the function
unchanged with MAC metadata stubs. IPv4 and IPv6 cases exercise unsupported
MSS rejection regardless of packet length, accepted MSS boundaries, TSO
context fields, ordinary checksum requests, and invalid metadata. The LSO
marker remains set on rejection for drop accounting.

Use `--source /path/to/ice_tx.c` to run the same regression against an earlier
implementation. The reviewed baseline fails the small-MSS case. The test does
not emulate the NIC or establish wire checksum correctness; LSO remains off
by default until hardware validation is complete.

## Reset request ownership regression

Run `python3 usr/src/test/ice-tests/reset_requests.py` with Python 3 and a C99
compiler (`CC` defaults to `cc`). It compiles the actual reset dispatch,
worker, request claim/completion helpers, and full rebuild body. Hardware and
taskq boundaries are controlled so the test can inject requests while the
worker waits, after the reset barrier, at interrupt rearm, and during atomic
completion. Assertions check reset counts and type, deferred work, request
retention, datapath restart suppression, and terminal handling.

Use `--source /path/to/ice.c --intr-source /path/to/ice_intr.c` for an earlier
revision. The reviewed implementation fails duplicate-dispatch coalescing.
Controls restoring the late request clear or unconditional stale-worker
rebuild fail too. This portable test does not establish hardware reset timing
or device recovery.

## Operational link regression

Run `python3 usr/src/test/ice-tests/link_operational.py` with Python 3 and a C99
compiler (`CC` defaults to `cc`). It compiles the actual link publication,
carrier/loopback updates, MAC start, and property getter. Failed operation
must remain DOWN despite repeated carrier UP; a successful start republishes
the retained carrier. Cases also cover null MAC handles, loopback, and a new
fault during startup. `reset_requests.py` exercises the actual rebuild's
nonterminal datapath-start and RX-resume failures.

Use `--source /path/to/ice_intr.c --gld-source /path/to/ice_gld.c` for an earlier
implementation. The baseline fails operational DOWN, as do incomplete fixes
that omit the effective publication/property gate or failed-start ERROR latch.
These tests substitute hardware and MAC boundaries; physical link, queue, and
wire behavior still require hardware validation.

## Upstream integration baseline

The 2026-09-11 integration merges TritonDataCenter/illumos-joyent master at
`eab31c4851f56d244d91d2452beb2f209538fd5f` into the ICE branch previously at
`21286590789bdbf8b6ab5b85c42863a06261b31e`. The merge preserves the ICE sources
and build integration; `usr/src/uts/common/Makefile.files` combines the ICE
object lists with the upstream changes without a manual resolution.

The pre-merge source-check baseline is 29 of 30 scripts passing.
`tx_bind_threshold.py` failed an obsolete exact-text assertion for the DROP
condition, which also handles minimum-length frame padding. That baseline
failure is now repaired by the executable copy/bind regression described below.

Run the source checks and a native module build against the merged source
tree before beginning driver fixes. Source checks do not compile the driver;
a module build does not validate the reviewed runtime failure paths.

## Running the source checks

These tests cover safety properties that can regress without requiring an
E810 device. Run them from anywhere in the source tree with:

```
python3 usr/src/test/ice-tests/rx_checksum.py
python3 usr/src/test/ice-tests/jumbo_rx.py
python3 usr/src/test/ice-tests/admin_interrupt.py
python3 usr/src/test/ice-tests/link_state.py
python3 usr/src/test/ice-tests/fma_dma.py
python3 usr/src/test/ice-tests/dma_lifetime.py
python3 usr/src/test/ice-tests/mac_filter.py
python3 usr/src/test/ice-tests/vsi_tx_vlan.py
python3 usr/src/test/ice-tests/loopback.py
python3 usr/src/test/ice-tests/hw_stats.py
python3 usr/src/test/ice-tests/link_speed_caps.py
python3 usr/src/test/ice-tests/lso.py
python3 usr/src/test/ice-tests/rss.py
python3 usr/src/test/ice-tests/reset_oicr.py
python3 usr/src/test/ice-tests/reset_rebuild.py
python3 usr/src/test/ice-tests/reset_serialize.py
python3 usr/src/test/ice-tests/tx_bind_threshold.py
python3 usr/src/test/ice-tests/tx_blocked.py
python3 usr/src/test/ice-tests/tx_doorbell.py
python3 usr/src/test/ice-tests/vlan_rx.py
python3 usr/src/test/ice-tests/rx_layout.py
python3 usr/src/test/ice-tests/rx_dma_faults.py
python3 usr/src/test/ice-tests/pool_locks.py
python3 usr/src/test/ice-tests/jumbo_copy.py
python3 usr/src/test/ice-tests/loan_wait.py
python3 usr/src/test/ice-tests/safe_mode.py
python3 usr/src/test/ice-tests/stale_comments.py
python3 usr/src/test/ice-tests/rx_intr_limit.py
python3 usr/src/test/ice-tests/rx_intr_rearm.py
```

`rx_checksum.py` verifies that receive checksum metadata is captured before
the descriptor is reposted and that all hardware-reported L3/L4 checksum error
bits suppress checksum validation.

`jumbo_rx.py` verifies that receive frames are assembled through an
EOP-terminated, bounded descriptor walk; segment and total lengths are checked
separately; only EOP metadata drives RXE and checksum handling; malformed,
DMA-fault, and allocation-failure paths advance the ring; and frame segments
are linked with `b_cont`.

`admin_interrupt.py` verifies that every interrupt on the dedicated admin
vector can schedule a bounded, single-flight ARQ drain without depending on an
OICR cause bit, while packet queues remain on separate vectors.

`link_state.py` verifies that the attach-time link state is published only
after successful MAC registration and that publication is serialized with
asynchronous link updates through the link-state lock. It also verifies that
the cache starts at `LINK_STATE_UNKNOWN`, preserving an honest result if the
initial hardware query fails.

`fma_dma.py` verifies that the driver advertises and preserves the negotiated
DMA-checking capability, applies `DDI_DMA_FLAGERR` to both datapath and
common-code control-queue allocations, and retains datapath handle checks.

`dma_lifetime.py` verifies that common-code DMA ownership is tracked by an
explicit bound flag rather than physical address zero, while preserving the
common-code-visible `va`/`pa`/`size` structure prefix.

`mac_filter.py` verifies identical MAC filter construction for add and remove.

`vsi_tx_vlan.py` verifies that the PF data VSI admits tagged and untagged Tx.

`loopback.py` verifies the standard netlb ioctl surface, its explicit STREAMS
and strsun dependencies, the `PRIV_SYS_NET_CONFIG` gate on mode changes,
adaptive thread-context serialization around the firmware command,
rollback-safe local-VSI permission and MAC-command ordering, the paired
`ALLOW_LB`/`LOCAL_LB` flags with inverse source-pruning transitions,
common-code VSI cache updates, link-state ordering, detach cleanup,
physical-event override, and absence of loopback branches in the packet
datapath.
`ice_loopback.c` is the small userland controller used for hardware validation.
It sends each netlb command through STREAMS `I_STR`, so the driver receives the
inline payload and exact `ioc_count` it validates. Build it on illumos from the
source root with:

```
gcc -Wall -Wextra -Werror -idirafter usr/src/uts/common \
    -o /tmp/ice_loopback usr/src/test/ice-tests/ice_loopback.c
```

`hw_stats.py` verifies the hardware statistics wiring: both counter refreshes
run under the stat lock, the clear-on-read VSI register is serviced through the
common code, attach captures both hardware baselines before exposing the
kstats, the kstat callbacks reject writes and lock correctly, teardown deletes
the kstats before destroying their lock, attach installs stats before MAC while
detach removes them before unmapping registers, and `ice_m_stat` sources the
MAC counters under the stat lock. Port refreshes are rate-limited so a MAC
kstat snapshot reads the hardware counter bank once, and unsupported MAC
statistics do not trigger register reads. Register-access faults from a
supported MAC statistic report degraded service and return `EIO`, while
private-kstat failures retain the established unaffected-service policy.

`link_speed_caps.py` verifies that PHY setup advertises the full media speed
set with automatic FEC, media insertion reapplies that configuration, and the
cached supported and advertised speed/FEC values reach the GLDv3 statistics
and read-only property callbacks.

`lso.py` verifies the dark-by-default LSO capability gate, context descriptor
encoding, hostile-metadata checks, per-segment and per-packet descriptor
limits, LSO bind emission, frame-sized copy fallback, DMA cookie-size guard,
and compile-time descriptor-layout checks. The `tx_lso_enable` integer driver
property remains zero by default and should be set in `/kernel/drv/ice.conf`
only for hardware validation.

`rss.py` verifies that interrupt allocation selects a power-of-two data-queue
count within the property, CPU, firmware queue, MSI-X vector, and driver caps;
that the granted vector count controls the final queue count; that the 1:1
ring-to-vector invariant is asserted at sizing and the MSI-X vector accounting
is logged; that the queue ISR dispatches by vector index rather than scanning
rings; and that the VSI, RSS LUT, and GLDv3 receive-ring interrupt handle use
that multiqueue layout.

`reset_oicr.py` verifies the fatal-cause OICR decode and fail-closed path: the
ISR latches reset and fatal causes, the MDD handler clears every detection
register and fails closed only for a this-function offender, the worker
snapshots the causes and skips the ARQ drain during a reset, and mac start
refuses while a reset failed terminally or a rebuild is owed.

`reset_rebuild.py` verifies the reset prepare/rebuild path: prepare quiesces the
datapath, silences the OICR, marks the link down, and marks the VSI absent while
keeping the tracked MAC list; the rebuild reinitializes only what a reset clears
(hardware, DDP, VSI, RSS, interrupt routing, link) and never re-runs the
one-time attach allocations (interrupts, MAC registration, ring DMA); it clears
`reset_ongoing` before the reinit so the admin queue is usable and clears the
fail-closed and reset-owed state only after every rebuild step; and the terminal
path sets `ICE_STATE_RESET_FAILED` with `DDI_SERVICE_LOST`. It also verifies that
`ice_vsi_rebuild` recreates the VSI, replays the tracked filters, restores RSS,
and re-applies promiscuous mode.

`reset_serialize.py` verifies the reset serialization and detach safety: mac
start/stop bracket the datapath in the outermost `ice_rebuild_lock` and start
goes through the factored `ice_start_datapath`; the taskq worker no-ops while
detaching and takes the rebuild lock only after dropping `ice_lock`; the reset
taskq is destroyed after the interrupt handlers are removed and before the rings
and VSI are freed; detach marks the device detaching under the rebuild lock
before unconfigure; and `ice_reset_dispatch` mirrors the `oicr_pending`
single-flight coalescing.

`tx_bind_threshold.py` compiles the actual `ice_tx_build_tcbs()` and
`ice_tx_copy_packet()` with a C99 compiler (`CC` defaults to `cc`). Twelve cases
exercise the copy threshold, zero-filled runt padding, runt retry when no
copy buffer is available, minimum-length bind fallback, permanent copy
failure, descriptor-budget and bind-failure fallback, partial-binding cleanup,
and transient/permanent fallback copy failures. The C harness substitutes pool
and DMA allocation boundaries; it does not load the driver or perform DMA.
Use `--source /path/to/ice_tx.c` to run against another source revision.
Controls omitting the DROP guard, runt guard, or pad zeroing each fail.

`tx_blocked.py` verifies the transmit back-pressure handshake: `itxr_blocked`
is armed under the ring lock and reclaim is re-driven after arming and before
the lock is dropped, so a fully drained ring that will raise no further
completion interrupt cannot stay blocked at MAC; the chain is returned for MAC
to retry; and both exits of the recycle path own the wakeup.

`tx_doorbell.py` verifies the descriptor-sync and doorbell sequence: only the
descriptors the packet wrote are synced, split at ring wrap with
descriptor-sized offsets; the sync precedes the tail advance and the doorbell;
the doorbell write is FM-checked; there is no per-packet MMIO readback or
whole-ring sync; and the control paths keep their flush while recycle keeps its
`DDI_DMA_SYNC_FORKERNEL` sync.

`vlan_rx.py` checks descriptor tag/decode ordering and in-place insertion
bounds. `rx_layout.py` executes the production copy, loan, descriptor posting,
VLAN reinsertion, and frame assembly functions. Sixteen cases verify aligned
and contiguous IP headers, packet bytes, the full DMA allocation and sync
extent, jumbo chains, and loan accounting. Shared `rx_test.py`/`rx_test.h`
supply controlled DDI/STREAMS boundaries. These are host regressions;
hardware performance measurements remain separate.

`rx_dma_faults.py` executes the production descriptor walk, frame assembly,
drain, and interrupt/poll entry points. Its 108 cases inject DMA sync and
handle faults before DD-clear/set reads, during jumbo validation, at cap
peeks and repost, plus data-buffer and register faults. Copy/loan cases
verify delivery suppression, counter/tail behavior, and cleanup outside
the ring lock; healthy controls retain ordinary delivery.

`pool_locks.py` verifies that both transmit copy-buffer pool locks are created
once at the negotiated interrupt priority before first use, destroyed exactly
once after the pools are torn down, and never held across the `ice_buf_fini`
unwind inside `ice_buf_init`.

`jumbo_copy.py` verifies that the transmit copy pool can hold any MTU-legal
frame: the general pool buffer is page-rounded from `ICE_MAX_FRAME_SIZE`, a
whole frame still fits one transmit descriptor, the copy fallback draws from
the small pool then the general pool without depending on LSO, and the former
receive-sized pool constant is too small for a jumbo frame.

`loan_wait.py` verifies that receive teardown never waits unbounded on loaned
buffers: one absolute deadline is computed before the ring loop, the wait is a
`cv_timedwait`, a ring that times out is left fully intact and is neither freed
nor reposted, every control-block free is guarded by the loan count or pool
ownership, a surviving pool is not clobbered on restart, and a single bounded
stop serves both the unplumb and reset callers.

`safe_mode.py` verifies that safe mode withholds the hardware offloads the DDP
package would have provided: the checksum and LSO capabilities are refused
outright rather than advertised with no flags, each guard precedes its
assignment, and the receive path reports no verified checksum when the
descriptor status bits carry no verdict.

`stale_comments.py` verifies that the glue comments describe the driver as it
is actually built: no development milestone labels survive in any glue source,
every `ICE_ATTACH_*` token appearing anywhere in the glue -- in code or in a
comment -- names a progress bit the `ice_attach_state_t` enum actually defines,
and the genuine multi-function limitation on the instance list stays recorded.

`rx_intr_limit.py` verifies the rx per-interrupt frame cap: the
`rx_limit_per_intr` property is read with both clamps and assigned before the
rx rings are allocated; the cap is hoisted once and disarmed for byte-budgeted
polls while covering nonpositive budgets; every consumed frame counts against
it, including discards; a limit hit is declared -- kstat and verdict both --
only after a DD peek confirms the next descriptor is actually ready, so an
exact-cap burst cannot schedule a software interrupt into an empty ring; a
zero-budget poll (reachable from a bandwidth-capped SRS) delivers nothing
rather than asserting; and the shipped `ice.conf` documents the default.

`rx_intr_rearm.py` verifies the limit-hit residual-drain contract from
datasheet section 9.1.2.6: the rx drain's limit verdict gates the software
interrupt, `SW_ITR_INDX` is programmed with its enable and throttled by the
queues' ITR slot (never No-ITR, which would refire unthrottled), the SWINT
bits fold into the single `GLINT_DYN_CTL` re-arm write, and the base word is
the shared `ICE_GLINT_DYN_CTL_REARM` definition.

## On-hardware datapath acceptance

`datapath_accept.sh` is not a source check: it runs on a host with a live
`ice0` and a physical peer, and is the reproducible functional regression suite
for the datapath. Run it on the device under test with the peer already serving
`iperf -s` on the peer address, once per MTU:

```
datapath_accept.sh 192.0.2.2 1500
datapath_accept.sh 192.0.2.2 9000
```

It asserts the module is bound; the test address plumbs at the requested MTU
and the link comes up; FMA access/DMA/dropped-ereport counters are zero before
and after traffic; small and near-MTU ICMP reach the peer; a four-stream
`iperf` run moves traffic and advances the PF byte counters; MAC and CRC error
counters stay zero; and three plumb/unplumb cycles each bring the link back
with FMA still clean. Exit status is zero only if every check passes. Run it
from both hosts to cover both traffic directions. Validated on boston<->hunter
at MTU 1500 (9.36 Gbps) and 9000 (9.59 Gbps), all checks green.
