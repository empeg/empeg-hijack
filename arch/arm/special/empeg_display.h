#ifndef EMPEG_DISPLAY_H
#define EMPEG_DISPLAY_H

// pinched from empeg_display.c

/* Animation speed (in fps) */
#define ANIMATION_FPS		12

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

#endif // EMPEG_DISPLAY_H
