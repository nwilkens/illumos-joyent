# Portable regressions and remaining device acceptance

From the repository root, with Python 3.9+ and a C99 compiler:

```sh
python3 -B usr/src/test/ice-tests/run_tests.py
python3 -B usr/src/test/ice-tests/run_tests.py --list
python3 -B usr/src/test/ice-tests/run_tests.py reset_requests.py rx_dma_faults.py
```

`run_tests.py` has an explicit manifest of 40 runnable tests. It excludes the
`c_test.py` and `rx_test.py` support modules and the hardware programs. It
reports each script, continues after failure, and returns nonzero if any script
fails or exceeds 60 seconds. Add new runnable checks to this manifest.

The actual-C runners use `c_test.py` to extract unchanged function bodies with
`#line` source locations, generate temporary headers, compile once, and run the
selected cases. `CC` defaults to `cc` and may include compiler arguments. All
use C99 with `-Wall -Wextra -Werror -pedantic`; individual runners retain their
needed pthread or unused-stub flags. Compilation has a 30-second deadline;
each C invocation has a 15-second deadline. `CTestFailure` labels `compile`
and `run` failures separately. `runner_checks.py` verifies these phases and
both deadlines with real processes, plus generated headers, flags, case
arguments, suite continuation, and support exclusion.

The source checks remain useful for boundaries outside the small fixtures.
They do not substitute for executing driver functions, and neither kind of
portable check establishes kernel scheduling, CPU memory ordering, firmware
behavior, DMA isolation, or throughput. Native module compilation is another
separate gate; its results belong with the exact source revision built.

| Worklist item | Actual-C regression | Controlled behavior | Device acceptance still pending |
| --- | --- | --- | --- |
| 1, 10 | `terminal_filters.py`, `filter_requests.py` | Desired filter ownership, retirement, callback failures, owed-reset gates, replay and attach rollback | Active-client cleanup after AQ/recovery failure; firmware replay and unregister |
| 2 | `detach_quiesce.py` | Isolation before unregister, failed stop/reset retention, FMA observer interleavings | Queue-disable/reset timeout, continued resource ownership, retry unload |
| 3 | `tx_quiesce.py` | Late notifications, active builders and in-flight callbacks using pthread handshakes | Delayed completion across failed queue stop and unregister |
| 4 | `rx_layout.py` | Copy/loan layout, VLAN reinsertion, full DMA write extent and packet bytes | Tagged/untagged and jumbo CPU, copies, software fanout and throughput |
| 5 | `reset_requests.py` | Real dispatch, worker and rebuild; duplicate work and preservation of later requests | Reset timing, coalescing and device recovery |
| 6 | `link_operational.py`, `reset_requests.py` | Cached carrier versus operational state, startup and rebuild failures | Physical carrier UP through failed restart and successful recovery |
| 7 | `rx_dma_faults.py` | Descriptor/data sync and handle errors, delivery suppression and loan cleanup | DMA fault injection, FMA reports and recovery |
| 8 | `lso_context.py` | IPv4/IPv6 MSS limits, context fields and rejection marker | Wire checksums and segmentation; LSO remains disabled by default |
| 9 | `filter_requests.py` | Captured request fields for GLD, attach, replay and teardown | Imported-core encoding and actual device programming |
| 13 | `tx_bind_threshold.py` | Copy/bind decisions, runt padding and fallback ownership | Ordinary datapath acceptance; driver behavior was unchanged by this test repair |

## Reproduce failing controls

Use current tests with selected historical production files. These commands
only create scratch copies; they leave the checkout unchanged. Run the test
commands individually: the listed negative controls must compile successfully
and then report `CTestFailure: run: exited ...`. A missing function or a
`compile` failure is not evidence that the behavioral regression caught the
bug. Current-source runs through the suite above must pass first.

