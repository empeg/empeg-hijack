/*
 * linux/arch/arm/drivers/char/console-empeg.c
 *
 * (C) 1999 Hugo Fiennes <altman@empeg.com>
 *
 * Frame buffer code for empeg-car
 *
 * This is the very basic console mapping code: we only provide a mmap()able
 * area at the moment - there is no linkup with the VT code. Really, this
 * module shouldn't be called console, but it's as near as we get on the
 * empeg to actually having a display.
 *
 * The empeg-car has a 128x32 pixel VFD with a very strange memory layout:
 * this is because it is driven by the LCD driver and some extreme hardware/
 * software hackery. Firstly, the LCD controller is set into its passive
 * monochrome single panel mode, which uses time-based greyscaling (ie, it does
 * PWM on the pixels to give greyshades). Then, we take LCD0 only (the SA1100
 * outputs 4 pixels at a time minimum: for simplicity we just take LCD0 and
 * ignore LCD1-3 completely) and feed this to the VFD circuitry. Due to the
 * way the VFD is scanned, screen memory is a nightmare:
 *
 * - The screen is made up of 64 pairs of vertical lines.
 * - Each of these 64 pairs corresponds to one horizontal LCD scanline
 * - Alternate pixels on each horizontal LCD scanline refer to different VFD
 *   columns.
 * - To prevent adjacent pixel bleed, column 0 and column 127 on the display
 *   are clocked in at the same time, leaving the other 63 vertical strips
 *   to be displayed in a more sensible and obvious order.
 * - 75% of screen memory is unused: not such a big problem on a small screen.
 *   Could probably do some fab & groovy things with the extra screen memory,
 *   but we've got so much CPU time we don't know what to do with it already :)
 * - Due to the low persistence of VFD pixels, we scan at around 200Hz and
 *   the only greyshades out of a 4bpp palette that are actually usable are:
 *   0 (off), 1 (dark), 2 (lighter) and 15 (full on). The other ones are very
 *   flickery and could well be used for majorly-trippy effects.
 *
 * This is further complicated by the fact that the LCD controller in the SA
 * appears not to work exactly as specced: this could be because of the small
 * panel size that has been programmed, but it could be a misreading of the
 * datasheet: basically, to get 64 data lines out of the controller I need to
 * program it as a 65 line display. Screen memory doesn't start where the
 * datasheet says it should, either: I have to skip an additional 63 shorts
 * after the palette data in order to get to the start of the screen. Don't
 * ask me why, I just do.
 *
 * In order that we don't go crazy trying to implement fontplots and so on for
 * the screen, we have two lots of screen memory: the first is a totally
 * logical, 4bpp 128x32 screen buffer, laid out in 4bpp packed pixel format
 * and a sane addressing scheme - this means that things like the fontplot can
 * just do sensible things. Next, we have our private screen buffer which
 * has the "on acid" layout, and we provide a call which does an optimised
 * assembler blat of data from one to the other in order to preserve sanity.
 */

#define NEW_CONSOLE
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/interrupt.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/kd.h>
#include <linux/major.h>
#include <linux/mm.h>
#include <linux/malloc.h>

#include <asm/segment.h>
#include <asm/irq.h>
#include <asm/io.h>

/* We run in a 4bpp mode */
#define EMPEG_SCREEN_BPP       4
#define EMPEG_PALETTE_SIZE     (1<<EMPEG_SCREEN_BPP)

/* Palette styles */
#define PALETTE_BLANK          0
#define PALETTE_STANDARD       1
#define PALETTE_DIRECT         2

/* Screen size */
#define EMPEG_SCREEN_WIDTH     128
#define EMPEG_SCREEN_HEIGHT    32

/* Screen buffer size, if we were using all screen memory */
#define EMPEG_SCREEN_SIZE      (EMPEG_SCREEN_WIDTH * EMPEG_SCREEN_HEIGHT * \
                                EMPEG_SCREEN_BPP / 8)

