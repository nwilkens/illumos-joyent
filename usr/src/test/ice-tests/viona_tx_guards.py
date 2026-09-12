#!/usr/bin/env python3

"""Check the viona TX guards that protect a shared physical provider.

viona is the path by which an untrusted guest reaches the ICE driver.  Its
LSO seed writes through raw pointers into the first mblk, and MAC does not
bound a client's frame against the link SDU.
"""

from pathlib import Path


REPO = Path(__file__).resolve().parents[4]
TX_SOURCE = REPO / "usr/src/uts/intel/io/viona/viona_tx.c"
IMPL_HEADER = REPO / "usr/src/uts/intel/io/viona/viona_impl.h"


def function(source: str, signature: str, following: str) -> str:
    start = source.index(signature)
    end = source.index(following, start)
    return source[start:end]


def main() -> None:
    tx = TX_SOURCE.read_text(encoding="utf-8")
    offloads = function(tx, "viona_tx_offloads(viona_vring_t *ring,",
                        "\nstatic mblk_t *\nviona_tx_alloc_headers")

    # LSO admission: whole parsed header in the first mblk, TCP only, and the
    # guest's checksum location is the parsed TCP checksum field.
    gate = function(offloads, "if ((link->l_features & dev_feature) == 0 ||",
                    "lso_info_set(mp, gso_size, HW_LSO);")
    assert "meoi->meoi_l4proto != IPPROTO_TCP" in gate
    assert "full_hdr_sz > MBLKL(mp)" in gate
    assert "hdr->vrh_csum_offset !=\n\t\t    tcp_csum_off" in gate
    assert "VIONA_RING_STAT_INCR(ring, tx_gso_fail);" in gate
    assert "return (B_FALSE);" in gate
    # The raw-pointer seed still follows the gate, not the other way round.
    assert offloads.index("full_hdr_sz > MBLKL(mp)") < \
        offloads.index("TCP_CHECKSUM_OFFSET);")

    # Over-MTU frames reach mac_tx only as an accepted LSO request.
    body = function(tx, "viona_tx(viona_link_t *link, viona_vring_t *ring)\n{",
                    "\ndrop_fail:")
    mtu = function(body, "if (pkt_len > sizeof (struct ether_vlan_header) + "
                   "link->l_mtu) {", "\n\t}\n")
    assert "(DB_LSOFLAGS(mp_head) & HW_LSO) == 0" in mtu
    assert "VIONA_RING_STAT_INCR(ring, tx_drop_over_mtu);" in mtu
    assert "goto drop_fail;" in mtu
    # After offload processing decided LSO, before the desb is committed.
    assert body.index("viona_tx_offloads(ring, &hdr, &meoi, mp_head") < \
        body.index("tx_drop_over_mtu")
    assert body.index("tx_drop_over_mtu") < body.index("dp->d_len = total_len;")

    header = IMPL_HEADER.read_text(encoding="utf-8")
    assert "uint64_t\trs_tx_drop_over_mtu;" in header

    print("PASS: viona tx LSO header and MTU guards")


if __name__ == "__main__":
    main()
