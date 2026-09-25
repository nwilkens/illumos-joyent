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
# Run the datapath acceptance checks against $ICE_TEST_PEER.
#

if [[ -z "${ICE_TEST_LINK:-}" || -z "${ICE_TEST_PEER:-}" ]]; then
	echo "SKIP: ICE_TEST_LINK and ICE_TEST_PEER are not set"
	exit 4
fi

exec /usr/bin/bash "$(dirname $0)/datapath_accept" "$ICE_TEST_PEER" \
    "${ICE_TEST_MTU:-1500}"
