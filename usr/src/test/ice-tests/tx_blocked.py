#!/usr/bin/env python3

"""Check the ice transmit back-pressure arm/wakeup source invariants."""

from pathlib import Path


REPO = Path(__file__).resolve().parents[4]
TX_SOURCE = REPO / "usr/src/uts/common/io/ice/ice_tx.c"


def function(source: str, signature: str, following: str) -> str:
    start = source.index(signature)
    end = source.index(following, start)
    return source[start:end]


def main() -> None:
    tx = TX_SOURCE.read_text(encoding="utf-8")

    one = function(
        tx,
        "ice_tx_one(ice_tx_ring_t *itr, mblk_t *mp)\n{",
        "\nmblk_t *\nice_ring_tx",
    )
    build = one[one.index("ice_tx_lso_chain(itr, mp"):]
    nores = build[
        build.index("if (res == ICE_TX_BUILD_NORES)"):
        build.index("if (res == ICE_TX_BUILD_DROP)")
    ]

    # blocked is armed under the ring lock ...
    enter = nores.index("mutex_enter(&itr->itxr_lock)")
    arm = nores.index("itr->itxr_blocked = B_TRUE")
    assert enter < arm

    # ... and reclaim is re-driven after arming, before dropping the lock
    recycle = nores.index("ice_tx_recycle(itr, &done, B_TRUE)")
    exit_ = nores.index("mutex_exit(&itr->itxr_lock)")
    assert arm < recycle < exit_

    # the caller still keeps the chain for MAC to retry
    assert "return (B_FALSE);" in nores
    # completed packets are freed once the ring lock is dropped
    assert exit_ < nores.index("ice_tx_done(itr, done)")

    # a drop that returns resources rewakes a ring another sender blocked
    drop = build[build.index("if (res == ICE_TX_BUILD_DROP)"):]
    drop = drop[:drop.index("return (B_TRUE);")]
    assert drop.index("mutex_enter(&itr->itxr_lock)") < \
        drop.index("if (itr->itxr_blocked)") < \
        drop.index("ice_tx_recycle(itr, &done, B_TRUE)") < \
        drop.index("mutex_exit(&itr->itxr_lock)")

    # the TX path never waits on an LSO allocation: MAC start made the pool
    assert "ice_tx_lso_resources" not in tx and "taskq" not in tx
    lso_copy = function(tx, "ice_tx_lso_copy(ice_tx_ring_t *itr,",
                        "\n}\n")
    # a ring without an LSO pool drops rather than blocking for good
    missing = lso_copy[:lso_copy.index("ice_tcb_alloc(itr,")]
    assert "itxr_lso_pool.ibp_nbufs == 0" in missing
    assert "ICE_TX_BUILD_DROP" in missing

    # after a reset every ring is woken by its own handle
    wake = function(tx, "ice_tx_wake(ice_t *ice)\n{", "\n}\n")
    assert "ice->ice_txr[i].itxr_mactxring" in wake
    assert "mac_tx_update(" not in tx

    # the sibling arm path still recycles before arming
    sibling = one[one.index("if (itr->itxr_avail <= ndesc)"):]
    assert sibling.index("ice_tx_recycle(itr, &done, B_TRUE)") < sibling.index(
        "itr->itxr_blocked = B_TRUE")

    # recycle still owns the wakeup in both of its exits
    rec = function(
        tx,
        "ice_tx_recycle(ice_tx_ring_t *itr, ice_tx_ctrl_block_t **donep, boolean_t wake)\n{",
        "\nstatic boolean_t\nice_tx_one",
    )
    assert rec.count("mac_tx_ring_update(") == 2
    assert rec.count("itr->itxr_blocked = B_FALSE") == 2
    assert "ASSERT(MUTEX_HELD(&itr->itxr_lock));" in rec

    # the interrupt wakes MAC only after its TCBs are back, and counts the
    # release as ring activity so a quiesce waits for it
    intr = function(tx, "ice_tx_ring_intr(ice_tx_ring_t *itr)\n{", "\n}\n")
    assert "ice_tx_recycle(itr, &done, B_FALSE)" in intr
    assert intr.index("itr->itxr_tx_active++") < \
        intr.index("ice_tx_done(itr, done)") < \
        intr.index("mac_tx_ring_update(") < \
        intr.index("--itr->itxr_tx_active")
    assert "!itr->itxr_quiesce" in intr

    print("PASS: ice tx back-pressure source invariants")


if __name__ == "__main__":
    main()
