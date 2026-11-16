#!/usr/bin/env python3
"""Check the TCP transport's range and limit arithmetic against a model."""

from common_h import KSHIM
from nvmf_test import NVMF, TESTDIR, define, function, run_c


def main():
    tcp = NVMF / "nvmf_tcp.c"
    parts = [KSHIM, """
#define	MIN(a, b)	((a) < (b) ? (a) : (b))
#define	MAX(a, b)	((a) > (b) ? (a) : (b))
typedef struct { uint8_t b[72]; } nvmf_tcp_cmd_t;
#define	NVMF_TCP_TERM_REQ_PDU_MAX_SIZE	152
"""]
    for name in ("NVMF_TCP_MIN_MAXH2CDATA", "NVMF_TCP_MIN_PDA",
                 "NVMF_TCP_MAX_PDA"):
        parts.append(define(tcp, name))
    for name in ("nvmf_tcp_range_ok", "nvmf_tcp_params_ok",
                 "nvmf_tcp_rx_limit"):
        parts.append(function(tcp, name))
    run_c(TESTDIR / "tcp_limits.c", {"tcp_limits.h": "\n".join(parts)})


if __name__ == "__main__":
    main()
