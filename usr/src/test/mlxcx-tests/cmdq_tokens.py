#!/usr/bin/env python3
"""Waiting callers hold no token that a page command needs."""

import cmdq


NAMES = ("waiting-callers", "fresh-tokens")

if __name__ == "__main__":
    cmdq.run(__doc__, "cmdq_tokens.c", NAMES)
