# mlxcx host checks

These checks run the mlxcx command, event and page code on a development
host with no NIC and no illumos kernel. They treat the device as hostile:
each scenario feeds the driver a value or an event order that a broken or
malicious firmware could produce.

Run `python3 -B usr/src/test/mlxcx-tests/run_tests.py` from the repository
root. It needs Python 3.9+ and a C compiler with AddressSanitizer (clang or
gcc) on a little-endian host. `--list` shows the manifest. Pass
`--source-dir DIR` to run the suite against another copy of
`usr/src/uts/common/io/mlxcx`, for example the parent of a fix, to see the
check fail without it. Each script also takes `--scenario NAME`.
Set `MLXCX_TEST_VERBOSE=1` to print driver warnings.

## How it works

`c_test.py` extracts functions and types from the driver source without
changes and compiles them with `mlxcx_stub.h`, which supplies
single-threaded stand-ins for mutexes, condition variables, lists, AVL
trees, id spaces, task queues and memory. `mlxcx_min.h` holds a cut-down
`mlxcx_t` and a DMA model that keeps freed buffers, so a device write to
freed memory fails the test. `include/sys/byteorder.h` lets the real
`mlxcx_reg.h` build on the host.

`mlxcx_cmdq_model.h` models the HCA command interface. A test script
decides, per doorbell, when and how the device answers, and can inject
stray completion events. A sleep that no scripted event can end fails the
test. The model fails the test if the driver rings a doorbell for a slot
the device still owns, or if the device would write to freed memory.

## Checks

| Script | What it proves |
| --- | --- |
| `cmdq_geometry.py` | Attach refuses a queue size and stride that do not fit the 4 KiB queue page. |
| `cmdq_completion.py` | A completion event for an idle, out-of-range, still-owned, wrong-token or already finished slot is counted and ignored. |
| `cmdq_abandon.py` | A timed-out command keeps its slot, token and mailboxes until hardware returns the entry with the right token; detach leaks them if it never does. |
