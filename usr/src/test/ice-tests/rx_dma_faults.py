#!/usr/bin/env python3
"""Inject DMA faults into the production RX walk and delivery entry points."""
from rx_test import FUNCTIONS, run

if __name__ == "__main__":
    run("rx_dma_faults", FUNCTIONS + (
        "ice_ring_rx",
        "ice_ring_rx_poll", "ice_rx_ring_intr",
    ))
