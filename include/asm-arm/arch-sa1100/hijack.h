#ifndef KERNEL_HIJACK_H
#define KERNEL_HIJACK_H

#include <linux/empeg.h>

// /* Basic sequence for userland app to bind into the menu, display, and IR buttons */
// /* Not shown: all "rc" return codes have to be checked for success/failure */
//
//    ?include <asm/arch/hijack.h>
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
//    rc = ioctl(fd,EMPEG_HIJACK_WAITMENU, NULL);	// release the menu system on program exit
//

#ifdef __KERNEL__
static inline unsigned long JIFFIES (void)
{
	unsigned long jiff = jiffies;
	return jiff ? jiff : ~0UL;
}
#ifdef CONFIG_SMC9194_TIFON	// not present on Mk1 models
extern int player_version;	// hijack.c
#endif
#endif

// Known player executable sizes are listed below.
//
// With the exception of (v2rc3 -> v2final),
// the player has increased in size for each release.
// So, for internal version tests, we can currently
// just use the player's size as a relative version number.
//
#define MK2_PLAYER_v102		1091472
#define MK2_PLAYER_v103		1120144
#define MK2_PLAYER_v2b3		1637404
#define MK2_PLAYER_v2b6		1667772
#define MK2_PLAYER_v2b7		1682484
#define MK2_PLAYER_v2b8		1312984
#define MK2_PLAYER_v2b9		1316280
#define MK2_PLAYER_v2b10	1323248
#define MK2_PLAYER_v2b11	1323440
#define MK2_PLAYER_v2b12	1328048
#define MK2_PLAYER_v2b13	1336792
#define MK2_PLAYER_v2rc1	1344824
#define MK2_PLAYER_v2rc2	1346936
#define MK2_PLAYER_v2rc3	1350780
#define MK2_PLAYER_v2final	1350748 // anomaly: smaller than v2rc3
#define MK2_PLAYER_v3a1		1731364
#define MK2_PLAYER_v3a2		1895536
#define MK2_PLAYER_v3a3		1900988
#define MK2_PLAYER_v3a5		1907572
#define MK2_PLAYER_v3a6		1915200

#define INRANGE(c,min,max)	((c) >= (min) && (c) <= (max))
#define TOUPPER(c)		(INRANGE((c),'a','z') ? ((c) - ('a' - 'A')) : (c))

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
#define EMPEG_HIJACK_GETPLAYERBUFFER	_IO(EMPEG_DISPLAY_MAGIC, 91)	// Inject button codes into player's input queue
#define EMPEG_HIJACK_GETPLAYERUIFLAGS	_IO(EMPEG_DISPLAY_MAGIC, 92)	// Inject button codes into player's input queue
#define EMPEG_HIJACK_READ_GPLR		_IO(EMPEG_DISPLAY_MAGIC, 0xa0)	// Read state of serial port flow control pins (and other stuff)
#define EMPEG_HIJACK_TUNER_SEND		_IO(EMPEG_DISPLAY_MAGIC, 0xee)	// Send bytestring to Tuner.  First byte is bytecount.

#define KFONT_HEIGHT			8				// font height is 8 pixels
#define KFONT_WIDTH			6				// font width 5 pixels or less, plus 1 for spacing
#define EMPEG_SCREEN_ROWS		32				// pixels
#define EMPEG_SCREEN_COLS		128				// pixels
#define EMPEG_SCREEN_BYTES		(EMPEG_SCREEN_ROWS * EMPEG_SCREEN_COLS / 2)
#define EMPEG_TEXT_ROWS			(EMPEG_SCREEN_ROWS / KFONT_HEIGHT)

// When using these colors from userland, or them with 0x80 first!
// These can be the first byte of a string for EMPEG_HIJACK_DISPTEXT
//
#define COLOR0	0	// blank pixels
#define COLOR1	1	// faint
#define COLOR2	2	// light
#define COLOR3	3	// bold
// use "-COLOR3" for inverse video


#define IR_INTERNAL		((void *)-1)


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
#define IR_KW_VOLMINUS_RELEASED		0x80b94615
#define IR_KW_STAR_RELEASED		0x80b9461B
#define IR_KW_TUNER_RELEASED		0x80b9461C
#define IR_KW_TAPE_RELEASED		0x80b9461D
#define IR_KW_CD_RELEASED		0x80b9461E
#define IR_KW_CDMDCH_RELEASED		0x80b9461F
#define IR_KW_DNPP_RELEASED		0x80b9465E

