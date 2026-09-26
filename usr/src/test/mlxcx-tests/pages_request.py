#!/usr/bin/env python3
"""Page requests survive any count and keep to the total page limit."""

import pages


NAMES = ("int-min", "runtime-limit", "boot-limit", "alloc-fail", "rejected",
         "bad-function")

if __name__ == "__main__":
    pages.run(__doc__, "pages_request.c", NAMES)
