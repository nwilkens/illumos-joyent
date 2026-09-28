#!/usr/bin/env python3
"""Fuzz the RDMA transport's CM private data checks, queue sizing and
transfer ranges (rdma_subr.c) under ASan and UBSan, then check that the run
fails once each check is taken out."""

import sys

from common_h import BASE
from nvmf_test import NVMF, TESTDIR, run_c
from rdma_host import _text

PRELUDE = BASE + r"""
#include <stdio.h>
#include <stdlib.h>
#define	bzero(p, n)	memset((p), 0, (n))
#define	MIN(a, b)	((a) < (b) ? (a) : (b))
#define	P2ROUNDUP(x, a)	(-(-(x) & -(a)))
"""

MUTANTS = (
    ("short private data is refused", "pdata", {
        "nvmf_rdma_subr.c": (("p == NULL || len < NVMF_RDMA_REQ_LEN",
                              "p == NULL"),)}),
    ("HRQSIZE covers the queue", "pdata", {
        "nvmf_rdma_subr.c": (("if (req->nrq_hrqsize < entries)\n"
                              "\t\treturn (NVMF_RDMA_REJ_INVALID_HRQSIZE);\n",
                              ""),)}),
    ("the host must allow RDMA READs", "pdata", {
        "nvmf_rdma_subr.c": (("if (peer_ird == 0)\n"
                              "\t\treturn (NVMF_RDMA_REJ_INVALID_IRD);\n",
                              ""),)}),
    ("the admin queue has its own limit", "pdata", {
        "nvmf_rdma_subr.c": (("req->nrq_qid == 0 ?\n"
                              "\t    lim->nrl_admin_entries : "
                              "lim->nrl_io_entries", "lim->nrl_io_entries"),)}),
    ("the CQ fits the device", "sizing", {
        "nvmf_rdma_subr.c": (("max_sq = MIN(dl->ndl_max_qp_wr, "
                              "dl->ndl_max_cqe - sz->nrs_rq);",
                              "max_sq = dl->ndl_max_qp_wr;"),)}),
    ("the send queue holds each transfer's requests", "sizing", {
        "nvmf_rdma_subr.c": (("(max_sq - base_sq) / wrs",
                              "(max_sq - base_sq)"),)}),
    ("a range ends within its total", "range", {
        "nvmf_rdma_subr.c": (("off <= total - len", "off <= total"),)}),
)


def build(replace=None, args=()):
    replace = replace or {}
    text = "\n".join([
        PRELUDE,
        _text(NVMF / "nvmf_rdma_impl.h", replace.get("nvmf_rdma_impl.h",
                                                     ())),
        _text(NVMF / "nvmf_rdma_subr.c", replace.get("nvmf_rdma_subr.c",
                                                     ())),
    ])
    run_c(TESTDIR / "rdma_subr.c", {"subr.h": text}, cases=(args,))


def main():
    build()
    status = 0
    for what, group, replace in MUTANTS:
        try:
            build(replace, (group,))
        except SystemExit as error:
            if "--verbose" in sys.argv:
                print(error)
            print(f"PASS: {what} (a build without it fails)")
            continue
        print(f"FAIL: a build without the check '{what}' passed")
        status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())