// Rio ERC-1A codes (list not completed yet)
//
#define IR_RIO_1_PRESSED		0x0020df00
#define IR_RIO_1_RELEASED		0x8020df00
#define IR_RIO_2_PRESSED		0x0020df01
#define IR_RIO_2_RELEASED		0x8020df01
#define IR_RIO_3_PRESSED		0x0020df02
#define IR_RIO_3_RELEASED		0x8020df02
#define IR_RIO_SOURCE_PRESSED		0x0020df03
#define IR_RIO_SOURCE_RELEASED		0x8020df03
#define IR_RIO_4_PRESSED		0x0020df04
#define IR_RIO_4_RELEASED		0x8020df04
#define IR_RIO_5_PRESSED		0x0020df05
#define IR_RIO_5_RELEASED		0x8020df05
#define IR_RIO_6_PRESSED		0x0020df06
#define IR_RIO_6_RELEASED		0x8020df06
#define IR_RIO_TUNER_PRESSED		0x0020df07
#define IR_RIO_TUNER_RELEASED		0x8020df07
#define IR_RIO_7_PRESSED		0x0020df08
#define IR_RIO_7_RELEASED		0x8020df08
#define IR_RIO_8_PRESSED		0x0020df09
#define IR_RIO_8_RELEASED		0x8020df09
#define IR_RIO_9_PRESSED		0x0020df0a
#define IR_RIO_9_RELEASED		0x8020df0a
#define IR_RIO_SELECTMODE_PRESSED	0x0020df0b
#define IR_RIO_SELECTMODE_RELEASED	0x8020df0b
#define IR_RIO_CANCEL_PRESSED		0x0020df0c
#define IR_RIO_CANCEL_RELEASED		0x8020df0c
#define IR_RIO_0_PRESSED		0x0020df0d
#define IR_RIO_0_RELEASED		0x8020df0d
#define IR_RIO_SEARCH_PRESSED		0x0020df0e
#define IR_RIO_SEARCH_RELEASED		0x8020df0e
#define IR_RIO_SOUND_PRESSED		0x0020df0f
#define IR_RIO_SOUND_RELEASED		0x8020df0f
#define IR_RIO_PREVTRACK_PRESSED	0x0020df10
#define IR_RIO_PREVTRACK_RELEASED	0x8020df10
#define IR_RIO_NEXTTRACK_PRESSED	0x0020df11
#define IR_RIO_NEXTTRACK_RELEASED	0x8020df11
#define IR_RIO_MENU_PRESSED		0x0020df12
#define IR_RIO_MENU_RELEASED		0x8020df12
#define IR_RIO_VOLPLUS_PRESSED		0x0020df13
#define IR_RIO_VOLPLUS_RELEASED		0x8020df13
#define IR_RIO_INFO_PRESSED		0x0020df14
#define IR_RIO_INFO_RELEASED		0x8020df14
#define IR_RIO_VISUAL_PRESSED		0x0020df15
#define IR_RIO_VISUAL_RELEASED		0x8020df15
#define IR_RIO_PLAY_PRESSED		0x0020df16
#define IR_RIO_PLAY_RELEASED		0x8020df16
#define IR_RIO_VOLMINUS_PRESSED		0x0020df17
#define IR_RIO_VOLMINUS_RELEASED	0x8020df17

// New in v2-rc1
#define IR_PREV_VISUAL_PRESSED		0x0020df18
#define IR_PREV_VISUAL_RELEASED		0x8020df18

// Rio buttons with alternate names
#define IR_RIO_SHUFFLE_PRESSED		IR_RIO_0_PRESSED
#define IR_RIO_SHUFFLE_RELEASED		IR_RIO_0_RELEASED
#define IR_RIO_REPEAT_PRESSED		IR_RIO_7_PRESSED
#define IR_RIO_REPEAT_RELEASED		IR_RIO_7_RELEASED
#define IR_RIO_EQUALIZER_PRESSED	IR_RIO_SOUND_PRESSED
#define IR_RIO_EQUALIZER_RELEASED	IR_RIO_SOUND_RELEASED
#define IR_RIO_MARK_PRESSED		IR_RIO_CANCEL_PRESSED
#define IR_RIO_MARK_RELEASED		IR_RIO_CANCEL_RELEASED
#define IR_RIO_PAUSE_PRESSED		IR_RIO_PLAY_PRESSED
#define IR_RIO_PAUSE_RELEASED		IR_RIO_PLAY_RELEASED
#define IR_RIO_HUSH_PRESSED		IR_RIO_PLAY_PRESSED
#define IR_RIO_HUSH_RELEASED		IR_RIO_PLAY_RELEASED

