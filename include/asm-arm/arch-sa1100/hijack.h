#ifndef KERNEL_HIJACK_H
#define KERNEL_HIJACK_H

#include <asm/arch/empeg.h>

// /* Basic sequence for userland app to bind into the menu, display, and IR buttons */
// /* Not shown: all "rc" return codes have to be checked for success/failure */
//
//    #include <asm/arch/hijack.h>
//    int fd;
//    unsigned long data, buttons[5] = {5,
//       IR_KW_PREVTRACK_PRESSED,
//       IR_KW_PREVTRACK_RELEASED,
//       IR_KW_NEXTTRACK_PRESSED,
//       IR_KW_NEXTTRACK_RELEASED};
//    unsigned char screenbuf[EMPEG_SCREEN_BYTES] = {0,};
//    hijack_geom_t geom = {8,24,32,100}, fullscreen = {0,EMPEG_SCREEN_ROWS-1,0,EMPEG_SCREEN_COLS-1};
//
//    fd = open("/dev/display");
//    top: while (1) {
//       const char *mymenu = {"MyStuff", NULL};  // a NULL terminated array of strings
//       rc = ioctl(fd, EMPEG_HIJACK_WAITMENU, mymenu);
//       if (rc < 0) perror(rc) else menu_index = rc;
//       rc = ioctl(fd,EMPEG_HIJACK_BINDBUTTONS, buttons);
//       rc = ioctl(fd,EMPEG_HIJACK_SETGEOM, &geom);
//       while (looping) {
//          rc = ioctl(fd,EMPEG_HIJACK_DISPWRITE, screenbuf); /* or ioctl(fd,EMPEG_HIJACK_DISPTEXT, "Some\nText"); */
//          rc = ioctl(fd,EMPEG_HIJACK_WAITBUTTONS, &data);
//       }
//       //rc = ioctl(fd,EMPEG_HIJACK_UNBINDBUTTONS, NULL); /* now done by WAITMENU */
//    }
//


// Parameter format for SETGEOM (pass a pointer to one of these)
// When used, the hijack kernel will display ONLY these rows/cols
// from your fullsize display buffer (when using DISPWRITE ioctl),
// with the regular player display on the rest of the screen.
typedef struct hijack_geom_s {
	unsigned short first_row;	// 0 .. EMPEG_SCREEN_ROWS-1
	unsigned short last_row;	// 0 .. EMPEG_SCREEN_ROWS-1, must be > first_row
	unsigned short first_col;	// 0 .. EMPEG_SCREEN_COLS-1; must be multiple of 2
	unsigned short last_col;	// 0 .. EMPEG_SCREEN_COLS-1; must be multiple of 2; must be > first_col
} hijack_geom_t;

#define EMPEG_HIJACK_WAITMENU		_IO(EMPEG_DISPLAY_MAGIC, 80)	// Create menu item and wait for it to be selected
#define EMPEG_HIJACK_DISPWRITE		_IO(EMPEG_DISPLAY_MAGIC, 82)	// Copy buffer to screen
#define EMPEG_HIJACK_BINDBUTTONS	_IO(EMPEG_DISPLAY_MAGIC, 83)	// Specify IR codes to be hijacked
#define EMPEG_HIJACK_UNBINDBUTTONS	_IO(EMPEG_DISPLAY_MAGIC, 84)	// Stop hijacking IR codes (don't forget this!)
#define EMPEG_HIJACK_WAITBUTTONS	_IO(EMPEG_DISPLAY_MAGIC, 85)	// Wait for next hijacked IR code
#define EMPEG_HIJACK_DISPCLEAR		_IO(EMPEG_DISPLAY_MAGIC, 86)	// Clear screen
#define EMPEG_HIJACK_DISPTEXT		_IO(EMPEG_DISPLAY_MAGIC, 87)	// Write text to screen
#define EMPEG_HIJACK_SETGEOM		_IO(EMPEG_DISPLAY_MAGIC, 88)	// Set screen overlay geometry
#define EMPEG_HIJACK_POLLBUTTONS	_IO(EMPEG_DISPLAY_MAGIC, 89)	// Read next IR code; EBUSY if none available
#define EMPEG_HIJACK_INJECTBUTTONS	_IO(EMPEG_DISPLAY_MAGIC, 90)	// Inject button codes into player's input queue

