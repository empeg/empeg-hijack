// Empeg display/IR hijacking by Mark Lord <mlord@pobox.com>
//
// Includes: font, drawing routines
//           extensible menu system
//           VolAdj settings and display
//           BreakOut game
//           Screen blanker (to prevent burn-out)
//           Vitals display
//           High temperature warning
//           ioctl() interface for userland apps
//           Simple integer calculator
//           Knob-Press re-definition
//           IR Button Press Display
//           Reboot Machine from menu
//

#include <asm/arch/hijack.h>
#include <linux/soundcard.h>	// for SOUND_MASK_*

extern int get_loadavg(char * buffer);					// fs/proc/array.c
extern void machine_restart(void *);					// arch/alpha/kernel/process.c
extern int real_input_append_code(unsigned long data);			// arch/arm/special/empeg_input.c
extern int empeg_state_dirty;						// arch/arm/special/empeg_state.c
extern void state_cleanse(void);					// arch/arm/special/empeg_state.c
extern void hijack_voladj_intinit(int, int, int, int, int);		// arch/arm/special/empeg_audio3.c
extern void hijack_beep (int pitch, int duration_msecs, int vol_percent);// arch/arm/special/empeg_audio3.c
extern unsigned long jiffies_since(unsigned long past_jiffies);		// arch/arm/special/empeg_input.c

extern int empeg_on_dc_power(void);					// arch/arm/special/empeg_power.c
extern unsigned char get_current_mixer_source(void);			// arch/arm/special/empeg_mixer.c
extern int empeg_readtherm(volatile unsigned int *timerbase, volatile unsigned int *gpiobase);	// arch/arm/special/empeg_therm.S
extern int empeg_inittherm(volatile unsigned int *timerbase, volatile unsigned int *gpiobase);	// arch/arm/special/empeg_therm.S
       int display_ioctl (struct inode *inode, struct file *filp, unsigned int cmd, unsigned long arg); //arch/arm/special/empeg_display.c

#ifdef CONFIG_NET_ETHERNET	// Mk2 or later? (Mk1 has no ethernet)
#define EMPEG_KNOB_SUPPORTED	// Mk2 and later have a front-panel knob
#endif

int  hijack_fsck_disabled = 0;		// used in fs/ext2/super.c
char hijack_notify_buf[84] = {0,};	// used in drivers/char/serial.c // FIXME: add to /proc, or a fifo?

static unsigned int PROMPTCOLOR = COLOR3, ENTRYCOLOR = -COLOR3;
static unsigned int hijack_classic = 0;	// 1 == don't highlite menu items

#define NEED_REFRESH		0
#define NO_REFRESH		1

#define HIJACK_INACTIVE		0
#define HIJACK_INACTIVE_PENDING	1
#define HIJACK_PENDING		2
#define HIJACK_ACTIVE		3

#define RESTORE_CARVISUALS
#ifdef RESTORE_CARVISUALS
static unsigned int carvisuals_enabled = 0;
static unsigned int restore_carvisuals = 0;
#endif // RESTORE_CARVISUALS
static unsigned int on_dc_power = 0;
static unsigned int hijack_status = HIJACK_INACTIVE;
static unsigned long hijack_last_moved = 0, hijack_last_refresh = 0, blanker_triggered = 0, blanker_lastpoll = 0;
static unsigned char blanker_lastbuf[EMPEG_SCREEN_BYTES] = {0,}, blanker_is_blanked = 0;

static int  (*hijack_dispfunc)(int) = NULL;
static void (*hijack_movefunc)(int) = NULL;
static unsigned long ir_lastevent = 0, ir_lasttime = 0, ir_selected = 0, ir_releasewait = 0, ir_trigger_count = 0;;
static unsigned long ir_menu_down = 0, ir_left_down = 0, ir_right_down = 0;
static unsigned long ir_move_repeat_delay, ir_shifted = 0;
static int *ir_numeric_input = NULL, player_menu_is_active = 0, player_sound_adjust_is_active = 0;

#define IR_FLAGS_LONGPRESS	1
#define IR_FLAGS_CAR	2
#define IR_FLAGS_HOME	4
#define IR_FLAGS_SHIFTED	8

typedef struct ir_translation_s {
	unsigned long	old;		// original code (bit31 == 0)
	unsigned char	count;		// how many codes in new[]
	unsigned char	source;		// (T)uner,(A)ux,(M)ain, or '\0'(any)
	unsigned char	flags;		// boolean flags
	unsigned char	spare;		// for future use
	unsigned long	new[1];		// start of macro table
} ir_translation_t;

#define IR_NULL_BUTTON	(0x3fffffff)

static ir_translation_t *ir_current_longpress = NULL;
static unsigned long *ir_translate_table = NULL, ir_init_buttoncode = 0x3ffffff0, ir_init_car = 0, ir_init_home = 0;

#define KNOBDATA_BITS 3
#ifdef EMPEG_KNOB_SUPPORTED
static unsigned long ir_knob_busy = 0, ir_knob_down = 0;

typedef struct knob_pair_s {
	unsigned long	pressed;
	unsigned long	released;
} knob_pair_t;

#define KNOBDATA_SIZE (1 << KNOBDATA_BITS)
static int knobdata_index = 0;
static const char *knobdata_labels[] = {" [default] ", " PopUp ", " VolAdj+ ", " Source ", " Hush ", " Info ", " Mark ", " Shuffle "};
static const knob_pair_t knobdata_pairs[1<<KNOBDATA_BITS] = {
	{IR_KNOB_PRESSED,		IR_KNOB_RELEASED},
	{IR_KNOB_PRESSED,		IR_KNOB_RELEASED},
	{IR_KNOB_PRESSED,		IR_KNOB_RELEASED},
	{IR_RIO_SOURCE_PRESSED,		IR_RIO_SOURCE_RELEASED},
	{IR_RIO_HUSH_PRESSED,		IR_RIO_HUSH_PRESSED},
	{IR_RIO_INFO_PRESSED,		IR_RIO_INFO_RELEASED},
	{IR_RIO_MARK_PRESSED,		IR_RIO_MARK_RELEASED},
	{IR_RIO_SHUFFLE_PRESSED,	IR_RIO_SHUFFLE_RELEASED} };

#define KNOBMENU_SIZE KNOBDATA_SIZE	// indexes share the same bits in flash..
static int knobmenu_index = 0;
static const char *knobmenu_labels[] = {" [default] ", " Details ", " Info ", " Mark ", " Repeat ", " SelectMode ", " Shuffle ", " Visual "};
static const knob_pair_t knobmenu_pairs[KNOBMENU_SIZE] = {
	{IR_KNOB_PRESSED,		IR_KNOB_RELEASED},
	{IR_RIO_INFO_PRESSED,		IR_RIO_INFO_PRESSED},
	{IR_RIO_INFO_PRESSED,		IR_RIO_INFO_RELEASED},
	{IR_RIO_MARK_PRESSED,		IR_RIO_MARK_RELEASED},
	{IR_RIO_REPEAT_PRESSED,		IR_RIO_REPEAT_RELEASED},
	{IR_RIO_SELECTMODE_PRESSED,	IR_RIO_SELECTMODE_PRESSED},
	{IR_RIO_SHUFFLE_PRESSED,	IR_RIO_SHUFFLE_RELEASED},
	{IR_RIO_VISUAL_PRESSED,		IR_RIO_VISUAL_RELEASED} };
#endif

// How button press/release events are handled:
//
// button -> interrupt -> input_append_code() -> inputq -> hijack_handle_button() -> playerq -> real_input_append_code()
//
// hijack.c::input_append_code() performs raw IR translations
// hijack.c::inputq[] holds translated buttons with timing information queued for hijack to examine
// hijack.c::hijack_handle_button() interprets buttons for hijack/menu functions
// hijack.c::playerq[] holds translated buttons with timing information queued for the Empeg player software
// empeg_input.c::real_input_append_code() feeds buttons to userland
//
// The flow is slightly different for userland apps which hijack buttons:
//
// button -> interrupt -> input_append_code() -> inputq -> hijack_handle_button() -> userq -> ioctl()
//
// Currently, userq[] does not hold useful timing information

#define HIJACK_BUTTONQ_SIZE	48
typedef struct hijack_buttondata_s {
	unsigned long delay;	// inter-button delay interval
	unsigned long button;	// button press/release code
} hijack_buttondata_t;

typedef struct hijack_buttonq_s {
	unsigned short		head;
	unsigned short		tail;
	unsigned long		last_deq;
	hijack_buttondata_t	data[HIJACK_BUTTONQ_SIZE];
} hijack_buttonq_t;

// Automatic volume adjustment parameters
#define MULT_POINT		12
#define MULT_MASK		((1 << MULT_POINT) - 1)
#define VOLADJ_THRESHSIZE	16
#define VOLADJ_HISTSIZE		128	/* must be a power of two */
#define VOLADJ_FIXEDPOINT(whole,fraction) ((((whole)<<MULT_POINT)|((unsigned int)((fraction)*(1<<MULT_POINT))&MULT_MASK)))
#define VOLADJ_BITS 2
int hijack_voladj_enabled = 0; // used by voladj code in empeg_audio3.c
static const char  *voladj_names[] = {" [Off] ", " Low ", " Medium ", " High "};
static unsigned int voladj_history[VOLADJ_HISTSIZE] = {0,}, voladj_last_histx = 0, voladj_histx = 0;
static unsigned int hijack_voladj_parms[(1<<VOLADJ_BITS)-1][5] = { // Values as suggested by Richard Lovejoy
	{0x1800,	 100,	0x1000,	25,	60},  // Low
	{0x2000,	 409,	0x1000,	27,	70},  // Medium (Normal)
	{0x2000,	3000,	0x0c00,	30,	80}}; // High

// Externally tuneable parameters for config.ini; the voladj_parms are also tuneable
static hijack_buttonq_t hijack_inputq, hijack_playerq, hijack_userq;
static int hijack_button_pacing			=  4;	// minimum spacing between press/release pairs within playerq
static int hijack_temperature_correction	= -4;	// adjust all h/w temperature readings by this celcius amount

typedef struct hijack_option_s {
	const char	*name;
	int		*target;
	int		num_items;
	int		min;
	int		max;
} hijack_option_t; 

static const hijack_option_t hijack_option_table[] = {
	// config.ini string		address-of-variable		howmany	min	max
	{"temperature_correction",	&hijack_temperature_correction,	1,	-20,	+20},
	{"button_pacing",		&hijack_button_pacing,		1,	0,	HZ},
	{"voladj_low",			&hijack_voladj_parms[0][0],	5,	0,	0x7ffe},
	{"voladj_medium",		&hijack_voladj_parms[1][0],	5,	0,	0x7ffe},
	{"voladj_high",			&hijack_voladj_parms[2][0],	5,	0,	0x7ffe},
	{"old_style",			&hijack_classic,		1,	0,	1},
	{NULL,NULL,0,0,0} // end-of-list
	};

#define HIJACK_USERQ_SIZE	8
static const unsigned long intercept_all_buttons[] = {1};
static const unsigned long *hijack_buttonlist = NULL;
//static unsigned long hijack_userq[HIJACK_USERQ_SIZE];
//static unsigned short hijack_userq_head = 0, hijack_userq_tail = 0;
static struct wait_queue *hijack_userq_waitq = NULL, *hijack_menu_waitq = NULL;

#define SCREEN_BLANKER_MULTIPLIER 15
#define BLANKER_BITS 6
static int blanker_timeout = 0;

#define BLANKERFUZZ_MULTIPLIER 5
#define BLANKERFUZZ_BITS 3
static int blankerfuzz_amount = 0;

#define MAXTEMP_OFFSET	34
#define MAXTEMP_BITS	5
static int maxtemp_threshold = 0;

#define DCPOWER_BITS 1
int hijack_force_dcpower = 0; // used by empeg_power.c

#define TIMERACTION_BITS 1
static int timer_timeout = 0, timer_started = 0, timer_action = 0;

#define MENU_BITS	5
#define MENU_MAX_ITEMS	(1<<MENU_BITS)
typedef int  (menu_dispfunc_t)(int);
typedef void (menu_movefunc_t)(int);
typedef struct menu_item_s {
	char			*label;
	menu_dispfunc_t		*dispfunc;
	menu_movefunc_t		*movefunc;
	unsigned long		userdata;
} menu_item_t;
static volatile short menu_item = 0, menu_size = 0, menu_top = 0;

// format of eight-byte flash memory savearea used for our hijack'd settings
static struct sa_struct {
	unsigned voladj_ac_power	: VOLADJ_BITS;		// 2 bits
	unsigned blanker_timeout	: BLANKER_BITS;		// 6 bits
	unsigned voladj_dc_power	: VOLADJ_BITS;		// 2 bits
	unsigned maxtemp_threshold	: MAXTEMP_BITS;		// 5 bits
	unsigned restore_visuals	: 1;
	unsigned fsck_disabled		: 1;
	unsigned byte3_leftover		: 5;
	unsigned timer_action		: TIMERACTION_BITS;	// 1 bit
	unsigned force_dcpower		: DCPOWER_BITS;		// 1 bit
	unsigned knob_ac		: 1+KNOBDATA_BITS;	// 4 bits
	unsigned knob_dc		: 1+KNOBDATA_BITS;	// 4 bits
	unsigned byte5_leftover		: 8;
	unsigned byte6_leftover		: 8;
	unsigned menu_item		: MENU_BITS;		// 5 bits
	unsigned blankerfuzz_amount	: BLANKERFUZZ_BITS;	// 3 bits
} hijack_savearea;

static unsigned char hijack_displaybuf[EMPEG_SCREEN_ROWS][EMPEG_SCREEN_COLS/2];

