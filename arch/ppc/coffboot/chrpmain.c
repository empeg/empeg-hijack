/*
 * Copyright (C) Paul Mackerras 1997.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */
#include "nonstdio.h"
#include "zlib.h"

extern void *finddevice(const char *);
extern int getprop(void *, const char *, void *, int);
void gunzip(void *, int, unsigned char *, int *);
void stop_imac_ethernet(void);
void stop_imac_usb(void);

#define get_16be(x)	(*(unsigned short *)(x))
#define get_32be(x)	(*(unsigned *)(x))

#define RAM_START	0x00000000
#define RAM_END		(8<<20)

#define PROG_START	0x00010000

char *avail_ram;
char *end_avail;

extern char _end[];
extern char image_data[];
extern int image_len;
extern char initrd_data[];
extern int initrd_len;


boot(int a1, int a2, void *prom)
{
    int ns, oh, i;
    unsigned sa, len;
    void *dst;
    unsigned char *im;
    unsigned initrd_start, initrd_size;
    extern char _start;
    
    printf("chrpboot starting: loaded at 0x%x\n", &_start);
    if (initrd_len) {
	initrd_size = initrd_len;
	initrd_start = (RAM_END - initrd_size) & ~0xFFF;
	a1 = initrd_start;
	a2 = initrd_size;
	claim(initrd_start, RAM_END - initrd_start, 0);
	printf("initial ramdisk moving 0x%x <- 0x%x (%x bytes)\n",
	       initrd_start, initrd_data, initrd_size);
	memcpy((char *)initrd_start, initrd_data, initrd_size);
    }
    im = image_data;
    len = image_len;
    /* claim 3MB starting at PROG_START */
    claim(PROG_START, 3 << 20, 0);
    dst = (void *) PROG_START;
    if (im[0] == 0x1f && im[1] == 0x8b) {
	/* claim 512kB for scratch space */
	avail_ram = (char *) claim(0, 512 << 10, 0x10);
	end_avail = avail_ram + (512 << 10);
	printf("avail_ram = %x\n", avail_ram);
	printf("gunzipping (0x%x <- 0x%x:0x%0x)...", dst, im, im+len);
	gunzip(dst, 3 << 20, im, &len);
	printf("done %u bytes\n", len);
    } else {
	memmove(dst, im, len);
    }

    flush_cache(dst, len);
    stop_imac_ethernet();
    stop_imac_usb();

    sa = (unsigned long)PROG_START;
    printf("start address = 0x%x\n", sa);

    (*(void (*)())sa)(a1, a2, prom);

    printf("returned?\n");

    pause();
}

#define eieio()	asm volatile("eieio");

void stop_imac_ethernet(void)
{
    void *macio, *enet;
    unsigned int macio_addr[5], enet_reg[6];
    int len;
    volatile unsigned int *dbdma;

    macio = finddevice("/pci/mac-io");
    enet = finddevice("/pci/mac-io/ethernet");
    if (macio == NULL || enet == NULL)
	return;
    len = getprop(macio, "assigned-addresses", macio_addr, sizeof(macio_addr));
    if (len != sizeof(macio_addr))
	return;
    len = getprop(enet, "reg", enet_reg, sizeof(enet_reg));
    if (len != sizeof(enet_reg))
	return;
    printf("macio base %x, dma at %x & %x\n",
	   macio_addr[2], enet_reg[2], enet_reg[4]);

    /* hope this is mapped... */
    dbdma = (volatile unsigned int *) (macio_addr[2] + enet_reg[2]);
    *dbdma = 0x80;	/* clear the RUN bit */
    eieio();
    dbdma = (volatile unsigned int *) (macio_addr[2] + enet_reg[4]);
    *dbdma = 0x80;	/* clear the RUN bit */
    eieio();
}

void stop_imac_usb(void)
{
    void *usb;
    unsigned int usb_addr[5];
    int len;
    volatile unsigned int *usb_ctrl;

    usb = finddevice("/pci/usb");
    if (usb == NULL)
	return;
    len = getprop(usb, "assigned-addresses", usb_addr, sizeof(usb_addr));
    if (len != sizeof(usb_addr))
	return;
    printf("usb base %x\n", usb_addr[2]);

    usb_ctrl = (volatile unsigned int *) (usb_addr[2] + 8);
    *usb_ctrl = 0x01000000;	/* cpu_to_le32(1) */
    eieio();
}

void *zalloc(void *x, unsigned items, unsigned size)
{
    void *p = avail_ram;

    size *= items;
    size = (size + 7) & -8;
    avail_ram += size;
    if (avail_ram > end_avail) {
	printf("oops... out of memory\n");
	pause();
    }
    return p;
}

void zfree(void *x, void *addr, unsigned nb)
{
}

#define HEAD_CRC	2
#define EXTRA_FIELD	4
#define ORIG_NAME	8
#define COMMENT		0x10
#define RESERVED	0xe0

#define DEFLATED	8

void gunzip(void *dst, int dstlen, unsigned char *src, int *lenp)
{
    z_stream s;
    int r, i, flags;

    /* skip header */
    i = 10;
    flags = src[3];
    if (src[2] != DEFLATED || (flags & RESERVED) != 0) {
	printf("bad gzipped data\n");
	exit();
    }
    if ((flags & EXTRA_FIELD) != 0)
	i = 12 + src[10] + (src[11] << 8);
    if ((flags & ORIG_NAME) != 0)
	while (src[i++] != 0)
	    ;
    if ((flags & COMMENT) != 0)
	while (src[i++] != 0)
	    ;
    if ((flags & HEAD_CRC) != 0)
	i += 2;
    if (i >= *lenp) {
	printf("gunzip: ran out of data in header\n");
	exit();
    }

    s.zalloc = zalloc;
    s.zfree = zfree;
    r = inflateInit2(&s, -MAX_WBITS);
    if (r != Z_OK) {
	printf("inflateInit2 returned %d\n", r);
	exit();
    }
    s.next_in = src + i;
    s.avail_in = *lenp - i;
    s.next_out = dst;
    s.avail_out = dstlen;
    r = inflate(&s, Z_FINISH);
    if (r != Z_OK && r != Z_STREAM_END) {
	printf("inflate returned %d msg: %s\n", r, s.msg);
	exit();
    }
    *lenp = s.next_out - (unsigned char *) dst;
    inflateEnd(&s);
}
