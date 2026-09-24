# irdma checks

The theory statements at the top of
[irdma.c](../../uts/common/io/irdma/irdma.c),
[irdma_ctl.c](../../uts/common/io/irdma/irdma_ctl.c) and
[ice_rdma.c](../../uts/common/io/ice/ice_rdma.c) record the rules these
checks exercise.  The ice MSI-X reservation is covered by
`usr/src/test/ice-tests/intr_irm.py`, `queue_count.py` and `rss.py`, and the
reset and detach hooks by `reset_requests.py` and `detach_quiesce.py` there.

## Portable suite

Run `python3 -B usr/src/test/irdma-tests/run_tests.py` from the repository
root with Python 3.9+ and a C99 compiler.

- `core_provenance.py`: every imported file in `core/` has the dual license
  line and a blob ID in `core/README.illumos`; a file with no `illumos:`
  marker hashes to its blob, and the marked functions are the ones the
  README lists.
- `fpm_checks.py`: runs the FPM query and commit checks against 28 hostile
  firmware values (zero block sizes the core divides by, counts that make
  its loops spin, sizes and bases that overflow or leave the SD table).
- `cqe_checks.py`: runs the core's CQE poll, with its local changes,
  against a forged QP pointer, a QP number that does not match, an SRQ
  entry, send and receive WQE indexes outside the posted work, a flush over
  a queue of NOPs and a CQ whose entries all stay valid.
- `cqp_requests.py`: runs the CQP request matching against stale, reused,
  forged and abandoned completions, and checks the core's CCQ reader
  bounds the WQE index and ignores the CQP pointer in the entry.
- `ice_qsets.py`: runs the ice qset add and delete operations: TC, VSI,
  count, duplicate handle and TEID ownership checks, rollback of a partial
  add, and a failed removal that owes a reset.
- `lock_order.py`: static checks of the interrupt priority and the peer lock
  order: the interrupt handler takes only its own lock and calls no core
  code; consumer completion handlers run from the interrupt task with no
  driver lock held, and the AEQ task never waits on a control command; ice calls the child with no lock held; the reset worker offlines and
  onlines the child with `ice_rebuild_lock` dropped; the quarantine is freed
  only after a reset.
- `rdk_verbs.py`: runs the rdmak QP state table against missing, extra and
  out-of-range attributes, states and types, and the DMA page walk against
  offsets, gaps, a full page list and a cookie that wraps the address
  space.
- `cstyle.py`: `cstyle -pP` over the driver, rdmak and the tests.

## On hardware

`rdma_accept.sh` runs on the DUT with `rdma_enable=1` in `ice.conf` and the
DEBUG irdma module installed; `irdmactl.c` is its ioctl tool:

    gcc -m64 -o irdmactl irdmactl.c
    rdma_accept.sh ./irdmactl <peer_ip> [tests]

It checks attach and detach of the child, rollback after an injected failure
at each bring-up step (`fail_step` in `irdma.conf`, DEBUG only), a CQP
command timeout, a PF reset with a CQP command in flight (DTrace shows every
buffer freed while the device may use it is held until the reset completes),
modunload with a held command, and an interrupt resource management trim
driven through the IRM callback.  Run
`usr/src/test/ice-tests/datapath_accept.sh` at MTU 1500 and 9000 with RDMA
enabled for the LAN.

`rdma_verbs.sh` runs the verbs tests on hardware with irdma, rdmak and
rdmat installed; `rdmatool.c` drives rdmat:

    gcc -m64 -o rdmatool rdmatool.c -lkstat -lsocket -lnsl
    rdma_verbs.sh -i <local_ip> [-p <peer_ip>] [-s <server_ip>] [tests]

On one host it runs the rdmatool suite between two sessions (SEND/RECV,
WRITE, READ and UD with every byte checked, FRWR with local and remote
invalidate, rejection of a bad or zero rkey, an out-of-bounds address and
missing MR or QP rights, latency, bandwidth with CPU per GB, and teardown with work
in flight), the same traffic with pings to the peer, irdma detached with a
stream in flight, a PF reset (DEBUG ice `_reset`) with a stream in flight,
and an interrupt resource management trim with a stream in flight.  With
`-s` it runs the suite and a TCP baseline (`rdmatool ... client <server>
tcp`) against `rdmatool -i <ip> server` on the other host.
