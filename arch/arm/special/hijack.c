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
extern int get_current_mixer_source(void);				// arch/arm/special/empeg_mixer.c
extern int empeg_readtherm(volatile unsigned int *timerbase, volatile unsigned int *gpiobase);	// arch/arm/special/empeg_therm.S
extern int empeg_inittherm(volatile unsigned int *timerbase, volatile unsigned int *gpiobase);	// arch/arm/special/empeg_therm.S

#ifdef CONFIG_NET_ETHERNET	// Mk2 or later? (Mk1 has no ethernet)
#define EMPEG_KNOB_SUPPORTED	// Mk2 and later have a front-panel knob
#endif

#define NEED_REFRESH		0
#define NO_REFRESH		1

#define HIJACK_INACTIVE		0
#define HIJACK_INACTIVE_PENDING	1
#define HIJACK_PENDING		2
#define HIJACK_ACTIVE		3

static unsigned int hijack_status = HIJACK_INACTIVE;
static unsigned long hijack_last_moved = 0, hijack_last_refresh = 0, blanker_triggered = 0, blanker_lastpoll = 0;
static unsigned char blanker_lastbuf[EMPEG_SCREEN_BYTES] = {0,}, blanker_is_blanked = 0;

static int  (*hijack_dispfunc)(int) = NULL;
static void (*hijack_movefunc)(int) = NULL;
static unsigned long ir_lasttime = 0, ir_selected = 0, ir_releasewait = 0, ir_trigger_count = 0;;
static unsigned long ir_menu_down = 0, ir_left_down = 0, ir_right_down = 0;
static unsigned long ir_move_repeat_delay;
static int *ir_numeric_input = NULL, player_menu_is_active = 0, player_sound_adjust_is_active = 0;

#define KNOBDATA_BITS 2
#ifdef EMPEG_KNOB_SUPPORTED
static unsigned long ir_knob_down, ir_delayed_knob_release = 0;
static const unsigned long knobdata_pressed [] = {IR_KNOB_PRESSED, IR_RIO_SHUFFLE_PRESSED, IR_RIO_INFO_PRESSED, IR_RIO_INFO_PRESSED};
static const unsigned long knobdata_released[] = {IR_KNOB_RELEASED,IR_RIO_SHUFFLE_RELEASED,IR_RIO_INFO_RELEASED,IR_RIO_INFO_PRESSED};
static int knobdata_index = 0;
#endif

#define HIJACK_DATAQ_SIZE	8
static const unsigned long intercept_all_buttons[] = {1};
static const unsigned long *hijack_buttonlist = NULL;
static unsigned long hijack_dataq[HIJACK_DATAQ_SIZE];
static unsigned int hijack_dataq_head = 0, hijack_dataq_tail = 0;
static struct wait_queue *hijack_dataq_waitq = NULL, *hijack_menu_waitq = NULL;

#define MULT_POINT		12
#define MULT_MASK		((1 << MULT_POINT) - 1)
#define VOLADJ_THRESHSIZE	16
#define VOLADJ_HISTSIZE		128	/* must be a power of two */
#define VOLADJ_FIXEDPOINT(whole,fraction) ((((whole)<<MULT_POINT)|((unsigned int)((fraction)*(1<<MULT_POINT))&MULT_MASK)))
#define VOLADJ_BITS 2

int hijack_voladj_enabled = 0; // used by voladj code in empeg_audio3.c

static const char *voladj_names[] = {"[Off]", "Low", "Medium", "High"};
static unsigned int voladj_history[VOLADJ_HISTSIZE] = {0,}, voladj_last_histx = 0, voladj_histx = 0;

#define SCREEN_BLANKER_MULTIPLIER 15
#define BLANKER_BITS (8 - VOLADJ_BITS)
static int blanker_timeout = 0;

#define BLANKERFUZZ_MULTIPLIER 5
#define BLANKERFUZZ_BITS 3
static int blankerfuzz_amount = 0;

#define MAXTEMP_OFFSET	29
#define MAXTEMP_BITS	(8 - VOLADJ_BITS)
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
	unsigned voladj_ac_power	: VOLADJ_BITS;
	unsigned blanker_timeout	: BLANKER_BITS;
	unsigned voladj_dc_power	: VOLADJ_BITS;
	unsigned maxtemp_threshold	: MAXTEMP_BITS;
	unsigned knobdata_index		: KNOBDATA_BITS;
	unsigned blankerfuzz_amount	: BLANKERFUZZ_BITS;
	unsigned timer_action		: TIMERACTION_BITS;
	unsigned force_dcpower		: DCPOWER_BITS;
	unsigned byte3_leftover		: (8 - (KNOBDATA_BITS + BLANKERFUZZ_BITS + TIMERACTION_BITS + DCPOWER_BITS));
	unsigned byte4			: 8;
	unsigned byte5			: 8;
	unsigned byte6			: 8;
	unsigned menu_item		: MENU_BITS;
	unsigned byte7_leftover		: (8 - MENU_BITS);
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

static hijack_geom_t *hijack_overlay_geom = NULL;

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

static void
clear_text_row(unsigned int rowcol, unsigned short last_col)
{
	unsigned short pixel_row, num_cols, row = (rowcol & 0xffff), pixel_col = (rowcol >> 16);

	num_cols = 1 + last_col - pixel_col;
	if (row & 0x8000)
		row = (row & ~0x8000) * KFONT_HEIGHT;
	for (pixel_row = 0; pixel_row < KFONT_HEIGHT; ++pixel_row) {
		unsigned int offset = 0;
		unsigned char *displayrow, pixel_mask = (pixel_col & 1) ? 0xf0 : 0x0f;
		if ((row + pixel_row) >= EMPEG_SCREEN_ROWS)
			return;
		displayrow = &hijack_displaybuf[row + pixel_row][0];
		do {
			unsigned char *pixel_pair = &displayrow[(pixel_col + offset) >> 1];
			*pixel_pair &= (pixel_mask = ~pixel_mask);
		} while (++offset < num_cols);
	}
}
	

