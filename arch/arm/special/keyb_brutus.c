/*
 * linux/arch/arm/drivers/char/keyb_brutus.c
 *
 * Keyboard driver for Brutus ARM Linux.
 *
 * Note!!! This driver talks directly to the keyboard.
 *
 * Changelog:
 * 22-01-1998	lsb	Created by modifying keyb_rpc.c
 *
 */

#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/mm.h>
#include <linux/malloc.h>
#include <linux/ptrace.h>
#include <linux/signal.h>
#include <linux/timer.h>
#include <linux/random.h>
#include <linux/ctype.h>

#include <asm/bitops.h>
#include <asm/irq.h>
#include <asm/hardware.h>
#include <asm/io.h>
#include <asm/system.h>
#include <asm/arch/ostimers.h>

extern void kbd_keyboardkey(unsigned int keycode, unsigned int up_flag);
extern void kbd_reset(void);

extern struct pt_regs *kbd_pt_regs;


#define EXT(d, b) ((((unsigned int)(d))>>(b))&1)


/*
 * Area of brutus keyboard specific functions
 */

#include "kbmap_brutus.h"

#define KBCTL_NODATA -1
#define KBCTL_AGAIN  -2
#define KBCTL_OLD_LEDS 0xffaef

static int key_fn_down=0;
static int key_numl_down=0;
static struct sspreg *ssp_base_v;
static struct scm_gpio_regs *gp_base_v;
static int *gpdr_v;

#ifndef CONFIG_SA1100_ITSY
static int putcToKBCTL (unsigned char);
static void putsToKBCTL (char *, int);
static char lrc (char *, int);
static void setKBled (int);
static int checkInputKBCTL (void);
static void waitInputKBCTL (void);
static int getcFromKBCTL (void);
static int wgetcFromKBCTL (void);
static void initKBCTL (void);
#endif

extern void lsb_flash (int);
extern void handle_scancode(unsigned char scancode, int down);

void kbd_setregs(struct pt_regs *regs)
{
	kbd_pt_regs = regs;
}

int kbd_drv_translate(unsigned char scancode, unsigned char *keycode_p, char raw_mode)
{
	/* We already did most of the translation work in handle_rawcode.  Just
	   figure out whether this is an up or down event.  */
	*keycode_p = scancode & 0x7f;
	return 1;
}

#ifndef CONFIG_SA1100_ITSY
static int putcToKBCTL (unsigned char c)
{
  /*
   * NOTE: on older versions of sa1100 you need to reset the ssp port
   *       every time (I assume new versions here). These functions
   *       were copied from code that came with early brutus boards.
   */
  
  while (!(Ser4SSSR & SSSR_TNF)); /* check for space */
  Ser4SSDR = (c<<8) | c; /* put char */
  while (!(Ser4SSSR & SSSR_RNE)); /* wait to get char back */
  return Ser4SSDR;
}

static void putsToKBCTL (char *m, int cnt)
{
  int i,j,j2,x;

  GPCR = 1<<23;
  for (j=0; j<3; j++)
    x = GPLR;
  GPSR = 1<<23;
  for (j=0; j<250*30; j++)
    x = GPLR;
  for (i=0; i<cnt; i++) {
    j2 = putcToKBCTL(m[i]);
    for (j=0; j<400; j++)
      x = GPLR;
  }
}

static char lrc (char *m, int l)
{
  int i,crc;
  
  crc = 0;			/* init crc */
  for(i=0; i<l; i++) { 		/* xor in all data */
    crc ^= *m++;
  }
   if ((crc & 0x80) == 0x80) {
     crc ^= 0xC0;
   }
  return crc;
}

static void setKBled (int i)
{
  char msg[20];
  
  msg[0]=0x1B; /* led on */
  msg[1]=0xa6;
  msg[2]=0;
  msg[3]=(char) (i & 1);
  msg[4]=0;
  msg[5]=0;
  msg[6]=0;
  msg[7]=0;
  msg[8]=lrc(msg,8);
  putsToKBCTL(msg, 9);
}

static int checkInputKBCTL (void)
{
  return (EXT(GPLR,25)); 	/* check KBCTL has data to send */
}

static void waitInputKBCTL (void)
{
  while (EXT(GPLR,25)); 
}


