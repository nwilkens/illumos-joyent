#!/usr/bin/env python3
"""Keep lifecycle policy and queue orchestration behind the MAC adapter."""

import argparse
from pathlib import Path
import re

from c_test import DRIVER, extract


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", type=Path, default=DRIVER)
    args = parser.parse_args()
    private = re.compile(r"\b(?:ice_start_datapath|ice_queues_program|"
                         r"ice_queues_disable|ice_queues_intr_map|"
                         r"ice_queues_intr_dissociate)\b")
    for path in [args.driver / "ice.h", *args.driver.glob("*.c")]:
        if path.name == "ice.c":
            continue
        code = re.sub(r"/\*[\s\S]*?\*/|//[^\n]*", "", path.read_text())
        match = private.search(code)
        assert match is None, f"{path.name} uses private lifecycle operation: {match[0]}"
    path = args.driver / "ice_gld.c"
    for name, call in (("ice_m_start", "return (ice_start(arg));"),
                       ("ice_m_stop", "ice_stop(arg);")):
        body = extract(path.read_text(),
                       rf"^static [\w *]+\n{name}\([\s\S]*?^}}", path)
        assert body[body.index("{") + 1:body.rindex("}")].strip() == call
    print("PASS: lifecycle policy has one owner and MAC submits start/stop intent")


if __name__ == "__main__":
    main()