static unsigned char kfont_spacing = 0;  // 0 == proportional

static int
draw_char (unsigned short pixel_row, short pixel_col, unsigned char c, unsigned char color, unsigned char inverse)
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
	if (pixel_col + num_cols + 1 >= EMPEG_SCREEN_COLS)
		return -1;
	for (pixel_row = 0; pixel_row < KFONT_HEIGHT; ++pixel_row) {
		unsigned char pixel_mask = (pixel_col & 1) ? 0xf0 : 0x0f;
		unsigned int offset = 0;
		do {
			unsigned char font_bit    = font_entry[offset] & (1 << pixel_row);
			unsigned char new_pixel   = font_bit ? (color & pixel_mask) : 0;
			unsigned char *pixel_pair = &displayrow[(pixel_col + offset) >> 1];
			*pixel_pair = ((inverse & pixel_mask) | (*pixel_pair & (pixel_mask = ~pixel_mask))) ^ new_pixel;
		} while (++offset < num_cols);
		displayrow += (EMPEG_SCREEN_COLS / 2);
	}
	return num_cols;
}

// 0x8000 in rowcol means "text_row"; otherwise "pixel_row"
#define ROWCOL(text_row,pixel_col)  ((unsigned int)(((pixel_col)<<16)|((text_row)|0x8000)))

