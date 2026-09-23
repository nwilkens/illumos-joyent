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
# Drive the identification LED through MAC: the driver offers only the
# default and ident modes, and a set mode reads back.
#

unalias -a
set -o pipefail

typeset -i fails=0
link="${ICE_TEST_LINK:-}"
dlled=/usr/lib/dl/dlled
if [[ -z "$link" ]]; then
	echo "SKIP: ICE_TEST_LINK is not set"
	exit 4
fi
if [[ ! -x "$dlled" ]]; then
	echo "SKIP: $dlled is not installed"
	exit 4
fi

function fail
{
	echo "FAIL: $*" >&2
	fails+=1
}

# dlled prints a header, then: LINK ACTIVE SUPPORTED.
function field
{
	$dlled "$link" 2>/dev/null | awk -v f=$1 'NR == 2 { print $f }'
}

[[ "$(field 3)" == "default,ident" ]] ||
    fail "supported LED modes: $(field 3)"

$dlled -s ident "$link" || fail "cannot set ident"
[[ "$(field 2)" == "ident" ]] || fail "ident did not read back: $(field 2)"

# The command cannot hold the LED on or off.
$dlled -s on "$link" >/dev/null 2>&1 && fail "LED mode on was accepted"

$dlled -s default "$link" || fail "cannot restore default"
[[ "$(field 2)" == "default" ]] || fail "default did not read back"

(( fails == 0 )) && echo "PASS: $link LED identify"
exit $(( fails != 0 ))
