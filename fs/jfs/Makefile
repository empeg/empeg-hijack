#
# Makefile for the linux journaling routines.
#
# Note! Dependencies are done automagically by 'make dep', which also
# removes any old dependencies. DON'T put your own dependencies here
# unless it's something special (ie not a .c file).
#
# Note 2! The CFLAGS definitions are now in the main makefile...

O_TARGET := jfs.o
O_OBJS   := transaction.o commit.o recovery.o checkpoint.o revoke.o

M_OBJS   := $(O_TARGET)

ifeq ($(CONFIG_MODULES),y)
OX_OBJS  += journal.o
else
O_OBJS	 += journal.o
endif

include $(TOPDIR)/Rules.make
