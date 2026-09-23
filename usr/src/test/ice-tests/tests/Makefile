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


PROGS = diag_ioctl
SCRIPTS = attach led datapath

ROOTOPTDIR = $(ROOT)/opt/ice-tests
ROOTOPTTESTS = $(ROOTOPTDIR)/tests
PROGS64 = $(PROGS:%=%.64)
ROOTOPTPROGS = $(PROGS64:%=$(ROOTOPTTESTS)/%) \
	$(SCRIPTS:%=$(ROOTOPTTESTS)/%) \
	$(ROOTOPTTESTS)/datapath_accept

include $(SRC)/cmd/Makefile.cmd
include $(SRC)/cmd/Makefile.ctf

# The private diagnostic ioctl interface lives with the driver.
CPPFLAGS += -I$(SRC)/uts/common/io/ice -D_REENTRANT
CSTD = $(CSTD_GNU17)
CTF_MODE = link

.KEEP_STATE:

all: $(PROGS64)

clobber: clean
	-$(RM) $(PROGS64)

clean:
	-$(RM) *.o.64

install: $(ROOTOPTTESTS) .WAIT $(ROOTOPTPROGS)

$(ROOTOPTDIR):
	$(INS.dir)

$(ROOTOPTTESTS): $(ROOTOPTDIR)
	$(INS.dir)

$(ROOTOPTTESTS)/%: %
	$(INS.file)

$(ROOTOPTTESTS)/%: %.ksh
	$(INS.rename)

$(ROOTOPTTESTS)/datapath_accept: ../datapath_accept.sh
	$(INS.rename)

%.o.64: %.c
	$(COMPILE64.c) -o $@ -c $<
	$(POST_PROCESS_O)

%.64: %.o.64
	$(LINK64.c) -o $@ $< $(LDLIBS64)
	$(POST_PROCESS)
