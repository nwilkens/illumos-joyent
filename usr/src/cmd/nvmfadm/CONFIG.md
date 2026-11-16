# Minimal NVMe-oF target config: one subsystem exporting one namespace

This is the smallest end-to-end recipe to stand up an `nvmft` (NVMe-over-Fabrics
COMSTAR target) subsystem that exports a single namespace backed by a zvol. It
uses the new `nvmfadm` CLI to create the subsystem target port, and the existing
`sbdadm`/`stmfadm` tools for the LU and its view (namespace) mapping.

`nvmfadm` only manages the subsystem *port* (one SubNQN -> one STMF
`stmf_local_port_t`). The LU-to-namespace mapping is done entirely with the
stock COMSTAR tools: NVMe NSID == STMF LUN + 1, so the first view entry (LUN 0)
becomes namespace 1.

## What nvmfadm does

`nvmfadm` opens `/devices/pseudo/nvmft@0:admin` and issues the
`NVMFT_IOC_SUBSYS_*` control ioctls defined in `<sys/nvme/nvmf_ioctl.h>`:

| Command                          | ioctl                     | Effect |
|----------------------------------|---------------------------|--------|
| `nvmfadm create-subsys <subnqn>` | `NVMFT_IOC_SUBSYS_CREATE` | `nvmft_port_alloc()` -> `stmf_register_local_port()` |
| `nvmfadm delete-subsys <subnqn>` | `NVMFT_IOC_SUBSYS_DELETE` | offline + `stmf_deregister_local_port()` -> `nvmft_port_free()` |
| `nvmfadm list-subsys`            | `NVMFT_IOC_SUBSYS_LIST`   | list registered SubNQNs |
| `nvmfadm restore`                | `NVMFT_IOC_SUBSYS_CREATE` | create the saved subsystems |

The request/reply payload is a packed `nvlist` carried in a `struct nvmf_ioc_nv`
(the same carrier the Fabrics host control path uses). For create, only the
SubNQN is required; the rest of the controller parameters fall back to the
Fabrics defaults (`NVMF_MAX_IO_ENTRIES`, `NVMF_IOCCSZ`, `NVMF_IORCSZ`,
`NVMF_NN`).

## Recipe

### 0. Load the modules

```bash
modload stmf          # COMSTAR framework (drv/stmf)
modload nvmf          # Fabrics transport core (misc/nvmf)
modload nvmft         # NVMe-oF target port provider (drv/nvmft)
svcadm enable stmf    # if not already running
```

### 1. Create the backing store (zvol) and the COMSTAR LU

```bash
zfs create -V 10G rpool/nvmf-disk
sbdadm create-lu /dev/zvol/rdsk/rpool/nvmf-disk
sbdadm list-lu                       # note the GUID
```

### 2. Create the subsystem target port

```bash
nvmfadm create-subsys \
    nqn.2024-01.com.example:nvmf-target:disk0
nvmfadm list-subsys                  # confirm it appears
```

Optional flags:

- `-s <serial>` set the controller serial number (default: derived from the
  host id).
- `-p <portid>` set the relative port id (default: 1).

### 3. Check the target

The port comes online when it is created, unless STMF's default state for new
target ports has been changed:

```bash
stmfadm list-target -v                                    # SubNQN should appear
stmfadm online-target nqn.2024-01.com.example:nvmf-target:disk0   # if offline
```

### 4. Map the LU as namespace 1

A view with no host/target group is visible to every target at LUN 0, which the
target reports as **NSID 1**:

```bash
stmfadm add-view <LU-GUID>           # GUID from step 1
stmfadm list-view -l <LU-GUID>       # confirm LUN 0
```

To restrict the LU to just this subsystem instead of all targets, put the
SubNQN in a target group and use `add-view -t <tg>`.

### 5. Start the target service

`nvmft` does not listen on the network itself. `nvmfd` accepts TCP connections
on the discovery port (8009) and the I/O port (4420), runs the Fabrics CONNECT
exchange, and hands each queue to the kernel:

```bash
svcadm enable svc:/system/nvmf/target:default
```

Once a host connects and issues `IDENTIFY`/`READ`/`WRITE` against NSID 1, the
target translates to SCSI and runs it against the zvol via `stmf_sbd`.

There is no authentication and no TLS. Any host that can reach the ports can
connect; restrict them with the service's `config/listen_addresses` property
and with STMF host groups.

## Teardown

```bash
stmfadm remove-view -l <LU-GUID> -a               # or remove-view <view#>
stmfadm offline-target nqn.2024-01.com.example:nvmf-target:disk0
nvmfadm delete-subsys nqn.2024-01.com.example:nvmf-target:disk0
sbdadm delete-lu <LU-GUID>
zfs destroy rpool/nvmf-disk
```

`delete-subsys` refuses (`EBUSY`) while any host is still connected; disconnect
hosts first. It offlines the port itself, so the explicit `offline-target` above
is optional but makes the intent clear.

## Notes

- The SubNQN must be a valid NQN (`nqn.YYYY-MM.` prefix or
  `nqn.2014-08.org.nvmexpress:uuid:<uuid>`); `nvmfadm` rejects others.
- One subsystem, one namespace is the minimal shape. Additional view entries on
  the same LU map to additional NSIDs (LUN + 1); additional LUs add more
  namespaces to the same subsystem.

## Persistence

`create-subsys` and `delete-subsys` also record the change in `nvmft`'s STMF
provider data, which `svc:/system/stmf` keeps across reboots. The target
service runs `nvmfadm restore` when it starts, which creates every saved
subsystem that does not exist yet.
