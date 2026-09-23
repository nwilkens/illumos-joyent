#!/usr/bin/env python3

"""Check the tx copy-buffer pools use each ring's TCB lock, and its lifetime."""

from pathlib import Path


REPO = Path(__file__).resolve().parents[4]
DRIVER = REPO / "usr/src/uts/common/io/ice"


def function(source: str, signature: str, following: str) -> str:
    start = source.index(signature)
    end = source.index(following, start)
    return source[start:end]


def main() -> None:
    header = (DRIVER / "ice.h").read_text(encoding="utf-8")
    lifecycle = (DRIVER / "ice.c").read_text(encoding="utf-8")
    tx = (DRIVER / "ice_tx.c").read_text(encoding="utf-8")
    dma = (DRIVER / "ice_dma.c").read_text(encoding="utf-8")
    everything = header + lifecycle + tx + dma + \
        (DRIVER / "ice_hw.c").read_text(encoding="utf-8")

    # The shared per-instance pools and their locks are gone.
    for name in ("ice_buf_lock", "ice_small_buf_lock", "ice_copy_pool",
                 "ice_lso_pool", "ice_small_pool"):
        assert name not in everything, name

    # Each ring's pools use its TCB lock, created at MSI-X priority when
    # the ring is allocated.
    ring = function(tx, "ice_tx_ring_alloc(ice_t *ice, ice_tx_ring_t *itr,",
                    "\nboolean_t\nice_tx_rings_alloc")
    init = ring[ring.index("mutex_init(&itr->itxr_tcb_lock,"):]
    assert "DDI_INTR_PRI(ice->ice_intr_pri)" in init[:init.index(";")]
    buf_init = function(dma, "ice_buf_init(ice_t *ice)\n{",
                        "\n/*\n * Release a ring's LSO pool")
    for pool in ("itxr_copy_pool", "itxr_small_pool", "itxr_lso_pool"):
        assert f"itr->{pool}.ibp_lock = &itr->itxr_tcb_lock;" in buf_init
    # Pool construction has exclusive lifecycle ownership.
    assert "mutex_enter(" not in buf_init
    assert "ice_buf_fini(ice)" in buf_init

    # Rings (and their locks) exist before the pools and outlive them.
    attach = lifecycle[lifecycle.index("\nice_attach(dev_info_t *dip"):]
    assert attach.index("ice_tx_rings_alloc(ice)") < \
        attach.index("ice_buf_init(ice)")
    teardown = function(lifecycle, "\nice_unconfigure(ice_t *ice)\n{",
                        "\nvoid\nice_reset_redispatch")
    assert teardown.index("ice_buf_fini(ice)") < \
        teardown.index("ice_tx_rings_free(ice)")

    # The LSO task runs without the ring lock while it allocates.
    task = function(dma, "ice_tx_lso_task(void *arg)\n{",
                    "\n/*\n * Destroying the taskq")
    assert task.index("ice_buf_pool_init(") < \
        task.index("mutex_enter(&itr->itxr_lock)")
    assert task.index("ice_tcb_lso_handles_alloc(") < \
        task.index("mutex_enter(&itr->itxr_lock)")

    print("PASS: ice tx pool lock and lifetime invariants")


if __name__ == "__main__":
    main()
