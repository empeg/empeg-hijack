// Empeg display/IR hijacking by Mark Lord <mlord@pobox.com>
//
// Includes: font, drawing routines
//           extensible menu system
//           VolAdj settings and display
//           BreakOut game
//           Screen blanker (to prevent burn-out)
//
// Near-future plans: Implement ioctls() for userland apps to bind into the menus,
// etc.. to be able to use the hijack capability to run while player is running.
// We hijack the screen, and MOST buttons, leaving just a few buttons under
// control of the player itself (most likely just the four on the front panel).
//
// Basic userland interface would be like this:
// 	vdev = open("/dev/hijack")	// access the hijack virtual device
// 	mmap(vdev,displaybuf,2048)	// get ptr to kernel's displaybuf
// 	fork();				// create an extra thread
// 	ioctl(vdev, HIJACK_WAIT_MENU, "GPS") // thread1: create a menu entry,
// 					     // and wait for it to be selected
// top:
// 	ioctl(vdev, HIJACK_WAIT_MENU, "TETRIS") // thread2: another menu entry
// 	...
// 	## user selected "TETRIS" menu item
// 	## kernel unblocks thread2 only.
// 	## thread2 now has *exclusive* control of screen and (most) buttons
// 	...
// 	while (playing_tetris) {	// thread2
// 	  ioctl(vdev, HIJACK_IR_POLL)	// thread2: poll for input from IR buttons
// 	  write stuff to displaybuf	// thread2
// 	  ioctl(vdev, HIJACK_UPDATE_DISPLAY) // thread2: force another screen update
// 	}
// 	...
// 	ioctl(vdev, HIJACK_UPDATE_DISPLAY) // thread2: force another screen update
// 	goto top;			// thread2
// 	...
// 	ioctl(vdev, HIJACK_REMOVE_MENU, "TETRIS") // optional
// 	close("/dev/hijack")			// optional
//
// Well, that's the idea, anyway.
// So if you want to port/develop an application for this,
// then at least you've now got the basic structure to build around.
// I hope to implement this (smaller than it looks) by early Nov/2001.
// --
// Mark Lord <mlord@pobox.com>
//-----------------------------------------------------------------------------
//
// >With your screen code, are you planning on any overlay capabilities 
// >like the clock hack in 1.03, or is it full screen only? 
// >Being able to say "Box with the text "Blah" at coord "2,34" 
// >for 2 seconds would be awesome. 
// 
// How about a simple ioctl() (from userland) for "HIJACK_SET_GEOM" to specify a "window" geometry (upper left
// coordinate, height, and width)? 
// 
// This would be somewhat messy to implement internally, but doable. 
// 
// Method (1) would be to modify the blatter routine to only blat the rectangle we specify. But given the extreme
// weirdness in the hardware displaybuf layout, this likely ain't gonna happen. 
// 
// Method (2) might be simpler: merge the player's displaybuf with our rectangle before blatting. A lessor known
// "secret" of the hijack method is that the player continues to update the "screen" even while we are running --
// except its updates never make it to the blatter. 
// 
// The second Method looks a lot less efficient than the first, but might not be too bad because its simpler logic
// makes for faster code than Method 1.. mmm.. 
//
//-----------------------------------------------------------------------------
//
// RE: kernels/42345-empeg_display.diff (kernel patch file for clock overlay on player)
//
// There's a few things in there we might use, such as the separate display queue for our hijacked overlays for
// menus/userland: what we do right now is simpler, but only works while the player is running (the code doesn't currently
// do refreshes on its own. ToBeFixed). 
// 
// And since the clockoverlay used the same device as the player, that answers our question about whether or not to
// create a brand new device: not needed since there isn't any apparent conflict with the player (it might have been using
// open(O_EXCL) but I guess it doesn't). 
// 
// So we can just stick in some ioctls() like the clockoveray did and voila, we're there! 
// 
//-----------------------------------------------------------------------------

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

