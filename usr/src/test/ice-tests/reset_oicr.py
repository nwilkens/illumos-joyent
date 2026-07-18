#!/usr/bin/env python3

"""Check ICE fatal-cause OICR decode and fail-closed source invariants."""

from pathlib import Path


REPO = Path(__file__).resolve().parents[4]
HEADER = REPO / "usr/src/uts/common/io/ice/ice.h"
INTR_SOURCE = REPO / "usr/src/uts/common/io/ice/ice_intr.c"
GLD_SOURCE = REPO / "usr/src/uts/common/io/ice/ice_gld.c"


def function(source: str, signature: str, following: str) -> str:
    start = source.index(signature)
    end = source.index(following, start)
    return source[start:end]


def main() -> None:
    header = HEADER.read_text(encoding="utf-8")
    assert "ICE_STATE_PFR_REQ" in header
    assert "ICE_STATE_RESET_FAILED" in header
    assert "uint32_t\t\tice_oicr_cause;" in header

    intr = INTR_SOURCE.read_text(encoding="utf-8")

    # Every fatal and reset cause stays enabled, and the fatal mask names the
    # PFR-triggering causes.
    for cause in (
        "PFINT_OICR_ECC_ERR_M",
        "PFINT_OICR_MAL_DETECT_M",
        "PFINT_OICR_PCI_EXCEPTION_M",
        "PFINT_OICR_HMC_ERR_M",
        "PFINT_OICR_PE_CRITERR_M",
        "PFINT_OICR_GRST_M",
    ):
        assert cause in intr, cause
    fatal_mask = function(intr, "#define\tICE_OICR_FATAL_MASK", "#define\tICE_OICR_CAUSE_MASK")
    for cause in (
        "PFINT_OICR_ECC_ERR_M",
        "PFINT_OICR_PCI_EXCEPTION_M",
        "PFINT_OICR_HMC_ERR_M",
        "PFINT_OICR_PE_CRITERR_M",
    ):
        assert cause in fatal_mask, cause

    # The ISR fails closed and latches causes.
    isr = function(intr, "ice_intr_oicr(ice_t *ice)\n{", "\nstatic uint_t\nice_intr_queue")
    assert "hw->reset_ongoing = true;" in isr
    assert "ICE_STATE_RESET_PENDING | ICE_STATE_ERROR" in isr
    assert "ICE_STATE_ERROR | ICE_STATE_PFR_REQ" in isr
    assert "ice->ice_oicr_cause |= oicr & ICE_OICR_CAUSE_MASK;" in isr
    # The admin-vector dispatch and re-arm remain.
    assert "ice->ice_oicr_pending = B_TRUE;" in isr
    assert "ice_intr_oicr_enable(ice);" in isr

    # The MDD handler clears every detection register and fails closed only when
    # this function is the offender.
    mdd = function(intr, "ice_oicr_mdd(ice_t *ice)\n{", "\nstatic void\nice_oicr_fatal")
    for reg in ("GL_MDET_TX_PQM", "GL_MDET_TX_TCLAN", "GL_MDET_RX",
                "PF_MDET_TX_PQM", "PF_MDET_TX_TCLAN", "PF_MDET_RX"):
        assert reg in mdd, reg
    assert "ICE_STATE_ERROR | ICE_STATE_PFR_REQ" in mdd
    # Reports whether this function was the offender so the caller can gate.
    assert "return (pf_mdd);" in mdd

    # The fatal handler logs and drives the link down only on a this-PF fault.
    fatal = function(intr, "ice_oicr_fatal(ice_t *ice, uint32_t cause)\n{", "\nstatic void\nice_oicr_task")
    assert "ice_oicr_mdd(ice)" in fatal
    assert "ice_link_state_set(ice, LINK_STATE_DOWN);" in fatal
    assert "if (fault) {" in fatal
    assert "LINK_STAT_CHANGE" not in fatal

    # The worker snapshots the causes and skips the drain during reset.
    worker = function(intr, "ice_oicr_task(void *arg)\n{", "\nstatic void\nice_intr_oicr_enable")
    assert "cause = ice->ice_oicr_cause;" in worker
    assert "ice->ice_oicr_cause = 0;" in worker
    assert "ice_oicr_fatal(ice, cause);" in worker
    assert "hw->reset_ongoing ||" in worker
    assert "ICE_STATE_RESET_PENDING | ICE_STATE_PFR_REQ" in worker

    # mac start refuses while a reset failed terminally or a rebuild is owed,
    # so a replumb cannot re-open the datapath on stale hardware.
    gld = GLD_SOURCE.read_text(encoding="utf-8")
    start = function(gld, "ice_m_start(void *arg)\n{", "\nstatic void\nice_m_stop")
    assert "ICE_STATE_RESET_FAILED" in start
    assert "ICE_STATE_PFR_REQ" in start
    assert "ICE_STATE_RESET_PENDING" in start
    assert "return (EIO);" in start

    print("PASS: ice fatal-cause OICR decode source invariants")


if __name__ == "__main__":
    main()