```sh
ice_tests=usr/src/test/ice-tests
ice_source=usr/src/uts/common/io/ice
ice_control=$(mktemp -d)
ice_review=21286590789bdbf8b6ab5b85c42863a06261b31e
for name in ice.c ice.h ice_gld.c ice_intr.c ice_rx.c ice_tx.c ice_vsi.c; do
    git show "$ice_review:$ice_source/$name" > "$ice_control/$name"
done

python3 -B "$ice_tests/terminal_filters.py" \
    --source "$ice_control/ice_gld.c" --vsi-source "$ice_control/ice_vsi.c"
python3 -B "$ice_tests/filter_requests.py" --scenario rebuild_invalid \
    --gld-source "$ice_control/ice_gld.c" --vsi-source "$ice_control/ice_vsi.c"
python3 -B "$ice_tests/detach_quiesce.py" \
    --source "$ice_control/ice.c" --gld-source "$ice_control/ice_gld.c"
python3 -B "$ice_tests/tx_quiesce.py" \
    --source "$ice_control/ice_tx.c" --case empty_late
python3 -B "$ice_tests/rx_layout.py" \
    --source "$ice_control/ice_rx.c" --header "$ice_control/ice.h"
python3 -B "$ice_tests/reset_requests.py" \
    --source "$ice_control/ice.c" --intr-source "$ice_control/ice_intr.c"
python3 -B "$ice_tests/link_operational.py" \
    --source "$ice_control/ice_intr.c" --gld-source "$ice_control/ice_gld.c"
python3 -B "$ice_tests/lso_context.py" --source "$ice_control/ice_tx.c"
```

The expanded filter fixture now checks callback recovery before terminal
retirement, so its first failure against the review baseline is the recovery
contract. The item 9 historical request-equivalence result predates this
expanded fixture; today's `requests` scenario also runs these callback tests
and is expected to fail against old callbacks.

For descriptor-fault controls, use the revision immediately before item 7;
the original review revision has an incompatible FMA observer signature:

```sh
git show "6288a8a997:$ice_source/ice_rx.c" > "$ice_control/rx-before-fault.c"
git show "6288a8a997:$ice_source/ice.h" > "$ice_control/rx-before-fault.h"
python3 -B "$ice_tests/rx_dma_faults.py" \
    --source "$ice_control/rx-before-fault.c" --header "$ice_control/rx-before-fault.h"
```

To isolate the callback/replay change after the VSI ownership fix, use item
10a's revision. This also compiles and fails at runtime:

```sh
git show "79bd14d475:$ice_source/ice_gld.c" > "$ice_control/gld-before-recovery.c"
git show "79bd14d475:$ice_source/ice_vsi.c" > "$ice_control/vsi-before-recovery.c"
python3 -B "$ice_tests/filter_requests.py" --scenario recovery_replay \
    --gld-source "$ice_control/gld-before-recovery.c" \
    --vsi-source "$ice_control/vsi-before-recovery.c"
```

Item 13 repaired a stale source assertion, so the reviewed production source
must pass its replacement regression. Removing the runt fallback guard from a
scratch copy supplies a failing control for that behavior:

```sh
python3 -B "$ice_tests/tx_bind_threshold.py" --source "$ice_control/ice_tx.c"
python3 - "$ice_control/ice_tx.c" "$ice_control/tx-without-runt-guard.c" <<'PY'
from pathlib import Path
import sys
source = Path(sys.argv[1]).read_text()
old = "if (res == ICE_TX_BUILD_DROP || msglen < ICE_TX_MIN_LEN)"
assert source.count(old) == 1
Path(sys.argv[2]).write_text(source.replace(old, "if (res == ICE_TX_BUILD_DROP)"))
PY
python3 -B "$ice_tests/tx_bind_threshold.py" \
    --source "$ice_control/tx-without-runt-guard.c"
```

Other focused incomplete-fix controls and their expected assertions are
recorded with each item in [WORKLIST.md](WORKLIST.md). Source selection options
also accept such local mutant files. All device acceptance in the table is
pending for the reviewed fixes; historical datapath measurements in
[README.md](README.md) do not validate this revision.
