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
		nvmf_controller.o \
		nvmf_host.o \
		nvmf_subr.o \
		nvmf_crc32c.o

include ../../Makefile.lib

SRCDIR =	../common
LIBS =		$(DYNLIB)
CSTD =		$(CSTD_GNU99)

#
# The library is a userland port of FreeBSD's lib/libnvmf.  It reuses the
# illumos NVMe-over-Fabrics on-wire definitions that the kernel transport and
# nvmft target already carry:
#
#   <sys/nvme.h>		base NVMe identify/status types
#   <sys/nvme/nvmf.h>	Fabrics + NVMe/TCP wire structures
#   <sys/nvme/nvmf_tcp.h>	NVMe/TCP PDU-header validation helper
#
# The concrete generic 64-byte SQE (nvme_sqe_t) / 16-byte CQE (nvme_cqe_t) and
# admin opcodes live in the driver-private io/nvme/nvme_reg.h.  The kernel
# transport (uts/common/io/nvmf) includes that header directly; this library
# does the same so the in-capsule SQE/CQE layout cannot drift from the kernel.
#
CPPFLAGS +=	-I$(SRC)/uts/common
CPPFLAGS +=	-I$(SRC)/uts/common/io/nvme
CPPFLAGS +=	-I$(SRC)/uts/common/io/comstar/port/nvmft

LDLIBS +=	-lsocket -lc -lnvpair -luuid -lsmbios

.KEEP_STATE:

all: $(LIBS)

include ../../Makefile.targ
