/*
 * linux/arch/arm/special/tifon_keyb.c
 *
 * Keyboard driver for tifon
 * 
 * Author: Stefan Hellkvist
 */

#define NEW_KEYBOARD

#include <linux/config.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/mm.h>
#include <linux/ptrace.h>
#include <linux/signal.h>
#include <linux/string.h>
#include <linux/ioport.h>
#include <linux/random.h>
#include <asm/bitops.h>
#include <asm/io.h>
#include <asm/system.h>
#include <linux/delay.h>
#include <linux/param.h>

#include <asm/arch/hardware.h>
#include <asm/irq.h>

#include "tifon_keyb.h"

// #define DEBUG 1


#ifndef Word
#define Word unsigned int
#endif

#define N_COLS 3
#define N_ROWS 6
#define N_KEYS 55

#define MAX_ROW (N_ROWS - 1)
#define MAX_COL (N_COLS - 1)

#define TIME_OUT (HZ / 2)          /* 500 msec */
#define DEBOUNCE_TIME_OUT (HZ / 20) /* 50 msec */

#define KEY_PRESSED(row, col, bm) ((bm) & (1 << ((col) * N_ROWS + (row))))
#define IS_REAL_COL(col) (!((col) % 2))
#define IS_REAL_ROW(row) (!((row) % 2))
#define IS_REAL_KEY(row, col) (IS_REAL_ROW(row) && IS_REAL_COL(col))


#define ENABLE_KEYB_IRQS() {int i; for(i=56; i<59; i++) enable_irq(i);}
#define DISABLE_KEYB_IRQS() {int i; for(i=56; i<59; i++) disable_irq(i);}

static struct timer_list tfon_kbd_timer, tfon_debounce_timer;
static int timer_started = 0;

/* what is this used for? */
unsigned char tfonkbd_sysrq_xlate[128];

static inline unsigned int read_keyb( void );
static unsigned char translate_to_scancode( unsigned int state );
extern void handle_scancode(unsigned char scancode, unsigned char updown);

/* The mode the pad is in. This is also used by the touch pad driver */
unsigned char tifon_keyboard_mode = ALPHA_MODE;
unsigned char tifon_shift_status = INACTIVE;
unsigned char tifon_capslock_status = INACTIVE;
unsigned char tifon_ctrl_status = INACTIVE;


/* The mapping to keycodes in the three modes */
static unsigned char keys[3][N_KEYS] = {
  { NO_KEY, CTRL_KEY, 3, 0, 4, 0, 5, 0, 6, 0, 0,
    RIGHT_KEY, DELETE_KEY, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    DOWN_KEY, UP_KEY, 11, 0, 12, 0, 13, 0, 14, 0, 0,
    LEFT_KEY, SHIFT_KEY, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    YES_KEY, ESCAPE_KEY, 19, 0, 20, 0, 21, 0, 22, 0, 0 },
  { NO_KEY, CTRL_KEY, 23, 24, 25, 26, 27, 28, 29, 0, 0,
    RIGHT_KEY, DELETE_KEY, 30, 31, 32, 33, 34, 35, 36, 0, 0,
    DOWN_KEY, UP_KEY, 37, 38, 39, 40, 41, 42, 43, 0, 0,
    LEFT_KEY, SHIFT_KEY, 44, 45, 46, 47, 48, 49, 50, 0, 0, 
    YES_KEY, ESCAPE_KEY, 51, 52, 53, 54, 55, 56, 57, 0, 0 },
  { NO_KEY, CTRL_KEY, TAB_KEY, 59, 60, 61, 62, 63, 64, 0, 0,
    RIGHT_KEY, DELETE_KEY, 65, 66, 67, 68, 69, 70, 71, 0, 0,
    DOWN_KEY, UP_KEY, 72, 73, 74, 75, 76, 77, 78, 0, 0,
    LEFT_KEY, SHIFT_KEY, 79, 80, 81, 82, 83, PGDN_KEY, PGUP_KEY, 0, 0, 
    YES_KEY, ESCAPE_KEY, 86, 87, 88, 89, 90, END_KEY, HOME_KEY, 0, 0 }
};


/**
 * debounce
 * Called after a short delay after timed_out is called
 * It checks if the key has been released and in that case it
 * enables the keypad again. Else it will be called again soon.
 **/
static void
debounce( unsigned long ptr )
{
  unsigned int stat = read_keyb();
  if ( !stat )
    {
      ENABLE_KEYB_IRQS();
      INTSTATCLR0 = (1 << 24) | (1 << 25) | (1 << 26);
      timer_started = 0;
    }
  else
    {
      init_timer( &tfon_debounce_timer );
      tfon_debounce_timer.function = debounce;
      tfon_debounce_timer.data = 0;
      tfon_debounce_timer.expires = jiffies + DEBOUNCE_TIME_OUT;
      add_timer( &tfon_debounce_timer );
    }
}

