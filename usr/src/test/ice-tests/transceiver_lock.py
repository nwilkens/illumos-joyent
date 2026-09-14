#!/usr/bin/env python3

"""Check that transceiver reads never hold ice_lock across firmware polling.

ice_lock is rebuilt with the MSI-X priority cookie, so it is a spin mutex
that blocks the OICR handler.  DLDIOC_READTRAN is reachable by any process in
the link's zone and each admin-queue command can poll for up to one second.
"""

from pathlib import Path


REPO = Path(__file__).resolve().parents[4]
GLD_SOURCE = REPO / "usr/src/uts/common/io/ice/ice_gld.c"
ICE_SOURCE = REPO / "usr/src/uts/common/io/ice/ice.c"


def function(source: str, signature: str, following: str) -> str:
    start = source.index(signature)
    end = source.index(following, start)
    return source[start:end]


def main() -> None:
    ice_c = ICE_SOURCE.read_text(encoding="utf-8")
    assert ("mutex_init(&ice->ice_lock, NULL, MUTEX_DRIVER,\n"
            "\t    DDI_INTR_PRI(ice->ice_intr_pri));") in ice_c

    gld = GLD_SOURCE.read_text(encoding="utf-8")
    read = function(
        gld,
        "ice_transceiver_read(void *arg, uint_t id, uint_t page, void *buf,",
        "\nstatic boolean_t\nice_m_getcapab",
    )
    assert "ice_aq_sff_eeprom(" in read
    assert "mutex_enter(&ice->ice_rebuild_lock);" in read
    assert "mutex_enter(&ice->ice_lock)" not in read
    assert "mutex_exit(&ice->ice_lock)" not in read

    # The lifecycle lock is released on both the failure and success paths.
    assert read.count("mutex_exit(&ice->ice_rebuild_lock);") == 2

    print("PASS: ice transceiver read holds no interrupt-priority lock")


if __name__ == "__main__":
    main()
