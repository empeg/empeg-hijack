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
// Cool. I'm planning on it NOW! 
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
// Cool. 
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
//

#define HIJACK_INACTIVE		0
#define HIJACK_INACTIVE_PENDING	1
#define HIJACK_SUBFUNC_PENDING	2
#define HIJACK_SUBFUNC_ACTIVE	3
static int hijack_status = HIJACK_INACTIVE;

#define EMPEG_SCREEN_ROWS	32		// pixels
#define EMPEG_SCREEN_COLS	128		// pixels
#define EMPEG_SCREEN_BYTES	(EMPEG_SCREEN_ROWS * EMPEG_SCREEN_COLS / 2)
#define EMPEG_TEXT_ROWS		(EMPEG_SCREEN_ROWS / KFONT_HEIGHT)
#define KFONT_HEIGHT		8		// font height is 8 pixels
#define KFONT_WIDTH		6		// font width 5 pixels or less, plus 1 for spacing

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
	{0x27,0x49,0x49,0x49,0x31,0x00}, // 5
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

#define COLOR0 0 // blank pixels
#define COLOR1 1 // prefix with '-' for inverse video
#define COLOR2 2 // prefix with '-' for inverse video
#define COLOR3 3 // prefix with '-' for inverse video

static unsigned char hijacked_displaybuf[EMPEG_SCREEN_ROWS][EMPEG_SCREEN_COLS/2];
static unsigned int ir_selected = 0, ir_releasewait = 0, ir_knob_down = 0, ir_left_down = 0, ir_right_down = 0, ir_trigger_count = 0;
static unsigned long screen_saver = 0;
int screen_blanker_timeout = 0;
#define SCREEN_BLANKER_MULTIPLIER 30

static void clear_hijacked_displaybuf (int color)
{
	color &= 3;
	color |= color << 4;
	memset(hijacked_displaybuf,color,EMPEG_SCREEN_BYTES);
}

static void draw_pixel (int pixel_row, unsigned int pixel_col, int color)
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

static int  menu_item = 0, menu_size = 0, menu_top = 0;
static void (*hijack_subfunc)(unsigned long, int);
static void (*hijack_movefunc)(unsigned long, int) = NULL;
static unsigned long menu_lastpress = 0, hijack_userdata = 0;

static void activate_subfunc (unsigned long userdata, void (*subfunc)(unsigned long, int), void (*movefunc)(unsigned long, int))
{
	ir_selected = 0;
	hijack_userdata = userdata;
	hijack_subfunc = subfunc;
	hijack_movefunc = movefunc;
	hijack_status = HIJACK_SUBFUNC_PENDING;
	subfunc(userdata, 1);
}

