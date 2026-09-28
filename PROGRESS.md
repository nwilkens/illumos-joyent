# NVMe-oF Port: Progress

Tracks the port of FreeBSD's in-kernel NVMe-over-Fabrics stack to illumos-joyent,
per the plan in `NVMEOF.md`. Divergences from FreeBSD are recorded in `DIVERGE.md`.

Last updated: 2026-06-14.

## Headline status

All four new kernel modules **compile and link cleanly** under the strict
illumos build (`-errwarn=%all` + `smatch --fatal-checks`, gcc10), **load**, and
**attach** on a live SmartOS kernel. 82 of the original 133 data-path
`PORT-TODO`s are implemented; 51 remain (see "Remaining work").

| Module | Type | Artifact | Builds | modload | attach |
|---|---|---|---|---|---|
| `nvmf` | misc | 14 KB | YES | **YES** | n/a |
| `nvmf_tcp` | misc | 34 KB | YES | **YES** (registers `tcp_ops`) | n/a |
| `nvmf_host` | drv (blkdev) | 71 KB | YES | **YES** | **YES** (control minor) |
| `nvmft` | drv (STMF port) | 62 KB | YES | **YES** | **YES** (STMF provider registered) |

Total ported: ~11.3K LOC kernel + 3 public headers under `sys/nvme/`.

**What this does and does not mean.** The stack compiles, loads, attaches, and
the target registers an STMF port provider, on real hardware. It does **not**
yet serve I/O: the userland control plane (`nvmfd`/`nvmfadm`/`libnvmf`) does not
exist, so no subsystem can be configured and no Connect/handoff can happen; the
TCP transport's fd->ksocket handoff (`tq_so`) and the reservation session wiring
are among the 51 remaining `PORT-TODO`s. So: kernel side load+attach complete;
wire-level I/O still needs userland + iterative debugging.

## Runtime modload test (2026-06-14)

Loaded on a live global zone (`10.199.199.40`, SmartOS `joyent_20260505`;
modules built from `joyent_20260520`, a 15-day skew). Modules staged in
`/kernel/{misc,drv}/amd64` (the writable `/` ramdisk; `/usr/kernel` is
read-only) and loaded with `modload -p`.

```
164  ksocket   (kernel socket module)        <- auto-loaded dependency
263  nvmf      (NVMe over Fabrics transport)  <- core, loaded
264  nvmf_tcp  (NVMe over Fabrics TCP transport) <- loaded; registered tcp_ops
```

- **`nvmf` (core) and `nvmf_tcp` (transport) load and run.** `nvmf_tcp`'s
  `_init` executed `nvmf_transport_register(&tcp_ops)` and registered the TCP
  transport into the core's table. The ported FreeBSD code runs on real
  hardware; no panics (box up 27 days, healthy).
- **Bug found and fixed at load time:** `nvmf_tcp` failed `modload` with
  `EINVAL` due to two undefined symbols, `ksocket_sendmblk` and
  `ksocket_krecv_set`, which live in `misc/ksocket` (not `genunix`). The
  Makefile was missing `-N misc/ksocket`. Added it (matching `idm`/`iscsit`/
  `rdsv3` precedent), rebuilt, and the module loaded. This is exactly the class
  of bug only runtime linking surfaces.
- **After the data-path implementation pass (82 PORT-TODOs):** both drivers
  `add_drv` cleanly. `nvmf_host` attaches (control minor `/pseudo/nvmf_host@0`).
  `nvmft` attaches and `stmf_register_port_provider` succeeds (`ns_pp` non-NULL,
  verified via mdb) -- the target is registered with COMSTAR/STMF. No panics;
  box healthy. The only fix needed after the agent implementation pass was
  re-adding two `nvmft_var.h` fields (`np_controllers_cv`, `ctrlr_session`) lost
  to concurrent agent edits of the same header.
- Full wire I/O still needs the userland control plane (Connect / discovery /
  socket handoff); see "Remaining work".

