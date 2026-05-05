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

# Copyright 2026 Edgecast Cloud LLC.

include $(SRC)/cmd/Makefile.cmd
include $(SRC)/cmd/Makefile.cmd.64
include $(SRC)/test/Makefile.com

PROG =	snapshot_buf_roundtrip

CLEAN_OBJS =	$(PROG:%=%.o)

ROOTOPTPKG = $(ROOT)/opt/bhyve-tests
TESTDIR = $(ROOTOPTPKG)/tests/migration

CMDS = $(PROG:%=$(TESTDIR)/%)
$(CMDS) := FILEMODE = 0555

CSTD=		$(CSTD_GNU99)
CPPFLAGS =	-I$(COMPAT)/bhyve -I$(CONTRIB)/bhyve \
		-I$(COMPAT)/bhyve/amd64 -I$(CONTRIB)/bhyve/amd64 \
		$(CPPFLAGS.master) \
		-I$(SRC)/uts/intel/io/vmm \
		-I$(SRC)/uts/intel \
		-I$(SRC)/uts/intel/sys
$(PROG) :=	LDLIBS += -lvmmapi

all: $(PROG)

install: all $(CMDS)

clean:
	-$(RM) $(CLEAN_OBJS)
clobber: clean
	-$(RM) $(PROG)

$(CMDS): $(TESTDIR) $(PROG)

$(TESTDIR):
	$(INS.dir)

$(TESTDIR)/%: %
	$(INS.file)

%: %.c
	$(LINK.c) -o $@ $< $(LDLIBS)
	$(POST_PROCESS)
