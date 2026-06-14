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
# Copyright 2026 Edgecast Cloud LLC.
#

#
#	Paths to the base of the uts directory trees
#
UTSBASE = ../..

#
#	Define the module and object file sets.
#
#	nvmf_host is the NVMe-oF host (initiator) driver.  It presents fabric
#	namespaces as blkdev children, so it is a leaf driver (modldrv) rather
#	than a misc module.
#
MODULE		= nvmf_host
OBJECTS		= $(NVMF_HOST_OBJS:%=$(OBJS_DIR)/%)
ROOTMODULE	= $(ROOT_DRV_DIR)/$(MODULE)
CONF_SRCDIR	= $(UTSBASE)/common/io/nvmf_host

#
#	Include common rules.
#
include $(UTSBASE)/intel/Makefile.intel

#
#	Define targets
#
ALL_TARGET	= $(BINARY) $(SRC_CONFFILE)
INSTALL_TARGET	= $(BINARY) $(ROOTMODULE) $(ROOT_CONFFILE)

#
#	Bits required for common source.  The host driver pulls in the
#	transport core headers as well as the shared NVMe field definitions.
#
CPPFLAGS	+= -I$(UTSBASE)/common/io/nvmf
CPPFLAGS	+= -I$(SRC)/common/nvme

#
#	Driver depends on blkdev (block device framework) and the nvmf
#	transport core misc module.
#
LDFLAGS		+= -N drv/blkdev -N misc/nvmf

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