**What "builds" means, and does not mean.** Compile + link success verifies the
type system, the illumos KPI bindings (STMF, blkdev, ksocket, DDI, nvlist), the
header layout, and that the ported control-flow is structurally sound. It does
**not** mean the modules serve I/O yet. After the implementation pass, **51 of
the original 133 `PORT-TODO`s remain** (82 implemented): the userland-facing
handoff paths, the TCP fd->ksocket wiring (`tq_so`), the reservation->PGR
session wiring, and parts of the ANA mechanism. See "Remaining work" below.

**Build vs runtime hosts.** Built on the SmartOS non-global build zone `.194`
(cannot `modload` there); loaded + attached on the global zone `.40`. Note
`/var/tmp` on `.40` is full, so use `/tmp` (32G swap) and stage modules in
`/kernel/{misc,drv}/amd64`. Cleanup: `rem_drv nvmft nvmf_host`, then
`modunload` the misc modules.

## Build environment and recipe

- Host: `root@142.147.4.194` (SmartOS build zone, 40 CPU / 64 GB).
- Tree: `/opt/smartos-live-vnext/projects/illumos` (TritonDataCenter illumos-joyent,
  already fully built; 520 MB proto, onbld toolchain).
- Env: `/opt/smartos-live-vnext/projects/illumos/illumos.sh`.
- Per-module build (non-DEBUG, matches the only `genunix` variant present):
  ```
  bldenv .../illumos.sh 'cd $SRC/uts/intel/<mod> && dmake def'
  # -> $SRC/uts/intel/<mod>/obj64/<mod>
  ```
- Source is edited on the canonical darwin checkout
  (`/Users/nwilkens/workspace/triton/illumos-joyent`) and tar-synced to `.194`;
  the two shared Makefiles were applied to `.194` via `git apply` so the Proteus
  branch's own edits are preserved.

## Files added

```
usr/src/uts/common/sys/nvme/nvmf.h            (fabrics wire defs, delta over sys/nvme.h)
usr/src/uts/common/sys/nvme/nvmf_transport.h  (transport consumer/provider API)
usr/src/uts/common/sys/nvme/nvmf_tcp.h        (NVMe/TCP PDU defs)
usr/src/uts/common/io/nvmf/                    nvmf_transport.c, nvmf_core.h,
                                               nvmf_transport_internal.h, nvmf_tcp.c
usr/src/uts/common/io/nvmf_host/               nvmf_host.c, nvmf_qpair.c, nvmf_ns.c,
                                               nvmf_aer.c, nvmf_cmd.c, nvmf_var.h,
                                               nvmf_blkdev.c (NEW), nvmf_mpath.c (NEW),
                                               nvmf_host.conf
usr/src/uts/common/io/comstar/port/nvmft/      nvmft.c, nvmft_controller.c, nvmft_qpair.c,
                                               nvmft_subr.c, nvmft_var.h, nvmft_stmf.c (NEW),
                                               nvmft_resv.c (NEW), nvmft_ana.c (NEW), nvmft.conf
usr/src/uts/intel/{nvmf,nvmf_tcp,nvmf_host,nvmft}/Makefile
```

Edited (additive): `usr/src/uts/common/Makefile.files`,
`usr/src/uts/common/Makefile.rules`.

## Build-fix log (workflow output -> clean compile)

The multi-agent port produced source that needed the following corrections to
pass the illumos build. All are recorded with rationale in `DIVERGE.md`.

1. Build glue: `NVMF_HOST_OBJS` listed FreeBSD filenames (`nvmf.o`,
   `nvmf_ctldev.o`) instead of the real objects; added `nvmf_blkdev.o`/
   `nvmf_mpath.o`. Added missing `intel/nvmf_tcp/Makefile` (orphaned
   `NVMF_TCP_OBJS`).
2. `cmn_err` format: illumos's kernel `cmn_err` has no `#` flag; `%#x` -> `0x%x`
   (6 files).
