# NVMe over Fabrics for illumos

**Status:** Draft / design proposal
**Scope:** In-kernel NVMe-over-Fabrics (NVMe-oF) host and controller for illumos-joyent, ported from FreeBSD, as the block data path for a distributed block storage system. TCP transport first; ANA/multipath designed in from v1; RDMA (RoCE v2) offload as a later, pluggable phase.
**Audience:** Kernel engineers (illumos storage/networking), storage architects.

---

## 1. TL;DR

illumos has no NVMe-oF (host or target) and no modern RDMA. FreeBSD has a clean, native, BSD-2-licensed in-kernel NVMe-oF implementation whose transport layer is pluggable. The plan:

1. **Port FreeBSD's transport-independent nvmf core and its TCP transport** onto illumos. The core is pure C with **zero LinuxKPI** dependency, so it ports with mechanical KPI translation, not a rewrite.
2. **Re-bind the controller (target) side to STMF** instead of FreeBSD CTL. Because FreeBSD's NVMe target already translates NVMe commands into the SCSI/CTL model, and STMF is illumos's CTL-equivalent, this is a faithful re-targeting. A major payoff: the NVMe **reservation** primitive (absent in FreeBSD's target) comes nearly for free by translating to STMF/`sbd`'s existing SCSI-3 persistent-reservation machinery.
3. **Re-bind the host (initiator) side to `blkdev`** instead of FreeBSD CAM. The host path is block-native, so this is a clean swap of the device-presentation layer.
4. **Design ANA / multipath / failover in from v1.** ANA is net-new relative to the FreeBSD port on both sides and it changes the host object model, so its *structure* is a v1 requirement (not a later bolt-on); the *mechanism* lands by Phase 3.5; only the failover *policy* is deferred to the distribution layer. See Section 9.
5. **Keep RDMA out of scope for v1.** Ship NVMe-oF/TCP, measure, then add a native RoCE v2 transport behind the same vtable. NVMe-oF is chosen precisely because its transport seam is pre-cut at the two functions where RDMA offload lives.

What this does **not** do: replication, sharding/placement, consistency, or the distribution control plane. NVMe-oF is the per-link block data path; ANA is the failover *hook*. The distributed system sits above and is a separate program of work (Section 13).

Rough effort for bidirectional NVMe-oF/TCP with ANA on illumos: **~8-14 months, 1-3 engineers.** RDMA transport adds materially on top (prior estimate: +6-15 months).

---

## 2. Goal and non-goals

### Goal
A production-quality, in-kernel NVMe-oF data path on illumos:
- **Target:** export a zvol (or any STMF LU) as an NVMe-oF namespace over the network, with ANA group state driveable by a control plane.
- **Initiator:** attach a remote NVMe-oF namespace as a single local `blkdev` disk aggregated over multiple paths, with ANA-aware path selection and failover.
- Standards-compliant enough to interoperate with Linux (`nvmet` / `nvme-cli`) in both directions, including multipath.
- Transport-pluggable, so RoCE v2 can be added later without touching the protocol layer.

### Non-goals (this document)
- The distributed layer: replication, erasure coding, placement, membership/consensus, *failover policy*, scrub/rebuild. Sketched in Section 13; owned by a separate effort. (ANA *mechanism* is in scope; ANA *policy* is not.)
- RDMA offload implementation. Deferred to Phase 4; only the seam is built now.
- A LinuxKPI compatibility shim for illumos. Explicitly rejected (Section 4, D4).

---

## 3. Background: the gap and the asset

### illumos today
- **No NVMe-oF.** The `nvme` driver is local-PCIe initiator only. No fabrics host, no target.
- **No modern RDMA.** IBTF/IBTL is InfiniBand-only; `mlxcx` (ConnectX-4/5/6) is Ethernet-only. No RoCE anywhere.
- **No reusable host multipath.** `scsi_vhci`/MPxIO is bound to SCSA/`scsi_pkt` (SCSI-only); `blkdev` is single-path. NVMe host multipath must be built natively (Section 9).
- **Reusable target machinery:** STMF/COMSTAR, `stmf_sbd` over zvol, `blkdev`, `ksocket`, `srpt` (an RDMA-based STMF port provider, structural reference), and STMF's binary ALUA/active-standby state (a coarse input to ANA, Section 9.2).

### FreeBSD today (`/Users/nwilkens/workspace/freebsd-src`)
- **`sys/dev/nvmf/`** is a complete in-kernel NVMe-oF host + controller, native C, **no LinuxKPI**, BSD-2-Clause.
  - Core: `nvmf_transport.c` (~450 LOC), `nvmf_transport_internal.h`, `nvmf_proto.h` (~765 LOC).
  - Host: `host/` (~3.5K LOC). Controller: `controller/` (~3.3K LOC) + `ctl_frontend_nvmf.c` (~680 LOC).
  - TCP transport: `nvmf_tcp.c` (~1.9K LOC). The only transport implemented; RDMA absent.
  - Userland: `lib/libnvmf`, `sbin/nvmecontrol`, `tools/tools/nvmf/nvmfd`.
- **Primitive audit (verified):** core block data path complete and solid (Connect, Identify, R/W, Flush, Write-Zeroes, Dataset-Management/TRIM, keep-alive, basic AER). **ANA/multipath: absent. Reservations: not implemented. Discovery: userland. Namespace management: static. In-band auth: absent.** FreeBSD's host has **no** multipath (one association per controller, no namespace aggregation), so ANA is greenfield relative to the port (Section 9).

---

## 4. Strategic decisions and rationale

### D1. NVMe-oF semantics, not iSCSI or a custom protocol
Block-native, RDMA-ready, ecosystem-interoperable. iSCSI carries SCSI overhead and is TCP-only here; a custom protocol forfeits interop and spec maturity.

### D2. Port FreeBSD, do not write from scratch
~9K LOC of spec-correct, BSD-2 C, no LinuxKPI. Port the core and TCP transport; rewrite only the OS-binding layers that must differ.

### D3. TCP first, RDMA later, behind the existing transport vtable
`nvmf_transport_ops` isolates the transport behind ~11 function pointers; two of them are exactly the RDMA-offload seam. Ship and validate over TCP, then add RDMA as one module.

### D4. Native RDMA, not a LinuxKPI port (when we get there)
For the single narrow RC-mode transport NVMe-oF/RDMA needs, a native `mlxcx` provider using `mlx5_ib` as a hardware reference is far cheaper than porting LinuxKPI. The LinuxKPI question is a separate platform bet, out of scope here.

### D5. Re-bind, don't port, the OS glue
- FreeBSD CTL → **do not port**; use STMF. Port only the translation logic in `ctl_frontend_nvmf.c`.
- FreeBSD CAM → **do not port**; use `blkdev`. Replace `nvmf_sim.c` wholesale.

### D6. ANA/multipath: structure in v1, mechanism by Phase 3.5, policy deferred
ANA changes the host *object model* (a disk becomes a multipath head, not a per-association device). Retrofitting that is a rewrite, so the object model and its seams are a hard v1 requirement (Phases 2-3). The full ANA *mechanism* (log page, AEN, path selector, control ioctl) lands in Phase 3.5 and is testable in isolation by manually toggling state. The failover *policy* (which namespaces group together, who decides to flip a group, when) belongs to the distribution control plane (Section 13). NVMe ANA is modeled natively in the NVMe layer, not forced through STMF's coarser SCSI ALUA (Section 9.2).

---

## 5. Target architecture

### 5.1 Layering

```
  INITIATOR (host) node                      TARGET (controller) node
  ---------------------                      ------------------------
  ZFS / consumer                             zvol  (ZFS volume)
      |                                          |
  blkdev  (/dev/dsk/...)   one disk           stmf_sbd  (SCSI block LU)
      |                    per ns "head"          |
  nvmf host: ns head + N paths + ANA          STMF  (scsi_task_t engine, ALUA state)
      |   path selector                          |   lport_xfer_data / dbuf
  nvmf TCP transport(s) (ksocket)             nvmf controller = STMF port provider
       \__ path A ... path B __/                  |  ANA group state + log page + AEN
                                             nvmf TCP transport (ksocket)
```

The **transport-independent core** is the ported FreeBSD code. The **transport** plugs in beneath it. The **OS binding** plugs in above it (blkdev head on the host, STMF on the target). Crucially, on the host a single namespace "head" aggregates multiple transport associations (paths); the head, not the association, owns the `bd_handle` (Section 9.3).

### 5.2 Userland / kernel split

FreeBSD splits each side into a userland control component (discovery, Fabrics Connect handshake, and for multipath, establishing each path) and a kernel data-path component, handing established associations to the kernel via ioctl. Recommendation: replicate this split. The control plane (placement, membership, failover decisions, connect/discovery, **which paths to establish**) lives in userland; only the block data path is in-kernel.

**Open design fork (R1):** transferring an established userland TCP socket into a kernel `ksocket`, vs in-kernel connection setup (as `iscsit`/`idm` do). This also gates multipath: the handoff interface must support attaching an **additional** path to an existing namespace head (Section 9.3). Resolve early (Section 12, R1).

---

## 6. Component port map

Disposition: **PORT** = copy + mechanical KPI translation; **REWRITE** = reimplement against illumos APIs using FreeBSD as reference; **NEW** = illumos-specific.

| FreeBSD source | Disposition | illumos target | Notes |
|---|---|---|---|
| `nvmf_proto.h` | PORT | `sys/nvmf/nvmf_proto.h` | Wire structs/constants. Endianness/packing review. |
| `nvmf_transport.c` + `_internal.h` | PORT | `uts/common/io/nvmf/nvmf_transport.c` | Transport dispatch + vtable. |
| `host/` core (`nvmf.c`, `nvmf_qpair.c`, `nvmf_ns.c`, `nvmf_aer.c`, `nvmf_cmd.c`) | PORT | `uts/common/io/nvmf_host/` | Capsule/queue/AER/keep-alive. |
| `host/nvmf_sim.c` (CAM SIM) | REWRITE | `uts/common/io/nvmf_host/nvmf_blkdev.c` + `nvmf_mpath.c` | Replace CAM with `blkdev` head + native multipath. See 7.3, 9.3. |
| `controller/` (`nvmft_*.c`) | PORT | `uts/common/io/comstar/port/nvmft/` | Controller state machine, command dispatch. |
| `ctl_frontend_nvmf.c` | REWRITE | `.../nvmft/nvmft_stmf.c` | NVMe->SCSI translation; bind to STMF. See 7.2. |
| `nvmf_tcp.c` | PORT+REWRITE | `uts/common/io/nvmf/nvmf_tcp.c` | PDU logic ports; socket I/O on `ksocket`; mbuf->mblk. See 7.1. |
| (none) RDMA transport | NEW (Phase 4) | `uts/common/io/nvmf/nvmf_rdma.c` | Native RoCE v2 over `mlxcx`. Deferred. |
| (none) host multipath/ANA | NEW (Phase 3.5) | `.../nvmf_host/nvmf_mpath.c` | Head/path/selector. Greenfield. See 9.3. |
| (none) target ANA | NEW (Phase 3.5) | `.../nvmft/nvmft_ana.c` | ANA groups, log page, AEN, control ioctl. See 9.4. |
| (none) reservations bridge | NEW (Phase 2) | `.../nvmft/nvmft_resv.c` | NVMe reservations -> `sbd` PGR. See 7.4. |
| `lib/libnvmf` | PORT | `usr/src/lib/libnvmf` | Userland Connect/discovery/handoff (incl. multipath). |
| `sbin/nvmecontrol` | PORT | `usr/src/cmd/nvmfadm` | Userland host control. |
| `tools/tools/nvmf/nvmfd` | PORT | `usr/src/cmd/nvmfd` | Userland target daemon. |
| (none) build glue | NEW | Makefiles, `Makefile.files`, `name_to_major`, `driver_aliases`, mapfiles | See 7.5. |

---

## 7. Key API mappings

### 7.1 Transport vtable -> ksocket (TCP)

`struct nvmf_transport_ops` (the seam): `allocate_qpair`/`free_qpair`, `allocate_capsule`/`free_capsule`, `transmit_capsule`, `validate_command_capsule`, `capsule_data_len`, and the two offload-seam functions `receive_controller_data` and `send_controller_data`.

| nvmf_tcp need | illumos |
|---|---|
| Listen/accept | `ksocket_socket`, `ksocket_bind`, `ksocket_listen`, `ksocket_accept` |
| Connect (if in-kernel) | `ksocket_connect` |
| Event-driven receive | `ksocket_krecv_set(ks, cb, arg)` — delivers `mblk_t` chains to the callback, no socket-buffer queueing (the `idm_so.c` path) |
| Transmit PDU | `ksocket_sendmblk(ks, msg, flags, &mblkpp, CRED())` |
| Buffer | `mblk_t` (from `struct mbuf`) |
| Options | `ksocket_setsockopt` (TCP_NODELAY, SO_SNDBUF) |

Principal mechanical task: `mbuf` -> `mblk_t` across the PDU code (`allocb`, chaining, refcount/`dupmsg`/`copymsg` semantics). Reference: `usr/src/uts/common/io/idm/idm_so.c`. For TCP, `send_controller_data`/`receive_controller_data` move C2H/H2C data PDUs; **in Phase 4 these two are replaced by RDMA WRITE/READ against registered memory.**

### 7.2 Controller -> STMF port provider (target data path)

Register via `stmf_register_port_provider()`, `stmf_register_local_port()` (implement `lport_xfer_data`, `lport_send_status`, `lport_task_free`, `lport_abort`, `lport_ctl`, `lport_info`, `lport_event_handler`), `stmf_register_scsi_session()` per connection. Model on `srpt`.

Per-I/O (READ): krecv parses an NVMe command capsule -> `stmf_task_alloc()` + translate NVMe command to a SCSI CDB + set `TF_READ_DATA`/`task_expected_xfer_length` -> `stmf_post_task()` -> LU allocates dbuf and calls `stmf_xfer_data()` -> our `lport_xfer_data(task, dbuf, ioflags)` transmits `db_sglist` as C2H data via `send_controller_data` then `stmf_data_xfer_done()` -> LU `stmf_send_scsi_status()` -> our `lport_send_status()` sends the completion capsule then `stmf_send_status_done()`. WRITE is symmetric (initial burst in `stmf_post_task(task, dbuf)` with `DB_DIRECTION_FROM_RPORT`).

Key structs: `scsi_task_t` (`task_flags`, `task_expected_xfer_length`, `task_cdb`, `task_lu`), `stmf_data_buf_t` (`db_sglist`, `db_data_size`, `db_relative_offset`, `db_flags`), `stmf_dbuf_store_t`.

**ANA responsibility (added):** the controller also advertises multipath capability (Identify Controller CMIC + ANACAP), assigns each namespace an ANA Group ID (ANAGRPID) in Identify Namespace, serves the ANA log page, and emits an ANA-change AEN when a group's state changes. State originates from the control plane via a new ioctl (Section 9.4); STMF's `ilu_access` active/standby can feed in as a coarse input but is not the model of record.

**Why this is faithful:** FreeBSD's `ctl_frontend_nvmf.c` already translates NVMe commands into CTL's SCSI-shaped `ctl_io`; we port that translation and retarget CTL -> STMF. NVMe Flush/Write-Zeroes/Dataset-Management/Compare map to SCSI equivalents STMF/`sbd` already execute.

### 7.3 Host -> blkdev head (initiator device presentation)

The host presents each **unique namespace** (a "head," identified by NGUID/EUI64/UUID from Identify Namespace) as one `blkdev` disk. No SCSI translation; blkdev is block-native. Critically, the head is decoupled from any single association: it owns a set of paths (Section 9.3), even when v1 populates exactly one.

- Attach: `bd_alloc_handle(head, &nvmf_bd_ops, &dma_attr, KM_SLEEP)` then `bd_attach_handle(dip, h)`. blkdev creates `/dev/dsk` nodes and handles labeling via `cmlb`.
- `bd_ops_t`: `o_drive_info` (queue count/size, EUI64/GUID/serial from Identify), `o_media_info` (`m_nblks`, `m_blksize`), `o_read`, `o_write`, `o_sync_cache` (Flush), `o_free_space` (Dataset-Management/deallocate), `o_devid_init` (devid from the namespace UUID — must be path-independent so failover does not change device identity).
- `o_read`/`o_write` receive a `bd_xfer_t` (`x_blkno`, `x_nblks`, DMA cookies `x_dmac`/`x_ndmac` or `x_kaddr`). The multipath layer **selects a path**, builds an NVMe command, submits via that path's transport, and on completion calls `bd_xfer_done(xfer, error)`; on path error it may **retry on another path** before completing (Section 9.3).

Reference blkdev idioms: `nvme` (`nvme_bd_read`/`nvme_bd_cmd`/`nvme_bd_xfer_done`), `vioblk` (cookie walk).

TCP vs RDMA buffer note: over TCP, data is copied between transport mblks and the blkdev DMA buffer; over RDMA (Phase 4) the buffer is registered and the NIC DMAs directly. Same offload seam as 7.1.

### 7.4 Reservations -> sbd PGR

The target path runs NVMe -> SCSI -> `sbd`, so NVMe Reservation Register/Acquire/Release/Report translate to SCSI PERSISTENT RESERVE OUT/IN flows enforced by `sbd_pgr` (`io/comstar/lu/stmf_sbd/sbd_pgr.c`: `sbd_pgr_t`/`sbd_pgr_key_t`, `sbd_pgr_out_register/reserve/clear/preempt/register_and_move`, `sbd_pgr_reservation_conflict()`, APTPL via `sbd_pgr_meta_load/write`). This turns target-enforced fencing from "build from scratch" into "bounded translation over tested code." Caveat: NVMe and SCSI-3 reservation models are close but not identical; build a verified mapping and conformance tests. This is the fencing primitive ANA-based failover depends on (Section 9.5).

**Scope and coupling.** Two limits bound this cheap path. First, it is cheap only for an **sbd-backed LU**: `sbd_pgr` state hangs off `sbd_lu_t` and persists through sbd metadata, and sbd does its I/O via vnode reads/writes on a backing zvol/file. A consumer whose LU is not sbd-over-zvol (e.g. a distributed engine presenting a custom or userland-backed LU) does **not** inherit PGR for free; it must either factor `sbd_pgr` into a reusable reservation module with pluggable persistence, or implement native NVMe reservation handling in `nvmft`. Second, PGR enforces fencing **only on the STMF/sbd target I/O path**; it cannot fence a back-end writer that bypasses this target (e.g. a partitioned coordinator still holding valid back-end write credentials). PGR is the protocol-visible compute-side fence; the distribution layer must supply its own authoritative fence (Section 9.5).

### 7.5 Build/integration glue (NEW)

Per kernel module (`nvmf`, `nvmf_host`, `nvmft`): source under `usr/src/uts/common/io/...`; per-arch `Makefile` under `usr/src/uts/intel/<module>/` (model on `intel/nvme`, `intel/vioblk`); objects in `usr/src/uts/common/Makefile.files`; `LDFLAGS += -N drv/blkdev` (host) / `-N drv/stmf` (target); pseudo-device entries in `os/name_to_major` and `driver_aliases`; export mapfiles. Userland: `usr/src/lib/libnvmf` + `usr/src/cmd/{nvmfd,nvmfadm}`, wired into the lib/cmd Makefiles and SmartOS/Triton packaging manifests.

---

## 8. Data buffer, DMA, and zero-copy strategy

- **Target:** STMF dbufs own DMA-able buffers. For TCP, the store can be plain kernel memory copied to/from socket mblks. Design the store so that in Phase 4 the same buffers are RDMA-registered and the NIC DMAs directly — keep buffer ownership/lifetime registration-compatible.
- **Host:** blkdev hands us DMA-bound buffers (`x_dmah`/cookies). TCP copies; RDMA registers and places. Choose `dma_attr` at `bd_alloc_handle` time compatible with both transport and eventual RDMA registration alignment.
- **Registration caching (Phase 4):** per-I/O register/dereg destroys latency; design now for a registration cache / pre-registered pools.

Forward-looking constraint: **buffer lifetime/ownership must be RDMA-registration-friendly from v1**, even though v1 only copies over TCP (gate R4).

---

## 9. ANA, multipath, and failover (designed in from v1)

This is the one capability whose absence in v1 would force a later rewrite, because it changes the host object model. It is greenfield on **both** sides relative to the FreeBSD port. Strategy: build the object model and seams now (Phases 2-3), the mechanism by Phase 3.5, and leave only policy to the distribution layer.

### 9.1 What ANA actually requires (NVMe mechanics)
- **ANA groups:** every namespace has an ANA Group ID (ANAGRPID, in Identify Namespace). A group is the unit that transitions together.
- **Per-path state:** for each controller (path), each group has a state: Optimized, Non-Optimized, Inaccessible, Persistent-Loss, or Change (transitioning).
- **ANA log page (0Ch):** the controller reports group->state and the NSIDs in each group; the host reads it to learn the topology.
- **AEN:** the controller emits a "Notice: Asymmetric Namespace Access Change" async event; the host re-reads the log page.
- **Identity + capability:** the host recognizes the *same* namespace across controllers by NGUID/EUI64/UUID; the controller advertises CMIC (multipath-capable) and ANACAP in Identify Controller.

### 9.2 What illumos gives us, and the mismatch
**Target (STMF ALUA) is too coarse to be the model of record:**
- `STMF_IOCTL_SET_ALUA_STATE` / `stmf_set_alua_state()` enable a global, two-node active/standby ALUA.
- LU access state is binary: `STMF_LU_ACTIVE`(0) / `STMF_LU_STANDBY`(1) on `stmf_i_lu_t.ilu_access`; `stmf_prepare_tpgs_data()` emits exactly two fixed target port groups keyed off `ilport_standby`.
- `stmf_set_lu_access()` deliberately does **not** notify initiators: the source comment says the caller is responsible for the SCSI unit attention (`STMF_SAA_ASYMMETRIC_ACCESS_CHANGED`, 0x062A06). There is a port event hook (`lport_event_handler` / `stmf_generate_lport_event`), but STMF does not fire access-state changes through it.
- Control surface exists and is a good parallel: `stmfSetAluaState()`, `stmfModifyLu(... STMF_LU_PROP_ACCESS_STATE ...)`, the access strings include transition states, and inter-cluster `STMF_ICM_LUN_ACTIVE` coordinates two-node moves.

**Decision:** model ANA groups and states **natively inside the nvmft port provider** (`nvmft_ana.c`), driven by its own control ioctl that parallels `stmfSetAluaState`/`stmfModifyLu`. Optionally consume STMF's binary active/standby as a coarse input (standby LU -> that controller reports Inaccessible/Non-Optimized for those namespaces), but keep ANA semantics in NVMe terms. Do not round-trip NVMe ANA through SCSI ALUA; the state models do not match (5 states + N groups vs 2 states + 2 TPGs).

**Host:** confirmed nothing reusable. `scsi_vhci` is bound to SCSA/`scsi_pkt` (SCSI-only); `blkdev` is single-path with no path abstraction. Native NVMe multipath is required, modeled on Linux `nvme-core` (subsystem -> namespace head -> sibling paths, ANA-aware).

### 9.3 Host object model (the expensive-to-retrofit structure)
Build this from v1 even though only one path is populated initially:
- **Namespace head:** keyed by namespace identity (NGUID/EUI64/UUID). Owns the single `bd_handle`. Device identity (`o_devid_init`, label) is derived from the head, **path-independent**, so failover does not change the disk.
- **Path:** a tuple (controller association, that controller's NSID for this head, current ANA state). A head owns a set of paths.
- **Path selector:** chooses a path per `bd_xfer_t` (v1: the one path; later: prefer Optimized, then Non-Optimized; skip Inaccessible). I/O error or path-down triggers retry on an alternate path before `bd_xfer_done`.
- **Keep-alive / disconnect routes to the path layer, not to device teardown.** A dead association marks its paths down and reroutes; the `bd_handle` survives as long as any path remains. This is a structural seam: in a naive design keep-alive death tears down the disk; here it must not.
- **Handoff of additional paths:** userland (discovery) may establish multiple associations to the same subsystem on different target addresses; the kernel aggregates them under one head by identity. The R1 handoff interface must therefore support "attach this association as a new path to existing head," or the kernel matches by NGUID post-handoff.

### 9.4 Target object model
- Namespace -> ANAGRPID mapping (configurable; default each namespace in its own group or grouped per the distribution layer's failover domains).
- Per-group ANA state, settable by the control plane via a new nvmft ioctl (parallel to `stmfSetAluaState`/`stmfModifyLu`).
- ANA log page builder and Identify CMIC/ANACAP bits.
- AEN emission on any group state change.
- v1 may hardcode every group to Optimized, but **all the plumbing (log page, AEN, Identify bits, control ioctl) ships in v1** so hosts (ours and Linux) have a real ANA surface to consume and the wire format is correct.

### 9.5 The failover unit and the distribution seam
The **ANA group is the failover unit.** How namespaces map to groups, and who flips a group's state and when, are distribution-layer decisions. The kernel provides the *mechanism* (namespaces carry ANAGRPIDs; groups have settable states that propagate via log page + AEN; the host reroutes accordingly); the distribution control plane provides the *policy*. Fencing (Section 7.4 reservations) is the protocol-visible backstop: when a group flips and a host reroutes, target-enforced reservations stop a partitioned old-primary from writing *through the target*. This is necessary but not sufficient for a distributed store: PGR guards only the STMF/sbd I/O path, so the distribution layer must enforce its own authoritative fence on back-end writers (an epoch / write-capability that chunk/replica servers check on every write), with that epoch as the source of truth and PGR as its protocol-visible projection. Old coordinators must be rejected by the back end, not merely by the front-end target.

This is also where the architecture choice (Section 13) lands:
- **Smart target / dumb client:** the target cluster flips ANA states; **any** standard initiator (including stock Linux) fails over. Here the **target** ANA is the higher-priority half (it serves all clients).
- **Smart client:** the client may drive its own routing and lean less on ANA; the **host** ANA mainly makes our initiator a good multipath citizen against third-party targets.

### 9.6 Build-now vs design-now (graded)
- **Must structure in v1 (Phases 2-3), retrofit is a rewrite:** host head/path decoupling + identity matching + path-independent device identity + keep-alive-to-path seam + additional-path handoff (R1); target ANAGRPID attribute + control-ioctl shape + log-page/AEN plumbing.
- **Build by Phase 3.5 (testable in isolation):** the path selector's ANA-state logic, AEN-driven re-read, error-triggered failover, and the target's settable group states. Test by manually toggling group state via ioctl and observing host reroute, with no distribution layer present.
- **Defer to the distribution layer:** group design (failover domains) and the policy that decides to flip a group and when.

Recommendation: build the **full ANA mechanism** by Phase 3.5 (it is independently testable), and defer only policy. By end of Phase 3.5, ANA is a complete kernel capability the distribution layer later just drives.

---

## 10. Primitive coverage: inherit vs build

| Primitive | FreeBSD status | illumos plan |
|---|---|---|
| Core block I/O (R/W/Flush/Write-Zeroes/DSM-TRIM) | Implemented | **Inherit** (port). DSM/deallocate -> `o_free_space`/UNMAP. |
| Connect / Property / Identify | Implemented | **Inherit** (port); extend Identify with CMIC/ANACAP (9.4). |
| Keep-alive | Implemented | **Inherit**; route death to the path layer (9.3). |
| AER | Implemented (basic) | **Inherit**; add ANA-change handling (9.3/9.4). |
| Discovery | Userland | **Port**; userland establishes multiple paths (9.3). |
| Reconnect | Userland-driven | **Port** or fold into in-kernel setup (R1). |
| **Reservations (fencing)** | Not implemented | **Build** via `sbd` PGR bridge (7.4). Backstops ANA failover. |
| Fused Compare-and-Write | Partial | Validate against `sbd` COMPARE AND WRITE. |
| **ANA / multipath / failover** | Absent both ends | **Build** (Section 9). Structure in v1, mechanism by Phase 3.5. |
| Namespace management (on-wire) | Not implemented | **Out of band** via the control plane (preferred). |
| In-band auth (DH-HMAC-CHAP) | Absent | **Defer**; rely on TLS / isolation for v1. |

---

## 11. Phasing and milestones

### Phase 0 — Foundation (spike), ~1-2 mo
Port `nvmf_proto.h` + transport core; compile/load an empty module; establish KPI translation conventions (mbuf->mblk, malloc->kmem, mtx->mutex, taskqueue->taskq, callout->cv/timeout). Spike R1 (socket handoff vs in-kernel setup) since it gates multipath.
**Deliverable:** core compiles/loads; vtable registration exercised with a stub.

### Phase 1 — TCP transport, ~1-2 mo
Port `nvmf_tcp.c` PDU logic onto `ksocket`; validate capsule exchange via loopback/`INTRA_HOST`.
**Deliverable:** two kernel endpoints exchange Connect + an I/O capsule over TCP.

### Phase 2 — Target (STMF port provider), ~2-4 mo
Port the controller; rewrite the CTL binding onto STMF (7.2); wire to `stmf_sbd`/zvol; userland `nvmfd`; reservations bridge (7.4). **ANA-structural requirement:** namespaces carry ANAGRPIDs and Identify advertises CMIC/ANACAP (states may be hardcoded Optimized here).
**Deliverable:** export a zvol over TCP; a **Linux `nvme-cli` initiator** connects, discovers, reads/writes, and exercises reservations.

### Phase 3 — Host (blkdev), ~2-4 mo
Port the host core; replace `nvmf_sim` with the blkdev **head/path** binding (7.3, 9.3) — head decoupled from association, identity matching, keep-alive-to-path seam — even though one path is populated. Userland connect/discovery (`libnvmf` + `nvmfadm`).
**Deliverable:** an illumos host attaches a remote namespace as one `/dev/dsk/...`; full **bidirectional Linux interop**; latency/throughput baseline captured.

### Phase 3.5 — ANA activation + multipath, ~2-3 mo
Target: settable per-group ANA states via control ioctl, ANA log page, ANA-change AEN. Host: multi-path attach (userland establishes >1 association; kernel aggregates by identity), ANA-aware path selector, AEN-driven re-read, error/path-down failover.
**Deliverable:** manually flip a group's ANA state (ioctl) and observe the illumos host reroute I/O with no data loss; verified against a **Linux multipath initiator** too.

> End of Phase 3.5: a working, interoperable, in-kernel NVMe-oF/TCP stack with ANA failover. The distribution layer now has both a data path and a failover hook.

### Phase 4 — RDMA (RoCE v2) offload [deferred]
Native RC-mode RoCE v2 transport in `mlxcx`, registered as `NVMF_TRTYPE_RDMA` behind the same vtable; replace the two data-movement functions with RDMA WRITE/READ against registered buffers; add a registration cache. Effort: see prior RDMA analysis (order +6-15 months). Gated by D4.

### Phase 5 — Distribution layer [separate program]
Out of scope (Section 13).

---

## 12. Risks and open questions

- **R1 — Socket handoff / multipath attach (5.2, 9.3):** userland->kernel socket transfer vs in-kernel setup, and supporting attach-additional-path. Shapes the userland/kernel boundary *and* multipath. Spike in Phase 0/1.
- **R2 — mbuf vs mblk semantics:** refcounting, `dupmsg`/`copymsg`, partial-PDU coalescing. Lift idioms from `idm_so.c`. Data-corruption risk if mishandled.
- **R3 — NVMe<->SCSI translation fidelity:** reservations (model differences), Compare-and-Write fused semantics, DSM/deallocate -> UNMAP, status mapping. Conformance-test against Linux.
- **R4 — Buffer ownership for future RDMA (8):** design-review gate before Phase 2 freezes the dbuf store, or Phase 4 becomes a rewrite.
- **R5 — blkdev DMA windowing:** handle `DDI_DMA_PARTIAL_MAP` multi-window completion (follow `vioblk`/`nvme`).
- **R6 — Licensing hygiene:** BSD-2 files carry their headers into the CDDL tree (illumos already hosts BSD drivers); document provenance per file; import no GPL-only `ibcore` code.
- **R7 — illumos lacks `xarray`/`idr`/`kref`:** map to `id_space_t`, `avl`, `mod_hash`, `refcount`/`kmem` during the port.
- **R8 — ANA state model mismatch (9.2):** STMF binary ALUA cannot be the model of record; nvmft owns ANA state natively. Decide whether/how to consume STMF active/standby as a coarse input, and reconcile with existing COMSTAR ALUA so the two do not fight.
- **R9 — Namespace identity & device stability (9.3):** the head and its `devid`/label must derive from namespace identity (NGUID/EUI64/UUID), path-independent, so failover and reconnect never change the disk seen by ZFS. Get Identify-Namespace identity handling right early.
- **R10 — ANA group design is a distribution concern (9.5):** the kernel must not bake in a grouping policy; expose ANAGRPID assignment to the control plane.
- **R11 — Reservation reuse coupling and scope (7.4, 9.5):** `sbd` PGR is bound to `sbd_lu_t` and persisted in sbd metadata, so a non-sbd / userland-engine LU does not inherit it for free (factor `sbd_pgr` with pluggable persistence, or implement native `nvmft` reservations). And PGR fences only the target I/O path, not back-end writers; the distribution layer owns the authoritative epoch/capability fence. Decide the reservation strategy alongside the consumer's LU-backing choice.

---

## 13. The distributed layer (out of scope, sketched)

NVMe-oF gives a correct **per-link block data path**; ANA gives a **failover hook**. Replication, sharding/placement, membership/consensus, *failover policy*, consistency, scrub/rebuild are a separate system above it. Two architectures determine where intelligence lives and how it uses this stack:

- **Smart target / dumb client:** stock NVMe-oF initiators (any Linux/illumos/VMware host); all distribution logic in the target cluster, which flips **ANA** states to steer clients during failover and relies on **reservations** to fence. The illumos **target** is the centerpiece.
- **Smart client / dumb targets:** trivial block-server targets (stock nvmf + STMF over zvol); a bespoke client knows the cluster map and routes/replicates. The illumos **host** is a thin multipath transport.

Either way, this document's deliverable (Phases 0-3.5) is the substrate plus the failover mechanism. What remains entirely the distribution layer's: membership/consensus, the replication protocol and quorum/ack policy, the placement function and its metadata, cross-node consistency/ordering, ANA **group design** and the **policy** that flips group states, and the control plane that programs target exports, reservations, and ANA states.

---

## 14. Appendix: reference inventory

### FreeBSD (`/Users/nwilkens/workspace/freebsd-src`)
- `sys/dev/nvmf/{nvmf_transport.c,nvmf_transport_internal.h,nvmf_proto.h}` — core + vtable.
- `sys/dev/nvmf/host/` — initiator (`nvmf.c`, `nvmf_qpair.c`, `nvmf_ns.c`, `nvmf_aer.c`, `nvmf_cmd.c`, `nvmf_sim.c`). No multipath (ANA is greenfield).
- `sys/dev/nvmf/controller/` + `ctl_frontend_nvmf.c` — target core + CTL binding (translation reference).
- `sys/dev/nvmf/nvmf_tcp.c` — TCP transport reference.
- `lib/libnvmf`, `sbin/nvmecontrol`, `tools/tools/nvmf/nvmfd` — userland. BSD-2-Clause; no LinuxKPI.

### illumos (`/Users/nwilkens/workspace/triton/illumos-joyent`)
- `usr/src/uts/common/sys/blkdev.h`, `io/blkdev/blkdev.c` — host device framework (single-path). Refs: `io/nvme/`, `io/vioblk/vioblk.c`.
- `usr/src/uts/common/sys/stmf.h`, `sys/portif.h`, `sys/lpif.h`, `sys/stmf_ioctl.h`; `io/comstar/stmf/stmf.c`, `stmf_impl.h` — target framework + ALUA: `stmf_set_alua_state()`, `STMF_IOCTL_SET_ALUA_STATE`, `stmf_set_lu_access()`, `ilu_access`/`ilu_alua`, `stmf_prepare_tpgs_data()`, `stmf_generate_lport_event()`/`lport_event_handler`, `STMF_SAA_ASYMMETRIC_ACCESS_CHANGED`.
- `usr/src/uts/common/io/comstar/lu/stmf_sbd/{sbd_pgr.c,sbd_impl.h}` — PGR (reservations bridge).
- `usr/src/uts/common/io/comstar/port/pppt/`, `sys/pppt_ic_if.h` — proxy port provider + inter-cluster messaging (`STMF_ICM_LUN_ACTIVE`).
- `usr/src/lib/libstmf/`, `usr/src/cmd/stmfadm/` — control surface (`stmfSetAluaState`, `stmfModifyLu`) to parallel for ANA.
- `usr/src/uts/common/sys/ksocket.h`; `io/idm/idm_so.c` — kernel TCP + reference usage.
- `usr/src/uts/common/io/comstar/port/srpt/` — closest port-provider analog.
- `usr/src/uts/common/io/scsi/adapters/scsi_vhci/` — SCSI-only multipath (confirmed not reusable for NVMe).
- Build: `usr/src/uts/common/Makefile.files`, `usr/src/uts/intel/<module>/Makefile`, `os/name_to_major`, `os/driver_aliases`.

### Related internal analysis
- RDMA / RoCE v2 feasibility and the native-`mlxcx`-vs-LinuxKPI decision: prior analysis. Phase 4 depends on it.