const unsigned char kfont [1 + '~' - ' '][KFONT_WIDTH] = {  // variable width font
	{0x00,0x00,0x00,0x00,0x00,0x00}, // space
	{0x5f,0x00,0x00,0x00,0x00,0x00}, // !
	{0x03,0x00,0x03,0x00,0x00,0x00}, // "
	{0x14,0x3e,0x14,0x3e,0x14,0x00}, // #
	{0x24,0x2a,0x7f,0x2a,0x12,0x00}, // $
	{0x26,0x16,0x08,0x34,0x32,0x00}, // %
	{0x36,0x49,0x56,0x20,0x50,0x00}, // ampersand
	{0x02,0x01,0x00,0x00,0x00,0x00}, // singlequote
	{0x3e,0x41,0x00,0x00,0x00,0x00}, // (
	{0x41,0x3e,0x00,0x00,0x00,0x00}, // )
	{0x22,0x14,0x7f,0x14,0x22,0x00}, // *
	{0x08,0x08,0x3e,0x08,0x08,0x00}, // +
	{0x80,0x40,0x00,0x00,0x00,0x00}, // ,
	{0x08,0x08,0x08,0x08,0x00,0x00}, // -
	{0x40,0x00,0x00,0x00,0x00,0x00}, // .
	{0x20,0x10,0x08,0x04,0x02,0x00}, // /
	{0x3e,0x51,0x49,0x45,0x3e,0x00}, // 0
	{0x40,0x42,0x7f,0x40,0x40,0x00}, // 1
	{0x42,0x61,0x51,0x49,0x46,0x00}, // 2
	{0x22,0x49,0x49,0x49,0x36,0x00}, // 3
	{0x18,0x14,0x12,0x7f,0x10,0x00}, // 4
	{0x2f,0x49,0x49,0x49,0x31,0x00}, // 5
	{0x3e,0x49,0x49,0x49,0x32,0x00}, // 6
	{0x01,0x01,0x61,0x19,0x07,0x00}, // 7
	{0x36,0x49,0x49,0x49,0x36,0x00}, // 8
	{0x26,0x49,0x49,0x49,0x3e,0x00}, // 9
	{0x24,0x00,0x00,0x00,0x00,0x00}, // :
	{0x40,0x24,0x00,0x00,0x00,0x00}, // ;
	{0x08,0x14,0x22,0x41,0x00,0x00}, // <
	{0x14,0x14,0x14,0x14,0x00,0x00}, // =
	{0x41,0x22,0x14,0x08,0x00,0x00}, // >
	{0002,0xb1,0x09,0x09,0x06,0x00}, // ?
	{0x3e,0x41,0x59,0x55,0x4e,0x00}, // @
	{0x7e,0x09,0x09,0x09,0x7e,0x00}, // A
	{0x7f,0x49,0x49,0x49,0x36,0x00}, // B
	{0x3e,0x41,0x41,0x41,0x22,0x00}, // C
	{0x7f,0x41,0x41,0x41,0x3e,0x00}, // D
	{0x7f,0x49,0x49,0x41,0x41,0x00}, // E
	{0x7f,0x09,0x09,0x01,0x01,0x00}, // F
	{0x3e,0x41,0x41,0x49,0x3a,0x00}, // G
	{0x7f,0x08,0x08,0x08,0x7f,0x00}, // H
	{0x41,0x7f,0x41,0x00,0x00,0x00}, // I
	{0x20,0x41,0x41,0x3f,0x01,0x00}, // J
	{0x7f,0x08,0x14,0x22,0x41,0x00}, // K
	{0x7f,0x40,0x40,0x40,0x40,0x00}, // L
	{0x7f,0x02,0x04,0x02,0x7f,0x00}, // M
	{0x7f,0x04,0x08,0x10,0x7f,0x00}, // N
	{0x3e,0x41,0x41,0x41,0x3e,0x00}, // O
	{0x7f,0x09,0x09,0x09,0x06,0x00}, // P
	{0x3e,0x41,0x51,0x21,0x5e,0x00}, // Q
	{0x7f,0x09,0x19,0x29,0x46,0x00}, // R
	{0x26,0x49,0x49,0x49,0x32,0x00}, // S
	{0x01,0x01,0x7f,0x01,0x01,0x00}, // T
	{0x3f,0x40,0x40,0x40,0x3f,0x00}, // U
	{0x1f,0x20,0x40,0x20,0x1f,0x00}, // V
	{0x7f,0x20,0x10,0x20,0x7f,0x00}, // W
	{0x63,0x14,0x08,0x14,0x63,0x00}, // X
	{0x03,0x04,0x78,0x04,0x03,0x00}, // Y
	{0x61,0x51,0x49,0x45,0x43,0x00}, // Z
	{0x7f,0x41,0x41,0x00,0x00,0x00}, // [
	{0x02,0x04,0x08,0x10,0x20,0x00}, // backslash
	{0x41,0x41,0x7f,0x00,0x00,0x00}, // ]
	{0x02,0x01,0x02,0x00,0x00,0x00}, // ^
	{0x80,0x80,0x80,0x80,0x00,0x00}, // _
	{0x01,0x02,0x00,0x00,0x00,0x00}, // `
	{0x38,0x44,0x24,0x78,0x00,0x00}, // a
	{0x7f,0x44,0x44,0x38,0x00,0x00}, // b
	{0x38,0x44,0x44,0x28,0x00,0x00}, // c
	{0x38,0x44,0x44,0x7f,0x00,0x00}, // d
	{0x38,0x54,0x54,0x58,0x00,0x00}, // e
	{0x08,0x7e,0x09,0x01,0x02,0x00}, // f
	{0x98,0xa4,0xa4,0x78,0x00,0x00}, // g
	{0x7f,0x04,0x04,0x78,0x00,0x00}, // h
	{0x7d,0x00,0x00,0x00,0x00,0x00}, // i
	{0x40,0x80,0x7d,0x00,0x00,0x00}, // j
	{0x7f,0x10,0x28,0x44,0x00,0x00}, // k
	{0x7f,0x00,0x00,0x00,0x00,0x00}, // l
	{0x7c,0x04,0x38,0x04,0x78,0x00}, // m
	{0x7c,0x04,0x04,0x78,0x00,0x00}, // n
	{0x38,0x44,0x44,0x38,0x00,0x00}, // o
	{0xfc,0x24,0x24,0x18,0x00,0x00}, // p
	{0x18,0x24,0x24,0xf8,0x00,0x00}, // q
	{0x7c,0x08,0x04,0x08,0x00,0x00}, // r
	{0x48,0x54,0x54,0x20,0x00,0x00}, // s
	{0x04,0x3e,0x44,0x24,0x00,0x00}, // t
	{0x3c,0x40,0x40,0x7c,0x00,0x00}, // u
	{0x1c,0x20,0x40,0x20,0x1c,0x00}, // v
	{0x3c,0x40,0x38,0x40,0x3c,0x00}, // w
	{0x44,0x28,0x10,0x28,0x44,0x00}, // x
	{0x9c,0xa0,0xa0,0x7c,0x00,0x00}, // y
	{0x64,0x54,0x4c,0x00,0x00,0x00}, // z
	{0x08,0x3e,0x41,0x00,0x00,0x00}, // {
	{0x7f,0x00,0x00,0x00,0x00,0x00}, // |
	{0x41,0x3e,0x08,0x00,0x00,0x00}, // }
	{0x02,0x01,0x02,0x04,0x02,0x00}  // ~
	};

static void
clear_hijack_displaybuf (int color)
{
	color &= 3;
	color |= color << 4;
	memset(hijack_displaybuf, color, EMPEG_SCREEN_BYTES);
}

static void
draw_pixel (unsigned short pixel_row, unsigned short pixel_col, int color)
{
	unsigned char *pixel_pair, pixel_mask;
	if (color < 0)
		color = -color;
	color &= 3;
	color |= color << 4;
	pixel_pair  = &hijack_displaybuf[pixel_row][pixel_col >> 1];
	pixel_mask  = (pixel_col & 1) ? 0xf0 : 0x0f;
	*pixel_pair = (*pixel_pair & ~pixel_mask) ^ (color & pixel_mask);
}

static void
draw_hline (unsigned short pixel_row, unsigned short pixel_col, unsigned short last_col, int color)
{
	while (pixel_col <= last_col)
		draw_pixel(pixel_row, pixel_col++, color);
}

static hijack_geom_t *hijack_overlay_geom = NULL;

static void
hijack_do_overlay (unsigned char *dest, unsigned char *src, const hijack_geom_t *geom)
{
	// for simplicity, we only do pixels in pairs
	unsigned short offset;
	short row = geom->first_row, last_col = (geom->last_col - geom->first_col) / 2;
	offset = (row * (EMPEG_SCREEN_COLS / 2)) + (geom->first_col / 2);
	for (row = geom->last_row - row; row >= 0; --row) {
		short col;
		for (col = last_col; col >= 0; --col)
			dest[offset + col] = src[offset + col];
		offset += (EMPEG_SCREEN_COLS / 2);
	}
}

#ifdef EMPEG_KNOB_SUPPORTED
static void
draw_frame (unsigned char *dest, const hijack_geom_t *geom)
{
	// draw a frame inside geom, one pixel smaller all around
	// for simplicity, we only do pixels in pairs
	unsigned short offset, top_or_bottom = 1;
	short row, last_byte_offset = (geom->last_col - geom->first_col - 4) / 2;
	offset = ((geom->first_row + 1) * (EMPEG_SCREEN_COLS / 2)) + (geom->first_col / 2) + 1;
	for (row = (geom->last_row - geom->first_row - 2); row >= 0;) {
		short col;
		dest[offset + 0] = dest[offset + last_byte_offset] = 0x11;
		if (top_or_bottom)
			for (col = last_byte_offset-1; col > 0; --col)
				dest[offset + col] = 0x11;
		offset += (EMPEG_SCREEN_COLS / 2);
		top_or_bottom = (--row > 0) ? 0 : 1;
	}
}

static void
clear_text_row (unsigned int rowcol, unsigned short last_col, int do_top_row)
{
	unsigned short num_cols, last_row, row = (rowcol & 0xffff), pixel_col = (rowcol >> 16);

	num_cols = 1 + last_col - pixel_col;
	if (row & 0x8000)
		row = (row & ~0x8000) * KFONT_HEIGHT;
	last_row = row + (KFONT_HEIGHT - 1);
	if (do_top_row && row > 0)
		--row;
	while (row <= last_row) {
		unsigned int offset = 0;
		unsigned char *displayrow, pixel_mask = (pixel_col & 1) ? 0xf0 : 0x0f;
		if (row >= EMPEG_SCREEN_ROWS)
			return;
		displayrow = &hijack_displaybuf[row++][0];
		do {
			unsigned char *pixel_pair = &displayrow[(pixel_col + offset) >> 1];
			*pixel_pair &= (pixel_mask = ~pixel_mask);
		} while (++offset < num_cols);
	}
}
#endif // EMPEG_KNOB_SUPPORTED

static unsigned char kfont_spacing = 0;  // 0 == proportional

static int
draw_char (unsigned short pixel_row, short pixel_col, unsigned char c, unsigned char foreground, unsigned char background)
{
	unsigned char num_cols;
	const unsigned char *font_entry;
	unsigned char *displayrow;

	if (pixel_row >= EMPEG_SCREEN_ROWS)
		return 0;
	displayrow = &hijack_displaybuf[pixel_row][0];
	if (c > '~' || c < ' ')
		c = ' ';
	font_entry = &kfont[c - ' '][0];
	if (!(num_cols = kfont_spacing)) {  // variable width font spacing?
		if (c == ' ')
			num_cols = 3;
		else
			for (num_cols = KFONT_WIDTH; !font_entry[num_cols-2]; --num_cols);
	}
	if ((pixel_col + num_cols) > EMPEG_SCREEN_COLS)
		return -1;
	for (pixel_row = 0; pixel_row < KFONT_HEIGHT; ++pixel_row) {
		unsigned char pixel_mask = (pixel_col & 1) ? 0xf0 : 0x0f;
		unsigned int offset = 0;
		do {
			unsigned char font_bit    = font_entry[offset] & (1 << pixel_row);
			unsigned char new_pixel   = (font_bit ? foreground : background) & pixel_mask;
			unsigned char *pixel_pair = &displayrow[(pixel_col + offset) >> 1];
			*pixel_pair = ( (*pixel_pair & (pixel_mask = ~pixel_mask)) ) | new_pixel;
		} while (++offset < num_cols);
		displayrow += (EMPEG_SCREEN_COLS / 2);
	}
	return num_cols;
}

// 0x8000 in rowcol means "text_row"; otherwise "pixel_row"
#define ROWCOL(text_row,pixel_col)  ((unsigned int)(((pixel_col)<<16)|((text_row)|0x8000)))
#define ROUND_CORNERS

static unsigned int
draw_string (unsigned int rowcol, const unsigned char *s, int color)
{
	unsigned short row = (rowcol & 0xffff), col = (rowcol >> 16);
	unsigned char background, foreground;
#ifdef ROUND_CORNERS
	unsigned char firstchar;
	int firstcol = (EMPEG_SCREEN_COLS*2), firstrow = 0;
#endif // ROUND_CORNERS

	if (!s || !*s)
		return rowcol;
#ifdef ROUND_CORNERS
	firstchar = *s;
#endif // ROUND_CORNERS
	if (row & 0x8000)
		row = (row & ~0x8000) * KFONT_HEIGHT;
	if (color < 0) {
		background = (COLOR1<<4)|COLOR1;
		foreground = (COLOR3<<4)|COLOR3;
	} else {
		background = (COLOR0<<4)|COLOR0;
		foreground = (color <<4)|color;
	}
top:	if (row < EMPEG_SCREEN_ROWS) {
		unsigned char c;
		while ((c = *s)) {
			int col_adj;
			if ((c == '\n' && *s++) || -1 == (col_adj = draw_char(row, col, c, foreground, background))) {
				col  = 0;
				row += KFONT_HEIGHT;
				goto top;
			}
#ifdef ROUND_CORNERS
			if (firstcol == (EMPEG_SCREEN_COLS*2)) {
				firstcol = col;
				firstrow = row;
			}
#endif // ROUND_CORNERS
			col += col_adj;
			++s;
		}
	}
	rowcol = (col << 16) | row;
#ifdef ROUND_CORNERS
	if (background && firstchar == ' ' && col-- > firstcol && row == firstrow && *(s-1) == ' ') {	// round corners?
		draw_pixel(row + (KFONT_HEIGHT - 1), firstcol, COLOR0);
		draw_pixel(row + (KFONT_HEIGHT - 1), col, COLOR0);
		if (row > 0)
			draw_hline(--row, firstcol+1, col-1, background);
		draw_pixel(row, firstcol, COLOR0);
		draw_pixel(row, col, COLOR0);
	}
#endif // ROUND_CORNERS
	return rowcol;
}

static unsigned int
draw_number (unsigned int rowcol, unsigned int number, const char *format, int color)
{
	unsigned char buf[16];
	unsigned char saved;

	sprintf(buf, format, number);
	saved = kfont_spacing;
	kfont_spacing = KFONT_WIDTH; // use fixed font spacing for numbers
	rowcol = draw_string(rowcol, buf, color);
	kfont_spacing = saved;
	return rowcol;

}

static void
hijack_initq (hijack_buttonq_t *q)
{
	q->head = q->tail = q->last_deq = 0;
}

static void
hijack_button_enq (hijack_buttonq_t *q, unsigned long button, unsigned long delay)
{
	unsigned short head;

	head = q->head;
	if (head != q->tail && delay < hijack_button_pacing && (button & 0x80000001) == 0 && q == &hijack_playerq)
		delay = hijack_button_pacing;	// ensure we have sufficient delay between press/release pairs
	if (++head >= HIJACK_BUTTONQ_SIZE)
		head = 0;
	if (head != q->tail) {
		hijack_buttondata_t *data = &q->data[q->head = head];
		data->button = button;
		data->delay  = delay;
	}
}

static int
hijack_button_deq (hijack_buttonq_t *q, hijack_buttondata_t *rdata, int nowait)
{
	hijack_buttondata_t *data;
	unsigned short tail;
	unsigned long flags;

	save_flags_cli(flags);
	if ((tail = q->tail) != q->head) {	// anything in the queue?
		if (++tail >= HIJACK_BUTTONQ_SIZE)
			tail = 0;
		data = &q->data[tail];
		if (nowait || !data->delay || jiffies_since(q->last_deq) >= data->delay) {
			rdata->button = data->button;
			rdata->delay  = data->delay;
			q->tail = tail;
			q->last_deq = jiffies;
			restore_flags(flags);
			return 1;	// button was available
		}
	}
	restore_flags(flags);
	return 0;	// no buttons available
}

static void
hijack_deactivate (int new_status)
{
	unsigned long flags;

	save_flags_cli(flags);
	ir_selected = 0;
	ir_numeric_input = NULL;
	hijack_movefunc = NULL;
	hijack_dispfunc = NULL;
	hijack_buttonlist = NULL;
	ir_trigger_count = 0;
	hijack_overlay_geom = NULL;
	hijack_status = new_status;
	restore_flags(flags);
}

static void
activate_dispfunc (int (*dispfunc)(int), void (*movefunc)(int))
{
	hijack_deactivate(HIJACK_PENDING);
	hijack_dispfunc = dispfunc;
	hijack_movefunc = movefunc;
	dispfunc(1);
}

static const unsigned int voladj_thresholds[VOLADJ_THRESHSIZE] = {
	VOLADJ_FIXEDPOINT(1,.00),
	VOLADJ_FIXEDPOINT(1,.20),
	VOLADJ_FIXEDPOINT(1,.50),
	VOLADJ_FIXEDPOINT(1,.75),
	VOLADJ_FIXEDPOINT(2,.00),
	VOLADJ_FIXEDPOINT(2,.33),
	VOLADJ_FIXEDPOINT(2,.66),
	VOLADJ_FIXEDPOINT(3,.00),
	VOLADJ_FIXEDPOINT(3,.50),
	VOLADJ_FIXEDPOINT(4,.00),
	VOLADJ_FIXEDPOINT(5,.00),
	VOLADJ_FIXEDPOINT(6,.50),
	VOLADJ_FIXEDPOINT(8,.00),
	VOLADJ_FIXEDPOINT(11,.00),
	VOLADJ_FIXEDPOINT(15,.50),
	VOLADJ_FIXEDPOINT(20,.00)};

