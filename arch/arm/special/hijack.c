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
//
//
// /* Basic sequence for userland app to bind into the menu, display, and IR buttons */
// /* Not shown: all "rc" return codes have to be checked for success/failure */
//
//    #include <asm/arch/hijack.h>
//    int fd;
//    unsigned long data, buttons[5] = {5, IR_KW_PREVTRACK_PRESSED, IR_KW_PREVTRACK_RELEASED, IR_KW_NEXTTRACK_PRESSED, IR_KW_NEXTTRACK_RELEASED};
//    unsigned char screenbuf[EMPEG_SCREEN_BYTES] = {0,};
//
//    fd = open("/dev/display");
//    top: while (1) {
//       rc = ioctl(fd, EMPEG_HIJACK_WAITMENU, "MyStuff");
//       rc = ioctl(fd,EMPEG_HIJACK_BINDBUTTONS, buttons);
//       while (looping) {
//          rc = ioctl(fd,EMPEG_HIJACK_DISPWRITE, screenbuf); /* or ioctl(fd,EMPEG_HIJACK_DISPTEXT, "Some\nText"); */
//          rc = ioctl(fd,EMPEG_HIJACK_WAITBUTTONS, &data);
//       }
//       rc = ioctl(fd,EMPEG_HIJACK_UNBINDBUTTONS, NULL); /* VERY important! */
//    }
//

#include <asm/arch/hijack.h>

#define NEED_REFRESH		0
#define NO_REFRESH		1

#define HIJACK_INACTIVE		0
#define HIJACK_INACTIVE_PENDING	1
#define HIJACK_PENDING		2
#define HIJACK_ACTIVE		3

static unsigned int hijack_status = HIJACK_INACTIVE;
static unsigned long hijack_last_moved = 0, hijack_userdata = 0, hijack_last_refresh = 0, blanker_activated = 0;

static int  (*hijack_dispfunc)(int), menu_item = 0, menu_size = 0, menu_top = 0;
static void (*hijack_movefunc)(int) = NULL;
static unsigned int ir_selected = 0, ir_releasewait = 0, ir_knob_down = 0, ir_left_down = 0, ir_right_down = 0, ir_trigger_count = 0;
static unsigned long ir_prev_pressed = 0;

#define HIJACK_DATAQ_SIZE	8
static unsigned long *hijack_buttonlist = NULL, hijack_dataq[HIJACK_DATAQ_SIZE];
static unsigned int hijack_dataq_head = 0, hijack_dataq_tail = 0;


#define SCREEN_BLANKER_MULTIPLIER 30
int blanker_timeout = 0;	// saved/restored in empeg_state.c

static int maxtemp_threshold, hijack_on_dc_power;
#define MAXTEMP_OFFSET	29
#define MAXTEMP_BITS	6

unsigned long jiffies_since(unsigned long past_jiffies);

static unsigned char hijack_displaybuf[EMPEG_SCREEN_ROWS][EMPEG_SCREEN_COLS/2];

#define COLOR0 0 // blank pixels
#define COLOR1 1 // prefix with '-' for inverse video
#define COLOR2 2 // prefix with '-' for inverse video
#define COLOR3 3 // prefix with '-' for inverse video

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
	memset(hijack_displaybuf,color,EMPEG_SCREEN_BYTES);
}

static void
draw_pixel (int pixel_row, unsigned int pixel_col, int color)
{
	unsigned char pixel_mask, *displayrow, *pixel_pair;
	if (color < 0)
		color = -color;
	color &= 3;
	color |= color << 4;
	displayrow  = ((unsigned char *)hijack_displaybuf) + (pixel_row * (EMPEG_SCREEN_COLS / 2));
	pixel_pair  = &displayrow[pixel_col >> 1];
	pixel_mask  = (pixel_col & 1) ? 0xf0 : 0x0f;
	*pixel_pair = (*pixel_pair & ~pixel_mask) ^ (color & pixel_mask);
}

