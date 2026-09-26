#!/usr/bin/env python3
"""A timed out command keeps its slot and mailboxes until hardware is done."""

import cmdq


NAMES = ("late-write", "no-alias", "all-abandoned", "late-wrong-token",
         "fini-leak")

if __name__ == "__main__":
    cmdq.run(__doc__, "cmdq_abandon.c", NAMES)