// Plot a nice moving history of voladj multipliers, VOLADJ_THRESHSIZE pixels in height.
// The effective resolution (height) is tripled by using brighter color shades for the in-between values.
//
static unsigned int
voladj_plot (unsigned short text_row, unsigned short pixel_col, unsigned int multiplier, int *prev)
{
	if (text_row < (EMPEG_TEXT_ROWS - 1)) {
		int mdiff, tdiff;
		unsigned int height = VOLADJ_THRESHSIZE-1, pixel_row, threshold, color;
		do {
			threshold = voladj_thresholds[height];
		} while (multiplier < threshold && --height != 0);
		pixel_row = (text_row * KFONT_HEIGHT) + (VOLADJ_THRESHSIZE-1) - height;
		mdiff = multiplier - threshold;
		if (height < (VOLADJ_THRESHSIZE-1))
			tdiff = voladj_thresholds[height+1] - threshold;
		else
			tdiff = threshold / 2;
		if (mdiff >= (2 * tdiff / 3))
			color = COLOR3;
		else if (mdiff >= (tdiff / 3))
			color = COLOR2;
		else
			color = COLOR1;
		draw_pixel(pixel_row, pixel_col, color);
		if (*prev >= 0) {
			if (*prev < pixel_row) {
				while (++(*prev) < pixel_row)
					draw_pixel(*prev, pixel_col, COLOR2);
			} else if (*prev > pixel_row) {
				while (--(*prev) > pixel_row)
					draw_pixel(*prev, pixel_col, COLOR2);
			}
		}
		*prev = pixel_row;
		++pixel_col;
	}
	return pixel_col;
}

// invoked from the voladj code in empeg_audio3.c
void
hijack_voladj_update_history (int multiplier)
{
	unsigned long flags;
	save_flags_cli(flags);
	voladj_histx = (voladj_histx + 1) & (VOLADJ_HISTSIZE-1);
	voladj_history[voladj_histx] = multiplier;
	restore_flags(flags);
}

static void
hijack_set_voladj_parms (void)
{
	empeg_state_dirty = 1;
	if (hijack_voladj_enabled) {
		unsigned const int *p = hijack_voladj_parms[hijack_voladj_enabled - 1];
		hijack_voladj_intinit(p[0],p[1],p[2],p[3],p[4]);	
	}
}

static void
voladj_move (int direction)
{
	unsigned int old = hijack_voladj_enabled;

	hijack_voladj_enabled += direction;
	if (hijack_voladj_enabled < 0)
		hijack_voladj_enabled = 0;
	else if (hijack_voladj_enabled >= ((1<<VOLADJ_BITS)-1))
		hijack_voladj_enabled   = ((1<<VOLADJ_BITS)-1);
	if (hijack_voladj_enabled != old)
		hijack_set_voladj_parms();
}

static int
voladj_display (int firsttime)
{
	int prev = -1;
	unsigned int histx, col, rowcol, mult;
	unsigned char buf[32];
	unsigned long flags;

	if (!firsttime && !hijack_last_moved && voladj_histx == voladj_last_histx)
		return NO_REFRESH;
	hijack_last_moved = 0;
	clear_hijack_displaybuf(COLOR0);
	rowcol = draw_string(ROWCOL(0,0), "Auto Volume Adjust: ", PROMPTCOLOR);
	(void)draw_string(rowcol, voladj_names[hijack_voladj_enabled], ENTRYCOLOR);
	save_flags_cli(flags);
	histx = voladj_last_histx = voladj_histx;
	mult  = voladj_history[histx];
	restore_flags(flags);
	sprintf(buf, "Current Multiplier: %2u.%02u", mult >> MULT_POINT, (mult & MULT_MASK) * 100 / (1 << MULT_POINT));
	(void)draw_string(ROWCOL(3,12), buf, PROMPTCOLOR);
	for (col = 0; col < VOLADJ_HISTSIZE; ++col)
		(void)voladj_plot(1, col, voladj_history[++histx & (VOLADJ_HISTSIZE-1)], &prev);
	return NEED_REFRESH;
}

#ifdef EMPEG_KNOB_SUPPORTED
static int
voladj_prefix_display (int firsttime)
{
	static const hijack_geom_t geom = {8, 8+6+KFONT_HEIGHT, 12, EMPEG_SCREEN_COLS-12};

	ir_selected = 0; // paranoia?
	if (firsttime) {
		hijack_last_moved = jiffies ? jiffies : -1;
		clear_hijack_displaybuf(COLOR0);
		draw_frame((unsigned char *)hijack_displaybuf, &geom);
		hijack_overlay_geom = (hijack_geom_t *)&geom;
	} else if (jiffies_since(hijack_last_moved) >= (HZ*3)) {
		hijack_deactivate(HIJACK_INACTIVE);
	} else {
		unsigned int rowcol = (geom.first_row+4)|((geom.first_col+6)<<16);
		rowcol = draw_string(rowcol, "Auto VolAdj: ", COLOR3);
		clear_text_row(rowcol, geom.last_col-4, 1);
		rowcol = draw_string(rowcol, voladj_names[hijack_voladj_enabled], ENTRYCOLOR);
		return NEED_REFRESH;
	}
	return NO_REFRESH;
}
#endif // EMPEG_KNOB_SUPPORTED

static int
kfont_display (int firsttime)
{
	unsigned int rowcol;
	unsigned char c;

	if (!firsttime)
		return NO_REFRESH;
	clear_hijack_displaybuf(COLOR0);
	rowcol = draw_string(ROWCOL(0,0), " ", -COLOR3);
	for (c = (unsigned char)' '; c <= (unsigned char)'~'; ++c) {
		unsigned char s[2] = {0,0};
		s[0] = c;
		rowcol = draw_string(rowcol, &s[0], COLOR3);
	}
	return NEED_REFRESH;
}

static int
init_temperature (void)
{
	static unsigned long inittherm_lasttime = 0;
	unsigned long flags;

	// restart the thermometer once every five minutes or so
	if (inittherm_lasttime && jiffies_since(inittherm_lasttime) < (HZ*5*60))
		return 0;
	inittherm_lasttime = jiffies ? jiffies : -1;
	save_flags_clif(flags);
	(void)empeg_inittherm(&OSMR0,&GPLR);
	restore_flags(flags);
	return 1;
}

static int
read_temperature (void)
{
	static unsigned long readtherm_lasttime = 0;
	static int temperature = 0;
	unsigned long flags;

	// Hugo (altman) writes:
	//   You shouldn't need to do inittherm repeatedly - just once.
	//   It starts the chip doing continuous conversions - or should do,
	//   if the config byte is set up for this (which it should be).
	//   Writing the config byte is dangerous, as I said before.
	//   
	//   It takes between 0.5s-1s to do a conversion.
	//   Read temperature reads the result of the last conversion.
	//   
	//   If you're trying to prevent HDD damage at low temperatures, use GPIO16;
	//   you can use this to hold the HDDs in reset, which puts them into deep sleep mode.
	//   The initial boot code has stuff in there to do this, which is currently disabled;
	//   the delay on reading the thermometer would add ~1s to boot time as you wouldn't
	//   be able to let the drives out of reset until you'd read the temperature.
	//
	// But in practice, the danged thing stops updating sometimes
	// so we may still need the odd call to empeg_inittherm() just to ensure
	// it is running.  -ml

	if ((readtherm_lasttime && jiffies_since(readtherm_lasttime) < (HZ*5)) || init_temperature())
		return temperature;
	readtherm_lasttime = jiffies ? jiffies : -1;
	save_flags_clif(flags);
	temperature = empeg_readtherm(&OSMR0,&GPLR);
	restore_flags(flags);
	/* Correct for negative temperatures (sign extend) */
	if (temperature & 0x80)
		temperature = -(128 - (temperature ^ 0x80));
	temperature += hijack_temperature_correction;
	return temperature;
}

static unsigned int
draw_temperature (unsigned int rowcol, int temp, int offset, int color)
{
	unsigned char buf[32];
	sprintf(buf, " %+dC/%+dF ", temp, temp * 180 / 100 + offset);
	return draw_string(rowcol, buf, color);
}

static int            savearea_display_offset = 0;
static unsigned char *last_savearea;
static unsigned long *last_updated  = NULL;
unsigned char **empeg_state_writebuf;	// initialized in empeg_state.c

static void
savearea_move (int direction)
{
	savearea_display_offset = (savearea_display_offset + (direction * 8)) & 0x7f;
}

static int
savearea_display (int firsttime)
{
	unsigned int rc = NO_REFRESH;
	unsigned char *empeg_statebuf = *empeg_state_writebuf;
	if (firsttime) {
		if (!last_savearea)
			last_savearea  = kmalloc(128, GFP_KERNEL);
		if (last_savearea)
			memcpy(last_savearea, empeg_statebuf, 128);
		if (!last_updated)
			last_updated  = kmalloc(128 * sizeof(long), GFP_KERNEL);
		if (last_updated)
			memset(last_updated, 0, 128 * sizeof(long));
		rc = NEED_REFRESH;
	} else if (jiffies_since(hijack_last_refresh) >= (HZ/4)) {
		rc = NEED_REFRESH;
	}
	if (rc == NEED_REFRESH) {
		int offset, row = 0, rowcol = ROWCOL(0,0);
		for (offset = savearea_display_offset; offset != ((savearea_display_offset + 32) & 0x7f);) {
			int color = COLOR2;
			unsigned char b = empeg_statebuf[offset];
			if (last_updated && last_savearea) {
				unsigned long elapsed;
				if (b != last_savearea[offset])
					last_updated[offset] = jiffies ? jiffies : -1;
				last_savearea[offset] = b;
				if (last_updated[offset]) {
					elapsed = jiffies_since(last_updated[offset]);
					if (elapsed < (10*HZ))
						color = (elapsed < (3*HZ)) ? -COLOR3 : COLOR3;
					else
						last_updated[offset] = 0;
				}
			}
			if ((offset & 7) == 0)
				rowcol = draw_number(ROWCOL(row++,0), offset, "%02x:", COLOR1);
			if ((offset & 1) == 0)
				rowcol = draw_string(rowcol, " ", COLOR0);
			rowcol = draw_number(rowcol, b, "%02X", color);
			offset = (offset + 1) & 0x7f;
		}
	}
	if (ir_selected) {	// not 100% effective, but good enough
		if (last_savearea) {
			kfree(last_savearea);
			last_savearea = NULL;
		}
		if (last_updated) {
			kfree(last_updated);
			last_updated = NULL;
		}
	}
	return rc;
}

static int
vitals_display (int firsttime)
{
	unsigned int *permset=(unsigned int*)(EMPEG_FLASHBASE+0x2000);
	unsigned char buf[80];
	int rowcol, i, count;
	struct sysinfo si;

	if (!firsttime && jiffies_since(hijack_last_refresh) < (HZ*2))
		return NO_REFRESH;
	clear_hijack_displaybuf(COLOR0);
	sprintf(buf, "Rev:%02d, Jiffies:%08lX\nTemperature:", permset[0], jiffies);
	rowcol = draw_string(ROWCOL(0,0), buf, PROMPTCOLOR);
	(void)draw_temperature(rowcol, read_temperature(), 32, PROMPTCOLOR);
	si_meminfo(&si);
	sprintf(buf, "Free: %lukB/%lukB\nLoadAvg: ", si.freeram/1024, si.totalram/1024);
	rowcol = draw_string(ROWCOL(2,0), buf, PROMPTCOLOR);
	(void)get_loadavg(buf);
	count = 0;
	for (i = 0;; ++i) {
		if (buf[i] == ' ' && ++count == 3)
			break;
	}
	buf[i] = '\0';
	(void)draw_string(rowcol, buf, PROMPTCOLOR);
	return NEED_REFRESH;
}

static void
forcedc_move (int direction)
{
	hijack_savearea.force_dcpower = !hijack_savearea.force_dcpower;
}

static int
forcedc_display (int firsttime)
{
	unsigned int rowcol;

	if (!firsttime && !hijack_last_moved)
		return NO_REFRESH;
	hijack_last_moved = 0;
	clear_hijack_displaybuf(COLOR0);
	(void)draw_string(ROWCOL(0,0), "Force DC/Car Operation", PROMPTCOLOR);
	rowcol = draw_string(ROWCOL(1,0), "Current Mode: ", PROMPTCOLOR);
	(void)draw_string(rowcol, on_dc_power ? "DC/Car" : "AC/Home", PROMPTCOLOR);
	rowcol = draw_string(ROWCOL(3,0), "On reboot: ", PROMPTCOLOR);
	(void)draw_string(rowcol, hijack_savearea.force_dcpower ? " Force DC/Car " : " [Normal] ", ENTRYCOLOR);
	return NEED_REFRESH;
}

static unsigned int
draw_hhmmss (unsigned int rowcol, unsigned int seconds, int color)
{
	unsigned char buf[32];
	unsigned int minutes = 0;

	if (seconds >= 60) {
		minutes = seconds / 60;
		seconds = seconds % 60;
		if (minutes > 60) {
			unsigned int hours = minutes / 60;
			minutes = minutes % 60;
			sprintf(buf, " %02u:%02u:%02u ", hours, minutes, seconds);
			goto draw;
		}
	}
	sprintf(buf, " %02u:%02u ", minutes, seconds);
draw:	return draw_string(rowcol, buf, color);
}

static void
timer_move (int direction)
{
	const int max = (99 * 60 * 60);  // Max 99 hours
	int new;

	if (direction == 0) {
		timer_timeout = 0;
	} else {
		if (timer_timeout >= (60 * 60))
			direction *= (10 * 60);
		else if (timer_timeout >= (4 * 60))
			direction *= 30;
		else if (timer_timeout >= (2 * 60))
			direction *= 15;
		new = timer_timeout + direction;
		if (new < 0)
			timer_timeout = 0;
		else if (new > max)
			timer_timeout = max;
		else
			timer_timeout = new;
		// Acceleration
		if (ir_move_repeat_delay > 2)
			ir_move_repeat_delay -= 2;
	}
}

static int
timer_display (int firsttime)
{
	static int paused = 0;
	unsigned int rowcol;
	unsigned char *offmsg = " [Off] ";

	if (firsttime) {
		paused = 0;
		if (timer_timeout) {  // was timer already running?
			int remaining = timer_timeout - (jiffies_since(timer_started) / HZ);
			if (remaining > 0) {
				timer_timeout = remaining;
				paused = 1;
			} else {
				timer_timeout = 0;  // turn alarm off if it was on
				offmsg = " [Cancelled] ";
			}
		}
		ir_numeric_input = &timer_timeout;
	}
	timer_started = jiffies;
	if (!firsttime && !hijack_last_moved)
		return NO_REFRESH;
	hijack_last_moved = 0;
	clear_hijack_displaybuf(COLOR0);
	(void)draw_string(ROWCOL(0,0), "Countdown Timer Timeout", PROMPTCOLOR);
	rowcol = draw_string(ROWCOL(2,0), "Duration: ", PROMPTCOLOR);
	if (timer_timeout) {
		rowcol = draw_hhmmss(rowcol, timer_timeout, ENTRYCOLOR);
		if (paused)
			(void)draw_string(rowcol, " [paused]", PROMPTCOLOR);
	} else {
		paused = 0;
		(void)draw_string(rowcol, offmsg, ENTRYCOLOR);
	}
	return NEED_REFRESH;
}

static void
fsck_move (int direction)
{
	hijack_fsck_disabled = (direction < 0);
	empeg_state_dirty = 1;
}

static int
fsck_display (int firsttime)
{
	unsigned int rowcol;

	if (!firsttime && !hijack_last_moved)
		return NO_REFRESH;
	hijack_last_moved = 0;
	clear_hijack_displaybuf(COLOR0);
	(void)draw_string(ROWCOL(0,0), "Filesystem Check on Sync:", PROMPTCOLOR);
	rowcol = draw_string(ROWCOL(2,0), "Periodic checking: ", PROMPTCOLOR);
	(void)   draw_string(rowcol, hijack_fsck_disabled ? " Disabled " : " Enabled ", ENTRYCOLOR);
	return NEED_REFRESH;
}

#ifdef RESTORE_CARVISUALS
static void
carvisuals_move (int direction)
{
	carvisuals_enabled = (direction > 0);
	empeg_state_dirty = 1;
}

static int
carvisuals_display (int firsttime)
{
	unsigned int rowcol;

	if (!firsttime && !hijack_last_moved)
		return NO_REFRESH;
	hijack_last_moved = 0;
	clear_hijack_displaybuf(COLOR0);
	(void)draw_string(ROWCOL(0,0), "Restore DC/Car Visuals", PROMPTCOLOR);
	rowcol = draw_string(ROWCOL(2,20), "Restore: ", PROMPTCOLOR);
	(void)   draw_string(rowcol, carvisuals_enabled ? " Enabled " : " Disabled ", ENTRYCOLOR);
	return NEED_REFRESH;
}
#endif // RESTORE_CARVISUALS

static void
timeraction_move (int direction)
{
	timer_action = (direction > 0);
	empeg_state_dirty = 1;
}