static void hijack_deactivate (void)
{
	hijack_movefunc = NULL;
	hijack_subfunc = NULL;
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
static unsigned int voladj_plot (int text_row, unsigned int pixel_col, unsigned int multiplier, int *prev)
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

static void voladj_move (unsigned long ignored, int direction)
{
	voladj_enabled = (voladj_enabled + direction) & 3;
}

static void voladj_refresh (unsigned long ignored, int firsttime)
{
	unsigned int i, col, prev = -1;
	unsigned char buf[32];

	if (firsttime) {
		memset(voladj_history, 0, sizeof(voladj_history));
		voladj_histx = 0;
	}
	voladj_history[voladj_histx = (voladj_histx + 1) % VOLADJ_HISTSIZE] = voladj_multiplier;
	clear_hijacked_displaybuf(COLOR0);
	col = draw_string(0, 0, "Volume Auto Adjust:  ", COLOR2);
	(void)draw_string(0, col, voladj_names[voladj_enabled], COLOR3);
	sprintf(buf, "Current Multiplier: %2u.%02u",
		voladj_multiplier >> MULT_POINT, (voladj_multiplier & MULT_MASK) * 100 / (1 << MULT_POINT));
	(void)draw_string(3, 12, buf, COLOR2);
	for (i = 1; i <= VOLADJ_HISTSIZE; ++i)
		(void)voladj_plot(1, i - 1, voladj_history[(voladj_histx + i) % VOLADJ_HISTSIZE], &prev);
}

static void kfont_refresh (unsigned long ignored, int firsttime)
{
	if (firsttime) {
		unsigned int col = 0, row = 0;
		unsigned char c;
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
	}
}

static void blanker_move (unsigned long ignored, int direction)
{
	screen_blanker_timeout = screen_blanker_timeout + direction;
	if (screen_blanker_timeout < 0)
		screen_blanker_timeout = 0;
	else if (screen_blanker_timeout > 0x3f)
		screen_blanker_timeout = 0x3f;
}

static void blanker_refresh (unsigned long ignored, int firsttime)
{
	unsigned int col;
	clear_hijacked_displaybuf(COLOR0);
	col = draw_string(0,   0, "Screen inactivity timeout:", COLOR2);
	col = draw_string(1,   0, "Blank: ", COLOR2);
	if (screen_blanker_timeout) {
		col = draw_string(1, col, "after ", COLOR2);
		col = draw_number(1, col, screen_blanker_timeout * SCREEN_BLANKER_MULTIPLIER, "%u", COLOR3);
		col = draw_string(1, col, " secs", COLOR2);
	} else {
		col = draw_string(1, col, "Off", COLOR3);
	}
	if (empeg_hardwarerevision() < 6)
		col = draw_string(3, 0, "(lost over power cycles)", COLOR2);
}

#define GAME_COLS (EMPEG_SCREEN_COLS/2)
#define GAME_VBOUNCE 0xff
#define GAME_BRICKS 0xee
#define GAME_BRICKS_ROW 5
#define GAME_HBOUNCE 0x77
#define GAME_BALL 0xff
#define GAME_OVER 0x11
#define GAME_PADDLE_SIZE 8

static short game_over, game_row, game_col, game_hdir, game_vdir, game_paddle_col, game_paddle_lastdir, game_speed, game_bricks;
static unsigned long game_starttime, game_ball_lastmove, game_paddle_lastmove, game_animbase = 0, game_animtime, game_paused;

unsigned long jiffies_since(unsigned long past_jiffies);

static void game_finale (void)
{
	unsigned char *d,*s;
	int a;
	unsigned int *frameptr=(unsigned int*)game_animbase;
	static int framenr, frameadj;

	(void)draw_string(2, 44, game_bricks ? "Game Over" : "You Win", COLOR3);
	// freeze the display for two seconds, so user knows game is over
	if (jiffies_since(game_ball_lastmove) < (HZ*2))
		return;
	if (game_bricks) {
		(void)draw_string(1, 20, " Enhancements.V19 ", -COLOR3);
		(void)draw_string(2, 33, "by Mark Lord", COLOR3);
		if (jiffies_since(game_ball_lastmove) < (HZ*3))
			return;
	}

	if (game_animbase == 0 || game_bricks > 0) {
		ir_selected = 1; // return to menu
		return;
	}

	// persistence pays off with a special reward
	if (jiffies_since(game_animtime) < (HZ/(ANIMATION_FPS - 2)))
		return;

	if (game_animtime == 0) { // first frame?
		framenr = 0;
		frameadj = 1;
	} else if (framenr < 0) {
		ir_selected = 1; // return to menu
		return;
	} else if (!frameptr[framenr]) {
		frameadj = -1;  // play it again, backwards
		framenr += frameadj;
	}
	s=(unsigned char*)(game_animbase+frameptr[framenr]);
	d = (unsigned char *)hijacked_displaybuf;
	for(a=0;a<2048;a+=2) {
		*d++=((*s&0xc0)>>2)|((*s&0x30)>>4);
		*d++=((*s&0x0c)<<2)|((*s&0x03));
		s++;
	}
	framenr += frameadj;
	game_animtime = jiffies ? jiffies : 1;
}

static void game_move (unsigned long ignored, int direction)
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
	game_paddle_lastmove = jiffies;
	game_paddle_lastdir  = direction;
	restore_flags(flags);
}

static void game_nuke_brick (short row, short col)
{
	if (col > 0 && col < GAME_COLS && hijacked_displaybuf[row][col] == GAME_BRICKS) {
		hijacked_displaybuf[row][col] = 0;
		if (--game_bricks <= 0)
			game_over = 1;
	}
}

