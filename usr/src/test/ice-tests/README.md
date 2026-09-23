# ice driver checks

The [review working list](WORKLIST.md) tracks the open correctness,
architecture, performance, and test issues, their priorities, and acceptance
criteria. The theory statement at the top of
[ice.c](../../uts/common/io/ice/ice.c) records the lock, lifecycle, DMA,
filter, and recovery rules exercised here.

## Scheduler resource admission regression

`sched_resources.py` compiles the real `ice_sched_query_res_alloc()` and its
response types. Thirty-four cases cover unsupported layer counts (including
one layer and values that would truncate to a supported count), zero child
fanouts at every consumed layer, valid 5/9-layer responses with varied and
minimum fanouts, allocation/AQ errors, and reuse of accepted cached resources.
Rejected responses leave scheduler state unchanged and release their buffer.
Unused records outside the reported topology may remain zero.

The test stops at resource admission; it does not build a scheduler tree or
exercise the downstream invalid-index/division paths. Both rejection groups
fail against the previous range-only check. Use `--source` for a source
revision and `--scenario levels|fanout|valid|failures|cached` to select a group.

## Portable suite

Run `python3 -B usr/src/test/ice-tests/run_tests.py` from the repository root
with Python 3.9+ and a C99 compiler. The explicit suite runs source checks and
actual-C regressions, excluding support modules. Use `--list` to see its
manifest, or pass script names to select checks. See
[REGRESSIONS.md](REGRESSIONS.md) for runner failure/timeout behavior,
reproducible negative controls, and the pending hardware acceptance matrix.

## Control-plane setup regression

`control_setup.py` executes the production link-event and RSS setup functions.
Seventeen scenarios check the event mask, missing-port handling, safe-mode
skip, invalid table sizes, each firmware failure stage, and successful RSS
key/table/flow programming. It checks round-robin entries and allocation
cleanup; removing unused link/RSS bookkeeping leaves these results unchanged.
Temporary source controls with a wrong event mask, zero-filled RSS table, or
symmetric hashing each compile and fail at runtime.

## Lifecycle interface regression

`lifecycle_api.py` executes the actual MAC adapters, lifecycle start/stop,
link publication, and TX/RX stop composition. Eighteen scenarios cover
detach, terminal and owed-reset admission, queue/RX startup failures, new
errors during startup, successful publication, loan retention, failed-disable
recovery, and restart with the lock already held. MAC start allocates the LSO
pools before it programs a queue or opens a ring; if it cannot, the start
fails with ENOMEM and programs nothing. A start that fails later frees the
pools, and a rebuild restart that fails keeps them for MAC stop. A stop that
confirmed the queue disable frees the pools after the reclaim; a stop that
did not keeps them. Four source mutations compile and fail
runtime checks for admission, barrier handling, programming errors, and lost
asynchronous errors. Paired `--source` and `--gld-source` paths also exercise
pre-refactor revisions. `lifecycle_boundary.py` rejects external calls to
private queue/startup operations and checks that MAC callbacks delegate intent.
These controlled boundaries do not emulate hardware DMA or interrupt delivery.

## Filter callback and recovery regression

```
python3 usr/src/test/ice-tests/terminal_filters.py
```

The runner extracts the MAC adapters from `ice_gld.c`, filter operations and
the private request constructor from `ice_filter.c`, and the state enum from
`ice.h`, then
compiles their bodies unchanged with boundary stubs in `terminal_filters.c`.
Thirty scenarios cover accepted ownership, failed unicast/multicast commands,
promiscuous rollback and retirement, owed/terminal recovery, duplicate/missing
entries, and reset requests arriving during the address-list check. Assertions
check errno, allocations, command counts, recovery dispatch, and lock boundaries.
A successful recorded-rule rollback still requests reset because an AQ error
can leave an unrecorded hardware rule. Direct replay must program an accepted
enabled policy even when its boolean is unchanged.

Use `--source /path/to/ice_gld.c`, `--vsi-source /path/to/ice_vsi.c`, and
`--filter-source /path/to/ice_filter.c` for matching source revisions. The pre-recovery source at `79bd14d475` compiles with this
fixture and fails at runtime because a failed add does not request recovery.
This test does not load the driver or establish hardware isolation. The
"Filters and replay" section of the ice.c theory statement records these
ownership and recovery rules.

## Device family regression

