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
#	Path to the base of the uts directory tree (usually /usr/src/uts).
#
UTSBASE	= ../..

ARCHDIR:sh = cd ..; basename `pwd`

#
#	Define the module and object file sets.
#
#	nvmft is the NVMe-oF target: a COMSTAR STMF port provider, modeled on
#	the existing srpt port provider.  It is a leaf driver (modldrv).
#
MODULE		= nvmft
OBJECTS		= $(NVMFT_OBJS:%=$(OBJS_DIR)/%)
ROOTMODULE	= $(ROOT_DRV_DIR)/$(MODULE)
CONF_SRCDIR	= $(UTSBASE)/common/io/comstar/port/nvmft

#
#	Include common rules.
#
include ../Makefile.$(ARCHDIR)

#
#	Define targets
#
ALL_TARGET	= $(BINARY) $(SRC_CONFFILE)
INSTALL_TARGET	= $(BINARY) $(ROOTMODULE) $(ROOT_CONFFILE)

#
#	Bits required for common source.  The port provider pulls in the
#	transport core headers.
#
CPPFLAGS	+= -I$(UTSBASE)/common/io/nvmf

#
#	Overrides and depends_on.  nvmft links against the STMF framework
#	(drv/stmf) and the nvmf transport core misc module.
#
LDFLAGS		+= -N drv/stmf -N misc/nvmf

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
include ../Makefile.targ
