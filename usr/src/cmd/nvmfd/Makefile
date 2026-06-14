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

PROG=		nvmfd

OBJS=		nvmfd.o		\
		controller.o	\
		discovery.o	\
		io.o		\
		nvmft.o

SRCS=		$(OBJS:%.o=%.c)

include ../Makefile.cmd
include ../Makefile.cmd.64

#
# nvmfd accepts NVMe-oF associations with libnvmf, runs the Fabrics CONNECT
# handshake in userland, and hands the established qpair off to the kernel
# nvmft COMSTAR/STMF port provider.  -lnvmf supplies the Fabrics protocol and
# the handoff packer; -lnvpair carries the packed handoff nvlist; -lsocket and
# -lnsl provide the TCP listener; -lpthread runs one accept thread per qpair.
#
LDLIBS +=	-lnvmf -lsocket -lpthread

#
# daemon(3C) and reallocf(3C) are guarded by __EXTENSIONS__, and the daemon
# is multithreaded (one accept thread per qpair).
#
CPPFLAGS +=	-I$(SRC)/uts/common
CPPFLAGS +=	-I$(SRC)/uts/common/io/nvme
CPPFLAGS +=	-I$(SRC)/lib/libnvmf/common
CPPFLAGS +=	-D__EXTENSIONS__ -D_REENTRANT

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
