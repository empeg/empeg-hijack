vpath %.c ../..

OBJS1 = prints.o stree.o  buffer.o dir.o file.o inode.o namei.o objectid.o \
        version.o
#journal.o
#bitmap.o


OBJS = do_balan.o fix_node.o ibalance.o lbalance.o teahash3.o prints.o super.o stree.o  buffer.o dir.o file.o inode.o namei.o objectid.o  version.o
#journal.o 
BITMAPOBJ = bitmap.o

REISERFS1 = $(TMPBINDIR)/libreiserfs1.a 
REISERFS2 = $(TMPBINDIR)/libreiserfs2.a
REISERFS = $(TMPBINDIR)/libreiserfs.a
BITMAP = $(TMPBINDIR)/libbitmap.a

all: $(REISERFS1) $(REISERFS2) $(REISERFS) $(BITMAP)

.c.o:
	$(CC) $(CFLAGS) $<

$(REISERFS1): $(OBJS1)
	$(AR) -r $(REISERFS1)  $(OBJS1)
$(REISERFS2): $(OBJS2)
	$(AR) -r $(REISERFS2)  $(OBJS2)

$(REISERFS): $(OBJS)
	$(AR) -r $(REISERFS)  $(OBJS)

$(BITMAP): $(BITMAPOBJ)
	$(AR) -r $(BITMAP)  $(BITMAPOBJ)

clean:
	rm -f *.o $(REISERFS1) $(REISERFS2) *~ TAGS .depend $(REISERFS) $(BITMAP)

dep:
	gcc -MM $(IDIRS) ../../*.c > .depend


ifeq (.depend,$(wildcard .depend))
include .depend
endif

