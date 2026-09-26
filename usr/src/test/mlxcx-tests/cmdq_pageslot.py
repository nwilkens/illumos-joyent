#!/usr/bin/env python3
"""Page commands have a slot and a taskq that nothing else can hold."""

import re
import sys

import cmdq
from c_test import function, source_parser


NAMES = ("full-queue", "own-slot", "one-slot")


def taskq_check(srcdir):
    body = function(srcdir / "mlxcx_intr.c", "mlxcx_intr_async")
    queues = {}
    for tq, func in re.findall(r"taskq_dispatch_ent\(mlxp->(\w+),\s*"
                               r"(\w+),", body):
        queues[func] = tq
    pages = queues.get("mlxcx_pages_task")
    link = queues.get("mlxcx_link_state_task")
    if pages is None or link is None:
        sys.exit("FAIL: cannot find the page and link taskq dispatches")
    if pages == link:
        sys.exit(f"FAIL: page requests share {pages} with link tasks")


if __name__ == "__main__":
    args, _ = source_parser(__doc__).parse_known_args()
    taskq_check(args.source_dir)
    cmdq.run(__doc__, "cmdq_pageslot.c", NAMES)
