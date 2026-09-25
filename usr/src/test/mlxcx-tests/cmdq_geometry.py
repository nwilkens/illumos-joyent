#!/usr/bin/env python3
"""Attach refuses a command queue geometry that overruns the queue page."""

import cmdq


NAMES = ("small-stride", "zero-stride", "overflow", "huge-stride", "normal",
         "tight")

if __name__ == "__main__":
    cmdq.run(__doc__, "cmdq_geometry.c", NAMES)
