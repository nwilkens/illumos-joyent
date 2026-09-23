#!/bin/bash
#
# ice(4D) on-hardware datapath acceptance suite.
#
# Unlike the source-invariant python checks in this directory, this runs on a
# host with a live ice0 and a physical peer.  It exercises attach state, FMA
# and error counters, bidirectional throughput, ping at the configured MTU,
# and repeated plumb/unplumb, and asserts no counter or fault regressions.
#
# Run on the DUT (e.g. boston) with the peer (e.g. hunter) already running
# "iperf -s" on PEER_IP:
#
#   datapath_accept.sh <peer_ip> [mtu]
#
# ICE_TEST_LINK (default ice0), ICE_TEST_ADDR (default 192.0.2.1/30) and
# ICE_TEST_IPERF (default /opt/tools/bin/iperf) select the link, the local
# address and the iperf binary.  The icetest wrapper sets them.  The kstat
# instance comes from the ice device behind the link, so a renamed link
# works; ICE_TEST_DEVICE, if set, must name that device.
#
# The test replumbs the link and changes its MTU, so it refuses a link that
# has IP configuration.  With ICE_TEST_ALLOW_IP=1 it accepts one whose
# addresses are all temporary static, DHCP or addrconf addresses; it records
# them with the MTU and the default routes over the link, restores them when
# it exits or is interrupted, and checks that the link matches the record.
# All test changes are temporary.
#
# Exit status is 0 only if every check passes.
#
# Copyright 2026 MNX Cloud, Inc.

set -u

PEER_IP="${1:?usage: datapath_accept.sh <peer_ip> [mtu]}"
MTU="${2:-1500}"
LINK="${ICE_TEST_LINK:-ice0}"
ADDR_LOCAL="${ICE_TEST_ADDR:-192.0.2.1/30}"
IPERF="${ICE_TEST_IPERF:-/opt/tools/bin/iperf}"
FAILED=0

msg() { printf '%s %s\n' "$1" "$2"; }
pass() { msg "PASS" "$*"; }
fail() { msg "FAIL" "$*"; FAILED=1; }

DEVICE=$(dladm show-phys -p -o device "$LINK" 2>/dev/null)
if [[ ! "$DEVICE" =~ ^ice[0-9]+$ ]]; then
	fail "$LINK is not an ice link (device '$DEVICE')"
	exit 1
fi
if [[ -n "${ICE_TEST_DEVICE:-}" && "$ICE_TEST_DEVICE" != "$DEVICE" ]]; then
	fail "ICE_TEST_DEVICE is $ICE_TEST_DEVICE but $LINK is $DEVICE"
	exit 1
fi
INST="${DEVICE#ice}"

kv() { kstat -p "ice:$INST:$1" 2>/dev/null | awk '{print $2}'; }
# Sum one statistic over every tx ring.
ringsum() {
	kstat -p "ice:$INST:tx_ring_*:$1" 2>/dev/null |
	    awk '{ s += $2 } END { print s + 0 }'
}

require_zero() {
	# require_zero <kstat-suffix> <label>
	local v
	v=$(kv "$1")
	if [[ -z "$v" ]]; then
		fail "$2: kstat ice:$INST:$1 missing"
	elif [[ "$v" != "0" ]]; then
		fail "$2: ice:$INST:$1 = $v (expected 0)"
	else
		pass "$2 ($1 = 0)"
	fi
}

ORIG_MTU=$(dladm show-linkprop -c -p mtu -o value "$LINK" 2>/dev/null)
if [[ ! "$ORIG_MTU" =~ ^[0-9]+$ ]]; then
	fail "cannot read the mtu of $LINK"
	exit 1
fi

