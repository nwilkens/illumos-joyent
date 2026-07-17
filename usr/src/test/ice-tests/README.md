# ice driver source checks

These tests cover safety properties that can regress without requiring an
E810 device. Run them from anywhere in the source tree with:

```
python3 usr/src/test/ice-tests/rx_checksum.py
python3 usr/src/test/ice-tests/admin_interrupt.py
python3 usr/src/test/ice-tests/link_state.py
```

`rx_checksum.py` verifies that receive checksum metadata is captured before
the descriptor is reposted and that all hardware-reported L3/L4 checksum error
bits suppress checksum validation.

`admin_interrupt.py` verifies that every interrupt on the dedicated admin
vector can schedule a bounded, single-flight ARQ drain without depending on an
OICR cause bit, while packet queues remain on separate vectors.

`link_state.py` verifies that the attach-time link state is published only
after successful MAC registration and that publication is serialized with
asynchronous link updates through the link-state lock. It also verifies that
the cache starts at `LINK_STATE_UNKNOWN`, preserving an honest result if the
initial hardware query fails.