/* Shadow buffer, rounded up to nearest 4k as mmap() does minimum of 1 page */
#define EMPEG_SHADOW_SIZE      ((EMPEG_SCREEN_SIZE+4095)&~0xfff)

/* The display buffer */
struct empegfb_buffer {
        short palette[EMPEG_PALETTE_SIZE];
        short fudge[63];
           /* Fudge factor for the stuff the LCD controller appears to miss */
        unsigned char buffer[4*EMPEG_SCREEN_SIZE];
           /* Screen buffer, remembering it's 4x bigger than what we actually
              use as LCD1-LCD3 are still being driven */
        short dma_overrun[16];
           /* 32 bytes of DMA overrun space for SA1100 LCD controller */
};

/* ...and the aligned pointer */
static unsigned char *empegfb_start;

/* ...and the structure pointer */
static struct empegfb_buffer *empegfb_bufferstart;

/* The virtual, sensibly-laid-out frame buffer, which is just a 128x32 4bpp
   buffer which all the fb_ stuff can write to before we blat it back to the
   actual framebuffer */
static unsigned char *empegfb_screen;

/* empeg-specific functions for display setup */
static void empeg_setpalette(int type)
{
  /* We only have 4 shades, including black & white. This isn't strictly
     true as the SA1100 provides 14 greyshades on a LCD display, but due to
     the low persistence of the VFD pixels, other shades tend to be very
     very flickery and not very effective. They could be used for trippy
     special effects, I suppose... */

  static short palettes[][16]= {
    /* All-black */
    {  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0 },

    /* Standard palette */
    {  0, 0, 0, 0,  1, 1, 1, 1,  2, 2, 2, 2, 15,15,15,15 },

    /* One-to-one mapping */
    {  0, 1, 2, 3,  4, 5, 6, 7,  8, 9,10,11, 12,13,14,15 } };

  if (type>=PALETTE_BLANK && type<=PALETTE_DIRECT)
    {
      int a;
      for(a=0;a<16;a++) empegfb_bufferstart->palette[a]=palettes[type][a];
    }
}

/* Plot a pixel */
static __inline__ void plot(int x,int y,int p)
  {
  volatile unsigned short *d=(volatile unsigned short*)empegfb_bufferstart->buffer;

  /* Shift display left one pixel, with wrap (the way the VFD
     works!) */
  x--;
  if (x<0) x+=128;

  /* Turn screen the right way up */
  y=31-y;

  /* Find position: lines first (x) */
  d+=(64*(x>>1));
  d+=(x&1);

  /* Now column (y) */
  d+=y*2;

  /* Plot byte */
  *d=(p);
  }

/* Plot a pixel on the actual display */
/* Screen-blatting function to move data from virtual screen buffer to actual
   screen buffer, translating the pixel layout to the one which makes your
   head hurt */
static void empeg_blatbuffer(void)
{
int x,y,p;

/* This is a very inefficient way of doing this - Toby has a much better
   assembler version, but this will do for now */
for(y=0;y<EMPEG_SCREEN_HEIGHT;y++)
  {
    for(x=0;x<EMPEG_SCREEN_WIDTH;x++)
      {
	/* Get pixel from logical screen buffer */
	p=empegfb_screen[(y*(EMPEG_SCREEN_WIDTH/2))+(x>>1)];
	p=(x&1)?(p>>4):(p&0xf);

	/* Plot in wierd buffer */
	plot(x,y,p);
      }
  }
}

static void empeg_splash()
{
}

/*
 * functions to handle /dev/fb
 *
 * This is all we really deal with on the empeg at the moment - we're not
 * actually interested in plain text output as with only a 128x32 screen it
 * gets a little tight: a 16x4 display? Sounds like some old Epson laptop :)
 *
 */
int con_fb_read(char *buf, unsigned long pos, int count)
{
	return -EIO;
}

int con_fb_write(const char *buf, unsigned long pos, int count)
{
	return -EIO;
}