HAD_IF=0
ORIG_ADDRS=()
ORIG_ROUTES=()
ORIG_ROUTES6=()
if ipadm show-if "$LINK" >/dev/null 2>&1; then
	HAD_IF=1
	if [[ "${ICE_TEST_ALLOW_IP:-0}" != 1 ]]; then
		fail "$LINK has IP configuration; set ICE_TEST_ALLOW_IP=1 to" \
		    "let the test replace it and restore it on exit"
		exit 1
	fi
	if [[ "$(ipadm show-if -p -o persistent "$LINK" 2>/dev/null)" == \
	    *[46]* ]]; then
		fail "$LINK has persistent IP configuration, which the test" \
		    "cannot restore"
		exit 1
	fi
	for obj in $(ipadm show-addr -p -o addrobj "$LINK/" 2>/dev/null); do
		type=$(ipadm show-addr -p -o type "$obj" 2>/dev/null)
		addr=$(ipadm show-addr -p -o addr "$obj" 2>/dev/null)
		case "$type" in
		static|dhcp|addrconf) ;;
		*)
			fail "$obj is a $type address, which the test cannot" \
			    "restore"
			exit 1
			;;
		esac
		ORIG_ADDRS+=("$type|$addr")
	done
	ORIG_ROUTES=($(netstat -rn -f inet 2>/dev/null |
	    awk -v l="$LINK" '$1 == "default" && $6 == l { print $2 }'))
	ORIG_ROUTES6=($(netstat -rn -f inet6 2>/dev/null |
	    awk -v l="$LINK" '$1 == "default" && $6 == l { print $2 }'))
	echo "saved $LINK: mtu $ORIG_MTU;" \
	    "addresses ${ORIG_ADDRS[*]+${ORIG_ADDRS[*]}};" \
	    "default routes ${ORIG_ROUTES[*]+${ORIG_ROUTES[*]}}" \
	    "${ORIG_ROUTES6[*]+${ORIG_ROUTES6[*]}}"
fi

has_default() {
	# has_default <inet|inet6> <gateway>
	netstat -rn -f "$1" 2>/dev/null |
	    awk -v g="$2" '$1 == "default" && $2 == g { f = 1 } END { exit !f }'
}

# The link's addresses as sorted "type|address" lines.  A DHCP or addrconf
# address can come back different, so only its type is compared.
addr_state() {
	local obj type addr

	for obj in $(ipadm show-addr -p -o addrobj "$LINK/" 2>/dev/null); do
		type=$(ipadm show-addr -p -o type "$obj" 2>/dev/null)
		addr=$(ipadm show-addr -p -o addr "$obj" 2>/dev/null)
		[[ "$type" == static ]] || addr=""
		echo "$type|$addr"
	done | sort
}

ORIG_STATE=$(addr_state)

# Delete an IP object.  A failure counts only if the object is still there.
remove_ip() {
	# remove_ip <delete-addr|delete-if> <object>
	local show=show-addr

	[[ "$1" == delete-if ]] && show=show-if
	ipadm "$1" "$2" >/dev/null 2>&1 && return 0
	ipadm "$show" "$2" >/dev/null 2>&1 || return 0
	echo "RESTORE FAILED: ipadm $1 $2"
	return 1
}

# Compare the link with the snapshot taken before the test changed it.
verify_restore() {
	local rc=0 mtu state gw

	mtu=$(dladm show-linkprop -c -p mtu -o value "$LINK" 2>/dev/null)
	if [[ "$mtu" != "$ORIG_MTU" ]]; then
		echo "RESTORE FAILED: $LINK mtu is '$mtu', was $ORIG_MTU"
		rc=1
	fi
	if ! (( HAD_IF )); then
		if ipadm show-if "$LINK" >/dev/null 2>&1; then
			echo "RESTORE FAILED: $LINK still has an IP interface"
			rc=1
		fi
		return $rc
	fi
	if ! ipadm show-if "$LINK" >/dev/null 2>&1; then
		echo "RESTORE FAILED: $LINK has no IP interface"
		return 1
	fi
	state=$(addr_state)
	if [[ "$state" != "$ORIG_STATE" ]]; then
		echo "RESTORE FAILED: $LINK addresses are" $state \
		    "but were" $ORIG_STATE
		rc=1
	fi
	for gw in ${ORIG_ROUTES[@]+"${ORIG_ROUTES[@]}"}; do
		has_default inet "$gw" ||
		    { echo "RESTORE FAILED: no default route $gw"; rc=1; }
	done
	for gw in ${ORIG_ROUTES6[@]+"${ORIG_ROUTES6[@]}"}; do
		has_default inet6 "$gw" ||
		    { echo "RESTORE FAILED: no default route $gw"; rc=1; }
	done
	return $rc
}

