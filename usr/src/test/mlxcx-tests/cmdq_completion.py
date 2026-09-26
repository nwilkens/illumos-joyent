#!/usr/bin/env python3
"""A command completion event completes only a command that is really done."""

import cmdq


NAMES = ("idle-slot", "out-of-range", "still-owned", "wrong-token",
         "double-event")

if __name__ == "__main__":
    cmdq.run(__doc__, "cmdq_completion.c", NAMES)
