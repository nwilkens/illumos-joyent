#!/usr/bin/ksh
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
# Check an attached ice instance: the kstats the driver publishes, including
# the per-ring checksum counters, and clean FMA counters.
#

unalias -a
set -o pipefail

typeset -i fails=0
link="${ICE_TEST_LINK:-}"
dev="${ICE_TEST_DEVICE:-}"
if [[ -z "$link" || -z "$dev" ]]; then
	echo "SKIP: ICE_TEST_LINK and ICE_TEST_DEVICE are not set"
	exit 4
fi
inst="${dev#ice}"

function fail
{
	echo "FAIL: $*" >&2
	fails+=1
}

function stat
{
	kstat -p "ice:$inst:$1" 2>/dev/null | awk '{ print $2 }'
}

modinfo | grep -qw ice || fail "the ice module is not loaded"

for k in pfstats:rx_bytes pfstats:crc_errors vsistats:rx_bytes \
    vsistats:tx_errors; do
	[[ -n "$(stat $k)" ]] || fail "kstat ice:$inst:$k is missing"
done

for k in rx_hck_v4hdr_ok rx_hck_v4hdr_err rx_hck_outer_err rx_hck_l4_ok \
    rx_hck_l4_err rx_hck_v6exthdr rx_hck_nol4 rx_hck_unprocessed \
    rx_hck_unknown; do
	[[ -n "$(stat rx_ring_0:$k)" ]] || fail "rx_ring_0 lacks $k"
done
for k in tx_packets tx_hck_hdrlen tx_hck_nol3 tx_hck_nol4 tx_hck_badl4 \
    tx_lso_nohck tx_lso_badhdr tx_lso_badmss; do
	[[ -n "$(stat tx_ring_0:$k)" ]] || fail "tx_ring_0 lacks $k"
done

for k in fm:acc_err fm:dma_err fm:erpt_dropped; do
	v=$(stat $k)
	if [[ -z "$v" ]]; then
		fail "kstat ice:$inst:$k is missing"
	elif [[ "$v" != "0" ]]; then
		fail "ice:$inst:$k is $v"
	fi
done

(( fails == 0 )) && echo "PASS: $link kstats and FMA counters"
exit $(( fails != 0 ))
