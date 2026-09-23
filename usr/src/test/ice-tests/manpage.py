#!/usr/bin/env python3
"""Check that ice(4D) matches the aliases, properties and defaults."""

from pathlib import Path
import re
import shutil
import subprocess

from c_test import DRIVER, REPO


PAGE = REPO / "usr/src/man/man4d/ice.4d"
MANIFEST = REPO / "usr/src/pkg/manifests/driver-network-ice.p5m"


def section(text, name):
    start = text.index(f".Sh {name}\n")
    end = text.find("\n.Sh ", start + 1)
    return text[start:end if end != -1 else len(text)]


def covered(devices):
    """Device IDs named or spanned by "A through B" in SUPPORTED DEVICES."""
    ids = set()
    for item in re.split(r"\n\.It ", devices)[1:]:
        tokens = re.findall(r"(?:pciex8086,)?([0-9a-f]{4})\b|(through)", item)
        values = []
        for hexid, through in tokens:
            values.append("through" if through else int(hexid, 16))
        for i, value in enumerate(values):
            if value == "through":
                ids.update(range(values[i - 1], values[i + 1] + 1))
            else:
                ids.add(value)
    return ids


def main():
    page = PAGE.read_text()
    aliases = {int(v, 16) for v in re.findall(r"alias=pciex8086,([0-9a-f]+)",
                                              MANIFEST.read_text())}
    devices = section(page, "SUPPORTED DEVICES")
    assert covered(devices) == aliases, (
        sorted(map(hex, covered(devices) ^ aliases)))
    # Only the E810 entry is hardware tested; every other family says so.
    entries = re.split(r"\n\.It ", devices)[1:]
    for entry in entries:
        if entry.startswith("Sy E810"):
            assert "Not validated" not in entry
        else:
            assert "Not validated on hardware." in entry, entry[:20]
    assert "pciex8086,1592" in devices

    # Every ice.conf tunable is documented with the driver's default.
    conf = (DRIVER / "ice.conf").read_text()
    props = section(page, "PROPERTIES")
    for name in re.findall(r"^# (\w+): ", conf, re.MULTILINE):
        assert f".It Sy {name}\n" in props, name
    for name, value in re.findall(r"^# (\w+)=(\d+);", conf, re.MULTILINE):
        block = props[props.index(f".It Sy {name}\n"):]
        block = block[:block.find("\n.It ", 1)]
        assert f".Sy {value} ." in block, (name, value)

    for heading in ("FIRMWARE", "LED", "STATISTICS", "FAULT MANAGEMENT"):
        section(page, heading)
    assert "fw_corrupt" in page and "88 bytes" in page

    mandoc = shutil.which("mandoc")
    if mandoc is not None:
        subprocess.run([mandoc, "-Tlint", "-W", "error", str(PAGE)],
                       check=True)
    print(f"PASS: ice(4D) documents {len(aliases)} device IDs and the "
          f"driver properties")


if __name__ == "__main__":
    main()