static void game_move_ball (void)
{
	if (game_over) {
		game_finale();
		return;
	}
	if (ir_left_down && jiffies_since(ir_left_down) >= (HZ/15)) {
		ir_left_down = jiffies ? jiffies : 1;
		game_move(0, -1);
	}
	if (ir_right_down && jiffies_since(ir_right_down) >= (HZ/15)) {
		ir_right_down = jiffies ? jiffies : 1;
		game_move(0, 1);
	}
	// Yeah, I know, this allows minor cheating.. but some folks may crave for it
	if (game_paused || (jiffies_since(game_ball_lastmove) < (HZ/game_speed)))
		return;
	game_ball_lastmove = jiffies;
	hijacked_displaybuf[game_row][game_col] = 0;
	game_row += game_vdir;
	game_col += game_hdir;
	if (hijacked_displaybuf[game_row][game_col] == GAME_HBOUNCE) {
		// need to bounce horizontally
		game_hdir = 0 - game_hdir;
		game_col += game_hdir;
	}
	if (game_row == GAME_BRICKS_ROW) {
		game_nuke_brick(game_row,game_col);
		game_nuke_brick(game_row,game_col-1);
		game_nuke_brick(game_row,game_col+1);
	}
	if (hijacked_displaybuf[game_row][game_col] == GAME_VBOUNCE) {
		// need to bounce vertically
		game_vdir = 0 - game_vdir;
		game_row += game_vdir;
		// if we hit a moving paddle, adjust the ball speed
		if (game_row > 1 && (game_ball_lastmove - game_paddle_lastmove) <= (HZ/10)) {
			if (game_paddle_lastdir == game_hdir) {
				if (game_speed < (HZ/2))
					game_speed += 6;
			} else {
				game_speed = (game_speed > 14) ? game_speed - 3 : 11;
			}
		}
	}
	if (hijacked_displaybuf[game_row][game_col] == GAME_OVER)
		game_over = 1;
	hijacked_displaybuf[game_row][game_col] = GAME_BALL;
}

static void game_refresh (unsigned long ignored, int firsttime)
{
	int i;
	if (!firsttime) {
		ir_selected = 0; // prevent accidental exit from game
		game_move_ball();
		return;
	}
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
	game_ball_lastmove = jiffies;
	game_bricks = GAME_COLS - 2;
	game_over = 0;
	game_paused = 0;
	game_speed = 16;
	game_animtime = 0;
	game_starttime = jiffies;
}

static void menu_exit (unsigned long ignored, int firsttime)
{
	hijack_deactivate();
}

static void menu_move (unsigned long ignored, int direction)
{
	menu_item += direction;
	if (menu_item >= menu_size)
		menu_item = menu_size - 1;
	else if (menu_item < 0)
		menu_item = 0;
	menu_lastpress = jiffies;
}

#define MENU_MAX_SIZE 10
static const char *menu_label  [MENU_MAX_SIZE] = {"Break-Out Game", "Volume Auto Adjust", "Screen Blanker", "Font Display", "[exit]", NULL,};
static void (*menu_refreshfunc [MENU_MAX_SIZE])(unsigned long, int) = {game_refresh, voladj_refresh, blanker_refresh, kfont_refresh, menu_exit, NULL,};
static void (*menu_movefunc    [MENU_MAX_SIZE])(unsigned long, int) = {game_move, voladj_move, blanker_move, NULL, NULL, NULL,};
static unsigned long menu_userdata[MENU_MAX_SIZE] = {0,};

static void menu_refresh (unsigned long ignored, int firsttime)
{
	static int old_menu_item = 0;

	if (firsttime || menu_item != old_menu_item) {
		old_menu_item = menu_item;
		clear_hijacked_displaybuf(COLOR0);
		if (firsttime)
			menu_top = 0;
		if (menu_item < menu_top)
			menu_top = menu_item;
		while (menu_item >= (menu_top + EMPEG_TEXT_ROWS))
			++menu_top;
		for (menu_size = 0; menu_label[menu_size] != NULL; ++menu_size) {
			if (menu_size >= menu_top && menu_size < (menu_top + EMPEG_TEXT_ROWS))
				(void)draw_string(menu_size - menu_top, 0, menu_label[menu_size], (menu_size == menu_item) ? COLOR3 : COLOR2);
		}
		menu_lastpress = jiffies;
	}
	if (!firsttime) {
		if (ir_selected)
			activate_subfunc(menu_userdata[menu_item], menu_refreshfunc[menu_item], menu_movefunc[menu_item]);
		else if (jiffies_since(menu_lastpress) > (5 * HZ))
			hijack_deactivate(); // menu timed-out
	}
}

