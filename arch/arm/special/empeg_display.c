#undef CONFIG_EMPEG_LCD

/*
 * linux/arch/arm/drivers/char/console-empeg.c
 *
 * (C) 1999/2000 empeg ltd, http://www.empeg.com
 *
 * Authors:
 *   Hugo Fiennes, <hugo@empeg.com>
 *   Mike Crowe, <mac@empeg.com>
 *
 * Frame buffer code for empeg-car
 *
 *            HBF Initial version
 *
 * 1999/02/17 MAC Converted to an independent driver.
 *
 * 1999/02/17 MAC Added cache flushing to make sure the user process's
 *                view of the memory and the kernel's view of the
 *                memory are the same during repeated updates.
 *
 * 1999/03/01 MAC Removed cache flushing and implemented philb's
 *                suggested method to cause the mmapping to occur
 *                uncached.
 *
 * 1999/03/10 MAC Screen queue is now cleared on opening the device so
 *                that everything stays in sync if anything dies
 *                without emptying the queue.
 *
 * 1999/03/17 HBF Optimised screen blat now working
 *
 * 1999/06/30 MAC Added calls to enable_powerfail around turning on
 *                the display so that the dip in voltage that occurs
 *                at this time doesn't cause a powerfail interrupt.
 *                Also don't go into an infinite loop when the power
 *                fails.
 *
 * 1999/07/09 HBF Twiddled powerfail delay to 100ms (was 25ms).
 *
 * 1999/10/11 HBF Display powers on with splash screen as opposed to
 *                blank display now.
 *
 * 1999/10/14 MAC The direct blat to the screen ioctl now goes via the
 *                queue so that if the audio driver goes and causes a
 *                blat of the top thing on the queue it will get the
 *                right thing.
 *
 * 1999/11/13 HBF Twiddled screen blat for CONFIG_EMPEG_NETBOX, to
 *	          rotate screen 180 degrees as the netbox has it glued
 *                in upside-down.
 *
 * 2000/01/08 MAC Added wait queue support so user programs can wait
 *                for a screen buffer to become free.
 *
 * 2000/01/13 MAC The display on/off status is now stored so that
 *                after an erroneous powerfail we can return the
 *                display to its * originally state rather than just
 *                blindly turning it on.
 *
 * 2000/07/22 HBF Widended start/end porches to increase tolerances on
 *                CLKG within BKG (needs 5us either side of rising edge
 *                to be within BKG). Also, the fudge factor suddenly
 *                vanished because of this (probably something to do with
 *                the minimum start porch). Some tidying.
 *
 * 2000/10/27 HBF Display power on/off now uses empeg_displaypower() -
 *                (in the empeg_power driver) as issue 9 boards no longer
 *                use a GPIO to control this.
 *
 * This is the very basic console mapping code: we only provide a mmap()able
 * area at the moment - there is no linkup with the VT code.
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
 * blat of data from one to the other in order to preserve what little
 * sanity I still have left. Pengiun.
 */

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
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/poll.h>
#include <asm/pgtable.h> /* For processor cache handling */

#include <asm/segment.h>
#include <asm/irq.h>
#include <asm/io.h>
#include <asm/arch/empeg.h>
#include <asm/uaccess.h>

#ifdef NO_ANIMATION
/* The Rio logo for the splash screen */
#include "rio_logo.h"
/* The empeg logo for the splash screen: includes tux :) */
#include "empeg_logo.h"
#else
/* Animations for both empeg and rio players. */
#include "empeg_ani.h"
#include "rio_ani.h"

/* Animation speed (in fps) */
#define ANIMATION_FPS		12
#endif

/* No hard disk found image */
#include "nohd_img.h"

inline void donothing(const char *s, ...)
{
}

#define DEBUGK donothing

#define BUFFER_COUNT_ORDER     (3) /* Eight buffers */
#define BUFFER_COUNT	       (1<<BUFFER_COUNT_ORDER)

/* Since we need to parallel the number of audio buffers there are
   actually two less due to DMA stuff. See the audio driver for
   details. */
#define MAX_FREE_BUFFERS	(BUFFER_COUNT - 2)

/* We run in a 4bpp mode */
#define EMPEG_SCREEN_BPP       4
#define EMPEG_PALETTE_SIZE     (1<<EMPEG_SCREEN_BPP)

/* Palette styles */
#define PALETTE_BLANK          0
#define PALETTE_STANDARD       1
#define PALETTE_DIRECT         3

/* Screen size */
#define EMPEG_SCREEN_WIDTH     128
#define EMPEG_SCREEN_HEIGHT    32

/* Screen buffer size, if we were using all screen memory */
#define EMPEG_SCREEN_SIZE      (EMPEG_SCREEN_WIDTH * EMPEG_SCREEN_HEIGHT * \
                                EMPEG_SCREEN_BPP / 8)

