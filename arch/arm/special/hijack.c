//
// Empeg kernel font routines, by Mark Lord <mlord@pobox.com>
//

#define EMPEG_SCREEN_ROWS 32	/* pixels */
#define EMPEG_SCREEN_COLS 128	/* pixels */

const unsigned char lcase_font[26][8] = {
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
	{0x64,0x54,0x4c,0x00,0x00,0x00,0x00,0x00}  // z
	};

static unsigned int draw_fontchar (unsigned char *display, unsigned int row, unsigned int col, const unsigned char *s)
{
	unsigned int max_offset = 0;

	if (row >= (EMPEG_SCREEN_ROWS/8) || col >= EMPEG_SCREEN_COLS)
		return 0;
	display += row * ((EMPEG_SCREEN_COLS/2)*8);
	for (row = 0; row < 8; ++row) {
		unsigned int offset = 0;
		do {
			unsigned char *d = display + ((col + offset) >> 1);
			unsigned char mask = ((col + offset) & 1) ? 0xf0 : 0x0f;
			*d = (*d & ~mask) | ((s[offset] & (1 << row)) ? (0x33 & mask) : 0);
		} while (s[offset++] && (col + offset) < EMPEG_SCREEN_COLS);
		if (offset > max_offset)
			max_offset = offset;
		display += (EMPEG_SCREEN_COLS/2);
	}
	return max_offset;
}

static unsigned int draw_string (void *display, unsigned int row, unsigned int col, const unsigned char *s)
{
	unsigned char c;

	while ((c = *s++)) {
		if (c >= 'A' && c <= 'Z')
			c += ('a' - 'A');
		if (c >= 'a' && c <= 'z') {
			col += draw_fontchar((unsigned char *)display, row, col, lcase_font[c - 'a']);
		} else {
			col += draw_fontchar((unsigned char *)display, row, col, "");
			col += draw_fontchar((unsigned char *)display, row, col, "");
			col += draw_fontchar((unsigned char *)display, row, col, "");
		}
	}
	return col;
}

//
// Empeg kernel BreakOut game, by Mark Lord <mlord@pobox.com>
//
#define GAME_ROWS (EMPEG_SCREEN_ROWS)
#define GAME_COLS (EMPEG_SCREEN_COLS/2)
#define GAME_VBOUNCE 0xff
#define GAME_BRICKS 0xee
#define GAME_BRICKS_ROW 5
#define GAME_HBOUNCE 0x77
#define GAME_BALL 0xff
#define GAME_OVER 0x11
#define GAME_PADDLE_SIZE 8

static unsigned char game_buffer[GAME_ROWS][GAME_COLS];
static short game_over, game_row, game_col, game_hdir, game_vdir, game_paddle_col, game_paddle_lastdir, game_speed, game_bricks;
static unsigned long game_starttime, game_ball_lastmove, game_paddle_lastmove, game_animbase = 0, game_animtime;
static unsigned int game_is_active = 0, game_knob_down, game_left_down, game_right_down, game_select_count = 0, game_paused;

unsigned long jiffies_since(unsigned long past_jiffies);

static void game_start (void)
{
	int i;
	game_is_active = 1;
	memset(game_buffer,0,EMPEG_SCREEN_SIZE);
	game_paddle_col = GAME_COLS / 2;
	for (i = 0; i < GAME_COLS; ++i) {
		game_buffer[0][i] = GAME_VBOUNCE;
		game_buffer[GAME_ROWS-1][i] = GAME_OVER;
		game_buffer[GAME_BRICKS_ROW][i] = GAME_BRICKS;
	}
	for (i = 0; i < GAME_ROWS; ++i)
		game_buffer[i][0] = game_buffer[i][GAME_COLS-1] = GAME_HBOUNCE;
	memset(&game_buffer[GAME_ROWS-3][game_paddle_col],GAME_VBOUNCE,GAME_PADDLE_SIZE);
	game_hdir = 1;
	game_vdir = 1;
	game_row = 6;
	game_col = jiffies % GAME_COLS;
	if (game_buffer[game_row][game_col] == GAME_HBOUNCE)
		game_col = game_col ? GAME_COLS - 1 : 1;
	game_ball_lastmove = jiffies;
	game_bricks = GAME_COLS - 2;
	game_over = 0;
	game_paused = 0;
	game_speed = 16;
	game_animtime = 0;
	game_starttime = jiffies;
}

