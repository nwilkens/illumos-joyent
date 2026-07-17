# ice driver source checks

These tests cover safety properties that can regress without requiring an
E810 device. Run them from anywhere in the source tree with:

```
python3 usr/src/test/ice-tests/rx_checksum.py
python3 usr/src/test/ice-tests/admin_interrupt.py
python3 usr/src/test/ice-tests/link_state.py
python3 usr/src/test/ice-tests/fma_dma.py
python3 usr/src/test/ice-tests/dma_lifetime.py
python3 usr/src/test/ice-tests/mac_filter.py
python3 usr/src/test/ice-tests/vsi_tx_vlan.py
python3 usr/src/test/ice-tests/loopback.py
python3 usr/src/test/ice-tests/hw_stats.py
python3 usr/src/test/ice-tests/link_speed_caps.py
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

`fma_dma.py` verifies that the driver advertises and preserves the negotiated
DMA-checking capability, applies `DDI_DMA_FLAGERR` to both datapath and
common-code control-queue allocations, and retains datapath handle checks.

`dma_lifetime.py` verifies that common-code DMA ownership is tracked by an
explicit bound flag rather than physical address zero, while preserving the
common-code-visible `va`/`pa`/`size` structure prefix.

`mac_filter.py` verifies identical MAC filter construction for add and remove.

`vsi_tx_vlan.py` verifies that the PF data VSI admits tagged and untagged Tx.

`loopback.py` verifies the standard netlb ioctl surface, its explicit STREAMS
and strsun dependencies, the `PRIV_SYS_NET_CONFIG` gate on mode changes,
adaptive thread-context serialization around the firmware command,
rollback-safe local-VSI permission and MAC-command ordering, the paired
`ALLOW_LB`/`LOCAL_LB` flags with inverse source-pruning transitions,
common-code VSI cache updates, link-state ordering, detach cleanup,
physical-event override, and absence of loopback branches in the packet
datapath.
`ice_loopback.c` is the small userland controller used for hardware validation.
It sends each netlb command through STREAMS `I_STR`, so the driver receives the
inline payload and exact `ioc_count` it validates. Build it on illumos from the
source root with:

```
gcc -Wall -Wextra -Werror -idirafter usr/src/uts/common \
    -o /tmp/ice_loopback usr/src/test/ice-tests/ice_loopback.c
```

`hw_stats.py` verifies the hardware statistics wiring: both counter refreshes
run under the stat lock, the clear-on-read VSI register is serviced through the
common code, attach captures both hardware baselines before exposing the
kstats, the kstat callbacks reject writes and lock correctly, teardown deletes
the kstats before destroying their lock, attach installs stats before MAC while
detach removes them before unmapping registers, and `ice_m_stat` sources the
MAC counters under the stat lock. Port refreshes are rate-limited so a MAC
kstat snapshot reads the hardware counter bank once, and unsupported MAC
statistics do not trigger register reads. Register-access faults from a
supported MAC statistic report degraded service and return `EIO`, while
private-kstat failures retain the established unaffected-service policy.

`link_speed_caps.py` verifies that PHY setup advertises the full media speed
set with automatic FEC, media insertion reapplies that configuration, and the
cached supported and advertised speed/FEC values reach the GLDv3 statistics
and read-only property callbacks.