static int
timeraction_display (int firsttime)
{
	unsigned int rowcol;

	if (!firsttime && !hijack_last_moved)
		return NO_REFRESH;
	hijack_last_moved = 0;
	clear_hijack_displaybuf(COLOR0);
	(void)draw_string(ROWCOL(0,0), "Countdown Timer Action", PROMPTCOLOR);
	rowcol = draw_string(ROWCOL(2,0), "On expiry: ", PROMPTCOLOR);
	(void)   draw_string(rowcol, timer_action ? " Beep Alarm " : " Toggle Standby ", ENTRYCOLOR);
	return NEED_REFRESH;
}

static int maxtemp_check_threshold (void)
{
	static unsigned long beeping, elapsed;

	if (!maxtemp_threshold || read_temperature() < (maxtemp_threshold + MAXTEMP_OFFSET))
		return 0;
	elapsed = jiffies_since(ir_lasttime) / HZ;
	if (elapsed < 1) {
		beeping = 0;
	} else if (((elapsed >> 2) & 7) == (beeping & 7)) {
		int volume = 3 * ((++beeping >> 2) & 15) + 15;
		hijack_beep(90, 280, volume);
	}
	if (hijack_status == HIJACK_INACTIVE && elapsed > 4) {
		unsigned int rowcol;
		int color = (elapsed & 1) ? COLOR3 : -COLOR3;
		clear_hijack_displaybuf(color);
		rowcol = draw_string(ROWCOL(2,18), " Too Hot:", -color);
		(void)draw_temperature(rowcol, read_temperature(), 32, -color);
		return 1;
	}
	return 0;
}

static void
blanker_move (int direction)
{
	const int max = ((1<<BLANKER_BITS)-1);
	int new;

	if (direction == 0) {
		blanker_timeout = 0;
	} else {
		new = blanker_timeout + direction;
		if (new < 0)
			blanker_timeout = 0;
		else if (new > max)
			blanker_timeout = max;
		else
			blanker_timeout = new;
		// Acceleration
		if (ir_move_repeat_delay > 2)
			ir_move_repeat_delay -= 2;
	}
	empeg_state_dirty = 1;
}

static int
blanker_display (int firsttime)
{
	unsigned int rowcol;

	if (firsttime)
		ir_numeric_input = &blanker_timeout;
	else if (!hijack_last_moved)
		return NO_REFRESH;
	hijack_last_moved = 0;
	clear_hijack_displaybuf(COLOR0);
	(void)draw_string(ROWCOL(0,0), "Screen Inactivity Blanker", PROMPTCOLOR);
	rowcol = draw_string(ROWCOL(2,20), "Timeout: ", PROMPTCOLOR);
	if (blanker_timeout) {
		(void)draw_hhmmss(rowcol, blanker_timeout * SCREEN_BLANKER_MULTIPLIER, ENTRYCOLOR);
	} else {
		(void)draw_string(rowcol, " [Off] ", ENTRYCOLOR);
	}
	return NEED_REFRESH;
}

static void
blankerfuzz_move (int direction)
{
	blankerfuzz_amount += direction;
	if (blankerfuzz_amount < 0 || direction == 0)
		blankerfuzz_amount = 0;
	else if (blankerfuzz_amount > ((1<<BLANKERFUZZ_BITS)-1))
		blankerfuzz_amount  = ((1<<BLANKERFUZZ_BITS)-1);
	empeg_state_dirty = 1;
}

static int
blankerfuzz_display (int firsttime)
{
	unsigned int rowcol;

	if (firsttime)
		ir_numeric_input = &blankerfuzz_amount;
	else if (!hijack_last_moved)
		return NO_REFRESH;
	hijack_last_moved = 0;
	clear_hijack_displaybuf(COLOR0);
	(void)draw_string(ROWCOL(0,0), "Screen Blanker Sensitivity", PROMPTCOLOR);
	rowcol = draw_string(ROWCOL(2,0), "Examine ", PROMPTCOLOR);
	rowcol = draw_number(rowcol, 100 - (blankerfuzz_amount * BLANKERFUZZ_MULTIPLIER), " %u%% ", ENTRYCOLOR);
	(void)   draw_string(rowcol, " of screen", PROMPTCOLOR);
	return NEED_REFRESH;
}

static int
screen_compare (unsigned long *screen1, unsigned long *screen2)
{
	const unsigned char bitcount4[16] = {0,1,1,2, 1,2,2,3, 1,2,3,4, 2,3,3,4};
	int allowable_fuzz = blankerfuzz_amount * (BLANKERFUZZ_MULTIPLIER * (2 * EMPEG_SCREEN_BYTES) / 100);
	unsigned long *end = screen1 - 1;

	// Compare backwards, since changes occur most frequently near bottom of screen
	screen1 += (EMPEG_SCREEN_BYTES / sizeof(unsigned long)) - 1;
	screen2 += (EMPEG_SCREEN_BYTES / sizeof(unsigned long)) - 1;

	do {	// compare 8 pixels at a time for speed
		unsigned long x = *screen1-- ^ *screen2--;
		if (x) { // Now figure out how many of the 8 pixels didn't match
			x |= x >>  1;		// reduce each pixel to one bit
			x &= 0x11111111;	// mask away the excess bits
			x |= x >> 15;		// move the upper 4 pixels to beside the lower 4
			x |= x >>  6;		// Squish all eight pixels into one byte
			allowable_fuzz -= bitcount4[x & 0xf] + bitcount4[(x >> 4) & 0xf];
			if (allowable_fuzz < 0)
				return 1;	// not the same
		}
	} while (screen1 > end);
	return 0;	// the same
}

#ifdef EMPEG_KNOB_SUPPORTED

static void
send_knob_pair (const knob_pair_t *kd)
{
	hijack_button_enq(&hijack_playerq, kd->pressed, 0);
	if (kd->pressed == kd->released) // simulate a long button press
		hijack_button_enq(&hijack_playerq, kd->pressed|0x80000000, HZ);
	else
		hijack_button_enq(&hijack_playerq, kd->released, 0);
}

static void
knobdata_move (int direction)
{
	knobdata_index = direction ? (knobdata_index + direction) & (KNOBDATA_SIZE-1) : 0;
	empeg_state_dirty = 1;
}

static int
knobdata_display (int firsttime)
{
	unsigned int rowcol;

	if (firsttime)
		ir_numeric_input = &knobdata_index;	// allows cancel/top to reset it to 0
	else if (!hijack_last_moved)
		return NO_REFRESH;
	hijack_last_moved = 0;
	clear_hijack_displaybuf(COLOR0);
	(void)draw_string(ROWCOL(0,0), "Knob Press Redefinition", PROMPTCOLOR);
	rowcol = draw_string(ROWCOL(2,0), "Quick press = ", PROMPTCOLOR);
	(void)draw_string(rowcol, knobdata_labels[knobdata_index], ENTRYCOLOR);
	return NEED_REFRESH;
}

static unsigned long knobmenu_pressed;
static const unsigned long knobmenu_buttons[3] = {3, IR_KNOB_PRESSED, IR_KNOB_RELEASED}; 

static void
knobmenu_move (int direction)
{
	if (direction == 0) {
		hijack_deactivate(HIJACK_INACTIVE);
	} else if (!knobmenu_pressed) {
		knobmenu_index += direction;
		if (knobmenu_index < 0)
			knobmenu_index = 0;
		else if (knobmenu_index >= KNOBMENU_SIZE)
			knobmenu_index = KNOBMENU_SIZE-1;
		empeg_state_dirty = 1;
	}
}

static int
knobmenu_display (int firsttime)
{
	unsigned long flags;
	hijack_buttondata_t data;
	static const hijack_geom_t knobmenu_geom = {8, 8+6+KFONT_HEIGHT, 4, EMPEG_SCREEN_COLS-4};

	ir_selected = 0; // paranoia?
	if (firsttime) {
		knobmenu_pressed = 0;
		hijack_last_moved = jiffies ? jiffies : -1;
		ir_numeric_input = &knobmenu_index;	// allows cancel/top to reset it to 0
		hijack_buttonlist = knobmenu_buttons;;
		hijack_initq(&hijack_userq);
		clear_hijack_displaybuf(COLOR0);
		draw_frame((unsigned char *)hijack_displaybuf, &knobmenu_geom);
		hijack_overlay_geom = (hijack_geom_t *)&knobmenu_geom;
	}
	if (!knobmenu_pressed) {
		if (jiffies_since(hijack_last_moved) >= (HZ*4)) {
			hijack_deactivate(HIJACK_INACTIVE);
		} else {
			unsigned int rowcol = (knobmenu_geom.first_row+4)|((knobmenu_geom.first_col+6)<<16);
			rowcol = draw_string(rowcol, "Select Action: ", COLOR3);
			clear_text_row(rowcol, knobmenu_geom.last_col-4, 1);
			(void)draw_string(rowcol, knobmenu_labels[knobmenu_index], ENTRYCOLOR);
		}
	}
	save_flags_cli(flags);
	while (hijack_status == HIJACK_ACTIVE && hijack_button_deq(&hijack_userq, &data, 0)) {
		if (!knobmenu_pressed && data.button == IR_KNOB_PRESSED) {
			knobmenu_pressed = jiffies ? jiffies : -1;
			//hijack_button_enq(&hijack_playerq, knobmenu_pairs[knobmenu_index].pressed, 0);
			send_knob_pair(&knobmenu_pairs[knobmenu_index]);
		} else if (knobmenu_pressed && data.button == IR_KNOB_RELEASED) {
			//hijack_button_enq(&hijack_playerq, knobmenu_pairs[knobmenu_index].pressed|0x80000000, jiffies_since(knobmenu_pressed));
			hijack_deactivate(HIJACK_INACTIVE);
		}
	}
	restore_flags(flags);
	return NEED_REFRESH;
}

#endif // EMPEG_KNOB_SUPPORTED

#define GAME_COLS		(EMPEG_SCREEN_COLS/2)
#define GAME_VBOUNCE		0xff
#define GAME_BRICKS		0xee
#define GAME_BRICKS_ROW		5
#define GAME_HBOUNCE		0x77
#define GAME_BALL		0xff
#define GAME_OVER		0x11
#define GAME_PADDLE_SIZE	8

static short game_over, game_row, game_col, game_hdir, game_vdir, game_paddle_col, game_paddle_lastdir, game_speed, game_bricks;
static unsigned long game_ball_last_moved, game_animtime;
static unsigned int *game_animptr = NULL;

static int
game_finale (void)
{
	static int framenr, frameadj;
	unsigned char *d,*s;
	int a;

	if (game_bricks) {  // Lost game?
		if (jiffies_since(game_ball_last_moved) < (HZ*3/2))
			return NO_REFRESH;
		if (game_animtime++ == 0) {
			(void)draw_string(ROWCOL(1,20), " Enhancements.v83 ", -COLOR3);
			(void)draw_string(ROWCOL(2,33), "by Mark Lord", COLOR3);
			return NEED_REFRESH;
		}
		if (jiffies_since(game_ball_last_moved) < (HZ*3))
			return NO_REFRESH;
		ir_selected = 1; // return to menu
		return NEED_REFRESH;
	}
	if (jiffies_since(game_animtime) < (HZ/(ANIMATION_FPS-2))) {
		(void)draw_string(ROWCOL(2,44), "You Win!", COLOR3);
		return NEED_REFRESH;
	}
	if (game_animtime == 0) { // first frame?
		framenr = 0;
		frameadj = 1;
	} else if (framenr < 0) { // animation finished?
		ir_selected = 1; // return to menu
		return NEED_REFRESH;
	} else if (!game_animptr[framenr]) { // last frame?
		frameadj = -1;  // play it again, backwards
		framenr += frameadj;
	}
	s = (unsigned char *)game_animptr + game_animptr[framenr];
	d = (unsigned char *)hijack_displaybuf;
	for(a=0;a<2048;a+=2) {
		*d++=((*s&0xc0)>>2)|((*s&0x30)>>4);
		*d++=((*s&0x0c)<<2)|((*s&0x03));
		s++;
	}
	framenr += frameadj;
	game_animtime = jiffies ? jiffies : -1;
	return NEED_REFRESH;
}

static void
game_move (int direction)
{
	unsigned char *paddlerow = hijack_displaybuf[EMPEG_SCREEN_ROWS-3];
	int i = 3;
	unsigned long flags;
	save_flags_cli(flags);
	while (i-- > 0) {
		if (direction < 0) {
			if (game_paddle_col > 1) {
				paddlerow[--game_paddle_col + GAME_PADDLE_SIZE] = 0;
				if (paddlerow[game_paddle_col] != 0)
					--game_row; // scoop up the ball
				paddlerow[game_paddle_col] = GAME_VBOUNCE;
			}
		} else {
			if (game_paddle_col < (GAME_COLS - GAME_PADDLE_SIZE - 1)) {
				paddlerow[game_paddle_col] = 0;
				if (paddlerow[game_paddle_col + GAME_PADDLE_SIZE] != 0)
					--game_row; // scoop up the ball
				paddlerow[game_paddle_col++ + GAME_PADDLE_SIZE] = GAME_VBOUNCE;
			}
		}
	}
	game_paddle_lastdir = direction;
	restore_flags(flags);
}

static void
game_nuke_brick (short row, short col)
{
	if (col > 0 && col < (GAME_COLS-1) && hijack_displaybuf[row][col] == GAME_BRICKS) {
		hijack_displaybuf[row][col] = 0;
		if (--game_bricks <= 0)
			game_over = 1;
	}
}

static int
game_move_ball (void)
{
	unsigned long flags;

	save_flags_cli(flags);
	ir_selected = 0; // prevent accidental exit from game
	if (ir_left_down && jiffies_since(ir_left_down) >= (HZ/15)) {
		ir_left_down = jiffies ? jiffies : -1;
		game_move(-1);
	} else if (ir_right_down && jiffies_since(ir_right_down) >= (HZ/15)) {
		ir_right_down = jiffies ? jiffies : -1;
		game_move(1);
	}
	if (jiffies_since(game_ball_last_moved) < (HZ/game_speed)) {
		restore_flags(flags);
		return (jiffies_since(hijack_last_moved) > jiffies_since(game_ball_last_moved)) ? NO_REFRESH : NEED_REFRESH;
	}
	game_ball_last_moved = jiffies;
	hijack_displaybuf[game_row][game_col] = 0;
	game_row += game_vdir;
	game_col += game_hdir;
	if (hijack_displaybuf[game_row][game_col] == GAME_HBOUNCE) {
		// need to bounce horizontally
		game_hdir = 0 - game_hdir;
		game_col += game_hdir;
	}
	if (game_row == GAME_BRICKS_ROW) {
		game_nuke_brick(game_row,game_col-1);
		game_nuke_brick(game_row,game_col);
		game_nuke_brick(game_row,game_col+1);
	}
	if (hijack_displaybuf[game_row][game_col] == GAME_VBOUNCE) {
		// need to bounce vertically
		game_vdir = 0 - game_vdir;
		game_row += game_vdir;
		// if we hit a moving paddle, adjust the ball speed
		if (game_row > 1 && jiffies_since(hijack_last_moved) <= (HZ/8)) {
			if (game_paddle_lastdir == game_hdir) {
				if (game_speed < (HZ/3))
					game_speed += 5;
			} else if (game_paddle_lastdir) {
				game_speed = (game_speed > 14) ? game_speed - 3 : 11;
			}
			game_paddle_lastdir = 0; // prevent multiple adjusts per paddle move
		}
	}
	if (hijack_displaybuf[game_row][game_col] == GAME_OVER) {
		(void)draw_string(ROWCOL(2,44), "Game Over", COLOR3);
		game_over = 1;
	}
	hijack_displaybuf[game_row][game_col] = GAME_BALL;
	restore_flags(flags);
	return NEED_REFRESH;
}

static int
game_display (int firsttime)
{
	int i;

	if (!firsttime)
		return game_over ? game_finale() : game_move_ball();
	clear_hijack_displaybuf(COLOR0);
	game_paddle_col = (GAME_COLS - GAME_PADDLE_SIZE) / 2;
	for (i = 0; i < GAME_COLS; ++i) {
		hijack_displaybuf[0][i] = GAME_VBOUNCE;
		hijack_displaybuf[EMPEG_SCREEN_ROWS-1][i] = GAME_OVER;
		hijack_displaybuf[GAME_BRICKS_ROW][i] = GAME_BRICKS;
	}
	for (i = 0; i < EMPEG_SCREEN_ROWS; ++i)
		hijack_displaybuf[i][0] = hijack_displaybuf[i][GAME_COLS-1] = GAME_HBOUNCE;
	memset(&hijack_displaybuf[EMPEG_SCREEN_ROWS-3][game_paddle_col],GAME_VBOUNCE,GAME_PADDLE_SIZE);
	game_hdir = 1;
	game_vdir = 1;
	game_row = 6;
	game_col = ((unsigned long)jiffies) % GAME_COLS;
	if (hijack_displaybuf[game_row][game_col])
		game_col = game_col ? GAME_COLS - 2 : 1;
	game_ball_last_moved = 0;
	game_paddle_lastdir = 0;
	game_bricks = GAME_COLS - 2;
	game_over = 0;
	game_speed = 16;
	game_animtime = 0;
	return NEED_REFRESH;
}

