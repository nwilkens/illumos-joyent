#!/usr/bin/env python3
"""An event mode command gives up at its deadline instead of hanging."""

import cmdq


NAMES = ("event-hang", "lost-event", "slot-wait", "dead-firmware",
         "dead-firmware-pages")

if __name__ == "__main__":
    cmdq.run(__doc__, "cmdq_deadline.c", NAMES)
