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

LIBRARY =	libnvmf.a
VERS =		.1
OBJECTS =	nvmf_transport.o \
		nvmf_tcp.o \
		nvmf_tcp_data.o \
		nvmf_tcp_pdu.o \
		nvmf_controller.o \
		nvmf_host.o \
		nvmf_host_ioctl.o \
		nvmf_hostid.o \
		nvmf_subr.o \
		nvmf_crc32c.o

include ../../Makefile.lib

SRCDIR =	../common
LIBS =		$(DYNLIB)
CSTD =		$(CSTD_GNU99)

#
# nvme_sqe_t and nvme_cqe_t come from the nvme driver's private nvme_reg.h,
# as they do for the kernel transport.
#
CPPFLAGS +=	-I$(SRC)/uts/common
CPPFLAGS +=	-I$(SRC)/uts/common/io/nvme

LDLIBS +=	-lsocket -lc -lnvpair -luuid -lsmbios

.KEEP_STATE:

all: $(LIBS)

include ../../Makefile.targ
