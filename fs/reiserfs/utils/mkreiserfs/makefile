VPATH = ../bin

OBJS = mkreiserfs.o

MKFS = $(TMPBINDIR)/mkreiserfs

all: $(MKFS)

.c.o:
	$(CC) $(CFLAGS) $<

$(MKFS): $(OBJS) libmisc.a
	$(CC) $(LFLAGS) -o $(MKFS) $(OBJS) -lmisc

clean:
	rm -f *.o $(MKFS) *~ TAGS .depend

dep:
	gcc -MM $(IDIRS) *.c > .depend

install:
	cp -f $(MKFS) $(SBIN)
	if [ -d $(MANDIR) ] ; then cp mkreiserfs.8 $(MANDIR) ; gzip -9 -f $(MANDIR)/mkreiserfs.8 ; fi


uninstall:
	rm -f $(MANDIR)/mkreiserfs.8.gz $(SBIN)/mkreiserfs


ifeq (.depend,$(wildcard .depend))
include .depend
endif









