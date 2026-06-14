# NVMe-oF Port: Divergences from FreeBSD

Every place the illumos port departs from FreeBSD's `sys/dev/nvmf` source, with
the reason. Companion to `NVMEOF.md` (design) and `PROGRESS.md` (build status).
Source of truth for "why is this different from upstream FreeBSD".

Provenance: ported from FreeBSD `sys/dev/nvmf` (BSD-2-Clause, Chelsio / John
Baldwin). Ported files preserve the upstream copyright plus an illumos CDDL
header and a per-file provenance note.

## A. Architectural divergences (by design — see NVMEOF.md)

| # | FreeBSD | illumos | Why |
|---|---|---|---|
| A1 | Target frontend = CTL (`ctl_frontend_nvmf.c`, `CTL_FRONTEND_DECLARE`) | COMSTAR STMF port provider (`nvmft_stmf.c`, `stmf_register_port_provider`) | illumos has STMF, not CTL. NVMe->SCSI translation is preserved; the framework binding is re-targeted. Modeled on `srpt`. |
| A2 | Host presentation = CAM SIM (`nvmf_sim.c`) | `blkdev` binding (`nvmf_blkdev.c`) + native multipath (`nvmf_mpath.c`) | illumos has no CAM. One `blkdev` disk per *namespace head* (NGUID/EUI64/UUID), each head owning N paths (NVMEOF.md 9.3). `nvmf_sim.c` is dropped, not ported. |
| A3 | No ANA / multipath | Native ANA modeled in `nvmft_ana.c` + host head/path model | FreeBSD implements neither. illumos models ANA natively in the NVMe layer (STMF's binary ALUA is too coarse — NVMEOF.md 9.2). Structure is in v1; mechanism is Phase 3.5. |
| A4 | No reservations | NVMe reservations -> `sbd_pgr` bridge (`nvmft_resv.c`) | FreeBSD's target rejects reservations. illumos can bridge to `stmf_sbd`'s SCSI-3 PGR (NVMEOF.md 7.4). Currently a stub that returns a clean unsupported error. |
| A5 | `nvmf` + `nvmf_tcp` kernel modules | same split: `nvmf` (misc) + `nvmf_tcp` (misc) + `nvmf_host` (drv) + `nvmft` (drv) | Transport core stays a separate `misc` module so transports plug in; host and target are separate drivers. `nvmf_host`/`nvmft` link `-N misc/nvmf`. |
| A6 | TCP via `socket`/`mbuf` upcalls | TCP via `ksocket` (`ksocket_krecv_set`, `ksocket_sendmblk`), `mblk_t` | illumos kernel sockets. `idm_so.c` is the idiom reference. |
| A7 | Userland handoff in `libnvmf` + newbus | DDI `dev_ops`/`cb_ops`; handoff TBD (NVMEOF.md R1) | Replaces `device_t`/`make_dev`/`cdevsw`/`eventhandler` with the illumos DDI. |

## B. Type / header divergences

- **B1. Raw `nvme_sqe_t` / `nvme_cqe_t` / `nvme_sgl_t`.** These 64/16/16-byte raw
  entries are NOT in the public `<sys/nvme.h>`; they live only in the driver
  private `io/nvme/nvme_reg.h`. Both `nvmf_host` and `nvmft` reach them via a
  relative cross-include `#include "..//../nvme/nvme_reg.h"`.
  *Recommended cleanup:* promote these three types from `nvme_reg.h` to
  `<sys/nvme.h>` (where `nvme_cqe_sf_t` and the `NVME_CQE_SC*` enums already
  live) and drop the cross-includes. Until then, no file may include both
  `<sys/nvme/nvmf.h>`-consumers' headers and `nvme_reg.h` independently.
- **B2. Fabrics wire defs.** `<sys/nvme/nvmf.h>` is a *delta* over the existing
  `<sys/nvme.h>` (which FreeBSD lacks; FreeBSD has its own `nvme.h` +
  `nvmf_proto.h`). It defines only the fabrics-specific structures (capsules,
  fabrics command set, fabrics Identify/property, discovery log, fabrics SGL)
  and `#include`s `<sys/nvme.h>` for the base spec, reusing e.g.
  `nvme_identify_ctrl_t` instead of FreeBSD's `struct nvme_controller_data`.
- **B3. Field name.** FreeBSD/agent `id_ctrlid` -> illumos `id_cntlid`
  (`nvme_identify_ctrl_t`).
- **B4. `nvmf_memdesc_t`** replaces FreeBSD's `struct memdesc` (`sys/memdesc.h`,
  absent on illumos). Defined in `nvmf_transport_internal.h`; the scaffold only
  implements the `NVMF_MEMDESC_VADDR` (kernel virtual buffer) variant.