`mac_family.py` derives the supported device IDs from the imported
`ice_set_mac_type()` and requires the package manifest aliases to match that
set exactly. Subsystem IDs and the unmapped `0x1888` stay out. It then
compiles the core mapping with the driver's `ice_family_name()` and the
per-family helpers, and runs them for every device ID: the slow EMPR wait,
the sideband receive drain, the E830 TCLAN detection registers, the E830 PHY
firmware wait, its bound and the deferred setup the admin worker completes,
the Get Link Status data length, and the DDP segment and signature type. A
faulted `GL_MNG_FWSM` read must end the wait as unreadable, and in the admin
worker it must keep the PHY setup pending, set the datapath error, and report
the service loss once until a clean read. Attach must fail on an unreadable
PHY firmware state. It also parses `firmware/ice.pkg` and requires a
signed configuration segment for each family. The check covers the
decisions that differ by family; it cannot show that an untested family
passes traffic.

## Queue count regression

`queue_count.py` compiles `ice_queue_limit()` and `ice_prop_get_num_queues()`
and runs them against CPU, firmware queue, MSI-X, RSS entry width and
`num_queues` combinations. Without the property the count is at most 16. The
count follows the CPUs without power-of-two rounding and stays at or below
`MAX_RINGS_PER_GROUP - 1`, because MAC keeps one SRS per rx ring plus one for
software classification in an array of that size. A property outside 1 to
127 is clamped and logged once. The check also requires that the vector grant
only lowers the count.

## LED regression

`led.py` compiles `ice_led_set()`, `ice_led_replay()` and `ice_led_fini()`
from `ice_port.c` with a stub admin queue. It checks that only DEFAULT and
IDENT are accepted, that each command runs under `ice_rebuild_lock`, that a
failed command returns EIO without changing the recorded mode, that a
rebuild blinks the LED again only when IDENT was set, and that detach gives
the LED back to firmware once. A restore the device refuses at detach is
logged with `dev_err` and leaves IDENT recorded, and detach continues. It
also checks the `MAC_CAPAB_LED` fields and the replay and restore call sites;
the restore must come after MAC unregister and before the admin queue
teardown in `ice_unconfigure()`.

## Checksum counter regression

`rx_hcksum.py` compiles `ice_rx_hcksum()` with the imported ptype and status
definitions and runs each verdict: clean IPv4 and IPv6 frames, IP header and
outer IP errors, L4 errors, IPv6 extension headers, IP without a summed L4
protocol, unprocessed and unknown frames, and safe mode. It checks the
reported flags and the per-ring `rx_hck_*` counter for each case, and that
every rx and tx checksum counter has a kstat name. `lso_context.py` checks
the reason `ice_tx_context()` records for each refused request, which feeds
the `tx_hck_*` and `tx_lso_*` counters.

## Firmware recovery regression

`fw_recovery.py` compiles `ice_fw_state()` and
`ice_fw_recovery_report()` with the imported `ice_get_fw_mode()`. Recovery
is detected for the recovery bit alone and with the debug bit (which the core
reports as DBG), within the two-bit E830 field, and not for normal, debug,
or rollback firmware. A faulted read returns `ICE_FW_UNREADABLE`, never a
usable verdict. The report posts one `ereport.io.device.fw_corrupt`, marks
the service lost, and tells the operator to update the NVM. The test also
requires the check to run before the first admin queue command at attach and
right after the reset completes in the rebuild, and requires both paths to
fail closed on recovery and on an unreadable register. `reset_requests.py`
runs the rebuild with recovery firmware and with a faulted read and requires
a terminal, fail-closed result with no control queue restart.

## Diagnostic ioctl regression

`diag_ioctl.py` compiles every function in `ice_ioctl.c` with stub admin
queue, credential and STREAMS boundaries and attacks the firmware logging and
debug dump ioctls. A zone caller fails with EPERM even with every privilege,
as does a global-zone caller without `{PRIV_SYS_DEVICES}` or
`{PRIV_SYS_CONFIG}`; TRANSPARENT and wrongly sized requests fail with EINVAL;
no firmware command runs for a refused request. It checks module, level,
resolution and flag validation, UART option preservation, the log ring
(whole-event drops, wrap-around, drop counts, and no stale bytes after the
returned length), the allowed debug dump clusters for E810 and E830, and a
firmware length larger than the buffer. Replies must replace every byte of
the caller's structure. The test also requires that no reset ioctl exists.
The admin queue stub models the firmware's Query and Set FW Logging
commands. Firmware lists log modules in its own order and need not list all
of them: the driver uses the count it returns, a reply in reverse order must
read and set levels by module ID, and a set must keep every other module's
level. A module the reply does not list fails GET and SET with ENOENT, and
ICE_FWLOG_MODULE_ALL fails with ENOENT when none is listed; the Set command
carries exactly the listed modules, never one firmware did not report. A
count above 32, or a listed entry with a repeated or out-of-range module ID
or a level above the maximum, fails GET and SET with EIO before any
configuration is sent; entries past the count are not read.