/**
 * timed_out
 * Called after a short delay after the first interrupt of
 * a keypress (the simulation of more keys than exists makes
 * a simulated keypress consist of several real keypresses)
 * This is where the actual reading and handling takes place
 **/      
static void
timed_out( unsigned long ptr )
{
  unsigned int stat = read_keyb();
  if ( stat )
    {
      unsigned char scancode = translate_to_scancode( stat );
      if ( scancode != 255 ) // scancode is 255 if mode switch
	{
	  // printk( "I think it was key number %d\n", scancode );
	  if ( tifon_shift_status == ACTIVE )
	    handle_scancode( 3 * (N_ROWS * 2 - 1) + 1, 1 ); // send shift down
	  if ( tifon_ctrl_status == ACTIVE )
	    handle_scancode( 1, 1 );  // send ctrl down

	  handle_scancode( scancode, 1 ); /* key goes down */
	  handle_scancode( scancode, 0 ); /* key goes up again */

	  if ( tifon_shift_status == ACTIVE )
	    {
	      handle_scancode( 3 * (N_ROWS * 2 - 1) + 1, 0 ); // send shift up
	      if ( tifon_capslock_status == INACTIVE )
		tifon_shift_status = INACTIVE;
	    }
	  if ( tifon_ctrl_status == ACTIVE )
	    {
	      handle_scancode( 1, 0 );  // send ctrl up
	      tifon_ctrl_status = INACTIVE;
	    }	      
	}
    }
 
  init_timer( &tfon_debounce_timer );
  tfon_debounce_timer.function = debounce;
  tfon_debounce_timer.data = 0;
  tfon_debounce_timer.expires = jiffies + DEBOUNCE_TIME_OUT;
  add_timer( &tfon_debounce_timer );
}


/**
 * keyboard interrupt
 * The handler for the sa1101 interrupt
 * Note that it's only called on positive flanks (key pressed)
 * I don't know if there is a way to get an interrupt on both
 * flanks
 **/
static void 
keyboard_interrupt( int irq, void *data, struct pt_regs *regs )
{
  /* enques a timer to call timed_out after TIME_OUT jiffies
     so that "simultaneous" keypresses can be detected
     see page 149 in LDD for an example of enqueuing a timer */

  // printk( "keyboard_interrupt: irq = %d\n", irq );

  if ( !timer_started )
    {
      DISABLE_KEYB_IRQS();
      timer_started = 1;
      init_timer( &tfon_kbd_timer );
      tfon_kbd_timer.function = timed_out;
      tfon_kbd_timer.data = 0;
      if ( tifon_keyboard_mode == NUM_MODE )
	tfon_kbd_timer.expires = jiffies + 1;   // short time out if in numerical
      else
	tfon_kbd_timer.expires = jiffies + TIME_OUT;
      add_timer( &tfon_kbd_timer );
    }
}


/**
 * issimulatedkeydown
 * A simulated key is down if one of the alternatives below are true:
 * 1) It's in a real column and the real key above and below are down
 * 2) It's in a real row and real keys to the right and to the left  are down
 * 3) It's in a simulated column and row and all it's real key neighbours are
 *    down
 **/
static inline int
issimulatedkeydown( int row, int col, unsigned int bm )
{
  /* If simulated key is in a real col then the key is "down" if
   * the real keys above and below in the same col is down */
  if ( IS_REAL_COL( col ) )
    return KEY_PRESSED( (row - 1) >> 1, col >> 1, bm ) &&
      KEY_PRESSED( (row + 1) >> 1, col >> 1, bm );

  /* If simulated key is in a real row then the key is "down" if
   * the real keys to the right and to the left in the same row is down */
  if (IS_REAL_ROW(row))
    return KEY_PRESSED(row >> 1, (col - 1) >> 1, bm) &&
      KEY_PRESSED(row >> 1, (col + 1) >> 1, bm);

  /* It's a middle key.
   * Middle keys are down if two real key neigbours form
   * a diagonal across it */
  return (KEY_PRESSED((row - 1) >> 1, (col - 1) >> 1, bm) &&
	  KEY_PRESSED((row + 1) >> 1, (col + 1) >> 1, bm)) ||
    (KEY_PRESSED((row - 1) >> 1, (col + 1) >> 1, bm) &&
     KEY_PRESSED((row + 1) >> 1, (col - 1) >> 1, bm));
}


/**
 * isrealkeydown
 * A real key is down if the key is down and it's not part of a
 * simulated keypress.
 **/
