#!/usr/bin/env python3
"""Pages from a timed out MANAGE_PAGES(GIVE) are kept, not freed."""

import pages


NAMES = ("runtime", "boot")

if __name__ == "__main__":
    pages.run(__doc__, "pages_give_timeout.c", NAMES)
