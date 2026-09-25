#!/usr/bin/env python3
"""Check that filter request construction remains private to its owner."""

from c_test import DRIVER
from filter_boundary import main as boundary


def main():
    boundary()
    source = (DRIVER / "ice_filter.c").read_text()
    assert "static void\nice_fltr_entry_init(" in source
    # filter_requests.py executes callback, attach, replay and removal requests.
    print("PASS: shared filter constructor is private")


if __name__ == "__main__":
    main()