#define EMPEG_SCREEN_ROWS		32				// pixels
#define EMPEG_SCREEN_COLS		128				// pixels
#define EMPEG_SCREEN_BYTES		(EMPEG_SCREEN_ROWS * EMPEG_SCREEN_COLS / 2)
#define EMPEG_TEXT_ROWS			(EMPEG_SCREEN_ROWS / KFONT_HEIGHT)
#define KFONT_HEIGHT			8				// font height is 8 pixels
#define KFONT_WIDTH			6				// font width 5 pixels or less, plus 1 for spacing

// When using these colors from userland, or them with 0x80 first!
// These can be the first byte of a string for EMPEG_HIJACK_DISPTEXT
//
#define COLOR0 0 // blank pixels
#define COLOR1 1 // prefix with '-' for inverse video
#define COLOR2 2 // prefix with '-' for inverse video
#define COLOR3 3 // prefix with '-' for inverse video

// Front panel codes
//
#define IR_TOP_BUTTON_PRESSED		0x00000000
#define IR_TOP_BUTTON_RELEASED		0x00000001
#define IR_RIGHT_BUTTON_PRESSED		0x00000002
#define IR_RIGHT_BUTTON_RELEASED	0x00000003
#define IR_LEFT_BUTTON_PRESSED		0x00000004
#define IR_LEFT_BUTTON_RELEASED		0x00000005
#define IR_BOTTOM_BUTTON_PRESSED	0x00000006
#define IR_BOTTOM_BUTTON_RELEASED	0x00000007
#define IR_KNOB_PRESSED			0x00000008
#define IR_KNOB_RELEASED		0x00000009
#define IR_KNOB_RIGHT			0x0000000a
#define IR_KNOB_LEFT			0x0000000b

// Kenwood RCA-R6A codes
//
#define IR_KW_0_PRESSED			0x00b94600
#define IR_KW_1_PRESSED			0x00b94601
#define IR_KW_2_PRESSED			0x00b94602
#define IR_KW_3_PRESSED			0x00b94603
#define IR_KW_4_PRESSED			0x00b94604
#define IR_KW_5_PRESSED			0x00b94605
#define IR_KW_6_PRESSED			0x00b94606
#define IR_KW_7_PRESSED			0x00b94607
#define IR_KW_8_PRESSED			0x00b94608
#define IR_KW_9_PRESSED			0x00b94609
#define IR_KW_PREVTRACK_PRESSED		0x00b9460A
#define IR_KW_NEXTTRACK_PRESSED		0x00b9460B
#define IR_KW_AM_PRESSED		0x00b9460C
#define IR_KW_FM_PRESSED		0x00b9460D
#define IR_KW_PROG_PRESSED		0x00b9460E
#define IR_KW_PLAY_PRESSED		IR_KW_PROG_PRESSED
#define IR_KW_PAUSE_PRESSED		IR_KW_PROG_PRESSED
#define IR_KW_DIRECT_PRESSED		0x00b9460F
#define IR_KW_VOL_PLUS_PRESSED		0x00b94614
#define IR_KW_VOL_MINUS_PRESSED		0x00b94615
#define IR_KW_STAR_PRESSED		0x00b9461B
#define IR_KW_TUNER_PRESSED		0x00b9461C
#define IR_KW_TAPE_PRESSED		0x00b9461D
#define IR_KW_CD_PRESSED		0x00b9461E
#define IR_KW_CDMDCH_PRESSED		0x00b9461F
#define IR_KW_DNPP_PRESSED		0x00b9465E
#define IR_KW_0_RELEASED		0x80b94600
#define IR_KW_1_RELEASED		0x80b94601
#define IR_KW_2_RELEASED		0x80b94602
#define IR_KW_3_RELEASED		0x80b94603
#define IR_KW_4_RELEASED		0x80b94604
#define IR_KW_5_RELEASED		0x80b94605
#define IR_KW_6_RELEASED		0x80b94606
#define IR_KW_7_RELEASED		0x80b94607
#define IR_KW_8_RELEASED		0x80b94608
#define IR_KW_9_RELEASED		0x80b94609
#define IR_KW_PREVTRACK_RELEASED	0x80b9460A
#define IR_KW_NEXTTRACK_RELEASED	0x80b9460B
#define IR_KW_AM_RELEASED		0x80b9460C
#define IR_KW_FM_RELEASED		0x80b9460D
#define IR_KW_PROG_RELEASED		0x80b9460E
#define IR_KW_PLAY_RELEASED		IR_KW_PROG_RELEASED
#define IR_KW_PAUSE_RELEASED		IR_KW_PROG_RELEASED
#define IR_KW_DIRECT_RELEASED		0x80b9460F
#define IR_KW_VOLPLUS_RELEASED		0x80b94614
#define IR_KW_VOLMINUS_RELEASED	0x80b94615
#define IR_KW_STAR_RELEASED		0x80b9461B
#define IR_KW_TUNER_RELEASED		0x80b9461C
#define IR_KW_TAPE_RELEASED		0x80b9461D
#define IR_KW_CD_RELEASED		0x80b9461E
#define IR_KW_CDMDCH_RELEASED		0x80b9461F
#define IR_KW_DNPP_RELEASED		0x80b9465E

