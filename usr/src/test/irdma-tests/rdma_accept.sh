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
# irdma on-hardware control-plane acceptance.  Run on the DUT with ice0
# attached with rdma_enable=1 and the DEBUG irdma module installed:
#
#	rdma_accept.sh [irdmactl] [peer_ip] [tests]
#
# tests is a list of test numbers, 1 to 6; the default runs all.
#
# Tests: child attach and detach; rollback after an injected failure at each
# bring-up step; CQP command timeout; PF reset with a CQP command in flight;
# modunload with pending work; an interrupt resource management trim.  Each
# prints PASS or FAIL with its numbers.  Exit status 0 only if all pass.
#

set -u
CTL=${1:-/var/tmp/rdma/irdmactl}
PEER=${2:-192.0.2.2}
TESTS=" ${3:-1 2 3 4 5 6} "
CONF=/kernel/drv/irdma.conf
FAILED=0
STEPS=(open dev intr cqp fpm hmc ccq ceq0 aeq pble ws pefltr)
FULL=4095

pass() { echo "PASS $*"; }
fail() { echo "FAIL $*"; FAILED=1; }
k() { kstat -p "$1" 2>/dev/null | awk '{print $2}'; }
irdma_id() { modinfo | awk '$6 == "irdma" {print $1}'; }
dev() { find /devices -name 'irdma@0:irdma' 2>/dev/null | head -1; }
msgs() { wc -l < /var/adm/messages; }
since() { tail -n +$(($1 + 1)) /var/adm/messages; }

detach() {
	local id
	id=$(irdma_id)
	[ -z "$id" ] && return 0
	modunload -i "$id" 2>/dev/null
}

attach() {
	devfsadm -i irdma >/dev/null 2>&1
	[ "$(k ice:0:rdma:state)" = 2 ]
}

wait_attached() {
	local i
	for i in $(seq 1 ${1:-30}); do
		[ "$(k ice:0:rdma:state)" = 2 ] &&
		    [ "$(k irdma:0:ctl:progress)" = $FULL ] && return 0
		sleep 1
	done
	return 1
}

want() { [[ "$TESTS" == *" $1 "* ]]; }

clean_closed() {
	[ "$(k ice:0:rdma:state)" = 0 ] && [ "$(k ice:0:rdma:dma_bufs)" = 0 ] &&
	    [ "$(k ice:0:rdma:qsets)" = 0 ]
}

set_conf() {
	cp /var/tmp/irdma.conf.base "$CONF"
	[ -n "${1:-}" ] && echo "$1" >> "$CONF"
	update_drv irdma >/dev/null 2>&1
}

[ -f /var/tmp/irdma.conf.base ] || cp "$CONF" /var/tmp/irdma.conf.base
set_conf
wait_attached 5 || attach || true
if ! wait_attached 10; then
	fail "irdma is not attached before the tests"
	exit 1
fi
BUFS=$(k ice:0:rdma:dma_bufs)
echo "=== irdma control-plane acceptance: $(k ice:0:rdma:vectors) vectors," \
    "$BUFS DMA buffers, $(k ice:0:rdma:dma_bytes) bytes," \
    "$(k irdma:0:ctl:hmc_sds) SDs, qps $(k irdma:0:ctl:qp_count) ==="

# 1. Attach and detach of the child.
if want 1; then
ok=0; t0=$(date +%s)
for i in 1 2 3 4 5; do
	detach || { fail "attach/detach cycle $i: modunload failed"; break; }
	clean_closed || { fail "attach/detach cycle $i: state" \
	    "$(k ice:0:rdma:state) bufs $(k ice:0:rdma:dma_bufs) qsets" \
	    "$(k ice:0:rdma:qsets) after detach"; break; }
	attach && wait_attached 10 || { fail "attach/detach cycle $i:" \
	    "no attach"; break; }
	[ "$(k ice:0:rdma:dma_bufs)" = "$BUFS" ] &&
	    [ "$(k ice:0:rdma:qsets)" = 1 ] || { fail "attach/detach cycle $i:" \
	    "bufs $(k ice:0:rdma:dma_bufs) qsets $(k ice:0:rdma:qsets)"; break; }
	ok=$i
done
[ $ok = 5 ] && pass "attach/detach: 5 cycles in $(($(date +%s) - t0))s," \
    "each detach left 0 buffers, 0 qsets," \
    "quarantine $(k ice:0:rdma:quarantine_bufs)"
fi

