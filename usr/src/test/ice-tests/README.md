# ICE driver tests

These tests cover the illumos ICE driver and its integration with the Intel
common code. The portable suite combines source checks with C fixtures that
compile selected production functions unchanged against controlled kernel,
MAC, DMA, and firmware interfaces.

## Running the portable suite

From the repository root, with Python 3.9 or later and a C99 compiler:

```
python3 -B usr/src/test/ice-tests/run_tests.py
```

Use `--list` to list the tests, or pass script names to select tests:

```
python3 -B usr/src/test/ice-tests/run_tests.py --list
python3 -B usr/src/test/ice-tests/run_tests.py rx_layout.py lso_context.py
```

The C fixtures use `cc` by default. Set `CC` to select another compiler;
compiler arguments in `CC` are supported. Temporary sources and binaries are
removed after each test. No third-party Python packages are required.

The suite returns nonzero when a test fails or exceeds its 60-second limit.
The shared C runner reports compilation and execution failures separately,
with limits of 30 seconds for compilation and 15 seconds per executable case.
`runner_checks.py` exercises these failure paths using real child processes.

## Behavioral coverage

| Tests | Behavior exercised |
| --- | --- |
| `terminal_filters.py`, `filter_requests.py` | Filter request fields, accepted address ownership, failed firmware commands, retirement, and VSI replay |
| `detach_quiesce.py` | DMA isolation decisions before MAC unregister, resource retention on failure, and access-error observers |
| `tx_quiesce.py` | Completion notifications and admitted transmit calls during quiescence, including concurrent callback handshakes |
| `reset_requests.py` | Reset request coalescing, request retention during rebuild, deferred work, and failed restart handling |
| `link_operational.py` | Carrier versus operational link state, failed start, loopback, and property reporting |
| `rx_layout.py` | Copy and loan layout, aligned headers, VLAN restoration, jumbo assembly, and allocation accounting |
| `rx_dma_faults.py` | Descriptor and data DMA failures, delivery suppression, reposting, and cleanup |
| `lso_context.py` | IPv4/IPv6 metadata, MSS boundaries, context fields, and unsupported-request rejection |
| `tx_bind_threshold.py` | Copy/bind selection, zero-filled short frames, descriptor limits, fallback, and partial-allocation cleanup |
| `vsi_stats.py` | Hardware VSI index bounds, missing contexts, and preservation of cached counters |

Individual behavioral runners provide source-file overrides and, where
supported, case selection. Use a runner's `--help` for its options. Alternate
source files must match the fixture's interfaces; a compilation failure is
not evidence that the behavioral assertion detected a regression.

The remaining checks inspect source structure for driver lifecycle, interrupt
routing and rearming, RX budgets, offload gating, descriptor handling, pool
ownership, statistics, and build integration. These checks do not execute the
kernel driver. Neither source checks nor the C fixtures replace a native
module build or hardware testing. Controlled interfaces do not establish
device DMA isolation, CPU memory ordering, wire checksums, or performance.

## Hardware tests

`datapath_accept.sh` runs on an illumos host with a live `ice0` and a physical
peer. It changes the interface configuration, assigns `192.0.2.1/30`, and
repeatedly unplumbs and replumbs the interface. Use a dedicated test interface.
The peer must serve `iperf -s` at `192.0.2.2`; the script expects the local
iperf binary at `/opt/tools/bin/iperf`.

```
bash usr/src/test/ice-tests/datapath_accept.sh 192.0.2.2 1500
bash usr/src/test/ice-tests/datapath_accept.sh 192.0.2.2 9000
```

It checks link state, ICMP reachability, traffic counters, FMA counters, and
plumb/unplumb cycles. Run in both directions with the addresses assigned for
each test. This script does not cover every recovery or offload failure path.

`ice_loopback.c` controls the standard netlb ioctls through STREAMS `I_STR`.
Build it on illumos from the repository root:

```
gcc -Wall -Wextra -Werror -idirafter usr/src/uts/common \
    -o /tmp/ice_loopback usr/src/test/ice-tests/ice_loopback.c
```

LSO is opt-in through `tx_lso_enable` in `ice.conf`. Its portable tests check
driver decisions and descriptor fields; segmentation and wire checksums
require validation with the hardware offload enabled.
