#!/usr/bin/env python3

"""Check that rx teardown never waits unbounded on loaned buffers."""

from pathlib import Path


REPO = Path(__file__).resolve().parents[4]
RX = REPO / "usr/src/uts/common/io/ice/ice_rx.c"
HEADER = REPO / "usr/src/uts/common/io/ice/ice.h"
ICE = REPO / "usr/src/uts/common/io/ice/ice.c"


def function(source: str, signature: str, following: str) -> str:
    start = source.index(signature)
    end = source.index(following, start)
    return source[start:end]


def main() -> None:
    rx = RX.read_text(encoding="utf-8")
    header = HEADER.read_text(encoding="utf-8")

    # No unbounded loan wait survives anywhere in the rx path.
    assert "cv_wait(&irr->irxr_cv" not in rx

    # The wait and the release are separate: the reset path takes only the
    # wait, because nothing that hardware can DMA into may be released before
    # the reset completes (see reset_rebuild.py).
    quiesce = function(
        rx,
        "ice_rx_quiesce(ice_t *ice)\n{",
        "\n/*\n * Wait, within the loan deadline",
    )

    # Every ring is closed before any ring is waited on, which is what makes a
    # single shared deadline fair.  irxr_shutdown is the gate that stops new
    # loans, and ice_prepare_for_reset() runs this before ice_queues_disable(),
    # so a ring still open while an earlier one is waited out keeps taking
    # fresh loans and can reach its own wait with more outstanding than when
    # the quiesce started -- and no budget left.
    assert quiesce.index("irxr_shutdown = B_TRUE") < quiesce.index(
        "deadline = ddi_get_lbolt()"
    )
    assert quiesce.count("for (i = 0; i < ice->ice_num_rxr") == 2
    close, wait = quiesce.split("deadline = ddi_get_lbolt()")
    # The closing pass may not block and may not release anything.
    assert "irxr_shutdown = B_TRUE" in close
    assert "irxr_started = B_FALSE" in close
    assert "cv_timedwait" not in close
    assert "ice_rx_pool_" not in close
    # ...and the waiting pass may not reopen the question of which rings are
    # closed.
    assert "irxr_shutdown = " not in wait
    assert "irxr_started = " not in wait
    assert "cv_timedwait(&irr->irxr_cv" in wait
    # An absolute deadline, not a relative one.  ice_rx_recycle() signals on
    # every returning buffer, so cv_reltimedwait would restart the clock on
    # each return and never terminate under a stack that dribbles loans back.
    # (i40e can use a relative wait only because it broadcasts once, at zero.)
    assert "cv_reltimedwait" not in quiesce
    # The waiting half releases nothing at all.
    assert "ice_rx_pool_" not in quiesce
    assert "ice_rx_reset_desc" not in quiesce
    assert "ice_rx_setup_bufs" not in quiesce
    assert "return (drained)" in quiesce

    # A ring that did not drain is left fully intact: its pool is not freed and
    # its descriptors are not reposted, so the loaned buffers return
    # harmlessly later and irxr_shutdown keeps them from being re-armed.
    reclaim = function(
        rx,
        "ice_rx_reclaim(ice_t *ice)\n{",
        "\nboolean_t\nice_rx_stop",
    )
    assert "ice_rx_pool_release(" in reclaim
    assert "ice_rx_reset_desc" not in reclaim
    assert "ice_rx_setup_bufs" not in reclaim

    # A pool leaves a ring only under the lock, and a pool with loans
    # outstanding only to be set aside: a loaned control block still holds
    # the stack's mblk in ircb_mp.  Pools are freed with the lock dropped.
    release = function(rx, "ice_rx_pool_release(ice_rx_ring_t *irr)\n{",
                       "\n}\n")
    assert release.index("mutex_enter(&irr->irxr_lock)") < \
        release.index("irxr_nloaned == 0") < \
        release.index("ice_rx_pool_swap(irr, NULL)") < \
        release.index("mutex_exit(&irr->irxr_lock)") < \
        release.index("ice_rx_pool_free(p)")
    assert rx.count("ice_rx_pool_swap(irr, ") == 2
    free = function(rx, "ice_rx_pool_free(ice_rx_pool_t *p)\n{", "\n}\n")
    assert "ASSERT0(p->irp_nloaned)" in free

    # A pool that survived a timed-out stop is never clobbered or reused.
    start = function(
        rx,
        "ice_rx_start(ice_t *ice)\n{",
        "\n/*\n * Tear down every rx ring",
    )
    assert start.index("ice_rx_pool_alloc(irr)") < \
        start.index("mutex_enter(&irr->irxr_lock)") < \
        start.index("ice_rx_pool_swap(irr, np)") < \
        start.index("mutex_exit(&irr->irxr_lock)") < \
        start.index("ice_rx_pool_retire(op)")
    # A pool with loans outstanding is replaced, not a reason to fail.
    assert start.index("ice_rx_pool_swap(irr, np)") < \
        start.index("op->irp_nloaned > 0") < \
        start.index("ice_rx_pool_orphan(irr, op)")
    assert "ICE_RX_ORPHANS_MAX" not in rx

    # Detach waits for the replaced pools' loans too, within the same bound.
    drain = function(rx, "ice_rx_orphans_drain(ice_t *ice)\n{", "\n}\n")
    assert "ice->ice_rx_orphan_loans == 0" in drain
    # A returned loan of a replaced pool can wait in a ring's return list.
    assert drain.index("ice_rx_harvest(irr)") < \
        drain.index("ice->ice_rx_orphan_loans == 0")
    assert "ICE_RX_LOAN_WAIT_US" in drain
    assert "return (B_FALSE);" in drain
    lifecycle = (Path(__file__).resolve().parents[2] /
                 "uts/common/io/ice/ice.c").read_text()
    assert "!ice_rx_quiesce(ice) || !ice_rx_orphans_drain(ice)" in lifecycle

    # Detach now uses the same close/upcall/loan fence as mac stop.  It
    # releases no buffer pool until hardware isolation has been confirmed.
    ring_free = function(
        rx,
        "ice_rx_ring_free(ice_rx_ring_t *irr)\n{",
        "\nstatic boolean_t\nice_rx_kstat_init",
    )
    assert "ice_rx_pool_release(irr)" in ring_free
    assert "ASSERT0(irr->irxr_nloaned)" in ring_free
    assert ring_free.index("ASSERT0(irr->irxr_nloaned)") < \
        ring_free.index("ice_rx_pool_release(irr)")
    # It must precede the ring teardown: the release takes irxr_lock.
    assert ring_free.index("ice_rx_pool_release(irr)") < \
        ring_free.index("ice_dma_free(&irr->irxr_desc_dma)")
    assert ring_free.index("ice_rx_pool_release(irr)") < \
        ring_free.index("mutex_destroy(&irr->irxr_lock)")

    rings_free = function(
        rx,
        "ice_rx_rings_free(ice_t *ice)\n{",
        "\n/*\n * QINT_RQCTL has no common-code helper",
    )
    assert "ice_rx_ring_free(&ice->ice_rxr[i])" in rings_free

    # A single bounded stop serves both the unplumb and the reset callers.
    assert "extern boolean_t ice_rx_stop(ice_t *);" in header
    assert "ice_rx_drain" not in header
    assert "ice_rx_stop_reset" not in header
    assert "ice_rx_stop_reset" not in rx
    assert "ICE_RX_RESET_LOAN_WAIT_US" not in rx
    assert "#define\tICE_RX_LOAN_WAIT_US" in rx

    # The behavioral detach regression exercises timeout, reset failure,
    # unregister failure, and admission rollback.  Check the integration
    # with the same bounded RX fence here.
    ice_c = ICE.read_text(encoding="utf-8")
    quiesce_detach = function(
        ice_c, "ice_detach_quiesce(ice_t *ice)\n{",
        "\nstatic int\nice_detach")
    assert quiesce_detach.index("ice_rx_quiesce(ice)") < \
        quiesce_detach.index("ice_queues_disable(ice)")
    assert "ice_rx_reclaim" not in quiesce_detach
    assert "ice_rx_drain" not in rx

    print("PASS: ice rx loan wait is bounded on every teardown path")


if __name__ == "__main__":
    main()
