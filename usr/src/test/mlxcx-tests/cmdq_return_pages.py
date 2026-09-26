#!/usr/bin/env python3
"""RETURN_PAGES never trusts a page count above the one it asked for."""

import cmdq


NAMES = ("too-many", "negative", "exact", "huge-request")

if __name__ == "__main__":
    cmdq.run(__doc__, "cmdq_return_pages.c", NAMES)