static void
maxtemp_move (int direction)
{
	if (maxtemp_threshold == 0 && direction > 0)
		maxtemp_threshold = 55 - MAXTEMP_OFFSET;
	else
		maxtemp_threshold += direction;
	if (maxtemp_threshold < 0 || direction == 0)
		maxtemp_threshold = 0;
	else if (maxtemp_threshold > ((1<<MAXTEMP_BITS)-1))
		maxtemp_threshold  = ((1<<MAXTEMP_BITS)-1);
	empeg_state_dirty = 1;
}

static int
maxtemp_display (int firsttime)
{
	unsigned int rowcol;

	if (firsttime)
		ir_numeric_input = &maxtemp_threshold;
	else if (!hijack_last_moved)
		return NO_REFRESH;
	hijack_last_moved = 0;
	clear_hijack_displaybuf(COLOR0);
	rowcol = draw_string(ROWCOL(0,0), "High Temperature Warning.", PROMPTCOLOR);
	rowcol = draw_string(ROWCOL(1,0), "Threshold: ", PROMPTCOLOR);
	if (maxtemp_threshold)
		(void)draw_temperature(rowcol, maxtemp_threshold + MAXTEMP_OFFSET, 32, ENTRYCOLOR);
	else
		rowcol = draw_string(rowcol, " [Off] ", ENTRYCOLOR);
	rowcol = draw_string(ROWCOL(2,0), "Currently: ", PROMPTCOLOR);
	(void)draw_temperature(rowcol, read_temperature(), 32, PROMPTCOLOR);
	rowcol = draw_string(ROWCOL(3,0), "Corrected by: ", PROMPTCOLOR);
	(void)draw_temperature(rowcol, hijack_temperature_correction, 0, PROMPTCOLOR);
	return NEED_REFRESH;
}

#define CALCULATOR_BUTTONS_SIZE	(1 + (13 * 4))
static const unsigned long calculator_buttons[CALCULATOR_BUTTONS_SIZE] = {CALCULATOR_BUTTONS_SIZE,
	IR_KW_0_PRESSED,	IR_KW_0_RELEASED,	IR_RIO_0_PRESSED,	IR_RIO_0_RELEASED,
	IR_KW_1_PRESSED,	IR_KW_1_RELEASED,	IR_RIO_1_PRESSED,	IR_RIO_1_RELEASED,
	IR_KW_2_PRESSED,	IR_KW_2_RELEASED,	IR_RIO_2_PRESSED,	IR_RIO_2_RELEASED,
	IR_KW_3_PRESSED,	IR_KW_3_RELEASED,	IR_RIO_3_PRESSED,	IR_RIO_3_RELEASED,
	IR_KW_4_PRESSED,	IR_KW_4_RELEASED,	IR_RIO_4_PRESSED,	IR_RIO_4_RELEASED,
	IR_KW_5_PRESSED,	IR_KW_5_RELEASED,	IR_RIO_5_PRESSED,	IR_RIO_5_RELEASED,
	IR_KW_6_PRESSED,	IR_KW_6_RELEASED,	IR_RIO_6_PRESSED,	IR_RIO_6_RELEASED,
	IR_KW_7_PRESSED,	IR_KW_7_RELEASED,	IR_RIO_7_PRESSED,	IR_RIO_7_RELEASED,
	IR_KW_8_PRESSED,	IR_KW_8_RELEASED,	IR_RIO_8_PRESSED,	IR_RIO_8_RELEASED,
	IR_KW_9_PRESSED,	IR_KW_9_RELEASED,	IR_RIO_9_PRESSED,	IR_RIO_9_RELEASED,
	IR_KW_CD_PRESSED,	IR_KW_CD_RELEASED,	IR_RIO_MENU_PRESSED,	IR_RIO_MENU_RELEASED,
	IR_KW_STAR_RELEASED,	IR_KW_STAR_PRESSED,	IR_RIO_CANCEL_RELEASED,	IR_RIO_CANCEL_PRESSED,
	IR_TOP_BUTTON_RELEASED,	IR_TOP_BUTTON_PRESSED,	IR_KNOB_PRESSED,	IR_KNOB_RELEASED};

static const unsigned char calculator_operators[] = {'+','-','*','/','%','='};

static long
calculator_do_op (long total, long value, long operator)
{
	switch (calculator_operators[operator]) {
		case '+': total += value; break;
		case '-': total -= value; break;
		case '*': total *= value; break;
		case '/': total  = value ? total / value : 0; break;
		case '%': total  = value ? total % value : 0; break;
		case '=': total  = value; break;
	}
	return total;
}

static int
calculator_display (int firsttime)
{
	static long total, value;
	static unsigned char operator, prev;
	long new;
	unsigned int i;
	unsigned long flags;
	unsigned char opstring[3] = {' ','\0'};
	hijack_buttondata_t data;

	save_flags_cli(flags);
	if (firsttime) {
		total = value = operator = prev = 0;
		hijack_buttonlist = calculator_buttons;;
		hijack_initq(&hijack_userq);
	} else while (hijack_button_deq(&hijack_userq, &data, 0)) {
		for (i = CALCULATOR_BUTTONS_SIZE-1; i > 0; --i) {
			if (data.button == calculator_buttons[i])
				break;
		}
		if (i & 1) { // very clever:  if (first_or_third_column_from_table) {
			i = (i - 1) / 4;
			switch (i) {
				case 10: // CD or MENU: toggle operators
					if (prev < 10) {
						total = calculator_do_op(total, value, operator);
						value = operator = 0;
					} else {
						operator = (operator + 1) % sizeof(calculator_operators);
					}
					break;
				case 11: // * or CANCEL: CE,CA,Quit
					if (value) {
						value = 0;
						break;
					}
					if (total) {
						total = 0;
						break;
					}
					// fall thru
				case 12: // TOP or KNOB: Quit
					hijack_buttonlist = NULL;
					ir_selected = 1; // return to main menu
					break;
				default: // 0,1,2,3,4,5,6,7,8,9
					new = (value * 10) + i;
					if ((new / 10) == value)
						value = new;
					break;
			}
			prev = i;
		}
	}
	restore_flags(flags);
	clear_hijack_displaybuf(COLOR0);
	(void) draw_string(ROWCOL(0,0), "Menu/CD: +-*/%=", PROMPTCOLOR);
	(void) draw_string(ROWCOL(1,0), "Cancel/*: CE,CA,Quit", PROMPTCOLOR);
	(void) draw_number(ROWCOL(2,8), total, "%11d ", ENTRYCOLOR);
	opstring[0] = calculator_operators[operator];
	(void) draw_string(ROWCOL(3,0), opstring, ENTRYCOLOR);
	(void) draw_number(ROWCOL(3,8), value, "%11d ", ENTRYCOLOR);
	return NEED_REFRESH;
}

static int
reboot_display (int firsttime)
{
	static unsigned char left_pressed, right_pressed;
	unsigned long flags;
	hijack_buttondata_t data;
	int rc;

        if (firsttime) {
		clear_hijack_displaybuf(COLOR0);
		(void) draw_string(ROWCOL(0,0), "Press & hold Left/Right\n  buttons to reboot.\n\nAny other button aborts", PROMPTCOLOR);
		left_pressed = right_pressed = 0;
		hijack_buttonlist = intercept_all_buttons;
		hijack_initq(&hijack_userq);
		return NEED_REFRESH;
	}
	if (left_pressed && right_pressed) {
		save_flags_cli(flags);
		state_cleanse();	// Ensure flash savearea is updated first
		restore_flags(flags);	// never executed (?)
		machine_restart(NULL);	// Reboot the machine NOW!  Fails in Tuner mode..
		return NEED_REFRESH;
	}
	rc = NO_REFRESH;
	save_flags_cli(flags);
	if (hijack_button_deq(&hijack_userq, &data, 0)) {
		switch (data.button) {
			case IR_LEFT_BUTTON_PRESSED:
				left_pressed++;
				break;
			case IR_RIGHT_BUTTON_PRESSED:
				right_pressed++;
				break;
			default:
				if ((data.button & 0x80000001) == 0) {
					hijack_buttonlist = NULL;
					ir_selected = 1; // return to main menu
				}
				break;
		}
		if (left_pressed && right_pressed) {
			clear_hijack_displaybuf(COLOR0);
			(void) draw_string(ROWCOL(2,30), "Rebooting..", PROMPTCOLOR);
			rc = NEED_REFRESH;
			// reboot on next refresh, AFTER screen has been updated on this pass
		}
	}
	restore_flags(flags);
	return rc;
}

static int
showbutton_display (int firsttime)
{
	static unsigned long *saved_table, prev[4], counter;
	unsigned long flags;
	hijack_buttondata_t data;
	int i;

	save_flags_cli(flags);
	if (firsttime) {
		counter = 0;
		prev[0] = prev[1] = prev[2] = prev[3] = -1;
		hijack_buttonlist = intercept_all_buttons;
		hijack_initq(&hijack_userq);
		// disable IR translations
		saved_table = ir_translate_table;
		ir_translate_table = NULL;
	}
	if (hijack_button_deq(&hijack_userq, &data, 0)) {
		if (++counter > 99)
			counter = 0;
		if (prev[0] == -1 && (data.button & 0x80000000)) {
			// ignore it: left over from selecting us off the menu
		} else if (data.button == prev[1] && ((data.button & 0x80000000) || data.button < 0x10)) {
			ir_translate_table = saved_table;
			hijack_buttonlist = NULL;
			ir_selected = 1; // return to main menu
		} else {
			for (i = 2; i >= 0; --i)
				prev[i+1] = prev[i];
			prev[0] = data.button;
		}
	}
	restore_flags(flags);
	if (firsttime || prev[0] != -1) {
		unsigned long rowcol;
		clear_hijack_displaybuf(COLOR0);
		rowcol=draw_string(ROWCOL(0,0), "Button Codes Display.  ", PROMPTCOLOR);
		(void) draw_number(rowcol, counter, " %02d ", ENTRYCOLOR);
		(void) draw_string(ROWCOL(1,0), "Repeat any button to exit", PROMPTCOLOR);
		if (prev[3] != -1)
			(void)draw_number(ROWCOL(2,4), prev[3], "%08X", PROMPTCOLOR);
		if (prev[2] != -1)
			(void)draw_number(ROWCOL(2,(EMPEG_SCREEN_COLS/2)), prev[2], " %08X ", PROMPTCOLOR);
		if (prev[1] != -1)
			(void)draw_number(ROWCOL(3,4), prev[1], "%08X", PROMPTCOLOR);
		if (prev[0] != -1)
			(void)draw_number(ROWCOL(3,(EMPEG_SCREEN_COLS/2)), prev[0], " %08X ", ENTRYCOLOR);
		return NEED_REFRESH;
	}
	return NO_REFRESH;
}

static menu_item_t menu_table [MENU_MAX_ITEMS] = {
	{" Auto Volume Adjust ",	voladj_display,		voladj_move,		0},
	{" Break-Out Game ",		game_display,		game_move,		0},
	{" Button Codes Display ",	showbutton_display,	NULL,			0},
	{" Calculator ",		calculator_display,	NULL,			0},
	{" Countdown Timer Timeout ",	timer_display,		timer_move,		0},
	{" Countdown Timer Action ",	timeraction_display,	timeraction_move,	0},
	{" Filesystem Check on Sync ",	fsck_display,		fsck_move,		0},
	{" Font Display ",		kfont_display,		NULL,			0},
	{" Force DC/Car Mode ",		forcedc_display,	forcedc_move,		0},
	{" High Temperature Warning ",	maxtemp_display,	maxtemp_move,		0},
#ifdef EMPEG_KNOB_SUPPORTED
	{" Knob Press Redefinition ",	knobdata_display,	knobdata_move,		0},
#endif // EMPEG_KNOB_SUPPORTED
	{" Reboot Machine ",		reboot_display,		NULL,			0},
#ifdef RESTORE_CARVISUALS
	{" Restore DC/Car Visuals ",	carvisuals_display,	carvisuals_move,	0},
#endif // RESTORE_CARVISUALS
	{" Screen Blanker Timeout ",	blanker_display,	blanker_move,		0},
	{" Screen Blanker Sensitivity ", blankerfuzz_display,	blankerfuzz_move,	0},
	{" Show Flash Savearea ",	savearea_display,	savearea_move,		0},
	{" Vital Signs ",		vitals_display,		NULL,			0},
	{NULL,				NULL,			NULL,			0},};

static void
menu_move (int direction)
{
	short max = menu_size - 1;

	if (direction < 0) {
		if (menu_item == menu_top && --menu_top < 0)
			menu_top = max;
		if (--menu_item < 0)
			menu_item = max;
	} else {
		short bottom = (menu_top + (EMPEG_TEXT_ROWS - 1)) % menu_size;
		if (menu_item == bottom && ++menu_top > max)
			menu_top = 0;
		if (++menu_item > max)
			menu_item = 0;
	}
	empeg_state_dirty = 1;
}

static int
menu_display (int firsttime)
{
	unsigned long flags;
	if (firsttime || hijack_last_moved) {
		unsigned int text_row;
		ir_lasttime = jiffies;	// prevent premature exit from menu
		hijack_last_moved = 0;
		clear_hijack_displaybuf(COLOR0);
		save_flags_cli(flags);
		for (text_row = 0; text_row < EMPEG_TEXT_ROWS; ++text_row) {
			unsigned int index = (menu_top + text_row) % menu_size;
			//unsigned int color = (index == menu_item) ? ENTRYCOLOR : PROMPTCOLOR;
			unsigned int color = (index == menu_item) ? ENTRYCOLOR : COLOR2;
			(void)draw_string(ROWCOL(text_row,0), menu_table[index].label, color);
		}
		restore_flags(flags);
		return NEED_REFRESH;
	}
	save_flags_cli(flags);
	if (ir_selected) {
		menu_item_t *item = &menu_table[menu_item];
		activate_dispfunc(item->dispfunc, item->movefunc);
	} else if (jiffies_since(ir_lasttime) > (HZ*5))
		hijack_deactivate(HIJACK_INACTIVE_PENDING); // menu timed-out
	restore_flags(flags);
	return NO_REFRESH;
}

static int userland_display_updated = 0;

// this is invoked when a userland menu item is active
static int
userland_display (int firsttime)
{
	unsigned long flags;
	int rc = NO_REFRESH;

	if (firsttime) {
		clear_hijack_displaybuf(COLOR0);
		userland_display_updated = 1;
	}
	save_flags_cli(flags);
	if (firsttime)
		wake_up(&hijack_menu_waitq);
	if (userland_display_updated) {
		userland_display_updated = 0;
		rc = NEED_REFRESH;
	}
	restore_flags(flags);
	return rc;
}

static void
hijack_move (int direction)
{
	if (hijack_status == HIJACK_ACTIVE) {
		if (hijack_movefunc != NULL)
			hijack_movefunc(direction);
		hijack_last_moved = jiffies ? jiffies : -1;
	}
}

static void
hijack_move_repeat (void)
{
	if (ir_left_down && jiffies_since(ir_left_down) >= ir_move_repeat_delay) {
		ir_left_down = jiffies ? jiffies : -1;
		hijack_move(-1);
		ir_lasttime = jiffies;
	} if (ir_right_down && jiffies_since(ir_right_down) >= ir_move_repeat_delay) {
		ir_right_down = jiffies ? jiffies : -1;
		hijack_move(+1);
		ir_lasttime = jiffies;
	}
}

static int
test_row (unsigned char *displaybuf, unsigned short row, unsigned char color)
{
	const unsigned int offset = 10;
	unsigned char *first = displaybuf + (row * (EMPEG_SCREEN_COLS/2)) + offset;
	unsigned char *test  = first + ((EMPEG_SCREEN_COLS/2) - (offset << 1));
	do {
		if (*--test != color)
			return 0;
	} while (test > first);
	return 1;
}