## C. Kernel-KPI divergences (FreeBSD -> illumos)

Applied throughout per the porting KPI map. The ones that bit the build:

- **C1. `printf`/`device_printf` -> `cmn_err`/`dev_err`**, and illumos `cmn_err`
  does **not** support the `#` alternate-form flag: `"%#x"` -> `"0x%x"`.
- **C2. `memset` -> `bzero`** (illumos kernel has no `memset`; `memcpy`/`memmove`
  do exist, declared in `<sys/sunddi.h>`/`<sys/systm.h>`). A `memset(_, ' ', _)`
  space-pad became an explicit loop.
- **C3. String/mem prototypes** require `<sys/sunddi.h>` (or `<sys/systm.h>`):
  `bzero`, `bcopy`, `memcpy`, `strlcpy`, `strlcat`.
- **C4. Include ordering:** `<sys/stmf.h>` uses `hrtime_t` but does not pull
  `<sys/time.h>`; every includer must include `<sys/time.h>` first (FreeBSD/srpt
  get it transitively via `<sys/cpuvar.h>`). `S_IFCHR` needs `<sys/stat.h>`.
- **C5. Other map entries (applied where present):** `malloc/M_WAITOK` ->
  `kmem_alloc/KM_SLEEP` (+ explicit size on free); `struct mtx` -> `kmutex_t`;
  `struct sx` -> `krwlock_t`; `cv_*` ~ compatible; `taskqueue` -> `ddi_taskq`;
  `callout` -> `timeout(9F)`/`cv_timedwait`; `refcount(9)` -> guarded `uint_t`/
  atomics; `mbuf` -> `mblk_t`; `nv(9)` -> `<sys/nvpair.h>`; `htole*/le*toh` ->
  `<sys/byteorder.h>` `LE_*`; `KASSERT`/`MPASS` -> `ASSERT`/`VERIFY`;
  `DRIVER_MODULE`/`moduledata_t` -> `modlinkage`/`modldrv`/`_init`/`_info`/`_fini`.

## D. Build-system divergences

- **D1.** `Makefile.files`: new object lists `NVMF_OBJS` (=`nvmf_transport.o`),
  `NVMF_TCP_OBJS` (=`nvmf_tcp.o`), `NVMF_HOST_OBJS`, `NVMFT_OBJS`. The host list
  tracks the *illumos* sources (`nvmf_host.o`, `nvmf_blkdev.o`, `nvmf_mpath.o`),
  not FreeBSD filenames.
- **D2.** `Makefile.rules`: per-directory `$(OBJS_DIR)/%.o` rules for
  `io/nvmf`, `io/nvmf_host`, `io/comstar/port/nvmft`.
- **D3.** Four `intel/*/Makefile`s. `nvmf`/`nvmf_tcp` are `misc` modules
  (`ROOT_MISC_DIR`); `nvmf_host` (`-N drv/blkdev -N misc/nvmf`) and `nvmft`
  (`-N drv/stmf -N misc/nvmf`) are drivers. `nvmf_tcp` additionally needs
  `-N misc/ksocket` (found at runtime modload: `ksocket_sendmblk`/
  `ksocket_krecv_set` are exported by `misc/ksocket`, not `genunix`; same
  pattern as `idm`/`iscsit`/`rdsv3`). FreeBSD records no such kmod deps.
- **D4.** New `nvmf_host.conf` / `nvmft.conf` (`parent="pseudo"`), required for
  the pseudo-device drivers; FreeBSD has no analog.
- **D5.** Built non-DEBUG (`dmake def` -> `obj64`) to match the only `genunix`
  variant in the SmartOS proto. A DEBUG build needs a DEBUG `genunix`.

## E. Deferred / stubbed (not a divergence in intent, but not yet functional)

133 `PORT-TODO` markers mark logic that is structurally present but not
implemented (the data paths): TCP socket state machine and digests; blkdev
`o_read`/`o_write` <-> NVMe command; keep-alive/reconnect timers; the multipath
path selector and ANA mechanism; the STMF datamove (`lport_xfer_data`/
`lport_send_status`); and the reservations->PGR translation. Each cites its
FreeBSD reference. See `PROGRESS.md` "Remaining work".

## F. Licensing

Ported files keep their FreeBSD BSD-2-Clause copyright plus an illumos CDDL
header and a provenance note. BSD-2 source coexists in the CDDL tree (illumos
already hosts BSD-licensed drivers). No GPL-only code was imported.