static void game_finale (void)
{
	unsigned char *d,*s;
	int a;
	unsigned int *frameptr=(unsigned int*)game_animbase;
	static int framenr, frameadj;

	(void)draw_string(game_buffer, 2, 45, game_bricks ? "Game Over" : "You Win");
	// freeze the display for two seconds, so user knows game is over
	if (jiffies_since(game_ball_lastmove) < (HZ*2))
		return;

	// just exit if the user lost
	if (game_animbase == 0 || game_bricks > 0) {
		game_is_active = 0;
		return;
	}

	// persistence pays off with a special reward
	if (jiffies_since(game_animtime) < (HZ/(ANIMATION_FPS - 2)))
		return;

	if (game_animtime == 0) { // first frame?
		framenr = 0;
		frameadj = 1;
	} else if (framenr < 0) {
		game_is_active = 0;
		return;
	} else if (!frameptr[framenr]) {
		frameadj = -1;  // play it again, backwards
		framenr += frameadj;
	}
	s=(unsigned char*)(game_animbase+frameptr[framenr]);
	d = (unsigned char *)game_buffer;
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
	unsigned char *paddlerow = game_buffer[GAME_ROWS-3];
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
	unsigned char *paddlerow = game_buffer[GAME_ROWS-3];
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
	if (col > 0 && col < GAME_COLS && game_buffer[row][col] == GAME_BRICKS) {
		game_buffer[row][col] = 0;
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
	if (game_left_down && jiffies_since(game_left_down) >= (HZ/15)) {
		game_left_down = jiffies ? jiffies : 1;
		game_move_left();
	}
	if (game_right_down && jiffies_since(game_right_down) >= (HZ/15)) {
		game_right_down = jiffies ? jiffies : 1;
		game_move_right();
	}
	// Yeah, I know, this allows minor cheating.. but some folks may crave for it
	if (game_paused || (jiffies_since(game_ball_lastmove) < (HZ/game_speed)))
		return;
	game_ball_lastmove = jiffies;
	game_buffer[game_row][game_col] = 0;
	game_row += game_vdir;
	game_col += game_hdir;
	if (game_buffer[game_row][game_col] == GAME_HBOUNCE) {
		// need to bounce horizontally
		game_hdir = 0 - game_hdir;
		game_col += game_hdir;
	}
	if (game_row == GAME_BRICKS_ROW) {
		game_nuke_brick(game_row,game_col);
		game_nuke_brick(game_row,game_col-1);
		game_nuke_brick(game_row,game_col+1);
	}
	if (game_buffer[game_row][game_col] == GAME_VBOUNCE) {
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
	if (game_buffer[game_row][game_col] == GAME_OVER)
		game_over = 1;
	game_buffer[game_row][game_col] = GAME_BALL;
}

extern int voladj_enabled;
static int voladj_display_active = 0;

static void voladj_display_setting (int started)
{
	if (!started) {
		unsigned int col = 0;
		memset(game_buffer,0,EMPEG_SCREEN_SIZE);
		col = draw_string(game_buffer, 2, 20, "Volume adjust is ");
		(void)draw_string(game_buffer, 2, col, voladj_enabled ? "On" : "Off");
		voladj_display_active = jiffies;
		game_is_active = 1;
	} else if (jiffies_since(voladj_display_active) >= (HZ*1)) {
		voladj_display_active = 0;
		game_is_active = 0;
	}
}

// This routine covertly intercepts all display updates,
// giving us a chance to substitute our own display.
//
int hijacked_display(struct display_dev *dev)
{
	unsigned long flags;
	save_flags_cli(flags);
	if (!game_is_active) {
		if (game_select_count >= 3 || (game_knob_down && jiffies_since(game_knob_down) >= HZ)) {
			game_select_count = 0;
			game_is_active = 1;
			game_start();
		}
	}
	if (game_is_active) {
		if (voladj_display_active)
			voladj_display_setting(1);
		else
			game_move_ball();
		restore_flags(flags);
		display_blat(dev, (unsigned char *)game_buffer);
		return 1; // display WAS hijacked
	}
	restore_flags(flags);
	return 0; // display was NOT hijacked
}

// This routine covertly intercepts all button presses/releases,
// giving us a chance to ignore them or to trigger our own responses.
//
int hijacked_input (unsigned long data)
{
	static unsigned long prev_pressed = 0, bottom_button_down = 0;

	//printk("Button: %08x\n",data);
	switch (data) {
		case 0x00b9461e: // IR_KW_CD_PRESSED
			// ugly Kenwood remote hack: press CD 3 times in a row to start game
			if (prev_pressed == data)
				++game_select_count;
			else
				game_select_count = 1;
			break;
		case 0x0020df0b: // IR_RIO_SELECTMODE_PRESSED
		case 0x00000008: // IR_KNOB_PRESSED
			game_knob_down = jiffies ? jiffies : 1;
			if (game_is_active)
				return 1; // input WAS hijacked
			break;
		case 0x8020df0b: // IR_RIO_SELECTMODE_RELEASED
		case 0x00000009: // IR_KNOB_RELEASED
			game_knob_down = 0;
			if (game_is_active)
				return 1; // input WAS hijacked
			break;
		case 0x0000000a: // IR_KNOB_RIGHT
			if (game_is_active) {
				game_move_right();
				return 1; // input WAS hijacked
			}
			break;
		case 0x0000000b: // IR_KNOB_LEFT
			if (game_is_active) {
				game_move_left();
				return 1; // input WAS hijacked
			}
			break;
		case 0x00b9460a: // IR_KW_PREVTRACK_PRESSED
		case 0x0020df10: // IR_RIO_PREVTRACK_PRESSED
			game_left_down = jiffies ? jiffies : 1;
			if (game_is_active)
				return 1; // input WAS hijacked
			break;
		case 0x80b9460a: // IR_KW_PREVTRACK_RELEASED
		case 0x8020df10: // IR_RIO_PREVTRACK_RELEASED
			game_left_down = 0;
			if (game_is_active)
				return 1; // input WAS hijacked
			break;
		case 0x00b9460b: // IR_KW_NEXTTRACK_PRESSED
		case 0x0020df11: // IR_RIO_NEXTTRACK_PRESSED
			game_right_down = jiffies ? jiffies : 1;
			if (game_is_active)
				return 1; // input WAS hijacked
			break;
		case 0x80b9460b: // IR_KW_NEXTTRACK_RELEASED
		case 0x8020df11: // IR_RIO_NEXTTRACK_RELEASED
			game_right_down = 0;
			if (game_is_active)
				return 1; // input WAS hijacked
			break;
		case 0x00b9460e:// IR_KW_PAUSE_PRESSED
		case 0x0020df16: // IR_RIO_PAUSE_PRESSED
			if (game_is_active) {
				game_paused = !game_paused;
				return 1; // input WAS hijacked
			}
			break;
		case 0x80b9460e: // IR_KW_PAUSE_RELEASED
		case 0x8020df16: // IR_RIO_PAUSE_RELEASED
			if (game_is_active)
				return 1; // input WAS hijacked
			break;
		case 0x00000006: // IR_BOTTOM_BUTTON_PRESSED
			bottom_button_down = jiffies ? jiffies : 1;
			break;
		case 0x00000007: // IR_BOTTOM_BUTTON_RELEASED
			bottom_button_down = 0;
			break;
		case 0x00000004: // IR_LEFT_BUTTON_PRESSED
			if (bottom_button_down) {
				voladj_enabled = 0;
				voladj_display_setting(0);
				return 1; // input WAS hijacked
			}
			break;
		case 0x00000002: // IR_RIGHT_BUTTON_PRESSED
			if (bottom_button_down) {
				voladj_enabled = 1;
				voladj_display_setting(0);
				return 1; // input WAS hijacked
			}
			break;
	}
	if (!(data & 0x80000000))
		prev_pressed = data;

	return 0; // input was NOT hijacked
}

