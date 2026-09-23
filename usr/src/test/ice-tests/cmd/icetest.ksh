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
# Run the on-system ice(4D) tests.
#
#	icetest [-p peer] [-m mtu] [link]
#
# The link defaults to $ICE_TEST_LINK, then to the first ice link.  The
# default tests need only the device and do not change its configuration
# for long.  With a peer address (-p or $ICE_TEST_PEER) the datapath tests
# also run; they plumb the link, and the peer must already serve "iperf -s".
#

export LC_ALL=C.UTF-8
unalias -a
set -o pipefail

it_arg0=$(basename $0)
it_rundir="$(dirname $0)/../runfiles"
it_runner="/opt/test-runner/bin/run"
it_peer="${ICE_TEST_PEER:-}"
it_mtu="${ICE_TEST_MTU:-1500}"

function fatal
{
	typeset msg="$*"
	[[ -z "$msg" ]] && msg="failed"
	echo "$it_arg0: $msg" >&2
	exit 1
}

while getopts ":p:m:" c; do
	case "$c" in
	p)	it_peer="$OPTARG" ;;
	m)	it_mtu="$OPTARG" ;;
	:)	fatal "option -$OPTARG needs an argument" ;;
	*)	fatal "usage: $it_arg0 [-p peer] [-m mtu] [link]" ;;
	esac
done
shift $((OPTIND - 1))

it_link="${1:-${ICE_TEST_LINK:-}}"
if [[ -z "$it_link" ]]; then
	it_link=$(dladm show-phys -p -o link,device 2>/dev/null |
	    awk -F: '$2 ~ /^ice[0-9]+$/ { print $1; exit }')
fi
[[ -n "$it_link" ]] || fatal "no ice link found; give one or set ICE_TEST_LINK"

it_device=$(dladm show-phys -p -o device "$it_link" 2>/dev/null)
[[ "$it_device" == ice+([0-9]) ]] || fatal "$it_link is not an ice link"

export ICE_TEST_LINK="$it_link"
export ICE_TEST_DEVICE="$it_device"
export ICE_TEST_PEER="$it_peer"
export ICE_TEST_MTU="$it_mtu"

$it_runner -c "$it_rundir/default.run" || it_fail=1
if [[ -n "$it_peer" ]]; then
	$it_runner -c "$it_rundir/datapath.run" || it_fail=1
fi
exit ${it_fail:-0}