## mdb module check

`mdb_module.py` checks the `ice` mdb module in
`usr/src/cmd/mdb/common/modules/ice`. The descriptor fields it copies from
the imported core must keep the core's values, its state bits must match
`ice_state_t`, and every member its CTF mirror types read must exist in the
driver structure. It also checks the dcmd and walker tables, the x86 module
list, and the package entries for the kmdb and mdb modules.

## Shared MAC filter request regression

```
python3 usr/src/test/ice-tests/filter_requests.py
```

The runner compiles the actual GLD adapters, filter owner operations, and
VSI attach, replay, and teardown functions. It reuses the terminal-filter fixtures and captures
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
policy. Select matching source files with `--gld-source`, `--vsi-source`, and
`--filter-source`, and a single scenario with `--scenario`. Before the VSI setup ownership fix, each
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

MSS values from 64 through 87 are rejected: the datasheet (10.5.8.4.4) makes
an MSS below 88 a malicious-driver event, and an earlier minimum of 64 let
those requests reach the queue.

Use `--source /path/to/ice_tx.c` to run the same regression against an earlier
implementation. The reviewed baseline fails the small-MSS case. The test does
not emulate the NIC or establish wire checksum correctness. LSO is on by
default; `datapath_accept.sh` exercises it on hardware.

## Reset request ownership regression

Run `python3 usr/src/test/ice-tests/reset_requests.py` with Python 3 and a C99
compiler (`CC` defaults to `cc`). It compiles the actual reset dispatch,
worker, request claim/completion helpers, and full rebuild body. Hardware and
taskq boundaries are controlled so the test can inject requests while the
worker waits, after the reset barrier, at interrupt rearm, and during atomic
completion. Assertions check reset counts and type, deferred work, request
retention, datapath restart suppression, and terminal handling. A slow EMPR
(E825-C and E830) must wait once before the reset-complete poll; a PF reset
issued by the driver must not wait. A stopped device frees its LSO pools
after the reset barrier; a started one keeps them. A rebuild that restarts
the datapath calls `ice_tx_wake()` once, since the reset closed the rings
without telling MAC; `tx_blocked.py` requires that it wake each ring by its
own MAC handle. A PHY firmware load still running after the reset leaves the
PHY setup pending, and an unreadable PHY firmware state
fails the rebuild closed.

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

## Statistics interface regression

`stats_read.py` executes the real selector API, port refresh, imported 40/32-bit
counter accumulation, baseline reset, and private-kstat fault reporting helper.
It covers all 14 mappings and aggregations, four unsupported selectors without
hardware access, the 10ms cache boundary, lock order, differing FMA policies,
reset baseline renewal without losing totals, and missing port information.
Five source mutations compile and fail runtime checks for mapping, unsupported
reads, refresh timing, service impact, and reset ownership. Use `--source` to
select another statistics implementation. `hw_stats.py` also rejects direct
cache, lock, or refresh-helper access from other driver modules.

## VSI statistics bounds regression

Run `python3 usr/src/test/ice-tests/vsi_stats.py` with Python 3 and a C99
compiler. The shared C runner extracts the actual `ice_stats_update_vsi()`,
statistics structure, maximum VSI count, and GLV register definitions.
Controlled imported-core boundaries count counter reads and the GLV_REPC clear
write. Five scenarios, each with initial and previously loaded counters,
exercise VSI numbers 0, 767, 768, UINT16_MAX, and a missing context. Rejected
identifiers issue no reads or writes and preserve cached counters and the
loaded flag; valid identifiers continue refreshing the expected registers.

Use `--source` for an earlier `ice_stats.c` and `--case` to select `zero`,
`last`, `limit`, `maximum`, or `missing`. The baseline fails `limit` and
`maximum`; a mutant using `>` rather than `>=` fails `limit`. This test does
not exercise device MMIO or firmware behavior.


## TX frame admission regression

```
python3 usr/src/test/ice-tests/tx_frame_limit.py
```

