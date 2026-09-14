#!/usr/bin/env python3

"""Check the tx copy-buffer pool locks are created, destroyed, and ordered."""

from pathlib import Path


REPO = Path(__file__).resolve().parents[4]
HEADER = REPO / "usr/src/uts/common/io/ice/ice.h"
SOURCE = REPO / "usr/src/uts/common/io/ice/ice.c"
DMA_SOURCE = REPO / "usr/src/uts/common/io/ice/ice_dma.c"

LOCKS = ("ice_buf_lock", "ice_small_buf_lock")


def function(source: str, signature: str, following: str) -> str:
    start = source.index(signature)
    end = source.index(following, start)
    return source[start:end]


def main() -> None:
    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")

    inits = {}
    for lock in LOCKS:
        assert "kmutex_t\t\t%s;" % lock in header

        init = "mutex_init(&ice->%s," % lock
        destroy = "mutex_destroy(&ice->%s" % lock
        assert source.count(init) == 1, lock
        assert source.count(destroy) == 1, lock

        idx = source.index(init)
        inits[lock] = idx
        call = source[idx:source.index(";", idx)]
        assert "DDI_INTR_PRI(ice->ice_intr_pri)" in call, lock

    pri = source.index("ddi_intr_get_pri(")
    first_use = source.index("ice_buf_init(ice)")
    for lock in LOCKS:
        assert pri < inits[lock] < first_use, lock

    assert source.index("ice_buf_fini(ice)") < \
        source.index("mutex_destroy(&ice->ice_buf_lock")

    dma = DMA_SOURCE.read_text(encoding="utf-8")
    buf_init = function(
        dma, "ice_buf_init(ice_t *ice)\n{", "\nvoid\nice_buf_fini")
    # Pool construction/destruction have exclusive lifecycle ownership.
    # The actual-C buf_pool test checks every allocator/free boundary.
    assert "mutex_enter(" not in buf_init
    assert "ice_buf_fini(ice)" in buf_init

    print("PASS: ice copy-buffer pool lock lifetime invariants")


if __name__ == "__main__":
    main()