// Button code range for Stalk:
#define IR_STALK_SHIFTED	0x00000020
#define IR_STALK_MASK		(IR_STALK_SHIFTED^0x00fffff0)
#define IR_STALK_MATCH		0x00000010

// Un-Shifted button codes for Stalk:
#define IR_KOFF_PRESSED		0x00000010
#define IR_KSOURCE_PRESSED	0x00000011
#define IR_KATT_PRESSED		0x00000012
#define IR_KFRONT_PRESSED	0x00000013
#define IR_KNEXT_PRESSED	0x00000014
#define IR_KPREV_PRESSED	0x00000015
#define IR_KVOLUP_PRESSED	0x00000016
#define IR_KVOLDOWN_PRESSED	0x00000017
#define IR_KREAR_PRESSED	0x00000018
#define IR_KBOTTOM_PRESSED	0x00000019

#define IR_KOFF_RELEASED	0x80000010
#define IR_KSOURCE_RELEASED	0x80000011
#define IR_KATT_RELEASED	0x80000012
#define IR_KFRONT_RELEASED	0x80000013
#define IR_KNEXT_RELEASED	0x80000014
#define IR_KPREV_RELEASED	0x80000015
#define IR_KVOLUP_RELEASED	0x80000016
#define IR_KVOLDOWN_RELEASED	0x80000017
#define IR_KREAR_RELEASED	0x80000018
#define IR_KBOTTOM_RELEASED	0x80000019

// Shifted button codes for Stalk:
#define IR_KSOFF_PRESSED	(IR_STALK_SHIFTED|IR_KOFF_PRESSED)
#define IR_KSSOURCE_PRESSED	(IR_STALK_SHIFTED|IR_KSOURCE_PRESSED)
#define IR_KSATT_PRESSED	(IR_STALK_SHIFTED|IR_KATT_PRESSED)
#define IR_KSFRONT_PRESSED	(IR_STALK_SHIFTED|IR_KFRONT_PRESSED)
#define IR_KSNEXT_PRESSED	(IR_STALK_SHIFTED|IR_KNEXT_PRESSED)
#define IR_KSPREV_PRESSED	(IR_STALK_SHIFTED|IR_KPREV_PRESSED)
#define IR_KSVOLUP_PRESSED	(IR_STALK_SHIFTED|IR_KVOLUP_PRESSED)
#define IR_KSVOLDOWN_PRESSED	(IR_STALK_SHIFTED|IR_KVOLDOWN_PRESSED)
#define IR_KSREAR_PRESSED	(IR_STALK_SHIFTED|IR_KREAR_PRESSED)
#define IR_KSBOTTOM_PRESSED	(IR_STALK_SHIFTED|IR_KBOTTOM_PRESSED)

#define IR_KSOFF_RELEASED	(IR_STALK_SHIFTED|IR_KOFF_RELEASED)
#define IR_KSSOURCE_RELEASED	(IR_STALK_SHIFTED|IR_KSOURCE_RELEASED)
#define IR_KSATT_RELEASED	(IR_STALK_SHIFTED|IR_KATT_RELEASED)
#define IR_KSFRONT_RELEASED	(IR_STALK_SHIFTED|IR_KFRONT_RELEASED)
#define IR_KSNEXT_RELEASED	(IR_STALK_SHIFTED|IR_KNEXT_RELEASED)
#define IR_KSPREV_RELEASED	(IR_STALK_SHIFTED|IR_KPREV_RELEASED)
#define IR_KSVOLUP_RELEASED	(IR_STALK_SHIFTED|IR_KVOLUP_RELEASED)
#define IR_KSVOLDOWN_RELEASED	(IR_STALK_SHIFTED|IR_KVOLDOWN_RELEASED)
#define IR_KSREAR_RELEASED	(IR_STALK_SHIFTED|IR_KREAR_RELEASED)
#define IR_KSBOTTOM_RELEASED	(IR_STALK_SHIFTED|IR_KBOTTOM_RELEASED)

// These are just for in-kernel use:
typedef struct tm_s
{
	int tm_sec;	/* seconds	*/
	int tm_min;	/* minutes	*/
	int tm_hour;	/* hours	*/
	int tm_mday;	/* day of month */
	int tm_mon;	/* month	*/
	int tm_year;	/* full year	*/
	int tm_wday;	/* day of week	*/
	int tm_yday;	/* days in year */
} tm_t;

#endif // KERNEL_HIJACK_H
