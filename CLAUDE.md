# bhyve Live Migration Test Process

This branch (`bhyve-live-migration`) implements cross-host live migration for
bhyve on SmartOS/illumos using a Rust GZ agent.

## Current Status — WORKING END-TO-END ✅

- Guest Ubuntu kernel survives migration
- Both vCPUs run normally post-migration, no kernel panic
- Network fully functional: 50/50 ping with 0% packet loss, ~0.24ms latency
- Built-in xxh3-64 RAM hash validation catches corrupted transfers
- Uses pause-first single-pass RAM transfer (guest paused before any RAM read)

### The fix that closed the loop
Pre-pause + dirty-convergence protocol was racy: pages captured mid-write while
the guest was running produced memory corruption (timer wheel LIST_POISON,
torn slab pointers → kernel panic post-resume). Switching to **pause-first
single-pass RAM transfer** (pause viona + vCPUs, push all RAM once, hash, then
device state) trades ~3s pause per GB for provable consistency. Matches the
already-working file-based `bhyve_migrate_import()` sequence.

Follow-up work (not blocking):
- Restore live-migration downtime optimization (dirty convergence) when we can
  guarantee page atomicity (e.g., snapshot memory before reads)
- Auto-stop source VM after successful migration (agent doesn't yet)
- Automate ZFS disk send/recv

## Architecture

```
Source GZ Agent ──TCP/WebSocket──> Dest GZ Agent
  │ /dev/vmm mmap                     │ /dev/vmm mmap
  │ VM_NPT_OPERATION (dirty tracking) │ page writes
  │ VM_DATA_READ (kernel state)       │
  │                                   │
  ╰─ bhyve_control sock ─╮    ╭─ bhyve_control sock ─╯
                          │    │
                    bhyve (in zone, per NUC)
```

- **Source bhyve**: paused via `pause-devices` (viona rings) + `pause-vm` (vCPUs)
- **Source agent**: calls `export-state` to get packed nvlists (kern + dev), sends to dest
- **Dest bhyve**: starts in migrate-listen mode (no UEFI), blocks in `bhyve_control_wait_import()`
- **Dest agent**: receives state, calls `import-state`
- **Dest bhyve import-state**: pause → state writes → PCI restore → resume → signal condvar
- **Dest main thread**: unblocks, starts vCPU threads → guest runs

## Critical Files

**C bhyve** (this repo, `bhyve-live-migration` branch):
- `usr/src/cmd/bhyve/common/bhyve_control.c` — control socket, cmd_import_state
- `usr/src/cmd/bhyve/common/bhyve_migrate.c` — file-based checkpoint (working reference)
- `usr/src/cmd/bhyve/common/bhyverun.c` — migrate-listen mode, wait_import
- `usr/src/cmd/bhyve/common/pci_virtio_viona.c` — pci_viona_restore, pci_viona_baraddr
- `usr/src/cmd/bhyve/common/pci_emul.c` — pci_pause_devices

**Rust agent** (separate repo):
- `~/workspace/triton_clean/vmm-migrate-agent/`
- `src/source.rs` — source orchestrator
- `src/destination.rs` — dest orchestrator
- `src/bhyve_ctl.rs` — control socket client
- `src/vmm_dev.rs` — /dev/vmm FFI (mmap, data_read/write, npt dirty)

## Test Environment

| Host | IP | Role | UUID |
|------|-----|------|------|
| Build | 142.147.4.194 | Builds bhyve + Rust agent | - |
| Headnode | 10.199.199.10 | SSH jump box; has SDC key | - |
| nuc1 (source) | 10.199.199.40 | VM source | 8b2a9975-6354-8a94-39e4-1c697aa96b33 |
| nuc0 (dest) | 10.199.199.41 | VM dest | f7d2efb6-8c3b-e1fe-111f-88aedd065474 |
| Test VM | 10.199.199.50 | Ubuntu guest | 4efa9014-a702-497b-a1ea-f68ee0bdf3c4 |

**SSH keys:**
- `~/.ssh/sdc.id_rsa` — local key for headnode only
- `/root/.ssh/sdc.id_rsa` on headnode — key to both NUCs
- Build host: direct root SSH (no key arg needed from local)

## Build Pipeline

### C bhyve (build on 142.147.4.194)

The smartos-live tree at `/opt/smartos-live-vmmctl` already has our modified source
files from prior sessions. Just update `bhyve_control.c` (or whatever we're iterating on)
and rebuild.

```bash
# Copy modified file to build host
scp -o StrictHostKeyChecking=no \
  usr/src/cmd/bhyve/common/bhyve_control.c \
  root@142.147.4.194:/opt/smartos-live-vmmctl/projects/illumos/usr/src/cmd/bhyve/common/

# Build (use dmake via bldenv, NOT gmake — gmake -j4 fails)
ssh -o StrictHostKeyChecking=no root@142.147.4.194 '
  cd /opt/smartos-live-vmmctl/projects/illumos
  ./usr/src/tools/scripts/bldenv illumos.sh "cd usr/src/cmd/bhyve && dmake" 2>&1 | grep -E "error:|^-rw"
  ls -la usr/src/cmd/bhyve/amd64/bhyve
'
```

**Build host gotchas:**
- Does NOT work: `gmake -j4` (dmake parsing of -j flag)
- Does NOT work: bare `gmake` on the old `/opt/bhyve-illumos-usr-src-cmd/` tree (missing headers)
- Works: `./usr/src/tools/scripts/bldenv illumos.sh "cd usr/src/cmd/bhyve && dmake"`
- Build uses `-Werror` — `stride set but not used` will fail the build

### Rust agent (build on 142.147.4.194 — cargo is NOT on NUCs or headnode)

```bash
# Use scp (NOT rsync — rsync misses src/ contents for some reason)
scp -o StrictHostKeyChecking=no -r \
  ~/workspace/triton_clean/vmm-migrate-agent/src/ \
  root@142.147.4.194:~/vmm-migrate-agent/src/

# If scp creates nested src/src/, fix it:
ssh root@142.147.4.194 'cd ~/vmm-migrate-agent && mv src/src/*.rs src/ && rmdir src/src'

# Build
ssh root@142.147.4.194 '
  export PATH=$HOME/.cargo/bin:$PATH
  cd ~/vmm-migrate-agent
  cargo build --release
'
```

### Deploy binaries

Both bhyve and the Rust agent must be uploaded to local, then to the headnode,
then SCP'd from headnode to the NUCs. The build host cannot SSH directly to NUCs
(no key).

```bash
# bhyve binary: build host -> local -> headnode -> NUCs
ssh root@142.147.4.194 "cat /opt/smartos-live-vmmctl/projects/illumos/usr/src/cmd/bhyve/amd64/bhyve" > /tmp/bhyve.new
scp -i ~/.ssh/sdc.id_rsa /tmp/bhyve.new root@10.199.199.10:/tmp/bhyve.new

ssh -i ~/.ssh/sdc.id_rsa root@10.199.199.10 '
  UUID=4efa9014-a702-497b-a1ea-f68ee0bdf3c4
  SDC_KEY=/root/.ssh/sdc.id_rsa
  SCP="scp -i $SDC_KEY -o StrictHostKeyChecking=no -o IdentitiesOnly=yes"
  $SCP /tmp/bhyve.new root@10.199.199.40:/zones/$UUID/root/bhyve.rust
  $SCP /tmp/bhyve.new root@10.199.199.41:/zones/$UUID/root/bhyve.rust
'

# Agent binary: same pattern, deploy to /opt/vmm-migrate-agent
ssh root@142.147.4.194 "cat ~/vmm-migrate-agent/target/release/vmm-migrate-agent" > /tmp/vmm-migrate-agent
chmod +x /tmp/vmm-migrate-agent
scp -i ~/.ssh/sdc.id_rsa /tmp/vmm-migrate-agent root@10.199.199.10:/tmp/vmm-migrate-agent

ssh -i ~/.ssh/sdc.id_rsa root@10.199.199.10 '
  SDC_KEY=/root/.ssh/sdc.id_rsa
  SCP="scp -i $SDC_KEY -o StrictHostKeyChecking=no -o IdentitiesOnly=yes"
  chmod +x /tmp/vmm-migrate-agent
  $SCP /tmp/vmm-migrate-agent root@10.199.199.40:/opt/vmm-migrate-agent
  $SCP /tmp/vmm-migrate-agent root@10.199.199.41:/opt/vmm-migrate-agent
'
```

## Running a Migration Test

Always go through the headnode — it has the only SSH key to the NUCs.

```bash
ssh -i ~/.ssh/sdc.id_rsa root@10.199.199.10 '
  UUID=4efa9014-a702-497b-a1ea-f68ee0bdf3c4
  SDC_KEY=/root/.ssh/sdc.id_rsa
  SSH="ssh -i $SDC_KEY -o StrictHostKeyChecking=no -o IdentitiesOnly=yes"

  # 1. Clean slate
  $SSH root@10.199.199.40 "vmadm stop $UUID -F 2>/dev/null; pkill -9 vmm-migrate; rm -f /var/run/vmm-migrate.sock"
  $SSH root@10.199.199.41 "vmadm stop $UUID -F 2>/dev/null; pkill -9 vmm-migrate; rm -f /var/run/vmm-migrate.sock"
  sleep 3

  # 2. Start source, wait for full boot (45s is safe)
  $SSH root@10.199.199.40 "rm -f /zones/$UUID/root/tmp/vm.checkpoint /zones/$UUID/root/tmp/migrate.listen; vmadm start $UUID"
  sleep 45
  $SSH root@10.199.199.40 "ping 10.199.199.50 3"    # must work before migrating

  # 3. Start dest in migrate-listen mode
  $SSH root@10.199.199.41 "touch /zones/$UUID/root/tmp/migrate.listen; vmadm start $UUID"
  sleep 3

  # 4. Start agents on both hosts
  $SSH root@10.199.199.40 "RUST_LOG=info nohup /opt/vmm-migrate-agent --listen 0.0.0.0:4567 --api-socket /var/run/vmm-migrate.sock > /tmp/vmm-agent.log 2>&1 &"
  $SSH root@10.199.199.41 "RUST_LOG=info nohup /opt/vmm-migrate-agent --listen 0.0.0.0:4567 --api-socket /var/run/vmm-migrate.sock > /tmp/vmm-agent.log 2>&1 &"
  sleep 2

  # 5. Trigger migration
  # Note: shell quoting is painful — use double-then-single nesting
  $SSH root@10.199.199.40 "echo '"'"'{\"command\":\"migrate\",\"vm\":\"$UUID\",\"dest\":\"10.199.199.41:4567\"}'"'"' | socat - UNIX-CONNECT:/var/run/vmm-migrate.sock"
  sleep 10

  # 6. Stop source (agent does NOT stop it automatically — TODO)
  $SSH root@10.199.199.40 "vmadm stop $UUID -F 2>&1"
  sleep 3

  # 7. Verify dest
  $SSH root@10.199.199.41 "ps -ef | grep bhyve | grep $UUID | grep -v grep | wc -l"  # should be 1
  $SSH root@10.199.199.41 "ping 10.199.199.50 5"                                       # currently broken
'
```

## Debugging

### Read dest bhyve stderr (import-state logs)

```bash
$SSH root@10.199.199.41 "grep -E \"import-state|viona restore|migrate-debug\" /zones/$UUID/logs/platform.log | tail -20"
```

Each log line is wrapped as `{"msg":"...", "stream":"stderr"}`. To strip the JSON:
```bash
| sed 's/.*\"msg\":\"/  /;s/\",\"stream.*//' | sed 's/\\\\n/\n  /g'
```

### viona kstat (on dest NUC)

```bash
$SSH root@10.199.199.41 "kstat -p viona::: 2>/dev/null | grep -E 'packets|bytes'"
```

Interpretation:
- `rx_packets > 0, tx_packets == 0` → guest RX works, TX is dead (current bug)
- `rx_packets == 0, tx_packets == 0` → both dead (worse)
- Both > 0 → fully working

### dtrace viona functions

```bash
# Available viona fbt probes
dtrace -ln 'fbt:viona::' | grep -Ei 'notify|kick|hook|tx'

# Trace MMIO notification path (this does NOT fire after migration - the bug)
dtrace -qn 'fbt::viona_notify_mmio:entry {
  printf("mmio: write=%d addr=0x%x\n", arg1, (int)arg2);
}'

# Trace kick + worker lifecycle
dtrace -qn '
fbt::viona_ioc_ring_kick:entry { @kick = count(); }
fbt::viona_worker_tx:entry { @tx_worker[tid] = count(); }
fbt::viona_worker_rx:entry { @rx_worker[tid] = count(); }
fbt::viona_notify_mmio:entry { @mmio = count(); }
tick-5s {
  printa("kicks: %@d\n", @kick);
  printa("tx_worker entries: %@d\n", @tx_worker);
  printa("rx_worker entries: %@d\n", @rx_worker);
  printa("mmio notify: %@d\n", @mmio);
  exit(0);
}'
```

Workers run in a long-loop; `:entry` probe fires once per thread creation, not per iteration.

### VM exit codes (`code=N` in migrate-debug logs)

From `usr/src/uts/intel/sys/vmm.h` enum `vm_exitcode`:
- 0 = INOUT
- 1 = VMX (inst_error=7 = invalid control fields → see root cause history below)
- 2 = BOGUS (HLT pseudo-exit)
- 3 = RDMSR
- 4 = WRMSR
- 5 = HLT
- 14 = SUSPENDED (vCPU tried VM_RUN while paused — expected during startup race)
- 15 = MMIO

### mdb on the dest NUC

```bash
echo "::walk viona_link_t | ::print viona_link_t l_notify_mmaddr" | mdb -k
# (walk may fail — viona types aren't always registered as mdb walks)
```

## Root Cause History (what we fixed, in order)

1. **Triple fault (VM_SUSPEND_TRIPLEFAULT) — FIXED**
   - `vlapic_data_write()` checks `vm_is_paused()` and defers LAPIC timer callout
     scheduling to `vm_resume_instance()`. Without pause, LAPIC timer import arms
     callouts with wrong time base.
   - Fix: wrap state writes with `vm_pause_instance` / `vm_resume_instance`.

2. **`VNA_IOC_SET_FEATURES` EFAULT — FIXED**
   - Kernel expects a pointer, not a value. `ioctl(fd, VNA_IOC_SET_FEATURES, features)`
     (value) returns EFAULT.
   - Fix: pass `&features` (pointer).

3. **VMX entry failure (inst_error=7) — FIXED**
   - Two causes:
     a) **Missing segment selectors.** `vm_set_desc` only sets base/limit/access;
        the selector is a SEPARATE VMCS field set via `vm_set_register`. CS=0x0
        post-import causes inst_error=7.
        Fix: export format is now `[regid:4, base:8, limit:4, access:4, sel:8]` = 28 bytes
        per segment. Import calls `vm_set_register(vcpu, regid, sel)` for non-GDTR/IDTR.
     b) **Order inversion: resume BEFORE pci_restore_all.** `VNA_IOC_SET_NOTIFY_MMIO`
        calls `vm_mmio_hook()` which modifies VM-level `mmiohooks` state. If vCPUs
        are already running (post-resume), they can enter VMX with inconsistent
        MMIO config → inst_error=7.
        Fix: match file-based path — pause → state writes → pci_restore_all → resume.