static int
draw_char (unsigned char *displayrow, int pixel_col, unsigned char c, unsigned char color, unsigned char inverse)
{
	unsigned int pixel_row, num_cols;
	const unsigned char *font_entry;

	if (c > '~' || c < ' ')
		c = ' ';
	font_entry = &kfont[c - ' '][0];
	if (c == ' ')
		num_cols = 3;
	else
		for (num_cols = KFONT_WIDTH; !font_entry[num_cols-2]; --num_cols);
	if (pixel_col + num_cols + 1 >= EMPEG_SCREEN_COLS) {
		//num_cols = EMPEG_SCREEN_COLS - pixel_col - 1;
		return -1;
	}
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

static unsigned int
draw_string (int text_row, unsigned int pixel_col, const unsigned char *s, int color)
{
	unsigned char inverse = 0;
	if (color < 0)
		color = inverse = -color;
	color &= 3;
	color |= color << 4;
	if (inverse)
		inverse = color;
top:
	if (text_row < EMPEG_TEXT_ROWS) {
		unsigned char *displayrow = &hijack_displaybuf[text_row * KFONT_HEIGHT][0];
		while (*s) {
			int rc;
			if (*s == '\n') {
				++s;
				++text_row;
				goto top;
			} else if (-1 == (rc = draw_char(displayrow, pixel_col, *s, color, inverse))) {
				++text_row;
				goto top;
			}
			pixel_col += rc;
			++s;
		}
	}
	return pixel_col;
}

static unsigned int
draw_number (int text_row, unsigned int pixel_col, unsigned int number, const char *format, int color)
{
	unsigned char buf[16];
	sprintf(buf, format, number);
	return draw_string(text_row, pixel_col, buf, color);
}

static void
activate_dispfunc (unsigned long userdata, int (*dispfunc)(int), void (*movefunc)(int))
{
	ir_selected = 0;
	hijack_userdata = userdata;
	hijack_dispfunc = dispfunc;
	hijack_movefunc = movefunc;
	hijack_status = HIJACK_PENDING;
	dispfunc(1);
}

static void
hijack_deactivate (void)
{
	hijack_movefunc = NULL;
	hijack_dispfunc = NULL;
	hijack_status = HIJACK_INACTIVE_PENDING;
}

#define MULT_POINT		12
#define MULT_MASK		((1 << MULT_POINT) - 1)
#define VOLADJ_THRESHSIZE	16
#define VOLADJ_HISTSIZE		128
#define VOLADJ_FIXEDPOINT(whole,fraction) ((((whole)<<MULT_POINT)|((unsigned int)((fraction)*(1<<MULT_POINT))&MULT_MASK)))

extern void voladj_next_preset(int);
extern unsigned int voladj_enabled, voladj_multiplier;
static const char *voladj_names[4] = {"Off", "Low", "Medium", "High"};
unsigned int voladj_histx = 0, voladj_history[VOLADJ_HISTSIZE] = {0,};

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
voladj_plot (int text_row, unsigned int pixel_col, unsigned int multiplier, int *prev)
{
	if (text_row < (EMPEG_TEXT_ROWS - 1)) {
		unsigned int height = VOLADJ_THRESHSIZE, pixel_row, threshold, color;
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
	voladj_enabled = (voladj_enabled + direction) & 3;
}

static int
voladj_display (int firsttime)
{
	unsigned int i, col, mult, prev = -1;
	unsigned char buf[32];
	unsigned long flags;

	if (firsttime) {
		memset(voladj_history, 0, sizeof(voladj_history));
		voladj_histx = 0;
	} else if (!hijack_last_moved && hijack_userdata == voladj_histx) {
		return NO_REFRESH;
	}
	hijack_last_moved = 0;
	hijack_userdata = voladj_histx;
	clear_hijack_displaybuf(COLOR0);
	save_flags_cli(flags);
	voladj_history[voladj_histx = (voladj_histx + 1) % VOLADJ_HISTSIZE] = voladj_multiplier;
	restore_flags(flags);
	col = draw_string(0,   0, hijack_on_dc_power ? "(DC)" : "(AC)", COLOR2);
	col = draw_string(0, col, " Volume Adjust:  ", COLOR2);
	(void)draw_string(0, col, voladj_names[voladj_enabled], COLOR3);
	mult = voladj_multiplier;
	sprintf(buf, "Current Multiplier: %2u.%02u", mult >> MULT_POINT, (mult & MULT_MASK) * 100 / (1 << MULT_POINT));
	(void)draw_string(3, 12, buf, COLOR2);
	save_flags_cli(flags);
	for (i = 1; i <= VOLADJ_HISTSIZE; ++i)
		(void)voladj_plot(1, i - 1, voladj_history[(voladj_histx + i) % VOLADJ_HISTSIZE], &prev);
	restore_flags(flags);
	return NEED_REFRESH;
}

static int
kfont_display (int firsttime)
{
	unsigned int col = 0, row = 0;
	unsigned char c;

	if (!firsttime)
		return NO_REFRESH;
	clear_hijack_displaybuf(COLOR0);
	col = draw_string(row, col, " ", -COLOR3);
	col = draw_string(row, col, " ", -COLOR2);
	col = draw_string(row, col, " ", -COLOR1);
	for (c = (unsigned char)' '; c <= (unsigned char)'~'; ++c) {
		unsigned char s[2] = {0,0};
		s[0] = c;
		col = draw_string(row, col, &s[0], COLOR2);
		if (col > (EMPEG_SCREEN_COLS - (KFONT_WIDTH - 1))) {
			col = 0;
			if (++row >= EMPEG_TEXT_ROWS)
				break;
		}
	}
	return NEED_REFRESH;
}

extern int empeg_readtherm(volatile unsigned int *timerbase, volatile unsigned int *gpiobase);
extern int empeg_inittherm(volatile unsigned int *timerbase, volatile unsigned int *gpiobase);
extern int get_loadavg(char * buffer);

static int
read_temperature (void)
{
	int temp;
	unsigned long flags;

	save_flags_clif(flags);
	temp = empeg_readtherm(&OSMR0,&GPLR);
	restore_flags(flags);
	/* Correct for negative temperatures (sign extend) */
	if (temp & 0x80)
		temp = -(128 - (temp ^ 0x80));
	return temp;

}

static int
vitals_display (int firsttime)
{
	unsigned int *permset=(unsigned int*)(EMPEG_FLASHBASE+0x2000);
	unsigned char buf[80];
	int temp, col;
	struct sysinfo i;

	if (!firsttime && jiffies_since(hijack_last_refresh) < (HZ*2))
		return NO_REFRESH;
	clear_hijack_displaybuf(COLOR0);
	sprintf(buf, "HwRev:%02d, Build:%x", permset[0], permset[3]);
	(void)draw_string(0, 0, buf, COLOR2);
	col = draw_string(1, 0, "Temperature: ", COLOR2);
	temp = read_temperature();
	sprintf(buf, "%dC/%dF", temp, temp * 180 / 100 + 32);
	(void)draw_string(1, col, buf, COLOR2);
	si_meminfo(&i);
	sprintf(buf, "Free: %lu/%lu", i.freeram, i.totalram);
	(void)draw_string(2, 0, buf, COLOR2);
	(void)get_loadavg(buf);
	temp = 0;
	for (col = 0;; ++col) {
		if (buf[col] == ' ' && ++temp == 3)
			break;
	}
	buf[col] = '\0';
	col = draw_string(3, 0, "LoadAvg: ", COLOR2);
	(void)draw_string(3, col, buf, COLOR2);
	return NEED_REFRESH;
}

static void
blanker_move (int direction)
{
	blanker_timeout = blanker_timeout + direction;
	if (blanker_timeout < 0)
		blanker_timeout = 0;
	else if (blanker_timeout > 0x3f)
		blanker_timeout = 0x3f;
}

static int
blanker_display (int firsttime)
{
	unsigned int col;

	if (!firsttime && !hijack_last_moved)
		return NO_REFRESH;
	hijack_last_moved = 0;
	clear_hijack_displaybuf(COLOR0);
	col = draw_string(0,   0, "Screen inactivity timeout:", COLOR2);
	col = draw_string(1,   0, "Blank: ", COLOR2);
	if (blanker_timeout) {
		col = draw_string(1, col, "after ", COLOR2);
		col = draw_number(1, col, blanker_timeout * SCREEN_BLANKER_MULTIPLIER, "%u", COLOR3);
		col = draw_string(1, col, " secs", COLOR2);
	} else {
		col = draw_string(1, col, "Off", COLOR3);
	}
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
			(void)draw_string(1, 20, " Enhancements.v25 ", -COLOR3);
			(void)draw_string(2, 33, "by Mark Lord", COLOR3);
			return NEED_REFRESH;
		}
		if (jiffies_since(game_ball_last_moved) < (HZ*3))
			return NO_REFRESH;
		ir_selected = 1; // return to menu
		return NEED_REFRESH;
	}
	if (jiffies_since(game_animtime) < (HZ/(ANIMATION_FPS-2))) {
		(void)draw_string(2, 44, "You Win", COLOR3);
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
		(void)draw_string(2, 44, "Game Over", COLOR3);
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
	game_col = jiffies % GAME_COLS;
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


static int
exit_display (int firsttime)
{
	hijack_deactivate();
	return NEED_REFRESH;
}

static void
menu_move (int direction)
{
	menu_item += direction;
	if (menu_item >= menu_size)
		menu_item = menu_size - 1;
	else if (menu_item < 0)
		menu_item = 0;
}

#define MENU_MAX_SIZE 15
static const char *menu_label [MENU_MAX_SIZE]       = {"Break-Out Game", "Auto Volume Adjust", "Screen Blanker", "Font Display", "Vital Signs", "[exit]", NULL,};
static int (*menu_displayfunc [MENU_MAX_SIZE])(int) = {game_display, voladj_display, blanker_display, kfont_display, vitals_display, exit_display, NULL,};
static void (*menu_movefunc   [MENU_MAX_SIZE])(int) = {game_move, voladj_move, blanker_move, NULL, NULL, NULL, NULL,};
static unsigned long menu_userdata[MENU_MAX_SIZE] = {0,};

static int
menu_display (int firsttime)
{
	unsigned int item = menu_item;

	if (firsttime || hijack_last_moved) {
		hijack_last_moved = 0;
		clear_hijack_displaybuf(COLOR0);
		if (menu_top > item)
			menu_top = item;
		while (item >= (menu_top + EMPEG_TEXT_ROWS))
			++menu_top;
		for (menu_size = 0; menu_label[menu_size] != NULL; ++menu_size) {
			if (menu_size >= menu_top && menu_size < (menu_top + EMPEG_TEXT_ROWS))
				(void)draw_string(menu_size - menu_top, 0, menu_label[menu_size], (menu_size == item) ? COLOR3 : COLOR2);
		}
		return NEED_REFRESH;
	}
	if (!firsttime) {
		unsigned long flags;
		save_flags_cli(flags);
		if (ir_selected)
			activate_dispfunc(menu_userdata[item], menu_displayfunc[item], menu_movefunc[item]);
		else if (jiffies_since(hijack_last_refresh) > (5 * HZ))
			hijack_deactivate(); // menu timed-out
		restore_flags(flags);
	}
	return NO_REFRESH;
}

// This routine covertly intercepts all display updates,
// giving us a chance to substitute our own display.
//
void hijack_display(struct display_dev *dev, unsigned char *player_buf)
{
	unsigned char *buf = (unsigned char *)hijack_displaybuf;
	unsigned long flags;
	int refresh = NEED_REFRESH, color;
	unsigned int secs_since_lastpoll;
	static int temp = 0;
	static unsigned long temp_lastpoll = 0;

	save_flags_cli(flags);
	switch (hijack_status) {
		case HIJACK_INACTIVE:
			if (ir_trigger_count >= 3 || (ir_knob_down && jiffies_since(ir_knob_down) >= HZ)) {
				ir_trigger_count = 0;
				menu_item = menu_top = 0;
				activate_dispfunc(0, menu_display, menu_move);
			} else if (!maxtemp_threshold || (secs_since_lastpoll = jiffies_since(temp_lastpoll) / HZ) < 3) {
				buf = player_buf;
			} else if ((secs_since_lastpoll & 1) && (temp = read_temperature()) < (maxtemp_threshold + MAXTEMP_OFFSET)) {
				temp_lastpoll = jiffies;
				buf = player_buf;
			} else if (temp < (maxtemp_threshold + MAXTEMP_OFFSET)) {
				buf = player_buf;
			} else {
				unsigned char msg[16];
				color = (secs_since_lastpoll & 1) ? COLOR3 : -COLOR3;
				clear_hijack_displaybuf(color);
				sprintf(msg, " Too Hot: %dC ", temp);
				(void)draw_string(2, 35, msg, -color);
				buf = (unsigned char *)hijack_displaybuf;
				refresh = NEED_REFRESH;
			}
			break;
		case HIJACK_ACTIVE:
			if (hijack_dispfunc == NULL) {  // userland app finished?
				ir_trigger_count = 0;
				activate_dispfunc(0, menu_display, menu_move);
			} else {
				restore_flags(flags);
				refresh = hijack_dispfunc(0);
				save_flags_cli(flags);
				if (ir_selected)
					activate_dispfunc(0, menu_display, menu_move);
			}
			break;
		case HIJACK_PENDING:
			if (!ir_releasewait) {
				ir_selected = 0;
				hijack_status = HIJACK_ACTIVE;
			}
			break;
		case HIJACK_INACTIVE_PENDING:
			if (!ir_releasewait)
				hijack_status = HIJACK_INACTIVE;
			break;
		default: // (good) paranoia
			hijack_deactivate();
			break;
	}
	restore_flags(flags);

	// Prevent screen burn-in on an inactive/unattended player:
	if (blanker_timeout) {
		static unsigned long blanker_lastpoll = 0;
		static unsigned char blanked = 0, last_buf[EMPEG_SCREEN_BYTES] = {0,};
		if (!blanker_activated)
			blanked = 0;
		if (jiffies_since(blanker_lastpoll) >= HZ) {
			blanker_lastpoll = jiffies;
			if (memcmp(last_buf, buf, EMPEG_SCREEN_BYTES)) {
				memcpy(last_buf, buf, EMPEG_SCREEN_BYTES);
				blanker_activated = 0;
			} else {
				refresh = NO_REFRESH;
				if (!blanker_activated)
					blanker_activated = jiffies ? jiffies : 1;
			}
		}
		if (blanker_activated && jiffies_since(blanker_activated) > (blanker_timeout * (SCREEN_BLANKER_MULTIPLIER * HZ))) {
			if (!blanked) {
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

static int
hijack_move (int direction)
{
	if (hijack_status != HIJACK_INACTIVE) {
		if (hijack_status == HIJACK_ACTIVE && hijack_movefunc != NULL)
			hijack_movefunc(direction);
		hijack_last_moved = jiffies ? jiffies : 1;
		return 1; // input WAS hijacked
	}
	return 0; // input NOT hijacked
}

static int
count_keypresses (unsigned long data)
{
	static unsigned long prev_presstime = 0;
	int rc = 0;
	if (hijack_status == HIJACK_INACTIVE) {
		// ugly Kenwood remote hack: press/release CD 3 times in a row to activate menu
		if (ir_prev_pressed == (data & 0x7fffffff) && prev_presstime && jiffies_since(prev_presstime) < HZ)
			++ir_trigger_count;
		else
			ir_trigger_count = 1;
		prev_presstime = jiffies;
	} else {
		ir_selected = 1;
		rc = 1; // input WAS hijacked
	}
	return rc;
}

static int userland_display_updated = 0;

static int
userland_display (int firsttime)
{
	// this is invoked when a userland menu item is active
	if (firsttime || *((struct wait_queue **)hijack_userdata) != NULL) {
		clear_hijack_displaybuf(COLOR0);
		wake_up((struct wait_queue **)hijack_userdata);
		userland_display_updated = 1;
	}
	if (userland_display_updated) {
		userland_display_updated = 0;
		return NEED_REFRESH;
	}
	ir_selected = 0;  // prevent accidental exit:  FIXME: Maybe remove this line? (userland can rebind buttons)
	return NO_REFRESH;
}

// This routine covertly intercepts all button presses/releases,
// giving us a chance to ignore them or to trigger our own responses.
//
int
hijacked_input (unsigned long data)
{
	int i, rc = 0; // input NOT hijacked
	unsigned long flags;

	save_flags_cli(flags);
	blanker_activated = 0;
	if (hijack_buttonlist && hijack_status == HIJACK_ACTIVE && hijack_dispfunc == userland_display) {
		for (i = hijack_buttonlist[0]; --i > 0;) {
			if (data == hijack_buttonlist[i]) {
				unsigned int head = (hijack_dataq_head + 1) % HIJACK_DATAQ_SIZE;
				if (head != hijack_dataq_tail)  // enqueue it, if there's still room
					hijack_dataq[hijack_dataq_head = head] = data;
				rc = 1; // input WAS hijacked
				goto done;
			}
		}
	}
	switch (data) {
		case IR_TOP_BUTTON_PRESSED:
			if (hijack_status != HIJACK_INACTIVE) {
				hijack_deactivate();
				rc = 1; // input WAS hijacked
			}
			break;
		case IR_RIO_SELECTMODE_PRESSED:
			ir_knob_down = jiffies ? jiffies : 1;
			// fall thru
		case IR_KW_CD_PRESSED:
			rc = count_keypresses(data);
			break;
		case IR_KNOB_PRESSED:
			ir_knob_down = jiffies ? jiffies : 1;
			if (hijack_status != HIJACK_INACTIVE) {
				ir_selected = 1;
				rc = 1; // input WAS hijacked
			}
			break;
		case IR_RIO_SELECTMODE_RELEASED:
		case IR_KNOB_RELEASED:  // these often arrive in pairs
			ir_knob_down = 0;
			if (hijack_status != HIJACK_INACTIVE)
				rc = 1; // input WAS hijacked
			break;
		case IR_KW_PREVTRACK_PRESSED:
		case IR_RIO_PREVTRACK_PRESSED:
			ir_left_down = jiffies ? jiffies : 1;
			// fall thru
		case IR_KNOB_LEFT:
			rc = hijack_move(-1);
			break;
		case IR_KW_PREVTRACK_RELEASED:
		case IR_RIO_PREVTRACK_RELEASED:
			ir_left_down = 0;
			if (hijack_status != HIJACK_INACTIVE)
				rc = 1; // input WAS hijacked
			break;
		case IR_KW_NEXTTRACK_PRESSED:
		case IR_RIO_NEXTTRACK_PRESSED:
			ir_right_down = jiffies ? jiffies : 1;
			// fall thru
		case IR_KNOB_RIGHT:
			rc = hijack_move(1);
			break;
		case IR_KW_NEXTTRACK_RELEASED:
		case IR_RIO_NEXTTRACK_RELEASED:
			ir_right_down = 0;
			if (hijack_status != HIJACK_INACTIVE)
				rc = 1; // input WAS hijacked
			break;
		case IR_KW_PAUSE_PRESSED:
		case IR_RIO_PAUSE_PRESSED:
			if (hijack_status != HIJACK_INACTIVE)
				game_paused = !game_paused;
			break;
	}
	if (ir_releasewait && data == ir_releasewait) {
		ir_releasewait = 0;
		rc = 1;
	} else if (rc && (data & 0x80000001) == 0 && data != IR_KNOB_LEFT && data != IR_KNOB_RIGHT) {
		ir_releasewait = data | ((((int)data) > 16) ? 0x80000000 : 0x00000001);
	}
done:
	// save button PRESSED codes in ir_prev_pressed
	if ((data & 0x80000001) == 0 || ((int)data) > 16)
		ir_prev_pressed = data;
	if (hijack_dataq_tail != hijack_dataq_head) {
		if (*((struct wait_queue **)hijack_userdata) != NULL) {
			wake_up((struct wait_queue **)hijack_userdata);
		}
	}
	restore_flags(flags);
	return rc;
}

static int
extend_menu (const char *label, unsigned long userdata, int (*displayfunc)(int), void (*movefunc)(int))
{
	int i;
	for (i = 0; i < (MENU_MAX_SIZE-1); ++i) {
		if (menu_label[i] == NULL) {
			// Insert new entry before last item [exit]
			menu_label       [i] = menu_label[i-1];
			menu_displayfunc [i] = menu_displayfunc[i-1];
			menu_movefunc    [i] = menu_movefunc[i-1];
			menu_userdata    [i] = menu_userdata[i-1];
			--i;
			menu_label       [i] = label;
			menu_displayfunc [i] = displayfunc;
			menu_movefunc    [i] = movefunc;
			menu_userdata    [i] = userdata;
			return 0; // success
		}
	}
	return 1; // no room; menu is full
}

static int
userland_extend_menu (const char *label, unsigned long userdata)
{
	int i, rc = -ENOMEM;
	unsigned long flags;

	save_flags_cli(flags);
	for (i = 0; i < (MENU_MAX_SIZE-1); ++i) {
		if (menu_label[i] == NULL) {
			int size = 1 + strlen(label);
			unsigned char *buf = kmalloc(size, GFP_KERNEL);
			if (buf == NULL) {
				rc = -ENOMEM;
				break;
			}
			memcpy(buf, label, size);
			rc = extend_menu(buf, userdata, userland_display, NULL);
			break;
		} else if (0 == strcmp(menu_label[i], label)) {
			menu_userdata[i] = userdata; // already there, so just update userdata
			if (menu_item == i && hijack_dispfunc == userland_display)
				hijack_dispfunc = NULL; // return to menu
			rc = 0;
			break;
		}
	}
	restore_flags(flags);
	return rc;
}

static void
maxtemp_move (int direction)
{
	maxtemp_threshold = maxtemp_threshold + direction;
	if (maxtemp_threshold < 0)
		maxtemp_threshold = 0;
	else if (maxtemp_threshold > ((1<<MAXTEMP_BITS)-1))
		maxtemp_threshold  = ((1<<MAXTEMP_BITS)-1);
}

static int
maxtemp_display (int firsttime)
{
	unsigned int col;

	if (!firsttime && !hijack_last_moved)
		return NO_REFRESH;
	hijack_last_moved = 0;
	clear_hijack_displaybuf(COLOR0);
	col = draw_string(0,   0, "Max Temperature Warning", COLOR2);
	col = draw_string(1,   0, "Threshold: ", COLOR2);
	if (maxtemp_threshold) {
		col = draw_number(1, col, maxtemp_threshold + MAXTEMP_OFFSET, "%2u", COLOR3);
		col = draw_string(1, col, "C", COLOR3);
	} else {
		col = draw_string(1, col, "Off", COLOR3);
	}
	return NEED_REFRESH;
}

// format of eight-byte flash memory savearea used for our hijack'd settings
static struct sa_struct {
	unsigned voladj_ac_power	: 2;
	unsigned blanker_timeout	: 6;
	unsigned voladj_dc_power	: 2;
	unsigned maxtemp_threshold	: MAXTEMP_BITS;
	unsigned byte2			: 8;
	unsigned byte3			: 8;
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
}

static int  // invoked from empeg_display.c::ioctl()
hijack_ioctl (struct inode *inode, struct file *filp, unsigned int cmd, unsigned long arg)
{
	unsigned long flags;
	struct wait_queue *wq = NULL;
	int rc;

	//struct display_dev *dev = (struct display_dev *)filp->private_data;
	switch (cmd) {
		case EMPEG_HIJACK_WAITMENU:	// create menu item and wait for it to be selected
		{
			// Invocation:  rc = ioctl(fd, EMPEG_HIJACK_WAITMENU, (char *)"Menu Label");
			int size = 0;
			unsigned char buf[32] = {0,}, *label = (unsigned char *)arg;
			do {
				if (copy_from_user(&buf[size], label+size, 1)) return -EFAULT;
			} while (buf[size++] && size < sizeof(buf));
			buf[size-1] = '\0';
			rc = userland_extend_menu(buf, (unsigned long)&wq);
			if (!rc)
				return -ENOMEM;
			sleep_on(&wq);	// wait indefinitely for this item to be selected
			return 0;
		}
		case EMPEG_HIJACK_DISPWRITE:	// copy buffer to screen
		{
			// Invocation:  rc = ioctl(fd, EMPEG_HIJACK_DISPWRITE, (unsigned char *)displaybuf); // sizeof(displaybuf)==2048
			if (copy_from_user(hijack_displaybuf, (void *)arg, sizeof(hijack_displaybuf))) return -EFAULT;
			userland_display_updated = 1;
			return 0;
		}
		case EMPEG_HIJACK_BINDBUTTONS:	// Specify IR codes to be hijacked
		{
			// Invocation:  rc = ioctl(fd, EMPEG_HIJACK_DISPWRITE, (unsigned long *)data[]); // data[0] specifies TOTAL number of table entries data[0..?]
			unsigned long size, nbuttons = 0, *buttonlist = NULL;
			if (copy_from_user(&nbuttons, (void *)arg, sizeof(nbuttons))) return -EFAULT;
			if (nbuttons == 0 || nbuttons > 256) return -EINVAL;	// 256 is an arbitrary limit for safety
			size = nbuttons * sizeof(nbuttons);
			if (!(buttonlist = kmalloc(size, GFP_KERNEL))) return -ENOMEM;
			if (copy_from_user(buttonlist, (void *)arg, size)) {
				kfree(buttonlist);
				return -EFAULT;
			}
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
			unsigned long button;
			// Invocation:  rc = ioctl(fd, EMPEG_HIJACK_DISPWRITE, (unsigned long *)&data); // IR code is written to "data" on return
			save_flags_cli(flags);
			while (hijack_dataq_tail == hijack_dataq_head) {
				hijack_userdata = (unsigned long)&wq;
				restore_flags(flags);
				sleep_on(&wq);
				save_flags_cli(flags);
			}
			hijack_dataq_tail = (hijack_dataq_tail + 1) % HIJACK_DATAQ_SIZE;
			button = hijack_dataq[hijack_dataq_tail];
			restore_flags(flags);
			return put_user(button, (int *)arg);

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
			unsigned char buf[256] = {0,}, *text = (unsigned char *)arg;
			do {
				if (copy_from_user(&buf[size], text+size, 1)) return -EFAULT;
			} while (buf[size++] && size < sizeof(buf));
			buf[size-1] = '\0';
			save_flags_cli(flags);
			(void)draw_string(0,0,buf,COLOR3);
			restore_flags(flags);
			userland_display_updated = 1;
			return 0;
		}
		case EMPEG_HIJACK_DISPGEOM:	// Set screen overlay geometry
			return -ENOSYS;		// not implemented

		default:			// Everything else
			return -EINVAL;
	}
}

// initial setup of hijack menu system
void
hijack_init (void)
{
	extern int getbitset(void);
	extend_menu("MaxTemp Warning", 0, maxtemp_display, maxtemp_move);
	hijack_on_dc_power = getbitset() & EMPEG_POWER_FLAG_DC;
}
