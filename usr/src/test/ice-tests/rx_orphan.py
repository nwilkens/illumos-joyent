#!/usr/bin/env python3
"""Exercise setting aside an RX pool whose loans are still up the stack."""
from rx_test import FUNCTIONS, run

if __name__ == "__main__":
    run("rx_orphan", functions=FUNCTIONS + ("ice_rx_start",))