# 2. Rollback after an injected failure at each step.
if want 2; then
n=0
for s in $(seq 1 ${#STEPS[@]}); do
	name=${STEPS[$((s - 1))]}
	detach
	set_conf "fail_step=$s;"
	m=$(msgs)
	attach
	if [ "$(k ice:0:rdma:state)" = 2 ]; then
		fail "rollback at $name: attach succeeded"
		continue
	fi
	if ! since "$m" | grep -q "injected failure at step $name"; then
		fail "rollback at $name: no injection message"
		continue
	fi
	if ! clean_closed || [ "$(k ice:0:rdma:quarantine_bufs)" != 0 ]; then
		fail "rollback at $name: state $(k ice:0:rdma:state) bufs" \
		    "$(k ice:0:rdma:dma_bufs) quarantine" \
		    "$(k ice:0:rdma:quarantine_bufs) qsets $(k ice:0:rdma:qsets)"
		continue
	fi
	n=$((n + 1))
done
set_conf
attach; wait_attached 10
[ $n = ${#STEPS[@]} ] && pass "rollback: $n of ${#STEPS[@]} steps" \
    "(${STEPS[*]}) failed on injection and left 0 buffers, 0 quarantine," \
    "0 qsets; attach after: progress $(k irdma:0:ctl:progress)"
fi

# 3. CQP command timeout: hold completions, run one command.
if want 3; then
r0=$(k ice:0:rdma:reset_requests); g0=$(k ice:0:rdma:generation)
f0=$(k ice:0:rdma:quarantine_freed); m=$(msgs)
D=$(dev)
$CTL "$D" hold && $CTL "$D" probe
sleep 8
wait_attached 60
if since "$m" | grep -q "CQP command timed out" &&
    [ "$(k ice:0:rdma:reset_requests)" -gt "$r0" ] &&
    [ "$(k ice:0:rdma:generation)" -gt "$g0" ] &&
    [ "$(k ice:0:rdma:quarantine_freed)" -gt "$f0" ] &&
    [ "$(k irdma:0:ctl:progress)" = $FULL ]; then
	pass "CQP timeout: reset requested, generation $g0 ->" \
	    "$(k ice:0:rdma:generation), $(( $(k ice:0:rdma:quarantine_freed) \
	    - f0 )) buffers freed after the reset, reattached"
else
	fail "CQP timeout: resets $r0 -> $(k ice:0:rdma:reset_requests)," \
	    "gen $g0 -> $(k ice:0:rdma:generation), freed $f0 ->" \
	    "$(k ice:0:rdma:quarantine_freed), progress" \
	    "$(k irdma:0:ctl:progress)"
fi
fi

# 4. PF reset with a CQP command in flight; DMA kept until the reset is done.
if want 4; then
cat > /var/tmp/rdma_inflight.d <<'EOF'
BEGIN { held = 0; freed = 0; lasthold = 0; reset = 0; firstfree = 0; }
fbt::ice_rdma_op_dma_free:entry /arg2 == 0/ { held++; lasthold = timestamp; }
fbt::ice_reset:return /arg1 == 0 && held > 0 && reset == 0/ {
	reset = timestamp;
}
fbt::ice_check_reset:return /arg1 == 0 && held > 0 && reset == 0/ {
	reset = timestamp;
}
fbt::ice_rdma_buf_free:entry /held > 0/ {
	freed++;
	firstfree = firstfree == 0 ? timestamp : firstfree;
}
tick-1s /freed >= held && held > 0/ {
	printf("held %d freed %d hold_before_reset %d free_after_reset %d\n",
	    held, freed, lasthold < reset, firstfree > reset);
	exit(0);
}
tick-60s { printf("held %d freed %d reset %d\n", held, freed, reset); exit(0); }
EOF
rm -f /var/tmp/rdma_inflight.out
dtrace -q -s /var/tmp/rdma_inflight.d -o /var/tmp/rdma_inflight.out &
DP=$!
sleep 3
D=$(dev)
$CTL "$D" hold && $CTL "$D" probe && $CTL "$D" reset
wait $DP
wait_attached 60
out=$(cat /var/tmp/rdma_inflight.out)
if echo "$out" | grep -q "hold_before_reset 1 free_after_reset 1" &&
    [ "$(k irdma:0:ctl:progress)" = $FULL ]; then
	pass "reset in flight: $out; reattached"
else
	fail "reset in flight: $out; progress $(k irdma:0:ctl:progress)"
fi
fi

# 5. modunload with pending work.
if want 5; then
D=$(dev)
$CTL "$D" hold && $CTL "$D" probe
sleep 1
t0=$(date +%s)
if detach && clean_closed && [ -z "$(irdma_id)" ]; then
	pass "modunload with a held command: unloaded in" \
	    "$(($(date +%s) - t0))s, 0 buffers, quarantine" \
	    "$(k ice:0:rdma:quarantine_bufs)"
else
	fail "modunload with a held command: module $(irdma_id) state" \
	    "$(k ice:0:rdma:state) bufs $(k ice:0:rdma:dma_bufs)"
fi
attach; wait_attached 10
fi

# 6. An interrupt resource management trim with RDMA enabled.
if want 6; then
D=$(dev)
c0=$(k irdma:0:ctl:ceq_intrs); m=$(msgs)
if $CTL "$D" irm-remove 8; then
	after=$(since "$m" | grep -o 'MSI-X vectors: [0-9]* -> [0-9]* .*' |
	    tail -1)
	$CTL "$D" probe && $CTL "$D" wait 10 >/dev/null
	perr=$?
	c1=$(k irdma:0:ctl:ceq_intrs)
	ping -sn "$PEER" 1400 5 >/dev/null 2>&1; pr=$?
	m=$(msgs)
	$CTL "$D" irm-add 8
	back=$(since "$m" | grep -o 'MSI-X vectors: [0-9]* -> [0-9]* .*' |
	    tail -1)
	$CTL "$D" probe && $CTL "$D" wait 10 >/dev/null
	perr2=$?
	if [ $perr = 0 ] && [ $perr2 = 0 ] && [ "$c1" -gt "$c0" ] &&
	    [ $pr = 0 ] && echo "$after" | grep -q 'rdma=2'; then
		pass "IRM trim: '$after', CQP on CEQ 0 after the trim" \
		    "(ceq_intrs $c0 -> $c1), ping ok; offer: '$back'"
	else
		fail "IRM trim: '$after' probe $perr/$perr2 ceq $c0 -> $c1" \
		    "ping $pr; offer '$back'"
	fi
else
	fail "IRM trim: irm-remove refused"
fi
fi

echo "=== state: $(k ice:0:rdma:vectors) vectors, $(k ice:0:rdma:dma_bufs)" \
    "buffers, quarantine $(k ice:0:rdma:quarantine_bufs), freed" \
    "$(k ice:0:rdma:quarantine_freed), resets" \
    "$(k ice:0:rdma:reset_requests) ==="
exit $FAILED
