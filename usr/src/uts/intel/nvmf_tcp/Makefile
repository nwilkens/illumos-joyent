#
# CDDL HEADER START
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
# CDDL HEADER END
#

#
#	Paths to the base of the uts directory trees
#
UTSBASE = ../..

#
#	Define the module and object file sets.
#
#	nvmf_tcp is the NVMe-oF TCP transport.  It is a misc module that
#	registers its transport vtable with the misc/nvmf core.
#
MODULE		= nvmf_tcp
OBJECTS		= $(NVMF_TCP_OBJS:%=$(OBJS_DIR)/%)
ROOTMODULE	= $(ROOT_MISC_DIR)/$(MODULE)

#
#	Include common rules.
#
include $(UTSBASE)/intel/Makefile.intel

#
#	Define targets
#
ALL_TARGET	= $(BINARY)
INSTALL_TARGET	= $(BINARY) $(ROOTMODULE)

#
#	nvmf_tcp depends on the transport core and on the kernel socket module
#	(ksocket_sendmblk / ksocket_krecv_set live in misc/ksocket, not genunix).
#	Adopting a userland socket fd as a ksocket_t uses getsonode(), which lives
#	in fs/sockfs.
#
LDFLAGS		+= -N misc/nvmf -N misc/ksocket -N fs/sockfs

#
#	Default build targets.
#
.KEEP_STATE:

def:		$(DEF_DEPS)

all:		$(ALL_DEPS)

clean:		$(CLEAN_DEPS)

clobber:	$(CLOBBER_DEPS)

install:	$(INSTALL_DEPS)

#
#	Include common targets.
#
include $(UTSBASE)/intel/Makefile.targ
