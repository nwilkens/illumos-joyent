# cxgbe offload core checks

Run `python3 -B usr/src/test/cxgbe-tests/run_tests.py` from the repository
root with Python 3.9 or later and a C99 compiler.  No NIC or illumos kernel
is needed.

- `intr_locks.py`: no offload code blocks while it holds an interrupt
  priority lock, and nothing an interrupt handler reaches blocks.
- `cpl_table.py`: the CPL dispatch table routes each opcode by an ID the
  CPL really carries, and accepts no NIC or host-to-chip opcode.
- `ops_boundary.py`: every child operation checks its peer and TID
  ownership, and the child never supplies a raw work request.
- `ioctl_abi.py`: the test ioctl has one layout for ILP32 and LP64.
- `tid_tables.py`: the real `t4_tid.c` against stub kernel headers.
- `ofld_units.py`: firmware range checks and completion waiters.

## Hardware

`t4ofld.c` drives `T4_IOCTL_OFLD_TEST` on an adapter with `rdma-enable=1`
in `t4nex.conf`.  Build it with the system compiler and `-I` for the t4nex
headers.  A two-host run:

    b# t4ofld DEV open
    b# t4ofld DEV listen PORT LADDR LPORT
    a# t4ofld DEV open
    a# t4ofld DEV connect PORT LADDR LPORT FADDR FPORT NEXTHOP_MAC
    a# t4ofld DEV send TID 64
    a# t4ofld DEV disconnect TID
    b# t4ofld DEV unlisten
    a# t4ofld DEV tpt OFFSET 4096
    a# t4ofld DEV close; b# t4ofld DEV close

`t4ofld DEV status` shows the connections; `kstat t4nex:0:ofld` shows the
CPL, TID and orphan counters.  Pick ports no host socket uses: the chip
takes SYNs to a listener and every segment of an offloaded connection.