// This routine covertly intercepts all display updates,
// giving us a chance to substitute our own display.
//
void hijack_display(struct display_dev *dev, unsigned char *player_buf)
{
	unsigned char *buf = (unsigned char *)hijacked_displaybuf;
	unsigned long flags;
	static unsigned long screen_saver_poll = 0;
	static unsigned char saved_screen[EMPEG_SCREEN_BYTES] = {0,};

	save_flags_cli(flags);
	switch (hijack_status) {
		case HIJACK_INACTIVE:
			if (ir_trigger_count >= 3 || (ir_knob_down && jiffies_since(ir_knob_down) >= HZ)) {
				ir_trigger_count = 0;
				menu_item = 0;
				activate_subfunc(0, menu_refresh, menu_move);
			} else {
				buf = player_buf;
			}
			break;
		case HIJACK_SUBFUNC_ACTIVE:
			if (hijack_subfunc == NULL) {
				hijack_deactivate();
			} else {
				hijack_subfunc(hijack_userdata, 0);
				if (ir_selected)
					activate_subfunc(0, menu_refresh, menu_move);
			}
			break;
		case HIJACK_SUBFUNC_PENDING:
			if (!ir_releasewait) {
				ir_selected = 0;
				hijack_status = HIJACK_SUBFUNC_ACTIVE;
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
	if (screen_blanker_timeout) {
		if (jiffies_since(screen_saver_poll) >= (HZ / 3)) {
			screen_saver_poll = jiffies;
			if (memcmp(saved_screen, buf, EMPEG_SCREEN_BYTES)) {
				memcpy(saved_screen, buf, EMPEG_SCREEN_BYTES);
				screen_saver = 0;
			} else if (screen_saver == 0) {
				screen_saver = jiffies ? jiffies : 1;
			}
		}
		if (screen_saver && jiffies_since(screen_saver) > (screen_blanker_timeout * (SCREEN_BLANKER_MULTIPLIER * HZ))) {
			buf = player_buf;
			memset(buf,0x00,EMPEG_SCREEN_BYTES);
		}
	}
	display_blat(dev, buf);
}

static int hijack_move (int direction)
{
	if (hijack_status != HIJACK_INACTIVE) {
		if (hijack_status == HIJACK_SUBFUNC_ACTIVE && hijack_movefunc != NULL)
			hijack_movefunc(hijack_userdata, direction);
		return 1; // input WAS hijacked
	}
	return 0; // input NOT hijacked
}

static unsigned long prev_pressed = 0;
static int count_keypresses (unsigned long data)
{
	static unsigned long prev_presstime = 0;
	int rc = 0;
	if (hijack_status == HIJACK_INACTIVE) {
		// ugly Kenwood remote hack: press/release CD 3 times in a row to activate menu
		if (prev_pressed == (data & 0x7fffffff) && prev_presstime && jiffies_since(prev_presstime) < (HZ + (HZ/2)))
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
int hijacked_input (unsigned long data)
{
	int rc = 0; // input NOT hijacked
	unsigned long flags;

	//printk("Button: %08lx, status=%d, ir_knob_down=%u, ir_selected=%u, menu_item=%d\n",
	//	data, hijack_status, ir_knob_down, ir_selected, menu_item);
	save_flags_cli(flags);
	screen_saver = 0;
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
	// save button PRESSED codes in prev_pressed
	if ((data & 0x80000001) == 0 || ((int)data) > 16)
		prev_pressed = data;
	restore_flags(flags);
	return rc;
}

#if 0
// This stuff is still under construction.
//
// The idea here is that userland apps do ioctl() to bind into the menu,
// the ioctl() calls menu_extend(), and then goes to sleep.
// The userland_move() and userland_refresh() procs use "userdata" to
// reference back to the sleeping process and awaken it on suitable events.
//
// I suspect the userland_move() may be useless, but it's there for now.
//


static void userland_move (unsigned long userdata, int direction)
{
	// FIXME: update user's data structure and then wake-up the sleeping thread
}

static void userland_refresh (unsigned long userdata, int firsttime)
{
	// FIXME: copy user's data to hijacked_displaybuf[]
}

static int userland_extend_menu (const char *label, unsigned long userdata)
{
	int i;
	for (i = 0; i < (MENU_MAX_SIZE-1); ++i) {
		if (menu_label[i] == NULL) {
			// Insert new entry before last item [exit]
			menu_label       [i] = menu_label[i-1];
			menu_refreshfunc [i] = menu_refreshfunc[i-1];
			menu_movefunc    [i] = menu_movefunc[i-1];
			menu_userdata    [i] = menu_userdata[i-1];
			--i;
			menu_label       [i] = label;
			menu_refreshfunc [i] = userland_refresh;
			menu_movefunc    [i] = userland_move;
			menu_userdata    [i] = userdata;
			return 0; // success
		}
	}
	return 1; // no room; menu is full
}
#endif /* 0 */