The runner compiles the real `ice_tx_frame_fits()` and the TX context type.
MAC does not bound a client's frame against the link SDU, and E810 reports a
packet above its programmed maximum as a malicious-driver event that halts
every client of the function. Cases cover a tagged maximum frame and one byte
more at MTU 1500 and 9000, the hardware maximum and a 65535-byte frame at MTU
9000, and LSO requests whose header plus MSS meets or exceeds the frame. It
also checks that the rule sits on the single admission path after LSO gating
and before any DMA binding, with its own `tx_oversize_drops` counter.

## RX interrupt routing regression

```
python3 usr/src/test/ice-tests/rx_intr_route.py
```

The runner compiles the real `QINT_RQCTL` writer, the lifecycle routing
transitions, and MAC's poll-mode callbacks against a recording register file
and a checked ring lock. Scenarios interleave a poll transition with reset
unmap and remap, and with stop's dissociation, and assert after every step
that the register equals the composition of ring state: no cause on a cleared
vector, no re-arm of a cause the lifecycle cleared, and MAC's chosen mode
restored by the rebuild's remap. It also checks that no other writer of the
register remains.

## Transceiver lock check

```
python3 usr/src/test/ice-tests/transceiver_lock.py
```

`ice_lock` is an interrupt-priority mutex. The check confirms the transceiver
read path, which any process in the link's zone can reach through
`DLDIOC_READTRAN`, holds only the adaptive lifecycle lock across its
admin-queue polling.

## Firmware command lock check

```
python3 usr/src/test/ice-tests/aq_locks.py [--list]
```

`aq_locks.py` builds a call graph of the glue and the vendored core and finds
every function that can reach `ice_sq_send_cmd()`, the one routine that
submits an admin or sideband queue command, or a reset poll (`ice_reset()`,
`ice_check_reset()`, `ice_pf_reset()`). It then walks each glue function,
tracking `mutex_enter()` and `mutex_exit()` by block, and fails on any call
into that set made while `ice_lock`, `ice_lse_lock` or a ring lock is held;
these are interrupt-priority mutexes. A block that unlocks and returns keeps
the lock state it was entered with. `--list` prints the core functions it
classifies. The check fails if it loses sight of known command wrappers such
as `ice_ena_vsi_txq()`, `ice_aq_sff_eeprom()` or `ice_fwlog_set()`, and a
planted violation and the TX queue disable put back under `ice_lock` must
both be reported. The walk ignores control flow beyond that, so it is a
guard, not a proof.

## DDP section bounds regression

```
python3 usr/src/test/ice-tests/ddp_sections.py
```

The runner compiles the real `ice_ddp_pkg_valid()` and its helpers against
structure stand-ins with the vendor field order and widths, then builds
packages whose section tables are valid, extend past the buffer, or declare
typed sections and counted arrays larger than their extent. The metadata case
reproduces the reviewed one-byte section at offset 4095. When the shipped
`firmware/ice.pkg` is present it must pass in full and fail when truncated by
one byte. It also checks that the core's metadata consumer verifies the size
it reads.

## MAC IPv6 extension-header regression

```
python3 usr/src/test/ice-tests/mac_ipv6_eh.py
```

The runner compiles the real MAC mblk cursor and L3 parser. Cases cover no
extension headers, a header split across mblks, the largest chain that fits
the 16-bit L3 length, a chain that exceeds it (which formerly returned `-1`
from a `bool` function with every output unset), and fragment flags. It also
checks that the caller defines its outputs before the parse.

## viona TX guard check

```
python3 usr/src/test/ice-tests/viona_tx_guards.py
```

viona is the path by which an untrusted guest reaches the driver. The check
confirms LSO is admitted only when the whole parsed header lies in the first
mblk, the protocol is TCP, and the guest's checksum location is the parsed
TCP checksum field, and that a non-LSO frame above the link MTU is dropped
before it reaches `mac_tx()`.

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

The suite above includes all source checks. Individual scripts remain
runnable, for example `python3 -B usr/src/test/ice-tests/rx_checksum.py`.
The following descriptions identify what each source check establishes.

`core_readme.py` requires `core/README.illumos` to name every core file
and function that carries an `illumos:` marker, and no file without one.

`manpage.py` checks that `ice(4D)` names every device ID the package binds,
marks every family except E810 as not validated on hardware, documents each
`ice.conf` property with the driver's default, and passes `mandoc -Tlint` at
the error level when mandoc is installed.