static int
check_if_equalizer_is_active (unsigned char *displaybuf)
{
	unsigned char *row = displaybuf + (13 * (EMPEG_SCREEN_COLS/2));
	const unsigned char eqrow[] = {	0xf0,0xff,0xff,0xf2,0xff,0xff,
					0xf2,0xff,0xff,0xf2,0xff,0xff,
					0xf2,0xff,0xff,0xf2,0xff,0xff,
					0xf2,0xff,0xff,0xf2,0xff,0xff };
					//0xf2,0xff,0xff,0xf2,0xff,0xff,
					//0x00,0x00,0x00,0x00,0xff,0xff};
	if (0 == memcmp(row, eqrow, sizeof(eqrow)))
		return 1;	// equalizer settings active
	return 0;	// equalizer settings not active
}

static int
check_if_player_menu_is_active (void *player_buf)
{
	if (test_row(player_buf, 2, 0x00))
		if ((test_row(player_buf, 0, 0x00) && test_row(player_buf, 1, 0x11))
		 || (test_row(player_buf, 3, 0x11) && test_row(player_buf, 4, 0x00)))
			return 1;
	return check_if_equalizer_is_active(player_buf);
}

static int
check_if_sound_adjust_is_active (void *player_buf)
{
	return (test_row(player_buf,  8, 0x00) && test_row(player_buf,  9, 0x11)
	     && test_row(player_buf, 16, 0x11) && test_row(player_buf, 17, 0x00));
}

#ifdef EMPEG_KNOB_SUPPORTED
static void
toggle_input_source (void)
{
	unsigned long button;

	switch (get_current_mixer_source()) {
		case 'T':	// Tuner
			button = IR_KW_TAPE_PRESSED; // aux
			break;
		case 'A':	// Aux
			button = IR_KW_CD_PRESSED;   // player
			break;
		default:	// main/mp3
			// by hitting "aux" before "tuner", we handle "tuner not present"
			hijack_button_enq(&hijack_playerq, IR_KW_TAPE_PRESSED,  0);	// aux
			hijack_button_enq(&hijack_playerq, IR_KW_TAPE_RELEASED, 0);
			button = IR_KW_TUNER_PRESSED;  // tuner
			break;
	}
	hijack_button_enq(&hijack_playerq, button,            0);
	hijack_button_enq(&hijack_playerq, button|0x80000000, 0);
}
#endif // EMPEG_KNOB_SUPPORTED

static int
timer_check_expiry (struct display_dev *dev)
{
	static unsigned long beeping;
	int color, elapsed;

	if (!timer_timeout || 0 > (elapsed = (jiffies_since(timer_started) / HZ) - timer_timeout))
		return 0;
	if (dev->power) {
		if (timer_action == 0) {  // Toggle Standby?
			// Power down the player
			hijack_button_enq(&hijack_playerq, IR_RIO_SOURCE_PRESSED,  0);
			hijack_button_enq(&hijack_playerq, IR_RIO_SOURCE_PRESSED, HZ);
			timer_timeout = 0; // cancel the timer
			return 0;
		}
	} else {
		// A harmless method of waking up the player
		hijack_button_enq(&hijack_playerq, IR_RIO_VOLPLUS_PRESSED,   0);
		hijack_button_enq(&hijack_playerq, IR_RIO_VOLPLUS_RELEASED,  0);
		hijack_button_enq(&hijack_playerq, IR_RIO_VOLMINUS_PRESSED,  0);
		hijack_button_enq(&hijack_playerq, IR_RIO_VOLMINUS_RELEASED, 0);
		if (timer_action == 0) {  // Toggle Standby?
			timer_timeout = 0; // cancel the timer
			return 0;
		}
	}

	// Preselect timer in the menu:
	if (hijack_status == HIJACK_INACTIVE) {
		while (menu_table[menu_item].dispfunc != timer_display)
			menu_move(+1);
		menu_move(+1); menu_move(-1);
	}

	// Beep Alarm
	if (elapsed < 1) {
		beeping = 0;
	} else if (((elapsed >> 2) & 7) == (beeping & 7)) {
		int volume = 3 * ((++beeping >> 2) & 15) + 15;
		hijack_beep(80, 400, volume);
	}
	if (dev->power && hijack_status == HIJACK_INACTIVE && jiffies_since(ir_lasttime) > (HZ*4)) {
		color = (elapsed & 1) ? -COLOR3 : COLOR3;
		clear_hijack_displaybuf(-color);
		(void) draw_string(ROWCOL(2,31), " Timer Expired ", color);
		return 1;
	}
	return 0;
}

static int hijack_check_buttonlist (unsigned long data, unsigned long delay)
{
	int i;

	if (hijack_buttonlist[0] < 2) {
		// empty table means capture EVERYTHING
		hijack_button_enq(&hijack_userq, data, delay);
		wake_up(&hijack_userq_waitq);
		return 1;
	}
	for (i = (hijack_buttonlist[0] - 1); i > 0; --i) {
		if (data == hijack_buttonlist[i]) {
			hijack_button_enq(&hijack_userq, data, delay);
			wake_up(&hijack_userq_waitq);
			return 1;
		}
	}
	return 0;
}

// This routine gets first shot at IR codes as soon as they leave the translator.
//
// In an ideal world, we would never use "jiffies" here, relying on the inter-code "delay" instead.
// But.. maybe later.  The timings are coarse enough that it shouldn't matter much.
//
static int
hijack_handle_button(unsigned long data, unsigned long delay)
{
	static unsigned long ir_lastpressed = -1;
	int hijacked = 0;

	blanker_triggered = 0;
	if (hijack_status == HIJACK_ACTIVE && hijack_buttonlist && hijack_check_buttonlist(data, delay)) {
		hijacked = 1;
		goto done;
	}
	if (hijack_status == HIJACK_ACTIVE && hijack_dispfunc == userland_display)
		goto done;	// just pass key codes straight through to userland
	switch (data) {
#ifdef EMPEG_KNOB_SUPPORTED
		case IR_KNOB_PRESSED:
			hijacked = 1; // hijack it and later send it with the release
			ir_knob_busy = 0;
			ir_knob_down = jiffies ? jiffies : -1;
			if (hijack_status == HIJACK_ACTIVE)
				hijacked = ir_selected = 1;
			break;
		case IR_KNOB_RELEASED:
			if (ir_knob_down) {
				if (ir_knob_busy) {
					ir_knob_busy = 0;
				} else if (hijack_status != HIJACK_INACTIVE) {
					hijacked = 1;
					if (hijack_dispfunc == voladj_prefix_display) {
						hijack_deactivate(HIJACK_INACTIVE);
						hijack_button_enq(&hijack_playerq, IR_KNOB_PRESSED,  0);
						hijack_button_enq(&hijack_playerq, IR_KNOB_RELEASED, 0);
					}
				} else if (hijack_status == HIJACK_INACTIVE) {
					int index = knobdata_index;
					if (player_menu_is_active || player_sound_adjust_is_active)
						index = 0;
					hijacked = 1;
					if (jiffies_since(ir_knob_down) < (HZ/2)) {	// short press?
						if (index == 1) { // ButtonMenu
							activate_dispfunc(knobmenu_display, knobmenu_move);
						} else if (index == 2) { // VolAdj+SoundStuff
							activate_dispfunc(voladj_prefix_display, voladj_move);
						} else {
							send_knob_pair(&knobdata_pairs[index]);
						}
					}
				}
			}
			ir_knob_down = 0;
			break;
#endif // EMPEG_KNOB_SUPPORTED
		case IR_RIO_MENU_PRESSED:
			if (!player_menu_is_active) {
				hijacked = 1; // hijack it and later send it with the release
				ir_menu_down = jiffies ? jiffies : -1;
			}
			if (hijack_status == HIJACK_ACTIVE)
				hijacked = ir_selected = 1;
			break;
		case IR_RIO_MENU_RELEASED:
			if (hijack_status != HIJACK_INACTIVE) {
				hijacked = 1;
			} else if (ir_menu_down && !player_menu_is_active) {
				hijack_button_enq(&hijack_playerq, IR_RIO_MENU_PRESSED, 0);
				if (data == ir_releasewait)
					ir_releasewait = 0;
			}
			ir_menu_down = 0;
			break;
		case IR_KW_CD_PRESSED:
			if (hijack_status == HIJACK_ACTIVE) {
				hijacked = ir_selected = 1;
			} else if (hijack_status == HIJACK_INACTIVE) {
				// ugly Kenwood remote hack: press/release CD quickly 3 times to activate menu
				if ((ir_lastpressed & ~0x80000000) != data || delay > HZ)
					ir_trigger_count = 1;
				else if (++ir_trigger_count >= 3)
					hijacked = 1;
			}
			break;

		case IR_RIO_CANCEL_PRESSED:
		case IR_KW_STAR_PRESSED:
		case IR_TOP_BUTTON_PRESSED:
			if (hijack_status != HIJACK_INACTIVE) {
				if (hijack_status == HIJACK_ACTIVE && ir_numeric_input && *ir_numeric_input)
					hijack_move(0);
				else
					hijack_deactivate(HIJACK_INACTIVE_PENDING);
				hijacked = 1;
			}
			break;
		case IR_KW_NEXTTRACK_PRESSED:
		case IR_RIO_NEXTTRACK_PRESSED:
			ir_move_repeat_delay = (hijack_movefunc == game_move) ? (HZ/15) : (HZ/3);
			ir_right_down = jiffies ? jiffies : -1;
		case IR_KNOB_RIGHT:
			if (hijack_status != HIJACK_INACTIVE) {
				hijack_move(1);
				hijacked = 1;
			}
			break;
		case IR_KW_PREVTRACK_PRESSED:
		case IR_RIO_PREVTRACK_PRESSED:
			ir_move_repeat_delay = (hijack_movefunc == game_move) ? (HZ/15) : (HZ/3);
			ir_left_down = jiffies ? jiffies : -1;
		case IR_KNOB_LEFT:
			if (hijack_status != HIJACK_INACTIVE) {
				hijack_move(-1);
				hijacked = 1;
			}
			break;
		case IR_KW_PREVTRACK_RELEASED:
		case IR_RIO_PREVTRACK_RELEASED:
			ir_left_down = 0;
			if (hijack_status != HIJACK_INACTIVE)
				hijacked = 1;
			break;
		case IR_KW_NEXTTRACK_RELEASED:
		case IR_RIO_NEXTTRACK_RELEASED:
			ir_right_down = 0;
			if (hijack_status != HIJACK_INACTIVE)
				hijacked = 1;
			break;
	}
done:
	// save button PRESSED codes in ir_lastpressed
	ir_lastpressed = data;

	// wait for RELEASED code of most recently hijacked PRESSED code
	if (data && data == ir_releasewait) {
		ir_releasewait = 0;
		hijacked = 1;
	} else if (hijacked && (data & 0x80000001) == 0 && data != IR_KNOB_LEFT && data != IR_KNOB_RIGHT) {
		ir_releasewait = data | ((data & 0xffffff00) ? 0x80000000 : 0x00000001);
	}
	if (!hijacked)
		hijack_button_enq(&hijack_playerq, data, delay);

	// See if we have any buttons ready for the player software
	return 0;
}

static void
hijack_send_buttons_to_player (void)
{
	hijack_buttondata_t data;

	while (hijack_button_deq(&hijack_playerq, &data, 0))
		(void)real_input_append_code(data.button);
}

static void
hijack_handle_buttons (void)
{
	hijack_buttondata_t data;
	unsigned long flags;

	while (hijack_button_deq(&hijack_inputq, &data, 1)) {
		save_flags_cli(flags);
		hijack_handle_button(data.button, data.delay);
		restore_flags(flags);
	}
}

// Send a release code
static void
ir_send_release (unsigned long button)
{
	unsigned long new, delay = 0;

	if (button != IR_NULL_BUTTON) {
		if (button & 0x80000000)
			delay = HZ;
		//printk("%8lu: SENDREL(%ld): %08lx\n", jiffies, delay, button);
		new = button & ~0xc0000000; // mask off the special flags bits
		new |= (new > 0xf) ? 0x80000000 : 1;	// front panel is weird
		hijack_button_enq(&hijack_inputq, new, delay);
	}
}

// Send a translated replacement sequence, except for the final release code
static void
ir_send_buttons (ir_translation_t *t)
{
	unsigned long *newp = &t->new[0];
	int count = t->count;

	while (count--) {
		unsigned long button = *newp++;
		if (button != IR_NULL_BUTTON) {
			unsigned long new = button & ~0xc0000000; // mask off the special flag bits
			if (button & 0x40000000)
				ir_shifted = !ir_shifted;
			hijack_button_enq(&hijack_inputq, new, 0);
			//printk("%8lu: SENDBUT: %08lx\n", jiffies, button);
			if (count)
				ir_send_release(button);
		}
	}
}

// This routine covertly intercepts all display updates,
// giving us a chance to substitute our own display.
//
static void	// invoked from empeg_display.c
hijack_handle_display (struct display_dev *dev, unsigned char *player_buf)
{
	unsigned char *buf = player_buf;
	unsigned long flags;
	int refresh = NEED_REFRESH;

	save_flags_cli(flags);
	if (ir_current_longpress && jiffies_since(ir_lastevent) >= HZ) {
		//printk("%8lu: LPEXP: %08lx\n", jiffies, ir_current_longpress->old);
		ir_send_buttons(ir_current_longpress);
		ir_current_longpress = NULL;
	}
	restore_flags(flags);

	// Handle any buttons that may be queued up
	hijack_handle_buttons();
	hijack_send_buttons_to_player();

	save_flags_cli(flags);
	if (!dev->power) {  // do (almost) nothing if unit is in standby mode
		hijack_deactivate(HIJACK_INACTIVE);
		(void)timer_check_expiry(dev);
		restore_flags(flags);
		display_blat(dev, player_buf);
		return;
	}

#ifdef RESTORE_CARVISUALS
	if (restore_carvisuals) {
		unsigned char *r15 = &player_buf[15 * EMPEG_SCREEN_COLS / 2];
		save_flags_cli(flags);
		// look for solid line, possibly (beta7) with a blinking track "position" pixel
		if (((*(unsigned long *)r15) & 0x22222222) == 0x22222222) {	// is track-info active yet?
			while (restore_carvisuals) {
				--restore_carvisuals;
				hijack_button_enq(&hijack_playerq, IR_RIO_INFO_PRESSED,  0);
				hijack_button_enq(&hijack_playerq, IR_RIO_INFO_RELEASED, 0);
			}
		}
		restore_flags(flags);
	}
#endif // RESTORE_CARVISUALS

#ifdef EMPEG_KNOB_SUPPORTED
	if (ir_knob_down && jiffies_since(ir_knob_down) > (HZ*2)) {
		ir_knob_busy = 1;
		ir_knob_down = jiffies - HZ;  // allow repeated cycling if knob is held down
		if (!ir_knob_down)
			ir_knob_down = -1;
		hijack_deactivate(HIJACK_INACTIVE);
		toggle_input_source();
	}
#endif // EMPEG_KNOB_SUPPORTED
	if (timer_check_expiry(dev) || maxtemp_check_threshold()) {
		buf = (unsigned char *)hijack_displaybuf;
		blanker_triggered = 0;
	}
	switch (hijack_status) {
		case HIJACK_INACTIVE:
			if (ir_trigger_count >= 3
#ifdef EMPEG_KNOB_SUPPORTED
			 || (!ir_knob_busy && ir_knob_down && jiffies_since(ir_knob_down) >= HZ)
#endif // EMPEG_KNOB_SUPPORTED
			 || (ir_menu_down && jiffies_since(ir_menu_down) >= HZ)) {
				activate_dispfunc(menu_display, menu_move);
			}
			break;
		case HIJACK_ACTIVE:
			buf = (unsigned char *)hijack_displaybuf;
			if (hijack_dispfunc == NULL) {  // userland app finished?
				activate_dispfunc(menu_display, menu_move);
			} else {
				if (hijack_movefunc != NULL)
					hijack_move_repeat();
				restore_flags(flags);
				refresh = hijack_dispfunc(0);
				save_flags_cli(flags);
				if (ir_selected && hijack_dispfunc != userland_display
#ifdef EMPEG_KNOB_SUPPORTED
				 && hijack_dispfunc != knobmenu_display && hijack_dispfunc != voladj_prefix_display
#endif // EMPEG_KNOB_SUPPORTED
				){
					if (hijack_dispfunc == forcedc_display)
						activate_dispfunc(reboot_display, NULL);
					else
						activate_dispfunc(menu_display, menu_move);
				}
			}
			if (hijack_overlay_geom) {
				hijack_do_overlay (player_buf, (unsigned char *)hijack_displaybuf, hijack_overlay_geom);
				buf = player_buf;
			}
			break;
		case HIJACK_PENDING:
			buf = (unsigned char *)hijack_displaybuf;
			if (!ir_releasewait) {
				ir_selected = 0;
				hijack_status = HIJACK_ACTIVE;
			}
			break;
		case HIJACK_INACTIVE_PENDING:
			if (!ir_releasewait || jiffies_since(ir_lasttime) > (2*HZ)) // timeout == safeguard
				hijack_deactivate(HIJACK_INACTIVE);
			break;
		default: // (good) paranoia
			hijack_deactivate(HIJACK_INACTIVE_PENDING);
			break;
	}
	// Use screen-scraping to keep track of some of the player states:
	player_menu_is_active = 0;
	if (buf == player_buf && !hijack_overlay_geom) {
		if (!(player_menu_is_active = check_if_player_menu_is_active(buf)))
			player_sound_adjust_is_active = check_if_sound_adjust_is_active(buf);
	}
	restore_flags(flags);

	// Prevent screen burn-in on an inactive/unattended player:
	if (blanker_timeout) {
		if (jiffies_since(blanker_lastpoll) >= (4*HZ/3)) {  // use an oddball interval to avoid patterns
			blanker_lastpoll = jiffies;
			if (screen_compare((unsigned long *)blanker_lastbuf, (unsigned long *)buf)) {
				memcpy(blanker_lastbuf, buf, EMPEG_SCREEN_BYTES);
				blanker_triggered = 0;
			} else if (!blanker_triggered) {
				blanker_triggered = jiffies ? jiffies : -1;
			}
		}
		if (!blanker_triggered) {
			blanker_is_blanked = 0;
		} else if (jiffies_since(blanker_triggered) > (blanker_timeout * (SCREEN_BLANKER_MULTIPLIER * HZ))) {
			if (!blanker_is_blanked) {
				buf = player_buf;
				memset(buf,0x00,EMPEG_SCREEN_BYTES);
				refresh = NEED_REFRESH;
			}
		}
	}
	if (refresh == NEED_REFRESH) {
		display_blat(dev, buf);
		hijack_last_refresh = jiffies;
	}
}

