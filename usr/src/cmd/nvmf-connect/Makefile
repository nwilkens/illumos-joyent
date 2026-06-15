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

PROG=		nvmf-connect

OBJS=		nvmf-connect.o
SRCS=		$(OBJS:%.o=%.c)

include ../Makefile.cmd
include ../Makefile.cmd.64

#
# nvmf-connect is the NVMe-oF host (initiator) connect tool.  It runs the
# Fabrics CONNECT handshake with libnvmf and hands the established qpairs to the
# kernel nvmf_host driver.  -lnvmf supplies the Fabrics protocol + the handoff
# packer; -lsocket provides the TCP listener and getaddrinfo(3SOCKET).
#
LDLIBS +=	-lnvmf -lsocket

CPPFLAGS +=	-I$(SRC)/uts/common
CPPFLAGS +=	-I$(SRC)/uts/common/io/nvme
CPPFLAGS +=	-I$(SRC)/lib/libnvmf/common
CPPFLAGS +=	-D__EXTENSIONS__

CSTD=		$(CSTD_GNU99)

.KEEP_STATE:

all: $(PROG)

$(PROG): $(OBJS)
	$(LINK.c) -o $@ $(OBJS) $(LDLIBS)
	$(POST_PROCESS)

install: all $(ROOTUSRSBINPROG)

clean:
	$(RM) $(OBJS)

include ../Makefile.targ
