
.EXPORT_ALL_VARIABLES:

TOPDIR = $(shell pwd)
TMPBINDIR = $(TOPDIR)/bin
INCLUDEDIR = $(TOPDIR)/include
INCLUDEDIR2 = $(TOPDIR)/../../../include/linux

SBIN = /sbin
MANDIR = /usr/man/man8

IDIRS = -I$(INCLUDEDIR) -I$(INCLUDEDIR2) -I-

CFLAGS = -Wall -c -g -O $(IDIRS)
LFLAGS = -L$(TMPBINDIR)
#CFLAGS = -Wall -c -g -pg -O $(IDIRS)
#LFLAGS =  -pg -L$(TMPBINDIR)


#ALL_SUB_DIRS = lib obj mkreiserfs print_disk_layout emu fsck
#ALL_PROGS = mkreiserfs fsck
ALL_SUB_DIRS = lib obj mkreiserfs resize_reiserfs fsck dumpreiserfs emu
ALL_PROGS = mkreiserfs resize_reiserfs fsck dumpreiserfs emu

all:
	mkdir -p bin
	set -e; for i in $(ALL_SUB_DIRS); do $(MAKE) -C $$i ; done

dep:
	set -e; for i in $(ALL_SUB_DIRS); do $(MAKE) -C $$i dep ; done

clean:
	set -e; for i in $(ALL_SUB_DIRS); do $(MAKE) -C $$i clean ; done

install:
	for i in $(ALL_PROGS); do $(MAKE) -C $$i install ; done

uninstall:
	set -e; for i in $(ALL_PROGS); do $(MAKE) -C $$i uninstall ; done

tags:   
	:> TAGS
#	cd mkreiserfs; etags *.[ch] ../lib/*.c ../include/*.h ../../*.c ../../../../include/linux/reiserfs*h
#	cd fsck; etags *.[ch] ../lib/*.c ../include/*.h ../../../../include/linux/reiserfs*h
#	cd print_disk_layout; etags *.[ch] ../lib/*.c ../include/*.h ../../../../include/linux/reiserfs*h
#	cd lib; etags *.[ch] ../lib/*.c ../include/*.h ../../../../include/linux/reiserfs*h
#	cd include; etags *.[ch] ../lib/*.c ../include/*.h ../../../../include/linux/reiserfs*h
#	cd emu; etags *.[ch] ../lib/*.c ../include/*.h ../../../../include/linux/reiserfs*h
#	cd obj; etags ../../*.[ch] ../../../../include/linux/reiserfs*h
	rm -f TAGS; etags ../*.[ch] ../../../include/linux/reiserfs*h include/*.h lib/*.c dumpreiserfs/*.c mkreiserfs/*.c emu/*.c fsck/*.c