static inline int
isrealkeydown( int row, int col, unsigned int bm )
{
  int rrow = row >> 1;
  int rcol = col >> 1;
    
  return KEY_PRESSED(rrow, rcol, bm) &&
    (rrow == 0 || !KEY_PRESSED(rrow-1, rcol, bm)) &&
    (rrow == MAX_ROW || !KEY_PRESSED(rrow+1, rcol, bm)) &&
    (rcol == 0 || !KEY_PRESSED(rrow, rcol - 1, bm)) &&
    (rcol == MAX_COL || !KEY_PRESSED(rrow, rcol + 1, bm)) &&
    ((rcol == MAX_COL && rrow == MAX_ROW) || !KEY_PRESSED(rrow + 1, rcol +  1, bm)) &&
    ((rcol == 0 && rrow == MAX_ROW) || !KEY_PRESSED(rrow + 1, rcol -  1, bm)) &&
    ((rcol == MAX_COL && rrow == 0) || !KEY_PRESSED(rrow - 1, rcol +  1, bm)) &&
    ((rcol == 0 && rrow == 0) || !KEY_PRESSED(rrow - 1, rcol -  1, bm));
}


/**
 * iskeydown
 * Checks if a key is down in the simulated 10x5-keypad
 * Args: row - The row to check
 *       col - The col to check
 *       bm  - The bitmask as returned from read_keyb
 * Returns: zero if not pressed, else non zero
 **/
static inline int 
iskeydown( int row, int col, unsigned int bm )
{
  if ( IS_REAL_KEY( row, col ) )  /* Is the key a real key? */
    return isrealkeydown( row, col, bm );
  return issimulatedkeydown( row, col, bm );
}


#ifdef DEBUG

static 
void printKeypadStatus( unsigned int kpad )
{
  int i, j;
  printk( "tfonkeyb: keypad status is 0x%x\n", kpad );
  
  for ( i = 0; i < N_ROWS; i++ )
    {
      for ( j = 0; j < N_COLS; j++ )
	{
	  if ( kpad & ( 1 << ( i * N_COLS + j ) ) )
	    printk( "X" );
	  else
	    printk( "." );
	}
      printk( "\n" );
    }
  printk( "\n" );
}

#endif


/**
 * read_keyb
 * Reads the 6x3 keypad. No simulated key mambo-jumbo involved yet.
 * Returns: a bit mask of which keys that are down, a 1 means key is down
 * Bit mask format:
 *                | Column 2        | Column 1        | Column 0        |
 *  ____________________________________________________________________
 * | 31-18 unused |R5|R4|R3|R2|R1|R0|R5|R4|R3|R2|R1|RO|R5|R4|R3|R2|R1|R0|
 *  --------------------------------------------------------------------
 **/           
static inline unsigned int
read_keyb()
{
  unsigned int i, j, ymask, scancode = 0;

  for ( i = 0; i < N_ROWS; i++ )
    {
      PXDWR = ~( 1 << i );     /* select which row to read */
      /* Must delay some here to allow value to propagate. How long? */
      udelay( 10 );
      for ( j = 0; j < N_COLS; j++ )
	{
	  ymask = ( 1 << j );
	  if ( !( PYDRR & ymask ) ) /* read col j */
	    {
	      scancode |= 1 << (j * N_ROWS + i);
#ifdef DEBUG
	      printk( "Key down in row %d, column %d (scancode = 0x%x)\n", 
		      i, j, scancode );
#endif  
	    }
	}
    }


#ifdef DEBUG
  printKeypadStatus( scancode );
#endif

  PXDWR = 0;   // set all X ports to output low again
  PYDWR = 0xffff; // set all Y ports to input again
  // ENABLE_KEYB_IRQS(); // enable interrupts again
  return scancode;
}


/**
 * translate_to_scancode
 * Translates the keyboard state to a scancode
 **/
