#!/usr/bin/env python3
"""Run the RoCEv2 GID table reconciliation against fake devices: slots that
fill up, deletions refused while a QP uses the slot, revival, moves between
ports, failed additions, device removal and a full table, then a seeded
random run checked against a model after every pass.  Tables with a step
taken out must fail."""

import sys

from cm_test import CTestFailure, RDMA, TESTDIR, run_c

MUTATIONS = (
    ("\t\tif (e->ge_state == RGS_STALE)\n\t\t\te->ge_revive = B_TRUE;\n", ""),
    ("\t\t\tgt->gt_ops->gto_withdraw(gt->gt_arg, e->ge_dev,\n"
     "\t\t\t    e->ge_port, e->ge_index, B_TRUE);\n", ""),
    ("\t\tif (!e->ge_marked) {\n\t\t\trdk_gident_free(gt, e);\n"
     "\t\t\tcontinue;\n\t\t}\n", "\t\tif (!e->ge_marked)\n\t\t\tcontinue;\n"),
    ("\t\tif (!rdk_gident_at(e, dev, port, mac)) {",
     "\t\tif (B_FALSE) {"),
    ("\t\tif (e->ge_state == RGS_STALE)\n\t\t\treturn (B_TRUE);\n", ""),
    ("\t\t\te->ge_err = ret;\n\t\t\tst->gs_busy++;\n",
     "\t\t\trdk_gident_free(gt, e);\n"),
)


def run(source, rounds):
    return run_c([TESTDIR / "gid_table.c", "rdk_cm_gidtab.c"],
                 {"rdk_cm_gidtab.c": source}, includes=[RDMA],
                 args=[str(rounds)])


def main():
    real = (RDMA / "rdk_cm_gidtab.c").read_text(encoding="utf-8")
    try:
        out, mode = run(real, 20000)
    except CTestFailure as error:
        print(error)
        return 1
    print(out, end="")
    for old, new in MUTATIONS:
        if real.count(old) != 1:
            print(f"mutation anchor not found: {old!r}")
            return 1
        try:
            run(real.replace(old, new), 5000)
        except CTestFailure:
            continue
        print(f"a table without {old.strip()!r} passed")
        return 1
    print(f"PASS: GID table ({mode}), {len(MUTATIONS)} mutations caught")
    return 0


if __name__ == "__main__":
    sys.exit(main())
