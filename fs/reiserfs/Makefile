#
# Makefile for the linux reiser-filesystem routines.
#
# Note! Dependencies are done automagically by 'make dep', which also
# removes any old dependencies. DON'T put your own dependencies here
# unless it's something special (ie not a .c file).
#
# Note 2! The CFLAGS definitions are now in the main makefile...

O_TARGET := reiserfs.o
O_OBJS   := bitmap.o do_balan.o namei.o inode.o file.o dir.o symlink.o fix_node.o preserve.o super.o prints.o objectid.o version.o \
lbalance.o ibalance.o stree.o teahash3.o buffer2.o lock.o journal.o resize.o
M_OBJS   := $(O_TARGET)

include $(TOPDIR)/Rules.make

TAGS:
	etags *.c

