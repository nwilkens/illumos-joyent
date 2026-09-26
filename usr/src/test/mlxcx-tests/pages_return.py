#!/usr/bin/env python3
"""Unknown or repeated returned pages are skipped without a panic."""

import pages


NAMES = ("teardown-unknown", "teardown-duplicate", "teardown-stuck",
         "take-unknown", "take-empty")

if __name__ == "__main__":
    pages.run(__doc__, "pages_return.c", NAMES)
