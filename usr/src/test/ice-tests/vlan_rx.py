#!/usr/bin/env python3

"""Check that the ice receive path reinserts a hardware-stripped VLAN tag."""

from pathlib import Path
import re


REPO = Path(__file__).resolve().parents[4]
SOURCE = REPO / "usr/src/uts/common/io/ice/ice_rx.c"


def function(source: str, signature: str, following: str) -> str:
    start = source.index(signature)
    end = source.index(following, start)
    return source[start:end]


def main() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    frame = function(
        source,
        "ice_ring_rx_frame(ice_rx_ring_t *irr, uint32_t *total_lenp,",
        "\n/*\n * Drain the rx ring",
    )
    insert = function(
        source,
        "ice_rx_vlan_insert(mblk_t *mp, uint16_t tci)\n{",
        "\n/*\n * Validate one complete frame",
    )

    # The tag and its status bit must both be consumed.
    tag = frame.index("desc->wb.l2tag1")
    assert frame.count("desc->wb.l2tag1") == 1
    assert "ICE_RX_FLEX_DESC_STATUS0_L2TAG1P_S" in frame

    # Writeback fields are only trusted after the consumer barrier.
    assert frame.index("membar_consumer()") < tag

    # Bounds are established before inserting into the reserved headroom.
    check = frame.index("seglens[0] < sizeof (struct ether_header)")
    assert check < frame.index("ice_rx_vlan_insert(")
    assert "ASSERT3U(MBLKL(mp), >=, sizeof (struct ether_header))" in insert
    assert "mp->b_rptr - mp->b_datap->db_base" in insert
    assert "mp->b_rptr -= VLAN_TAGSZ" in insert
    assert "ovbcopy(mp->b_rptr + VLAN_TAGSZ, mp->b_rptr, 2 * ETHERADDRL)" in insert
    assert "ASSERT3U(MBLKL(mp), >=, sizeof (struct ether_vlan_header))" in insert
    # Byte preservation and contiguous IP/TCP headers run in rx_layout.py.

    # Checksum metadata lands on the head mac actually receives.
    assert frame.index("ice_rx_vlan_insert(") < frame.index("ice_rx_hcksum(")

    # The 802.1Q header is emitted in network byte order.
    compact = re.sub(r"\s+", "", insert)
    assert "*p++=(ETHERTYPE_VLAN>>8)&0xff;*p++=ETHERTYPE_VLAN&0xff;" in compact
    assert "*p++=(tci>>8)&0xff;*p++=tci&0xff;" in compact


if __name__ == "__main__":
    main()
