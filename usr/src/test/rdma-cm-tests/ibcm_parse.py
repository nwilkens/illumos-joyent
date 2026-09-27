#!/usr/bin/env python3
"""Run the IB CM message parser against hand-placed IBTA layouts, malformed
headers and fields, the RoCEv2 IPv4 and RDMA CM headers, round trips of
every message and a seeded fuzz run whose accepted MADs must hold the CM's
invariants.  Parsers with a check taken out must fail."""

import sys

from cm_test import CTestFailure, RDMA, TESTDIR, run_c

# Each mutation drops one check a peer could otherwise get past.
MUTATIONS = (
    ("rdk_ibcm_msg.c", "\t\t    m->m_ari_len > IBCM_REJ_ARI_MAX) {",
     "\t\t    B_FALSE) {"),
    ("rdk_ibcm_msg.c", "\t    !rdk_ibcm_qpn_ok(m->m_qpn) || m->m_pkey != "
     "IBCM_PKEY_DEFAULT ||", "\t    m->m_pkey != IBCM_PKEY_DEFAULT ||"),
    ("rdk_ibcm_msg.c", "m->m_transport != IBCM_TRANSPORT_RC ||", ""),
    ("rdk_ibcm_msg.c", "\t    m->m_mtu < 1 || m->m_mtu > 5 || ", "\t    "),
    ("rdk_ibcm_msg.c", "\tif (sum != 0xffff)\n\t\treturn (EINVAL);\n", ""),
    ("rdk_ibcm_msg.c", "(get16(h, 6) & 0xbfff) != 0 || ", ""),
    ("rdk_ibcm_msg.c", "\tif (get16(p, 2) == 0 || !rdk_ibcm_unicast4(src) "
     "||", "\tif (!rdk_ibcm_unicast4(src) ||"),
    ("rdk_ibcm_msg.c", "\t\tif (m->m_local_id == 0 || m->m_remote_id == 0 "
     "|| m->m_msg > 2)", "\t\tif (m->m_local_id == 0 || m->m_remote_id == 0)"),
)


def run(source, iters):
    return run_c([TESTDIR / "ibcm_parse.c", "rdk_ibcm_msg.c"],
                 {"rdk_ibcm_msg.c": source}, includes=[RDMA],
                 args=[str(iters)])


def main():
    real = (RDMA / "rdk_ibcm_msg.c").read_text(encoding="utf-8")
    try:
        out, mode = run(real, 400000)
    except CTestFailure as error:
        print(error)
        return 1
    print(out, end="")
    for _, old, new in MUTATIONS:
        if real.count(old) != 1:
            print(f"mutation anchor not found: {old!r}")
            return 1
        try:
            run(real.replace(old, new), 50000)
        except CTestFailure:
            continue
        print(f"a parser without {old.strip()!r} passed")
        return 1
    print(f"PASS: IB CM parser ({mode}), {len(MUTATIONS)} mutations caught")
    return 0


if __name__ == "__main__":
    sys.exit(main())
