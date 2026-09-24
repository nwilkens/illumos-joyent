#!/bin/bash
#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# http://www.illumos.org/license/CDDL.
#

#
# Copyright 2026 Edgecast Cloud LLC.
#

#
# RDMA verbs acceptance on hardware.  Run on the DUT with ice0 attached
# with rdma_enable=1, irdma, rdmak and rdmat installed, rdmatool and
# irdmactl in the current directory:
#
#	rdma_verbs.sh -i local_ip [-p peer_ip] [-s server_ip] [tests]
#
# tests is a list of test numbers, 1 to 7; the default runs all.
#
#   1	the rdmatool suite between two sessions on this device
#   2	RDMA writes, reads and bandwidth with Ethernet traffic to the peer
#	(-p) running at the same time
#   3	irdma detached (modunload) with a write stream in flight, then
#	attached again
#   4	a PF reset (DEBUG ice _reset) with a write stream in flight
#   5	an interrupt resource management trim with a stream in flight
#   6	the rdmatool suite against "rdmatool server" on another host (-s)
#   7	latency and bandwidth against the server (-s)
#
# Each prints PASS or FAIL with its numbers.  Exit status 0 only if all pass.
#

set -u
LOCAL=
PEER=
SERVER=
while getopts "i:p:s:" c; do
	case $c in
	i) LOCAL=$OPTARG ;;
	p) PEER=$OPTARG ;;
	s) SERVER=$OPTARG ;;
	*) echo "usage: $0 -i local_ip [-p peer_ip] [-s server_ip] [tests]"
	   exit 2 ;;
	esac
done
shift $((OPTIND - 1))
TESTS=" ${*:-1 2 3 4 5 6 7} "
[ -n "$LOCAL" ] || { echo "-i is required"; exit 2; }
TOOL=./rdmatool
CTL=./irdmactl
FAILED=0

pass() { echo "PASS $*"; }
fail() { echo "FAIL $*"; FAILED=1; }
want() { [[ "$TESTS" == *" $1 "* ]]; }
k() { kstat -p "$1" 2>/dev/null | awk '{print $2}'; }
msgs() { wc -l < /var/adm/messages; }
since() { tail -n +$(($1 + 1)) /var/adm/messages; }
dev() { find /devices -name 'irdma@0:irdma' 2>/dev/null | head -1; }
up() {
	local i
	for i in $(seq 1 ${1:-60}); do
		[ "$(k irdma:0:ctl:progress)" = 4095 ] &&
		    [ "$(k ice:0:rdma:state)" = 2 ] && return 0
		sleep 1
	done
	return 1
}

# Start a long write stream in the background; its output goes to $1.
stream() {
	($TOOL -i "$LOCAL" -t 5 loop bw > "$1" 2>&1; echo "rc=$?" >> "$1") &
	sleep 5
}
stream_wait() {
	local i
	for i in $(seq 1 ${2:-180}); do
		grep -q '^rc=' "$1" && return 0
		sleep 1
	done
	pkill -x rdmatool
	return 1
}
# The suite's result line for one test, and whether it passed.
suite() {
	local out=$1
	grep -c '^FAIL' "$out"
}

up 30 || { fail "irdma is not attached before the tests"; exit 1; }
echo "=== RDMA verbs acceptance: $($TOOL info)"

# 1. The suite in loopback.
if want 1; then
out=/tmp/rv1.$$
m=$(msgs)
$TOOL -i "$LOCAL" -t 3 loop > $out 2>&1
rc=$?
grep -E '^(PASS|FAIL)' $out | sed 's/^/  /'
n=$(grep -c '^PASS' $out)
if [ $rc = 0 ] && [ "$(k irdma:0:ctl:verbs_qps)" = 0 ] &&
    [ "$(k irdma:0:ctl:bad_cqes)" = 0 ] &&
    ! since $m | grep -q 'failed to drain'; then
	pass "loopback suite: $n checks, every object freed, no bad CQE"
else
	fail "loopback suite: rc $rc, qps $(k irdma:0:ctl:verbs_qps)," \
	    "bad CQEs $(k irdma:0:ctl:bad_cqes)"
fi
rm -f $out
fi

# 2. RDMA with Ethernet traffic to the peer at the same time.
if want 2; then
if [ -z "$PEER" ]; then
	fail "concurrent Ethernet: no peer (-p)"
else
	out=/tmp/rv2.$$
	ping -s -I 0.01 "$PEER" 1400 1500 > /tmp/rv2p.$$ 2>&1 &
	PP=$!
	$TOOL -i "$LOCAL" -t 3 loop write read bw > $out 2>&1
	rc=$?
	wait $PP
	loss=$(grep -o '[0-9.]*% packet loss' /tmp/rv2p.$$ | cut -d% -f1)
	if [ $rc = 0 ] && [ -n "$loss" ] &&
	    awk -v l="$loss" 'BEGIN { exit !(l < 1) }'; then
		pass "concurrent Ethernet: writes, reads and bandwidth passed" \
		    "with 1500 x 1400 B pings to $PEER, ${loss}% lost;" \
		    "$(grep 'write 1048576' $out | sed 's/.*: //')"
	else
		fail "concurrent Ethernet: rdmatool rc $rc, ping loss" \
		    "${loss:-?}%"
		grep FAIL $out
	fi
	rm -f $out /tmp/rv2p.$$
