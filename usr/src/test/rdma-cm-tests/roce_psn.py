#!/usr/bin/env python3
"""Check the RoCE CM's Starting PSN against a Linux peer.

In Linux cm.c and cma.c, the Starting PSN of a REQ or REP is the PSN its
sender expects to receive: the sender programs it as its rq_psn and the
peer programs it as its sq_psn.  This test reads, from the rdk sources,
which connection field is random, which goes into the REQ and REP, which
takes the peer's value, and which feed rq_psn and sq_psn; then runs rdk
against a Linux model, each side active in turn, and rdk against rdk.
Each side's first packet must carry the PSN the other expects.  The
swapped reading (advertise the send PSN) must fail."""

import random
import re
import sys

from cm_test import CXGBE_TESTS, RDMA

sys.path.insert(0, str(CXGBE_TESTS))
from c_src import functions  # noqa: E402

FIELD = r"c->(ic_\w+)"


def one(pattern, body, what):
    found = set(re.findall(pattern, body))
    if len(found) != 1:
        raise SystemExit(f"cannot tell {what}: {sorted(found)}")
    return found.pop()


def rules(texts):
    bodies = {}
    for t in texts.values():
        bodies.update(functions(t))
    alloc = bodies["rdk_ibconn_alloc"]
    build = bodies["rdk_ibconn_build"]
    qp = bodies["rdk_ibconn_qp_rtr_rts"]
    req = build[build.index("IBCM_SEND_REQ"):build.index("IBCM_SEND_REP")]
    rep = build[build.index("IBCM_SEND_REP"):build.index("IBCM_SEND_RTU")]
    return {
        "random": one(r"random_get_pseudo_bytes\(\(uint8_t \*\)&" + FIELD,
                      alloc, "the random PSN"),
        "req": one(r"m\.m_psn = " + FIELD, req, "the REQ's PSN"),
        "rep": one(r"m\.m_psn = " + FIELD, rep, "the REP's PSN"),
        "peer_req": one(FIELD + r" = m->m_psn",
                        bodies["rdk_ibconn_from_req"], "the REQ's use"),
        "peer_rep": one(FIELD + r" = m->m_psn",
                        bodies["rdk_cm_roce_rep"], "the REP's use"),
        "rq": one(r"a\.rq_psn = " + FIELD, qp, "rq_psn"),
        "sq": one(r"a\.sq_psn = " + FIELD, qp, "sq_psn"),
    }


class Rdk:
    def __init__(self, r, rng):
        self.r = r
        self.f = {r["random"]: rng.randrange(1 << 24)}

    def advertise(self, kind):
        return self.f.get(self.r[kind])

    def take(self, kind, psn):
        self.f[self.r["peer_" + kind]] = psn

    def rq(self):
        return self.f.get(self.r["rq"])

    def sq(self):
        return self.f.get(self.r["sq"])


class Linux:
    """cma.c seq_num; cm.c rq_psn from its own message, sq_psn from the
    peer's."""

    def __init__(self, rng):
        self.seq = rng.randrange(1 << 24)
        self.sq_psn = None

    def advertise(self, kind):
        return self.seq

    def take(self, kind, psn):
        self.sq_psn = psn

    def rq(self):
        return self.seq

    def sq(self):
        return self.sq_psn


def connect(active, passive):
    passive.take("req", active.advertise("req"))
    active.take("rep", passive.advertise("rep"))
    return (active.sq() is not None and active.sq() == passive.rq() and
            passive.sq() is not None and passive.sq() == active.rq())


def check(r, rounds=200):
    rng = random.Random(7)
    bad = []
    for _ in range(rounds):
        for name, pair in (("rdk active, Linux passive",
                            (Rdk(r, rng), Linux(rng))),
                           ("Linux active, rdk passive",
                            (Linux(rng), Rdk(r, rng))),
                           ("rdk to rdk", (Rdk(r, rng), Rdk(r, rng)))):
            if not connect(*pair) and name not in bad:
                bad.append(name)
    return bad


def main():
    texts = {f: (RDMA / f).read_text(encoding="utf-8") for f in
             ("rdk_cm_roce.c", "rdk_cm_roce_conn.c", "rdk_cm_roce_rx.c")}
    r = rules(texts)
    bad = check(r)
    if bad:
        print("PSNs do not meet: " + "; ".join(bad) + f" ({r})")
        return 1
    # The reading Linux interop caught: advertise and send the random one.
    swapped = dict(r, req="ic_spsn", rep="ic_spsn", random="ic_spsn",
                   peer_req="ic_rpsn", peer_rep="ic_rpsn")
    if "rdk active, Linux passive" not in check(swapped, 20):
        print("the swapped reading met a Linux peer")
        return 1
    print("PASS: Starting PSN is the receive PSN; rdk meets a Linux peer "
          "either way round and itself; the swapped reading fails")
    return 0


if __name__ == "__main__":
    sys.exit(main())
