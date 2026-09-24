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

UTSBASE = ../..

MODULE		= irdma
OBJECTS		= $(IRDMA_OBJS:%=$(OBJS_DIR)/%) $(IRDMA_CORE_OBJS:%=$(OBJS_DIR)/%)
ROOTMODULE	= $(ROOT_DRV_DIR)/$(MODULE)
CONF_SRCDIR	= $(UTSBASE)/common/io/irdma

include $(UTSBASE)/intel/Makefile.intel

CPPFLAGS	+= -I$(UTSBASE)/common/io/irdma
CPPFLAGS	+= -I$(UTSBASE)/common/io/irdma/core
CPPFLAGS	+= -I$(UTSBASE)/common/io/ice
CPPFLAGS	+= -I$(UTSBASE)/common/io/rdma

LDFLAGS		+= -N misc/rdmak

ALL_TARGET	= $(BINARY)
INSTALL_TARGET	= $(BINARY) $(ROOTMODULE) $(ROOT_CONFFILE)

#
# The core/ files are the Linux irdma shared code; relax the smatch checks
# they do not satisfy on those objects only.
#
$(OBJS_DIR)/ctrl.o		:= SMOFF += all_func_returns,unreachable
$(OBJS_DIR)/hmc.o		:= SMOFF += all_func_returns
$(OBJS_DIR)/pble.o		:= SMOFF += all_func_returns,64bit_shift
$(OBJS_DIR)/uk.o		:= SMOFF += all_func_returns
$(OBJS_DIR)/puda.o		:= SMOFF += all_func_returns
$(OBJS_DIR)/uda.o		:= SMOFF += all_func_returns
$(OBJS_DIR)/ws.o		:= SMOFF += all_func_returns
$(OBJS_DIR)/icrdma_hw.o		:= SMOFF += all_func_returns

.KEEP_STATE:

def:		$(DEF_DEPS)

all:		$(ALL_DEPS)

clean:		$(CLEAN_DEPS)

clobber:	$(CLOBBER_DEPS)

install:	$(INSTALL_DEPS)

include $(UTSBASE)/intel/Makefile.targ