MODIFIED=0
restore() {
	local rc=0 i=0 entry type addr gw

	(( MODIFIED )) || return 0
	remove_ip delete-addr "$LINK/v4accept" || rc=1
	remove_ip delete-if "$LINK" || rc=1
	if [[ "$(dladm show-linkprop -c -p mtu -o value "$LINK" 2>/dev/null)" \
	    != "$ORIG_MTU" ]] &&
	    ! dladm set-linkprop -t -p mtu="$ORIG_MTU" "$LINK"; then
		echo "RESTORE FAILED: mtu $ORIG_MTU on $LINK"
		rc=1
	fi
	if ! (( HAD_IF )); then
		verify_restore || rc=1
		return $rc
	fi

	if ! ipadm create-if -t "$LINK"; then
		echo "RESTORE FAILED: IP interface $LINK"
		return 1
	fi
	for entry in ${ORIG_ADDRS[@]+"${ORIG_ADDRS[@]}"}; do
		type=${entry%%|*}
		addr=${entry#*|}
		case "$type" in
		static)
			if [[ "$addr" == *"->"* ]]; then
				addr="local=${addr%%->*},remote=${addr#*->}"
			fi
			ipadm create-addr -t -T static -a "$addr" "$LINK/rs$i"
			;;
		*)
			ipadm create-addr -t -T "$type" "$LINK/rs$i"
			;;
		esac || { echo "RESTORE FAILED: $type $addr on $LINK"; rc=1; }
		i=$((i + 1))
	done
	for gw in ${ORIG_ROUTES[@]+"${ORIG_ROUTES[@]}"}; do
		has_default inet "$gw" || route -n add default "$gw" ||
		    { echo "RESTORE FAILED: default route $gw"; rc=1; }
	done
	for gw in ${ORIG_ROUTES6[@]+"${ORIG_ROUTES6[@]}"}; do
		has_default inet6 "$gw" ||
		    route -n add -inet6 default "$gw" ||
		    { echo "RESTORE FAILED: default route $gw"; rc=1; }
	done
	verify_restore || rc=1
	return $rc
}

on_exit() {
	local status=$?

	trap '' INT TERM HUP
	if ! restore; then
		echo "=== $LINK was not fully restored ==="
		status=1
	fi
	exit $status
}
trap on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

echo "=== ice datapath acceptance: peer=$PEER_IP mtu=$MTU ==="

# 1. Driver loaded and bound.
if ! modinfo | grep -qiw ice; then
	fail "ice module not loaded"
	exit 1
fi
pass "ice module loaded"

# 2. Configure MTU (requires the link unplumbed) and plumb the test address.
MODIFIED=1
ipadm delete-if "$LINK" 2>/dev/null
if ! dladm set-linkprop -t -p mtu="$MTU" "$LINK" 2>/dev/null; then
	fail "could not set mtu=$MTU on $LINK"
fi
ipadm create-if -t "$LINK" 2>/dev/null
if ipadm create-addr -t -T static -a "$ADDR_LOCAL" "$LINK/v4accept" \
    2>/dev/null; then
	pass "plumbed $ADDR_LOCAL mtu=$MTU"
else
	fail "could not plumb $ADDR_LOCAL"
fi

# 3. Link state.  dladm show-phys columns: LINK MEDIA STATE SPEED DUPLEX DEVICE.
sleep 2
LSTATE=$(dladm show-phys "$LINK" 2>/dev/null | awk 'NR==2{print $3}')
LSPEED=$(dladm show-phys "$LINK" 2>/dev/null | awk 'NR==2{print $4}')
if [[ "$LSTATE" == "up" ]]; then
	pass "link up ($LSPEED Mbps)"
else
	fail "link state=$LSTATE"
fi