static unsigned char
translate_to_scancode( unsigned int state )
{
  int i, j;
  
#ifdef DEBUG
  j = 0;
  for ( i = 0; i < 32; i++ )
    if ( state & ( 1 << i ) )
      j++;

  printk( "Number of keys down: %d\n", j );
#endif

  for ( i = 0; i < 2 * N_ROWS - 1; i++ )
    for ( j = 0; j < 2 * N_COLS - 1; j++ )
      if ( iskeydown( i, j, state ) )
	{
	  if ( i == 10 )  /* is it a mode shift key? */
	    {
	      unsigned char relcol = j >> 1;
#ifdef DEBUG
	      printk( "\nShifting from mode %d to", 
	      tifon_keyboard_mode ); 
#endif
	      switch (relcol)
		{
		case 0: tifon_keyboard_mode = ALPHA_MODE; break;
		case 1: tifon_keyboard_mode = MOUSE_MODE; break;
		case 2: tifon_keyboard_mode = NUM_MODE; break;
		};
#ifdef DEBUG
	      printk( " %d\n", tifon_keyboard_mode );
#endif
	      return 255;
	    }
	  if ( i == 1 && j == 3 ) // is it the shift key
	    {
	      if ( tifon_shift_status == INACTIVE )
		tifon_shift_status = ACTIVE;
	      else
		if ( tifon_capslock_status == INACTIVE )
		  tifon_capslock_status = ACTIVE;
		else
		  {
		    tifon_capslock_status = INACTIVE;
		    tifon_shift_status = INACTIVE;
		  }
#ifdef DEBUG
	      printk( "Shift mode is %s\n", 
		      tifon_shift_status == ACTIVE ? "on" : "off" );
	      printk( "Caps lock mode is %s\n", 
		      tifon_capslock_status == ACTIVE ? "on" : "off" );
#endif
	      return 255;
	    }
	  if ( i == 1 && j == 0 ) // is it the ctrl key
	    {
	      if ( tifon_ctrl_status == INACTIVE )
		tifon_ctrl_status = ACTIVE;
	      return 255;
	    }
#ifdef DEBUG
	  printk( "Key at row %d col %d is down\n", i, j );
#endif
	  return (unsigned char) (j * (2 * N_ROWS - 1) + i );
	}
  return 0;
}


/**
 * tfonkbd_pretranslate
 * Part of the interface to the keyboard driver
 * Checks if a scancode is valid
 **/
int 
tfonkbd_pretranslate( unsigned char scancode, char raw_mode )
{
  return 1;
}


/**
 * tfonkbd_translate
 * Part of the interface to the keyboard driver
 * Translates the scancode to a keycode
 **/
int 
kbd_drv_translate( unsigned char scancode, unsigned char *keycode, char rawmode )
{
  *keycode =  keys[tifon_keyboard_mode][scancode & 0x7f];
  return 1;
}


/**
 * tfonkbd_setkeycode
 * Part of the interface to the keyboard driver
 * Sets the keycode for a scancode.
 * Note: The first 55 scancodes belongs to numerical mode, 
 *       second 55 to alpha mode and the last 55 to mouse mode
 **/
int 
tfonkbd_setkeycode( unsigned int scancode, unsigned int keycode )
{
  if (scancode >= 3 * 55 || keycode > 92)
    return -EINVAL;
  keys[scancode / 55][scancode % 55] = keycode;
  return 0;
}


/**
 * tfonkbd_getkeycode
 * Part of the interface to the keyboard driver
 * Gets the keycode for a scancode
 * Note: The first 55 scancodes belongs to numerical mode, 
 *       second 55 to alpha mode and the last 55 to mouse mode
 **/
int 
tfonkbd_getkeycode( unsigned int scancode )
{
  return (scancode >= 3 * 55) ? -EINVAL :
    keys[scancode / 55][scancode % 55];
}


/**
 * tfonkbd_init_hw
 * Part of the interface to the keyboard driver
 * Initializes the interrupt handling
 **/
void 
kbd_drv_init(void)
{
  int irq;

  INTPOL0 |= (1 << 24) | (1 << 25) | (1 << 26);
  for ( irq = KPYIN0; irq <= KPYIN2; irq++ ) 
    {
      if ( request_irq( irq, keyboard_interrupt, SA_INTERRUPT, "Keypad", NULL ))
	{
	  printk(KERN_ERR "Can't get SA1101_IRQ (IRQ = %d).\n", irq);
	  return;
	}
    }
  SKPCR &= ~0x40; 
  PYDWR = ~0;  // set all Y ports to input
  PXDWR = 0;   // set all X ports to output low
  printk( "Keypad driver initialized: INTPOL0 = 0x%x, SKPCR = 0x%x", INTPOL0, SKPCR );
}

/**
 * tfonkbd_leds
 * Part of the interface to the keyboard driver
 * Does nothing yet since we don't have no leds
 **/
void
tfonkbd_leds( unsigned char leds )
{
}


/**
 * tfonkbd_unexpected_up
 * Part of the interface to the keyboard driver
 * Haven't figured out what this does yet but I guess it doesn't apply
 * to our touch pad
 **/
char
tfonkbd_unexpected_up( unsigned char keycode )
{
  return 0200;
}



/**
 * kbd_disable_irq
 * Part of the interface to the keyboard driver
 * Should probably disable irq but that is not done yet
 **/
void 
kbd_disable_irq( void )
{
}


/**
 * kbd_enable_irq
 * Part of the interface to the keyboard driver
 * Should probably disable irq but that is not done yet
 **/
void 
kbd_enable_irq( void )
{
}