#define SCREEN_BLANKER_MULTIPLIER 30
int blanker_timeout = 0;	// saved/restored in empeg_state.c

unsigned long jiffies_since(unsigned long past_jiffies);

#define EMPEG_SCREEN_ROWS	32		// pixels
#define EMPEG_SCREEN_COLS	128		// pixels
#define EMPEG_SCREEN_BYTES	(EMPEG_SCREEN_ROWS * EMPEG_SCREEN_COLS / 2)
#define EMPEG_TEXT_ROWS		(EMPEG_SCREEN_ROWS / KFONT_HEIGHT)
#define KFONT_HEIGHT		8		// font height is 8 pixels
#define KFONT_WIDTH		6		// font width 5 pixels or less, plus 1 for spacing

static unsigned char hijacked_displaybuf[EMPEG_SCREEN_ROWS][EMPEG_SCREEN_COLS/2];

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
clear_hijacked_displaybuf (int color)
{
	color &= 3;
	color |= color << 4;
	memset(hijacked_displaybuf,color,EMPEG_SCREEN_BYTES);
}

static void
draw_pixel (int pixel_row, unsigned int pixel_col, int color)
{
	unsigned char pixel_mask, *displayrow, *pixel_pair;
	if (color < 0)
		color = -color;
	color &= 3;
	color |= color << 4;
	displayrow  = ((unsigned char *)hijacked_displaybuf) + (pixel_row * (EMPEG_SCREEN_COLS / 2));
	pixel_pair  = &displayrow[pixel_col >> 1];
	pixel_mask  = (pixel_col & 1) ? 0xf0 : 0x0f;
	*pixel_pair = (*pixel_pair & ~pixel_mask) ^ (color & pixel_mask);
}

