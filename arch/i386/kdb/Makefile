#
# Makefile for i386-specific kdb files..
#
#  Copyright 1999, Silicon Graphics Inc.
#
#  Written March 1999 by Scott Lurndal at Silicon Graphics, Inc.
#

SUB_DIRS     :=
MOD_SUB_DIRS := $(SUB_DIRS)
ALL_SUB_DIRS := $(SUB_DIRS)

.S.o:
	$(CC) -D__ASSEMBLY__ $(AFLAGS) -traditional -c $< -o $*.o

L_TARGET = kdb.a
L_OBJS  = kdb.o i386-dis.o kd_id.o kdbsupport.o kdb_io.o kdb_bp.o kdb_bt.o

MOD_SUB_DIRS += modules

include $(TOPDIR)/Rules.make
