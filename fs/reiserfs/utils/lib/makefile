OBJS = misc.o io.o  inode.o  version.o reiserfs.o
#hlam.o

MISC = $(TMPBINDIR)/libmisc.a

all: $(MISC)

.c.o:
	$(CC) $(CFLAGS) $<

$(MISC): $(OBJS)
	$(AR) -r $(MISC)  $(OBJS)

clean:
	rm -f *.o $(MISC) *~ TAGS .depend ../include/*~ ../include/TAGS

dep:
	gcc -MM $(IDIRS) *.c > .depend

ifeq (.depend,$(wildcard .depend))
include .depend
endif