static unsigned int
draw_char (unsigned char *displayrow, unsigned int pixel_col, unsigned char c, unsigned char color, unsigned char inverse)
{
	unsigned int pixel_row, num_cols;
	const unsigned char *font_entry;

	if (pixel_col >= EMPEG_SCREEN_COLS)
		return 0;
	if (c > '~' || c < ' ')
		c = ' ';
	font_entry = &kfont[c - ' '][0];
	if (c == ' ')
		num_cols = 3;
	else
		for (num_cols = KFONT_WIDTH; !font_entry[num_cols-2]; --num_cols);
	if (pixel_col + num_cols + 1 >= EMPEG_SCREEN_COLS)
		num_cols = EMPEG_SCREEN_COLS - pixel_col - 1;
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
	if (text_row < EMPEG_TEXT_ROWS) {
		unsigned char *displayrow = ((unsigned char *)hijacked_displaybuf) + (text_row * (KFONT_HEIGHT * EMPEG_SCREEN_COLS / 2));
		unsigned char inverse = 0;
		if (color < 0)
			color = inverse = -color;
		color &= 3;
		color |= color << 4;
		if (inverse)
			inverse = color;
		while (*s) {
			pixel_col += draw_char(displayrow, pixel_col, *s++, color, inverse);
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
	clear_hijacked_displaybuf(COLOR0);
	save_flags_cli(flags);
	voladj_history[voladj_histx = (voladj_histx + 1) % VOLADJ_HISTSIZE] = voladj_multiplier;
	restore_flags(flags);
	col = draw_string(0, 0, "Volume Auto Adjust:  ", COLOR2);
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
	clear_hijacked_displaybuf(COLOR0);
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
vitals_display (int firsttime)
{
	unsigned int *permset=(unsigned int*)(EMPEG_FLASHBASE+0x2000);
	unsigned char buf[80];
	int temp, col;
	struct sysinfo i;
	unsigned long flags;

	if (!firsttime && jiffies_since(hijack_last_refresh) < (HZ*2))
		return NO_REFRESH;
	clear_hijacked_displaybuf(COLOR0);
	sprintf(buf, "HwRev:%02d, Build:%x", permset[0], permset[3]);
	(void)draw_string(0, 0, buf, COLOR2);
	col = draw_string(1, 0, "Temperature: ", COLOR2);
	if (firsttime) {
		save_flags_clif(flags);
		empeg_inittherm(&OSMR0,&GPLR);
		restore_flags(flags);
		(void)draw_string(1, col, "(reading)", COLOR2);
	} else {
		save_flags_clif(flags);
		temp = empeg_readtherm(&OSMR0,&GPLR);
		restore_flags(flags);
		/* Correct for negative temperatures (sign extend) */
		if (temp & 0x80)
			temp = -(128 - (temp ^ 0x80));
		sprintf(buf, "%dC/%dF", temp, temp * 180 / 100 + 32);
		(void)draw_string(1, col, buf, COLOR2);
	}
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
	clear_hijacked_displaybuf(COLOR0);
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
			(void)draw_string(1, 20, " Enhancements.v24 ", -COLOR3);
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
		ir_selected = 1;
		return NEED_REFRESH;
	} else if (!game_animptr[framenr]) { // last frame?
		frameadj = -1;  // play it again, backwards
		framenr += frameadj;
	}
	s = (unsigned char *)game_animptr + game_animptr[framenr];
	d = (unsigned char *)hijacked_displaybuf;
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
	unsigned char *paddlerow = hijacked_displaybuf[EMPEG_SCREEN_ROWS-3];
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
	if (col > 0 && col < (GAME_COLS-1) && hijacked_displaybuf[row][col] == GAME_BRICKS) {
		hijacked_displaybuf[row][col] = 0;
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
	hijacked_displaybuf[game_row][game_col] = 0;
	game_row += game_vdir;
	game_col += game_hdir;
	if (hijacked_displaybuf[game_row][game_col] == GAME_HBOUNCE) {
		// need to bounce horizontally
		game_hdir = 0 - game_hdir;
		game_col += game_hdir;
	}
	if (game_row == GAME_BRICKS_ROW) {
		game_nuke_brick(game_row,game_col-1);
		game_nuke_brick(game_row,game_col);
		game_nuke_brick(game_row,game_col+1);
	}
	if (hijacked_displaybuf[game_row][game_col] == GAME_VBOUNCE) {
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
	if (hijacked_displaybuf[game_row][game_col] == GAME_OVER) {
		(void)draw_string(2, 44, "Game Over", COLOR3);
		game_over = 1;
	}
	hijacked_displaybuf[game_row][game_col] = GAME_BALL;
	restore_flags(flags);
	return NEED_REFRESH;
}

static int
game_display (int firsttime)
{
	int i;

	if (!firsttime)
		return game_over ? game_finale() : game_move_ball();
	clear_hijacked_displaybuf(COLOR0);
	game_paddle_col = GAME_COLS / 2;
	for (i = 0; i < GAME_COLS; ++i) {
		hijacked_displaybuf[0][i] = GAME_VBOUNCE;
		hijacked_displaybuf[EMPEG_SCREEN_ROWS-1][i] = GAME_OVER;
		hijacked_displaybuf[GAME_BRICKS_ROW][i] = GAME_BRICKS;
	}
	for (i = 0; i < EMPEG_SCREEN_ROWS; ++i)
		hijacked_displaybuf[i][0] = hijacked_displaybuf[i][GAME_COLS-1] = GAME_HBOUNCE;
	memset(&hijacked_displaybuf[EMPEG_SCREEN_ROWS-3][game_paddle_col],GAME_VBOUNCE,GAME_PADDLE_SIZE);
	game_hdir = 1;
	game_vdir = 1;
	game_row = 6;
	game_col = jiffies % GAME_COLS;
	if (hijacked_displaybuf[game_row][game_col])
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
static const char *menu_label [MENU_MAX_SIZE]       = {"Break-Out Game", "Volume Auto Adjust", "Screen Blanker", "Font Display", "Vital Signs", "[exit]", NULL,};
static int (*menu_displayfunc [MENU_MAX_SIZE])(int) = {game_display, voladj_display, blanker_display, kfont_display, vitals_display, exit_display, NULL,};
static void (*menu_movefunc   [MENU_MAX_SIZE])(int) = {game_move, voladj_move, blanker_move, NULL, NULL, NULL, NULL,};
static unsigned long menu_userdata[MENU_MAX_SIZE] = {0,};

static int
menu_display (int firsttime)
{
	unsigned int item = menu_item;

	if (firsttime || hijack_last_moved) {
		hijack_last_moved = 0;
		clear_hijacked_displaybuf(COLOR0);
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
	unsigned char *buf = (unsigned char *)hijacked_displaybuf;
	unsigned long flags;
	int refresh = NEED_REFRESH;

	save_flags_cli(flags);
	switch (hijack_status) {
		case HIJACK_INACTIVE:
			if (ir_trigger_count >= 3 || (ir_knob_down && jiffies_since(ir_knob_down) >= HZ)) {
				ir_trigger_count = 0;
				menu_item = menu_top = 0;
				activate_dispfunc(0, menu_display, menu_move);
			} else {
				buf = player_buf;
			}
			break;
		case HIJACK_ACTIVE:
			if (hijack_dispfunc == NULL) {
				hijack_deactivate();
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
		if (ir_prev_pressed == (data & 0x7fffffff) && prev_presstime && jiffies_since(prev_presstime) < (HZ + (HZ/2)))
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

// This routine covertly intercepts all button presses/releases,
// giving us a chance to ignore them or to trigger our own responses.
//
#include "ir_codes.h"
int
hijacked_input (unsigned long data)
{
	int rc = 0; // input NOT hijacked
	unsigned long flags;

	save_flags_cli(flags);
	blanker_activated = 0;
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
	// save button PRESSED codes in ir_prev_pressed
	if ((data & 0x80000001) == 0 || ((int)data) > 16)
		ir_prev_pressed = data;
	restore_flags(flags);
	return rc;
}

#if 0
// This stuff is still under construction.
//
// The idea here is that userland apps do ioctl() to bind into the menu,
// the ioctl() calls menu_extend(), and then goes to sleep.
// The userland_move() and userland_display() procs use "userdata" to
// reference back to the sleeping process and awaken it on suitable events.
//
// I suspect the userland_move() may be useless, but it's there for now.
//


static void
userland_move (int direction)
{
	// FIXME: update user's data structure and then wake-up the sleeping thread
}

static int
userland_display (int firsttime)
{
	// FIXME: copy user's data to hijacked_displaybuf[]
	return NEED_REFRESH;
}

static int
userland_extend_menu (const char *label, unsigned long userdata)
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
			menu_displayfunc [i] = userland_display;
			menu_movefunc    [i] = userland_move;
			menu_userdata    [i] = userdata;
			return 0; // success
		}
	}
	return 1; // no room; menu is full
}
#endif /* 0 */

typedef struct sa_struct {
	unsigned voladj_enabled	: 2;
	unsigned blanker_timeout: 6;
	unsigned byte1		: 8;
	unsigned byte2		: 8;
	unsigned byte3		: 8;
	unsigned byte4		: 8;
	unsigned byte5		: 8;
	unsigned byte6		: 8;
	unsigned byte7		: 8;
} hijack_savearea_t;

void hijack_save_settings (unsigned char *buf)
{
	// save state
	hijack_savearea_t sa;
	memset(&sa,0,sizeof(sa));
	sa.voladj_enabled	= voladj_enabled;
	sa.blanker_timeout	= blanker_timeout;
	memcpy(buf, &sa, sizeof(sa));
}

void hijack_restore_settings (const unsigned char *buf)
{
	// restore state
	hijack_savearea_t sa;
	memcpy(&sa, buf, sizeof(sa));
	voladj_enabled		= sa.voladj_enabled;
	blanker_timeout		= sa.blanker_timeout;
}