# 4. FMA and hardware fault counters must be clean before traffic.
require_zero "fm:acc_err" "FMA access errors (pre)"
require_zero "fm:dma_err" "FMA dma errors (pre)"
require_zero "fm:erpt_dropped" "FMA ereports dropped (pre)"

# 5. Reachability: small and near-MTU ICMP.
if ping -sn "$PEER_IP" 56 3 >/dev/null 2>&1; then
	pass "ping 56B x3"
else
	fail "ping 56B failed"
fi
# Large ping sized just under the MTU (leave room for IP+ICMP headers).
BIG=$((MTU - 60))
if ping -sn "$PEER_IP" "$BIG" 3 >/dev/null 2>&1; then
	pass "ping ${BIG}B x3 (near-MTU)"
else
	fail "ping ${BIG}B failed (peer MTU mismatch?)"
fi

# 6. Counters before traffic.
RXB0=$(kv "pfstats:rx_bytes"); TXB0=$(kv "pfstats:tx_bytes")
LSO0=$(ringsum tx_lso_packets)

# 7. TX throughput (peer must run iperf -s).  Run this script on both hosts to
# cover both directions; iperf dual/reverse mode is unreliable here.
echo "--- iperf 10s, 4 streams (DUT -> peer) ---"
IOUT=$("$IPERF" -c "$PEER_IP" -t 10 -P 4 2>&1)
echo "$IOUT" | grep -E "SUM|Mbits|Gbits" | tail -2
if echo "$IOUT" | grep -qE "Gbits/sec|Mbits/sec"; then
	pass "iperf throughput completed"
else
	fail "iperf produced no throughput result"
fi

# 8. Counters advanced.
RXB1=$(kv "pfstats:rx_bytes"); TXB1=$(kv "pfstats:tx_bytes")
if (( RXB1 > RXB0 )); then pass "rx_bytes advanced ($RXB0 -> $RXB1)"; else fail "rx_bytes did not advance"; fi
if (( TXB1 > TXB0 )); then pass "tx_bytes advanced ($TXB0 -> $TXB1)"; else fail "tx_bytes did not advance"; fi

# 8b. LSO is on by default: TCP bulk transmit must use it, and no request
# may be refused for its checksum or segmentation metadata.
LSO1=$(ringsum tx_lso_packets)
if (( LSO1 > LSO0 )); then
	pass "tx_lso_packets advanced ($LSO0 -> $LSO1)"
else
	fail "tx_lso_packets did not advance (LSO not in use?)"
fi
for k in tx_lso_badmss tx_lso_badhdr tx_lso_nohck tx_hck_hdrlen \
    tx_hck_nol3 tx_hck_nol4 tx_hck_badl4; do
	v=$(ringsum "$k")
	if [[ "$v" == "0" ]]; then pass "$k = 0"; else fail "$k = $v"; fi
done

# 9. Error and FMA counters still clean after traffic.
for k in mac:ierrors mac:oerrors mac:fcs_errors mac:align_errors \
    mac:macrcv_errors mac:macxmt_errors pfstats:crc_errors \
    fm:acc_err fm:dma_err fm:erpt_dropped; do
	require_zero "$k" "clean after traffic"
done

# 10. Repeated plumb/unplumb; link and FMA must recover each cycle.
echo "--- plumb/unplumb x3 ---"
for i in 1 2 3; do
	ipadm delete-addr "$LINK/v4accept" 2>/dev/null
	ipadm delete-if "$LINK" 2>/dev/null
	ipadm create-if -t "$LINK" 2>/dev/null
	ipadm create-addr -t -T static -a "$ADDR_LOCAL" "$LINK/v4accept" \
	    2>/dev/null
	sleep 2
	ST=$(dladm show-phys "$LINK" 2>/dev/null | awk 'NR==2{print $3}')
	if [[ "$ST" == "up" ]]; then pass "cycle $i: link up"; else fail "cycle $i: link $ST"; fi
done
require_zero "fm:acc_err" "FMA access errors (post-cycle)"
require_zero "fm:dma_err" "FMA dma errors (post-cycle)"

echo "=== $([ $FAILED -eq 0 ] && echo ALL-PASS || echo FAILURES-PRESENT) ==="
exit $FAILED
