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
//

#include <asm/arch/hijack.h>

#define NEED_REFRESH		0
#define NO_REFRESH		1

#define HIJACK_INACTIVE		0
#define HIJACK_INACTIVE_PENDING	1
#define HIJACK_PENDING		2
#define HIJACK_ACTIVE		3

static unsigned int hijack_status = HIJACK_INACTIVE;
static unsigned long hijack_last_moved = 0, hijack_last_refresh = 0, blanker_activated = 0;

static int  (*hijack_dispfunc)(int) = NULL;
static void (*hijack_movefunc)(int) = NULL;
static unsigned int ir_lasttime = 0, ir_selected = 0, ir_releasewait = 0, ir_knob_down = 0, ir_left_down = 0, ir_right_down = 0, ir_trigger_count = 0;
static unsigned long ir_delayed_knob_release = 0;
extern int real_input_append_code(void *dev, unsigned long data); // empeg_input.c
static int hijack_player_menu_is_active = 0, hijack_sound_adjust_is_active = 0;
static unsigned char *hijack_player_buf = NULL;

#define KNOBDATA_BITS 2
static const unsigned long knobdata_pressed [] = {IR_KNOB_PRESSED, IR_RIO_SHUFFLE_PRESSED, IR_RIO_INFO_PRESSED, IR_RIO_INFO_PRESSED};
static const unsigned long knobdata_released[] = {IR_KNOB_RELEASED,IR_RIO_SHUFFLE_RELEASED,IR_RIO_INFO_RELEASED,IR_RIO_INFO_PRESSED};
static short knobdata_index = 0;

#define HIJACK_DATAQ_SIZE	8
static unsigned long *hijack_buttonlist = NULL, hijack_dataq[HIJACK_DATAQ_SIZE];
static unsigned int hijack_dataq_head = 0, hijack_dataq_tail = 0;
static struct wait_queue *hijack_dataq_waitq = NULL, *hijack_menu_waitq = NULL;

#define MULT_POINT		12
#define MULT_MASK		((1 << MULT_POINT) - 1)
#define VOLADJ_THRESHSIZE	16
#define VOLADJ_HISTSIZE		128
#define VOLADJ_FIXEDPOINT(whole,fraction) ((((whole)<<MULT_POINT)|((unsigned int)((fraction)*(1<<MULT_POINT))&MULT_MASK)))
#define VOLADJ_BITS 2
extern void voladj_next_preset(int);
extern int voladj_enabled;
extern unsigned int voladj_multiplier;
static const char *voladj_names[] = {"[Off]", "Low", "Medium", "High"};
unsigned int voladj_histx = 0, voladj_history[VOLADJ_HISTSIZE] = {0,};

#define SCREEN_BLANKER_MULTIPLIER 15
#define BLANKER_BITS (8 - VOLADJ_BITS)
int blanker_timeout = 0;	// saved/restored in empeg_state.c

static int maxtemp_threshold = 0, hijack_on_dc_power = 0;
#define MAXTEMP_OFFSET	29
#define MAXTEMP_BITS	(8 - VOLADJ_BITS)

unsigned long jiffies_since(unsigned long past_jiffies);

static unsigned char hijack_displaybuf[EMPEG_SCREEN_ROWS][EMPEG_SCREEN_COLS/2];

