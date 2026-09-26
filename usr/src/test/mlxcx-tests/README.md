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

`pages_test.h` runs the page request, give, take and teardown code against
a model of the page commands that tracks which pages the device holds; the
device writes to each of them, so freeing one it holds fails the test.

## Checks

| Script | What it proves |
| --- | --- |
| `cmdq_geometry.py` | Attach refuses a queue size and stride that do not fit the 4 KiB queue page. |
| `cmdq_completion.py` | A completion event for an idle, out-of-range, still-owned, wrong-token or already finished slot is counted and ignored. |
| `cmdq_abandon.py` | A timed-out command keeps its slot, token and mailboxes until hardware returns the entry with the right token; detach leaks them if it never does. |
| `cmdq_deadline.py` | In event mode a command gives up at its deadline when firmware never answers or no slot comes free, and recovers when the completion event is lost. Once hardware holds every usable slot past its timeout, new commands fail within a rescan period. |
| `cmdq_pageslot.py` | MANAGE_PAGES has the last slot to itself and completes behind a full queue; other commands never take it; a one-slot queue is refused; page requests do not share the link-state taskq. |
| `cmdq_tokens.py` | Callers that have set up a command but hold no slot hold no token either, so a page command still posts; each post to a slot carries a non-zero token that differs from the last one in that slot, even after 254 posts elsewhere. |
| `cmdq_return_pages.py` | RETURN_PAGES refuses a returned count above the request, including one with the sign bit set, and a request above the page limit. |
| `pages_return.py` | Unknown and repeated returned PAs are counted and skipped in both the request and teardown paths; teardown stops when hardware returns nothing we know; a reclaim request with no pages given does not assert. |
| `pages_request.py` | A page request of INT32_MIN, a runtime or boot request that would pass the page limit, a failed allocation and a refused gift are all handled with the page lock balanced; a request for an unknown function is counted and dropped. |
| `pages_give_timeout.py` | Pages from a MANAGE_PAGES(GIVE) that timed out stay tracked as given, so teardown reclaims them instead of freeing memory the device holds. |
| `cmdq_uar.py` | ALLOC_UAR refuses UAR 0, a page past the BAR0 size that attach now keeps, and an index whose offset wraps 32 bits. |
| `teardown_order.py` | Source check: detach stops interrupts, then drains the page and async taskqs, before it destroys their mutexes, the ports, the EQs, the page list or the command queue. It tears packet buffers down before TEARDOWN_HCA only when none are quarantined, and frees or orphans them after it. |
| `quarantine.py` | After a failed DESTROY of a WQ, CQ or EQ, releasing its DMA does not panic; the memory stays live for hardware until TEARDOWN_HCA succeeds, and is leaked if it never does. A work queue that does not stop is not destroyed, and its memory is kept the same way, as is the memory of a WQ, CQ or EQ whose CREATE timed out; each CREATE records that timeout. |
| `groups.py` | A TX group whose CREATE_SQ failed or timed out tears down without the CQ backlink assertion, and keeps the memory of a queue whose CREATE timed out. A TX group that failed at its TIS, a CQ or an SQ part way tears down exactly the rings it set up, without holding `mlg_mtx`. RX setup undoes a failed ring at once (source check). |
| `bufs.py` | The posted RX buffers and TX chains of a work queue that did not stop stay allocated and bound, with the mblks and loaned RX buffers they use, until TEARDOWN_HCA succeeds; then everything is freed. If TEARDOWN_HCA fails they are leaked, a loaned buffer the stack returns afterwards does not touch the freed `mlxcx_t`, and `_fini()` refuses to unload the module their callbacks point into. |
| `cmdq_hca_cap.py` | QUERY_HCA_CAP returns failure for a bad status, a bad delivery status and a timeout. |
| `no_device_panic.py` | Source check: no VERIFY, ASSERT or `mlxcx_panic()` in the command, EQ and page paths uses a value read from a register, a command entry or output, an event entry or a returned page. Taint is tracked within each function only; `mlxcx_ring.c` and CQ entries are included. It also fails any VERIFY or ASSERT that requires a `*_DESTROYED` bit, which only a successful DESTROY sets, and any destroy function that asserts `*_STARTED` is clear, which only a successful stop does. |
