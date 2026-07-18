#!/usr/bin/env python3

"""Check that rx teardown never waits unbounded on loaned buffers."""

from pathlib import Path


REPO = Path(__file__).resolve().parents[4]
RX = REPO / "usr/src/uts/common/io/ice/ice_rx.c"
HEADER = REPO / "usr/src/uts/common/io/ice/ice.h"


def function(source: str, signature: str, following: str) -> str:
    start = source.index(signature)
    end = source.index(following, start)
    return source[start:end]


def main() -> None:
    rx = RX.read_text(encoding="utf-8")
    header = HEADER.read_text(encoding="utf-8")

    # No unbounded loan wait survives anywhere in the rx path.
    assert "cv_wait(&irr->irxr_cv" not in rx

    stop = function(
        rx,
        "ice_rx_stop(ice_t *ice)\n{",
        "\nboolean_t\nice_rx_drain",
    )

    # One absolute deadline computed before the ring loop, not per ring.
    deadline = stop.index("deadline = ddi_get_lbolt()")
    ring_loop = stop.index("for (i = 0; i < ice->ice_num_rxr")
    assert deadline < ring_loop
    assert "cv_timedwait(&irr->irxr_cv" in stop

    # On timeout the ring is left fully intact: nothing freed, nothing reposted.
    bail = stop.index("if (irr->irxr_nloaned > 0) {", ring_loop)
    free = stop.index("ice_rx_free_rcbs(irr)", bail)
    assert bail < free
    bailout = stop[bail:free]
    assert "continue" in bailout
    assert "ice_rx_free_rcbs" not in bailout
    assert "ice_rx_reset_desc" not in bailout
    assert "ice_rx_setup_bufs" not in bailout

    # Every call site is guarded by the loan count or by pool ownership: a
    # loaned control block still holds the stack's mblk in ircb_mp.
    offset = 0
    sites = 0
    while True:
        try:
            offset = rx.index("ice_rx_free_rcbs(irr)", offset)
        except ValueError:
            break
        preceding = rx[max(0, offset - 400):offset]
        assert ("irxr_nloaned > 0" in preceding or
            "irxr_rcb_area == NULL" in preceding or
            "irxr_rcb_area != NULL" in preceding)
        sites += 1
        offset += 1
    assert sites > 0

    # A pool that survived a timed-out stop is never clobbered or reused.
    start = function(
        rx,
        "ice_rx_start(ice_t *ice)\n{",
        "\n/*\n * Tear down every rx ring",
    )
    assert "irxr_rcb_area != NULL" in start
    assert start.index("irxr_nloaned > 0") < start.index("ice_rx_alloc_rcbs")

    # Detach drain shares the same bounded shape and reclaims a stale pool.
    drain = function(
        rx,
        "ice_rx_drain(ice_t *ice)\n{",
        "\n/*\n * Resume the rx rings",
    )
    assert drain.index("deadline = ddi_get_lbolt()") < \
        drain.index("for (i = 0; i < ice->ice_num_rxr")
    assert "cv_timedwait(&irr->irxr_cv" in drain
    assert "irxr_rcb_area != NULL" in drain

    # A single bounded stop serves both the unplumb and the reset callers.
    assert "extern boolean_t ice_rx_stop(ice_t *);" in header
    assert "extern boolean_t ice_rx_drain(ice_t *);" in header
    assert "ice_rx_stop_reset" not in header
    assert "ice_rx_stop_reset" not in rx
    assert "ICE_RX_RESET_LOAN_WAIT_US" not in rx
    assert "#define\tICE_RX_LOAN_WAIT_US" in rx

    print("PASS: ice rx loan wait is bounded on every teardown path")


if __name__ == "__main__":
    main()
