
int hijack_status = 0, hijack_knob_wait = 0;
#define HIJACK_INACTIVE		0
#define HIJACK_KNOB_WAIT	1
#define HIJACK_MENU_ACTIVE	2
#define HIJACK_GAME_ACTIVE	3
#define HIJACK_VOLADJ_ACTIVE	4

//
// Empeg kernel font routines, by Mark Lord <mlord@pobox.com>
//
#define EMPEG_SCREEN_ROWS 32	/* pixels */
#define EMPEG_SCREEN_COLS 128	/* pixels */

const unsigned char kfont[94][8] = {
	{0x5f,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // !
	{0x03,0x03,0x00,0x00,0x00,0x00,0x00,0x00}, // "
	{0x14,0x3e,0x14,0x3e,0x14,0x00,0x00,0x00}, // #
	{0x24,0x4a,0x7f,0x4a,0x32,0x00,0x00,0x00}, // $
	{0x26,0x16,0x08,0x34,0x32,0x00,0x00,0x00}, // %
	{0x02,0x01,0x00,0x00,0x00,0x00,0x00,0x00}, // '
	{0x34,0x4a,0x49,0x56,0x20,0x50,0x00,0x00}, // &
	{0x3e,0x41,0x00,0x00,0x00,0x00,0x00,0x00}, // (
	{0x41,0x3e,0x00,0x00,0x00,0x00,0x00,0x00}, // )
	{0x24,0x18,0x3c,0x18,0x24,0x00,0x00,0x00}, // *
	{0x08,0x08,0x3e,0x08,0x08,0x00,0x00,0x00}, // +
	{0x80,0x40,0x00,0x00,0x00,0x00,0x00,0x00}, // ,
	{0x08,0x08,0x08,0x08,0x00,0x00,0x00,0x00}, // -
	{0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // .
	{0x20,0x10,0x08,0x04,0x02,0x00,0x00,0x00}, // /
	{0x3e,0x41,0x41,0x41,0x3e,0x00,0x00,0x00}, // 0
	{0x02,0x7f,0x00,0x00,0x00,0x00,0x00,0x00}, // 1
	{0x42,0x61,0x51,0x49,0x46,0x00,0x00,0x00}, // 2
	{0x22,0x49,0x49,0x49,0x36,0x00,0x00,0x00}, // 3
	{0x18,0x14,0x12,0x7f,0x10,0x00,0x00,0x00}, // 4
	{0x27,0x49,0x49,0x49,0x31,0x00,0x00,0x00}, // 5
	{0x3e,0x49,0x49,0x49,0x32,0x00,0x00,0x00}, // 6
	{0x01,0x01,0x61,0x19,0x07,0x00,0x00,0x00}, // 7
	{0x36,0x49,0x49,0x49,0x36,0x00,0x00,0x00}, // 8
	{0x26,0x49,0x49,0x49,0x3e,0x00,0x00,0x00}, // 9
	{0x24,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // :
	{0x40,0x24,0x00,0x00,0x00,0x00,0x00,0x00}, // ;
	{0x08,0x14,0x22,0x41,0x00,0x00,0x00,0x00}, // <
	{0x14,0x14,0x14,0x14,0x00,0x00,0x00,0x00}, // =
	{0x41,0x22,0x14,0x08,0x00,0x00,0x00,0x00}, // >
	{0002,0xb1,0x09,0x09,0x06,0x00,0x00,0x00}, // ?
	{0x3e,0x49,0x55,0x55,0x5e,0x00,0x00,0x00}, // @
	{0x7e,0x09,0x09,0x09,0x7e,0x00,0x00,0x00}, // A
	{0x7f,0x49,0x49,0x49,0x36,0x00,0x00,0x00}, // B
	{0x3e,0x41,0x41,0x41,0x22,0x00,0x00,0x00}, // C
	{0x7f,0x41,0x41,0x41,0x3e,0x00,0x00,0x00}, // D
	{0x7f,0x49,0x49,0x41,0x41,0x00,0x00,0x00}, // E
	{0x7f,0x09,0x09,0x01,0x01,0x00,0x00,0x00}, // F
	{0x3e,0x41,0x41,0x49,0x3a,0x00,0x00,0x00}, // G
	{0x7f,0x08,0x08,0x08,0x7f,0x00,0x00,0x00}, // H
	{0x41,0x7f,0x41,0x00,0x00,0x00,0x00,0x00}, // I
	{0x20,0x41,0x41,0x3f,0x01,0x00,0x00,0x00}, // J
	{0x7f,0x08,0x14,0x22,0x41,0x00,0x00,0x00}, // K
	{0x7f,0x40,0x40,0x40,0x40,0x00,0x00,0x00}, // L
	{0x7f,0x02,0x04,0x02,0x7f,0x00,0x00,0x00}, // M
	{0x7f,0x04,0x08,0x10,0x7f,0x00,0x00,0x00}, // N
	{0x3e,0x41,0x41,0x41,0x3e,0x00,0x00,0x00}, // O
	{0x7f,0x09,0x09,0x09,0x06,0x00,0x00,0x00}, // P
	{0x3e,0x41,0x51,0x21,0x5e,0x00,0x00,0x00}, // Q
	{0x7f,0x09,0x19,0x29,0x46,0x00,0x00,0x00}, // R
	{0x26,0x49,0x49,0x49,0x32,0x00,0x00,0x00}, // S
	{0x01,0x01,0x7f,0x01,0x01,0x00,0x00,0x00}, // T
	{0x3f,0x40,0x40,0x40,0x3f,0x00,0x00,0x00}, // U
	{0x1f,0x20,0x40,0x20,0x1f,0x00,0x00,0x00}, // V
	{0x7f,0x20,0x10,0x20,0x7f,0x00,0x00,0x00}, // W
	{0x63,0x14,0x08,0x14,0x63,0x00,0x00,0x00}, // X
	{0x03,0x04,0x78,0x04,0x03,0x00,0x00,0x00}, // Y
	{0x61,0x51,0x49,0x45,0x43,0x00,0x00,0x00}, // Z
	{0x7f,0x41,0x41,0x00,0x00,0x00,0x00,0x00}, // [
	{0x02,0x04,0x08,0x10,0x20,0x00,0x00,0x00}, // \ 
	{0x41,0x41,0x7f,0x00,0x00,0x00,0x00,0x00}, // ]
	{0x02,0x01,0x02,0x00,0x00,0x00,0x00,0x00}, // ^
	{0x80,0x80,0x80,0x80,0x00,0x00,0x00,0x00}, // _
	{0x01,0x02,0x00,0x00,0x00,0x00,0x00,0x00}, // `
	{0x38,0x44,0x24,0x78,0x00,0x00,0x00,0x00}, // a
	{0x7f,0x44,0x44,0x38,0x00,0x00,0x00,0x00}, // b
	{0x38,0x44,0x44,0x28,0x00,0x00,0x00,0x00}, // c
	{0x38,0x44,0x44,0x7f,0x00,0x00,0x00,0x00}, // d
	{0x38,0x54,0x54,0x58,0x00,0x00,0x00,0x00}, // e
	{0x08,0x7e,0x09,0x01,0x02,0x00,0x00,0x00}, // f
	{0x98,0xa4,0xa4,0x78,0x00,0x00,0x00,0x00}, // g
	{0x7f,0x04,0x04,0x78,0x00,0x00,0x00,0x00}, // h
	{0x7d,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // i
	{0x40,0x80,0x7d,0x00,0x00,0x00,0x00,0x00}, // j
	{0x7f,0x10,0x28,0x44,0x00,0x00,0x00,0x00}, // k
	{0x7f,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // l
	{0x7c,0x04,0x04,0x7c,0x04,0x04,0x78,0x00}, // m
	{0x7c,0x04,0x04,0x78,0x00,0x00,0x00,0x00}, // n
	{0x38,0x44,0x44,0x38,0x00,0x00,0x00,0x00}, // o
	{0xfc,0x24,0x24,0x18,0x00,0x00,0x00,0x00}, // p
	{0x18,0x24,0x24,0xf8,0x00,0x00,0x00,0x00}, // q
	{0x7c,0x08,0x04,0x08,0x00,0x00,0x00,0x00}, // r
	{0x08,0x54,0x54,0x20,0x00,0x00,0x00,0x00}, // s
	{0x04,0x3e,0x44,0x24,0x00,0x00,0x00,0x00}, // t
	{0x3c,0x40,0x40,0x7c,0x00,0x00,0x00,0x00}, // u
	{0x1c,0x20,0x40,0x20,0x1c,0x00,0x00,0x00}, // v
	{0x3c,0x40,0x78,0x40,0x3c,0x00,0x00,0x00}, // w
	{0x44,0x28,0x10,0x28,0x44,0x00,0x00,0x00}, // x
	{0x9c,0xa0,0xa0,0x7c,0x00,0x00,0x00,0x00}, // y
	{0x64,0x54,0x4c,0x00,0x00,0x00,0x00,0x00}, // z
	{0x08,0x3e,0x41,0x00,0x00,0x00,0x00,0x00}, // {
	{0x7f,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // |
	{0x41,0x3e,0x08,0x00,0x00,0x00,0x00,0x00}, // }
	{0x02,0x01,0x02,0x04,0x02,0x00,0x00,0x00}  // ~
	};

#define KFONT_HEIGHT	8	// kfont characters are 8 pixels high (and variable width)
#define COLOR0		0	// blank pixels
#define COLOR1		1	// prefix with '-' for inverse video
#define COLOR2		2	// prefix with '-' for inverse video
#define COLOR3		3	// prefix with '-' for inverse video

static unsigned char hijacked_displaybuf[EMPEG_SCREEN_ROWS][EMPEG_SCREEN_COLS/2];

static void clear_hijacked_displaybuf (int color)
{
	color &= 3;
	color |= color << 4;
	memset(hijacked_displaybuf,color,EMPEG_SCREEN_SIZE);
}

static unsigned int
draw_font_entry (unsigned char *displayrow, unsigned int pixel_col, const unsigned char *font_entry, int color)
{
	unsigned int  pixel_row, max_offset = 0;
	unsigned char inverse = 0;

	if (pixel_col >= EMPEG_SCREEN_COLS)
		return 0;
	if (color < 0)
		color = inverse = -color;
	color &= 3;
	color |= color << 4;
	if (inverse)
		inverse = color;
	for (pixel_row = 0; pixel_row < KFONT_HEIGHT; ++pixel_row) {
		unsigned int offset = 0;
		unsigned char pixel_mask = (pixel_col & 1) ? 0xf0 : 0x0f;
		do {
			unsigned char font_bit    = font_entry[offset] & (1 << pixel_row);
			unsigned char new_pixel   = font_bit ? (color & pixel_mask) : 0;
			unsigned char *pixel_pair = displayrow + ((pixel_col + offset) >> 1);
			*pixel_pair = ((inverse & pixel_mask) | (*pixel_pair & (pixel_mask = ~pixel_mask))) ^ new_pixel;
		} while (font_entry[offset++] && (pixel_col + offset) < EMPEG_SCREEN_COLS);
		if (offset > max_offset)
			max_offset = offset;
		displayrow += (EMPEG_SCREEN_COLS / 2);
	}
	return max_offset;
}

static unsigned int
draw_string (int text_row, unsigned int pixel_col, const unsigned char *s, int color)
{
	if (text_row < (EMPEG_SCREEN_ROWS / KFONT_HEIGHT)) {
		unsigned char *displayrow = ((unsigned char *)hijacked_displaybuf) + (text_row * (KFONT_HEIGHT * EMPEG_SCREEN_COLS / 2));
		unsigned char c;
		while ((c = *s++) != 0) {
			if (c >= 0x21 && c <= 0x7e) {
				pixel_col += draw_font_entry(displayrow, pixel_col, kfont[c - 0x21], color);
			} else {
				// output a "space" (three columns with no pixels set)
				pixel_col += draw_font_entry(displayrow, pixel_col, "", color);
				pixel_col += draw_font_entry(displayrow, pixel_col, "", color);
				pixel_col += draw_font_entry(displayrow, pixel_col, "", color);
			}
		}
	}
	return pixel_col;
}

//
// Empeg kernel BreakOut game, by Mark Lord <mlord@pobox.com>
//
#define GAME_COLS (EMPEG_SCREEN_COLS/2)
#define GAME_VBOUNCE 0xff
#define GAME_BRICKS 0xee
#define GAME_BRICKS_ROW 5
#define GAME_HBOUNCE 0x77
#define GAME_BALL 0xff
#define GAME_OVER 0x11
#define GAME_PADDLE_SIZE 8

static short game_over, game_row, game_col, game_hdir, game_vdir, game_paddle_col, game_paddle_lastdir, game_speed, game_bricks;
static unsigned long game_starttime, game_ball_lastmove, game_paddle_lastmove, game_animbase = 0, game_animtime;
static unsigned int ir_selected = 0, ir_knob_down = 0, ir_left_down = 0, ir_right_down = 0, ir_trigger_count = 0, game_paused;

unsigned long jiffies_since(unsigned long past_jiffies);

static void game_start (void)
{
	int i;
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
	if (hijacked_displaybuf[game_row][game_col] == GAME_HBOUNCE)
		game_col = game_col ? GAME_COLS - 1 : 1;
	game_ball_lastmove = jiffies;
	game_bricks = GAME_COLS - 2;
	game_over = 0;
	game_paused = 0;
	game_speed = 16;
	game_animtime = 0;
	game_starttime = jiffies;
	hijack_status = HIJACK_GAME_ACTIVE;
}

static void game_finale (void)
{
	unsigned char *d,*s;
	int a;
	unsigned int *frameptr=(unsigned int*)game_animbase;
	static int framenr, frameadj;

	(void)draw_string(2, 40, game_bricks ? "Game Over" : "You Win", COLOR3);
	// freeze the display for two seconds, so user knows game is over
	if (jiffies_since(game_ball_lastmove) < (HZ*2))
		return;
	if (game_bricks) {
		(void)draw_string(1, 17, " Enhancements.V14 ", -COLOR3);
		(void)draw_string(2, 30, "by Mark Lord", COLOR3);
		if (jiffies_since(game_ball_lastmove) < (HZ*3))
			return;
	}

	// just exit if the user lost
	if (game_animbase == 0 || game_bricks > 0) {
		hijack_status = HIJACK_INACTIVE;
		return;
	}

	// persistence pays off with a special reward
	if (jiffies_since(game_animtime) < (HZ/(ANIMATION_FPS - 2)))
		return;

	if (game_animtime == 0) { // first frame?
		framenr = 0;
		frameadj = 1;
	} else if (framenr < 0) {
		hijack_status = HIJACK_INACTIVE;
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

static void game_move_right (void)
{
	unsigned char *paddlerow = hijacked_displaybuf[EMPEG_SCREEN_ROWS-3];
	int i = 3;
	unsigned long flags;
	save_flags_cli(flags);
	while (i-- > 0) {
		if (game_paddle_col < (GAME_COLS - GAME_PADDLE_SIZE - 1)) {
			paddlerow[game_paddle_col] = 0;
			if (paddlerow[game_paddle_col + GAME_PADDLE_SIZE] != 0)
				--game_row; // scoop up the ball
			paddlerow[game_paddle_col++ + GAME_PADDLE_SIZE] = GAME_VBOUNCE;
			game_paddle_lastmove = jiffies;
			game_paddle_lastdir = 1;
		}
	}
	restore_flags(flags);
}

static void game_move_left (void)
{
	unsigned char *paddlerow = hijacked_displaybuf[EMPEG_SCREEN_ROWS-3];
	int i = 3;
	unsigned long flags;
	save_flags_cli(flags);
	while (i-- > 0) {
		if (game_paddle_col > 1) {
			paddlerow[--game_paddle_col + GAME_PADDLE_SIZE] = 0;
			if (paddlerow[game_paddle_col] != 0)
				--game_row; // scoop up the ball
			paddlerow[game_paddle_col] = GAME_VBOUNCE;
			game_paddle_lastmove = jiffies;
			game_paddle_lastdir = -1;
		}
	}
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
		game_move_left();
	}
	if (ir_right_down && jiffies_since(ir_right_down) >= (HZ/15)) {
		ir_right_down = jiffies ? jiffies : 1;
		game_move_right();
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

static int  menu_item = 0, menu_size = 0;

static void menu_draw (void)
{
	const char *menu[] = {"Volume Adjust Preset", "Break-Out Game", "[exit]", NULL};
	clear_hijacked_displaybuf(COLOR0);
	for (menu_size = 0; menu[menu_size] != NULL; ++menu_size)
		(void)draw_string(menu_size, 0, menu[menu_size], (menu_size == menu_item) ? COLOR3 : COLOR2);
}

static void menu_start (void)
{
	menu_item = 0;
	menu_draw();
	hijack_knob_wait = HIJACK_MENU_ACTIVE;
	hijack_status = HIJACK_KNOB_WAIT;
}

static void menu_move (int amount)
{
	menu_item += amount;
	if (menu_item >= menu_size)
		menu_item = 0;
	else if (menu_item < 0)
		menu_item = menu_size - 1;
}

extern void voladj_next_preset(int);
extern int  voladj_enabled;
static const char *voladj_names[4] = {"Off", "Normal", "High", "Max"};

static void voladj_display_setting (int firsttime)
{
	unsigned int col;
	clear_hijacked_displaybuf(COLOR0);
	col  = draw_string(0,   0, "Volume Adjust:  ", COLOR2);
	(void) draw_string(0, col, voladj_names[voladj_enabled], COLOR3);
	if (firsttime) {
		hijack_knob_wait = HIJACK_VOLADJ_ACTIVE;
		hijack_status = HIJACK_KNOB_WAIT;
	} else if (ir_selected) {
		//hijack_status = HIJACK_INACTIVE;
		menu_start(); // return to main menu
	}
}

static void voladj_move (int amount)
{
	voladj_enabled = (voladj_enabled + amount) & 3;
}

// This routine covertly intercepts all display updates,
// giving us a chance to substitute our own display.
//
int hijacked_display(struct display_dev *dev)
{
	unsigned long flags;

	save_flags_cli(flags);
	if (hijack_status == HIJACK_INACTIVE) {
		if (ir_trigger_count >= 3 || (ir_knob_down && jiffies_since(ir_knob_down) >= HZ)) {
			ir_trigger_count = 0;
			menu_start();
		}
	}
	if (hijack_status != HIJACK_INACTIVE) {
		if (hijack_status == HIJACK_GAME_ACTIVE) {
			game_move_ball();
		} else if (hijack_status == HIJACK_VOLADJ_ACTIVE) {
			voladj_display_setting(0);
		} else if (hijack_status == HIJACK_KNOB_WAIT) {
			if (!ir_knob_down) {
				ir_selected = 0;
				hijack_status = hijack_knob_wait;
				hijack_knob_wait = 0;
			}
		} else if (hijack_status == HIJACK_MENU_ACTIVE) {
			menu_draw();
			if (ir_selected) {
				ir_selected = 0;
				switch (menu_item) {
					case 0:
						voladj_display_setting(1);
						break;
					case 1:
						game_start();
						break;
					default:
						hijack_knob_wait = HIJACK_INACTIVE;
						hijack_status = HIJACK_KNOB_WAIT;
						break;
				}
			}
		} else {
			hijack_status = HIJACK_INACTIVE;
		}
		restore_flags(flags);
		display_blat(dev, (unsigned char *)hijacked_displaybuf);
		return 1; // display WAS hijacked
	}
	restore_flags(flags);
	return 0; // display was NOT hijacked
}

static unsigned long prev_pressed = 0;

static int check_selected (unsigned long data)
{
	int rc = 0;
	switch (hijack_status) {
		case HIJACK_VOLADJ_ACTIVE:
		case HIJACK_MENU_ACTIVE:
			ir_selected = 1;
			rc = 1; // input WAS hijacked
			break;
		case HIJACK_INACTIVE:
			// ugly Kenwood remote hack: press/release CD 3 times in a row to activate menu
			if (prev_pressed == (data & 0x7fffffff))
				++ir_trigger_count;
			else
				ir_trigger_count = 1;
			break;
		default:
			rc = 1; // input WAS hijacked
	}
	return rc;
}

// This routine covertly intercepts all button presses/releases,
// giving us a chance to ignore them or to trigger our own responses.
//
int hijacked_input (unsigned long data)
{
	int rc = 0;
	unsigned long flags;

	//printk("Button: %08x, status=%d, ir_knob_down=%u, ir_selected=%u\n", data, hijack_status, ir_knob_down, ir_selected);
	save_flags_cli(flags);
	switch (data) {
		case 0x80b9461e: // IR_KW_CD_RELEASED
			rc = check_selected(data);
			break;
		case 0x0020df0b: // IR_RIO_SELECTMODE_PRESSED
		case 0x00000008: // IR_KNOB_PRESSED
			ir_knob_down = jiffies ? jiffies : 1;
			if (hijack_status != HIJACK_INACTIVE) {
				switch (hijack_status) {
					case HIJACK_VOLADJ_ACTIVE:
					case HIJACK_MENU_ACTIVE:
						ir_selected = 1;
						break;
				}
				rc = 1; // input WAS hijacked
			}
			break;
		case 0x8020df0b: // IR_RIO_SELECTMODE_RELEASED
			rc = check_selected(data);
			// fall thru
		case 0x00000009: // IR_KNOB_RELEASED  (these come in pairs sometimes)
			ir_knob_down = 0;
			if (hijack_status != HIJACK_INACTIVE) {
				rc = 1; // input WAS hijacked
			}
			break;
		case 0x0000000a: // IR_KNOB_RIGHT
			if (hijack_status != HIJACK_INACTIVE) {
				switch (hijack_status) {
					case HIJACK_MENU_ACTIVE:   menu_move(1); break;
					case HIJACK_GAME_ACTIVE:   game_move_right(); break;
					case HIJACK_VOLADJ_ACTIVE: voladj_move(1); break;
				}
				rc = 1; // input WAS hijacked
			}
			break;
		case 0x0000000b: // IR_KNOB_LEFT
			if (hijack_status != HIJACK_INACTIVE) {
				switch (hijack_status) {
					case HIJACK_MENU_ACTIVE:   menu_move(-1); break;
					case HIJACK_GAME_ACTIVE:   game_move_left(); break;
					case HIJACK_VOLADJ_ACTIVE: voladj_move(-1); break;
				}
				rc = 1; // input WAS hijacked
			}
			break;
		case 0x00b9460a: // IR_KW_PREVTRACK_PRESSED
		case 0x0020df10: // IR_RIO_PREVTRACK_PRESSED
			ir_left_down = jiffies ? jiffies : 1;
			if (hijack_status != HIJACK_INACTIVE) {
				switch (hijack_status) {
					case HIJACK_MENU_ACTIVE:   menu_move(-1); break;
					case HIJACK_VOLADJ_ACTIVE: voladj_move(-1); break;
				}
				rc = 1; // input WAS hijacked
			}
			break;
		case 0x80b9460a: // IR_KW_PREVTRACK_RELEASED
		case 0x8020df10: // IR_RIO_PREVTRACK_RELEASED
			ir_left_down = 0;
			if (hijack_status != HIJACK_INACTIVE)
				rc = 1; // input WAS hijacked
			break;
		case 0x00b9460b: // IR_KW_NEXTTRACK_PRESSED
		case 0x0020df11: // IR_RIO_NEXTTRACK_PRESSED
			ir_right_down = jiffies ? jiffies : 1;
			if (hijack_status != HIJACK_INACTIVE) {
				switch (hijack_status) {
					case HIJACK_MENU_ACTIVE:   menu_move(1); break;
					case HIJACK_VOLADJ_ACTIVE: voladj_move(1); break;
				}
				rc = 1; // input WAS hijacked
			}
			break;
		case 0x80b9460b: // IR_KW_NEXTTRACK_RELEASED
		case 0x8020df11: // IR_RIO_NEXTTRACK_RELEASED
			ir_right_down = 0;
			if (hijack_status != HIJACK_INACTIVE)
				rc = 1; // input WAS hijacked
			break;
		case 0x00b9460e:// IR_KW_PAUSE_PRESSED
		case 0x0020df16: // IR_RIO_PAUSE_PRESSED
			if (hijack_status != HIJACK_INACTIVE) {
				if (hijack_status == HIJACK_GAME_ACTIVE)
					game_paused = !game_paused;
				rc = 1; // input WAS hijacked
			}
			break;
		case 0x80b9460e: // IR_KW_PAUSE_RELEASED
		case 0x8020df16: // IR_RIO_PAUSE_RELEASED
			if (hijack_status == HIJACK_GAME_ACTIVE)
				rc = 1; // input WAS hijacked
			break;
	}
	// save button PRESSED codes in prev_pressed
	if ((data & 0x80000001) == 0 || ((int)data) > 16)
		prev_pressed = data;
	restore_flags(flags);
	return rc;
}

