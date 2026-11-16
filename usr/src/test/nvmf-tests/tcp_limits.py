#!/usr/bin/env python3
"""Check the TCP transport's range and limit arithmetic against a model."""

from common_h import KSHIM
from nvmf_test import NVMF, TESTDIR, function, run_c


def main():
    tcp = NVMF / "nvmf_tcp.c"
    parts = [KSHIM]
    for name in ("nvmf_tcp_range_ok",):
        parts.append(function(tcp, name))
    run_c(TESTDIR / "tcp_limits.c", {"tcp_limits.h": "\n".join(parts)})


if __name__ == "__main__":
    main()
