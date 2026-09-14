# ICE filter ownership and recovery

MAC owns client reference counts. Its unicast address/VLAN records, multicast
memberships, and device-promiscuous count determine when the driver receives
callbacks. ICE does not duplicate those counts. The driver's `vi_macs` set
records addresses accepted for replay or retirement, including station and
broadcast defaults installed at attach until those addresses are retired.
It is neither a per-client reference table nor hardware readback.

`ice_filter.c` owns that set, its lock, accepted promiscuous policy, and all
request construction. GLD callbacks submit intent through the filter setters;
VSI attach, rebuild, and teardown use explicit filter lifecycle operations.
Other modules do not walk the address list or construct imported filter types.

The imported switch code owns separate `filt_rules` entries, firmware rule
IDs, and VSI-list membership maps. Callback request entries are temporary;
the imported code copies the information needed into its own allocations.
Those allocations survive until imported removal, replay cleanup, or common
teardown. Driver code must not edit imported lists to imitate a firmware
operation. Hardware rules constitute a third state: a command failure can
leave hardware, imported bookkeeping, and the driver's accepted policy out
of agreement.

## Serialization and ordinary operations

Callbacks and rebuild hold `ice_rebuild_lock`. The nested `vi_mac_lock`
protects the address set and is released around blocking switch commands.
Attach owns the instance before publication; final teardown runs after
callbacks and reset work have drained. Imported recipe locks protect their
own lists. See [LIFECYCLE.md](LIFECYCLE.md) for the surrounding lifetime rules.

A healthy duplicate address add succeeds without another switch command.
An absent removal returns `ENOENT`. New addresses enter `vi_macs` only after
the imported add succeeds. Successful removal retires the address. These
set semantics are distinct from imported duplicate handling: nonshared
unicast add can skip an existing rule, while adding an already present VSI
to multicast or promiscuous rules can return `ICE_ERR_ALREADY_EXISTS`.

`ice_promisc_on` records accepted promiscuous policy. MAC owns its reference
count; the driver needs only a boolean. The policy comprises unicast and
multicast rules in RX and TX. Broadcast uses the ordinary broadcast filter.
A same-state callback succeeds without repeating the four-rule operation.
Rebuild calls `ice_filters_replay_promisc()` so an accepted enabled policy is
programmed again even though its boolean is unchanged. Callback setters acquire
the lifecycle lock; replay requires it already held. Setup and address replay
return imported ICE status; setters and promiscuous replay return errno.

## Uncertain commands and ownership retirement

An imported filter operation can fail after changing part of the switch.
For example, bulk unicast add allocates management entries after programming
rules, and an allocation failure can leave a rule unrecorded. An indeterminate
admin-queue completion likewise cannot prove that hardware was unchanged.
A remove operation that consults imported lists cannot discover every such
rule.

On a failed add, the callback preserves the original errno, leaves the address
unaccepted, and requests recovery. On a failed removal, it retires the address,
requests recovery, and returns success. MAC client teardown can discard its
reference even when removal returns an error; retaining the driver's record
would replay an address that the client has already abandoned.

Failed promiscuous enable restores the previous accepted boolean and returns
the original errno. It attempts to clear the entire mask, then requests reset
whether that rollback succeeds or fails. A successful clear removes recorded
partial rules but cannot disprove an unrecorded hardware rule. Errno is decoded
before rollback can overwrite the admin-queue status. Failed disable clears
the accepted boolean, requests recovery, and succeeds so MAC can retire its
last owner.

Recovery atomically latches `ERROR | PFR_REQ`, reports service lost and link
DOWN, and redispatches the reset worker under `ice_rebuild_lock`. This gates
software traffic and further additions; it does not acknowledge hardware
filter deletion, drain already admitted callbacks, or stop DMA. The existing
reset worker establishes the hardware reset barrier. Dispatch failure leaves
the request owed for retry by the admin periodic or a lifecycle gate owner.

While `PFR_REQ` or `RESET_PENDING` is owed, adds and promiscuous enables return
`EIO`. Tracked removals and promiscuous disable retire software ownership
without another command. Missing removals still return `ENOENT`. If a request
arrives during the address-list check, an add whose command is skipped cannot
be recorded as successful. A command already issued and completed successfully
still contributes its accepted address to later replay. Plain `ERROR` alone
allows a firmware attempt; command failure then requests the stronger recovery.

Terminal `RESET_FAILED` uses the same software retirement rules but queues no
further recovery. It blocks start and replay until driver reload. Imported
records remain owned by common teardown. None of these retirement rules is a
DMA isolation guarantee.

## Attach, rebuild, and teardown

Attach creates the VSI and installs station and broadcast filters. It tracks
both only after the batch succeeds. Failure performs best-effort rule/VSI
cleanup and then follows the common attach unwind. VSI setup does not destroy
its caller's address ownership: attach's failure label owns complete teardown;
a rebuild setup failure preserves records and partial VSI state for terminal
client retirement and eventual detach.

After a confirmed reset clears hardware rules, rebuild recreates the VSI.
`ice_replay_pre_init()` moves stale imported operational entries onto replay
lists so fresh adds do not collide with them. ICE then builds requests from
`vi_macs`, restores RSS, and applies only the accepted promiscuous policy.
It frees the old imported replay entries on every exit after pre-init,
including pre-init failure. This is explicit driver-policy replay, not the
imported `ice_replay_vsi()` dispatcher. A newly arriving reset request remains
owed and does not suppress the current worker's direct replay. Reconstruction
failure is terminal; the driver's address set remains available for retirement.

Final VSI teardown asks the filter owner to attempt switch removal and destroy
its address records and lock, then releases the hardware VSI. Software records
are disposed of regardless of command status. `ice_deinit_hw()` frees imported
bookkeeping and locks. Software disposal is not a firmware acknowledgment;
detach establishes packet DMA isolation separately before irreversible MAC
unregister and resource release.

## Source and validation

The ownership boundaries are implemented in [ice_filter.c](ice_filter.c).
[ice_gld.c](ice_gld.c) adapts MAC callbacks; [ice_vsi.c](ice_vsi.c) sequences
filter lifecycle operations around VSI and RSS programming. Imported
creation/removal is in
[core/ice_switch.c](core/ice_switch.c); replay preparation and common teardown
are in [core/ice_common.c](core/ice_common.c).

The MAC side is defined by `mac_add_macaddr_vlan()`, `mac_remove_macaddr_vlan()`,
`mac_fini_macaddr()`, and `i_mac_promisc_set()` in [../mac/mac.c](../mac/mac.c),
client teardown in [../mac/mac_datapath_setup.c](../mac/mac_datapath_setup.c),
and final multicast deletion in [../mac/mac_bcast.c](../mac/mac_bcast.c).

The portable `terminal_filters.py` and `filter_requests.py` regressions execute
actual callback, filter policy, setup, attach, replay, and teardown C bodies
with controlled
imported-core boundaries. They verify ownership decisions, late reset requests,
original errno, command counts, lock boundaries, and replay selection. They do
not emulate the device or prove firmware reset/deletion completion.
`mac_filter.py` also enforces that only the filter module accesses private
policy or constructs imported switch requests. Hardware
acceptance must inject partial switch/AQ failures with active clients, close
clients during owed and terminal recovery, verify the post-reset receive set,
and detach without leaked MAC ownership or DMA access.
