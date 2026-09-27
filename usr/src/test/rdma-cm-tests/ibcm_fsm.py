#!/usr/bin/env python3
"""Run the IB CM state machine through setup, teardown, retries, MRA
extensions, rejects, an abort and a flush in every state, and a seeded
random run checked against a model of the timer, the QP and the ID's
events.  Machines with a rule taken out must fail."""

import sys

from cm_test import CTestFailure, RDMA, TESTDIR, run_c

MUTATIONS = (
    ("\t\ta->ia_final = B_TRUE;\n\t\tf->f_attached = B_FALSE;\n",
     "\t\ta->ia_final = B_TRUE;\n"),
    ("\tif (!f->f_attached)\n\t\treturn;\n", ""),
    ("\tf->f_retries--;\n", ""),
    ("\ta->ia_timer_ms = f->f_tw_ms;\n", ""),
    ("\t\trdk_ibcm_ev(f, a, IBCE_CLOSE, ENXIO, B_TRUE);\n", ""),
    ("\tms = rdk_ibcm_time_ms(in->ii_mra_timeout) + f->f_life_ms;\n",
     "\tms = rdk_ibcm_time_ms(in->ii_mra_timeout);\n"),
    ("\t\tif (s != IBCS_REP_SENT && s != IBCS_MRA_REP_RCVD)\n\t\t\tbreak;\n"
     "\t\ta->ia_timer_stop = B_TRUE;\n",
     "\t\tif (s != IBCS_REP_SENT && s != IBCS_MRA_REP_RCVD)\n\t\t\tbreak;\n"),
    ("\t\trdk_ibcm_ev(f, a, IBCE_CLOSE, 0, B_TRUE);\n\t\tbreak;\n"
     "\tcase IBCS_IDLE:", "\t\tbreak;\n\tcase IBCS_IDLE:"),
    ("\t\tif (f->f_mra_max_ms != 0 && ms > f->f_mra_max_ms)\n"
     "\t\t\tms = f->f_mra_max_ms;\n", ""),
    ("\tf->f_max_retries = max_retries > 15 ? 15 : max_retries;\n",
     "\tf->f_max_retries = max_retries;\n"),
)


def run(source, n):
    return run_c([TESTDIR / "ibcm_fsm.c", "rdk_ibcm_fsm.c"],
                 {"rdk_ibcm_fsm.c": source}, includes=[RDMA],
                 args=[str(n)])


def main():
    real = (RDMA / "rdk_ibcm_fsm.c").read_text(encoding="utf-8")
    try:
        out, mode = run(real, 200000)
    except CTestFailure as error:
        print(error)
        return 1
    print(out, end="")
    for old, new in MUTATIONS:
        if real.count(old) != 1:
            print(f"mutation anchor not found: {old!r}")
            return 1
        try:
            run(real.replace(old, new), 20000)
        except CTestFailure:
            continue
        print(f"a machine without {old.strip()!r} passed")
        return 1
    print(f"PASS: IB CM state machine ({mode}), {len(MUTATIONS)} mutations "
          f"caught")
    return 0


if __name__ == "__main__":
    sys.exit(main())