4. **Dirty pages sent as zeroes — FIXED**
   - `send_dirty_batch()` in source.rs was sending `vec![0u8; size]` instead of
     reading from the mmap'd guest region.
   - Fix: pass mmap pointers through, compute offset per GPA, call `read_guest_page`.

5. **Pre-captured destination VMM_TIME was stale — FIXED**
   - Agent captured dst time, sent over socket. By the time bhyve processed it,
     it could be seconds old. Time adjustment used stale data.
   - Fix: bhyve's `cmd_import_state` reads dst VMM_TIME live from kernel via
     `vm_data_read(ctx, -1, VDC_VMM_TIME, ...)`. Agent no longer sends `time_len`.

6. **Per-vCPU import order wrong — FIXED**
   - Control socket path wrote `MSRs/LAPIC/VMM_ARCH → registers → segments → FPU`.
   - File-based working path does `registers → segments → FPU → MSRs/LAPIC/VMM_ARCH`.
   - Fix: match the file-based order.

## Resolved Bug — Guest Kernel Panic Post-Migration

**RESOLVED via pause-first RAM transfer**. The panic was triggered by guest
memory being inconsistent on the destination (pages captured mid-write during
pre-pause convergence). Removing the pre-pause push and doing a single
paused push fixed all symptoms: no panic, TX works, ping works.

