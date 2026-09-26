#!/usr/bin/env python3
"""Check the offload CPL dispatch table against the CPL definitions.

Every opcode appears once.  An opcode routed by the TID in its header must
be a CPL that starts with union opcode_tid, so GET_TID() reads the TID and
not some other field.  No NIC or host-to-chip opcode may be accepted from
the offload queues, and every class in the table has a case in the dispatch
switch.  Negative advice on an active open is dropped before the ATID is
touched: the chip still owns the open, so the ATID must not be freed.
"""

import re
import sys

from c_src import COMMON, T4NEX, functions, strip

BY_TID = {"TCC_STID", "TCC_PASS_ACCEPT", "TCC_ACT_EST", "TCC_HWTID",
          "TCC_ACT_OPEN_RPL"}
NEVER = {"CPL_RX_PKT", "CPL_TX_PKT", "CPL_ACT_OPEN_REQ", "CPL_PASS_OPEN_REQ",
         "CPL_TX_DATA", "CPL_ABORT_REQ", "CPL_ABORT_RPL", "CPL_CLOSE_CON_REQ",
         "CPL_TID_RELEASE", "CPL_SET_TCB_FIELD", "CPL_L2T_WRITE_REQ",
         "CPL_RX_DATA_ACK", "CPL_PASS_ACCEPT_RPL"}


def first_member(msg, struct):
    match = re.search(rf"struct {struct} \{{(.*?)\}};", msg, re.DOTALL)
    if match is None:
        return None
    body = strip(match.group(1))
    for line in body.splitlines():
        line = line.strip()
        if line and line not in ("RSS_HDR", "WR_HDR;"):
            return line
    return None


def main():
    src = (T4NEX / "t4_ofld_cpl.c").read_text(encoding="utf-8")
    msg = (COMMON / "t4_msg.h").read_text(encoding="utf-8")
    table = re.search(r"t4_cpl_table\[NUM_CPL_CMDS\] = \{(.*?)\};", src,
                      re.DOTALL).group(1)
    entries = re.findall(r"TCD\((\w+),\s*(\w+),\s*struct (\w+)\)", table)
    assert len(entries) >= 15, entries
    bad = []
    seen = set()
    for opcode, cls, struct in entries:
        if opcode in seen:
            bad.append(f"{opcode} appears twice")
        seen.add(opcode)
        if opcode in NEVER:
            bad.append(f"{opcode} must not be accepted from offload queues")
        member = first_member(msg, struct)
        if member is None:
            bad.append(f"no struct {struct} in t4_msg.h")
        elif cls in BY_TID and member != "union opcode_tid ot;":
            bad.append(f"{opcode}: struct {struct} starts with {member!r}")
    switch = src[src.index("t4_ofld_cpl_dispatch_one(t4_ofld_t *of"):]
    for cls in {c for _, c, _ in entries} - {"TCC_DROP"}:
        if f"case {cls}:" not in switch:
            bad.append(f"no dispatch case for {cls}")
    rpl = functions(src)["t4_ofld_cpl_act_open_rpl"]
    advice, owner = rpl.find("t4_cpl_neg_advice("), rpl.find("TEF_OPEN")
    if advice < 0 or owner < 0 or advice > owner:
        bad.append("ACT_OPEN_RPL ends the open on negative advice")
    if bad:
        print("\n".join(bad))
        return 1
    print(f"PASS: {len(entries)} CPL opcodes, each routed by a checked ID")
    return 0


if __name__ == "__main__":
    sys.exit(main())