// Rio ERC-1A codes (list not completed yet)
//
#define IR_RIO_SELECTMODE_PRESSED	0x0020df0b
#define IR_RIO_SELECTMODE_RELEASED	0x8020df0b
#define IR_RIO_PREVTRACK_PRESSED	0x0020df10
#define IR_RIO_PREVTRACK_RELEASED	0x8020df10
#define IR_RIO_NEXTTRACK_PRESSED	0x0020df11
#define IR_RIO_NEXTTRACK_RELEASED	0x8020df11
#define IR_RIO_PAUSE_PRESSED		0x0020df16
#define IR_RIO_PAUSE_RELEASED		0x8020df16
#define IR_RIO_INFO_PRESSED		0x0020df14
#define IR_RIO_INFO_RELEASED		0x8020df14
#define IR_RIO_MENU_PRESSED		0x0020df12
#define IR_RIO_MENU_RELEASED		0x8020df12
#define IR_RIO_SOURCE_PRESSED		0x0020df03
#define IR_RIO_SOURCE_RELEASED		0x8020df03
#define IR_RIO_VOLPLUS_PRESSED		0x0020df13
#define IR_RIO_VOLPLUS_RELEASED		0x8020df13
#define IR_RIO_VOLMINUS_PRESSED		0x0020df17
#define IR_RIO_VOLMINUS_RELEASED	0x8020df17

#define IR_RIO_0_PRESSED		0x0020df0d
#define IR_RIO_SHUFFLE_PRESSED		IR_RIO_0_PRESSED
#define IR_RIO_1_PRESSED		0x0020df00
#define IR_RIO_2_PRESSED		0x0020df01
#define IR_RIO_3_PRESSED		0x0020df02
#define IR_RIO_4_PRESSED		0x0020df04
#define IR_RIO_5_PRESSED		0x0020df05
#define IR_RIO_6_PRESSED		0x0020df06
#define IR_RIO_7_PRESSED		0x0020df08
#define IR_RIO_REPEAT_PRESSED		IR_RIO_7_PRESSED
#define IR_RIO_8_PRESSED		0x0020df09
#define IR_RIO_9_PRESSED		0x0020df0a
#define IR_RIO_SOUND_PRESSED		0x0020df0f
#define IR_RIO_EQUALIZER_PRESSED	IR_RIO_SOUND_PRESSED
#define IR_RIO_CANCEL_PRESSED		0x0020df0c
#define IR_RIO_PLAY_PRESSED		0x0020df16
#define IR_RIO_SEARCH_PRESSED		0x0020df0e

#define IR_RIO_0_RELEASED		0x8020df0d
#define IR_RIO_SHUFFLE_RELEASED		IR_RIO_0_RELEASED
#define IR_RIO_1_RELEASED		0x8020df00
#define IR_RIO_2_RELEASED		0x8020df01
#define IR_RIO_3_RELEASED		0x8020df02
#define IR_RIO_4_RELEASED		0x8020df04
#define IR_RIO_5_RELEASED		0x8020df05
#define IR_RIO_6_RELEASED		0x8020df06
#define IR_RIO_7_RELEASED		0x8020df08
#define IR_RIO_REPEAT_RELEASED		IR_RIO_7_RELEASED
#define IR_RIO_8_RELEASED		0x8020df09
#define IR_RIO_9_RELEASED		0x8020df0a
#define IR_RIO_SOUND_RELEASED		0x8020df0f
#define IR_RIO_EQUALIZER_RELEASED	IR_RIO_SOUND_RELEASED
#define IR_RIO_CANCEL_RELEASED		0x8020df0c
#define IR_RIO_PLAY_RELEASED		0x8020df16
#define IR_RIO_SEARCH_RELEASED		0x8020df0e

#endif // KERNEL_HIJACK_H
