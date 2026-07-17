# ice driver source checks

These tests cover safety properties that can regress without requiring an
E810 device. Run them from anywhere in the source tree with:

```
python3 usr/src/test/ice-tests/rx_checksum.py
```

`rx_checksum.py` verifies that receive checksum metadata is captured before
the descriptor is reposted and that all hardware-reported L3/L4 checksum error
bits suppress checksum validation.
