#!/usr/bin/env python3
"""A UAR index from ALLOC_UAR must name a page inside BAR0."""

import cmdq
from c_test import function


NAMES = ("zero", "past-bar", "last-page", "wrap")


def regs_map(srcdir):
    return function(srcdir / "mlxcx.c", "mlxcx_regs_map")


if __name__ == "__main__":
    cmdq.run(__doc__, "cmdq_uar.c", NAMES, regs_map)
