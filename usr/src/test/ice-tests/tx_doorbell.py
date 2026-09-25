#!/usr/bin/env python3

"""Check the ice transmit doorbell and descriptor-sync source invariants."""

from pathlib import Path


REPO = Path(__file__).resolve().parents[4]
TX_SOURCE = REPO / "usr/src/uts/common/io/ice/ice_tx.c"


def function(source: str, signature: str, following: str) -> str:
    start = source.index(signature)
    end = source.index(following, start)
    return source[start:end]


def main() -> None:
    tx = TX_SOURCE.read_text(encoding="utf-8")

    emit = function(
        tx,
        "ice_tx_emit(ice_tx_ring_t *itr,",
        "\nstatic boolean_t\nice_tx_desc_done",
    )
    doorbell = function(
        tx,
        "ice_tx_doorbell(ice_tx_ring_t *itr)\n{",
        "\n}\n",
    )

    # the doorbell writes the tail, and is still FM-checked
    assert "QTX_COMM_DBELL(itr->itxr_index), itr->itxr_tail);" in doorbell
    assert "ice_check_acc_handle(ice, ice->ice_osdep.ios_reg_handle)" in \
        doorbell
    assert "ASSERT(MUTEX_HELD(&itr->itxr_lock));" in doorbell
    assert doorbell.index("itxr_unposted == 0") < doorbell.index("wr32(")

    # emit only counts, and posts once a batch is written
    assert "wr32(" not in emit
    assert emit.index("itr->itxr_tail = tail;") < \
        emit.index("itr->itxr_unposted += ndesc;") < \
        emit.index("ice_tx_doorbell(itr);")
    assert "ICE_TX_DOORBELL_BATCH" in emit

    # every return from the send entry point posts what it wrote
    ring_tx = function(tx, "\nice_ring_tx(void *arg, mblk_t *mp)\n{",
                       "\n}\n")
    tail = ring_tx[ring_tx.rindex("mutex_enter(&itr->itxr_lock);"):]
    assert tail.index("ice_tx_doorbell(itr);") < \
        tail.index("--itr->itxr_tx_active") < tail.index("return (mp);")
    assert ring_tx.count("return (") == 3

    # no per-packet MMIO readback
    assert "ice_flush(" not in emit and "ice_flush(" not in doorbell

    # no whole-ring sync; only the descriptors written are pushed
    assert "ddi_dma_sync(itr->itxr_dma.idb_dma_handle, 0, 0" not in emit
    assert "ice_tx_sync_descs(itr, itr->itxr_tail, written);" in emit
    assert "ice_check_dma_handle(itr->itxr_dma.idb_dma_handle)" in emit

    # the sync happens before itxr_tail advances, so before any doorbell
    sync = emit.index("ice_tx_sync_descs(itr, itr->itxr_tail, written);")
    advance = emit.index("itr->itxr_tail = tail;")
    assert sync < advance

    # the helper covers wrap and uses descriptor-sized offsets
    helper = function(
        tx,
        "ice_tx_sync_descs(ice_tx_ring_t *itr,",
        "\nstatic void\nice_tx_write_desc",
    )
    assert "sizeof (struct ice_tx_desc)" in helper
    assert helper.count("ddi_dma_sync(") == 2
    assert "DDI_DMA_SYNC_FORDEV" in helper
    assert "itr->itxr_size - start" in helper

    # control paths keep their flush
    mapq = function(tx, "ice_map_txq_vector(ice_t *ice,", "\nstatic boolean_t")
    assert "ice_flush(hw);" in mapq

    # the report-status queue probes scattered slots, so the completion sync
    # lives in the probe and covers exactly the one descriptor it reads
    done = function(
        tx,
        "ice_tx_desc_done(const ice_tx_ring_t *itr, uint16_t slot)",
        "\n/*\n * Reclaim descriptors",
    )
    assert "DDI_DMA_SYNC_FORKERNEL" in done
    assert "(off_t)slot * dsz" in done
    assert done.count("ddi_dma_sync(") == 1

    # recycle no longer syncs the whole ring; it walks the report-status queue
    rec = function(tx, "ice_tx_recycle(ice_tx_ring_t *itr, ice_tx_ctrl_block_t **donep,", "\n/*")
    assert "ddi_dma_sync(" not in rec
    assert "itr->itxr_rsq[rs_cidx]" in rec
    assert "ice_check_dma_handle(itr->itxr_dma.idb_dma_handle)" in rec

    print("PASS: ice tx doorbell source invariants")


if __name__ == "__main__":
    main()
