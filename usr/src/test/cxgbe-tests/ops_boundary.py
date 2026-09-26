#!/usr/bin/env python3
"""Check the boundary between t4nex and its RDMA child.

Every operation in t4_rdma_ops_t is filled in, and every entry point
validates the peer before using it.  Operations on the client's IDs pin the
client, so that close cannot sweep the IDs while one is running.  No operation takes a raw work request
from the child: the child names IDs and parameters and t4nex builds the
CPL.  Each connection operation checks that the calling client owns the
TID, and each work request goes through t4_ofld_wr_send().
"""

import re
import sys

from c_src import T4NEX, calls, functions

# Operations that work without a live client, or that manage the client.
UNPINNED = {"tro_open", "tro_close", "tro_dma_free", "tro_reset",
            "tro_stopped"}


def main():
    header = (T4NEX / "t4_rdma.h").read_text(encoding="utf-8")
    ops = "".join((T4NEX / f).read_text(encoding="utf-8") for f in
                  ("t4_ofld_ops.c", "t4_rdma_peer.c", "t4_ofld_ri.c"))
    vector = re.search(r"typedef struct t4_rdma_ops \{(.*?)\} t4_rdma_ops_t;",
                       header, re.DOTALL).group(1)
    members = re.findall(r"\(\*(tro_\w+)\)", vector)
    init = re.search(r"const t4_rdma_ops_t t4_rdma_ops = \{(.*?)\};", ops,
                     re.DOTALL).group(1)
    assigned = dict(re.findall(r"\.(tro_\w+) = (\w+)", init))
    bad = [f"{m} is not filled in" for m in members if m not in assigned]
    bad += [f"{m} is not in t4_rdma_ops_t" for m in assigned
            if m not in members]
    bad += [f"{m} looks like a raw work request op" for m in members
            if re.search(r"wr_send|raw|_wr$|cmd", m)]

    bodies = functions(ops)
    if "t4_rdma_peer_ofld" not in bodies.get("t4_rdma_op_enter", ""):
        bad.append("t4_rdma_op_enter() does not validate its peer")
    for member, func in assigned.items():
        body = bodies.get(func, "")
        if member in UNPINNED:
            if "t4_rdma_peer_ofld" not in body:
                bad.append(f"{func}() does not validate its peer")
        elif "t4_rdma_op_enter" not in body or \
                "t4_rdma_op_exit" not in body:
            bad.append(f"{func}() does not pin the client")

    for func in ("t4_ofld_accept", "t4_ofld_flowc", "t4_ofld_close_con",
                 "t4_ofld_abort", "t4_ofld_abort_rpl", "t4_ofld_rx_credits",
                 "t4_ofld_tx_data", "t4_ofld_set_tcb_field",
                 "t4_ofld_tid_release", "t4_ofld_tid_bind"):
        body = bodies[func]
        if not re.search(r"t4_tid_owned\(|t4_ofld_conn\(", body):
            bad.append(f"{func}() does not check TID ownership")
    builds = re.compile(r"t4_ofld_init_tp_wr|FW_ULPTX_WR|"
                        r"FW_OFLD_TX_DATA_WR|V_FW_WR_OP\(")
    builders = {f for f, b in bodies.items() if builds.search(b)}
    for func, body in bodies.items():
        if "t4_ofld_wr_send" in body and func not in builders and \
                not calls(body) & builders:
            bad.append(f"{func}() sends a work request it did not build")
    if bad:
        print("\n".join(bad))
        return 1
    print(f"PASS: {len(members)} child operations, each peer-checked and "
          f"building its own work requests")
    return 0


if __name__ == "__main__":
    sys.exit(main())