static const unsigned char hexchars[] = "0123456789abcdefABCDEF";

static int
skip_over (unsigned char **s, const unsigned char *skipchars)
{
	unsigned char *t = *s;
	if (t) {
		while (*t && strchr(skipchars, *t))
			++t;
		*s = t;
	}
	return (t && *t); // 0 == end of string
}

static int
match_char (unsigned char **s, unsigned char c)
{
	if (skip_over(s, " \t") && **s == c) {
		++*s;
		return skip_over(s, " \t");
	}
	return 0; // match failed
}

static int
get_number (unsigned char **src, int *target, unsigned int base, const char *nextchars)
{
	int digits;
	unsigned int data = 0, prev;
	unsigned char *s = *src, *cp, neg = 0;

	if (!s)
		return 0; // failure
	if (base == 10 && *s == '-') {
		++s;
		neg = 1;
	} else if (s[0] == '0' && s[1] == 'x') {
		s += 2;
		base = 16;
	}
	for (digits = 0; (cp = strchr(hexchars, *s)); ++digits) {
		unsigned char d;
		d = cp - hexchars;
		if (d > 0xf)
			d -= 6;
		if (d >= base)
			break;	// not a valid digit
		prev = data;
		data = (prev * base) + d;
		if ((data / base) != prev)
			break;	// numeric overflow
		++s;
	}
	if (!digits || (*s && !strchr(nextchars, *s)))
		return 0; // failure
	*target = neg ? -data : data;
	*src = s;
	return 1; // success
}

static int
get_option_vals (int syntax_only, unsigned char **s, const hijack_option_t *opt)
{
	int i, rc = 0;
	if (syntax_only)
		printk("hijack: %s =", opt->name);
	for (i = 0; i < opt->num_items; ++i) {
		int val;
		if (!get_number(s, &val, 10, " ,;\t\r\n") || val < opt->min || val > opt->max)
			break;	// failure
		if (syntax_only)
			printk(" %d", val);
		else
			opt->target[i] = val;
		if ((i + 1) == opt->num_items)
			rc = 1;	// success
		else if (!match_char(s, ','))
			break;	// failure
	}
	if (syntax_only)
		printk("\n");
	return rc; // success
}

static void
get_hijack_option (unsigned char *s, const hijack_option_t *opt)
{
	int optlen = strlen(opt->name);

	while (skip_over(&s, " \t\r\n") && *s != '[') {
		if (!strncmp(s, opt->name, optlen)) {
			s += optlen;
			if (match_char(&s, '=')) {
				unsigned char *test = s;
				if (get_option_vals(1, &test, opt)) {		// first pass to validate all args
					(void)get_option_vals(0, &s, opt);	// second pass to actually save the data
					return;  // done
				}
			}
		}
		while (*s && *s != '\n') // skip to end-of-line
			++s;
	}
}

static void
get_hijack_options (unsigned char *buf)
{
	const char header[] = "[hijack]";
	const hijack_option_t *opt = &hijack_option_table[0];

	// find start of options
	if (buf && *buf && (buf = strstr(buf, header)) && *buf) {
		buf += sizeof(header) - 1;
		while (opt->name) {
			get_hijack_option(buf, opt);
			++opt;
		}
	}
}

static int
ir_setup_translations2 (unsigned char *buf, unsigned long *table)
{
	const char header[] = "[ir_translate]";
	unsigned char *s;
	int index = 0;
	unsigned long *common_bits = NULL;

	if (table) {
		common_bits = &table[index++];
		*common_bits = 0x3fffffff;	// The set of bits common to all translated codes
	}
	// find start of translations
	if (!buf || !*buf || !(s = strstr(buf, header)) || !*s)
		return 0;
	s += sizeof(header) - 1;
	while (skip_over(&s, " \t\r\n")) {
		unsigned int old = 0, new, initial = 0;
		ir_translation_t *t = NULL;
		if (!strncmp(s, "initial", 7)) {
			char next = s[7];
			if (next == '=' || next == '.' || next == ' ' || next == '\t') {
				s += 7;
				initial = 1;
				old = ir_init_buttoncode++;
			}
		}
		if (initial || get_number(&s, &old, 16, " \t.=")) {
			unsigned char flags = 0, source = 0;
			if (!initial)
				old &= ~0xc0000000;
			if (*s == '.') {
				loop: if (*++s) {
					switch (*s) {
						case 'L': // Longpress
							if (!(flags & IR_FLAGS_LONGPRESS)) {
								flags |= IR_FLAGS_LONGPRESS;
								goto loop;
							}
							break;
						case 'S': // Shifted
							if (!(flags & IR_FLAGS_SHIFTED)) {
								flags |= IR_FLAGS_SHIFTED;
								goto loop;
							}
							break;
						case 'C': // Car
							if (!(flags & IR_FLAGS_CAR)) {
								flags |= IR_FLAGS_CAR;
								goto loop;
							}
							break;
						case 'H': // Home
							if (!(flags & IR_FLAGS_HOME)) {
								flags |= IR_FLAGS_HOME;
								goto loop;
							}
							break;
						case 'T': // Tuner
						case 'A': // Aux
						case 'M': // Main/Mp3/dsp
							if (!source) {
								source = *s;
								goto loop;
							}
							break;
					}
				}
			}
			if (old > 0xf && match_char(&s, '=')) {	// We currently disallow front-panel buttons
				int saved = index;
				if (table) {
					t = (ir_translation_t *)&(table[index]);
					if (initial) {
						unsigned char car  = flags & IR_FLAGS_CAR;
						unsigned char home = flags & IR_FLAGS_HOME;
						if (car  || !home)
							ir_init_car  = old;
						if (home || !car)
							ir_init_home = old;
					}
					t->old    = old;
					t->flags  = flags;
					t->source = source;
					*common_bits = old & *common_bits;	// build up common_bits mask
					t->count = 0;
				}
				index += (sizeof(ir_translation_t) - sizeof(unsigned long)) / sizeof(unsigned long);
				do {
					if (!get_number(&s, &new, 16, " .,;\t\r\n")) {
						index = saved; // error: completely ignore this line
						break;
					}
					new &= ~0xc0000000;
					if (new <= 0xf && new != IR_KNOB_LEFT)
						new &= ~1;
					if (*s == '.') {
						if (*++s == 'L') {
							++s;
							if (new != IR_KNOB_LEFT && new != IR_KNOB_RIGHT)
								new |= 0x80000000;	// mark it as a longpress
						}
						if (*s == 'S') {
							++s;
							new |= 0x40000000;	// mark as a "shift" button
						}
					}
					if (t)
						t->new[t->count++] = new;
					++index;
				} while (match_char(&s, ','));
				if (*s && !strchr(" ;\t\r\n", *s))
					index = saved; // error: completely ignore this line
			}
		}
		while (*s && *s != '\n')	// skip to end-of-line
			++s;
	}
	if (index) {
		if (table)
			table[index] = -1;	// end of table marker
		++index;
	}
	return index * sizeof(unsigned long);
}

static void
ir_setup_translations (unsigned char *buf)
{
	unsigned long flags, *table = NULL;
	int size;

	save_flags_cli(flags);
	if (ir_translate_table) {
		kfree(ir_translate_table);
		ir_translate_table = NULL;
	}
	restore_flags(flags);
	size = ir_setup_translations2(buf, NULL);	// first pass to calculate table size
	if (size <= 0)
		return;
	table = kmalloc(size, GFP_KERNEL);
	if (!table) {
		printk("ir_setup_translations failed: no memory\n");
	} else {
		(void)ir_setup_translations2(buf, table);// second pass actually saves the data
		save_flags_cli(flags);
		ir_translate_table = table;
		restore_flags(flags);
	}
}

void  // invoked from multiple places (time-sensitive code) in empeg_input.c
input_append_code(void *ignored, unsigned long button)  // empeg_input.c
{
	static unsigned long ir_downkey = 0;
	unsigned long flags, *table = ir_translate_table;

	save_flags_cli(flags);
	//printk("\n%8ld: IAC(%08lx)\n", jiffies, button);
	if (button > 0xf) {	// we cannot correctly handle front-panel here
		int released = button >> 31;
		if (released) {
			if (!ir_downkey)
				goto done;	// already taken care of (we hope)
			ir_downkey = 0;
		} else {
			if (ir_downkey == button)
				goto done;	// ignore repeated press with no intervening release
			ir_downkey = button;
		}
		ir_current_longpress = NULL;
		//printk("%8lu: %08lx\n", jiffies, button);
		if (table) {
			unsigned long old = button & ~0xc0000000, common_bits = *table++;
			if ((old & common_bits) == common_bits) {	// saves time (usually) on large tables
				int delayed_send = 0;
				unsigned char flags = on_dc_power ? IR_FLAGS_CAR : IR_FLAGS_HOME;
				if (ir_shifted)
					flags |= IR_FLAGS_SHIFTED;
				while (*table != -1) {
					ir_translation_t *t = (ir_translation_t *)table;
					table += (sizeof(ir_translation_t) / sizeof(unsigned long) - 1) + t->count;
					if (old == t->old
					 && (!t->source  || t->source == get_current_mixer_source())
					 && ((t->flags & (IR_FLAGS_SHIFTED|IR_FLAGS_HOME|IR_FLAGS_CAR)) == (t->flags & flags))) {
						if (released) {	// button release?
							if ((t->flags & IR_FLAGS_LONGPRESS) && jiffies_since(ir_lastevent) < HZ) {
								delayed_send = 1;
								continue; // look for shortpress instead
							}
							if (delayed_send)
								ir_send_buttons(t);
							ir_send_release(t->new[t->count - 1]); // final button release
						} else { // button press?
							if ((t->flags & IR_FLAGS_LONGPRESS))
								ir_current_longpress = t;
							else
								ir_send_buttons(t);
						}
						ir_lasttime = ir_lastevent = jiffies;
						goto done;
					}
				}
				if (delayed_send)
					hijack_button_enq(&hijack_inputq, old, 0);
			}
		}
	}
	ir_lasttime = ir_lastevent = jiffies;
	hijack_button_enq(&hijack_inputq, button, 0);
done:
	restore_flags(flags);
}

// returns menu index >= 0,  or -ERROR
static int
extend_menu (menu_item_t *new)
{
	int i;
	for (i = 0; i < MENU_MAX_ITEMS; ++i) {
		menu_item_t *item = &menu_table[i];
		if (item->label == NULL || !strcmp(item->label, new->label)) {
			if (item->label == NULL) // extending table?
				menu_size = i + 1;
			*item = *new;	// copy data regardless
			return i;	// success
		}
	}
	return -ENOMEM; // no room; menu is full
}

// returns menu index >= 0,  or -ERROR
static int
userland_extend_menu (char *label, unsigned long userdata)
{
	int rc = -ENOMEM, size = strlen(label);
	unsigned long flags;
	menu_item_t item;

	item.label = kmalloc(size+3, GFP_KERNEL);
	if (item.label == NULL)
		return -ENOMEM;
	memcpy(item.label+1, label, size);
	item.label[0]      = ' ';
	item.label[size+1] = ' ';
	item.label[size+2] = '\0';
	item.dispfunc = userland_display;
	item.movefunc = NULL;
	item.userdata = userdata;
	save_flags_cli(flags);
	rc = extend_menu(&item);
	restore_flags(flags);
	return rc;
}

static void
menu_init (void)
{
	// menu_size, menu_item, and menu_top
	for (menu_size = 0; menu_table[menu_size].label != NULL; ++menu_size); // Calculate initial menu size
	while (menu_table[menu_item].label == NULL)
		--menu_item;
	menu_top = (menu_item ? menu_item : menu_size) - 1;
}

#define HIJACK_SAVEAREA_OFFSET (128 - 2 - sizeof(hijack_savearea))

void	// invoked from empeg_state.c
hijack_save_settings (unsigned char *buf)
{
	// save state
	if (on_dc_power)
		hijack_savearea.voladj_dc_power	= hijack_voladj_enabled;
	else
		hijack_savearea.voladj_ac_power	= hijack_voladj_enabled;
	hijack_savearea.blanker_timeout		= blanker_timeout;
	hijack_savearea.maxtemp_threshold	= maxtemp_threshold;
#ifdef EMPEG_KNOB_SUPPORTED
{
	unsigned int knob;
	if (knobdata_index == 1)
		knob = (1 << KNOBDATA_BITS) | knobmenu_index;
	else
		knob = knobdata_index;
	if (on_dc_power)
		hijack_savearea.knob_dc = knob;
	else
		hijack_savearea.knob_ac = knob;
}
#endif // EMPEG_KNOB_SUPPORTED
	hijack_savearea.blankerfuzz_amount	= blankerfuzz_amount;
	hijack_savearea.timer_action		= timer_action;
	hijack_savearea.menu_item		= menu_item;
	hijack_savearea.restore_visuals		= carvisuals_enabled;
	hijack_savearea.fsck_disabled		= hijack_fsck_disabled;
	//hijack_savearea.force_dcpower is only updated from the menu!
	hijack_savearea.byte3_leftover		= 0;
	hijack_savearea.byte5_leftover		= 0;
	hijack_savearea.byte6_leftover		= 0;
	memcpy(buf+HIJACK_SAVEAREA_OFFSET, &hijack_savearea, sizeof(hijack_savearea));
}

