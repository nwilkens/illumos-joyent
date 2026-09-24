#!/usr/bin/env python3
"""Exercise replacing an RX pool whose loans are still up the stack."""
from rx_test import FUNCTIONS, run

if __name__ == "__main__":
    run("rx_orphan", functions=FUNCTIONS + (
        "ice_rx_start", "ice_rx_orphans_drain", "ice_ring_rx",
        "ice_ring_rx_poll", "ice_rx_quiesce"))
