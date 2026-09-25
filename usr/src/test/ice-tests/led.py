#!/usr/bin/env python3
"""Execute the LED capability callbacks and check their wiring."""

import argparse
from pathlib import Path

from c_test import DRIVER, TESTDIR, extract, run_c


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DRIVER / "ice_port.c")
    args = parser.parse_args()
    source = args.source.read_text()
    fragments = [extract(source, rf"^(?:int|void)\n{name}\([\s\S]*?^}}",
                         args.source)
                 for name in ("ice_led_set", "ice_led_replay", "ice_led_fini")]
    run_c(TESTDIR / "led.c", {"ice_led_body.h": "\n".join(fragments)})

    gld = (DRIVER / "ice_gld.c").read_text()
    capab = gld[gld.index("case MAC_CAPAB_LED:"):]
    capab = capab[:capab.index("break;")]
    assert "mcl->mcl_flags = 0;" in capab
    assert "mcl->mcl_modes = MAC_LED_DEFAULT | MAC_LED_IDENT;" in capab
    assert "mcl->mcl_set = ice_led_set;" in capab
    lifecycle = (DRIVER / "ice.c").read_text()
    # Replay follows the VSI rebuild; restore follows MAC unregister.
    rebuild = lifecycle[lifecycle.index("\nice_rebuild(ice_t *ice"):]
    assert rebuild.index("ice_vsi_rebuild(ice)") < \
        rebuild.index("ice_led_replay(ice);")
    # The restore rides the admin queue, so it must precede the teardown.
    detach = lifecycle[lifecycle.index("\nice_detach(dev_info_t *dip"):]
    assert detach.index("ice_mac_unregister(ice)") < \
        detach.index("ice_led_fini(ice);") < detach.index("ice_unconfigure(ice)")


if __name__ == "__main__":
    main()