static int getcFromKBCTL (void)
{
  int x,y;
  static int lastc=-1;
  
  if (EXT(GPLR,25)) {
    return KBCTL_NODATA;
  }

  x = putcToKBCTL(0)&0xff; 	/* get char by sending one */
  if (x == lastc || x == 0 || x == 0xff)
    return KBCTL_NODATA;

  lastc = x;

  if (EXT(x,7)) {    		/* key up */
    x &= 0x7f;
    if (x == 0x21) {		/* fn key up */
      key_fn_down = 0;
      return KBCTL_AGAIN;
    }
    else if (key_fn_down) { 	/* this is a fn modified key */
      y = kbmapFN[x];
      if (y == KK_NUML) 
	return KBCTL_AGAIN;
    }
    else if (key_numl_down) 	/* this is a numlock modified key */
      y = kbmapNL[x];
    else
      y = kbmap[x];
    return (y | 0x80);
  } else {			/* key down */
    /* key down */
    if (x == 0x21) {		/* fn key down */
      key_fn_down = 1;
      return KBCTL_AGAIN;
    }
    else if (key_fn_down) {	/* this is a fn modified key */
      y = kbmapFN[x];
      if (y == KK_NUML) {	/* toggle local numlock */
	key_numl_down = !key_numl_down;
//	kbd_drv_setleds(KBCTL_OLD_LEDS);
	return KBCTL_AGAIN;
      }
    }
    else if (key_numl_down)	/* this is a numlock modified key */
      y = kbmapNL[x];
    else
      y = kbmap[x];
    return y;
  }
}

static int wgetcFromKBCTL (void)
{
  waitInputKBCTL();
  return getcFromKBCTL();
}

static void initKBCTL (void)
{
  char msg[30];
  int  i;

  /* set dir for ssp */
  GPDR = (GPDR | (0xD<<10) | (1<<23)) & ~(0x2 << 10);
  GAFR |= (0xf << 10);

  GPSR = 1<<23; 	/* deassert the kbctl wakup pin */
  PPAR |= 1<<18; 		/* set alt function for spi interface  */
 
  Ser4SSCR0 = 0;
  Ser4SSCR1 = 0; 		/* no ints no loopback */
  Ser4SSSR = 0; 		/* remove any rcv overrun errors */
  /* turn on SSP */
  Ser4SSCR0 = SSCR0_DataSize(8) + SSCR0_SerClkDiv(8) + SSCR0_SSE + SSCR0_Motorola;

  /* drain any data already there */
  while (Ser4SSSR & SSSR_RNE)  i = Ser4SSDR;
  
  msg[0]=0x1B; /* led on */
  msg[1]=0xa7;
  msg[2]=0;
  msg[3]=3;
  msg[4]=lrc(msg, 4);
  putsToKBCTL(msg, 5);

  /* clear keyboard buffer */
  while (getcFromKBCTL() != KBCTL_NODATA) {
    ;
  }
  
}

/*
 * End of Brutus Functions
 */

#endif /* ! CONFIG_SA1100_ITSY */
	  
#define VERSION 101

static inline void kbd_drv_reset(void)
{
	key_fn_down = 0;
	key_numl_down = 0;
	kbd_reset();
}

int ignore_kbd_irq = 0;
extern void vsync_irq(void);
extern void count_fb_red(void);

static void kbd_drv_rx(int irq, void *dev_id, struct pt_regs *regs)
{
  	int i;
	
        kbd_setregs(regs);
#ifndef CONFIG_SA1100_ITSY
	while (!ignore_kbd_irq  &&  (i=getcFromKBCTL()) != KBCTL_NODATA) {
	  if (i != KBCTL_AGAIN  &&  i != KK_NONE)
            handle_scancode((unsigned char) i, ((unsigned char)i & 0x80) ? 0 : 1);
	}
#endif
	// vsync_irq();
	reset_timer1( (TIMER1_RATE+30)/60 );
	mark_bh(KEYBOARD_BH);
}

static void kbd_drv_tx(int irq, void *dev_id, struct pt_regs *regs)
{
}


int kbd_drv_init (void)
{
	unsigned long flags;

	save_flags_cli (flags);
#ifndef CONFIG_SA1100_ITSY
	initKBCTL();
#endif
	setup_timer1( (TIMER1_RATE+30)/60 );
	if (request_irq (IRQ_OST1, kbd_drv_rx, SA_SHIRQ, "keyboard", NULL) != 0)
	  printk("Could not allocate keyboard receive IRQ!");
	/* LSB should use panic() above */
	
	restore_flags (flags);

	printk (KERN_INFO "Keyboard driver v%d.%02d\n", VERSION/100, VERSION%100);
	//printk("**** Turning off console printing ****\n");

	/* Turn off console printk messages */
#ifndef CONFIG_SA1100_ITSY
 	// con_printable(0);
#endif
	return 0;
}
