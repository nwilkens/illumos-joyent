#!/usr/bin/env python3
"""Check that every OpenIB-licensed file of the RDMA stack is covered.

A C file whose header offers the OpenIB.org BSD license (Linux-OpenIB) needs
a THIRDPARTYLICENSE in its directory or a parent directory of the stack, or
one elsewhere in the same driver that names it.
That file must carry the license text and a copyright line for each holder
the file names; it must name the file, or say it covers its whole directory,
or a README.illumos beside either must list the file.  The license needs a
THIRDPARTYLICENSE.descrip beside it and a license action in a package
manifest.
"""

from pathlib import Path
import re
import sys

from cm_test import REPO

IO = REPO / "usr/src/uts/common/io"
ROOTS = (IO / "rdma", IO / "iwcxgbe", IO / "irdma")
MANIFESTS = REPO / "usr/src/pkg/manifests"
OPENIB = re.compile(r"OpenIB\.org BSD license|Linux-OpenIB")
BODY = "Redistributions of source code must retain the above"
HOLDER = re.compile(r"Copyright \(c\)\s+[-0-9, ]+\s+(.*?)\s*(?:All rights "
                    r"reserved\.?)?\s*$", re.M)


def header(text):
    """The first comment block, with the leading stars taken off."""
    m = re.match(r"\s*/\*(.*?)\*/", text, re.S)
    if m is None:
        return ""
    return "\n".join(re.sub(r"^\s*\*?\s?", "", ln) for ln in
                     m.group(1).splitlines())


def holders(text):
    return {h.rstrip(".").strip() for h in HOLDER.findall(text)}


def license_for(path, root):
    """The nearest license up the tree, or else one elsewhere in the
    driver that names the file (irdma keeps its license in core/)."""
    d = path.parent
    while True:
        if (d / "THIRDPARTYLICENSE").exists():
            return d / "THIRDPARTYLICENSE"
        if d == root:
            break
        d = d.parent
    for lic in sorted(root.rglob("THIRDPARTYLICENSE")):
        if listed(path, lic):
            return lic
    return None


def listed(path, lic):
    text = lic.read_text(encoding="utf-8")
    if re.search(rf"\b{re.escape(path.name)}\b", text):
        return True
    if lic.parent == path.parent and "files in this directory" in text:
        return True
    for d in {path.parent, lic.parent}:
        readme = d / "README.illumos"
        if readme.exists() and re.search(
                rf"\b{re.escape(path.name)}\b",
                readme.read_text(encoding="utf-8")):
            return True
    return False


def check(files, manifests):
    bad = []
    used = set()
    for root, path in files:
        text = path.read_text(encoding="utf-8", errors="replace")
        head = header(text)
        if not OPENIB.search(head):
            continue
        lic = license_for(path, root)
        rel = path.relative_to(REPO)
        if lic is None:
            bad.append(f"{rel}: OpenIB header but no THIRDPARTYLICENSE")
            continue
        used.add(lic)
        ltext = lic.read_text(encoding="utf-8")
        if BODY not in ltext:
            bad.append(f"{lic.relative_to(REPO)}: no OpenIB license text")
        if not listed(path, lic):
            bad.append(f"{rel}: not named by {lic.relative_to(REPO)} or a "
                       f"README.illumos")
        for h in holders(head) - holders(ltext):
            if "Edgecast" in h:
                continue
            bad.append(f"{rel}: holder {h!r} missing from "
                       f"{lic.relative_to(REPO)}")
    for lic in sorted(used):
        rel = str(lic.relative_to(REPO))
        if not (lic.parent / "THIRDPARTYLICENSE.descrip").exists():
            bad.append(f"{rel}: no THIRDPARTYLICENSE.descrip")
        if not any(f"license {rel} " in m or f"license={rel}" in m
                   for m in manifests):
            bad.append(f"{rel}: no package manifest license action")
    return used, bad


def sources():
    return [(root, p) for root in ROOTS if root.exists()
            for p in sorted(root.rglob("*.[ch]"))]


def main():
    manifests = [p.read_text(encoding="utf-8")
                 for p in MANIFESTS.glob("*.p5m")]
    files = sources()
    used, bad = check(files, manifests)
    if bad:
        print("\n".join(bad))
        return 1

    # The check must fail without the iwcxgbe manifest action, and for an
    # OpenIB file nothing names.
    iwc = IO / "iwcxgbe/THIRDPARTYLICENSE"
    stripped = [m.replace(str(iwc.relative_to(REPO)), "x") for m in manifests]
    _, found = check(files, stripped)
    if not any("manifest" in f for f in found):
        print("missed a license without a manifest action")
        return 1
    stray = IO / "iwcxgbe/iwc_stray_test.c"
    try:
        stray.write_text((IO / "iwcxgbe/iwc_cq.c").read_text(
            encoding="utf-8"), encoding="utf-8")
        _, found = check(files + [(IO / "iwcxgbe", stray)], manifests)
    finally:
        stray.unlink()
    if not any("iwc_stray_test.c" in f for f in found):
        print("missed an OpenIB file no license names")
        return 1
    print(f"PASS: OpenIB files covered by "
          f"{', '.join(str(u.relative_to(REPO)) for u in sorted(used))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