/* Shadow buffer, rounded up to nearest 4k as mmap() does minimum of 1 page */
#define EMPEG_SHADOW_SIZE      ((EMPEG_SCREEN_SIZE+4095)&~0xfff)

/* 100ms delay after powering on display before re-enabling powerfails */
#define POWERFAIL_DISABLED_DELAY 100000

/* LCD register configuration */
#define LCCR1_SETUP \
		(LCCR1_DisWdth(EMPEG_SCREEN_HEIGHT*2*4)+ /* 2 v-strips */  \
		LCCR1_HorSnchWdth(23)+ /* Gives ~10us sync */              \
		LCCR1_EndLnDel(2)+     /* Gives around 400ns porch */      \
		LCCR1_BegLnDel(2))

#define LCCR2_SETUP \
		(LCCR2_DisHght((EMPEG_SCREEN_WIDTH/2)+1)+ /* ie width/2 */ \
		LCCR2_VrtSnchWdth(1)+ /* Only one non-data lineclock */    \
		LCCR2_EndFrmDel(0)+   /* No begin/end frame delay */       \
		LCCR2_BegFrmDel(0))
	
#define LCCR3_SETUP \
		(LCCR3_PixClkDiv(100)+ /* 1us pixel clock at 220Mhz */     \
		LCCR3_ACBsDiv(10)+     /* AC bias divisor (from pixclk) */ \
		LCCR3_ACBsCntOff+                                          \
		LCCR3_VrtSnchH+       /* Vertical sync active high */      \
		LCCR3_HorSnchH+       /* Horizontal sync active high */    \
		LCCR3_PixFlEdg)       /* Pixdata on falling edge of PCLK */

#define LCCR0_SETUP \
		(LCCR0_Mono+          /* Monochrome mode */                \
		LCCR0_Sngl+           /* Single panel mode */              \
		LCCR0_LDM+            /* No LCD disable done IRQ */        \
		LCCR0_BAM+            /* No base address update IRQ */     \
		LCCR0_ERM+            /* No LCD error IRQ */               \
		LCCR0_Pas+            /* Passive display */                \
		LCCR0_LtlEnd+         /* Little-endian frame buffer */     \
		LCCR0_4PixMono+       /* 4-pix-per-clock mono display */   \
		LCCR0_DMADel(0))      /* No DMA delay */

/* Display power control (in empeg_power.c) */
extern void empeg_displaypower(int);

/* The display buffer */
struct empegfb_buffer {
        short palette[EMPEG_PALETTE_SIZE];
        unsigned char buffer[(4*EMPEG_SCREEN_SIZE)+128];
           /* Screen buffer, remembering it's 4x bigger than what we actually
              use as LCD1-LCD3 are still being driven */
        short dma_overrun[16];
           /* 32 bytes of DMA overrun space for SA1100 LCD controller */
};

/* We only support one device */
struct display_dev
{
	/* The aligned pointer to the actual frame buffer */
	unsigned char *hardware_buffer;

	/* The structure used to encapsulate the LCD controller */
	struct empegfb_buffer *bufferstart;

	/* The nice regular flat screen image */
	unsigned char *software_buffer;

	unsigned char *software_queue;

	/* Use a wait queue so that user mode programs can use
	   poll to be alterted when there is space in the queue. */
	struct wait_queue *wq;
	
	int queue_rp;
	int queue_wp;
	int queue_used;
	int queue_free;

	unsigned power : 1;
};

static struct display_dev devices[1];

/* Timer to display boot animation */
static struct timer_list animation_timer;

/* Timer to display user's splash screen */
static struct timer_list display_timer;

#ifdef CONFIG_EMPEG_LCD
static volatile unsigned char
        *lcd_command0=(volatile unsigned char*)0xe0000000,
	*lcd_command1=(volatile unsigned char*)0xe0000008;
static volatile unsigned char
        *lcd_data0=(volatile unsigned char*)0xe0000004,
	*lcd_data1=(volatile unsigned char*)0xe000000c;
#endif

/* Prettier cache flushing function. */
static void display_flush_cache(unsigned long start, unsigned long len)
{
	processor.u.armv3v4._flush_cache_area(start, start + len, 0);
}

