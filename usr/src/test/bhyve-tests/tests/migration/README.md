<!--
This file and its contents are supplied under the terms of the
Common Development and Distribution License ("CDDL"), version 1.0.
You may only use this file in accordance with the terms of version
1.0 of the CDDL.

A full copy of the text of the CDDL should have accompanied this
source.  A copy of the CDDL is also available via the Internet at
http://www.illumos.org/license/CDDL.
-->

<!-- Copyright 2026 Edgecast Cloud LLC. -->

# bhyve migration tests

Tests for the bhyve live migration save / restore path.  Userspace,
unit-shaped tests; live-VM and cross-CN integration tests are run
separately on a paired test bench.

## What lives here today

- `snapshot_buf_roundtrip` — exercises `vm_snapshot_buf` and
  `vm_snapshot_buf_cmp`, the libvmmapi primitives that back
  `SNAPSHOT_VAR_OR_LEAVE` and `SNAPSHOT_VAR_CMP_OR_LEAVE`.  Every
  cross-host topology check on the import path (ncpus mismatch,
  MSI-X table_count mismatch, virtio `vc_nvq` vs `vc_max_nvq`,
  viona `nrings` vs `vc_max_nvq`) reads its value through one of
  those primitives before validating; a regression in the
  primitives would let those checks pass garbage.

## What should grow here

These tests need either a small refactor to expose the relevant
helpers from `cmd/bhyve/common/bhyve_control.c` and
`cmd/bhyve/common/pci_emul.c`, or a fixture that drives a real
bhyve through its control socket.  Both are larger than the bug
fixes they would cover; tracked separately:

- **`section_hdr_validation`** — construct a `ctl_blob_hdr` /
  `ctl_section_hdr` stream with bad magic, bad version, name_len
  past the end of the buffer, blob_len past the end, and verify
  `parse_and_apply_stream` rejects each.
- **`bar_restore_in_range`** — feed every `pcibar_type` value
  including non-enumerated ones into `bar_restore_in_range` and
  verify only the four valid types pass.
- **`ncpus_mismatch`** — paired sender / receiver vmctxs at
  different `cpu_count`, exercise `snapshot_class_per_vcpu` and
  `snapshot_vmcx`, expect `EINVAL`.
- **`msix_table_count_mismatch`** — synthetic device save with one
  `table_count`, restore against a device init'd with a different
  `table_count`, expect `EINVAL` from `pci_snapshot_pci_dev`.
- **`vc_nvq_overrun`** — restore a virtio softc with `vc_nvq`
  larger than `vc_max_nvq`, expect `EINVAL` from
  `vi_pci_snapshot_consts`.
- **`viona_nrings_overrun`** — same shape against
  `pci_viona_snapshot_inner`.
- **`paddr_guest2host_null`** — restore a virtqueue with a GPA
  that does not map to a host region; expect `EFAULT` from
  `vm_snapshot_guest2host_addr`.
- **`vmcx_seg_fpu_runstate`** — restore a VMCX whose seg/FPU/run-
  state fields the kernel will reject, expect `restore_vmcx_vcpu`
  to return errno rather than logging and continuing.

## Live-VM and cross-CN tests

Out of scope for `bhyve-tests`.  Documented separately in
`mariana-trench/services/vmm-migrate-agent/TESTING.md`.