fi
fi

# 3. Detach with work in flight.
if want 3; then
out=/tmp/rv3.$$
stream $out
q0=$(k ice:0:rdma:quarantine_bufs)
t0=$(date +%s)
id=$(modinfo | awk '$6 == "irdma" {print $1}')
modunload -i "$id"
urc=$?
dt=$(($(date +%s) - t0))
stream_wait $out 60
st=$(k ice:0:rdma:state); bufs=$(k ice:0:rdma:dma_bufs)
devfsadm -i irdma >/dev/null 2>&1
up 30
if [ $urc = 0 ] && [ "$st" = 0 ] && [ "$bufs" = 0 ] &&
    grep -q 'No such device' $out &&
    $TOOL -i "$LOCAL" loop write > /dev/null 2>&1; then
	pass "detach in flight: unloaded in ${dt}s, the stream ended with" \
	    "ENXIO, 0 buffers left, quarantine $q0 ->" \
	    "$(k ice:0:rdma:quarantine_bufs); attached again and a write" \
	    "checked"
else
	fail "detach in flight: modunload $urc state $st bufs $bufs;" \
	    "$(grep -E 'FAIL|rc=' $out | tr '\n' ' ')"
fi
rm -f $out
fi

# 4. A PF reset with work in flight.
if want 4; then
out=/tmp/rv4.$$
f0=$(k ice:0:rdma:quarantine_freed); g0=$(k ice:0:rdma:generation)
m=$(msgs)
stream $out
if dladm set-linkprop -p _reset=1 ice0 2>/dev/null; then
	stream_wait $out 90
	up 90
	f1=$(k ice:0:rdma:quarantine_freed)
	if grep -q 'No such device' $out && [ "$f1" -gt "$f0" ] &&
	    [ "$(k ice:0:rdma:generation)" -gt "$g0" ] &&
	    [ "$(k ice:0:rdma:quarantine_bufs)" = 0 ] &&
	    $TOOL -i "$LOCAL" loop send write > /dev/null 2>&1; then
		pass "reset in flight: the stream ended with ENXIO," \
		    "$((f1 - f0)) buffers held until the reset and then" \
		    "freed, generation $g0 -> $(k ice:0:rdma:generation)," \
		    "send and write checked after"
	else
		fail "reset in flight: freed $f0 -> $f1, quarantine" \
		    "$(k ice:0:rdma:quarantine_bufs);" \
		    "$(grep -E 'FAIL|rc=' $out | tr '\n' ' ')"
	fi
else
	stream_wait $out 120
	fail "reset in flight: _reset needs the DEBUG ice"
fi
rm -f $out
fi

# 5. An interrupt resource management trim with work in flight.
if want 5; then
out=/tmp/rv5.$$
D=$(dev)
m=$(msgs)
stream $out
$CTL "$D" irm-remove 8; r1=$?
sleep 2
$CTL "$D" irm-add 8; r2=$?
stream_wait $out 180
trim=$(since $m | grep -o 'MSI-X vectors: [0-9]* -> [0-9]* .*' | head -1)
if [ $r1 = 0 ] && [ $r2 = 0 ] && grep -q '^rc=0' $out &&
    echo "$trim" | grep -q 'rdma=2'; then
	pass "IRM trim in flight: '$trim', every bandwidth run passed"
elif [ $r1 != 0 ]; then
	fail "IRM trim in flight: irm-remove refused (DEBUG irdma needed)"
else
	fail "IRM trim in flight: '$trim'; $(grep -E 'FAIL|rc=' $out |
	    tr '\n' ' ')"
fi
rm -f $out
fi

# 6 and 7. Against another host.
if want 6 || want 7; then
if [ -z "$SERVER" ]; then
	fail "two hosts: no server (-s)"
else
	out=/tmp/rv6.$$
	list=
	want 6 && list="send write read frwr localinv badkey zerokey bounds"
	want 6 && list="$list access ud inflight"
	want 7 && list="$list pingpong bw"
	$TOOL -i "$LOCAL" -t 5 client "$SERVER" $list > $out 2>&1
	rc=$?
	grep -E '^(PASS|FAIL)' $out | sed 's/^/  /'
	if [ $rc = 0 ]; then
		pass "two hosts: $(grep -c '^PASS' $out) checks against" \
		    "$SERVER; $(tail -1 $out | sed 's/=== //')"
	else
		fail "two hosts: rc $rc; $(tail -1 $out)"
	fi
	rm -f $out
fi
fi

echo "=== irdma: qps $(k irdma:0:ctl:verbs_qps) cqs $(k irdma:0:ctl:verbs_cqs)" \
    "mrs $(k irdma:0:ctl:verbs_mrs), bad CQEs $(k irdma:0:ctl:bad_cqes)," \
    "QP errors $(k irdma:0:ctl:qp_errors), quarantine" \
    "$(k ice:0:rdma:quarantine_bufs) ==="
exit $FAILED
