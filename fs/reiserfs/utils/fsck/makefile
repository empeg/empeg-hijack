VPATH = ../bin
vpath %.c ../..:../bin

OBJS = main.o pass1.o pass2.o semantic.o pass4.o noname.o ubitmap.o info.o check.o ufile.o ustree.o fix_node.o do_balan.o lbalance.o ibalance.o teahash3.o uobjectid.o segments.o check_tree.o journal.o lost+found.o
#OBJS = main.o


FSCK = $(TMPBINDIR)/reiserfsck

.c.o:
	$(CC)  -DREISERFS_FSCK $(CFLAGS) $<

all: $(FSCK)


$(FSCK): $(OBJS) libmisc.a libreiserfs.a
	$(CC) $(LFLAGS) -o $(FSCK) $(OBJS) -lmisc -lreiserfs

clean:
	rm -f *.o $(FSCK) *~ TAGS .depend

dep:
	gcc -MM $(IDIRS) *.c ../../*.c > .depend

install:
	cp -f $(FSCK) $(SBIN)
	if [ -d $(MANDIR) ] ; then cp reiserfsck.8 $(MANDIR) ; gzip -f -9 $(MANDIR)/reiserfsck.8 ; fi

uninstall:
	rm -f $(MANDIR)/reiserfsck.8.gz $(SBIN)/reiserfsck


ifeq (.depend,$(wildcard .depend))
include .depend
endif