3. Doc-comment cascades: prose containing a literal `*/` (`/* PORT-TODO ... */`
   in `nvmf_host.c`; `TF_*/xfer` in `nvmft_stmf.c`) closed the block comment
   early and mis-parsed the rest of the file.
4. `<sys/time.h>` before `<sys/stmf.h>` (for `hrtime_t`) in all `nvmft` sources.
5. `<sys/stat.h>` for `S_IFCHR`; `<sys/sunddi.h>` for `bzero`/`memcpy`/`strlcpy`
   in `nvmft_qpair.c`/`nvmft_resv.c`.
6. `memset` -> `bzero` (illumos kernel has no `memset`).
7. Raw `nvme_sqe_t`/`nvme_cqe_t`/`nvme_sgl_t` are driver-private
   (`io/nvme/nvme_reg.h`); `nvmft` now cross-includes it (matching the host).
8. `nvmf_memdesc_t` made visible to the host via `nvmf_var.h`.
9. Field name `id_ctrlid` -> `id_cntlid`.
10. `valid_flags` initialized; `VERIFY3U` bound added to satisfy a smatch
    buffer-overflow false positive in `nvmft_controller.c`.
11. Created `nvmf_host.conf` / `nvmft.conf` driver config files.
12. (found at runtime modload) `nvmf_tcp` missing `-N misc/ksocket`:
    `ksocket_sendmblk`/`ksocket_krecv_set` are exported by the `misc/ksocket`
    module, not `genunix`. Added the dependency.

## Remaining work (to functional, by PORT-TODO concentration)

- `nvmf_tcp.c` (27): the socket RX/TX state machine and H2C/C2H data transfer
  over `ksocket` (`ksocket_krecv_set` / `ksocket_sendmblk`); digest handling.
- `nvmf_host.c` (25) + `nvmf_blkdev.c` (12) + `nvmf_mpath.c` (8): blkdev
  `o_read`/`o_write` to NVMe commands and completion; keep-alive/reconnect
  timers; the namespace-head/path object model and ANA-aware path selector
  (NVMEOF.md 9.3).
- `nvmft_stmf.c` (17): the NVMe-command -> `scsi_task_t` translation and
  `lport_xfer_data`/`lport_send_status` datamove against STMF dbufs.
- `nvmft_ana.c` (8) + `nvmft_resv.c` (7): ANA log page / AEN / control ioctl
  (NVMEOF.md 9.4); NVMe reservations -> `sbd_pgr` bridge (NVMEOF.md 7.4).
- Userland (not started): `libnvmf`, `nvmfadm`/`nvmecontrol`, `nvmfd` (discovery
  + association handoff). Required before any end-to-end test.

## Task status

- [x] Validate `.194` build environment (`bldenv`/`dmake def` -> `obj64`).
- [x] Generate ported source (multi-agent workflow, 17 agents).
- [x] Sync to `.194`; apply shared-Makefile deltas.
- [x] Build core `nvmf`.
- [x] Build `nvmf_tcp`, `nvmf_host`, `nvmft` (build-fix loop).
- [x] Artifacts produced; this doc + `DIVERGE.md` authored.
- [x] Runtime modload test (global zone): `nvmf` + `nvmf_tcp` load and register;
      fixed the `-N misc/ksocket` dependency found at load.
- [x] Implement data-path `PORT-TODO`s (82/133; workflow + build-fix). All four
      modules still build clean.
- [x] Driver `attach(9E)` + `add_drv`: both attach on `.40`; `nvmft` registers
      its STMF port provider.
- [ ] Remaining 51 `PORT-TODO`s: TCP fd->ksocket handoff, reservation->PGR
      session wiring, ANA mechanism completion.
- [ ] Userland control plane (`libnvmf`/`nvmfd`/`nvmfadm`) -- the gate to actual
      wire I/O.
- [ ] End-to-end I/O test (export a zvol, connect a Linux initiator).