int con_fb_ioctl(uint command, ulong argument)
{
  switch(command)
    {
    case 0: /* Screen blat */
      empeg_blatbuffer();
      break;

    case 1: /* Screen power control */
      if (argument)
	{
	  /* Turning display on */
	  GPSR=EMPEG_DISPLAYPOWER;

	  /* Keep palette 0 until display has warmed up (500ms?) and then
	     turn palette on. */
	}
      else
	{
	  /* Turning display off */
	  GPCR=EMPEG_DISPLAYPOWER;

	  /* Set standby LED mode */
	}
      break;
    
    case 2: /* Standby LED settings */
      break;

    case 3: /* Set screen brightness */
      break;

    case 4: /* Set screen palette */
      empeg_setpalette(argument);
      break;

    default:
      return(-EINVAL);
    }

return 0;
}

/* This is no longer used */

int con_fb_mmap(unsigned long vma_start, unsigned long vma_offset,
			unsigned long vma_end, pgprot_t prot)
{
	if (vma_offset>EMPEG_SHADOW_SIZE ||
	    (vma_end-vma_start+vma_offset)>EMPEG_SHADOW_SIZE)
		return -EINVAL;

	printk("### Mapping physical address %p into virtual address %p length %lu offset %lu.\n",
		   ((int)empegfb_screen), vma_start, vma_end - vma_start, vma_offset);

	if (remap_page_range(vma_start,
			     ((int)empegfb_screen),
			     4096, prot))
	{
		printk("remap_page_range failed.\n");
		return -EAGAIN;
	}
	else
	{
	    /* *((unsigned char *)vma_start) = 0xff; */
	    *(((unsigned char *)empegfb_screen) + 10) = 0xff;
	}
	
	return 0;
}

unsigned long empegfb_vma_nopage(struct vm_area_struct *vma, unsigned long address, int write_access)
{
	unsigned long offset = address - vma->vm_start + vma->vm_offset;
	unsigned long pageptr = (unsigned long)empegfb_screen;

	printk("In nopage.\n");
	if (offset >= PAGE_SIZE)
		return 0;
	
	atomic_inc(&mem_map[MAP_NR(pageptr)].count);
	return pageptr;
}

static struct vm_operations_struct empegfb_vm_ops = {
	NULL, /* open */
	NULL, /* close */
	NULL, /* unmap */
	NULL, /* protect */
	NULL, /* sync */
	NULL, /* advise */
	empegfb_vma_nopage, /* nopage */
};


int con_fb_mmap2(struct vm_area_struct *vma)
{
	/* I think this is required to stop the memory being swapped out */
	vma->vm_flags |= (VM_SHM|VM_LOCKED);

	/* nopage will fill in the map */
	vma->vm_ops = &empegfb_vm_ops;
	return 0;
}