Below is the historical debugging notes, preserved for reference.

---

**RESOLVED THEORY**: The viona MMIO notify hook is installed CORRECTLY after migration
(confirmed via debug logs: hook at `0xc200d000` matches the source's BAR + c_baroff).
But the hook never fires because **the guest kernel itself panics ~20-30 seconds after
resume**, so it stops generating any MMIO writes. TX=0 is a symptom, not the root cause.

**Confirmed symptoms**:
- `viona kstat rx_packets` grows (VNIC still delivers packets to guest RX ring)
- `viona kstat tx_packets` stays 0 (guest's virtio-net driver never TXes after some point)
- `dtrace fbt::viona_notify_mmio:entry` never fires
- `dtrace fbt::vm_service_mmio_write:entry` never fires either — guest makes ZERO MMIO
  writes of any kind post-panic
- Guest console shows `Kernel panic - not syncing: Fatal exception in interrupt`

**Guest panic signatures observed** (in `/zones/<uuid>/logs/console.log`):
- `RIP: __run_timers+0x224/0x300`, `RAX: dead000000000122` (LIST_POISON1 + 0x22 →
  timer wheel traversal hit a freed list head)
- `RIP: __kmem_cache_alloc_bulk+0x6e/0x340`, `RAX: 379de3607cd3005e` (weird pointer
  value — torn or poisoned slab cache entry)
- Multiple different panics across runs — suggests widespread memory corruption, not
  one specific code path

**Root cause investigation (findings from /codex)**:

1. **`pci_restore_cfgspace()` is incomplete** (`usr/src/cmd/bhyve/common/pci_emul.c:2503`).
   It restores raw `pi_cfgdata` bytes + MSI/MSI-X state, but does NOT:
   - Restore `pi->pi_bar[idx].addr`
   - Call `register_bar()` to re-register BAR MEM/IO ranges with the VMM
   - Call `pe_baraddr()` callbacks to notify devices of BAR changes

   We worked around this for viona by manually calling `pci_viona_baraddr()` in
   `pci_viona_restore()`. That fixes the viona notify hook, but the underlying
   `pi_bar[]` state is still stale — ALL other PCI devices (virtio-blk, etc.)
   also have stale BAR tracking.

2. **`VMM_BLOCK_HOOK` (kernel flag)** can block `vmm_drv_mmio_hook()` with EBUSY
   if set. Only `VM_REINIT` sets it, so probably not a factor here — but worth
   confirming our ioctl return value is checked.

3. **TSC/hrtime adjustment** — `cmd_import_state` applies VMM_TIME adjustment
   but the guest uptime IS advancing (46s at capture → ~46s at resume → guest runs
   for another 20s → panic at ~68s). So the time math doesn't go backwards, but
   the guest kernel's memory gets corrupted within seconds of resume.

**Most likely root cause — RAM corruption / missing dirty pages**:

The Rust agent's dirty-page convergence runs WHILE the source guest is running. It:
1. Pushes all pages unconditionally
2. Resets EPT dirty bits
3. Waits 500ms
4. Pushes dirty pages, resets, repeat until under threshold
5. Pauses, pushes final dirty pages

If a page is written to between `npt_reset_dirty` and the next read (even briefly),
we might miss that write — dirty bit reset BEFORE we've captured the data. The
agent reads the dirty bitmap, then re-reads the page; but between reading the bitmap
and re-reading the page, another write could happen without re-flagging (because
we just cleared the bit).

RX still works post-migration because the guest's RX ring buffers are populated by
viona post-migration — not read from pre-migration state. TX requires the guest's
OWN memory state (scheduler, timer wheel, etc.) to be intact, and even small
memory corruption there → kernel panic.

**Next debug steps (ranked)**:

1. **Checksum guest RAM post-migration**: read all pages on source (paused) and dest
   (resumed but before guest runs), compare. If checksums differ, confirm RAM
   corruption. Use `dd if=/dev/vmm/<name>` + `sha256` or similar.
2. **Review dirty-page protocol for races**: in `source.rs`, examine the ordering
   of `npt_reset_dirty()` vs `read_guest_page()`. The correct order is
   *collect → reset → send* if we can guarantee the pages weren't re-dirtied;
   but with a running guest, better is *snapshot → send* where snapshot is atomic
   (e.g., use EPT write-protect + copy-on-write).
3. **Compare with vmmnew**: vmmnew's live migration works on these same hosts.
   Diff the dirty-tracking + RAM-push protocol against ours. Files:
   `~/workspace/triton_clean/vmmnew/crates/vmm-migrate/src/` (source_sync,
   destination_sync, protocol, etc.).
4. **Try file-based checkpoint path via agent**: as a sanity check, have the agent
   export to a temp file on source (using existing SIGUSR1 path), SCP the file,
   and import via the existing `bhyve_migrate_import()`. If that works end-to-end
   via the agent, the RAM-mmap path is the bug.
5. **Fix `pci_restore_cfgspace`** to call `register_bar()` / `pe_baraddr()` for each
   BAR after memcpy'ing config data. Without this, non-viona devices may also have
   inconsistent MMIO routing.

**Ruled out**:
- MAC conflict (we stop source VM post-migration)
- Wrong pause/resume order (fixed: pause → writes → PCI restore → resume)
- Missing segment selectors (fixed: export includes sel now)
- Missing `VNA_IOC_SET_FEATURES` (fixed: pass `&features` pointer)
- Missing MSI-X ring setup (done via `pci_viona_ring_set_msix`)
- Missing MMIO notify hook (installed at correct address — debug log confirms)
- Order inversion resume before PCI restore (fixed, matches `bhyve_migrate_import`)

**Reference**: `VMM_BLOCK_HOOK` codepath in `usr/src/uts/intel/io/vmm/vmm_sol_dev.c:2513`,
`2577`, `2684`. `pci_restore_cfgspace` gap in `usr/src/cmd/bhyve/common/pci_emul.c:2503-2542`.

## Style & Convention

- Match file-based `bhyve_migrate.c` / `bhyve_migrate_import()` ordering and approach
  — it's the reference implementation that works.
- Don't use `pre-captured` data in the agent when bhyve can read live from kernel.
- When adding fields to the segment export format, keep it backward compatible
  (we support both 20-byte and 28-byte formats in import).
- Always read before edit (conversation context may be summarized — re-read the file).