`cstyle_glue.py` runs `usr/src/tools/scripts/cstyle.pl -pP` over the driver
glue sources and the mdb module. It does not check the vendored `core/` code.
It reports SKIP when perl is not installed.

`exception_lists.py` checks that the `exception_lists/` files make the
copyright, cstyle, hdrchk, wscheck, keywords, and utf8check passes skip the
vendored `core/` code and the binary DDP package, and that no pattern skips
the driver glue.

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

`mac_filter.py` verifies private request construction and runs
`filter_boundary.py`, which rejects filter policy or imported switch-request
access outside the filter module.

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
run under the stat lock, the VSI error register is accumulated and explicitly
cleared through the common code, attach captures both baselines before exposing the
kstats, the kstat callbacks reject writes and lock correctly, teardown deletes
the kstats before destroying their lock, attach installs stats before MAC while
detach removes them before unmapping registers, and `ice_m_stat` sources the
MAC counters through the statistics owner. Port refreshes are rate-limited so a MAC
kstat snapshot reads the hardware counter bank once, and unsupported MAC
statistics do not trigger register reads. Register-access faults from a
supported MAC statistic report degraded service and return `EIO`, while
private-kstat failures retain the established unaffected-service policy.

`link_speed_caps.py` verifies that PHY setup advertises the full media speed
set with automatic FEC, media insertion reapplies that configuration, and the
cached supported and advertised speed/FEC values reach the GLDv3 statistics
and read-only property callbacks.

`lso.py` verifies the LSO capability gate (on by default, withheld in safe
mode or by the property), context descriptor
encoding, hostile-metadata checks, per-segment and per-packet descriptor
limits, LSO bind emission, frame-sized copy fallback, DMA cookie-size guard,
and compile-time descriptor-layout checks. The LSO header is copied into a
small-pool buffer, so only payload copies take LSO-pool buffers. Setting the
`tx_lso_enable`
driver property to 0 in `/kernel/drv/ice.conf` withholds LSO.

`rss.py` verifies that interrupt allocation takes its data-queue count from
`ice_queue_limit()` (executed by `queue_count.py`); that the granted vector
count can only lower the final queue count; that the 1:1
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
to retry; and both exits of the recycle path own the wakeup. A packet dropped
after its build returned TCBs or buffers re-drives reclaim when the ring is
blocked, since the pools are per ring and another sender may have blocked on
them. The TX path never waits on an LSO allocation, and an LSO payload copy
on a ring without an LSO pool is dropped rather than blocking the ring.

`tx_emit.py` compiles the actual descriptor writers, emission, DMA sync,
TCB cleanup, and completion walk. Twenty cases cover ordinary/LSO bindings,
all copy-buffer types, ring wrap, context and RS descriptors, exactly-once
ownership, pre-doorbell DMA failures, and doorbell errors. Descriptor fields
are decoded independently of the writers. Controls with wrong addresses,
TCB placement, rollback, or bind-handle selection fail at runtime. Pool and
DDI operations are boundary substitutes, not hardware validation.

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

`buf_pool.py` compiles the actual TX pool functions, the pool sizing, and
the LSO pool allocation and release that MAC start and stop run. For 1 to 127
rings and 64 to 4096 descriptors per ring, each ring's copy and small pools
take `MIN(per-ring count, cap / rings)` buffers whatever the descriptor count,
the instance totals stay within the caps, and every ring at 127 queues still
has LSO buffers for its largest packet. No LSO buffer or LSO bind handle
exists after `ice_buf_init()`. `ice_tx_lso_alloc()` gives every ring its LSO
pool and handles under the lifecycle lock, does nothing with LSO off, and
allocates nothing on a rebuild that kept them. A buffer or handle failure on
any ring returns failure with no ring holding part of a pool.
`ice_tx_lso_free()` releases them all, and detach releases pools a stop left.
The test also covers failure at every DMA allocation, repeated cleanup, and
per-ring exhaustion and returns. The source must keep no TX taskq and no LSO
state machine. Allocation and release boundaries assert that
no pool or ring lock is held. These controlled boundaries do not exercise
real DMA.

`pool_locks.py` checks that each ring's pools use the ring's TCB lock, which
is created at the negotiated interrupt priority before the pools and destroyed
after them, and that the old per-instance pools and locks are gone.
Construction and unwind rely on exclusive lifecycle ownership rather than
holding the lock. `ice_tx_lso_alloc()` takes no ring lock, and MAC start
calls it before it programs the queues and opens the rings; `ice_tx_stop()`
frees the pools only after the reclaim.

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