static unsigned int
draw_string (unsigned int rowcol, const unsigned char *s, int color)
{
	unsigned short row = (rowcol & 0xffff), col = (rowcol >> 16);
	unsigned char inverse = 0;

	if (row & 0x8000)
		row = (row & ~0x8000) * KFONT_HEIGHT;
	if (color < 0)
		color = inverse = -color;
	color &= 3;
	color |= color << 4;
	if (inverse)
		inverse = color;
top:	if (s && row < EMPEG_SCREEN_ROWS) {
		while (*s) {
			int col_adj;
			if (*s++ == '\n' || -1 == (col_adj = draw_char(row, col, *(s-1), color, inverse))) {
				col  = 0;
				row += KFONT_HEIGHT;
				goto top;
			}
			col += col_adj;
		}
	}
	return (col << 16) | row;
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
hijack_deactivate (int new_status)
{
	ir_selected = 0;
	ir_numeric_input = NULL;
	hijack_movefunc = NULL;
	hijack_dispfunc = NULL;
	hijack_buttonlist = NULL;
	ir_trigger_count = 0;
	hijack_overlay_geom = NULL;
	hijack_status = new_status;
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

const unsigned int hijack_voladj_parms[(1<<VOLADJ_BITS)-1][5] = { // Values as suggested by Richard Lovejoy
	{0x1800,	 100,	0x1000,	25,	60},  // Low
	{0x2000,	 409,	0x1000,	25,	60},  // Medium (Normal)
	{0x2000,	3000,	0x0c00,	30,	80}}; // High

static void
voladj_move (int direction)
{
	unsigned int old = hijack_voladj_enabled;

	hijack_voladj_enabled += direction;
	if (hijack_voladj_enabled < 0)
		hijack_voladj_enabled = 0;
	else if (hijack_voladj_enabled >= ((1<<VOLADJ_BITS)-1))
		hijack_voladj_enabled   = ((1<<VOLADJ_BITS)-1);
	if (hijack_voladj_enabled != old) {
		empeg_state_dirty = 1;
		if (hijack_voladj_enabled) {
			unsigned const int *p = hijack_voladj_parms[hijack_voladj_enabled - 1];
			hijack_voladj_intinit(p[0],p[1],p[2],p[3],p[4]);	
		}
	}
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
	rowcol = draw_string(ROWCOL(0,0), "Auto Volume Adjust: ", COLOR2);
	(void)draw_string(rowcol, voladj_names[hijack_voladj_enabled], COLOR3);
	save_flags_cli(flags);
	histx = voladj_last_histx = voladj_histx;
	mult  = voladj_history[histx];
	restore_flags(flags);
	sprintf(buf, "Current Multiplier: %2u.%02u", mult >> MULT_POINT, (mult & MULT_MASK) * 100 / (1 << MULT_POINT));
	(void)draw_string(ROWCOL(3,12), buf, COLOR2);
	for (col = 0; col < VOLADJ_HISTSIZE; ++col)
		(void)voladj_plot(1, col, voladj_history[++histx & (VOLADJ_HISTSIZE-1)], &prev);
	return NEED_REFRESH;
}

static int
voladj_prefix (int firsttime)
{
	static const hijack_geom_t geom = {8, 8+6+KFONT_HEIGHT, 16, EMPEG_SCREEN_COLS-16};

	ir_selected = 0; // paranoia?
	if (firsttime) {
		hijack_last_moved = jiffies ? jiffies : 1;
		clear_hijack_displaybuf(COLOR0);
		draw_frame((unsigned char *)hijack_displaybuf, &geom);
		hijack_overlay_geom = (hijack_geom_t *)&geom;
	} else if (jiffies_since(hijack_last_moved) >= (HZ*2)) {
		hijack_deactivate(HIJACK_INACTIVE);
	} else {
		unsigned int rowcol = (geom.first_row+4)|((geom.first_col+6)<<16);
		rowcol = draw_string(rowcol, "Auto VolAdj: ", COLOR3);
		clear_text_row(rowcol, geom.last_col-4);
		rowcol = draw_string(rowcol, voladj_names[hijack_voladj_enabled], COLOR3);
		return NEED_REFRESH;
	}
	return NO_REFRESH;
}

static int
kfont_display (int firsttime)
{
	unsigned int rowcol;
	unsigned char c;

	if (!firsttime)
		return NO_REFRESH;
	clear_hijack_displaybuf(COLOR0);
	rowcol = draw_string(ROWCOL(0,0), " ", -COLOR3);
	rowcol = draw_string(rowcol, " ", -COLOR2);
	rowcol = draw_string(rowcol, " ", -COLOR1);
	for (c = (unsigned char)' '; c <= (unsigned char)'~'; ++c) {
		unsigned char s[2] = {0,0};
		s[0] = c;
		rowcol = draw_string(rowcol, &s[0], COLOR2);
	}
	return NEED_REFRESH;
}

static void
init_temperature (void)
{
	unsigned long flags;

	save_flags_clif(flags);
	(void)empeg_inittherm(&OSMR0,&GPLR);
	restore_flags(flags);
}

static unsigned long temp_lasttime = 0;
static int
read_temperature (void)
{

	static int temp = 0;
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

	if (temp_lasttime && jiffies_since(temp_lasttime) < (HZ*2))
		return temp;
	save_flags_clif(flags);			//  power cyles without inittherm()
	temp = empeg_readtherm(&OSMR0,&GPLR);
	restore_flags(flags);
	temp_lasttime = jiffies ? jiffies : 1;
	if (((temp_lasttime / HZ) & 0x63) == 0) // restart the thermometer once a minute or so
		init_temperature();
	/* Correct for negative temperatures (sign extend) */
	if (temp & 0x80)
		temp = -(128 - (temp ^ 0x80));
	return temp;
}

static unsigned int
draw_temperature (unsigned int rowcol, int temp, int color)
{
	unsigned char buf[32];
	sprintf(buf, "%dC/%dF ", temp, temp * 180 / 100 + 32);
	return draw_string(rowcol, buf, color);
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
	sprintf(buf, "Rev:%02d, Jiffies:%08lX\nTemperature: ", permset[0], jiffies);
	rowcol = draw_string(ROWCOL(0,0), buf, COLOR2);
	(void)draw_temperature(rowcol, read_temperature(), COLOR2);
	si_meminfo(&si);
	sprintf(buf, "Free: %lukB/%lukB\nLoadAvg: ", si.freeram/1024, si.totalram/1024);
	rowcol = draw_string(ROWCOL(2,0), buf, COLOR2);
	(void)get_loadavg(buf);
	count = 0;
	for (i = 0;; ++i) {
		if (buf[i] == ' ' && ++count == 3)
			break;
	}
	buf[i] = '\0';
	(void)draw_string(rowcol, buf, COLOR2);
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
	(void)draw_string(ROWCOL(0,0), "Force DC/Car Operation", COLOR2);
	rowcol = draw_string(ROWCOL(1,0), "Current Mode: ", COLOR2);
	(void)draw_string(rowcol, empeg_on_dc_power() ? "DC/Car" : "AC/Home", COLOR2);
	rowcol = draw_string(ROWCOL(3,0), "Next reboot: ", COLOR2);
	(void)draw_string(rowcol, hijack_savearea.force_dcpower ? "Force DC/Car" : "[Normal]", COLOR3);
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
			sprintf(buf, "%02u:%02u:%02u", hours, minutes, seconds);
			goto draw;
		}
	}
	sprintf(buf, "%02u:%02u", minutes, seconds);
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
	unsigned char *offmsg = "[Off]";

	if (firsttime) {
		paused = 0;
		if (timer_timeout) {  // was timer already running?
			int remaining = timer_timeout - (jiffies_since(timer_started) / HZ);
			if (remaining > 0) {
				timer_timeout = remaining;
				paused = 1;
			} else {
				timer_timeout = 0;  // turn alarm off if it was on
				offmsg = "[Cancelled]";
			}
		}
		ir_numeric_input = &timer_timeout;
	}
	timer_started = jiffies;
	if (!firsttime && !hijack_last_moved)
		return NO_REFRESH;
	hijack_last_moved = 0;
	clear_hijack_displaybuf(COLOR0);
	(void)draw_string(ROWCOL(0,0), "Countdown Timer Timeout", COLOR2);
	rowcol = draw_string(ROWCOL(2,0), "Duration: ", COLOR2);
	if (timer_timeout) {
		rowcol = draw_hhmmss(rowcol, timer_timeout, COLOR3);
		if (paused)
			(void)draw_string(rowcol, " [paused]", COLOR2);
	} else {
		paused = 0;
		(void)draw_string(rowcol, offmsg, COLOR3);
	}
	return NEED_REFRESH;
}

static void
timeraction_move (int direction)
{
	timer_action = !timer_action;
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
	(void)draw_string(ROWCOL(0,0), "Countdown Timer Action", COLOR2);
	rowcol = draw_string(ROWCOL(2,0), "On expiry: ", COLOR2);
	(void)   draw_string(rowcol, timer_action ? "Beep Alarm" : "Toggle Standby", COLOR3);
	return NEED_REFRESH;
}

