VPATH = ../bin

OBJS1 = dumpreiserfs.o
OBJS2 = unpack.o

PRFS = $(TMPBINDIR)/dumpreiserfs
UNPACK = $(TMPBINDIR)/unpackreiserfs


all: $(PRFS) $(UNPACK)

.c.o:
	$(CC) $(CFLAGS) $<

$(PRFS): $(OBJS1)  libreiserfs.a libmisc.a libbitmap.a
	$(CC) $(LFLAGS) -o $(PRFS) $(OBJS1) -lmisc -lreiserfs -lbitmap

$(UNPACK): $(OBJS2)  libmisc.a
	$(CC) $(LFLAGS) -o $(UNPACK) $(OBJS2) -lmisc

clean:
	rm -f *.o $(PRFS) $(UNPACK) *~ TAGS .depend

dep:
	gcc -MM $(IDIRS) *.c > .depend

install:
	cp -f $(PRFS) $(SBIN)

uninstall:
	rm -f $(SBIN)/dumpreiserfs


ifeq (.depend,$(wildcard .depend))
include .depend
endif


