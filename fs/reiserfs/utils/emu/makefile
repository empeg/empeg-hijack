VPATH = ../bin

OBJS = emu.o

EMU = $(TMPBINDIR)/emu

all: $(EMU)

.c.o:
	$(CC) $(CFLAGS) $<

#$(EMU): $(OBJS) libreiserfs1.a libreiserfs2.a libmisc.a
$(EMU): $(OBJS) libreiserfs.a libmisc.a libbitmap.a
	$(CC) $(LFLAGS) -o $(EMU) $(OBJS) -lmisc -lreiserfs -lbitmap

clean:
	rm -f *.o $(EMU) *~ TAGS .depend

dep:
	gcc -MM $(IDIRS) *.c > .depend


install:

ifeq (.depend,$(wildcard .depend))
include .depend
endif