void	// invoked from empeg_state.c
hijack_restore_settings (unsigned char *buf)
{
	on_dc_power = empeg_on_dc_power();
	// restore state
	memcpy(&hijack_savearea, buf+HIJACK_SAVEAREA_OFFSET, sizeof(hijack_savearea));
	hijack_force_dcpower		= hijack_savearea.force_dcpower;
	if (on_dc_power) {
		hijack_voladj_enabled	= hijack_savearea.voladj_dc_power;
	} else {
		hijack_voladj_enabled	= hijack_savearea.voladj_ac_power;
	}
	blanker_timeout			= hijack_savearea.blanker_timeout;
	maxtemp_threshold		= hijack_savearea.maxtemp_threshold;
#ifdef EMPEG_KNOB_SUPPORTED
{
	unsigned int knob;
	knob = on_dc_power ? hijack_savearea.knob_dc : hijack_savearea.knob_ac;
	if ((knob & (1 << KNOBDATA_BITS)) == 0) {
		knobmenu_index		= 0;
		knobdata_index		= knob;
	} else {
		knobmenu_index		= knob & ((1 << KNOBDATA_BITS) - 1);
		knobdata_index		= 1;
	}
}
#endif // EMPEG_KNOB_SUPPORTED
	blankerfuzz_amount		= hijack_savearea.blankerfuzz_amount;
	timer_action			= hijack_savearea.timer_action;
	menu_item			= hijack_savearea.menu_item;
	menu_init();
	carvisuals_enabled		= hijack_savearea.restore_visuals;
	hijack_fsck_disabled		= hijack_savearea.fsck_disabled;

#ifdef RESTORE_CARVISUALS
	restore_carvisuals = 0;
	if (on_dc_power && carvisuals_enabled) {
#if 0 // easy to read, but big and slow..
		unsigned int b40cd = (buf[0x40] << 16) | (buf[0x4c] << 8) | buf[0x4d];
		switch (b40cd) {
			case 0x000004: // off (beta3, beta6)
				restore_carvisuals = 2;
				break;
			case 0x000005: // off (beta7)
				restore_carvisuals = 3;
				break;
			case 0x010004: // line (beta3, beta6)
				restore_carvisuals = 3;
				break;
			case 0x010005: // line (beta7)
				restore_carvisuals = 4;
				break;
			case 0x020004: // transient (beta3, beta6)
				restore_carvisuals = 4;
				break;
			case 0x020005: // transient (beta7)
				restore_carvisuals = 5;
				break;
		  //	case 0x020403: // track (beta3, beta6, beta7)
		  //	case 0x020404: // now&next (beta3, beta6)
		  //	case 0x020405: // now&next (beta7)
		  //	case 0x020404: // infoseek (beta7)
		}
		if (restore_carvisuals) {
#else // not so obvious, but tiny and fast
		if (buf[0x4c] == 0) {
			restore_carvisuals = buf[0x40] + buf[0x4d] - 2;
#endif // 0
			// switch to "track" mode on startup (because it's easy to detect later one),
			// and then restore the original mode when the track info appears in the screen buffer.
			buf[0x40] = 0x02;
			buf[0x4c] = 0x04;
			buf[0x4d] = 0x03;
			empeg_state_dirty = 1;
		}
	}
#endif // RESTORE_CARVISUALS
}

static int
hijack_wait_on_menu (char *argv[])
{
	struct wait_queue wait = {current, NULL};
	int i, rc, index, num_items, indexes[MENU_MAX_ITEMS];
	unsigned long flags, userdata = (unsigned long)current->pid << 8;

	// (re)create the menu items, with our pid/index as userdata
	for (i = 0; *argv && i < MENU_MAX_ITEMS; ++i) {
		unsigned char label[32], size = 0, *argp = *argv++;
		do {
			if (copy_from_user(&label[size], &argp[size], 1))
				return -EFAULT;
		} while (label[size++] && size < sizeof(label));
		label[size-1] = '\0';

		save_flags_cli(flags);
		index = userland_extend_menu(label, userdata | i);
		restore_flags(flags);
		if (index < 0)
			return index;
		indexes[i] = index;
	}
	num_items = i;
	save_flags_cli(flags);
	if (hijack_dispfunc == userland_display && (menu_table[menu_item].userdata & ~0xff) == userdata) {
		hijack_dispfunc = NULL;		// restart the main menu
		if (hijack_buttonlist) {	// release any buttons we had grabbed
			kfree(hijack_buttonlist);
			hijack_buttonlist = NULL;
		}
	}
	add_wait_queue(&hijack_menu_waitq, &wait);
	while (1) {
		unsigned long menudata;
		current->state = TASK_INTERRUPTIBLE;
		menudata = menu_table[menu_item].userdata;
		if (signal_pending(current)) {
			rc = -EINTR;
			break;
		}
		if (hijack_dispfunc == userland_display && (menudata & ~0xff) == userdata) {
			rc = menudata & 0x7f;
			break;
		}
		restore_flags(flags);
		schedule();
		save_flags_cli(flags);
	}
	if (rc < 0) {
		if (hijack_dispfunc == userland_display && (menu_table[menu_item].userdata & ~0xff) == userdata)
			hijack_dispfunc = NULL;		// restart the main menu
		for (i = 0; i < num_items; ++i)	{	// disable our menu items until next time
			menu_item_t *item = &menu_table[indexes[i]];
			item->dispfunc = NULL;		// disable the menu item
		}
	}
	current->state = TASK_RUNNING;
	remove_wait_queue(&hijack_menu_waitq, &wait);
	restore_flags(flags);
	return rc;
}

static int
copy_buttonlist_from_user (unsigned long arg, unsigned long **buttonlist, unsigned long max_size)
{
	// data[0] specifies TOTAL number of table entries data[0..?]
	// data[0] cannot be zero; data[0]==1 means "capture everything"
	unsigned long *list = NULL, size;
	long nbuttons;
	if (copy_from_user(&nbuttons, (void *)arg, sizeof(nbuttons)))
		return -EFAULT;
	if (nbuttons <= 0 || nbuttons > max_size)
		return -EINVAL;
	size = nbuttons * sizeof(nbuttons);
	if (!(list = kmalloc(size, GFP_KERNEL)))
		return -ENOMEM;
	if (copy_from_user(list, (void *)arg, size)) {
		kfree(list);
		return -EFAULT;
	}
	*buttonlist = list;
	return 0;
}

#include <linux/smp_lock.h>	// for lock_kernel() and unlock_kernel()

static int
get_file (const char *path, unsigned char **buffer)
{
	unsigned int size;
	int rc = 0;
	struct file *filp;

	*buffer = NULL;
	lock_kernel();	// FIXME?  Is this necessary?
	filp = filp_open(path,O_RDONLY,0);
	if (IS_ERR(filp) || !filp) {
		rc = -ENOENT;
	} else {
		if (filp->f_dentry && filp->f_dentry->d_inode) {
			size = filp->f_dentry->d_inode->i_size;
			if (size > 0) {
				unsigned char *buf = (unsigned char *)kmalloc(size+1, GFP_KERNEL);
				if (!buf) {
					rc = -ENOMEM;
				} else {
					mm_segment_t old_fs = get_fs();
					filp->f_pos = 0;
					set_fs(KERNEL_DS);
					rc = filp->f_op->read(filp, buf, size, &(filp->f_pos));
					set_fs(old_fs);
					if (rc < 0) {
						kfree(buf);
					} else {
						buf[rc] = '\0';
						*buffer = buf;
					}
				}
			}
		}
		filp_close(filp,NULL);
	}
	unlock_kernel();
  	return rc;
}

static void
print_ir_translates (void)
{
	unsigned long *table = ir_translate_table;

	if (table) {
		++table;	// skip over "common_bits" word
		while (*table != -1) {
			ir_translation_t *t = (ir_translation_t *)table;
			unsigned long *newp = &t->new[0];
			int count = t->count;
			if (t->old == ir_init_car || t->old == ir_init_home)
				printk("ir_translate: initial");
			else
				printk("ir_translate: %08lx", t->old);
			if (t->flags || t->source) {
				printk(".");
				if ((t->flags & IR_FLAGS_HOME))
					printk("H");
				if ((t->flags & IR_FLAGS_CAR))
					printk("C");
				if ((t->flags & IR_FLAGS_LONGPRESS))
					printk("L");
				if ((t->flags & IR_FLAGS_SHIFTED))
					printk("S");
				if (t->source)
					printk("%c", t->source);
			}
			printk("=");
			while (count--) {
				unsigned long new = *newp++;
				printk("%08lx", new & ~0xc0000000);
				if (new & 0xc0000000) {
					printk(".");
					if (new & 0x80000000)
						printk("L");
					if (new & 0x40000000)
						printk("S");
				}
				printk(count ? "," : "\n");
			}
			table = &t->new[t->count];
		}
	}
}

void
hijack_read_config_file (const char *path)
{
	unsigned char *buf = NULL;
	unsigned long flags;
	int rc;

	printk("\n");
	rc = get_file(path, &buf);
	if (rc < 0) {
		printk("hijack.c: open(%s) failed (errno=%d)\n", path, rc);
	} else if (rc > 0 && buf && *buf) {
		get_hijack_options(buf);
		if (hijack_classic) {
			PROMPTCOLOR		=  COLOR2;
			ENTRYCOLOR		=  COLOR3;
		} else {
			PROMPTCOLOR		=  COLOR3;
			ENTRYCOLOR		= -COLOR3;
		}
		ir_setup_translations(buf);
		print_ir_translates();

		// Send initial button sequences, if any
		save_flags_cli(flags);
		if ( on_dc_power && ir_init_car)
			input_append_code(NULL, ir_init_car);
		if (!on_dc_power && ir_init_home)
			input_append_code(NULL, ir_init_home);
		restore_flags(flags);
	}
	if (buf)
		kfree(buf);
	hijack_set_voladj_parms();
}

int hijack_ioctl  (struct inode *inode, struct file *filp, unsigned int cmd, unsigned long arg)
{
	unsigned long flags;
	int rc;

	switch (cmd) {
		case EMPEG_HIJACK_WAITMENU:	// (re)create menu item(s) and wait for them to be selected
		{
			// Invocation:  char *menulabels[] = {"Label1", "Label2", NULL};
			//              rc = ioctl(fd, EMPEG_HIJACK_WAITMENU, &menu_labels)
			//              if (rc < 0) perror(); else selected_index = rc;
			// The screen is then YOURS until you issue another EMPEG_HIJACK_WAITMENU
			return hijack_wait_on_menu((char **)arg);
		}
		case EMPEG_HIJACK_DISPWRITE:	// copy buffer to screen
		{
			// Invocation:  rc = ioctl(fd, EMPEG_HIJACK_DISPWRITE, (unsigned char *)displaybuf); // sizeof(displaybuf)==2048
			if (copy_from_user(hijack_displaybuf, (void *)arg, sizeof(hijack_displaybuf)))
				return -EFAULT;
			userland_display_updated = 1;
			return 0;
		}
		case EMPEG_HIJACK_BINDBUTTONS:	// Specify IR codes to be hijacked
		{
			// Invocation:  rc = ioctl(fd, EMPEG_HIJACK_DISPWRITE, (unsigned long *)data[]);
			// data[0] specifies TOTAL number of table entries data[0..?]
			// data[0] cannot be zero; data[0]==1 means "capture everything"
			unsigned long *buttonlist = NULL;
			if ((rc = copy_buttonlist_from_user(arg, &buttonlist, 256)))
				return rc;
			save_flags_cli(flags);
			if (hijack_buttonlist) {
				restore_flags(flags);
				kfree(buttonlist);
				return -EBUSY;
			}
			hijack_buttonlist = buttonlist;
			hijack_initq(&hijack_userq);
			restore_flags(flags);
			return 0;
		}
		case EMPEG_HIJACK_UNBINDBUTTONS:	// Specify IR codes to be hijacked
		{
			// Invocation:  rc = ioctl(fd, EMPEG_HIJACK_UNBINDBUTTONS, NULL);
			save_flags_cli(flags);
			if (!hijack_buttonlist) {
				rc = -ENXIO;
			} else {
				kfree(hijack_buttonlist);
				hijack_buttonlist = NULL;
				rc = 0;
			}
			restore_flags(flags);
			return rc;
		}
		case EMPEG_HIJACK_WAITBUTTONS:	// Wait for next hijacked IR code
		{
			// Invocation:  rc = ioctl(fd, EMPEG_HIJACK_WAITBUTTONS, (unsigned long *)&data);
			// IR code is written to "data" on return
			hijack_buttondata_t data;
			struct wait_queue wait = {current, NULL};
			save_flags_cli(flags);
			add_wait_queue(&hijack_userq_waitq, &wait);
			rc = 0;
			while (1) {
				current->state = TASK_INTERRUPTIBLE;
				if (hijack_button_deq(&hijack_userq, &data, 0))
					break;
				if (signal_pending(current)) {
					rc = -EINTR;
					break;
				}
				restore_flags(flags);
				schedule();
				save_flags_cli(flags);
			}
			current->state = TASK_RUNNING;
			remove_wait_queue(&hijack_userq_waitq, &wait);
			if (!rc) {
				restore_flags(flags);
				rc = put_user(data.button, (int *)arg);
			}
			return rc;
		}
		case EMPEG_HIJACK_POLLBUTTONS:	// See if any input is available
		{
			// Invocation:  rc = ioctl(fd, EMPEG_HIJACK_POLLBUTTONS, (unsigned long *)&data);
			// IR code is written to "data" on return
			hijack_buttondata_t data;
			save_flags_cli(flags);
			if (!hijack_button_deq(&hijack_userq, &data, 0)) {
				restore_flags(flags);
				return -EAGAIN;
			}
			restore_flags(flags);
			return put_user(data.button, (int *)arg);

		}
		case EMPEG_HIJACK_INJECTBUTTONS:	// Insert button codes into player's input queue (bypasses hijack)
		{					// same args/usage as EMPEG_HIJACK_BINDBUTTONS
			unsigned long *buttonlist = NULL;
			int i;
			if ((rc = copy_buttonlist_from_user(arg, &buttonlist, 256)))
				return rc;
			save_flags_cli(flags);
			for (i = 1; i < buttonlist[0]; ++i)
				hijack_button_enq(&hijack_playerq, buttonlist[i], 0);
			restore_flags(flags);
			kfree(buttonlist);
			return signal_pending(current) ? -EINTR : 0;

		}
		case EMPEG_HIJACK_SETGEOM:	// Set window overlay geometry
		{
			// Invocation:  rc = ioctl(fd, EMPEG_HIJACK_SETGEOM, (hijack_geom_t *)&geom);
			static hijack_geom_t geom; // static is okay here cuz only one app can be active at a time
			if (copy_from_user(&geom, (hijack_geom_t *)arg, sizeof(geom)))
				return -EFAULT;
			if (geom.first_row >= EMPEG_SCREEN_ROWS || geom.last_row >= EMPEG_SCREEN_ROWS
			 || geom.first_row >= geom.last_row     || geom.first_col >= geom.last_col
			 || geom.first_col >= EMPEG_SCREEN_COLS || geom.last_col >= EMPEG_SCREEN_COLS
			 || (geom.first_col & 1) || (geom.last_col & 1))
				return -EINVAL;
			save_flags_cli(flags);
			if (geom.first_row != 0 || geom.last_row != (EMPEG_SCREEN_ROWS-1) || geom.first_col != 0 || geom.last_col != (EMPEG_SCREEN_COLS-1))
				hijack_overlay_geom = &geom;	// partial overlay
			else
				hijack_overlay_geom = NULL;	// full screen
			restore_flags(flags);
			return 0;
		}
		case EMPEG_HIJACK_DISPCLEAR:	// Clear screen
		{
			// Invocation:  rc = ioctl(fd, EMPEG_HIJACK_DISPCLEAR, NULL);
			clear_hijack_displaybuf(COLOR0);
			userland_display_updated = 1;
			return 0;
		}
		case EMPEG_HIJACK_DISPTEXT:	// Write text to screen
		{
			// Invocation:  rc = ioctl(fd, EMPEG_HIJACK_DISPTEXT, (unsigned char *)"Text for the screen");
			int size = 0;
			unsigned char color = COLOR3, buf[256] = {0,}, *text = (unsigned char *)arg;
			do {
				if (copy_from_user(&buf[size], text+size, 1))
					return -EFAULT;
			} while (buf[size++] && size < sizeof(buf));
			buf[size-1] = '\0';
			text = buf;
			if (buf[0] & 0x80) {		// First byte can now specify a color as: (color | 0x80)
				color = *text++;	// Note also that negative colors are used for "reverse video"
				if (!(color & 0x40))
					color &= 3;
			}
			save_flags_cli(flags);
			(void)draw_string(ROWCOL(0,0),text,color);
			restore_flags(flags);
			userland_display_updated = 1;
			return 0;
		}
		default:			// Everything else
		{
			return display_ioctl(inode, filp, cmd, arg);
		}
	}
}

// initial setup of hijack menu system
void
hijack_init (void)
{
	static int initialized = 0;

	if (!initialized) {
		initialized = 1;
		(void)init_temperature();
		hijack_initq(&hijack_inputq);
		hijack_initq(&hijack_playerq);
		hijack_initq(&hijack_userq);
		menu_init();
	}
}