## On-system test suite

The `cmd`, `runfiles` and `tests` directories are an illumos test-runner
suite, installed to `/opt/ice-tests` by the `system/test/icetest` package.
Run it on a host with an ice link:

```
/opt/ice-tests/bin/icetest [-p peer] [-m mtu] [link]
```

The link defaults to `$ICE_TEST_LINK`, then to the first ice link. The
default runfile checks the published kstats and FMA counters (`attach`), the
LED identify modes through `dlled` (`led`), and the diagnostic ioctls
(`diag_ioctl.64`: privilege drops, size and cluster checks, a firmware log
configuration read). With a peer (`-p` or `$ICE_TEST_PEER`) the datapath
runfile also runs `datapath_accept.sh`. A test without its device or peer
reports SKIP. `attach` fails if an FMA counter kstat is missing or nonzero.
The suite does not reset the device, inject faults, or test memory ordering,
device timing, interrupt delivery or concurrency beyond what its traffic
produces. `onsystem_suite.py` checks the runfiles, scripts and package
manifest on the build host, and runs `attach` under `ksh` against a stub
`kstat` to check that a missing or nonzero FMA counter fails it.

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
`iperf` run moves traffic and advances the PF byte counters and the per-ring
`tx_lso_packets`, with no `tx_lso_*` or `tx_hck_*` refusals; MAC and CRC error
counters stay zero; and three plumb/unplumb cycles each bring the link back
with FMA still clean. Exit status is zero only if every check passes. Run it
from both hosts to cover both traffic directions.

The kstat instance comes from the ice device behind `$ICE_TEST_LINK`
(`dladm show-phys -p -o device`), so a renamed link such as `net0` over
`ice3` reads `ice:3`. A link that is not an ice device, or an
`$ICE_TEST_DEVICE` that names another device, stops the run before it
changes anything.

The script replumbs the link and changes its MTU, so it refuses a link with
IP configuration. `ICE_TEST_ALLOW_IP=1` lets it take a link whose addresses
are all temporary static, DHCP or addrconf addresses; persistent `ipadm`
configuration and other address types are still refused. It records the MTU,
those addresses and the default routes over the link, makes only temporary
changes, and restores the record from an `EXIT`, `INT`, `TERM` and `HUP`
trap. A delete of the test address or interface may fail only when the
object is confirmed absent. After the restore the script compares the MTU,
the interface, the static addresses and the types of the other addresses,
and the default routes with the record. A restore that fails, or a final
state that differs, prints `RESTORE FAILED` and fails the run. The
`icetest` wrapper applies the same refusal before it starts the datapath
runfile, and the `led` test returns the LED to firmware control from its own
exit trap.

`accept_script.py` runs the script on the build host against stub `dladm`,
`ipadm`, `netstat`, `route`, `kstat` and traffic commands and checks these
rules: the instance comes from the device, a configured link is left
untouched without the override, and a run that finishes or takes `SIGTERM`
leaves the MTU, addresses and default route as they were. A delete that
fails while the object remains, and a restored address that silently goes
missing, must each fail the run with `RESTORE FAILED`; a delete of an
address that was never created must not. It also runs the `icetest` refusal
under `ksh`.

Historical validation recorded in commit `60beba06389` (2026-07-18) reported
boston<->hunter at MTU 1500 (9.36 Gbps) and 9000 (9.59 Gbps), with all checks
green. Those earlier branch results do not validate the reviewed fixes.

On 2026-09-23 this revision ran on boston (E810-C 0x1592, firmware 6.2.9,
DDP 1.3.41.0) against hunter, which ran an older driver, in one direction
only (boston transmits). The platform granted 8 MSI-X vectors, so the driver
used 7 queue pairs, and all 7 receive rings took traffic. With LSO on,
`datapath_accept.sh` passed every check at MTU 1500 (7.3 to 9.2 Gbps over
four runs) and 9000 (9.88 Gbps); with `tx_lso_enable=0`, MTU 1500 reached
5.3 to 5.9 Gbps. The on-system `attach`, `led` and `diag_ioctl.64` tests
passed, detach and reattach with the LED in identify mode were clean, and
the mdb module read the instance and its rings. The reverse direction, the
other device families, reset and fault injection were not tested, and these
runs do not show memory ordering or interrupt delivery under load beyond the
traffic they passed.
