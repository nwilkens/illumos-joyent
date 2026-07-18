#!/usr/bin/env python3

"""Check that safe mode withholds the hardware offload capabilities."""

from pathlib import Path


REPO = Path(__file__).resolve().parents[4]
GLD_SOURCE = REPO / "usr/src/uts/common/io/ice/ice_gld.c"
DDP_SOURCE = REPO / "usr/src/uts/common/io/ice/ice_ddp.c"


def function(source: str, signature: str, following: str) -> str:
    start = source.index(signature)
    end = source.index(following, start)
    return source[start:end]


def main() -> None:
    ddp = DDP_SOURCE.read_text(encoding="utf-8")
    assert "ice->ice_safe_mode = B_TRUE" in ddp

    gld = GLD_SOURCE.read_text(encoding="utf-8")

    hck = function(
        gld,
        "case MAC_CAPAB_HCKSUM: {",
        "case MAC_CAPAB_TRANSCEIVER:",
    )
    assert "ice->ice_safe_mode" in hck
    # The capability must be refused outright, not advertised with no flags.
    assert "return (B_FALSE)" in hck
    # The guard must precede the assignment, not follow it.
    assert hck.index("ice->ice_safe_mode") < hck.index(
        "*txflags = HCKSUM_INET_PARTIAL")

    lso = function(gld, "case MAC_CAPAB_LSO: {", "\n\tdefault:")
    assert "ice->ice_safe_mode" in lso
    assert lso.index("ice->ice_safe_mode") < lso.index("LSO_TX_BASIC_TCP_IPV4")

    # The rx half: without the DDP package the descriptor status bits carry no
    # checksum verdict, so ice_rx_hcksum() must not report a verified checksum.
    rx = (REPO / "usr/src/uts/common/io/ice/ice_rx.c").read_text(encoding="utf-8")
    hck = function(
        rx,
        "ice_rx_hcksum(ice_rx_ring_t *irr, mblk_t *mp, uint16_t status0,",
        "\nstatic mblk_t *",
    )
    assert "irr->irxr_ice->ice_safe_mode" in hck
    assert hck.index("irr->irxr_ice->ice_safe_mode") < hck.index("HCK_IPV4_HDRCKSUM_OK")

    print("PASS: ice safe mode offload invariants")


if __name__ == "__main__":
    main()
