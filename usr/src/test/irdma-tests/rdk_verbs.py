#!/usr/bin/env python3
"""Run the rdmak QP state table and DMA page walk against bad input."""

import re

from irdma_test import REPO, TESTDIR, function, run_c

RDMA = REPO / "usr/src/uts/common/io/rdma"


def block(text, start):
    """From the line that starts with start to the next line '};'."""
    i = text.index(start)
    return text[i:text.index("\n};\n", i) + 4]


def main():
    hdr = (RDMA / "rdk.h").read_text(encoding="utf-8")
    verbs_path = RDMA / "rdk_verbs.c"
    verbs = verbs_path.read_text(encoding="utf-8")
    parts = [block(hdr, "enum rdk_qp_type {"),
             block(hdr, "enum rdk_qp_attr_mask {"),
             block(hdr, "enum rdk_qp_state {"),
             block(verbs, "static const struct {\n\tboolean_t\tvalid;"),
             function(verbs_path, "rdk_modify_qp_is_ok"),
             function(verbs_path, "rdk_sg_to_pages")]
    run_c(TESTDIR / "rdk_verbs.c", {"rdk_bodies.h": "\n".join(parts)},
          cflags=("-Wno-unused-parameter", "-Wno-missing-field-initializers",
                  "-Wno-pedantic"))

    # Every verb that takes an address resolves its source GID first.
    for name in ("rdk_modify_qp", "rdk_create_ah"):
        text = re.search(rf"^{name}\([\s\S]*?^}}", verbs, re.MULTILINE)
        assert text and "rdk_resolve_ah_attr(" in text.group(), name
    # The framework never hands out an unsafe global rkey.
    alloc = re.search(r"^rdk_alloc_pd\([\s\S]*?^}", verbs, re.MULTILINE)
    assert "if (flags != 0 ||" in alloc.group()
    print("PASS: QP state table, page walk and verb argument checks")


if __name__ == "__main__":
    main()