static int maxtemp_check_threshold (void)
{
	static unsigned long beeping;
	unsigned int elapsed = jiffies_since(ir_lasttime) / HZ;
 
	if (!maxtemp_threshold || read_temperature() < (maxtemp_threshold + MAXTEMP_OFFSET))
		return 0;
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
		rowcol = draw_string(ROWCOL(2,18), " Too Hot: ", -color);
		(void)draw_temperature(rowcol, read_temperature(), -color);
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
	(void)draw_string(ROWCOL(0,0), "Screen Inactivity Blanker", COLOR2);
	rowcol = draw_string(ROWCOL(2,20), "Timeout: ", COLOR2);
	if (blanker_timeout) {
		(void)draw_hhmmss(rowcol, blanker_timeout * SCREEN_BLANKER_MULTIPLIER, COLOR3);
	} else {
		(void)draw_string(rowcol, "[Off]", COLOR3);
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
	(void)draw_string(ROWCOL(0,0), "Screen Blanker Sensitivity", COLOR2);
	rowcol = draw_string(ROWCOL(2,0), "Examine ", COLOR2);
	rowcol = draw_number(rowcol, 100 - (blankerfuzz_amount * BLANKERFUZZ_MULTIPLIER), "%u%%", COLOR3);
	(void)   draw_string(rowcol, " of screen", COLOR2);
	return NEED_REFRESH;
}

static int
screen_compare (unsigned long *screen1, unsigned long *screen2)
{
	const unsigned char bitcount4[16] = {0,1,1,2, 1,2,2,3, 1,2,3,4, 2,3,3,4};
	int allowable_fuzz = blankerfuzz_amount * (BLANKERFUZZ_MULTIPLIER * (2 * EMPEG_SCREEN_BYTES) / 100);
	unsigned long *end = screen1 - 1;

	// Compare backwards, since changes are most frequently near bottom of screen
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
knobdata_move (int direction)
{
	knobdata_index = direction ? (knobdata_index + direction) & ((1<<KNOBDATA_BITS)-1) : 0;
	empeg_state_dirty = 1;
}

static int
knobdata_display (int firsttime)
{
	unsigned int rowcol;
	unsigned char *s = "";
 
	if (firsttime)
		ir_numeric_input = &knobdata_index;
	else if (!hijack_last_moved)
		return NO_REFRESH;
	hijack_last_moved = 0;
	clear_hijack_displaybuf(COLOR0);
	(void)draw_string(ROWCOL(0,0), "Knob Press Redefinition", COLOR2);
	switch (knobdata_released[knobdata_index]) {
		case IR_KNOB_RELEASED:		s = "[default]";break;
		case IR_RIO_SHUFFLE_RELEASED:	s = "Shuffle";	break;
		case IR_RIO_INFO_RELEASED:	s = "Info";	break;
		case IR_RIO_INFO_PRESSED:	s = "Details";	break;
	}
	rowcol = draw_string(ROWCOL(2,0), "Quick press = ", COLOR2);
	(void)draw_string(rowcol, s, COLOR3);
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
			(void)draw_string(ROWCOL(1,20), " Enhancements.v58 ", -COLOR3);
			(void)draw_string(ROWCOL(2,33), "by Mark Lord", COLOR3);
			return NEED_REFRESH;
		}
		if (jiffies_since(game_ball_last_moved) < (HZ*3))
			return NO_REFRESH;
		ir_selected = 1; // return to menu
		return NEED_REFRESH;
	}
	if (jiffies_since(game_animtime) < (HZ/(ANIMATION_FPS-2))) {
		(void)draw_string(ROWCOL(2,44), "You Win", COLOR3);
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
	game_animtime = jiffies ? jiffies : 1;
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
		ir_left_down = jiffies ? jiffies : 1;
		game_move(-1);
	} else if (ir_right_down && jiffies_since(ir_right_down) >= (HZ/15)) {
		ir_right_down = jiffies ? jiffies : 1;
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
	rowcol = draw_string(ROWCOL(0,0), "High Temperature Warning", COLOR2);
	rowcol = draw_string(ROWCOL(1,0), "Threshold: ", COLOR2);
	if (maxtemp_threshold)
		(void)draw_temperature(rowcol, maxtemp_threshold + MAXTEMP_OFFSET, COLOR3);
	else
		rowcol = draw_string(rowcol, "[Off]", COLOR3);
	rowcol = draw_string(ROWCOL(3,0), "Currently: ", COLOR2);
	(void)draw_temperature(rowcol, read_temperature(), COLOR2);
	return NEED_REFRESH;
}

#define CALCULATOR_BUTTONS_SIZE	(1 + (14 * 4))
unsigned long calculator_buttons[CALCULATOR_BUTTONS_SIZE] = {CALCULATOR_BUTTONS_SIZE,
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

static const unsigned char calculator_operators[] = {'+','-','*','/','='};

static long
calculator_do_op (long total, long value, long operator)
{
	switch (calculator_operators[operator]) {
		case '+': total += value; break;
		case '-': total -= value; break;
		case '*': total *= value; break;
		case '/': total  = value ? total / value : 0; break;
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
	unsigned long flags, button;
	unsigned char opstring[3] = {' ','\0'};

	save_flags_cli(flags);
	if (firsttime) {
		total = value = operator = prev = 0;
		hijack_buttonlist = calculator_buttons;;
		hijack_dataq_tail = hijack_dataq_head = 0;
	} else while (hijack_dataq_tail != hijack_dataq_head) {
		hijack_dataq_tail = (hijack_dataq_tail + 1) % HIJACK_DATAQ_SIZE;
		button = hijack_dataq[hijack_dataq_tail];
		for (i = CALCULATOR_BUTTONS_SIZE-1; i > 0; --i) {
			if (button == calculator_buttons[i])
				break;
		}
		if ((i & 1)) { // very clever:  if (first_or_third_column_from_table) {
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
	(void) draw_string(ROWCOL(0,0), "Menu/CD: =+-*/", COLOR2);
	(void) draw_string(ROWCOL(1,0), "Cancel/*: CE,CA,quit", COLOR2);
	(void) draw_number(ROWCOL(2,8), total, "%11d", COLOR3);
	opstring[0] = calculator_operators[operator];
	(void) draw_string(ROWCOL(3,0), opstring, COLOR3);
	(void) draw_number(ROWCOL(3,8), value, "%11d", COLOR3);
	return NEED_REFRESH;
}

static int
reboot_display (int firsttime)
{
	static unsigned char left_pressed, right_pressed;
	unsigned long flags, button;
        if (firsttime) {
		clear_hijack_displaybuf(COLOR0);
		(void) draw_string(ROWCOL(0,0), "Press & hold Left/Right\n  buttons to reboot.", COLOR3);
		(void) draw_string(ROWCOL(3,0), "Any other button aborts", COLOR3);
		left_pressed = right_pressed = 0;
		hijack_buttonlist = intercept_all_buttons;
		return NEED_REFRESH;
	}
	save_flags_cli(flags);
	if (hijack_dataq_tail != hijack_dataq_head) {
		int quit = 0;
		hijack_dataq_tail = (hijack_dataq_tail + 1) % HIJACK_DATAQ_SIZE;
		button = hijack_dataq[hijack_dataq_tail];
		switch (button) {
			case IR_LEFT_BUTTON_PRESSED:
				if (left_pressed++)
					quit = 1;
				break;
			case IR_RIGHT_BUTTON_PRESSED:
				if (right_pressed++)
					quit = 1;
				break;
			default:
				if ((button & 0x80000001) == 0)
					quit = 1;
				break;
		}
		if (quit) {
			hijack_buttonlist = NULL;
			ir_selected = 1; // return to main menu
		} else if (left_pressed && right_pressed) {
			state_cleanse();	// Ensure flash is updated first
			machine_restart(NULL);	// Reboot the machine NOW!
		}
	}
	restore_flags(flags);
	return NO_REFRESH;
}

static int
showbutton_display (int firsttime)
{
	static unsigned long prev[2];
	unsigned long flags, button;

	save_flags_cli(flags);
	if (firsttime) {
		prev[0] = prev[1] = -1;
		hijack_buttonlist = intercept_all_buttons;
	}
	if (hijack_dataq_tail != hijack_dataq_head) {
		hijack_dataq_tail = (hijack_dataq_tail + 1) % HIJACK_DATAQ_SIZE;
		button = hijack_dataq[hijack_dataq_tail];
		if (prev[0] == -1 && button & 0x80000000) {
			// ignore it: left over from selecting us off the menu
		} else if (button == prev[1] && ((button & 0x80000000) || button < 0x10)) {
			hijack_buttonlist = NULL;
			ir_selected = 1; // return to main menu
		} else {
			prev[1] = prev[0];
			prev[0] = button;
		}
	}
	restore_flags(flags);
	if (firsttime || prev[0] != -1) {
		clear_hijack_displaybuf(COLOR0);
		(void) draw_string(ROWCOL(0,0), "Button Code Display.", COLOR3);
		(void) draw_string(ROWCOL(1,0), "Repeat any button to exit", COLOR2);
		if (prev[1] != -1)
			(void)draw_number(ROWCOL(3,0), prev[1], "0x%08x", COLOR3);
		if (prev[0] != -1)
			(void)draw_number(ROWCOL(3,EMPEG_SCREEN_COLS/2), prev[0], "0x%08x", COLOR3);
		return NEED_REFRESH;
	}
	return NO_REFRESH;
}

static menu_item_t menu_table [MENU_MAX_ITEMS] = {
	{"Auto Volume Adjust",		voladj_display,		voladj_move,		0},
	{"Break-Out Game",		game_display,		game_move,		0},
	{"Button Codes Display",	showbutton_display,	NULL,			0},
	{"Calculator",			calculator_display,	NULL,			0},
	{"Countdown Timer Timeout",	timer_display,		timer_move,		0},
	{"Countdown Timer Action",	timeraction_display,	timeraction_move,	0},
	{"Font Display",		kfont_display,		NULL,			0},
	{"Force DC/Car Mode",		forcedc_display,	forcedc_move,		0},
	{"High Temperature Warning",	maxtemp_display,	maxtemp_move,		0},
#ifdef EMPEG_KNOB_SUPPORTED
	{"Knob Press Redefinition",	knobdata_display,	knobdata_move,		0},
#endif // EMPEG_KNOB_SUPPORTED
	{"Reboot Machine",		reboot_display,		NULL,			0},
	{"Screen Blanker Timeout",	blanker_display,	blanker_move,		0},
	{"Screen Blanker Sensitivity",	blankerfuzz_display,	blankerfuzz_move,	0},
	{"Vital Signs",			vitals_display,		NULL,			0},
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
		hijack_last_moved = 0;
		clear_hijack_displaybuf(COLOR0);
		save_flags_cli(flags);
		for (text_row = 0; text_row < EMPEG_TEXT_ROWS; ++text_row) {
			unsigned int index = (menu_top + text_row) % menu_size;
			(void)draw_string(ROWCOL(text_row,0), menu_table[index].label, (index == menu_item) ? COLOR3 : COLOR2);
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
		hijack_last_moved = jiffies ? jiffies : 1;
	}
}

static void
hijack_move_repeat (void)
{
	if (ir_left_down && jiffies_since(ir_left_down) >= ir_move_repeat_delay) {
		ir_left_down = jiffies ? jiffies : 1;
		hijack_move(-1);
	} if (ir_right_down && jiffies_since(ir_right_down) >= ir_move_repeat_delay) {
		ir_right_down = jiffies ? jiffies : 1;
		hijack_move(1);
	}
}

static int
test_row (void *displaybuf, unsigned short row, unsigned char color)
{
	const unsigned int offset = 10;
	unsigned char *first = ((unsigned char *)displaybuf) + (row * (EMPEG_SCREEN_COLS/2)) + offset;
	unsigned char *test  = first + ((EMPEG_SCREEN_COLS/2) - offset - offset);
	do {
		if (*--test != color)
			return 0;
	} while (test > first);
	return 1;
}

static int
check_if_player_menu_is_active (void *player_buf)
{
	if (!test_row(player_buf, 2, 0x00))
		return 0;
	if (test_row(player_buf, 0, 0x00) && test_row(player_buf, 1, 0x11))
		return 1;
	return test_row(player_buf, 3, 0x11) && test_row(player_buf, 4, 0x00);
}

static int
check_if_sound_adjust_is_active (void *player_buf)
{
	return test_row(player_buf,  8, 0x00)
	    && test_row(player_buf,  9, 0x11)
	    && test_row(player_buf, 15, 0x00)
	    && test_row(player_buf, 16, 0x11);
}

static void
toggle_input_source (void)
{
	unsigned long button;

	switch (get_current_mixer_source()) {
		case SOUND_MASK_RADIO:  // FM radio
		case SOUND_MASK_LINE1:  // AM radio
			button = IR_KW_TAPE_PRESSED; // aux
			break;
		case SOUND_MASK_LINE:   // AUX
			button = IR_KW_CD_PRESSED;   // player
			break;
		default:	// player (SOUND_MASK_PCM)
			// by hitting "aux" before "tuner", we handle "tuner not present"
			(void)real_input_append_code(IR_KW_TAPE_PRESSED);  // aux
			(void)real_input_append_code(IR_KW_TAPE_RELEASED);
			button = IR_KW_TUNER_PRESSED;  // tuner
			break;
	}
	(void)real_input_append_code(button);
	(void)real_input_append_code(button|0x80000000);
}

static int
timer_check_expiry (struct display_dev *dev)
{
	static unsigned long beeping, flags;
	int color, elapsed = (jiffies_since(timer_started) / HZ) - timer_timeout;

	if (!timer_timeout || elapsed < 0)
		return 0;
	if (dev->power) {
		if (timer_action == 0) {  // Toggle Standby?
			// Power down the player
			(void)real_input_append_code(IR_RIO_SOURCE_PRESSED);
			timer_timeout = 0; // cancel the timer
			return 0;
		}
	} else {
		// A harmless method of waking up the player
		(void)real_input_append_code(IR_RIO_VOLPLUS_PRESSED);
		(void)real_input_append_code(IR_RIO_VOLPLUS_RELEASED);
		(void)real_input_append_code(IR_RIO_VOLMINUS_PRESSED);
		(void)real_input_append_code(IR_RIO_VOLMINUS_RELEASED);
		if (timer_action == 0) {  // Toggle Standby?
			timer_timeout = 0; // cancel the timer
			return 0;
		}
	}

	// Preselect timer in the menu:
	save_flags(flags);
	if (hijack_status == HIJACK_INACTIVE) {
		while (menu_table[menu_item].dispfunc != timer_display)
			menu_move(+1);
		menu_move(+1); menu_move(-1);
	}
	restore_flags(flags);

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

// This routine covertly intercepts all display updates,
// giving us a chance to substitute our own display.
//
void
hijack_display (struct display_dev *dev, unsigned char *player_buf)
{
	unsigned char *buf = player_buf;
	unsigned long flags;
	int refresh = NEED_REFRESH;

	save_flags_cli(flags);
	if (!dev->power) {  // do (almost) nothing if unit is in standby mode
		hijack_deactivate(HIJACK_INACTIVE);
		(void)timer_check_expiry(dev);
		restore_flags(flags);
		display_blat(dev, player_buf);
		return;
	}
#ifdef EMPEG_KNOB_SUPPORTED
	if (ir_knob_down && jiffies_since(ir_knob_down) > (HZ*2)) {
		ir_knob_down = jiffies - HZ;  // allow repeated cycling if knob is held down
		toggle_input_source();
		hijack_deactivate(HIJACK_INACTIVE_PENDING);
	}
	if (ir_delayed_knob_release && jiffies_since(ir_delayed_knob_release) > (HZ+(HZ/4))) {
		ir_delayed_knob_release = 0;
		(void)real_input_append_code(knobdata_released[knobdata_index] | 0x80000000);
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
			 || (ir_knob_down && jiffies_since(ir_knob_down) >= HZ)
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
				if (ir_selected && hijack_dispfunc != userland_display && hijack_dispfunc != voladj_prefix) {
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
				ir_lasttime = jiffies; // prevents premature exit from menu reentry
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
	player_menu_is_active = player_sound_adjust_is_active = 0;
	if (buf == player_buf && !hijack_overlay_geom) {
		player_menu_is_active = check_if_player_menu_is_active(buf);
		if (!player_menu_is_active)
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
				blanker_triggered = jiffies ? jiffies : 1;
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

static void hijack_enq_button (unsigned long data)
{
	// enqueue a button code, if there's still room
	unsigned int head = (hijack_dataq_head + 1) % HIJACK_DATAQ_SIZE;
	if (head != hijack_dataq_tail)
		hijack_dataq[hijack_dataq_head = head] = data;
	wake_up(&hijack_dataq_waitq);
}

static int hijack_check_buttonlist (unsigned long data)
{
	int i;

	if (hijack_buttonlist[0] < 2) {
		// empty table means capture EVERYTHING
		hijack_enq_button(data);
		return 1;
	}
	for (i = (hijack_buttonlist[0] - 1); i > 0; --i) {
		if (data == hijack_buttonlist[i]) {
			hijack_enq_button(data);
			return 1;
		}
	}
	return 0;
}

// This routine covertly intercepts all button presses/releases,
// giving us a chance to ignore them or to trigger our own responses.
//
void  // invoked from multiple places in empeg_input.c
input_append_code(void *dev, unsigned long data)  // empeg_input.c
{
	static unsigned long ir_lastpressed = -1;
	int hijacked = 0;
	unsigned long flags;

	save_flags_cli(flags);
	blanker_triggered = 0;
	if (hijack_status == HIJACK_ACTIVE && hijack_buttonlist && hijack_check_buttonlist(data)) {
		hijacked = 1;
		goto done;
	}
	switch (data) {
#ifdef EMPEG_KNOB_SUPPORTED
		case IR_KNOB_PRESSED:
			hijacked = 1; // hijack it and later send it with the release
			if (!ir_delayed_knob_release)
				ir_knob_down = jiffies ? jiffies : 1;
			if (hijack_status == HIJACK_ACTIVE)
				hijacked = ir_selected = 1;
			break;
		case IR_KNOB_RELEASED:
			if (ir_knob_down) {
				if (hijack_status != HIJACK_INACTIVE) {
					hijacked = 1;
					if (hijack_dispfunc == voladj_prefix) {
						hijack_deactivate(HIJACK_INACTIVE);
						(void)real_input_append_code(IR_KNOB_PRESSED);
						(void)real_input_append_code(IR_KNOB_RELEASED);
					}
				} else if (hijack_status == HIJACK_INACTIVE) {
					int index = player_menu_is_active ? 0 : knobdata_index;
					hijacked = 1;
					if (jiffies_since(ir_knob_down) < (HZ/2)) {	// short press?
						if (!player_menu_is_active && !player_sound_adjust_is_active && !index) {
							activate_dispfunc(voladj_prefix, voladj_move);
						} else {
							(void)real_input_append_code(knobdata_pressed[index]);
							if (knobdata_pressed[index] == knobdata_released[index])
								ir_delayed_knob_release = jiffies ? jiffies : 1;
							else
								(void)real_input_append_code(knobdata_released[index]);
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
				ir_menu_down = jiffies ? jiffies : 1;
			}
			if (hijack_status == HIJACK_ACTIVE)
				hijacked = ir_selected = 1;
			break;
		case IR_RIO_MENU_RELEASED:
			if (hijack_status != HIJACK_INACTIVE) {
				hijacked = 1;
			} else if (ir_menu_down && !player_menu_is_active) {
				(void)real_input_append_code(IR_RIO_MENU_PRESSED);
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
				if ((ir_lastpressed & 0x7fffffff) != data || jiffies_since(ir_lasttime) > HZ)
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
				else if (hijack_dispfunc != userland_display)
					hijack_deactivate(HIJACK_INACTIVE_PENDING);
				hijacked = 1;
			}
			break;
		case IR_KW_NEXTTRACK_PRESSED:
		case IR_RIO_NEXTTRACK_PRESSED:
			ir_move_repeat_delay = (hijack_movefunc == game_move) ? (HZ/15) : (HZ/3);
			ir_right_down = jiffies ? jiffies : 1;
		case IR_KNOB_RIGHT:
			if (hijack_status != HIJACK_INACTIVE) {
				hijack_move(1);
				hijacked = 1;
			}
			break;
		case IR_KW_PREVTRACK_PRESSED:
		case IR_RIO_PREVTRACK_PRESSED:
			ir_move_repeat_delay = (hijack_movefunc == game_move) ? (HZ/15) : (HZ/3);
			ir_left_down = jiffies ? jiffies : 1;
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
	ir_lasttime    = jiffies;

	// wait for RELEASED code of most recently hijacked PRESSED code
	if (data && data == ir_releasewait) {
		ir_releasewait = 0;
		hijacked = 1;
	} else if (hijacked && (data & 0x80000001) == 0 && data != IR_KNOB_LEFT && data != IR_KNOB_RIGHT) {
		ir_releasewait = data | ((data & 0xffffff00) ? 0x80000000 : 0x00000001);
	}
	restore_flags(flags);
	if (!hijacked)
		(void)real_input_append_code(data);
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
	int rc = -ENOMEM, size = 1 + strlen(label);
	unsigned long flags;
	menu_item_t item;

	item.label = kmalloc(size, GFP_KERNEL);
	if (item.label == NULL)
		return -ENOMEM;
	memcpy(item.label, label, size);
	item.dispfunc	= userland_display;
	item.movefunc	= NULL;
	item.userdata	= userdata;
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

void	// invoked from empeg_state.c
hijack_save_settings (unsigned char *buf)
{
	// save state
	if (empeg_on_dc_power())
		hijack_savearea.voladj_dc_power	= hijack_voladj_enabled;
	else
		hijack_savearea.voladj_ac_power	= hijack_voladj_enabled;
	hijack_savearea.blanker_timeout		= blanker_timeout;
	hijack_savearea.maxtemp_threshold	= maxtemp_threshold;
#ifdef EMPEG_KNOB_SUPPORTED
	hijack_savearea.knobdata_index		= knobdata_index;
#endif // EMPEG_KNOB_SUPPORTED
	hijack_savearea.blankerfuzz_amount	= blankerfuzz_amount;
	hijack_savearea.timer_action		= timer_action;
	hijack_savearea.menu_item		= menu_item;
	//hijack_savearea.force_dcpower is only updated from the menu!
	memcpy(buf, &hijack_savearea, sizeof(hijack_savearea));
}

void	// invoked from empeg_state.c
hijack_restore_settings (const unsigned char *buf)
{
	// restore state
	memcpy(&hijack_savearea, buf, sizeof(hijack_savearea));
	hijack_force_dcpower		= hijack_savearea.force_dcpower;
	if (empeg_on_dc_power())
		hijack_voladj_enabled	= hijack_savearea.voladj_dc_power;
	else
		hijack_voladj_enabled	= hijack_savearea.voladj_ac_power;
	blanker_timeout			= hijack_savearea.blanker_timeout;
	maxtemp_threshold		= hijack_savearea.maxtemp_threshold;
#ifdef EMPEG_KNOB_SUPPORTED
	knobdata_index			= hijack_savearea.knobdata_index;
#endif // EMPEG_KNOB_SUPPORTED
	blankerfuzz_amount		= hijack_savearea.blankerfuzz_amount;
	timer_action			= hijack_savearea.timer_action;
	menu_item			= hijack_savearea.menu_item;
	menu_init();
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
	struct file *file;

	*buffer = NULL;
	lock_kernel();
	file = filp_open(path,O_RDONLY,0);
	if (!file) {
		rc = -ENOENT;
	} else {
		if ((size = file->f_dentry->d_inode->i_size) > 0) {
			unsigned char *buf = (unsigned char *)kmalloc(size, GFP_KERNEL);
			if (!buf) {
				rc = -ENOMEM;
			} else {
				mm_segment_t old_fs = get_fs();
				file->f_pos = 0;
				set_fs(get_ds());
				rc = file->f_op->read(file, buf, size, &(file->f_pos));
				set_fs(old_fs);
				if (rc < 0)
					kfree(buf);
				else
					*buffer = buf;
			}
		}
		filp_close(file,NULL);
	}
	unlock_kernel();
  	return rc;
}

static void
hijack_read_config_file (const char *path)
{
	unsigned char *buf = NULL;
	int rc = get_file(path, &buf);
	if (rc < 0) {
		printk("hijack.c: open(%s) failed (errno=%d)\n", path, rc);
	} else if (rc > 0) {

		// Code to parse config file goes here!
		// Code to parse config file goes here!
		// Code to parse config file goes here!
		// Code to parse config file goes here!
		// Code to parse config file goes here!

	}
	if (buf) kfree(buf);
}

int display_ioctl (struct inode *inode, struct file *filp, unsigned int cmd, unsigned long arg);
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
			hijack_dataq_tail = hijack_dataq_head = 0;
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
			unsigned long button;
			struct wait_queue wait = {current, NULL};
			save_flags_cli(flags);
			add_wait_queue(&hijack_dataq_waitq, &wait);
			rc = 0;
			while (1) {
				current->state = TASK_INTERRUPTIBLE;
				if (hijack_dataq_tail != hijack_dataq_head)
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
			remove_wait_queue(&hijack_dataq_waitq, &wait);
			if (!rc) {
				hijack_dataq_tail = (hijack_dataq_tail + 1) % HIJACK_DATAQ_SIZE;
				button = hijack_dataq[hijack_dataq_tail];
				restore_flags(flags);
				rc = put_user(button, (int *)arg);
			}
			return rc;
		}
		case EMPEG_HIJACK_POLLBUTTONS:	// See if any input is available
		{
			// Invocation:  rc = ioctl(fd, EMPEG_HIJACK_POLLBUTTONS, (unsigned long *)&data);
			// IR code is written to "data" on return
			unsigned long button;
			save_flags_cli(flags);
			if (hijack_dataq_tail == hijack_dataq_head) {
				restore_flags(flags);
				return -EAGAIN;
			}
			hijack_dataq_tail = (hijack_dataq_tail + 1) % HIJACK_DATAQ_SIZE;
			button = hijack_dataq[hijack_dataq_tail];
			restore_flags(flags);
			return put_user(button, (int *)arg);

		}
		case EMPEG_HIJACK_INJECTBUTTONS:	// Insert button codes into player's input queue (bypasses hijack)
		{					// same args/usage as EMPEG_HIJACK_BINDBUTTONS
			unsigned long *buttonlist = NULL;
			int i;
			if ((rc = copy_buttonlist_from_user(arg, &buttonlist, 256)))
				return rc;
			save_flags_cli(flags);
			for (i = 1; i < buttonlist[0]; ++i) {
				while (!signal_pending(current) && real_input_append_code(buttonlist[i])) {
					// no room in input queue, so wait 1/10sec and try again
					restore_flags(flags);
					current->state = TASK_INTERRUPTIBLE;
					schedule_timeout(HZ/10);
					current->state = TASK_RUNNING;
					save_flags_cli(flags);
				}
			}
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
			static int read_config = 0;
			if (!read_config) {
				read_config = 1;
				hijack_read_config_file("/empeg/var/config.ini");
			}
			return display_ioctl(inode, filp, cmd, arg);
		}
	}
}

// initial setup of hijack menu system
void
hijack_init (void)
{
	menu_init();
	init_temperature();
}