const unsigned char kfont [1 + '~' - ' '][KFONT_WIDTH] = {  // variable width font
	{0x00,0x00,0x00,0x00,0x00,0x00}, // space
	{0x5f,0x00,0x00,0x00,0x00,0x00}, // !
	{0x03,0x00,0x03,0x00,0x00,0x00}, // "
	{0x14,0x3e,0x14,0x3e,0x14,0x00}, // #
	{0x24,0x2a,0x7f,0x2a,0x12,0x00}, // $
	{0x26,0x16,0x08,0x34,0x32,0x00}, // %
	{0x02,0x01,0x00,0x00,0x00,0x00}, // singlequote
	{0x36,0x49,0x56,0x20,0x50,0x00}, // ampersand
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
	unsigned char pixel_mask, *pixel_pair;
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
		unsigned char *displayrow, pixel_mask = (pixel_col & 1) ? 0xf0 : 0x0f;
		unsigned int offset = 0;
		if ((row + pixel_row) >= EMPEG_SCREEN_ROWS)
			return;
		displayrow = &hijack_displaybuf[row + pixel_row][0];
		do {
			unsigned char new_pixel   = 0;
			unsigned char *pixel_pair = &displayrow[(pixel_col + offset) >> 1];
			*pixel_pair = ((*pixel_pair & (pixel_mask = ~pixel_mask))) ^ new_pixel;
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
activate_dispfunc (int (*dispfunc)(int), void (*movefunc)(int))
{
	ir_selected = 0;
	ir_trigger_count = 0;
	hijack_overlay_geom = NULL;
	if (dispfunc != NULL) {
		hijack_dispfunc = dispfunc;
		hijack_movefunc = movefunc;
		hijack_status = HIJACK_PENDING;
		dispfunc(1);
	}
}

static void
hijack_deactivate (void)
{
	hijack_movefunc = NULL;
	hijack_dispfunc = NULL;
	hijack_overlay_geom = NULL;
	hijack_status = HIJACK_INACTIVE_PENDING;
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
		unsigned short height = VOLADJ_THRESHSIZE, pixel_row, threshold, color;
		int mdiff, tdiff;
		do {
			threshold = voladj_thresholds[--height];
		} while (multiplier < threshold && height != 0);
		pixel_row = (text_row * KFONT_HEIGHT) + (VOLADJ_THRESHSIZE - 1) - height;
		mdiff = multiplier - threshold;
		if (height < (VOLADJ_THRESHSIZE - 1))
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

static void
voladj_move (int direction)
{
	voladj_enabled += direction;
	if (voladj_enabled < 0)
		voladj_enabled = 0;
	else if (voladj_enabled > (sizeof(voladj_names) / sizeof(voladj_names[0]) - 1))
		voladj_enabled  = sizeof(voladj_names) / sizeof(voladj_names[0]) - 1;
}

static int
voladj_display (int firsttime)
{
	unsigned int i, rowcol, mult, prev = -1;
	unsigned char buf[32];
	unsigned long flags;
	static unsigned long last_histx = -1;

	if (firsttime) {
		memset(voladj_history, 0, sizeof(voladj_history));
		voladj_histx = 0;
	} else if (!hijack_last_moved && last_histx == voladj_histx) {
		return NO_REFRESH;
	}
	hijack_last_moved = 0;
	last_histx = voladj_histx;
	clear_hijack_displaybuf(COLOR0);
	save_flags_cli(flags);
	voladj_history[voladj_histx = (voladj_histx + 1) % VOLADJ_HISTSIZE] = voladj_multiplier;
	restore_flags(flags);
	rowcol = draw_string(ROWCOL(0,0), "Auto Volume Adjust: ", COLOR2);
	(void)draw_string(rowcol, voladj_names[voladj_enabled], COLOR3);
	mult = voladj_multiplier;
	sprintf(buf, "Current Multiplier: %2u.%02u", mult >> MULT_POINT, (mult & MULT_MASK) * 100 / (1 << MULT_POINT));
	(void)draw_string(ROWCOL(3,12), buf, COLOR2);
	save_flags_cli(flags);
	for (i = 1; i <= VOLADJ_HISTSIZE; ++i)
		(void)voladj_plot(1, i - 1, voladj_history[(voladj_histx + i) % VOLADJ_HISTSIZE], &prev);
	restore_flags(flags);
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
		hijack_deactivate();
		ir_selected = 0;
		hijack_status = HIJACK_INACTIVE;
	} else if (hijack_player_buf) {
		unsigned int rowcol = (geom.first_row+4)|((geom.first_col+6)<<16);
		rowcol = draw_string(rowcol, "Auto VolAdj: ", COLOR3);
		clear_text_row(rowcol, geom.last_col-4);
		rowcol = draw_string(rowcol, voladj_names[voladj_enabled], COLOR3);
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

extern int empeg_readtherm(volatile unsigned int *timerbase, volatile unsigned int *gpiobase);
extern int empeg_inittherm(volatile unsigned int *timerbase, volatile unsigned int *gpiobase);
extern int get_loadavg(char * buffer);

static void
init_temperature (void)
{
	unsigned long flags;

	save_flags_clif(flags);
	(void)empeg_inittherm(&OSMR0,&GPLR);
	restore_flags(flags);
}

static int
read_temperature (void)
{
	static unsigned long lastread = 0;
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

	if (lastread && jiffies_since(lastread) < (HZ*2))
		return temp;
	save_flags_clif(flags);			//  power cyles without inittherm()
	temp = empeg_readtherm(&OSMR0,&GPLR);
	restore_flags(flags);
	lastread = jiffies ? jiffies : 1;
	if (((lastread / HZ) & 0x63) == 0) // restart the thermometer once a minute or so
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
	sprintf(buf, "%dC/%dF", temp, temp * 180 / 100 + 32);
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
	sprintf(buf, "HwRev:%02d, Build:%x\nTemperature: ", permset[0], permset[3]);
	rowcol = draw_string(ROWCOL(0,0), buf, COLOR2);
	(void)draw_temperature(rowcol, read_temperature(), COLOR2);
	si_meminfo(&si);
	sprintf(buf, "Free: %lu/%lu\nLoadAvg: ", si.freeram, si.totalram);
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
blanker_move (int direction)
{
	blanker_timeout += direction;
	if (blanker_timeout < 0)
		blanker_timeout = 0;
	else if (blanker_timeout > ((1<<BLANKER_BITS)-1))
		blanker_timeout  = ((1<<BLANKER_BITS)-1);
}

static int
blanker_display (int firsttime)
{
	unsigned int rowcol;

	if (!firsttime && !hijack_last_moved)
		return NO_REFRESH;
	hijack_last_moved = 0;
	clear_hijack_displaybuf(COLOR0);
	(void)draw_string(ROWCOL(0,0), "Screen Inactivity Timeout", COLOR2);
	rowcol = draw_string(ROWCOL(2,0), "Blank", COLOR2);
	if (blanker_timeout) {
		rowcol = draw_string(rowcol, " after ", COLOR2);
		rowcol = draw_number(rowcol, blanker_timeout * SCREEN_BLANKER_MULTIPLIER, "%u", COLOR3);
		(void)   draw_string(rowcol, " secs", COLOR2);
	} else {
		(void)draw_string(rowcol, ":  [Off]", COLOR3);
	}
	return NEED_REFRESH;
}

#define BLANKERFUZZ_BITS 3
static int blankerfuzz_5pcts = 0;

static void
blankerfuzz_move (int direction)
{
	blankerfuzz_5pcts += direction;
	if (blankerfuzz_5pcts < 0)
		blankerfuzz_5pcts = 0;
	else if (blankerfuzz_5pcts > ((1<<BLANKERFUZZ_BITS)-1))
		blankerfuzz_5pcts  = ((1<<BLANKERFUZZ_BITS)-1);
}

static int
blankerfuzz_display (int firsttime)
{
	unsigned int rowcol;

	if (!firsttime && !hijack_last_moved)
		return NO_REFRESH;
	hijack_last_moved = 0;
	clear_hijack_displaybuf(COLOR0);
	(void)draw_string(ROWCOL(0,0), "Screen Blanker Sensitivity", COLOR2);
	rowcol = draw_string(ROWCOL(2,0), "Examine ", COLOR2);
	rowcol = draw_number(rowcol, 100 - (5 * blankerfuzz_5pcts), "%u%%", COLOR3);
	(void)   draw_string(rowcol, " of screen", COLOR2);
	return NEED_REFRESH;
}

static int
screen_compare (unsigned long *screen1, unsigned long *screen2)
{
	const unsigned char bitcount4[16] = {0,1,1,2, 1,2,2,3, 1,2,3,4, 2,3,3,4};
	unsigned long *end = (unsigned long *)(((unsigned char *)screen1) + EMPEG_SCREEN_BYTES);
	int allowable_fuzz = blankerfuzz_5pcts * (5 * (2 * EMPEG_SCREEN_BYTES) / 100);
	do {	// compare 8 pixels at a time for speed
		unsigned long x = *screen1 ^ *screen2++;
		if (x) { // Now figure out how many of the 8 pixels didn't match
			x |= x >>  1;		// reduce each pixel to one bit
			x &= 0x11111111;	// mask away the excess bits
			x |= x >> 15;		// move the upper 4 pixels to beside the lower 4
			x |= x >>  6;		// Squish all eight pixels into one byte
			allowable_fuzz -= bitcount4[x & 0xf] + bitcount4[(x >> 4) & 0xf];
			if (allowable_fuzz < 0)
				return 1;	// not the same
		}
	} while (++screen1 < end);
	return 0;	// the same
}

static void
knobdata_move (int direction)
{
	knobdata_index = (knobdata_index + direction) & ((1<<KNOBDATA_BITS)-1);
}

static int
knobdata_display (int firsttime)
{
	unsigned int rowcol;
	unsigned char *s = "";

	if (!firsttime && !hijack_last_moved)
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

#define GAME_COLS		(EMPEG_SCREEN_COLS/2)
#define GAME_VBOUNCE		0xff
#define GAME_BRICKS		0xee
#define GAME_BRICKS_ROW		5
#define GAME_HBOUNCE		0x77
#define GAME_BALL		0xff
#define GAME_OVER		0x11
#define GAME_PADDLE_SIZE	8

static short game_over, game_row, game_col, game_hdir, game_vdir, game_paddle_col, game_paddle_lastdir, game_speed, game_bricks;
static unsigned long game_ball_last_moved, game_animtime, game_paused;
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
			(void)draw_string(ROWCOL(1,20), " Enhancements.v42 ", -COLOR3);
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
	if (game_paused || (jiffies_since(game_ball_last_moved) < (HZ/game_speed))) {
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
	game_paused = 0;
	game_speed = 16;
	game_animtime = 0;
	return NEED_REFRESH;
}

static void
maxtemp_move (int direction)
{
	if (maxtemp_threshold == 0) {
		if (direction > 0)
			++maxtemp_threshold;
	} else if (direction < 0) {
		--maxtemp_threshold;
	} else if (maxtemp_threshold < ((1<<MAXTEMP_BITS)-1)) {
		++maxtemp_threshold;
	}
}

static int
maxtemp_display (int firsttime)
{
	unsigned int rowcol;

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

static const unsigned char calculator_operators[] = {'=','+','-','*','/'};

static long
calculator_do_op (long total, long value, long operator)
{
	switch (calculator_operators[operator]) {
		case '=': total  = value; break;
		case '+': total += value; break;
		case '-': total -= value; break;
		case '*': total *= value; break;
		case '/': total = value ? total / value : 0; break;
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
					if (new > value)
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
showbutton_display (int firsttime)
{
	static unsigned long prev[2];
	static unsigned long showbutton_buttonlist[] = {1};
	unsigned long flags, button;

	save_flags_cli(flags);
	if (firsttime) {
		prev[0] = prev[1] = -1;
		hijack_buttonlist = showbutton_buttonlist;
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

typedef int  (menu_dispfunc_t)(int);
typedef void (menu_movefunc_t)(int);

typedef struct menu_item_s {
	char			*label;
	menu_dispfunc_t		*dispfunc;
	menu_movefunc_t		*movefunc;
	unsigned long		userdata;
} menu_item_t;

static volatile short menu_item = 0, menu_size = 0, menu_top = 0;

static menu_item_t menu_table [] = {
	{"Auto Volume Adjust",		voladj_display,		voladj_move,		0},
	{"Break-Out Game",		game_display,		game_move,		0},
	{"Button Codes Display",	showbutton_display,	NULL,			0},
	{"Calculator",			calculator_display,	NULL,			0},
	{"Font Display",		kfont_display,		NULL,			0},
	{"High Temperature Warning",	maxtemp_display,	maxtemp_move,		0},
	{"Knob Press Redefinition",	knobdata_display,	knobdata_move,		0},
	{"Screen Blanker Time-out",	blanker_display,	blanker_move,		0},
	{"Screen Blanker Sensitivity",	blankerfuzz_display,	blankerfuzz_move,	0},
	{NULL,				NULL,			NULL,			0},
	{NULL,				NULL,			NULL,			0},
	{NULL,				NULL,			NULL,			0},
	{NULL,				NULL,			NULL,			0},
	{NULL,				NULL,			NULL,			0},
	{NULL,				NULL,			NULL,			0},
	{NULL,				NULL,			NULL,			0}};

#define MENU_MAX_SIZE (sizeof(menu_table) / sizeof(menu_table[0]))

static void
menu_move (int direction)
{
	short menu_max = menu_size - 1;
	if (direction < 0) {
		if (menu_item == menu_top && --menu_top < 0)
			menu_top = menu_max;
		if (--menu_item < 0)
			menu_item = menu_max;
	} else {
		short bottom = (menu_top + (EMPEG_TEXT_ROWS - 1)) % menu_size;
		if (menu_item == bottom && ++menu_top > menu_max)
			menu_top = 0;
		if (++menu_item > menu_max)
			menu_item = 0;
	}
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
		hijack_deactivate(); // menu timed-out
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

static int
hijack_move (int direction)
{
	if (hijack_movefunc != NULL)
		hijack_movefunc(direction);
	hijack_last_moved = jiffies ? jiffies : 1;
	return 1; // input WAS hijacked
}

static void
hijack_move_repeat (void)
{
	unsigned int repeat_interval = (hijack_movefunc == game_move) ? (HZ/15) : (HZ/3);
	if (ir_left_down && jiffies_since(ir_left_down) >= repeat_interval) {
		ir_left_down = jiffies ? jiffies : 1;
		hijack_move(-1);
	} else if (ir_right_down && jiffies_since(ir_right_down) >= repeat_interval) {
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

// This routine covertly intercepts all display updates,
// giving us a chance to substitute our own display.
//
void
hijack_display (struct display_dev *dev, unsigned char *player_buf)
{
	unsigned char *buf = (unsigned char *)hijack_displaybuf;
	unsigned long flags;
	int refresh = NEED_REFRESH, color;

	save_flags_cli(flags);
	switch (hijack_status) {
		case HIJACK_INACTIVE:
			if (!dev->power) {  // do not activate menu if unit is in standby mode!
				buf = player_buf;
			} else if (ir_trigger_count >= 6 || (ir_knob_down && jiffies_since(ir_knob_down) >= HZ)) {
				activate_dispfunc(menu_display, menu_move);
			} else if (jiffies_since(ir_lasttime) < (HZ*5) || !maxtemp_threshold || read_temperature() < (maxtemp_threshold + MAXTEMP_OFFSET)) {
				buf = player_buf;
			} else {
				unsigned int rowcol;
				color = ((jiffies / HZ) & 1) ? COLOR3 : -COLOR3;
				clear_hijack_displaybuf(color);
				rowcol = draw_string(ROWCOL(2,18), " Too Hot: ", -color);
				(void)draw_temperature(rowcol, read_temperature(), -color);
				buf = (unsigned char *)hijack_displaybuf;
			}
			break;
		case HIJACK_ACTIVE:
			if (hijack_dispfunc == NULL) {  // userland app finished?
				activate_dispfunc(menu_display, menu_move);
			} else {
				if (hijack_movefunc != NULL)
					hijack_move_repeat();
				restore_flags(flags);
				hijack_player_buf = player_buf;
				refresh = hijack_dispfunc(0);
				save_flags_cli(flags);
				if (ir_selected && hijack_dispfunc != userland_display)
					activate_dispfunc(menu_display, menu_move);
			}
			if (hijack_overlay_geom) {
				hijack_do_overlay (player_buf, (unsigned char *)hijack_displaybuf, hijack_overlay_geom);
				buf = player_buf;
			}
			break;
		case HIJACK_PENDING:
			if (!ir_releasewait) {
				ir_selected = 0;
				ir_lasttime = jiffies; // prevents premature exit from menu reentry
				hijack_status = HIJACK_ACTIVE;
			}
			break;
		case HIJACK_INACTIVE_PENDING:
			if (!ir_releasewait) {
				ir_selected = 0;
				hijack_status = HIJACK_INACTIVE;
			}
			break;
		default: // (good) paranoia
			hijack_deactivate();
			break;
	}
	// Use screen-scraping to keep track of some of the player states:
	hijack_player_menu_is_active = hijack_sound_adjust_is_active = 0;
	if (buf == player_buf && !hijack_overlay_geom) {
		hijack_player_menu_is_active = check_if_player_menu_is_active(buf);
		if (!hijack_player_menu_is_active)
			hijack_sound_adjust_is_active = check_if_sound_adjust_is_active(buf);
	}
	restore_flags(flags);

	// Prevent screen burn-in on an inactive/unattended player:
	if (dev->power && blanker_timeout) {
		static unsigned long blanker_lastpoll = 0;
		static unsigned char blanked = 0, last_buf[EMPEG_SCREEN_BYTES] = {0,};
		if (!blanker_activated)
			blanked = 0;
		if (jiffies_since(blanker_lastpoll) >= (4*HZ/3)) {  // use an oddball interval to avoid patterns
			blanker_lastpoll = jiffies;
			if (screen_compare((unsigned long *)last_buf, (unsigned long *)buf)) {
				memcpy(last_buf, buf, EMPEG_SCREEN_BYTES);
				blanker_activated = 0;
			} else {
				refresh = NO_REFRESH;
				if (!blanker_activated)
					blanker_activated = jiffies ? jiffies : 1;
			}
		}
		if (blanker_activated) {
			if (jiffies_since(blanker_activated) > jiffies_since(ir_lasttime)) {
				blanker_activated = 0;
			} else if (jiffies_since(blanker_activated) > (blanker_timeout * (SCREEN_BLANKER_MULTIPLIER * HZ))) {
				if (!blanked) {
					buf = player_buf;
					memset(buf,0x00,EMPEG_SCREEN_BYTES);
					refresh = NEED_REFRESH;
				}
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
}

// This routine covertly intercepts all button presses/releases,
// giving us a chance to ignore them or to trigger our own responses.
//
void  // invoked from multiple places in empeg_input.c
input_append_code(void *dev, unsigned long data)  // empeg_input.c
{
	static unsigned long ir_lastpressed = 0;
	int i, hijacked = 0, knob_prefix_was_active;
	unsigned long flags;

	if (data == IR_KNOB_RELEASED && data == ir_lastpressed)
		return;	// we sometimes get two of these in a row (??)
	save_flags_cli(flags);
	knob_prefix_was_active = (hijack_dispfunc == voladj_prefix);
	blanker_activated = 0;
	if (ir_delayed_knob_release && jiffies_since(ir_delayed_knob_release) > (HZ+(HZ/4))) {
		ir_delayed_knob_release = 0;
		real_input_append_code(dev, knobdata_released[knobdata_index] | 0x80000000);
	}
	if (hijack_status != HIJACK_INACTIVE) {
		if (hijack_buttonlist && hijack_status == HIJACK_ACTIVE) {
			if (hijack_buttonlist[0] < 2) {
				// "an empty table exists": means capture EVERYTHING
				hijack_enq_button(data);
				hijacked = 1;
			} else for (i = (hijack_buttonlist[0] - 1); i > 0; --i) {
				if (data == hijack_buttonlist[i]) {
					hijack_enq_button(data);
					hijacked = 1;
					break;
				}
			}
			if (hijack_dataq_tail != hijack_dataq_head)
				wake_up(&hijack_dataq_waitq);
		}
		if (!hijacked) {
			hijacked = 1;
			switch (data) {
				case IR_KW_STAR_PRESSED:
				case IR_RIO_CANCEL_PRESSED:
				case IR_TOP_BUTTON_PRESSED:
					if (hijack_dispfunc != userland_display)
						hijack_deactivate();
					break;
				case IR_RIO_MENU_PRESSED:
				case IR_KW_CD_PRESSED:
				case IR_KNOB_PRESSED:
					if (!knob_prefix_was_active)
						ir_selected = 1;
					break;
				case IR_KW_NEXTTRACK_PRESSED:
				case IR_RIO_NEXTTRACK_PRESSED:
				case IR_KNOB_RIGHT:
					if (hijack_status == HIJACK_ACTIVE)
						hijacked = hijack_move(1);
					break;
				case IR_KW_PREVTRACK_PRESSED:
				case IR_RIO_PREVTRACK_PRESSED:
				case IR_KNOB_LEFT:
					if (hijack_status == HIJACK_ACTIVE)
						hijacked = hijack_move(-1);
					break;
				case IR_KNOB_RELEASED:  // note: this one often arrives in pairs, so check knob_down first!
					if (ir_knob_down && knob_prefix_was_active && hijack_status == HIJACK_ACTIVE) { 
						hijack_deactivate();
						ir_selected = 0;
						hijack_status = HIJACK_INACTIVE;
						real_input_append_code(dev, IR_KNOB_PRESSED);
						real_input_append_code(dev, IR_KNOB_RELEASED);
					}
					break;
				case IR_KW_NEXTTRACK_RELEASED:
				case IR_KW_PREVTRACK_RELEASED:
				case IR_RIO_NEXTTRACK_RELEASED:
				case IR_RIO_PREVTRACK_RELEASED:
				case IR_RIO_MENU_RELEASED:
					break;
				case IR_KW_PAUSE_PRESSED:
				case IR_RIO_PAUSE_PRESSED:
					game_paused = !game_paused;
					// fall thru
				default:
					hijacked = 0;
					break;
			}
		}
	} else if (devices[0].power && data == IR_KW_CD_PRESSED) {
		// ugly Kenwood remote hack: press/release CD quickly 3 times to activate menu
		if ((ir_lastpressed & 0x7fffffff) == data && jiffies_since(ir_lasttime) < HZ)
			++ir_trigger_count;
		else
			ir_trigger_count = 1;
	}
	// Update button states
	if (ir_releasewait && data == ir_releasewait) {
		ir_releasewait = 0;
		hijacked = 1;
	} else if (hijacked && (data & 0x80000001) == 0 && data != IR_KNOB_LEFT && data != IR_KNOB_RIGHT) {
		ir_releasewait = data | ((((int)data) > 16) ? 0x80000000 : 0x00000001);
	}
	// save button PRESSED codes in ir_lastpressed
	ir_lastpressed = data;
	ir_lasttime = jiffies;
	switch (data) {
		case IR_RIO_MENU_PRESSED:
			if (hijack_status == HIJACK_INACTIVE && !hijack_player_menu_is_active) {
				hijacked = 1; // hijack it and later send it with the release (below)
				ir_knob_down = jiffies ? jiffies : 1;
			}
			break;
		case IR_RIO_MENU_RELEASED:
			if (ir_knob_down && hijack_status == HIJACK_INACTIVE && !hijack_player_menu_is_active) {
				if (!hijacked && jiffies_since(ir_knob_down) < (HZ/2))
					real_input_append_code(dev, IR_RIO_MENU_PRESSED);
			}
			ir_knob_down = 0;
			break;
		case IR_KNOB_PRESSED:
			hijacked = 1; // hijack it and later send it with the release (below)
			if (!ir_delayed_knob_release)
				ir_knob_down = jiffies ? jiffies : 1;
			break;
		case IR_KNOB_RELEASED:  // note: this one often arrives in pairs, so check knob_down first!
			if (ir_knob_down && hijack_status == HIJACK_INACTIVE && !hijacked) {
				int index = hijack_player_menu_is_active ? 0 : knobdata_index;
				data = knobdata_released[index];	// substitute our translation
				if (jiffies_since(ir_knob_down) < (HZ/2)) {	// short press?
					if (!(hijack_player_menu_is_active | index | knob_prefix_was_active | hijack_sound_adjust_is_active)) {
						activate_dispfunc(voladj_prefix, voladj_move);
						hijacked = 1;
					} else {
						real_input_append_code(dev, knobdata_pressed[index]);
						if (knobdata_pressed[index] == knobdata_released[index]) {
							hijacked = 1; // eat the released code for now
							ir_delayed_knob_release = jiffies ? jiffies : 1;
						}
					}
				}
			}
			ir_knob_down = 0;
			break;
		case IR_KW_PREVTRACK_PRESSED:
		case IR_RIO_PREVTRACK_PRESSED:
			ir_left_down = jiffies ? jiffies : 1;
			break;
		case IR_KW_PREVTRACK_RELEASED:
		case IR_RIO_PREVTRACK_RELEASED:
			ir_left_down = 0;
			break;
		case IR_KW_NEXTTRACK_PRESSED:
		case IR_RIO_NEXTTRACK_PRESSED:
			ir_right_down = jiffies ? jiffies : 1;
			break;
		case IR_KW_NEXTTRACK_RELEASED:
		case IR_RIO_NEXTTRACK_RELEASED:
			ir_right_down = 0;
			break;
	}
	restore_flags(flags);
	if (!hijacked)
		real_input_append_code(dev, data);
}

// returns menu index >= 0,  or -ERROR
static int
extend_menu (menu_item_t *new)
{
	int i;
	for (i = 0; i < MENU_MAX_SIZE; ++i) {
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

// format of eight-byte flash memory savearea used for our hijack'd settings
static struct sa_struct {
	unsigned voladj_ac_power	: VOLADJ_BITS;
	unsigned blanker_timeout	: BLANKER_BITS;
	unsigned voladj_dc_power	: VOLADJ_BITS;
	unsigned maxtemp_threshold	: MAXTEMP_BITS;
	unsigned knobdata_index		: KNOBDATA_BITS;
	unsigned blankerfuzz_5pcts	: BLANKERFUZZ_BITS;
	unsigned leftover		: (8 - (KNOBDATA_BITS + BLANKERFUZZ_BITS));
	unsigned byte4			: 8;
	unsigned byte5			: 8;
	unsigned byte6			: 8;
	unsigned byte7			: 8;
} hijack_savearea;

void	// invoked from empeg_state.c
hijack_save_settings (unsigned char *buf)
{
	// save state
	if (hijack_on_dc_power)
		hijack_savearea.voladj_dc_power	= voladj_enabled;
	else
		hijack_savearea.voladj_ac_power	= voladj_enabled;
	hijack_savearea.blanker_timeout		= blanker_timeout;
	hijack_savearea.maxtemp_threshold	= maxtemp_threshold;
	hijack_savearea.knobdata_index		= knobdata_index;
	hijack_savearea.blankerfuzz_5pcts	= blankerfuzz_5pcts;
	memcpy(buf, &hijack_savearea, sizeof(hijack_savearea));
}

void	// invoked from empeg_state.c
hijack_restore_settings (const unsigned char *buf)
{
	// restore state
	memcpy(&hijack_savearea, buf, sizeof(hijack_savearea));
	if (hijack_on_dc_power)
		voladj_enabled	= hijack_savearea.voladj_dc_power;
	else
		voladj_enabled	= hijack_savearea.voladj_ac_power;
	blanker_timeout		= hijack_savearea.blanker_timeout;
	maxtemp_threshold	= hijack_savearea.maxtemp_threshold;
	knobdata_index		= hijack_savearea.knobdata_index;
	blankerfuzz_5pcts	= hijack_savearea.blankerfuzz_5pcts;
}

static int
hijack_wait_on_menu (char *argv[])
{
	struct wait_queue wait = {current, NULL};
	int i, rc, index, num_items, indexes[MENU_MAX_SIZE];
	unsigned long flags, userdata = (unsigned long)current->pid << 8;

	// (re)create the menu items, with our pid/index as userdata
	for (i = 0; *argv && i < MENU_MAX_SIZE; ++i) {
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

static int  // invoked from empeg_display.c::ioctl()
hijack_ioctl (struct inode *inode, struct file *filp, unsigned int cmd, unsigned long arg)
{
	unsigned long flags;
	int rc;

	//struct display_dev *dev = (struct display_dev *)filp->private_data;
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
				extern void *input_devices;
				while (!signal_pending(current) && real_input_append_code(input_devices, buttonlist[i])) {
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
			// Invocation:  rc = ioctl(fd, EMPEG_HIJACK_DISPCLEAR, (hijack_geom_t *)&geom);
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
			return -EINVAL;
	}
}

// initial setup of hijack menu system
void
hijack_init (void)
{
	extern int getbitset(void);
	menu_item_t item = {"Vital Signs", vitals_display, NULL, 0};
	(void)extend_menu(&item);	// we need at least one call to extend_menu() to set the menu_size!!
	hijack_on_dc_power = getbitset() & EMPEG_POWER_FLAG_DC;
	init_temperature();
}