/* Display initialisation */
void console_empeg_init(void)
{
  int size;

  /* Firstly, we need to locate the LCD DMA buffer to a 4k page boundary to
     ensure that the SA1100 DMA controller can do display fetches for the
     LCD. We currently do this by having a container which is 4k bigger than
     required, ensuring we can 4k align the actual buffer within is container
     without much hassle */
  size=(sizeof(struct empegfb_buffer)+4095)&~0xfff;
  empegfb_start=kmalloc(size,GFP_KERNEL);

  /* Point the framebuffer structure at it */
  empegfb_bufferstart=(struct empegfb_buffer*)empegfb_start;

  /* Allocate base of 'sensible' screen memory */
  //empegfb_screen=kmalloc(EMPEG_SHADOW_SIZE,GFP_KERNEL);
#if EMPEG_SHADOW_SIZE <= PAGE_SIZE
  empegfb_screen = (void *)get_free_page(GFP_KERNEL);
#else
  #error Screen size is bigger than a page. FIXME!
#endif

  /* MAC hack. See Linux Device Drivers p283 */
  //mem_map[MAP_NR(empegfb_screen)].flags |= PG_reserved;

  {
	  int x, y;
	  for(y = 0; y < EMPEG_SCREEN_HEIGHT; y++)
	  {
		  for(x = 0; x < EMPEG_SCREEN_WIDTH/2; x++)
		  {
			  *(empegfb_screen + y * (EMPEG_SCREEN_WIDTH/2) + x) = 0;
		  }
	  }
  }
  
  /* Setup normal palette */
  empeg_setpalette(PALETTE_STANDARD);

  /* Set up SA1100 LCD controller: first ensure that it's turned off */
  LCCR0 = 0;

  printk("Frame buffer start is at %p\n", empegfb_start);
  printk("In physical memory that's %p\n", (unsigned char *)virt_to_phys((int)empegfb_start));
  
  /* Set up the DMA controller's base address for the screen */
  DBAR1 = (unsigned char*)virt_to_phys((int)empegfb_start);
  
  /* LCD control register 1; display width */
  LCCR1 = 
    LCCR1_DisWdth(EMPEG_SCREEN_HEIGHT*2*4)+ /* ie height */
    LCCR1_HorSnchWdth(15)+/* Hsync width minimum 15 due to PIC */
    LCCR1_EndLnDel(1)+    /* Minimum start/end line delays */
    LCCR1_BegLnDel(1); 

  /* LCD control register 2; display height */
  LCCR2 =
    LCCR2_DisHght((EMPEG_SCREEN_WIDTH/2)+1)+ /* ie width/2 */
    LCCR2_VrtSnchWdth(1)+ /* Only one non-data lineclock */
    LCCR2_EndFrmDel(0)+   /* No begin/end frame delay */
    LCCR2_BegFrmDel(0);

  /* LCD control register 3; display flags and clockrate */
  LCCR3 =
    LCCR3_PixClkDiv(100)+ /* 1us pixel clock at 220Mhz */
    LCCR3_ACBsDiv(2)+     /* AC bias divisor (from pixel clock) */
    LCCR3_ACBsCntOff+
    LCCR3_VrtSnchH+       /* Vertical sync active high */
    LCCR3_HorSnchH+       /* Horizontal sync active high */
    LCCR3_PixFlEdg;       /* Pixel data valid of palling edge of PCLK */
  
  /* LCD control register 0; flags & enable */
  LCCR0 = 
    LCCR0_LEN+            /* Enable LCD controller */
    LCCR0_Mono+           /* Monochrome mode (ie time-domain greyscaling) */
    LCCR0_Sngl+           /* Single panel mode */
    LCCR0_LDM+            /* No LCD disable done IRQ */
    LCCR0_BAM+            /* No base address update IRQ */
    LCCR0_ERM+            /* No LCD error IRQ */
    LCCR0_Pas+            /* Passive display */
    LCCR0_LtlEnd+         /* Little-endian frame buffer */
    LCCR0_4PixMono+       /* 4-pixels-per-clock mono display */
    LCCR0_DMADel(0);      /* No DMA delay */
  
  /* Turn on display: raising GPIO18 turns on the VN02 high-side driver, which
     feeds the raw car input power to a number of departments totally separate
     from the normal power supplies:

     - The AUXPWR pin on the docking connector is now powered. This is used for
       external amplifier power control (max 1A drain as the input is fused
       inside the empeg through a 3A polyfuse).

     - The display board +12v power input, which drives the 60v switch-mode
       power supply for the display.

     - A MOSFET on the front board which provides +5v from the normal 5v PSU
       to the display filament heater (~250mA) and logic power for the display.

     - The power sense pins on the display board PIC and PEEL which causes
       the PIC to exit powerdown mode (where it controls the standby LED in
       a number of patterns) and enter display scan mode. The powersense also
       causes the PIC/PEEL outputs to the display board to tristate when low
       to ensure we don't backdrive the display through its logic pins.
  */
  GPSR=EMPEG_DISPLAYPOWER;

  /* Set up splash image */
  //empeg_splash();

}