/* empeg-specific functions for display setup */
static void display_setpalette(struct display_dev *dev, int type)
{
	/* We only have 4 shades, including black & white. This isn't
	   strictly true as the SA1100 provides 14 greyshades on a LCD
	   display, but due to the low persistence of the VFD pixels,
	   other shades tend to be very very flickery and not very
	   effective. They could be used for trippy special effects, I
	   suppose... */
	
	static short palettes[][16]= {
		/* All-black */
		{  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0 },
		
		/* Special palette to support Toby's code */
		{  0, 1, 2, 15, 0, 1, 2, 15, 0, 1, 2, 15, 0, 1, 2, 15 },

		/* Standard palette */
		{  0, 0, 0, 0,  1, 1, 1, 1,  2, 2, 2, 2, 15,15,15,15 },
		
		/* One-to-one mapping */
		{  0, 1, 2, 3,  4, 5, 6, 7,  8, 9,10,11, 12,13,14,15 },

	};
	
	if (type>=PALETTE_BLANK && type<=PALETTE_DIRECT)
	{
		int a;
		for(a=0;a<16;a++)
			dev->bufferstart->palette[a]=palettes[type][a];
	}
}

/* Plot a pixel */
static __inline__ void display_plot(struct display_dev *dev, int x,int y,int p)
{
	volatile unsigned short *d=(volatile unsigned short*)dev->bufferstart->buffer;
	
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

#ifdef CONFIG_EMPEG_LCD
/* Put 122x32 screen image to LCD device */
static void lcd(unsigned char *source_buffer)
{
	/* LCD on IDE port support */
	int x,y,p,t0,t1,w;

	/* 4 strips */
	for(y=0;y<32;y+=8) {
		/* Set row */
		*lcd_command0=0xb8|(3-(y>>3));
		*lcd_command1=0xb8|(3-(y>>3));
		udelay(1);
		*lcd_command0=0x00;
		*lcd_command1=0x00;
		udelay(1);
		for(x=0;x<61;x++) {
			/* Build word out of 8 vertical pixels */
			t0=t1=0;
			for(w=0;w<8;w++) {
				p=(source_buffer[((y+w)*(EMPEG_SCREEN_WIDTH/2))+(x>>1)]);
				t0|=((((x&1)?(p>>4):(p&0xf))>1)?1:0)<<(7-w);
				p=(source_buffer[((y+w)*(EMPEG_SCREEN_WIDTH/2))+((x+61)>>1)]);
				t1|=((((x&1)?(p&0xf):(p>>4))>1)?1:0)<<(7-w);
			}

			/* Send it to display */
			*lcd_data0=t0;
			*lcd_data1=t1;
			udelay(1);
		}
	}
}
#endif

/* Do a direct refresh straight to the screen */

/* Plot a pixel on the actual display */
/* Screen-blatting function to move data from virtual screen buffer to actual
   screen buffer, translating the pixel layout to the one which makes your
   head hurt */
static void display_blat(struct display_dev *dev, unsigned char *source_buffer)
{
#ifdef CONFIG_EMPEG_DISPLAY_INVERTED
	int x,y,p;

	/* This is a very inefficient way of doing this - Toby has a
	   much better assembler version, but this will do for now */
	for(y=0;y<EMPEG_SCREEN_HEIGHT;y++) {
		for(x=0;x<EMPEG_SCREEN_WIDTH;x++) {
			/* Get pixel from logical screen buffer */
			p=source_buffer[(y*(EMPEG_SCREEN_WIDTH/2))+(x>>1)];
			p=(x&1)?(p>>4):(p&0xf);
			
			/* Plot in wierd buffer */
			display_plot(dev, 127-x, 31-y, p);
		}
	}
#else
	/* Instead of a pixel-eye's view of the display, with logical x-y
	   mappings taken from the actual logical screen layout, we deal with
	   the screen in the destination pixel order, finding pixels from the
	   logical buffer and placing them in the output buffer */
	unsigned short *d=(unsigned short*)dev->bufferstart->buffer;
	unsigned char *s=(unsigned char*)source_buffer;
	int c,r;

#ifdef CONFIG_EMPEG_LCD
	lcd(source_buffer);
#endif

	/* The main body of the screen is logical */
	for(c=0;c<63;c++) {
		/* Do one pair of columns: non-special case ones (ie not the
		   ends of the display). Start at the bottom... */
		s+=(64*32);
		for(r=0;r<32;r++) {
			/* Next line is 64 bytes away (screen is 128 wide,
			   4bpp) */
			s-=64;
			
			/* Take pixel pair */
			*d++=s[0]>>4;
			*d++=s[1];
		}
		
		/* Next column is two pixels to the right */
		s++;
	}
	
	/* Last scanline translates to leftmost and rightmost display cols */
	s=(unsigned char*)source_buffer;
	s+=(64*32);

	/* Fudge factor: due to the way SIG is driven, we have a blank bit of
	   screen memory followed by the actual column 0/127 data */
	d+=64;

	for(r=0;r<32;r++) {
		/* Next line is 64 bytes away (screen is 128 wide, 4bpp) */
		s-=64;
		
		/* Get pixel pair */
		*d++=s[63]>>4;
		*d++=s[0];
	}
#endif
}

/* Clear the screen */
static void display_clear(struct display_dev *dev)
{	
	int x, y;
	for(y = 0; y < EMPEG_SCREEN_HEIGHT; y++) {
		for(x = 0; x < EMPEG_SCREEN_WIDTH/2; x++) {
			*(dev->software_buffer + y * (EMPEG_SCREEN_WIDTH/2) + x) = 0;
		}
	}
}

/* Use a number of display images in a queue and blat one to the
   screen for each audio chunk that is sent to the DSP. */

#define INC_QUEUE_INDEX(X) if (++(X) >= BUFFER_COUNT) (X) = 0

static void display_queue_flush(struct display_dev *dev)
{
	dev->queue_rp = dev->queue_wp;
	dev->queue_free = BUFFER_COUNT;
	dev->queue_used = 0;
}

/* Add the current display to the queue. If the queue is full then
   just re-write the current top */

static void display_queue_add(struct display_dev *dev)
{
	unsigned long flags;
	if (dev->queue_free) {
		save_flags_cli(flags);
		DEBUGK("Room in queue, queuing.\n");
		DEBUGK("Before: rp=%d, wp=%d, free=%d, used=%d.\n",
		       dev->queue_rp,
		       dev->queue_wp,
		       dev->queue_free,
		       dev->queue_used);
		INC_QUEUE_INDEX(dev->queue_wp);
		dev->queue_free--;
		dev->queue_used++;
		DEBUGK("After: rp=%d, wp=%d, free=%d, used=%d.\n",
		       dev->queue_rp,
		       dev->queue_wp,
		       dev->queue_free,
		       dev->queue_used);
		restore_flags(flags);
	}

	/* Flush the mmap and target buffers */

	/* Strictly speaking this isn't necessary but we do it anyway. */
	DEBUGK("Copying buffer to queue entry %d.\n", dev->queue_wp);
	display_flush_cache((unsigned long)dev->software_queue + dev->queue_wp * EMPEG_SCREEN_SIZE,
			    EMPEG_SCREEN_SIZE);

	memcpy(dev->software_queue + dev->queue_wp * EMPEG_SCREEN_SIZE,
	       dev->software_buffer,
	       EMPEG_SCREEN_SIZE);

}

/* Display the current top of queue. If there isn't anything in the
   queue then just re-use the last one. */
   
void display_queue_draw(struct display_dev *dev)
{
	unsigned long flags;
	if (dev->queue_used) {
		save_flags_cli(flags);
		DEBUGK("Spare in queue. Removing one.\n");
		DEBUGK("Before: rp=%d, wp=%d, free=%d, used=%d.\n",
		       dev->queue_rp,
		       dev->queue_wp,
		       dev->queue_free,
		       dev->queue_used);
		dev->queue_used--;
		dev->queue_free++;
		INC_QUEUE_INDEX(dev->queue_rp);
		DEBUGK("After: rp=%d, wp=%d, free=%d, used=%d.\n",
		       dev->queue_rp,
		       dev->queue_wp,
		       dev->queue_free,
		       dev->queue_used);
		restore_flags(flags);
	}
	DEBUGK("Blatting from queue entry %d.\n", dev->queue_rp);
	display_blat(dev, dev->software_queue + dev->queue_rp * EMPEG_SCREEN_SIZE);

	/* Wake up anyone polling on us */
	wake_up_interruptible(&dev->wq);
}

static void display_refresh(struct display_dev *dev)
{
	/* We used to go through the complete motions here but this
	   meant that the queue and what was on the screen could be
	   very different, and once the audio thread copied the queue
	   to the display things could look a bit wierd. It's a lot
	   better for us to flush the queue, copy the buffer to it and
	   then draw the queue - good code reuse too. MAC 1999/10/14 */
	display_queue_flush(dev);
	display_queue_add(dev);
	display_queue_draw(dev);
}

/* Display splash screen */
static void display_splash(struct display_dev *dev, const unsigned char *image)
{
	/* Copy splash screen to the software buffer */
	memcpy(dev->software_buffer,image,EMPEG_SCREEN_SIZE);

	/* Blat it: well, add it to the refresh buffer, otherwise when the
	   audio DMA starts it all goes blank... */
	display_refresh(dev);
}

/* Display user-configurable screen */
static void display_user_splash(unsigned long screen)
{
	struct display_dev *dev = devices;
	const unsigned char *image=(const unsigned char*)screen;
	display_splash(dev, image);
}

/* Boot failure */
void display_bootfail(void)
{
	/* Display the no hard disks found image */
	display_user_splash((unsigned long)nohd_img);
}

/* Deal with next animation frame */
static void display_animation(unsigned long animation_base)
{
	struct display_dev *dev = devices;
	unsigned int *frameptr=(unsigned int*)animation_base;

	/* Used once only, so this can be static */
	static int framenr=-1;

	/* Called once to initialise */
	if (framenr>=0) {
		unsigned char *d,*s;
		int a;

		/* Find applicable frame to display */
		s=(unsigned char*)(animation_base+frameptr[framenr]);

		/* End of animation? */
		if (!frameptr[framenr]) return;

		/* Decompress and display */
		d=dev->software_buffer;
		for(a=0;a<2048;a+=2) {
			*d++=((*s&0xc0)>>2)|((*s&0x30)>>4);
			*d++=((*s&0x0c)<<2)|((*s&0x03));
			s++;
		}

		/* Blat it: well, add it to the refresh buffer, otherwise when
		   the audio DMA starts it all goes blank... */
		display_refresh(dev);
	}
		
	/* Re-queue ourselves at ANIMATION_FPS (0.5 seconds for the first
	   frame) */
	init_timer(&animation_timer);
	animation_timer.data=animation_base;
	animation_timer.expires=(jiffies+((framenr==0)?(HZ/2):(HZ/ANIMATION_FPS)));
	animation_timer.function=display_animation;
	add_timer(&animation_timer);

	/* Next frame */
	framenr++;
}

#define CHARS_TO_ULONG(A, B, C, D) ((A) | ((B) << 8) | ((C) << 16) | ((D) << 24))
	
static void handle_splash(struct display_dev *dev)
{
	const int LOGO_EMPEG = 0;
	const int LOGO_RIO = 1;
	const int LOGO_MASK = 0xf;
	const int LOGO_CUSTOM = 0x10;	
	int logo_type;
	unsigned char *user_splash=(unsigned char*)(EMPEG_FLASHBASE+0xa000);
	int animation_time=(3*HZ);
	unsigned long *ani_ptr;

	unsigned long splash_signature = *((unsigned long *)user_splash);

	printk("Signature is %08lx '%c%c%c%c'\n", splash_signature,
	       user_splash[0], user_splash[1], user_splash[2], user_splash[3]);
	
	switch (splash_signature)
	{
	case CHARS_TO_ULONG('e', 'm', 'p', 'g'):
		logo_type = LOGO_CUSTOM | LOGO_EMPEG;
		break;
	case CHARS_TO_ULONG('e', 'm', 'p', ' '):
		logo_type = LOGO_EMPEG;
		break;
	case CHARS_TO_ULONG('r', 'i', 'o', ' '):
		logo_type = LOGO_RIO;
		break;
	case CHARS_TO_ULONG('r', 'i', 'o', 'c'):
		logo_type = LOGO_CUSTOM | LOGO_RIO;
		break;
	default:
		logo_type = LOGO_EMPEG;
	}
	
#ifdef NO_ANIMATION
	/* Load splash screen image */
	if ((logo_type & LOGO_MASK) == LOGO_RIO)
		display_splash(dev, &rio_logo);
	else
		display_splash(dev, &empeg_logo);
#else
	/* Display splash screen animation */
	if ((logo_type & LOGO_MASK) == LOGO_RIO) {
		display_animation((unsigned long)rio_ani);
		ani_ptr=(unsigned long*)rio_ani;
	} else {
		display_animation((unsigned long)empeg_ani);
		ani_ptr=(unsigned long*)empeg_ani;
	}

	/* Work out time to play animation: 1s (0.5 start & end) + frames */
	animation_time=HZ;
	while(*ani_ptr++) animation_time+=(HZ/ANIMATION_FPS);
#endif

	/* Setup timer to display user's image (if present) in 4 seconds */
	if (logo_type & LOGO_CUSTOM) {
		printk("Scheduling custom logo.\n");
		init_timer(&display_timer);
		display_timer.expires=(jiffies+animation_time);

		/* On AC or DC power? AC is first image, DC is second */
		display_timer.data=(unsigned long)(user_splash+4);
		if (GPLR&EMPEG_EXTPOWER) display_timer.data+=EMPEG_SCREEN_SIZE;

		/* Set up function pointer & add to timer queue (it will remove
		   itself when the timer expires) */
		display_timer.function=display_user_splash;
		add_timer(&display_timer);
	}
}



/* This handles the mmap call. To be able to mmap RAM we need to swing
   through hoops a little. See p283 of Linux Device Drivers for details */

unsigned long display_vma_nopage(struct vm_area_struct *vma, unsigned long address, int write_access)
{
	struct display_dev *dev = devices;
	unsigned long offset = address - vma->vm_start + vma->vm_offset;
	unsigned long pageptr = (unsigned long)dev->software_buffer;

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
	display_vma_nopage, /* nopage */
};

/* Send a command to the empeg via the display control line. This is only
   present on the Mk2 mainboard and display board, and uses pulse widths to
   indicate the databits. This line is fed into the button control PIC on the
   mainboard, which can then send the current button state or set the display
   dimmer level */
static void display_sendcontrol(int b)
{
	int bit;
	unsigned long flags;

	/* Send a byte to the display serially via control line (mk2 only) */
	if (empeg_hardwarerevision()<6) return;

	/* Starts with line high, plus a little delay to make sure that
	   the PIC is listening */
	GPSR=EMPEG_DISPLAYCONTROL;
	
	/* Wait 100ms */
	current->state=TASK_INTERRUPTIBLE;
	schedule_timeout(HZ/10);

	/* Need to do this with IRQs disabled to preserve timings */
	/* Disable FIQs too - might make the 2us delay too long */
	save_flags_clif(flags);

	/* Send 8 bits */
	for(bit=7;bit>=0;bit--) {
		/* Set line low */
		GPCR=EMPEG_DISPLAYCONTROL;

		/* Check the bit */
		if (b&(1<<bit)) {
			/* High - 15us of low */
			udelay(15);
		} else {
			/* Low - 2us of low */
			//udelay(1);
			{ int a; for(a=0;a<50;a++); }
		}

		/* Set line high */
		GPSR=EMPEG_DISPLAYCONTROL;

		/* Inter-bit delay */
	        udelay(15);
	}

	/* End of transmission, line low */
	GPCR=EMPEG_DISPLAYCONTROL;

	/* Reenable IRQs */
	restore_flags(flags);
}

static int display_open(struct inode *inode, struct file *filp)
{
	struct display_dev *dev = devices;

	/*MOD_INC_USE_COUNT;*/

	filp->private_data = dev;

	/* Flush the queue on open so that we don't get out of sync. */
	display_queue_flush(dev);
	
	return 0;
}

static int display_release(struct inode *inode, struct file *filp)
{
	/*MOD_DEC_USE_COUNT;*/
	return 0;
}

int display_mmap(struct file *filp, struct vm_area_struct *vma)
{
	/* We can only map at offset zero. It is pointless to try
	   otherwise */
	if (vma->vm_offset != 0)
		return -EINVAL;
	
	/* I think this is required to stop the memory being swapped out */
	vma->vm_flags |= (VM_SHM|VM_LOCKED);

	/* No cache please */
	pgprot_val(vma->vm_page_prot) &= ~(PTE_CACHEABLE | PTE_BUFFERABLE);

	/* nopage will fill in the map */
	vma->vm_ops = &empegfb_vm_ops;
	return 0;
}

/*
 * functions to handle /dev/fb
 *
 * This is all we really deal with on the empeg at the moment - we're not
 * actually interested in plain text output as with only a 128x32 screen it
 * gets a little tight: a 16x4 display? Sounds like some old Epson laptop :)
 *
 */

int display_ioctl(struct inode *inode, struct file *filp, unsigned int cmd,
		  unsigned long arg)
{
	struct display_dev *dev =
		(struct display_dev *)filp->private_data;

	if (cmd < 0x10)
		printk("Deprecated display ioctl %d used.\n", cmd);
	
	switch(cmd) {
	case EMPEG_DISPLAY_REFRESH:
	case 0: /* Screen blat */
		display_refresh(dev);
		break;

	case EMPEG_DISPLAY_POWER:
	case 1: /* Screen power control */
		if (arg) {
			if (LCCR0&LCCR0_LEN) {
				/* Lcd control register 0; everything off */
				LCSR = LCSR_LDD;
				LCCR0 = 0;

				/* Wait for controller off */
				while((LCSR&LCSR_LDD)==0);
			}

			/* Clear error flags */
			LCSR = 0xfff;

			/* Set up the DMA controller's base address for the
			   screen */
			DBAR1 = (unsigned char*)virt_to_phys((int)dev->hardware_buffer);

			/* Now enable the screen */
			LCCR0 = LCCR0_SETUP;
			LCCR0 |= LCCR0_LEN;
			
			/* Disable powerfail interrupts */
			enable_powerfail(FALSE);

			/* Let powerfail know that the display is supposed to be on */
			dev->power = TRUE;
			
			/* Turning display on */
			empeg_displaypower(1);

			/* Wait for a while for it to come to life */
			udelay(POWERFAIL_DISABLED_DELAY);
			enable_powerfail(TRUE);

			/* Keep palette 0 until display has warmed up (500ms?) and then
			   turn palette on. */
		} else {
			/* Do this first in case powerfail triggers */
			dev->power = FALSE;
			
			/* Turning display off */
			empeg_displaypower(0);
			
			/* Set standby LED mode */
		}
		break;
		
	case 2: /* Standby LED settings */
		break;
		
	case 3: /* Set screen brightness */
		break;

	case EMPEG_DISPLAY_WRITE_PALETTE:
	case 4: /* Set screen palette */
		display_setpalette(dev, arg);
		break;

	case EMPEG_DISPLAY_CLEAR:
	case 5: /* Clear screen */
		display_clear(dev);
		break;
		
	case EMPEG_DISPLAY_ENQUEUE:
	case 6: /* Enqueue screen */
	        display_queue_add(dev);
		break;

	case EMPEG_DISPLAY_POPQUEUE:
	case 7:
	        display_queue_draw(dev);
		break;
	case EMPEG_DISPLAY_FLUSHQUEUE:
	case 8:
	        display_queue_flush(dev);
		break;
	case EMPEG_DISPLAY_QUERYQUEUEFREE:
		return put_user(dev->queue_free, (int *)arg);
		
	case EMPEG_DISPLAY_SENDCONTROL:
		display_sendcontrol(arg);
		break;

	case EMPEG_DISPLAY_SETBRIGHTNESS:
		/* Set screen brightness */
		{
			int ret, level;
			if((ret = get_user(level, (int *) arg)) != 0) return ret;
			if(level < 0 || level > 100) return -EINVAL;
			level = 116 - level;
			display_sendcontrol(level);
		}
		break;	

	default:
		return(-EINVAL);
	}
	
	return 0;
}

static int display_fsync(struct file *filp, struct dentry *dentry)
{
	struct display_dev *dev = filp->private_data;
	display_refresh(dev);
	return 0;
}

unsigned int display_poll(struct file *filp, poll_table *wait)
{
	struct display_dev *dev = filp->private_data;

	/* Add ourselves to the wait queue */
	poll_wait(filp, &dev->wq, wait);

	/* Check if the queue has available space */
	if (dev->queue_free)
		return POLLOUT | POLLWRNORM;
	else
		return 0;
}


static struct file_operations display_fops = {
	NULL, /* display_lseek */
	NULL, /* display_read */
	NULL, /* display_write */
	NULL, /* display_readdir */
	display_poll,
	display_ioctl,
	display_mmap,
	display_open,
	NULL, /* display_flush */
	display_release,
	display_fsync,
};

/* Action performed when audio is sent out */
void audio_emitted_action(void)
{
        display_queue_draw(devices);
}

/* Display initialisation */
void __init empeg_display_init(void)
{
	struct display_dev *dev = devices;
	int result,delay;
	
	/* Firstly, we need to locate the LCD DMA buffer to a 4k page
	   boundary to ensure that the SA1100 DMA controller can do
	   display fetches for the LCD. We currently do this by having
	   a container which is 4k bigger than required, ensuring we
	   can 4k align the actual buffer within is container without
	   much hassle */
	
	/* Why do we need to do this? The SA1100 has a 4K page size
	   anyway so we can just use get_free_page to get a page and
	   use it can't we? MAC */

	/* That is what we're doing now. Once it works we can remove
           all the comments :-) */

	if (sizeof(struct empegfb_buffer) > 4*PAGE_SIZE) {
		printk(KERN_ERR "empeg_display_init: Frame Buffer size (%d) is too large.\n",
		       sizeof(struct empegfb_buffer));
		return;
	}

	/* We say we're using it for DMA but linux-arm probably
	   ignores that. MAC */
	dev->hardware_buffer = (unsigned char *)__get_free_pages(GFP_KERNEL, 2);
	memset(dev->hardware_buffer, 0, (1<<2)*PAGE_SIZE);
	
	/* Point the framebuffer structure at it */
	dev->bufferstart=(struct empegfb_buffer*)dev->hardware_buffer;

	/* Allocate base of 'sensible' screen memory */
#if EMPEG_SHADOW_SIZE > PAGE_SIZE
#error Screen size is bigger than a page. FIXME!
#endif
	dev->software_buffer = (void *)get_free_page(GFP_KERNEL);

	/* Allocate a number of buffers to contain the images that are
	   buffered in time with the audio buffer. */
	dev->software_queue = (void *)__get_free_pages(GFP_KERNEL, BUFFER_COUNT_ORDER);
	memset(dev->software_queue, 0, (1<<BUFFER_COUNT_ORDER)*PAGE_SIZE);

	dev->wq = NULL;
	dev->queue_rp = dev->queue_wp = 0;
	dev->queue_used = 0;
	dev->queue_free = BUFFER_COUNT;

	/* Setup normal palette */
	display_setpalette(dev, PALETTE_STANDARD);
	
	/* First ensure that LCD controller is turned off */
	LCCR0 = 0;

	/* Small delay to ensure DMA is turned off */
	delay=jiffies+(HZ/10);
	while(jiffies<delay);

	/* Set up the DMA controller's base address for the screen */
	DBAR1 = (unsigned char*)virt_to_phys((int)dev->hardware_buffer);

	/* LCD control register 1; display width */
	LCCR1 = LCCR1_SETUP;

	/* LCD control register 2; display height */
	LCCR2 = LCCR2_SETUP;
	
	/* LCD control register 3; display flags and clockrate */
	LCCR3 = LCCR3_SETUP;
	
	/* LCD control register 0; flags & enable */
	LCCR0 = LCCR0_SETUP;
	LCCR0|= LCCR0_LEN;

	/* Turn on display: raising GPIO18 turns on the VN02 high-side
	   driver, which feeds the raw car input power to a number of
	   departments totally separate from the normal power
	   supplies:

	   - The AUXPWR pin on the docking connector is now
	     powered. This is used for external amplifier power
	     control (max 1A drain as the input is fused inside the
	     empeg through a 3A polyfuse).
	   
	   - The display board +12v power input, which drives the 60v
	     switch-mode power supply for the display.
	   
	   - A MOSFET on the front board which provides +5v from the
	     normal 5v PSU to the display filament heater (~250mA) and
	     logic power for the display.
	   
	   - The power sense pins on the display board PIC and PEEL
	     which causes the PIC to exit powerdown mode (where it
	     controls the standby LED in a number of patterns) and
	     enter display scan mode. The powersense also causes the
	     PIC/PEEL outputs to the display board to tristate when
	     low to ensure we don't backdrive the display through its
	     logic pins.  */

	/* Disable powerfail detection. It is possible that this will
	   trigger when the display is turned on due to the current
	   draw - so we disable it, turn on the power, wait a bit, then
	   re-enable it */

	enable_powerfail(FALSE);
	dev->power = TRUE;
	empeg_displaypower(1);
	udelay(POWERFAIL_DISABLED_DELAY);
	enable_powerfail(TRUE);

	result = register_chrdev(EMPEG_DISPLAY_MAJOR, "empeg_display",
				 &display_fops);
	if (result < 0) {
		printk(KERN_WARNING "empeg Display: Major number %d "
		       "unavailable.\n", EMPEG_DISPLAY_MAJOR);
		return;
	}

#ifdef CONFIG_EMPEG_LCD
	/* Display on */
        *lcd_command0=0xe2;
        *lcd_command1=0xe2;
	udelay(1);
	*lcd_command0=0xaf;
	*lcd_command1=0xaf;
	udelay(1);
#endif

	handle_splash(dev);
	printk("empeg display initialised.\n");
}

void display_powerreturn_action(void)
{
#if 0
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

	/* Display on again */
	empeg_displaypower(1);
#else
	struct display_dev *dev = devices;

	/* Set up SA1100 LCD controller: first ensure that it's turned off */
	LCCR0 = 0;
	
//	printk("Frame buffer start is at %p\n", empegfb_start);
//	printk("In physical memory that's %p\n", (unsigned char *)virt_to_phys((int)empegfb_start));
	
	/* Set up the DMA controller's base address for the screen */
	DBAR1 = (unsigned char*)virt_to_phys((int)dev->hardware_buffer);
	
	/* Set up LCD controller */
	LCCR1 = LCCR1_SETUP;
	LCCR2 = LCCR2_SETUP;
	LCCR3 = LCCR3_SETUP;
	
	/* LCD control register 0; flags & enable */
	LCCR0 = LCCR0_SETUP;
	LCCR0 |= LCCR0_LEN;
  
	/* Turn on display: raising GPIO18 turns on the VN02 high-side
	   driver, which feeds the raw car input power to a number of
	   departments totally separate from the normal power
	   supplies:

	   - The AUXPWR pin on the docking connector is now
	     powered. This is used for external amplifier power
	     control (max 1A drain as the input is fused inside the
	     empeg through a 3A polyfuse).
	   
	   - The display board +12v power input, which drives the 60v
	     switch-mode power supply for the display.
	   
	   - A MOSFET on the front board which provides +5v from the
	     normal 5v PSU to the display filament heater (~250mA) and
	     logic power for the display.
	   
	   - The power sense pins on the display board PIC and PEEL
	     which causes the PIC to exit powerdown mode (where it
	     controls the standby LED in a number of patterns) and
	     enter display scan mode. The powersense also causes the
	     PIC/PEEL outputs to the display board to tristate when
	     low to ensure we don't backdrive the display through its
	     logic pins.  */

	/* Disable powerfail detection. It is possible that this will
	   trigger when the display is turned on due to the current
	   draw - so we disable it, turn on the power, wait a bit, then
	   re-enable it */

	if (dev->power) {
		empeg_displaypower(1);
		udelay(POWERFAIL_DISABLED_DELAY);
	}
#endif
}
