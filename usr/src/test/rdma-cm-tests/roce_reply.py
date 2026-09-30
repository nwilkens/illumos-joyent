#!/usr/bin/env python3
"""Check where the RoCE CM sends replies that no resolved path covers.

A stateless reply (a REJ to a REQ nobody takes, a DREP to an unknown
DREQ) goes to the source MAC of the frame it answers, which the GSI agent
keeps from the completion.  A request starts with the same way back, so a
REJ sent before its neighbor resolution ends still reaches the peer.  Nothing on that path may start a neighbor
resolution: a sender we do not know must not make the host ARP.
Variants of the sources with a rule taken out must fail."""

import re
import sys

from cm_test import CXGBE_TESTS, RDMA

sys.path.insert(0, str(CXGBE_TESTS))
from c_src import calls, functions  # noqa: E402

FILES = ("rdk_gsi.c", "rdk_cm_roce_rx.c", "rdk_cm_addr.c", "rdk_cm_listen.c",
         "rdk_cm.c", "rdk_cm_roce.c", "rdk_cm_roce_conn.c")
RESOLVE = re.compile(r"\bIP2MAC_RESOLVE\b|\brdk_cm_arp_start\s*\(")


def reach(bodies, start):
    seen, todo = set(), [start]
    while todo:
        n = todo.pop()
        if n in seen or n not in bodies:
            continue
        seen.add(n)
        todo.extend(calls(bodies[n]))
    return seen


def check(texts):
    bodies = {}
    for t in texts.values():
        bodies.update(functions(t))
    bad = []
    recv = bodies.get("rdk_gsi_recv_done", "")
    if not re.search(r"RDK_WC_WITH_SMAC", recv) or \
            not re.search(r"bcopy\s*\(\s*wc->smac\s*,\s*rx->rx_smac", recv):
        bad.append("rdk_gsi_recv_done() does not keep the source MAC")
    back = bodies.get("rdk_cm_roce_back", "")
    use = back.find("rx->rx_smac")
    nbr = back.find("rdk_cm_nexthop_lookup")
    local = back.find("p.cp_smac")
    if use < 0:
        bad.append("rdk_cm_roce_back() does not use the source MAC")
    elif 0 <= nbr < use or 0 <= local < use:
        bad.append("rdk_cm_roce_back() prefers a route's MAC to the frame's")
    req = bodies.get("rdk_cm_roce_req", "")
    back_at = req.find("rdk_cm_roce_back(")
    mac_at = req.find("c->ic_path.gp_dmac")
    resolve_at = req.find("rdk_ibconn_resolve(")
    if not 0 <= back_at < mac_at < resolve_at:
        bad.append("rdk_cm_roce_req() resolves before the REQ's way back "
                   "is set, so an early REJ has no MAC")
    for fn in ("rdk_cm_roce_back", "rdk_cm_roce_reply"):
        for n in reach(bodies, fn):
            if RESOLVE.search(bodies[n]):
                bad.append(f"{fn}() reaches a neighbor resolution in {n}()")
    return bad


def main():
    texts = {f: (RDMA / f).read_text(encoding="utf-8") for f in FILES}
    bad = check(texts)
    if bad:
        print("\n".join(bad))
        return 1
    cases = (
        ("rdk_gsi.c", "\tif (rx->rx_has_smac)\n\t\tbcopy(wc->smac, "
         "rx->rx_smac, ETHERADDRL);\n", ""),
        ("rdk_cm_roce_rx.c", "\t\telse if (rx->rx_has_smac)\n\t\t\tbcopy("
         "rx->rx_smac, gp->gp_dmac, ETHERADDRL);\n", ""),
        ("rdk_cm_roce_rx.c", "\t\telse if (rx->rx_has_smac)\n\t\t\tbcopy("
         "rx->rx_smac, gp->gp_dmac, ETHERADDRL);\n\t\telse if (p.cp_local)\n"
         "\t\t\tbcopy(p.cp_smac, gp->gp_dmac, ETHERADDRL);\n",
         "\t\telse if (p.cp_local)\n\t\t\tbcopy(p.cp_smac, gp->gp_dmac, "
         "ETHERADDRL);\n\t\telse if (rx->rx_has_smac)\n\t\t\tbcopy("
         "rx->rx_smac, gp->gp_dmac, ETHERADDRL);\n"),
        ("rdk_cm_roce_rx.c", "\t\t\tret = rdk_cm_nexthop_lookup(",
         "\t\t\tret = rdk_cm_arp_start("),
        ("rdk_cm_roce_rx.c", "\t\tbcopy(gp.gp_dmac, c->ic_path.gp_dmac, "
         "ETHERADDRL);\n", ""),
    )
    for name, old, new in cases:
        if texts[name].count(old) != 1:
            print(f"mutation anchor not found in {name}: {old!r}")
            return 1
        if not check(dict(texts, **{name: texts[name].replace(old, new)})):
            print(f"a variant without {old.strip()!r} passed")
            return 1
    print(f"PASS: stateless and early replies use the frame's source MAC "
          f"and never resolve; {len(cases)} variants caught")
    return 0


if __name__ == "__main__":
    sys.exit(main())
