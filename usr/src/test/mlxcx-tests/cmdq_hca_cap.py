#!/usr/bin/env python3
"""QUERY_HCA_CAP reports a failed command as a failure."""

import cmdq


NAMES = ("bad-status", "bad-delivery", "timeout", "good")

if __name__ == "__main__":
    cmdq.run(__doc__, "cmdq_hca_cap.c", NAMES)
