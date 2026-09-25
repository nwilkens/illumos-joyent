#!/usr/bin/env python3
"""Check that the ice mdb module still matches the driver and the core."""

from pathlib import Path
import re

from c_test import DRIVER, REPO


MODULE = REPO / "usr/src/cmd/mdb/common/modules/ice/ice.c"
MANIFEST = REPO / "usr/src/pkg/manifests/driver-network-ice.p5m"


def defines(text):
    """Map each simple #define to its value text with whitespace removed."""
    found = {}
    for name, value in re.findall(r"^#define\s+(\w+)\s+(.+?)\s*(?:/\*.*)?$",
                                  text, re.MULTILINE):
        found[name] = re.sub(r"\s+", "", value).replace("UL", "ULL") \
            .replace("ULLL", "ULL")
    return found


def main():
    module = MODULE.read_text()
    local = defines(re.sub(r"\\\n", " ", module))
    core = (DRIVER / "core/ice_lan_tx_rx.h").read_text()
    core_defs = defines(re.sub(r"\\\n", " ", core))

    # Descriptor fields copied from the core must keep the core's values.
    for name, value in local.items():
        if name.startswith(("ICE_TXD_", "ICE_RX_FLEX_DESC_PTYPE_M",
                            "ICE_RX_FLX_DESC_PKT_LEN_M")):
            want = core_defs[name].strip("()").replace("ULL", "")
            have = value.strip("()").replace("ULL", "")
            assert want == have, (name, want, have)
    for name in ("ICE_TX_DESC_DTYPE_DATA", "ICE_TX_DESC_DTYPE_CTX",
                 "ICE_TX_DESC_DTYPE_DESC_DONE", "ICE_TX_DESC_CMD_EOP",
                 "ICE_TX_DESC_CMD_RS", "ICE_TX_CTX_DESC_TSO"):
        enum = re.search(rf"\b{name}\s*=\s*(0x[0-9A-Fa-f]+)", core)
        assert int(enum.group(1), 16) == int(local[name], 16), name
    status = re.search(r"enum ice_rx_flex_desc_status_error_0_bits \{"
                       r"([\s\S]*?)\};", core).group(1)
    bits = re.findall(r"(ICE_RX_FLEX_DESC_STATUS0_\w+_S)", status)
    for name in ("ICE_RX_FLEX_DESC_STATUS0_DD_S",
                 "ICE_RX_FLEX_DESC_STATUS0_EOF_S",
                 "ICE_RX_FLEX_DESC_STATUS0_RXE_S"):
        assert bits.index(name) == int(local[name]), name
    assert int(local["ICE_TX_DESC_SIZE"]) == 16
    assert int(local["ICE_RX_DESC_SIZE"]) == 32

    # The state bits mirror ice_state_t.
    header = (DRIVER / "ice.h").read_text()
    state = re.search(r"typedef enum ice_state \{([\s\S]*?)\} ice_state_t;",
                      header).group(1)
    for name, shift in re.findall(r"(ICE_STATE_\w+)\s*=\s*1 << (\d+)", state):
        assert local[name] == f"(1<<{shift})", name
        assert f'"{name[10:]}", {name}, {name}' in module, name

    # mdb_ctf_vread() matches members by name: each must exist in ice.h.
    for mirror, real in (("mdb_ice_t", "ice_t"),
                         ("mdb_ice_tx_ring_t", "ice_tx_ring_t"),
                         ("mdb_ice_rx_ring_t", "ice_rx_ring_t")):
        body = re.search(rf"typedef struct \w+ \{{([^{{}}]*)\}} {mirror};",
                         module).group(1)
        target = re.search(rf"typedef struct \w+ \{{([^{{}}]*)\}} {real};",
                           header).group(1)
        for member in re.findall(r"(\w+);", body):
            assert re.search(rf"\b\*?{member}(?:\[[^\]]*\])?;", target), \
                (real, member)
    assert "ice_state_p" in (DRIVER / "ice.c").read_text()

    for dcmd in ("ice", "ice_txq", "ice_rxq", "ice_tx_ring", "ice_tx_desc",
                 "ice_rx_desc"):
        assert f'{{ "{dcmd}", ' in module, dcmd
    for walker in ("ice", "ice_txq", "ice_rxq"):
        assert f'{{ "{walker}", "walk' in module, walker

    # Build and packaging follow the x86-only module precedent (uhci).
    amd64 = (REPO / "usr/src/cmd/mdb/intel/amd64/Makefile").read_text()
    assert re.search(r"^MODULES \+= .*\bice\b", amd64, re.MULTILINE)
    make = (REPO / "usr/src/cmd/mdb/intel/amd64/ice/Makefile").read_text()
    assert "MODULE = ice.so" in make and "MDBTGT = kvm" in make
    manifest = MANIFEST.read_text()
    assert "file path=kernel/kmdb/$(ARCH64)/ice group=sys mode=0555" \
        in manifest
    assert "file path=usr/lib/mdb/kvm/$(ARCH64)/ice.so group=sys mode=0555" \
        in manifest
    print("PASS: mdb module matches the driver, core and packaging")


if __name__ == "__main__":
    main()
