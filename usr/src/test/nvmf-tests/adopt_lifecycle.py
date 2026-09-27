#!/usr/bin/env python3
"""Run the transport registry with kernel-born qpairs against unload races."""

from common_h import KSHIM
from nvmf_test import (NVMF, NVMF_H, TESTDIR, function, run_c, struct,
                       typedef)


def main():
    core = NVMF / "nvmf_transport.c"
    internal = NVMF / "nvmf_transport_internal.h"
    text = core.read_text(encoding="utf-8")
    start = text.index("struct nvmf_transport {")
    end = text.index("static boolean_t\nnvmf_supported_trtype")
    parts = [KSHIM, typedef(NVMF_H, "nvmf_trtype_t"), """
typedef struct nvlist nvlist_t;
typedef struct msgb mblk_t;
struct nvmf_capsule;
struct nvmf_io_request;
typedef void nvmf_qpair_error_t(void *, int);
typedef void nvmf_capsule_receive_t(void *, struct nvmf_capsule *);
int nvlist_lookup_boolean_value(nvlist_t *, const char *, boolean_t *);
""", struct(internal, "nvmf_transport_ops"),
        struct(internal, "nvmf_qpair"), text[start:end]]
    for name in ("nvmf_supported_trtype", "nvmf_allocate_qpair",
                 "nvmf_adopt_qpair", "nvmf_free_qpair",
                 "nvmf_transport_register", "nvmf_transport_unregister",
                 "nvmf_qpair_error", "nvmf_capsule_received"):
        src = core if name.startswith("nvmf_") and name not in (
            "nvmf_qpair_error", "nvmf_capsule_received") else internal
        parts.append(function(src, name).replace(
            "static inline void\n", "static void\n"))
    run_c(TESTDIR / "adopt_lifecycle.c", {"adopt.h": "\n".join(parts)})


if __name__ == "__main__":
    main()
