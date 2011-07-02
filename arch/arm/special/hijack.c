// Empeg hacks by Mark Lord <mlord@pobox.com>
//
#define HIJACK_VERSION	"v511"
const char hijack_vXXX_by_Mark_Lord[] = "Hijack "HIJACK_VERSION" by Mark Lord";

#undef EMPEG_FIXTEMP	// #define this for special "fix temperature sensor" builds

// mainline code is in hijack_handle_display() way down in this file

#define __KERNEL_SYSCALLS__
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/major.h>
#include <linux/mm.h>
#include <linux/malloc.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/kernel_stat.h>
#include <linux/unistd.h>
#include <linux/dirent.h>
#include <linux/random.h>

#include <linux/empeg.h>
#include <asm/uaccess.h>

#include <asm/arch/hijack.h>		// for ioctls, IR_ definitions, etc..
#include <linux/soundcard.h>		// for SOUND_MASK_*
#include "../../../drivers/block/ide.h"	// for ide_hwifs[]
#include "empeg_display.h"
#include "empeg_mixer.h"

extern unsigned char empeg_ani[];					// arch/arm/special/empeg_display.c
extern unsigned char nohd_img[];					// arch/arm/special/empeg_display.c
extern int hijack_exec(const char *, const char *);			// arch/arm/special/kexec.c
extern void *hijack_get_state_read_buffer (void);			// arch/arm/special/empeg_state.c
extern void save_current_volume(void);					// arch/arm/special/empeg_state.c
extern void input_wakeup_waiters(void);					// arch/arm/special/empeg_input.c
extern int display_sendcontrol_part1(int);				// arch/arm/special/empeg_display.c
extern int display_sendcontrol_part2(int);				// arch/arm/special/empeg_display.c
extern void display_animation_frame(unsigned char *buf, unsigned char *frame); // arch/arm/special/empeg_display.c

extern int remount_drives (int writeable);				// arch/arm/special/notify.c
extern void init_notify (void);						// arch/arm/special/notify.c
extern int sys_sync(void);						// fs/buffer.c
extern int get_loadavg(char * buffer);					// fs/proc/array.c
extern void machine_restart(void *);					// arch/arm/kernel/process.c
extern int empeg_state_dirty;						// arch/arm/special/empeg_state.c
extern void state_cleanse(void);					// arch/arm/special/empeg_state.c
extern void hijack_voladj_intinit(int, int, int, int, int);		// arch/arm/special/empeg_audio3.c
extern void hijack_beep (int pitch, int duration_msecs, int vol_percent);// arch/arm/special/empeg_audio3.c
extern unsigned long jiffies_since(unsigned long past_jiffies);		// arch/arm/special/empeg_input.c
extern void display_blat(struct display_dev *dev, unsigned char *source_buffer); // empeg_display.c
extern tm_t *hijack_convert_time(time_t, tm_t *);			// from arch/arm/special/notify.c
static void remove_menu_entry (const char *label);

extern void empeg_mixer_select_input(int input);			// arch/arm/special/empeg_mixer.c
extern void hijack_tone_set(int, int, int, int, int, int);					// arch/arm/special/empeg_mixer.c
extern int empeg_readtherm(volatile unsigned int *timerbase, volatile unsigned int *gpiobase);	// arch/arm/special/empeg_therm.S
extern int empeg_readtherm_status(volatile unsigned int *timerbase, volatile unsigned int *gpiobase);	// arch/arm/special/empeg_therm.S
extern int empeg_inittherm(volatile unsigned int *timerbase, volatile unsigned int *gpiobase);	// arch/arm/special/empeg_therm.S
extern int empeg_fixtherm(volatile unsigned int *timerbase, volatile unsigned int *gpiobase);	// arch/arm/special/empeg_therm.S
       int display_ioctl (struct inode *inode, struct file *filp, unsigned int cmd, unsigned long arg); //arch/arm/special/empeg_display.c
       int get_number (unsigned char **src, int *target, unsigned int base, const char *nextchars);
extern int hijack_current_mixer_input;

#ifdef CONFIG_HIJACK_TUNER	// Mk2 or later? (Mk1 has no ethernet chip)
#define EMPEG_KNOB_SUPPORTED	// Mk2 and later have a front-panel knob
#define EMPEG_STALK_SUPPORTED	// Mk2 and later have a front-panel knob
extern void hijack_serial_rx_insert (const char *buf, int size, int port); // drivers/char/serial_sa1100.c
#endif

int	hijack_cs4231a_failed;		// non-zero for a failed cs4231a chip
int	hijack_loopback;		// 1 == detected docked "loopback" mode
int	kenwood_disabled;		// used by Nextsrc button
int	empeg_on_dc_power;		// used in arch/arm/special/empeg_power.c
int	empeg_tuner_present = 0;	// used in many places
int	hijack_volumelock_enabled = 0;	// used by arch/arm/special/empeg_state.c
int	hijack_fsck_disabled = 0;	// used in fs/ext2/super.c
int	hijack_onedrive = 0;		// used in drivers/block/ide-probe.c
int	hijack_saveserial = 0;		// set to "1" to pass "-s-" to player on startup
int	hijack_reboot = 0;		// set to "1" to cause reboot on next display refresh
int	hijack_force_pause_player = 0;	// set to "1" to force PAUSE on next player startup
pid_t	hijack_player_pid;		// set in fs/exec.c
pid_t	hijack_player_config_ini_pid;	// used in fs/read_write.c, fs/exec.c
unsigned int hijack_player_started = 0;	// set to jiffies when player startup is detected on serial port (notify.c)
static unsigned char *last_player_buf;

static unsigned int PROMPTCOLOR = COLOR3, ENTRYCOLOR = -COLOR3;

#define NEED_REFRESH		0
#define NO_REFRESH		1
#define SHOW_PLAYER		2

#define HIJACK_IDLE		0
#define HIJACK_IDLE_PENDING	1
#define HIJACK_ACTIVE_PENDING	2
#define HIJACK_ACTIVE		3

static unsigned int carvisuals_enabled = 0;
static unsigned int hijack_status = HIJACK_IDLE;
static unsigned long hijack_last_moved = 0, hijack_last_refresh = 0, blanker_triggered = 0, blanker_lastpoll = 0;
static unsigned char blanker_lastbuf[EMPEG_SCREEN_BYTES] = {0,};
const unsigned char hexchars[] = "0123456789ABCDEFabcdef";

static int  (*hijack_dispfunc)(int) = NULL;
static void (*hijack_movefunc)(int) = NULL;
static unsigned long hijack_userdata = 0;

// This is broken.. we have run out of bits here because much of this
//  because most of these flags really need to be processed by handle_buttons()
//  rather than by input_append_code().  To fix it, we must defer translations
//  to later in the chain somehow..
#define BUTTON_FLAGS_LONGPRESS	(0x80000000)	// send this out as a long press
#define BUTTON_FLAGS_SHIFT	(0x40000000)	// toggle shift state when sending
#define BUTTON_FLAGS_UI		(0x20000000)	// send only if player menus are idle
#define BUTTON_FLAGS_NOTUI	(0x10000000)	// send only if player menus are active
#define BUTTON_FLAGS_ALTNAME	(0x08000000)	// use alternate name when displaying
#define BUTTON_FLAGS_SOUNDADJ	(0x04000000)	// send only if player Vol/Loud/Bal/Fader popup is active
#define BUTTON_FLAGS_UISTATE	(BUTTON_FLAGS_UI|BUTTON_FLAGS_NOTUI|BUTTON_FLAGS_SOUNDADJ)
#define BUTTON_FLAGS		(0xff000000)
#define IR_NULL_BUTTON		(~BUTTON_FLAGS)

#define LONGPRESS_DELAY		(HZ+(HZ/3))	// delay between press/release for emulated longpresses

#ifdef EMPEG_STALK_SUPPORTED
// Sony Stalk packets look like this:
//
//	0x02 0x0S 0xBB 0xCC
//
// First byte is always 0x02.
// Second byte is either 0x00 (unshifted) or 0x01 (shifted).
// Third byte is actual button A/D measurement; 0xff == no button pressed.
// Fourth byte is packet checksum; sum of second + third bytes.
//
// Example:
//
// 02 01 ff 00 == shift button being pressed
// 02 01 2b 2c == ATT button pressed while shifted
// 02 01 ff 00 == no button pressed, shift still active (ATT got released)
// 02 00 ff ff == all buttons released
//
// Trivia: command to adjust the A/D conversions, which normally won't occur
// until the reading has "stabilized", as follows:  0x01 0x08 <fuzz> <loops>
// where <fuzz> is the +/- factor and <loops> specifies trips round the
// main A/D loop before it gets sent.  Reply will be 0x01 0x08 0x00 0x08.
//
// The button order in the stalk tables below is FIXED; do not modify it!!
// Also, the hijack_option_table[] depends upon this order.

static const unsigned int stalk_buttons[] = {
	IR_KOFF_PRESSED,
	IR_KSOURCE_PRESSED,
	IR_KATT_PRESSED,
	IR_KFRONT_PRESSED,
	IR_KNEXT_PRESSED,
	IR_KPREV_PRESSED,
	IR_KVOLUP_PRESSED,
	IR_KVOLDOWN_PRESSED,
	IR_KREAR_PRESSED,
	IR_KBOTTOM_PRESSED,
	IR_NULL_BUTTON};

typedef struct min_max_s {
	int	min;
	int	max;
} min_max_t;

       int hijack_stalk_enabled = 0;	// used to ignore stalk input when in standby
static unsigned char most_recent_stalk_code;
static int stalk_on_left = 0;
static min_max_t rhs_stalk_vals[11];	// 10 sets of values, followed by {-1,-1} terminator.
static min_max_t lhs_stalk_vals[11];	// 10 sets of values, followed by {-1,-1} terminator.

static min_max_t rhs_stalk_default[] = {
	{0x00, 0x07},	// IR_KOFF_PRESSED
	{0x10, 0x1c},	// IR_KSOURCE_PRESSED
	{0x24, 0x30},	// IR_KATT_PRESSED
	{0x34, 0x40},	// IR_KFRONT_PRESSED
	{0x42, 0x4e},	// IR_KNEXT_PRESSED
	{0x54, 0x60},	// IR_KPREV_PRESSED
	{0x68, 0x7a},	// IR_KVOLUP_PRESSED
	{0x7e, 0x8a},	// IR_KVOLDOWN_PRESSED
	{0x94, 0xa0},	// IR_KREAR_PRESSED
	{0xa1, 0xb5}};	// IR_KBOTTOM_PRESSED

static min_max_t lhs_stalk_default[] = {
	{0x00, 0x07},	// IR_KOFF_PRESSED
	{0x10, 0x1c},	// IR_KSOURCE_PRESSED
	{0x24, 0x30},	// IR_KATT_PRESSED
	{0x34, 0x40},	// IR_KREAR_PRESSED
	{0x42, 0x4e},	// IR_KPREV_PRESSED
	{0x54, 0x60},	// IR_KNEXT_PRESSED
	{0x68, 0x7a},	// IR_KVOLDOWN_PRESSED
	{0x7e, 0x8a},	// IR_KVOLUP_PRESSED
	{0x94, 0xa0},	// IR_KFRONT_PRESSED
	{0xa1, 0xb5}};	// IR_KBOTTOM_PRESSED
#endif // EMPEG_STALK_SUPPORTED

static unsigned long ir_lastevent = 0, ir_lasttime = 0, ir_selected = 0;
static unsigned int ir_releasewait = IR_NULL_BUTTON, ir_trigger_count = 0;;
static unsigned long ir_menu_down = 0, ir_left_down = 0, ir_right_down = 0;
static unsigned long ir_move_repeat_delay, ir_shifted = 0;
static int *ir_numeric_input = NULL;

// Qualifier flags for ir_tranlsate matches
//
#define IR_FLAGS_LONGPRESS	0x0001	// only if pressed >= 1 second
#define IR_FLAGS_CAR		0x0002	// on DC power
#define IR_FLAGS_HOME		0x0004	// on AC power
#define IR_FLAGS_SHIFTED	0x0008	// "shift" state is set
#define IR_FLAGS_NOTSHIFTED	0x0010	// "shift" state is not set
#define IR_FLAGS_TUNER		0x0020	// Tuner is active
#define IR_FLAGS_AUX		0x0040	// Aux (line-in) is active
#define IR_FLAGS_MAIN		0x0080	// Main/mp3/pcm/dsp is active
#define IR_FLAGS_POPUP		0x0100	// This is a PopUp menu (not a regular translation)

#define IR_FLAGS_CARHOME	(IR_FLAGS_CAR|IR_FLAGS_HOME)
#define IR_FLAGS_SHIFTSTATE	(IR_FLAGS_SHIFTED|IR_FLAGS_NOTSHIFTED)
#define IR_FLAGS_MIXER		(IR_FLAGS_TUNER|IR_FLAGS_AUX|IR_FLAGS_MAIN)

static short ir_flag_defaults[] = {IR_FLAGS_SHIFTSTATE, IR_FLAGS_CARHOME, IR_FLAGS_MIXER, 0};

typedef struct ir_flags_s {
	unsigned char	symbol;
	unsigned char	filler;	// for 32-bit alignment
	unsigned short	flag;
} ir_flags_t;

static ir_flags_t ir_flags[] = {
	{'L', 0, IR_FLAGS_LONGPRESS},
	{'C', 0, IR_FLAGS_CAR},
	{'H', 0, IR_FLAGS_HOME},
	{'S', 0, IR_FLAGS_SHIFTED},
	{'N', 0, IR_FLAGS_NOTSHIFTED},
	{'T', 0, IR_FLAGS_TUNER},
	{'A', 0, IR_FLAGS_AUX},
	{'M', 0, IR_FLAGS_MAIN},
	{0,0,0 } };

typedef struct button_name_s {
	char		name[12];
	unsigned long	code;
} button_name_t;

#define IR_FAKE_INITIAL		(IR_NULL_BUTTON-1)
#define IR_FAKE_POPUP3		(IR_NULL_BUTTON-2)
#define IR_FAKE_POPUP2		(IR_NULL_BUTTON-3)
#define IR_FAKE_POPUP1		(IR_NULL_BUTTON-4)
#define IR_FAKE_POPUP0		(IR_NULL_BUTTON-5)
#define IR_FAKE_VOLADJ3		(IR_NULL_BUTTON-6)
#define IR_FAKE_VOLADJ2		(IR_NULL_BUTTON-7)
#define IR_FAKE_VOLADJ1		(IR_NULL_BUTTON-8)
#define IR_FAKE_VOLADJOFF	(IR_NULL_BUTTON-9)
#define IR_FAKE_VOLADJMENU	(IR_NULL_BUTTON-10)
#define IR_FAKE_KNOBSEEK	(IR_NULL_BUTTON-11)
#define IR_FAKE_CLOCK		(IR_NULL_BUTTON-12)
#define IR_FAKE_NEXTSRC		(IR_NULL_BUTTON-13)
#define IR_FAKE_BASSADJ		(IR_NULL_BUTTON-14)
#define IR_FAKE_TREBLEADJ	(IR_NULL_BUTTON-15)
#define IR_FAKE_QUICKTIMER	(IR_NULL_BUTTON-16)
#define IR_FAKE_VISUALSEEK	(IR_NULL_BUTTON-17)
#define IR_FAKE_SAVESRC		(IR_NULL_BUTTON-18)
#define IR_FAKE_SAVEAUX		(IR_NULL_BUTTON-19)
#define IR_FAKE_RESTORESRC	(IR_NULL_BUTTON-20)
#define IR_FAKE_AM		(IR_NULL_BUTTON-21)
#define IR_FAKE_FM		(IR_NULL_BUTTON-22)
#define IR_FAKE_REBOOT		(IR_NULL_BUTTON-23)
#define IR_FAKE_FIDENTRY	(IR_NULL_BUTTON-24)
#define IR_FAKE_HIJACKMENU	(IR_NULL_BUTTON-25)	// This MUST be the lowest numbered FAKE code
#define ALT			BUTTON_FLAGS_ALTNAME

typedef struct ir_translation_s {
	unsigned int	old;		// original button, if known
	unsigned short	flags;		// boolean flags
	unsigned char	count;		// how many codes in new[]
	unsigned char	popup_index;	// for PopUp translations only: most recent menu position
	unsigned int	new[0];		// start of macro table with replacement buttons to send
} ir_translation_t;

// special set-up for the SeekTool:
static struct {
		ir_translation_t	hdr;
		unsigned int		buttons[2];
	} seektool_default_translation =
	{	{IR_NULL_BUTTON, 0, 2, 0},
		{IR_FAKE_POPUP0, IR_KNOB_PRESSED|ALT}
	};

// a default translation for PopUp0 menu:
static struct {
		ir_translation_t	hdr;
		unsigned int		buttons[16];
	} popup0_default_translation =
	{	{IR_FAKE_POPUP0, 0, 16, 0},
		{IR_FAKE_CLOCK,
		IR_RIO_INFO_PRESSED|ALT|BUTTON_FLAGS_LONGPRESS,	// "Detail"
		IR_FAKE_FIDENTRY,
		IR_RIO_PLAY_PRESSED|ALT|BUTTON_FLAGS_LONGPRESS,	// "Hush"
		IR_RIO_INFO_PRESSED,
		IR_KNOB_PRESSED,
		IR_FAKE_KNOBSEEK,
		IR_RIO_MARK_PRESSED|ALT,			// "Mark"
		IR_FAKE_NEXTSRC,
		IR_FAKE_QUICKTIMER,
		IR_RIO_SELECTMODE_PRESSED,
		IR_RIO_0_PRESSED|ALT,				// "Shuffle"
		IR_RIO_SOURCE_PRESSED,
		IR_RIO_TUNER_PRESSED,
		IR_FAKE_VISUALSEEK,
		IR_FAKE_VOLADJMENU}
	};

static ir_translation_t *ir_current_longpress = NULL;
static unsigned int *ir_translate_table = NULL;

// Fixme (someday): serial-port-"w" == "pause" (not pause/play): create a fake button for this.

static button_name_t button_names[] = {
	{"PopUp0",	IR_FAKE_POPUP0},	// index 0 assumed later in hijack_option_table[]
	{"PopUp1",	IR_FAKE_POPUP1},	// index 1 assumed later in hijack_option_table[]
	{"PopUp2",	IR_FAKE_POPUP2},	// index 2 assumed later in hijack_option_table[]
	{"PopUp3",	IR_FAKE_POPUP3},	// index 3 assumed later in hijack_option_table[]
	{"VolAdjLow",	IR_FAKE_VOLADJ1},	// index 4 assumed later in hijack_option_table[]
	{"VolAdjMed",	IR_FAKE_VOLADJ2},	// index 5 assumed later in hijack_option_table[]
	{"VolAdjHigh",	IR_FAKE_VOLADJ3},	// index 6 assumed later in hijack_option_table[]
	{"BassAdj",	IR_FAKE_BASSADJ},
	{"TrebleAdj",	IR_FAKE_TREBLEADJ},
	{"VolAdjOff",	IR_FAKE_VOLADJOFF},
	{"KnobSeek",	IR_FAKE_KNOBSEEK},
	{"Clock",	IR_FAKE_CLOCK},
	{"NextSrc",	IR_FAKE_NEXTSRC},
	{"SaveSrc",	IR_FAKE_SAVESRC},
	{"SaveAux",	IR_FAKE_SAVEAUX},
	{"RestoreSrc",	IR_FAKE_RESTORESRC},
	{"AM",		IR_FAKE_AM},
	{"FM",		IR_FAKE_FM},
	{"FidEntry",	IR_FAKE_FIDENTRY},
	{"Reboot",	IR_FAKE_REBOOT},
	{"VolAdj",	IR_FAKE_VOLADJMENU},
	{"QuickTimer",	IR_FAKE_QUICKTIMER},
	{"HijackMenu",	IR_FAKE_HIJACKMENU},
	{"VisualSeek",	IR_FAKE_VISUALSEEK},

	{"Initial",	IR_FAKE_INITIAL},
	{"null",	IR_NULL_BUTTON},
	{"Source",	IR_RIO_SOURCE_PRESSED},
	{"Src",		IR_KW_SRC_PRESSED},	// the player s/w treats this similarly to our "NextSrc" button
	{"Power",	IR_RIO_SOURCE_PRESSED|ALT|BUTTON_FLAGS_LONGPRESS},
	{"Time",	IR_RIO_1_PRESSED|ALT},
	{"One",		IR_RIO_1_PRESSED},
	{"Artist",	IR_RIO_2_PRESSED|ALT},
	{"Two",		IR_RIO_2_PRESSED},
	{"Album",	IR_RIO_3_PRESSED|ALT},	// "Source"
	{"Three",	IR_RIO_3_PRESSED},
	{"Four",	IR_RIO_4_PRESSED},
	{"Genre",	IR_RIO_5_PRESSED|ALT},
	{"Five",	IR_RIO_5_PRESSED},
	{"Year",	IR_RIO_6_PRESSED|ALT},
	{"Six",		IR_RIO_6_PRESSED},
	{"Repeat",	IR_RIO_7_PRESSED|ALT},
	{"Seven",	IR_RIO_7_PRESSED},
	{"Swap",	IR_RIO_8_PRESSED|ALT},
	{"Eight",	IR_RIO_8_PRESSED},
	{"Title",	IR_RIO_9_PRESSED|ALT},
	{"Nine",	IR_RIO_9_PRESSED},
	{"Shuffle",	IR_RIO_0_PRESSED|ALT},
	{"Zero",	IR_RIO_0_PRESSED},
	{"Tuner",	IR_RIO_TUNER_PRESSED},
	{"SelMode",	IR_RIO_SELECTMODE_PRESSED},	// no "ALT" on this one!!
	{"SelectMode",	IR_RIO_SELECTMODE_PRESSED},
	{"Cancel",	IR_RIO_CANCEL_PRESSED},
	{"Mark",	IR_RIO_MARK_PRESSED|ALT},
	{"Search",	IR_RIO_SEARCH_PRESSED},
	{"Sound",	IR_RIO_SOUND_PRESSED},
	{"Equalizer",	IR_RIO_SOUND_PRESSED|ALT|BUTTON_FLAGS_LONGPRESS},
	{"PrevTrack",	IR_RIO_PREVTRACK_PRESSED},
	{"Prev",	IR_RIO_PREVTRACK_PRESSED|ALT},
	{"Track-",	IR_RIO_PREVTRACK_PRESSED},
	{"NextTrack",	IR_RIO_NEXTTRACK_PRESSED},
	{"Next",	IR_RIO_NEXTTRACK_PRESSED|ALT},
	{"Track+",	IR_RIO_NEXTTRACK_PRESSED},
	{"Ok",		IR_RIO_MENU_PRESSED|ALT},
	{"Menu",	IR_RIO_MENU_PRESSED},
	{"VolDown",	IR_RIO_VOLMINUS_PRESSED|ALT},
	{"Vol-",	IR_RIO_VOLMINUS_PRESSED},
	{"VolUp",	IR_RIO_VOLPLUS_PRESSED|ALT},
	{"Vol+",	IR_RIO_VOLPLUS_PRESSED},
	{"Vol ",	IR_RIO_VOLPLUS_PRESSED},	// for http "button=vol+", where '+' becomes a space..
	{"Detail",	IR_RIO_INFO_PRESSED|ALT|BUTTON_FLAGS_LONGPRESS},
	{"Info",	IR_RIO_INFO_PRESSED},
	{"Hush",	IR_RIO_PLAY_PRESSED|ALT|BUTTON_FLAGS_LONGPRESS},
	{"Play",	IR_RIO_PLAY_PRESSED|ALT},
	{"Pause",	IR_RIO_PLAY_PRESSED},

	{"Top",		IR_TOP_BUTTON_PRESSED},
	{"Bottom",	IR_BOTTOM_BUTTON_PRESSED},
	{"Left",	IR_LEFT_BUTTON_PRESSED},
	{"Right",	IR_RIGHT_BUTTON_PRESSED},
	{"KnobLeft",	IR_KNOB_LEFT},
	{"KnobRight",	IR_KNOB_RIGHT},
	{"Knob",	IR_KNOB_PRESSED},
	{"SeekTool",	IR_KNOB_PRESSED|ALT},

	{"AM-",		IR_KW_AM_PRESSED},
	{"FM+",		IR_KW_FM_PRESSED},
	{"Direct",	IR_KW_DIRECT_PRESSED},
	{"*",		IR_KW_STAR_PRESSED|ALT},
	{"Star",	IR_KW_STAR_PRESSED},
	{"Radio",	IR_KW_TUNER_PRESSED},
	{"Auxiliary",	IR_KW_TAPE_PRESSED|ALT},
	{"Tape",	IR_KW_TAPE_PRESSED},
	{"Player",	IR_KW_CD_PRESSED|ALT},
	{"CD",		IR_KW_CD_PRESSED},
	{"CDMDCH",	IR_KW_CDMDCH_PRESSED},
	{"DNPP",	IR_KW_DNPP_PRESSED},

#ifdef EMPEG_STALK_SUPPORTED
	{"KOff",	IR_KOFF_PRESSED},	// Stalk
	{"KSource",	IR_KSOURCE_PRESSED},	// Stalk
	{"KAtt",	IR_KATT_PRESSED},	// Stalk
	{"KFront",	IR_KFRONT_PRESSED},	// Stalk
	{"KNext",	IR_KNEXT_PRESSED},	// Stalk
	{"KPrev",	IR_KPREV_PRESSED},	// Stalk
	{"KVolUp",	IR_KVOLUP_PRESSED},	// Stalk
	{"KVolDown",	IR_KVOLDOWN_PRESSED},	// Stalk
	{"KRear",	IR_KREAR_PRESSED},	// Stalk
	{"KBottom",	IR_KBOTTOM_PRESSED},	// Stalk

	{"KSOff",	IR_KSOFF_PRESSED},	// Stalk
	{"KSSource",	IR_KSSOURCE_PRESSED},	// Stalk
	{"KSAtt",	IR_KSATT_PRESSED},	// Stalk
	{"KSFront",	IR_KSFRONT_PRESSED},	// Stalk
	{"KSNext",	IR_KSNEXT_PRESSED},	// Stalk
	{"KSPrev",	IR_KSPREV_PRESSED},	// Stalk
	{"KSVolUp",	IR_KSVOLUP_PRESSED},	// Stalk
	{"KSVolDown",	IR_KSVOLDOWN_PRESSED},	// Stalk
	{"KSRear",	IR_KSREAR_PRESSED},	// Stalk
	{"KSBottom",	IR_KSBOTTOM_PRESSED},	// Stalk
#endif // EMPEG_STALK_SUPPORTED

	{"NextVisual",	IR_RIO_VISUAL_PRESSED|ALT},
	{"PrevVisual",	IR_PREV_VISUAL_PRESSED},// v2-rc1
	{"Visual",	IR_RIO_VISUAL_PRESSED},

	{"\0",		IR_NULL_BUTTON}		// end-of-table-marker
	};
#undef ALT

#define KNOBDATA_SIZE	8
#define KNOBDATA_MASK	(KNOBDATA_SIZE - 1)
#define KNOBDATA_BITS	4
#define POPUP0_MASK	((1 << KNOBDATA_BITS) - 1)
static int knobdata_index = 0;
static int popup0_index = 0;		// (PopUp0) saved/restored index
static int hijack_knobseek = 0;

#ifdef EMPEG_KNOB_SUPPORTED

static unsigned long ir_knob_busy = 0, ir_knob_down = 0;

// Mmm.. this *could* be eliminated entirely, in favour of IR-translations and PopUp's..
// But for now, we leave it in because it is (1) a Major convenience, and (2) can be modified "on the fly".
static const char *knobdata_labels[KNOBDATA_SIZE] =
	{"[default]", button_names[0].name, "VolAdj+", "Details", "Info", "Mark", "Shuffle", "NextSrc"};
static const unsigned int knobdata_buttons[KNOBDATA_SIZE] = {
	IR_KNOB_PRESSED,
	IR_FAKE_POPUP0,
	IR_KNOB_PRESSED,
	IR_RIO_INFO_PRESSED|BUTTON_FLAGS_LONGPRESS,
	IR_RIO_INFO_PRESSED,
	IR_RIO_MARK_PRESSED,
	IR_RIO_SHUFFLE_PRESSED,
	IR_FAKE_NEXTSRC,
	};

static	int hijack_option_stalk_enabled;	// 0 == ignore (drop) all stalk input
#endif // EMPEG_KNOB_SUPPORTED

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
// This method has to change: we need to be able to select translations based
// upon CURRENT player menu state, which is unavailable in the interrupt routines.
// To fix it, we need to defer translations to the point where buttons are pulled
// from the inputq.. where we know the player state, and have more realtime margin.
// For now, though, it continues to hobble along, and is very easy to break.
//
// The flow is slightly different for userland apps which hijack buttons:
//
// button -> interrupt -> input_append_code() -> inputq -> hijack_handle_button() -> userq -> ioctl()
//
// Currently, userq[] does not hold useful timing information

#define HIJACK_BUTTONQ_SIZE	48
typedef struct hijack_buttondata_s {
	unsigned long delay;	// inter-button delay interval
	unsigned int button;	// button press/release code
} hijack_buttondata_t;

typedef struct hijack_buttonq_s {
	unsigned long		last_deq;
	unsigned short		head;
	unsigned short		tail;
	unsigned char		qname;
	hijack_buttondata_t	data[HIJACK_BUTTONQ_SIZE];
} hijack_buttonq_t;

// Automatic volume adjustment parameters
#define MULT_POINT		12
#define MULT_MASK		((1 << MULT_POINT) - 1)
#define VOLADJ_THRESHSIZE	16
#define VOLADJ_HISTSIZE		128	/* must be a power of two */
#define VOLADJ_FIXEDPOINT(whole,fraction) ((((whole)<<MULT_POINT)|((unsigned int)((fraction)*(1<<MULT_POINT))&MULT_MASK)))
#define VOLADJ_BITS 2

int hijack_voladj_enabled = 0;	// used by voladj code in empeg_audio3.c
int hijack_delaytime = 0;	// used by delay code in empeg_audio3.c
static const char  *voladj_names[] = {"[Off]", "Low", "Medium", "High"};
static unsigned int voladj_history[VOLADJ_HISTSIZE] = {0,}, voladj_last_histx = 0, voladj_histx = 0;
static unsigned int hijack_voladj_parms[(1<<VOLADJ_BITS)-1][5];

// Default VolAdj parms courtesy of tfabris April-2002
static int voladj_ldefault[] = {0x1500,	 400,	0x1000,	200,	1100}; // Low
static int voladj_mdefault[] = {0x1700,	 600,	0x1000,	200,	 600}; // Medium
static int voladj_hdefault[] = {0x2200,	1000,	0x1000,	100,	 500}; // High

#ifdef CONFIG_NET_ETHERNET
struct semaphore hijack_kxxxd_startup_sem	= MUTEX_LOCKED; // sema for starting daemons after we read config.ini
#endif // CONFIG_NET_ETHERNET
struct semaphore hijack_menuexec_sem		= MUTEX_LOCKED;	// sema for waking up menuxec when we issue a command

static hijack_buttonq_t hijack_inputq, hijack_playerq, hijack_userq;
int hijack_khttpd_new_fid_dirs;			// 0 == don't look for new fids sub-directories

// Externally tuneable parameters for config.ini; the voladj_parms are also tuneable
//
static	int hijack_buttonled_off_level;		// button brightness when player is "off"
static	int hijack_buttonled_dim_level;		// when non-zero, button brightness when headlights are on
static	int hijack_dc_servers;			// 1 == allow kftpd/khttpd when on DC power
static	int hijack_decimal_fidentry;		// 1 == fidentry uses base10 instead of hex
	int hijack_disable_emplode;		// 1 == block TCP port 8300 (Emplode/Emptool)
	int hijack_extmute_off;			// buttoncode to inject when EXT-MUTE goes inactive
	int hijack_extmute_on;			// buttoncode to inject when EXT-MUTE goes active
	int hijack_ir_debug;			// printk() for every ir press/release code
static	int hijack_spindown_seconds;		// drive spindown timeout in seconds
	int hijack_fake_tuner;			// pretend we have a tuner, when we really don't have one
	int hijack_trace_tuner;			// dump incoming tuner/stalk packets onto console
#ifdef EMPEG_STALK_SUPPORTED
static	int hijack_stalk_debug;			// trace button in/out actions to/from Stalk?
#endif // EMPEG_STALK_SUPPORTED
#ifdef CONFIG_NET_ETHERNET
	char hijack_kftpd_password[16];		// kftpd password
	char hijack_khttpd_basic[20];		// khttpd "user:password" for basic web streaming
	char hijack_khttpd_full[20];		// khttpd "user:password" for unrestricted web access
	int hijack_kftpd_control_port;		// kftpd control port
	int hijack_kftpd_data_port;		// kftpd data port
	int hijack_kftpd_verbose;		// kftpd verbosity
	int hijack_rootdir_dotdot;		// 1 == show '..' in rootdir listings
	int hijack_kftpd_show_dotfiles;		// 1 == show '.*' in rootdir listings
	int hijack_khttpd_show_dotfiles;	// 1 == show '.*' in rootdir listings
	int hijack_max_connections;		// restricts memory use
	int hijack_khttpd_port;			// khttpd port
	int hijack_khttpd_verbose;		// khttpd verbosity
	int hijack_ktelnetd_port;		// ktelnetd port
#endif // CONFIG_NET_ETHERNET
static	int nextsrc_aux_enabled;		// "1" == include "AUX" when doing NextSrc
static	int hijack_old_style;			// 1 == don't highlite menu items
static	int hijack_quicktimer_minutes;		// increment size for quicktimer function
static	int hijack_standby_minutes;		// number of minutes after screen blanks before we go into standby
	int hijack_silent;			// 1 == avoid printing messages to serial port
	int hijack_suppress_notify;		// 1 == suppress player "notify" and "dhcp" text on serial port
	long hijack_time_offset;		// adjust system time-of-day clock by this many seconds
	int hijack_temperature_correction;	// adjust all h/w temperature readings by this celcius amount
	int hijack_trace_fs;			// trace major filesystem accesses, on serial console
	int hijack_standbyLED_on, hijack_standbyLED_off;	// on/off duty cycle for standby LED
static	int hijack_keypress_flash;		// flash display when buttons are pressed
	unsigned int hijack_visuals_gain;	// gain factor for cs4321a sampling (FM & AUX)

void show_message (const char *message, unsigned long time);
static unsigned int hijack_menuexec_no;
static const char *no_yes[2] = {"No ", "Yes"};
static char *hijack_menuexec_command;

// Bass Treble Adjustment stuff follows  --genixia

static int hijack_bass_freq;            	// Sets center frequency for Bass Adjustment
static int hijack_bass_q;			// Sets Q factor for Bass Adjustment
static int hijack_bass_adj;			// Sets gain for Bass Adjustment
static int hijack_treble_freq;          	// Sets center frequency for Bass Adjustment
static int hijack_treble_adj;			// Sets gain for Treble Adjustment
static int hijack_treble_q;			// Sets Q factor for Treble Adjustment

// dB LUT used by Bass/Treble Adjustments
typedef struct hijack_db_table_s {
	char		name[8];
	unsigned int	value;
} hijack_db_table_t;

const hijack_db_table_t hijack_db_table[] =
{
	// No easy way to calculate logs at runtime. Small table solves a lot of problems.
	// Some of these values have mathematical errors - bit 0 must be unset for some reason.
	// dB   , actual ratio.
	{"[Off]"	, 64	},	// "64" is "special":  hijack_tone_set() recognizes/treats it as "flat".

	{"+1.0 dB"	, 72	},
	{"+2.0 dB"	, 82	},
	{"+3.0 dB"	, 90	},
	{"+4.0 dB"	, 102	},
	{"+5.0 dB"	, 114	},
	{"+6.0 dB"	, 128	},

	{"-6.0 dB"	, 32	},
	{"-5.0 dB"	, 36	},
	{"-4.0 dB"	, 40	},
	{"-3.0 dB"	, 44	},
	{"-2.0 dB"	, 50	},
	{"-1.0 dB"	, 56	},
};

// End Bass/Treble defs.

// Volume Boost values -- genixia

       int hijack_volboost[] = {0,0,0,0};	// {FM, MP3, AUX, AM}

// Tony bonus. Give the option of removing default bass boost on FM  -- genixia
       int hijack_disable_bassboost_FM;

// audio overlay patch settings
       int hijack_overlay_bg_min = 0x00004000;
       int hijack_overlay_bg_max = 0x00010000;
       int hijack_overlay_bg_fadestep = 0x00000AAA;

#ifdef CONFIG_EMPEG_I2C_FAN_CONTROL
static int fan_control_enabled, fan_control_low, fan_control_high;
#endif // CONFIG_EMPEG_I2C_FAN_CONTROL

typedef struct hijack_option_s {
	char	*name;
	void	*target;
	int	defaultval;  // or (void *)
	int	num_items;
	int	min;
	int	max;
} hijack_option_t;

char hijack_khttpd_style[64], hijack_khttpd_root_index[64];

static const hijack_option_t hijack_option_table[] =
{
// config.ini string		address-of-variable		default			howmany	min	max
//===========================	==========================	=========		=======	===	================
{"buttonled_off",		&hijack_buttonled_off_level,	1,			1,	0,	7},
{"buttonled_dim",		&hijack_buttonled_dim_level,	0,			1,	0,	7},
{"dc_servers",			&hijack_dc_servers,		0,			1,	0,	1},
{"decimal_fidentry",		&hijack_decimal_fidentry,	0,			1,	0,	1},
{"disable_emplode",		&hijack_disable_emplode,	0,			1,	0,	1},
{"spindown_seconds",		&hijack_spindown_seconds,	30,			1,	0,	(239 * 5)},
{"extmute_off",			&hijack_extmute_off,		0,			-1,	0,	IR_NULL_BUTTON},
{"extmute_on",			&hijack_extmute_on,		0,			-1,	0,	IR_NULL_BUTTON},
{"fake_tuner",			&hijack_fake_tuner,		0,			1,	0,	1},
#ifdef CONFIG_EMPEG_I2C_FAN_CONTROL
{"fan_control",			&fan_control_enabled,		0,			1,	0,	1},
{"fan_low",			&fan_control_low,		45,			1,	0,	100},
{"fan_high",			&fan_control_high,		50,			1,	0,	100},
#endif // CONFIG_EMPEG_I2C_FAN_CONTROL
{"ir_debug",			&hijack_ir_debug,		0,			1,	0,	1},
{"keypress_flash",		&hijack_keypress_flash,		0,			1,	0,	65535},
#ifdef CONFIG_NET_ETHERNET
{"kftpd_control_port",		&hijack_kftpd_control_port,	21,			1,	0,	65535},
{"kftpd_data_port",		&hijack_kftpd_data_port,	20,			1,	0,	65535},
{"kftpd_password",		&hijack_kftpd_password,		(int)"",		0,	0,	sizeof(hijack_kftpd_password)-1},
{"kftpd_verbose",		&hijack_kftpd_verbose,		0,			1,	0,	1},
{"rootdir_dotdot",		&hijack_rootdir_dotdot,		0,			1,	0,	1},
{"kftpd_show_dotfiles",		&hijack_kftpd_show_dotfiles,	0,			1,	0,	1},
{"khttpd_basic",		&hijack_khttpd_basic,		(int)"",		0,	0,	sizeof(hijack_khttpd_basic)-1},
{"khttpd_full",			&hijack_khttpd_full,		(int)"",		0,	0,	sizeof(hijack_khttpd_full)-1},
{"khttpd_show_dotfiles",	&hijack_khttpd_show_dotfiles,	1,			1,	0,	1},
{"khttpd_root_index",		&hijack_khttpd_root_index,	(int)"/index.html",	0,	0,	sizeof(hijack_khttpd_root_index)-1},
{"khttpd_port",			&hijack_khttpd_port,		80,			1,	0,	65535},
{"khttpd_style",		&hijack_khttpd_style,		(int)"/default.xsl",	0,	0,	sizeof(hijack_khttpd_style)-1},
{"khttpd_verbose",		&hijack_khttpd_verbose,		0,			1,	0,	2},
{"ktelnetd_port",		&hijack_ktelnetd_port,		0,			1,	0,	65535},
{"max_connections",		&hijack_max_connections,	4,			1,	0,	20},
#endif // CONFIG_NET_ETHERNET
{"nextsrc_aux_enabled",		&nextsrc_aux_enabled,		0,			1,	0,	1},
{"old_style",			&hijack_old_style,		0,			1,	0,	1},
{button_names[0].name,		button_names[0].name,		(int)"PopUp0",		0,	0,	8},
{button_names[1].name,		button_names[1].name,		(int)"PopUp1",		0,	0,	8},
{button_names[2].name,		button_names[2].name,		(int)"PopUp2",		0,	0,	8},
{button_names[3].name,		button_names[3].name,		(int)"PopUp3",		0,	0,	8},
{"quicktimer_minutes",		&hijack_quicktimer_minutes,	30,			1,	1,	120},
{"silent",			&hijack_silent,			0,			1,	0,	1},
#ifdef EMPEG_STALK_SUPPORTED
{"stalk_debug",			&hijack_stalk_debug,		0,			1,	0,	1},
{"stalk_lhs",			lhs_stalk_vals,			(int)lhs_stalk_default,	20,	0,	0xff},
{"stalk_rhs",			rhs_stalk_vals,			(int)rhs_stalk_default,	20,	0,	0xff},
{"stalk_enabled",		&hijack_option_stalk_enabled,	1,			1,	0,	1},
#endif // EMPEG_STALK_SUPPORTED
{"standbyLED_on",		&hijack_standbyLED_on,		-1,			1,	-1,	HZ*60},
{"standbyLED_off",		&hijack_standbyLED_off,		10*HZ,			1,	0,	HZ*60},
{"standby_minutes",		&hijack_standby_minutes,	30,			1,	0,	240},
{"suppress_notify",		&hijack_suppress_notify,	1,			1,	0,	1},
{"temperature_correction",	&hijack_temperature_correction,	-4,			1,	-20,	+20},
{"trace_fs",			&hijack_trace_fs,		0,			1,	0,	1},
{"trace_tuner",			&hijack_trace_tuner,		0,			1,	0,	1},
{button_names[4].name,		&hijack_voladj_parms[0][0],	(int)voladj_ldefault,	5,	0,	0x7ffe},
{button_names[5].name,		&hijack_voladj_parms[1][0],	(int)voladj_mdefault,	5,	0,	0x7ffe},
{button_names[6].name,		&hijack_voladj_parms[2][0],	(int)voladj_hdefault,	5,	0,	0x7ffe},
{"bass_freq",			&hijack_bass_freq,		0x6a7d,			1,	0x0000,	0xffff}, // For now, these are the internal register format.
{"bass_q",			&hijack_bass_q,			0x4b00,			1,	0x0000,	0xffff}, // Useful in config.ini to set my suggested values as default
{"treble_freq",			&hijack_treble_freq,		0x6d50,			1,	0x0000,	0xffff}, // and for testing purposes. We shouldn't advertise these settings
{"treble_q",			&hijack_treble_q,		0xc800,			1,	0x0000,	0xffff}, // as we don't know what they really mean.  --genixia
{"visuals_gain",		&hijack_visuals_gain,		0x1f,			1,	0,	0x1f},
{"volume_boost_FM",		&hijack_volboost[0],		0,			1,	-100,	100},
{"volume_boost_MP3",		&hijack_volboost[1],		0,			1,	-100,	100},
{"volume_boost_AUX",		&hijack_volboost[2],		0,			1,	-100,	100},
{"volume_boost_AM",		&hijack_volboost[3],		0,			1,	-100,	100},
{"disable_bassboost_FM",	&hijack_disable_bassboost_FM,	0,			1,	0,	1,},
{"overlay_bg_min",	&hijack_overlay_bg_min,	0x00004000, 1, 0, 0x00010000,},
{"overlay_bg_max",	&hijack_overlay_bg_max,	0x00010000, 1, 0, 0x00010000,},
{"overlay_bg_fadestep",	&hijack_overlay_bg_max,	0x00000AAA, 1, 0, 0x00010000,},

{NULL,NULL,0,0,0,0} // end-of-list
};

static const char showbutton_menu_label	[] = "Button Codes Display";
static const char timer_menu_label	[] = "Countdown Timer Timeout";
static const char timeraction_menu_label[] = "Countdown Timer Action";
static const char fsck_menu_label	[] = "Filesystem Check on Sync";
static const char forcepower_menu_label	[] = "Force AC/DC Power Mode";
static const char onedrive_menu_label	[] = "Hard Disk Detection";
static const char hightemp_menu_label	[] = "High Temperature Warning";
static const char delaytime_menu_label	[] = "Left/Right Time Alignment";
static const char homework_menu_label	[] = "Home/Work Location";
static const char knobdata_menu_label	[] = "Knob Press Redefinition";
static const char carvisuals_menu_label	[] = "Restore DC/Car Visuals";
static const char blankerfuzz_menu_label[] = "Screen Blanker Sensitivity";
static const char blanker_menu_label	[] = "Screen Blanker Timeout";
static const char saveserial_menu_label	[] = "Serial Port Assignment";
static const char bass_menu_label       [] = "Tone: Bass Adjust";
static const char treble_menu_label     [] = "Tone: Treble Adjust";

#ifdef CONFIG_HIJACK_TUNER
#ifdef CONFIG_HIJACK_TUNER_ADJUST
static const char dx_lo_menu_label      [] = "Tuner: DX/LO Mode";
static const char if2_bw_menu_label     [] = "Tuner: IF2 Bandwidth";
static const char agc_menu_label        [] = "Tuner: Wideband AGC";
#endif
#endif

static const char volumelock_menu_label	[] = "Volume Level on Boot";

#define HIJACK_USERQ_SIZE	8
static const unsigned int intercept_all_buttons[] = {1};
static const unsigned int *hijack_buttonlist = NULL;
//static unsigned long hijack_userq[HIJACK_USERQ_SIZE];
//static unsigned short hijack_userq_head = 0, hijack_userq_tail = 0;
static struct wait_queue *hijack_userq_waitq = NULL, *hijack_menu_waitq = NULL, *hijack_player_init_waitq = NULL;

#define SCREEN_BLANKER_MULTIPLIER 15
#define BLANKER_BITS 6
static int blanker_timeout = 0;

#define SENSITIVITY_MULTIPLIER 5
#define SENSITIVITY_BITS 3
static int blanker_sensitivity = 0;

#define HIGHTEMP_OFFSET	34
#define HIGHTEMP_BITS	5
static int hightemp_threshold = 0;

#define FORCEPOWER_BITS 4
static unsigned int hijack_force_power = 0;

#define TIMERACTION_BITS 1
static int timer_timeout = 0, timer_started = 0, timer_action = 0;

static int hijack_homework = 0;
static const char *homework_labels[] = {";@HOME", ";@WORK"};

#define MENU_BITS	5
#define MENU_MAX_ITEMS	(1<<MENU_BITS)
typedef int  (menu_dispfunc_t)(int);
typedef void (menu_movefunc_t)(int);
typedef struct menu_item_s {
	const char		*label;
	menu_dispfunc_t		*dispfunc;
	menu_movefunc_t		*movefunc;
	unsigned long		userdata;
} menu_item_t;
static volatile short menu_item = 0, menu_size = 0, menu_top = 0;

static unsigned char hijack_displaybuf[EMPEG_SCREEN_ROWS][EMPEG_SCREEN_COLS/2];

// Font characters are specified in an 6x8 (wxh) matrix
// each byte represents a column msb at the bottom
// no antialiasing <sigh>

#define FIRST_CHAR (unsigned char)0x1e  // was ' '. Reduce if adding chars to start of list
#define LAST_CHAR (unsigned char)0x7f  // was '~'. Increase if adding chars to end of list

#define HENRY_LEFTC  0x1e
#define HENRY_RIGHTC 0x1f
const unsigned char kfont [1 + LAST_CHAR - FIRST_CHAR][KFONT_WIDTH] = {  // variable width font
	{0x20,0x38,0x26,0x21,0xc5,0x81},  // henry-left
	{0xc5,0x21,0x26,0x38,0x20,0x20},  // henry-right
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
	{0x22,0x41,0x49,0x49,0x36,0x00}, // 3
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
	{0x02,0xb1,0x09,0x09,0x06,0x00}, // ?
	{0x3e,0x41,0x59,0x55,0x4e,0x00}, // @
	{0x7e,0x09,0x09,0x09,0x7e,0x00}, // A
	{0x7f,0x49,0x49,0x49,0x36,0x00}, // B
	{0x3e,0x41,0x41,0x41,0x22,0x00}, // C
	{0x7f,0x41,0x41,0x41,0x3e,0x00}, // D
	{0x7f,0x49,0x49,0x41,0x00,0x00}, // E
	{0x7f,0x09,0x09,0x01,0x00,0x00}, // F
	{0x3e,0x41,0x41,0x49,0x79,0x00}, // G
	{0x7f,0x08,0x08,0x08,0x7f,0x00}, // H
	{0x7f,0x00,0x00,0x00,0x00,0x00}, // I
	{0x20,0x41,0x41,0x3f,0x01,0x00}, // J
	{0x7f,0x08,0x08,0x14,0x63,0x00}, // K
	{0x7f,0x40,0x40,0x40,0x00,0x00}, // L
	{0x7f,0x02,0x04,0x02,0x7f,0x00}, // M
	{0x7f,0x04,0x08,0x10,0x7f,0x00}, // N
	{0x3e,0x41,0x41,0x41,0x3e,0x00}, // O
	{0x7f,0x09,0x09,0x09,0x06,0x00}, // P
	{0x3e,0x41,0x51,0x21,0x5e,0x00}, // Q
	{0x7f,0x09,0x09,0x09,0x76,0x00}, // R
	{0x26,0x49,0x49,0x49,0x32,0x00}, // S
	{0x01,0x01,0x7f,0x01,0x01,0x00}, // T
	{0x3f,0x40,0x40,0x40,0x3f,0x00}, // U
	{0x07,0x18,0x60,0x18,0x07,0x00}, // V
	{0x7f,0x20,0x10,0x20,0x7f,0x00}, // W
	{0x63,0x14,0x08,0x14,0x63,0x00}, // X
	{0x07,0x08,0x70,0x08,0x07,0x00}, // Y
	{0x61,0x51,0x49,0x45,0x43,0x00}, // Z
	{0x7f,0x41,0x41,0x00,0x00,0x00}, // [
	{0x02,0x04,0x08,0x10,0x20,0x00}, // backslash
	{0x41,0x41,0x7f,0x00,0x00,0x00}, // ]
	{0x02,0x01,0x02,0x00,0x00,0x00}, // ^
	{0x80,0x80,0x80,0x80,0x00,0x00}, // _
	{0x01,0x02,0x00,0x00,0x00,0x00}, // `
	{0x20,0x54,0x54,0x78,0x00,0x00}, // a
	{0x7f,0x44,0x44,0x38,0x00,0x00}, // b
	{0x38,0x44,0x44,0x28,0x00,0x00}, // c
	{0x38,0x44,0x44,0x7f,0x00,0x00}, // d
	{0x38,0x54,0x54,0x58,0x00,0x00}, // e
	{0x08,0x7e,0x09,0x01,0x00,0x00}, // f
	{0x18,0xa4,0xa4,0x78,0x00,0x00}, // g
	{0x7f,0x04,0x04,0x78,0x00,0x00}, // h
	{0x7d,0x00,0x00,0x00,0x00,0x00}, // i
	{0x80,0x80,0x7d,0x00,0x00,0x00}, // j
	{0x7f,0x10,0x28,0x44,0x00,0x00}, // k
	{0x7f,0x00,0x00,0x00,0x00,0x00}, // l
	{0x7c,0x04,0x78,0x04,0x78,0x00}, // m
	{0x7c,0x04,0x04,0x78,0x00,0x00}, // n
	{0x38,0x44,0x44,0x38,0x00,0x00}, // o
	{0xfc,0x24,0x24,0x18,0x00,0x00}, // p
	{0x18,0x24,0x24,0xf8,0x00,0x00}, // q
	{0x7c,0x08,0x04,0x00,0x00,0x00}, // r
	{0x08,0x54,0x54,0x20,0x00,0x00}, // s
	{0x04,0x3e,0x44,0x00,0x00,0x00}, // t
	{0x3c,0x40,0x40,0x7c,0x00,0x00}, // u
	{0x0c,0x30,0x40,0x30,0x0c,0x00}, // v
	{0x3c,0x40,0x3c,0x40,0x3c,0x00}, // w
	{0x6c,0x10,0x10,0x6c,0x00,0x00}, // x
	{0x1c,0xa0,0xa0,0x7c,0x00,0x00}, // y
	{0x64,0x54,0x4c,0x00,0x00,0x00}, // z
	{0x08,0x36,0x41,0x00,0x00,0x00}, // {
	{0x7f,0x00,0x00,0x00,0x00,0x00}, // |
	{0x41,0x36,0x08,0x00,0x00,0x00}, // }
	{0x02,0x01,0x02,0x04,0x02,0x00}  // ~
	};

int
strxcmp (const char *s, const char *pattern, int partial)
{
	unsigned char c, p;

	while ((p = *pattern)) {
		++pattern;
		c = *s++;
		if (TOUPPER(c) != TOUPPER(p))
			return 1;	// did not match
	}
	return (!partial && *s);	// 0 == matched; 1 == not matched
}

static void
clear_hijack_displaybuf (unsigned char color)
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

static const hijack_geom_t *hijack_overlay_geom = NULL;

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
draw_frame (const hijack_geom_t *geom)
{
	// draw a frame inside geom, one pixel smaller all around
	// for simplicity, we only do pixels in pairs
	unsigned char *dest = (unsigned char *)hijack_displaybuf;
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
create_overlay (const hijack_geom_t *geom)
{
	clear_hijack_displaybuf(COLOR0);
	draw_frame(geom);
	hijack_overlay_geom = geom;
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

static unsigned char kfont_spacing = 0;  // 0 == proportional

// Note the colors here are not the same as the colors used by draw_string()
static int
draw_char (unsigned short pixel_row, short pixel_col, unsigned char c, unsigned char foreground, unsigned char background)
{
	unsigned char num_cols;
	const unsigned char *font_entry;
	unsigned char *displayrow;

	if (pixel_row >= EMPEG_SCREEN_ROWS)
		return 0;
	displayrow = &hijack_displaybuf[pixel_row][0];
	if (c > LAST_CHAR || c < FIRST_CHAR)
		c = ' ';
	font_entry = &kfont[c - FIRST_CHAR][0];
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
	int firstcol = (EMPEG_SCREEN_COLS*2), firstrow = 0;
	unsigned char firstchar;
#endif // ROUND_CORNERS
	int no_wraplines = row & 0x04000;
	row &= ~0x4000;

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
				if (no_wraplines)
					break;
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
draw_string_spaced (unsigned int rowcol, const unsigned char *s, int color)
{
	char buf[64];
	sprintf(buf, " %s ", s);
	return draw_string(rowcol, buf, color);
}

static unsigned int
draw_number (unsigned int rowcol, unsigned int number, const char *format, int color)
{
	unsigned char buf[31], saved;

	sprintf(buf, format, number);
	saved = kfont_spacing;
	kfont_spacing = KFONT_WIDTH; // use fixed font spacing for numbers
	rowcol = draw_string(rowcol, buf, color);
	kfont_spacing = saved;
	return rowcol;

}

static void
hijack_initq (hijack_buttonq_t *q, unsigned char qname)
{
	q->qname = qname;
	q->head = q->tail = q->last_deq = 0;
	q->last_deq = jiffies;
}

static unsigned long
PRESSCODE (unsigned int button)
{
	button &= ~BUTTON_FLAGS;
	if (button > 0xf || button == IR_KNOB_LEFT || button == IR_KNOB_RIGHT)
		return button;
	return button & ~1;
}

unsigned int
RELEASECODE (unsigned int button)
{
	button &= ~BUTTON_FLAGS;
	if ((button >= IR_FAKE_HIJACKMENU && button <= IR_NULL_BUTTON) || button == IR_KNOB_LEFT || button == IR_KNOB_RIGHT)
		return IR_NULL_BUTTON;
	return (button > 0xf) ? button | 0x80000000 : button | 1;
}

static int
IS_RELEASE (unsigned int rawbutton)
{
	unsigned int button = rawbutton & ~BUTTON_FLAGS;
	if (button != IR_NULL_BUTTON) {
		if (button > 0xf)
			return (rawbutton >> 31);
		if (button != IR_KNOB_LEFT)
			return (button & 1);
	}
	return 0;
}

static char *get_button_name (unsigned int button, char *buf)
{
	button_name_t *bn = button_names;

	button = PRESSCODE(button) | (button & (BUTTON_FLAGS_ALTNAME|BUTTON_FLAGS_LONGPRESS));
search:
	for (bn = button_names; bn->name[0]; ++bn) {
		if (button == (bn->code & ~(BUTTON_FLAGS ^ BUTTON_FLAGS_ALTNAME))) {
			return bn->name;
		}
	}
	if (button & BUTTON_FLAGS_LONGPRESS) {
		button ^= BUTTON_FLAGS_LONGPRESS;
		goto search;
	}
	sprintf(buf, "0%x", button);
	return buf;
}

static int empeg_powerstate = 0;

static void
hijack_enq_button (hijack_buttonq_t *q, unsigned int button, unsigned long hold_time)
{
	unsigned short head;
	unsigned long flags;

#if 1 //fixme someday
	// special case to allow embedding PopUp's
	if ((button & ~BUTTON_FLAGS) >= IR_FAKE_HIJACKMENU && (button & ~BUTTON_FLAGS) <= IR_FAKE_POPUP3)
		q = &hijack_inputq;
#endif
	if (q != &hijack_inputq)
		button &= ~(BUTTON_FLAGS^BUTTON_FLAGS_LONGPRESS);

	save_flags_clif(flags);
	head = q->head;
	if (++head >= HIJACK_BUTTONQ_SIZE)
		head = 0;
	if (head != q->tail) {
		hijack_buttondata_t *data = &q->data[q->head = head];
		data->button = button;
		data->delay  = hold_time;
		if (hijack_ir_debug)
			printk("%lu: ENQ.%c: @%p: %08x.%ld\n", jiffies, q->qname, data, button, hold_time);
	}
	restore_flags(flags);
	if (q == &hijack_playerq)
		input_wakeup_waiters();		// wake-up the player software
}

// Send a release code
static void
hijack_enq_release (hijack_buttonq_t *q, unsigned int rawbutton, unsigned long hold_time)
{
	unsigned int button = RELEASECODE(rawbutton);
	if (button != IR_NULL_BUTTON) {
		button |= (rawbutton & BUTTON_FLAGS_UISTATE);
		if (rawbutton & BUTTON_FLAGS_LONGPRESS)
			hold_time = LONGPRESS_DELAY;
		hijack_enq_button(q, button, hold_time);
	}
}

static inline void
hijack_enq_button_pair (unsigned int button)
{
	hijack_enq_button (&hijack_playerq, button & ~BUTTON_FLAGS, 0);
	hijack_enq_release(&hijack_playerq, button, 0);
}

// Send a translated replacement sequence, except for the final release code
static void
hijack_enq_translations (ir_translation_t *t)
{
	unsigned int *newp = &t->new[0];
	int count = t->count, waitrelease = (t->old < IR_FAKE_HIJACKMENU) && t->old != IR_KNOB_LEFT && t->old != IR_KNOB_RIGHT;

	while (count--) {
		unsigned long code = *newp++;
		hijack_enq_button(&hijack_inputq, code & ~BUTTON_FLAGS_LONGPRESS, 0);
		if (count || !waitrelease) {
			unsigned int button = code & ~(BUTTON_FLAGS^BUTTON_FLAGS_UISTATE);
			hijack_enq_release(&hijack_inputq, button|(code & BUTTON_FLAGS_LONGPRESS), 0);
		}
	}
}

#ifdef EMPEG_STALK_SUPPORTED
static void
inject_stalk_button (unsigned int button)
{
	unsigned char pkt[4] = {0x02, 0x00, 0xff, 0xff};
	min_max_t *vals, *v;
	int i;
	unsigned int rawbutton = button;

	pkt[0] = 0x02;
	pkt[1] = 0x00;
	if (button & IR_STALK_SHIFTED) {
		button ^= IR_STALK_SHIFTED;
		pkt[1] = 0x01;
	}
	if (button & 0x80000000) {
		pkt[2] = 0xff;
	} else {
		for (i = 0; stalk_buttons[i] != button; ++i) {
			if (stalk_buttons[i] == IR_NULL_BUTTON)
				return;
		}
		vals = stalk_on_left ? lhs_stalk_default : rhs_stalk_default;
		v = &vals[i];
		pkt[2] = (v->min + v->max) / 2;
	}
	pkt[3] = pkt[1] + pkt[2];
	if (hijack_stalk_debug)
		printk("Stalk: out=%02x %02x %02x %02x == %08x\n", pkt[0], pkt[1], pkt[2], pkt[3], rawbutton);
	hijack_serial_rx_insert(pkt, sizeof(pkt), 0);
}
#endif // EMPEG_STALK_SUPPORTED

static int
hijack_button_deq (hijack_buttonq_t *q, hijack_buttondata_t *rdata, int nowait)
{
	hijack_buttondata_t *data;
	unsigned short tail;
	unsigned long flags;

	save_flags_clif(flags);
	if ((tail = q->tail) != q->head) {	// anything in the queue?
		if (++tail >= HIJACK_BUTTONQ_SIZE)
			tail = 0;
		data = &q->data[tail];
		if (nowait || !data->delay || jiffies_since(q->last_deq) > data->delay) {
			rdata->button = data->button;
			rdata->delay  = data->delay;
			q->tail = tail;
			q->last_deq = jiffies;
			if (hijack_ir_debug)
				printk("%lu: DEQ.%c: @%p: %08x.%ld\n", q->last_deq, q->qname, data, data->button, data->delay);
			restore_flags(flags);
			return 1;	// button was available
		}
	}
	restore_flags(flags);
	return 0;	// no buttons available
}

int
hijack_playerq_deq (unsigned int *rbutton)	// for use by empeg_input.c
{
	hijack_buttondata_t *data;
	unsigned short tail;
	unsigned long flags;
	static int dmsg = 0;

	save_flags_clif(flags);
	while ((tail = hijack_playerq.tail) != hijack_playerq.head) {	// anything in the queue?
		unsigned int button;
		if (++tail >= HIJACK_BUTTONQ_SIZE)
			tail = 0;
		data = &(hijack_playerq.data[tail]);
		if (jiffies_since(hijack_playerq.last_deq) < data->delay) {
			if (hijack_ir_debug && !dmsg) {
				printk("Delaying..\n");
				dmsg = 1;
			}
			break;		// no button available yet
		}
		dmsg = 0;

		button = data->button;
#ifdef EMPEG_STALK_SUPPORTED
		if ((button & IR_STALK_MASK) == IR_STALK_MATCH) {
			//
			// Dequeue/issue the stalk button, and loop again
			//
			hijack_playerq.tail = tail;
			hijack_playerq.last_deq = jiffies;
			if (hijack_ir_debug)
				printk("%lu: DEQ.%c: @%p: %08x.%ld (stalk)\n", hijack_playerq.last_deq, hijack_playerq.qname, data, button, data->delay);
			restore_flags(flags);
			inject_stalk_button(button);
			save_flags_clif(flags);
		} else
#endif // EMPEG_STALK_SUPPORTED
		{
			//
			// If caller gave us a pointer, then deq/return button,
			// otherwise leave button on the queue for now.
			//
			if (rbutton) {
				*rbutton = button;
				hijack_playerq.tail = tail;
				hijack_playerq.last_deq = jiffies;
				if (hijack_ir_debug)
					printk("%lu: DEQ.%c: @%p: %08x.%ld\n", hijack_playerq.last_deq, hijack_playerq.qname, data, button, data->delay);
			}
			restore_flags(flags);
			return 0;	// a button was available
		}
	}
	restore_flags(flags);
	return -EAGAIN;	// no buttons available
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
	hijack_userdata = 0;
	hijack_buttonlist = NULL;
	ir_trigger_count = 0;
	hijack_overlay_geom = NULL;
	hijack_status = new_status;
	restore_flags(flags);
}

static inline void
untrigger_blanker (void)
{
	blanker_triggered = 0;
}

static void
activate_dispfunc (int (*dispfunc)(int), void (*movefunc)(int), unsigned long userdata)
{
	hijack_deactivate(HIJACK_ACTIVE_PENDING);
	hijack_dispfunc = dispfunc;
	hijack_movefunc = movefunc;
	hijack_userdata = userdata;
	untrigger_blanker();
	dispfunc(1);
}

#ifdef EMPEG_KNOB_SUPPORTED

/*
 * LittleBlueThing's WhackA??? menu entry for the 10th Birthday Party.
 * A game where characters pop up at the edges of the display
 * and you whack them by pressing the up/down/left/right buttons.
 *
 * Tested by Denise...
 */
static const char wam_menu_label[] = "Whack-a-mole";

#define  wam_printk(...) 
// #define  wam_printk(...) printk(__VA_ARGS__)
unsigned short wam_playing = 0;    // game in progress
int wam_next_mole_time = 0;
unsigned short wam_h_loc = 0;        // Where's henry
unsigned short wam_whack_loc = 0;      // Where did you hit or WAM_NONE
unsigned short wam_status = 0;     // What happened?
short wam_score = 0;      // Score
unsigned short wam_score_counted = 0;     // Score counted?
unsigned int  wam_molet = 0;      // time mole displayed

// This changes how often henries appear 4 = always somewhere...
// higher numbers mean more empty screen.
#define wam_henry_freq 6
// This is how long henry hangs around for in jiffies 128 is fairly easy
#define WAM_MIN_TIME 30
#define WAM_TIME_LIMIT_10 80
#define WAM_TIME_LIMIT_20 60
#define WAM_TIME_LIMIT_30 40
#define WAM_TIME_LIMIT_40 20
#define WAM_TIME_LIMIT_50 0 // got to be kidding!!
#define WAM_IN_CAR_EASIER 15 // extra time in the car
int wam_time_limit = WAM_TIME_LIMIT_10;

int wam_misses_left;
#define WAM_MISSES 10

// used by wam_h_loc and wam_whack_loc
#define WAM_NONE 0
#define WAM_UP 1
#define WAM_DOWN 2
#define WAM_LEFT 3
#define WAM_RIGHT 4
#define WAM_DONE 5

#define WAM_READY 0
#define WAM_HIT 1
#define WAM_MISS 2
#define WAM_SCORE_COUNTED 3
#define WAM_GAME_OVER 4
#define WAM_WAIT_RESTART 5

typedef struct glyph_s {
	unsigned short   width;
	unsigned short   height;
	const char		 *data;
} glyph_t;

static void
wam_move (int move)
{
	if (wam_score_counted)
		return;

	wam_whack_loc = move;
	if (wam_whack_loc == wam_h_loc) {
		wam_printk("HIT\n");
		wam_status = WAM_HIT;
	} else {
		wam_printk("MISS\n");
		wam_status = WAM_MISS;
	}
}

static void
wam_init_game (void) {
		// setup game state
		wam_whack_loc = WAM_NONE;
		wam_h_loc = WAM_NONE;
		wam_score=0;
		wam_misses_left=WAM_MISSES;
		wam_printk("misses_left : %d\n", wam_misses_left);
}

static int
wam_display (int firsttime)
{
	hijack_buttondata_t data;
	unsigned char buf[20];
	unsigned short row=0;
	short col =0 ;
	int refresh = NO_REFRESH; 

	if (firsttime || (wam_status == WAM_GAME_OVER)) {
		clear_hijack_displaybuf(COLOR0);
		if (firsttime) {
			// Show the menu text
			(void) draw_string(ROWCOL(0,0), "Welcome to Whack-a-Mole.\nleft/right/up/down to whack.\nKnob: turn starts, press exits", PROMPTCOLOR);
			// Do our own button handling
			hijack_buttonlist = intercept_all_buttons;
			hijack_initq(&hijack_userq, 'U');
			wam_status=WAM_READY;
		} else { // GAMEOVER
			sprintf(buf, "Game Over\nScore : %3d",wam_score);
			(void) draw_string(ROWCOL(0,20), buf, PROMPTCOLOR);
			draw_char(8, 90, HENRY_LEFTC, (COLOR3<<4)|COLOR3, (COLOR0<<4)|COLOR0);
			draw_char(8, 90+6, HENRY_RIGHTC, (COLOR3<<4)|COLOR3, (COLOR0<<4)|COLOR0);

			wam_status=WAM_WAIT_RESTART;
			wam_next_mole_time = 0;
		}
		wam_playing = 0;
		wam_init_game();
		return NEED_REFRESH;
	}

	// Do our own button handling
	if (hijack_button_deq(&hijack_userq, &data, 0)) {
		wam_printk("KEY\n");
		switch (data.button) {
		case IR_TOP_BUTTON_PRESSED:
			wam_move(WAM_UP); break;
		case IR_LEFT_BUTTON_PRESSED:
			wam_move(WAM_LEFT); break;
		case IR_RIGHT_BUTTON_PRESSED:
			wam_move(WAM_RIGHT); break;
		case IR_BOTTOM_BUTTON_PRESSED:
			wam_move(WAM_DOWN); break;
		case IR_KNOB_RIGHT:
		case IR_KNOB_LEFT:
			wam_init_game();
			wam_status=WAM_WAIT_RESTART;
			wam_playing = 1;
			wam_molet=JIFFIES();
			wam_next_mole_time = 100;
			clear_hijack_displaybuf(COLOR0);
			(void) draw_string(ROWCOL(1,20)," GET READY ... " , PROMPTCOLOR);
			return NEED_REFRESH;
			break;
		case IR_KNOB_PRESSED:
			hijack_buttonlist = NULL;
			ir_selected = 1; // return to main menu
			break;
		}
	}

	if (! wam_playing)
		return NO_REFRESH;

	// if timeout passed - mark it as a READY so we stop showing hit/miss
	// A keypress could just have arrived so only do this if there isn't one
	// or if it's marked 'done'
	if 	((jiffies_since(wam_molet) >= wam_next_mole_time) &&
		(wam_whack_loc == WAM_NONE || wam_whack_loc == WAM_DONE))
	{
		wam_score_counted=0;
		wam_status = WAM_READY;
		wam_printk("READY\n");
	}

	// if it's a hit/miss then flash up the message and change the score
	// the 'double' time check is because this bit needs re-displaying every
	// display cycle for the fancy(!) shaking text. Was it worth it?
	if (wam_status == WAM_HIT || wam_status == WAM_MISS) {
		int row = 40;
		int col = 8;		
		int j = JIFFIES() & 0xf;
		row+=j;
		col+=(j&7);
		j|=j<<4;
		clear_hijack_displaybuf(COLOR0);
		if (wam_status == WAM_HIT) {// (COLOR0<<4)|COLOR0
			row+=draw_char(col,row, 'H', j, COLOR0);		  
			row+=draw_char(col,row, 'I', j+1, COLOR0);
			row+=draw_char(col,row, 'T', j+2, COLOR0);
			row+=draw_char(col,row, ' ', j+2, COLOR0);
			row+=draw_char(col,row, '!', j+3, COLOR0);
			row+=draw_char(col,row, '!', j+4, COLOR0);
		} else if (wam_status == WAM_MISS) {
			row+=draw_char(col,row, 'M', j, COLOR0);
			row+=draw_char(col,row, 'I', j, COLOR0);
			row+=draw_char(col,row, 'S', j, COLOR0);
			row+=draw_char(col,row, 'S', j, COLOR0);
			row+=draw_char(col,row, '!', j, COLOR0);
			row+=draw_char(col,row, '!', j, COLOR0);
		}
		if (!wam_score_counted){
			wam_molet = JIFFIES();
			wam_score_counted=1;
			wam_whack_loc = WAM_DONE;  // mark whack as 'seen'
			if (wam_status == WAM_MISS) {
				hijack_beep(60, 100, 50);	// sad beep
				if (! --wam_misses_left)
					wam_status = WAM_GAME_OVER;
				wam_printk("MISS: misses_left : %d\n", wam_misses_left);
				wam_score-=1;
				wam_next_mole_time = HZ/2;
			} else if (wam_status == WAM_HIT) {
				hijack_beep(80, 100, 50); // happy beep
				wam_next_mole_time = HZ;
				wam_score+=1;
				wam_printk("HIT: %d\n", wam_score);
			}
			switch (wam_score / 10) {				
			case 0: wam_time_limit = WAM_TIME_LIMIT_10; break;
			case 1: wam_time_limit = WAM_TIME_LIMIT_20; break;
			case 2: wam_time_limit = WAM_TIME_LIMIT_30; break;
			case 3: wam_time_limit = WAM_TIME_LIMIT_40; break;
			default: wam_time_limit = WAM_TIME_LIMIT_50;
			}
			// make it easier in the car
			wam_time_limit+=empeg_on_dc_power?WAM_IN_CAR_EASIER:0;
		}
		sprintf(buf, "Score : %3d",wam_score);
		(void) draw_string(ROWCOL(3,0), buf, PROMPTCOLOR);	
		return NEED_REFRESH; // every time - flickery display
	}

	// 
	if 	(jiffies_since(wam_molet) <= wam_next_mole_time)
		return refresh;


	// If there's no hit AND we're out of time for this henry then:
	// * get a new time
	// * get a new mole
	// * draw the mole

	// if there was a henry and no attempt then decrement the misses_left
	if (wam_h_loc != WAM_NONE && wam_whack_loc==WAM_NONE) {
		wam_misses_left--;
		wam_printk("NOGO: misses_left : %d\n", wam_misses_left);
		// sad_beep()
		hijack_beep(48, 100, 50);
		if (wam_misses_left==0)
			wam_status = WAM_GAME_OVER;
	}

	wam_printk("\nNew mole\n");
	clear_hijack_displaybuf(COLOR0);

	if (wam_h_loc == WAM_NONE) { // maybe draw a henry?
		unsigned char rand_b ;         // place to put a random byte
		get_random_bytes(&rand_b,1);
		wam_next_mole_time = rand_b % wam_time_limit;
		wam_next_mole_time += WAM_MIN_TIME;
		wam_printk("next_mole due : %d\n",wam_next_mole_time);

		get_random_bytes(&rand_b,1);
		wam_h_loc = (int)(rand_b % wam_henry_freq) + 1;
		switch (wam_h_loc) {
		case WAM_UP    : col=56;  row=0;
			break;
		case WAM_DOWN  : col=56;  row=24;
			break;
		case WAM_LEFT  : col=0;   row=8;
			break;
		case WAM_RIGHT : col=104; row=8;
			break;
		default :
			wam_h_loc=WAM_NONE;
			break;
		}		
		if (wam_h_loc != WAM_NONE) { // Do we have a henry?
			wam_printk("Mole up\n");
			draw_char(row, col, HENRY_LEFTC, (COLOR3<<4)|COLOR3, (COLOR0<<4)|COLOR0);
			draw_char(row, col+6, HENRY_RIGHTC, (COLOR3<<4)|COLOR3, (COLOR0<<4)|COLOR0);
		}
	} else { // force a delay after showing a henry
		wam_next_mole_time = WAM_MIN_TIME*2;		
		wam_h_loc=WAM_NONE;
	}	

	// Draw remaining time
	{
		int width = (100 * (wam_misses_left-1))/WAM_MISSES;
		int i;
		for (i=0; i<width; i++) {
			draw_pixel(14,12+i, COLOR3);
			draw_pixel(15,12+i, COLOR1);
			draw_pixel(16,12+i, COLOR3);
		}
	}
	wam_molet = JIFFIES();
	wam_whack_loc = WAM_NONE; // mark start of new attempt
	return NEED_REFRESH;
}

#endif /* EMPEG_KNOB_SUPPORTED */

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

void
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
	(void)draw_string_spaced(rowcol, voladj_names[hijack_voladj_enabled], ENTRYCOLOR);
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

static int
voladj_prefix_display (int firsttime)
{
	static const hijack_geom_t geom = {8, 8+6+KFONT_HEIGHT, 12, EMPEG_SCREEN_COLS-12};

	if (firsttime) {
		hijack_last_moved = JIFFIES();
		create_overlay(&geom);
	} else if (jiffies_since(hijack_last_moved) >= (HZ*3)) {
		hijack_deactivate(HIJACK_IDLE);
	} else {
		unsigned int rowcol = (geom.first_row+4)|((geom.first_col+6)<<16);
		rowcol = draw_string(rowcol, "Auto VolAdj: ", COLOR3);
		clear_text_row(rowcol, geom.last_col-4, 1);
		rowcol = draw_string_spaced(rowcol, voladj_names[hijack_voladj_enabled], ENTRYCOLOR);
	}
	return NO_REFRESH;	// gets overridden if overlay still active
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
	for (c = (unsigned char)' '; c <= (unsigned char)'~'; ++c) {
		unsigned char s[2] = {0,0};
		s[0] = c;
		rowcol = draw_string(rowcol, &s[0], COLOR3);
	}
	return NEED_REFRESH;
}

static unsigned long hijack_last_readtherm = 0;
static int           hijack_last_temperature = 0;

static void
init_temperature (int force)
{
	static int done = 0;
	unsigned long flags;

	save_flags_clif(flags);
	if (!done || force) {
		int status, was_dead = 0;
		done = 1;
		status = empeg_readtherm_status(&OSMR0,&GPLR);
		if ((status & 0x45) != 0x40) {
			was_dead = 1;
			if (status != -1) {
				empeg_fixtherm(&OSMR0,&GPLR);
				status = empeg_readtherm_status(&OSMR0,&GPLR);
			}
		}
		restore_flags(flags);	// give other interrupts a chance
		if (was_dead) {
			const char *msg = "Dead temp.sensor";
			if ((status & 0x45) == 0x40) {
				msg = "Fixed temp.sensor";
				show_message(msg, 5*HZ);
			}
			if (!hijack_silent)
				printk("%s, status=0x%02x\n", msg, status & 0xff);
		}
		save_flags_clif(flags);
		empeg_inittherm(&OSMR0,&GPLR);
	}
	restore_flags(flags);
}

int
hijack_read_temperature (void)
{
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
	// so we may still need the odd call to empeg_inittherm()
	// just to ensure it is running.  -ml
	// Update: perhaps this was due to corrupted config register,
	// which we now try to deal with in init_temperature.

	init_temperature(0);
	save_flags_clif(flags);
	if (jiffies_since(hijack_last_readtherm) < (HZ*5)) {
		restore_flags(flags);
		return hijack_last_temperature;
	}
	hijack_last_temperature = empeg_readtherm(&OSMR0,&GPLR);
	/* Correct for negative temperatures (sign extend) */
	if (hijack_last_temperature & 0x80)
		hijack_last_temperature = -(128 - (hijack_last_temperature ^ 0x80));
	hijack_last_temperature += hijack_temperature_correction;
	hijack_last_readtherm = jiffies;
	restore_flags(flags);
	hijack_last_readtherm = jiffies;
	return hijack_last_temperature;
}

static unsigned int
draw_temperature (unsigned int rowcol, int temp, int offset, int color)
{
	unsigned char buf[32];
	sprintf(buf, "%+dC/%+dF", temp, temp * 180 / 100 + offset);
	return draw_string_spaced(rowcol, buf, color);
}

static unsigned char *last_savearea;
static unsigned long *last_updated  = NULL;
unsigned char **empeg_state_writebuf;	// initialized in empeg_state.c
static unsigned int savearea_display_offset = 0;

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
	int i;
	if (firsttime) {
		if (!last_savearea)
			last_savearea = kmalloc(128, GFP_KERNEL);
		if (!last_updated)
			last_updated = kmalloc(128 * sizeof(long), GFP_KERNEL);
		if (!last_savearea || !last_updated) {
			if (!hijack_silent)
				printk("savearea_display: no memory\n");
			ir_selected = 1;
			return NO_REFRESH;
		}
		memcpy(last_savearea, empeg_statebuf, 128);
		memset(last_updated, 0, 128 * sizeof(long));
		clear_hijack_displaybuf(COLOR0);
		rc = NEED_REFRESH;
	} else if (jiffies_since(hijack_last_refresh) >= (HZ/4)) {
		unsigned long now = JIFFIES();
		for (i = 127; i >= 0; --i) {	// Monitor all 128 bytes
			unsigned char b = empeg_statebuf[i];
			if (b != last_savearea[i]) {
				last_savearea[i] = b;
				last_updated[i] = now;
				rc = NEED_REFRESH;
			}
		}
		if (rc != NEED_REFRESH) {
			if (hijack_last_moved || jiffies_since(hijack_last_refresh) >= (HZ))
				rc = NEED_REFRESH;
		}
	}
	if (rc == NEED_REFRESH) {
		unsigned int rowcol = ROWCOL(0,0), offset = savearea_display_offset;
		hijack_last_moved = 0;
		for (i = 0; i < 32; ++i) {	// Show 32 bytes at a time on the screen
			unsigned long elapsed;
			unsigned int addr = (offset + i) & 0x7f, color = COLOR2;
			unsigned char b = empeg_statebuf[addr];
			if (last_updated[addr]) {
				elapsed = jiffies_since(last_updated[addr]);
				if (elapsed < (11*HZ))
					color = (elapsed < (3*HZ)) ? -COLOR3 : COLOR3;
				else
					last_updated[addr] = 0;
			}
			if (!(i & 7))
				rowcol = draw_number(ROWCOL(i>>3,0), addr, "%02x:", COLOR1);
			if (!(i & 1))
				rowcol = draw_string(rowcol, " ", COLOR0);
			rowcol = draw_number(rowcol, b, "%02X", color);
		}
	}
	if (ir_selected) {
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
get_drive_size (int hwif, int unit)
{
	extern unsigned long idedisk_capacity (ide_drive_t  *drive);
	unsigned long capacity = idedisk_capacity(&(ide_hwifs[hwif].drives[unit]));
	return (capacity + (1000 * 1000)) / (2 * 1000 * 1000);
}

static int
vitals_display (int firsttime)
{
	extern unsigned long memory_end;
	extern int nr_free_pages;
	extern const char *notify_fid(void);
	unsigned int *permset=(unsigned int*)(EMPEG_FLASHBASE+0x2000);
	unsigned char buf[80];
	int rowcol, i, count = 0, model = 0x2a;
	unsigned char *sa;
	unsigned long flags;

	if (!firsttime && jiffies_since(hijack_last_refresh) < HZ)
		return NO_REFRESH;
	clear_hijack_displaybuf(COLOR0);

	// Model, DRAM, Drives
	if (permset[0] < 7)
		model = 1;
	else if (permset[0] < 9)
		model = 2;
	if (hijack_cs4231a_failed)
		buf[count++] = '*';
	count += sprintf(buf+count, "Mk%x: %luMB, %d", model, (memory_end - PAGE_OFFSET) >> 20, get_drive_size(0,0));
	model = (model == 1);	// 0 == Mk2(a); 1 == Mk1
	if (ide_hwifs[model].drives[!model].present)
		sprintf(buf+count, "+%d", get_drive_size(model,!model));
	rowcol = draw_string(ROWCOL(0,0), buf, PROMPTCOLOR);

	// Current Playlist and Fid:
	save_flags_cli(flags);
	sa = *empeg_state_writebuf;
	sprintf(buf, "GB\nPlaylist.Trk:%02x%02x.%u", sa[0x45], sa[0x44], *(unsigned int *)(void *)(sa+0x24));
	restore_flags(flags);
	rowcol = draw_string(rowcol, buf, PROMPTCOLOR);

	// Virtual Memory Pages Status:  Physical,Cached,Buffers,Free
	sprintf(buf,"\nCac:%lu,Buf:%lu,Fre:%u",
		page_cache_size, buffermem/PAGE_SIZE, nr_free_pages);
	rowcol = draw_string(rowcol, buf, PROMPTCOLOR);

	// System Load Averages:
	rowcol = draw_string(rowcol, "\nLoadAvg:", PROMPTCOLOR);
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

/*
 * We call this when the player crashes, to clear the current playlist
 * and hopefully thus prevent infinite player crash loops.
 * See arch/arm/mm/fault-common.c for the call to this routine.
 */
void hijack_clear_playlist (void)
{
	unsigned char *sa;
	unsigned long flags;

	save_flags_cli(flags);
	sa = *empeg_state_writebuf;
	sa[0x44] = sa[0x45] = 0;
	sa = hijack_get_state_read_buffer();
	sa[0x44] = sa[0x45] = 0;
	empeg_state_dirty = 1;
	restore_flags(flags);
}

static char *acdc_text[2] = {"AC/Home", "DC/Car"};

#define FORCE_NORMAL		0
#define FORCE_AC		1
#define FORCE_DC		2
#define FORCE_TUNER		3	// and higher..
#define FORCE_NOLOOPBACK	((1<<FORCEPOWER_BITS)-1)

static void
forcepower_move (int direction)
{
	hijack_force_power = (hijack_force_power + direction) & ((1<<FORCEPOWER_BITS)-1);
#ifndef EMPEG_KNOB_SUPPORTED
	if (hijack_force_power >= FORCE_TUNER)
		hijack_force_power = (direction < 0) ? FORCE_DC : FORCE_NORMAL;
#endif // EMPEG_KNOB_SUPPORTED
	empeg_state_dirty = 1;
}

static int
forcepower_display (int firsttime)
{
	unsigned int rowcol, force_power;
	char  buf[32], *msg = buf;

	if (!firsttime && !hijack_last_moved)
		return NO_REFRESH;
	hijack_last_moved = 0;
	clear_hijack_displaybuf(COLOR0);
	draw_string(ROWCOL(0,0), forcepower_menu_label, PROMPTCOLOR);
	rowcol = draw_string(ROWCOL(1,0), "Current Mode: ", PROMPTCOLOR);
	draw_string(rowcol, acdc_text[empeg_on_dc_power], PROMPTCOLOR);
	rowcol = draw_string(ROWCOL(2,0), "On reboot: ", PROMPTCOLOR);
	force_power = hijack_force_power;
	switch (force_power) {
		default:
#ifdef EMPEG_KNOB_SUPPORTED
			rowcol = ROWCOL(3,0);
			force_power -= FORCE_TUNER;
			sprintf(msg, "If tuner=%d, Force %s", force_power >> 1, acdc_text[force_power & 1]);
			break;
#endif // EMPEG_KNOB_SUPPORTED
		case FORCE_NOLOOPBACK:
			msg = "No-Loopback";
			break;
		case FORCE_NORMAL:
			msg = "Normal";
			break;
		case FORCE_AC:
		case FORCE_DC:
			sprintf(msg, "Force %s", acdc_text[force_power == FORCE_DC]);
			break;
	}
	draw_string_spaced(rowcol, msg, ENTRYCOLOR);
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
draw:	return draw_string_spaced(rowcol, buf, color);
}

static void
timer_move (int direction)
{
	const int max = (99 * 60 * 60 * HZ);  // Max 99 hours
	int new;

	if (direction == 0) {
		timer_timeout = 0;
	} else {
		if (timer_timeout >= (5 * 60 * 60 * HZ))
			direction *= 30;
		else if (timer_timeout >= (1 * 60 * 60 * HZ))
			direction *= 15;
		else if (direction > 0 && timer_timeout >= (5 * 60 * HZ))
			direction *= 5;
		else if (direction < 0 && timer_timeout > (5 * 60 * HZ))
			direction *= 5;
		else if (timer_timeout == 0)
			direction *= 30;
		new = timer_timeout + (direction * (60 * HZ));
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
	empeg_state_dirty = 1;
}

static int
timer_display (int firsttime)
{
	static int timer_paused = 0;
	unsigned int rowcol;
	unsigned char *offmsg = "[Off]";

	if (firsttime) {
		timer_paused = 0;
		if (timer_timeout) {  // was timer already running?
			int remaining = timer_timeout - jiffies_since(timer_started);
			if (remaining >= HZ) {
				timer_timeout = remaining;
				timer_paused = 1;
			} else {
				timer_timeout = 0;  // turn alarm off if it was on
				offmsg = "[Cancelled]";
			}
		}
		ir_numeric_input = &timer_timeout;
	}
	timer_started = JIFFIES();
	if (!firsttime && !hijack_last_moved)
		return NO_REFRESH;
	hijack_last_moved = 0;
	clear_hijack_displaybuf(COLOR0);
	(void)draw_string(ROWCOL(0,0), timer_menu_label, PROMPTCOLOR);
	rowcol = draw_string(ROWCOL(2,0), "Duration: ", PROMPTCOLOR);
	if (timer_timeout) {
		rowcol = draw_hhmmss(rowcol, timer_timeout / HZ, ENTRYCOLOR);
		offmsg = timer_paused ? "[paused]" : NULL;
	} else {
		timer_paused = 0;
	}
	if (offmsg)
		(void)draw_string_spaced(rowcol, offmsg, ENTRYCOLOR);
	return NEED_REFRESH;
}

static void
volumelock_move (int direction)
{
	hijack_volumelock_enabled = !hijack_volumelock_enabled;
	empeg_state_dirty = 1;
}

static const char *last_current[2] = {"Previous", "Current"};

static int
volumelock_display (int firsttime)
{
	unsigned int rowcol;

	save_current_volume();
	if (!firsttime && !hijack_last_moved)
		return NO_REFRESH;
	hijack_last_moved = 0;
	clear_hijack_displaybuf(COLOR0);
	(void)draw_string(ROWCOL(0,0), volumelock_menu_label, PROMPTCOLOR);
	rowcol = draw_string(ROWCOL(2,0), empeg_on_dc_power ? "DC: Use: " : "AC: Use: ", PROMPTCOLOR);
	rowcol = draw_string_spaced(rowcol, last_current[hijack_volumelock_enabled], ENTRYCOLOR);
	(void)   draw_string(rowcol, " volume", PROMPTCOLOR);
	return NEED_REFRESH;
}

static void
fsck_move (int direction)
{
	hijack_fsck_disabled = !hijack_fsck_disabled;
	empeg_state_dirty = 1;
}

static const char *disabled_enabled[2] = {"Disabled", "Enabled"};

static int
fsck_display (int firsttime)
{
	unsigned int rowcol;

	if (!firsttime && !hijack_last_moved)
		return NO_REFRESH;
	hijack_last_moved = 0;
	clear_hijack_displaybuf(COLOR0);
	(void)draw_string(ROWCOL(0,0), fsck_menu_label, PROMPTCOLOR);
	rowcol = draw_string(ROWCOL(2,0), "Periodic checking: ", PROMPTCOLOR);
	(void)   draw_string_spaced(rowcol, disabled_enabled[!hijack_fsck_disabled], ENTRYCOLOR);
	return NEED_REFRESH;
}

static int
menuexec_display2 (int firsttime)
{
	if (hijack_menuexec_no) {
		ir_selected = 1; // return to menu
	} else if (firsttime) {
		hijack_menuexec_command = (char *)hijack_userdata;
		up(&hijack_menuexec_sem);	// wake up the daemon
	} else if (hijack_menuexec_command == NULL) {
		ir_selected = 1; // return to menu
	}
	return NO_REFRESH;
}

static void
menuexec_move (int direction)
{
	hijack_menuexec_no = !hijack_menuexec_no;
}

static int
menuexec_display (int firsttime)
{
	unsigned int rowcol;

	if (firsttime)
		hijack_menuexec_no = 1;
	else if (!hijack_last_moved && !ir_selected)
		return NO_REFRESH;
	hijack_last_moved = 0;
	clear_hijack_displaybuf(COLOR0);
	rowcol = draw_string(ROWCOL(0,0), "Execute this Command? ", PROMPTCOLOR);
	(void)   draw_string_spaced(rowcol, no_yes[!hijack_menuexec_no], ENTRYCOLOR);
	rowcol = draw_string(ROWCOL(2,0), (char *)hijack_userdata, COLOR1);
	return NEED_REFRESH;
}

static void
homework_move (int direction)
{
	hijack_homework = !hijack_homework;
	empeg_state_dirty = 1;
}

static int
homework_display (int firsttime)
{
	unsigned int rowcol;

	if (!firsttime && !hijack_last_moved)
		return NO_REFRESH;
	hijack_last_moved = 0;
	clear_hijack_displaybuf(COLOR0);
	(void)draw_string(ROWCOL(0,0), homework_menu_label, PROMPTCOLOR);
	rowcol = draw_string(ROWCOL(2,0), "config.ini mode: ", PROMPTCOLOR);
	(void)   draw_string_spaced(rowcol, homework_labels[hijack_homework], ENTRYCOLOR);
	return NEED_REFRESH;
}

static void
onedrive_move (int direction)
{
	hijack_onedrive = !hijack_onedrive;
	empeg_state_dirty = 1;
}

static const char *onedrive_msg[2] = {"One or Two Drives (slower)", "One Drive only (fast boot)"};

static int
onedrive_display (int firsttime)
{
	if (!firsttime && !hijack_last_moved)
		return NO_REFRESH;
	hijack_last_moved = 0;
	clear_hijack_displaybuf(COLOR0);
	(void)draw_string(ROWCOL(0,0), onedrive_menu_label, PROMPTCOLOR);
	(void)draw_string_spaced(ROWCOL(2,0), onedrive_msg[hijack_onedrive], ENTRYCOLOR);
	return NEED_REFRESH;
}

static void
saveserial_move (int direction)
{
	hijack_saveserial = !hijack_saveserial;
	empeg_state_dirty = 1;
}

static const char *saveserial_msg[2] = {"Player Uses Serial Port", "Apps Use Serial Port"};

static int
saveserial_display (int firsttime)
{
	if (!firsttime && !hijack_last_moved)
		return NO_REFRESH;
	hijack_last_moved = 0;
	clear_hijack_displaybuf(COLOR0);
	(void)draw_string(ROWCOL(0,0), saveserial_menu_label, PROMPTCOLOR);
	(void)draw_string_spaced(ROWCOL(2,0), saveserial_msg[hijack_saveserial], ENTRYCOLOR);
	return NEED_REFRESH;
}

static void
carvisuals_move (int direction)
{
	carvisuals_enabled = !carvisuals_enabled;
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
	(void)draw_string(ROWCOL(0,0), carvisuals_menu_label, PROMPTCOLOR);
	rowcol = draw_string(ROWCOL(2,20), "Restore: ", PROMPTCOLOR);
	(void)   draw_string_spaced(rowcol, disabled_enabled[carvisuals_enabled], ENTRYCOLOR);
	return NEED_REFRESH;
}

static void
timeraction_move (int direction)
{
	timer_action = !timer_action;
	empeg_state_dirty = 1;
}

static const char *timeraction_msg[2] = {"Toggle Standby", "Beep Alarm"};

static int
timeraction_display (int firsttime)
{
	unsigned int rowcol;

	if (!firsttime && !hijack_last_moved)
		return NO_REFRESH;
	hijack_last_moved = 0;
	clear_hijack_displaybuf(COLOR0);
	(void)draw_string(ROWCOL(0,0), timeraction_menu_label, PROMPTCOLOR);
	rowcol = draw_string(ROWCOL(2,0), "On expiry: ", PROMPTCOLOR);
	(void)   draw_string_spaced(rowcol, timeraction_msg[timer_action], ENTRYCOLOR);
	return NEED_REFRESH;
}

static int hightemp_check_threshold (void)
{
	static unsigned long beeping, elapsed;

	if (!hightemp_threshold || hijack_read_temperature() < (hightemp_threshold + HIGHTEMP_OFFSET))
		return 0;
	elapsed = jiffies_since(ir_lasttime) / HZ;
	if (elapsed < 1) {
		beeping = 0;
	} else if (((elapsed >> 2) & 7) == (beeping & 7)) {
		int volume = 3 * ((++beeping >> 2) & 15) + 15;
		hijack_beep(90, 280, volume);
	}
	if (hijack_status == HIJACK_IDLE && elapsed > 4) {
		unsigned int rowcol;
		int color = (elapsed & 1) ? COLOR3 : -COLOR3;
		clear_hijack_displaybuf(COLOR0);
		rowcol = draw_string(ROWCOL(2,18), " Too Hot:", -color);
		(void)draw_temperature(rowcol, hijack_read_temperature(), 32, -color);
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
	(void)draw_string(ROWCOL(0,0), blanker_menu_label, PROMPTCOLOR);
	rowcol = draw_string(ROWCOL(2,20), "Timeout: ", PROMPTCOLOR);
	if (blanker_timeout) {
		(void)draw_hhmmss(rowcol, blanker_timeout * SCREEN_BLANKER_MULTIPLIER, ENTRYCOLOR);
	} else {
		(void)draw_string_spaced(rowcol, "[Off]", ENTRYCOLOR);
	}
	return NEED_REFRESH;
}

static void
blankerfuzz_move (int direction)
{
	blanker_sensitivity += direction;
	if (blanker_sensitivity < 0 || direction == 0)
		blanker_sensitivity = 0;
	else if (blanker_sensitivity > ((1<<SENSITIVITY_BITS)-1))
		blanker_sensitivity  = ((1<<SENSITIVITY_BITS)-1);
	empeg_state_dirty = 1;
}

static int
blankerfuzz_display (int firsttime)
{
	unsigned int rowcol;

	if (firsttime)
		ir_numeric_input = &blanker_sensitivity;
	else if (!hijack_last_moved)
		return NO_REFRESH;
	hijack_last_moved = 0;
	clear_hijack_displaybuf(COLOR0);
	(void)draw_string(ROWCOL(0,0), blankerfuzz_menu_label, PROMPTCOLOR);
	rowcol = draw_string(ROWCOL(2,0), "Examine ", PROMPTCOLOR);
	rowcol = draw_number(rowcol, 100 - (blanker_sensitivity * SENSITIVITY_MULTIPLIER), " %u%% ", ENTRYCOLOR);
	(void)   draw_string(rowcol, " of screen", PROMPTCOLOR);
	return NEED_REFRESH;
}

static int
screen_compare (unsigned long *screen1, unsigned long *screen2)
{
	const unsigned char bitcount4[16] = {0,1,1,2, 1,2,2,3, 1,2,2,3, 2,3,3,4};
	int allowable_fuzz = blanker_sensitivity * (SENSITIVITY_MULTIPLIER * (2 * EMPEG_SCREEN_BYTES) / 100);
	unsigned long *end = screen1 - 1;

	if (allowable_fuzz == 0)
		allowable_fuzz = 2;	// beta7 always has a single blinking pixel on track-info
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

#define BUTTONLED_BITS		3
static unsigned int hijack_buttonled_on_level = 0;
static unsigned int hijack_buttonled_level = 0;

static const char buttonled_menu_label	[] = "Button Illumination Level";

// Front panel illumination info from Hugo:
//
// You can in theory dim them by sending commands to the display - there is no
// ioctl for this. See the display_sendcontrol() function in
// arch/arm/special/empeg_display.c - this sends a single byte to the display.
//
// Looking at the source to the pic that it talks to, the commands are:
//
// 000..239 sets display brightness level. Note that the higher end (nearer 0)
// can overdrive the display by about 10%, the maximum value you should use is 16
// as I remember. You don't get a lot more brightness by overdriving it.
//
// 240 - send display board pic version as IR keypress
// 241 - force send of current button state
// 242 - turn off button illumination (=255)
// 243 - increase illumination by 5 (-=5)
// 244 - turn on button illumination full (=0)
//
// So, in theory to get them to come on, you need to call
// display_sendcontrol(244). Different brightnesses can be acheived by sending
// 242, then multiple 243's to step the brightness up (no idea why I never put
// one to dim the other way...)

static int
headlight_sense_is_active (void)
{
	static unsigned long debouncing  = 0;
	static int sense = 0, prev = 0;
	int new;

	new = !(GPLR & EMPEG_SERIALCTS);   // EMPEG_SERIALCTS is active low.
	if (new != prev) {
		prev = new;
		debouncing = JIFFIES();
	} else if (debouncing && jiffies_since(debouncing) > (HZ+(HZ/2))) {
		debouncing = 0;
		sense = new;
	}
	return sense;
}

static unsigned int buttonled_command = 0;

static void	// invoked from empeg_display.c
hijack_adjust_buttonled (int power)
{
	const unsigned char bright_levels[1<<BUTTONLED_BITS] = {0, 1, 2, 4, 7, 14, 24, 255};
	int brightness;

	// illumination command already in progress?
	if (buttonled_command) {
		if (display_sendcontrol_part2(buttonled_command))
			return;	// still busy with command
		buttonled_command = 0;
	}

	// start a new command if level needs adjustment:
	if (power)
		brightness = hijack_buttonled_on_level;
	else if (hijack_buttonled_on_level)
		brightness = hijack_buttonled_off_level;
	else
		brightness = 0;
	if (headlight_sense_is_active()) {
		if (hijack_buttonled_dim_level)
			brightness = hijack_buttonled_dim_level;
		else
			brightness = (brightness + 1) / 2;
	}
	brightness = bright_levels[brightness];
	if (hijack_buttonled_level != brightness) {
		// don't do adjustments until buttons have been idle for half a second or more
		if (jiffies_since(ir_lastevent) >= (HZ/2)) {
			if (brightness == 255)
				buttonled_command = 244;	// select full brightness
			else if (hijack_buttonled_level > brightness)
				buttonled_command = 242;	// turn off LEDs and ramp up again
			else if (hijack_buttonled_level < brightness)
				buttonled_command = 243;	// brighten LEDs by 5/255 (2%)
			if (buttonled_command) {
				if (display_sendcontrol_part1(1)) {
					buttonled_command = 0;	// busy, try again later
				} else {
					switch (buttonled_command) {
						case 242: hijack_buttonled_level = 0;	break;
						case 243: hijack_buttonled_level++;	break;
						case 244: hijack_buttonled_level = 255;	break;
					}
				}
			}
		}
	}
}

static void
buttonled_move (int direction)
{
	if (direction == 0) {
		hijack_buttonled_on_level = 0;
	} else {
		int level = hijack_buttonled_on_level + direction;
		if (level >= 0 && level < (1<<BUTTONLED_BITS))
			hijack_buttonled_on_level = level;
	}
	empeg_state_dirty = 1;
}

static int
buttonled_display (int firsttime)
{
	unsigned int rowcol, level;
	char buf[4];

	if (firsttime)
		ir_numeric_input = &hijack_buttonled_on_level;
	else if (!hijack_last_moved)
		return NO_REFRESH;
	hijack_last_moved = 0;
	clear_hijack_displaybuf(COLOR0);
	(void) draw_string(ROWCOL(0,0), buttonled_menu_label, PROMPTCOLOR);
	rowcol = draw_string(ROWCOL(2,0), "Brightness: ", PROMPTCOLOR);
	level = hijack_buttonled_on_level;
	if (!level)
		(void) draw_string_spaced(rowcol, "[off]", ENTRYCOLOR);
	else if (level == ((1<<BUTTONLED_BITS)-1))
		(void) draw_string_spaced(rowcol,"[max]", ENTRYCOLOR);
	else {
		sprintf(buf, " %u ", level);
		(void) draw_string(rowcol, buf, ENTRYCOLOR);
	}
	return NEED_REFRESH;
}

static unsigned short
get_current_mixer_source (void)
{
	unsigned short source;
	int input = hijack_current_mixer_input;

	switch (input) {
		case INPUT_AUX:	// Aux in
			source = IR_FLAGS_AUX;
			break;
		case INPUT_PCM:	// Main/mp3
			source = IR_FLAGS_MAIN;
			break;
		//case INPUT_RADIO_FM:// FM Tuner
		//case INPUT_RADIO_AM:// AM Tuner
		default:
			source = IR_FLAGS_TUNER;
			break;
	}
	return source;
}

#ifdef EMPEG_KNOB_SUPPORTED

static void
knobdata_move (int direction)
{
	knobdata_index = direction ? (knobdata_index + direction) & KNOBDATA_MASK : 0;
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
	(void)draw_string(ROWCOL(0,0), knobdata_menu_label, PROMPTCOLOR);
	rowcol = draw_string(ROWCOL(2,0), "Quick press = ", PROMPTCOLOR);
	(void)draw_string_spaced(rowcol, knobdata_labels[knobdata_index], ENTRYCOLOR);
	return NEED_REFRESH;
}

int player_version = 0;		// used in kftpd.c, possibly elsewhere

static void
get_player_version (void)
{
	extern int sys_newstat(char *, struct stat *);
	mm_segment_t old_fs = get_fs();
	struct stat st;

	set_fs(KERNEL_DS);
	if (0 == sys_newstat("/proc/self/exe", &st))
		player_version = st.st_size;
	set_fs(old_fs);
}

#endif // EMPEG_KNOB_SUPPORTED

static unsigned long knobseek_lasttime;

static void
knobseek_move_visuals (int direction)
{
	unsigned int button;

	button = (direction > 0) ? IR_RIO_VISUAL_PRESSED : IR_PREV_VISUAL_PRESSED;
	hijack_enq_button_pair(button);
}

static void
knobseek_move_tuner (int direction)
{
	unsigned int button;

	button = (direction > 0) ? IR_KPREV_PRESSED : IR_KNEXT_PRESSED;
	hijack_enq_button_pair(button);
}

static void
knobseek_move_other (int direction)
{
	unsigned int button;

	if (jiffies_since(knobseek_lasttime) >= (HZ/4)) {
		knobseek_lasttime = JIFFIES();
		button = (direction > 0) ? IR_RIGHT_BUTTON_PRESSED : IR_LEFT_BUTTON_PRESSED;
		hijack_enq_button_pair(button);
	}
}

static int
knobseek_display (int firsttime)
{
	static hijack_geom_t geom;

	if (firsttime) {
		unsigned int rowcol;
		const char *msg;
		if (hijack_movefunc == knobseek_move_visuals) {
			geom = (hijack_geom_t){8, 8+6+KFONT_HEIGHT, 44, EMPEG_SCREEN_COLS-44};
			msg  = "Visuals";
		} else if (get_current_mixer_source() == IR_FLAGS_TUNER) {
			geom = (hijack_geom_t){8, 8+6+KFONT_HEIGHT, 8, EMPEG_SCREEN_COLS-50};
			msg  = "Manual Tuning";
			hijack_movefunc = knobseek_move_tuner;
		} else {
			geom = (hijack_geom_t){8, 8+6+KFONT_HEIGHT, 20, EMPEG_SCREEN_COLS-20};
			msg  = "Knob \"Seek\" Mode";
			hijack_movefunc = knobseek_move_other;
		}
		rowcol = (geom.first_row+4)|((geom.first_col+6)<<16);
		create_overlay(&geom);
		rowcol = draw_string(rowcol, msg, COLOR3);
		hijack_knobseek = 1;
		knobseek_lasttime = hijack_last_moved = JIFFIES();
	} else if (ir_selected || jiffies_since(hijack_last_moved) >= (HZ*5)) {
		hijack_knobseek = 0;
		hijack_deactivate(HIJACK_IDLE_PENDING);
	}
	return NO_REFRESH;	// gets overridden if overlay still active
}

static ir_translation_t *
ir_next_match (ir_translation_t *table, unsigned int button)
{
	button &= ~BUTTON_FLAGS;
	while (1) {
		if (table == NULL)
			table = (ir_translation_t *)ir_translate_table;
		else
			((unsigned int *)table) += (sizeof(ir_translation_t) / sizeof(unsigned int)) + table->count;
		if (!table || table->old == -1)
			return NULL;
		if ((table->old & ~BUTTON_FLAGS) == button)
			return table;
	}
}

static int		popup_index;
static unsigned long	popup_got_press;
static ir_translation_t	*current_popup = NULL;
static unsigned int	popup_buttonlist[] = {5, IR_KNOB_PRESSED, IR_KNOB_RELEASED, IR_RIO_MENU_PRESSED, IR_RIO_MENU_RELEASED};

static void
popup_move (int direction)
{
	if (direction == 0) {
		hijack_deactivate(HIJACK_IDLE);
	} else if (!popup_got_press) {
		popup_index += direction;
		if (popup_index < 0)
			popup_index = current_popup->count - 1;
		else if (popup_index >= current_popup->count)
			popup_index = 0;
		current_popup->popup_index = popup_index;
		if (current_popup->old == IR_FAKE_POPUP0) {
			popup0_index = current_popup->popup_index & POPUP0_MASK;
			empeg_state_dirty = 1;
		}
	}
}

static int
popup_display (int firsttime)
{
	unsigned long flags;
	hijack_buttondata_t data;
	static const hijack_geom_t geom = {8, 8+6+KFONT_HEIGHT, 4, EMPEG_SCREEN_COLS-4};
	int rc = NO_REFRESH;
	unsigned int button;

	if (firsttime) {
		popup_got_press = 0;
		popup_index = current_popup->popup_index;
		if (popup_index >= current_popup->count)
			popup_index = 0;
		hijack_last_moved = JIFFIES();
		ir_numeric_input = &popup_index;	// allows cancel/top to reset it to 0
		hijack_buttonlist = popup_buttonlist;
		hijack_initq(&hijack_userq, 'U');
		create_overlay(&geom);
	}

	button = current_popup->new[popup_index];
	if (popup_got_press) {
		rc = SHOW_PLAYER;
	} else if (jiffies_since(hijack_last_moved) >= (HZ*4)) {
		hijack_deactivate(HIJACK_IDLE);
	} else {
		char buf[16];
		unsigned int rowcol = (geom.first_row+4)|((geom.first_col+6)<<16);
		rowcol = draw_string(rowcol, "Select Action: ", COLOR3);
		clear_text_row(rowcol, geom.last_col-4, 1);
		(void)draw_string_spaced(rowcol, get_button_name(button, buf), ENTRYCOLOR);
		rc = NEED_REFRESH;
	}
	save_flags_cli(flags);
	while (hijack_status == HIJACK_ACTIVE && hijack_button_deq(&hijack_userq, &data, 0)) {
		if (!IS_RELEASE(data.button)) {
			if (!popup_got_press) {
				unsigned int b = button & ~BUTTON_FLAGS;
				popup_got_press = JIFFIES();
				hijack_enq_button(&hijack_playerq, b, 0);
				if (button & BUTTON_FLAGS_LONGPRESS)
					hijack_enq_release(&hijack_playerq, b, LONGPRESS_DELAY);
			}
		} else {
			if (popup_got_press) {
				if (!(button & BUTTON_FLAGS_LONGPRESS))
					hijack_enq_release(&hijack_playerq, button & ~BUTTON_FLAGS, jiffies_since(popup_got_press));
				hijack_deactivate(HIJACK_IDLE);
			}
		}
	}
	restore_flags(flags);
	return rc;
}

#define GAME_COLS		(EMPEG_SCREEN_COLS/2)
#define GAME_VBOUNCE		0xff
#define GAME_BRICKS		0xee
#define GAME_BRICKS_ROW		5
#define GAME_HBOUNCE		0x77
#define GAME_BALL		0xff
#define GAME_OVER		0x11
#define GAME_PADDLE_SIZE	8

static short game_over, game_row, game_col, game_hdir, game_vdir, game_paddle_col, game_paddle_lastdir, game_speed, game_bricks, game_moves;
static unsigned long game_ball_last_moved, game_animtime;
       unsigned int *hijack_game_animptr = NULL;	// written by empeg_display.c

static int
game_finale (void)
{
	static int framenr, frameadj;
	unsigned long rowcol;
	int score;

	if (game_bricks) {  // Lost game?
		if (jiffies_since(game_ball_last_moved) < (HZ*3/2))
			return NO_REFRESH;
		if (game_animtime++ == 0) {
			(void)draw_string_spaced(ROWCOL(2,4), hijack_vXXX_by_Mark_Lord, -COLOR3);
			goto draw_score;
		}
		if (jiffies_since(game_ball_last_moved) < (HZ*10))
			return NO_REFRESH;
		ir_selected = 1; // return to menu
		return NEED_REFRESH;
	}
	if (jiffies_since(game_animtime) < (HZ/(ANIMATION_FPS-2))) {
		rowcol = draw_string(ROWCOL(2,44), "You Win!", COLOR3);
		goto draw_score;
	}
	if (game_animtime == 0) { // first frame?
		framenr = 0;
		frameadj = 1;
	} else if (framenr < 0) { // animation finished?
		ir_selected = 1; // return to menu
		return NEED_REFRESH;
	} else if (!hijack_game_animptr[framenr]) { // last frame?
		frameadj = -1;  // play it again, backwards
		framenr += frameadj;
	}
	display_animation_frame((unsigned char *)hijack_displaybuf,
							(unsigned char *)(hijack_game_animptr + hijack_game_animptr[framenr]));
	framenr += frameadj;
	game_animtime = JIFFIES();
	return NEED_REFRESH;
draw_score:
	rowcol = draw_string(ROWCOL(1,8), "Score=", PROMPTCOLOR);
	score = GAME_COLS - 2 - game_bricks;
	rowcol = draw_number(rowcol, score, "%02d", PROMPTCOLOR);
	rowcol = draw_string(rowcol, ", Moves=", PROMPTCOLOR);
	(void)   draw_number(rowcol, game_moves, "%d", PROMPTCOLOR);
	return NEED_REFRESH;
}

static void
game_move (int direction)
{
	unsigned char *paddlerow = hijack_displaybuf[EMPEG_SCREEN_ROWS-3];
	int i = 3;

	while (i-- > 0) {
		if (direction < 0) {
			if (game_paddle_col > 1) {
				if (game_bricks)
					game_moves++;
				paddlerow[--game_paddle_col + GAME_PADDLE_SIZE] = 0;
				if (paddlerow[game_paddle_col] != 0)
					--game_row; // scoop up the ball
				paddlerow[game_paddle_col] = GAME_VBOUNCE;
			}
		} else {
			if (game_paddle_col < (GAME_COLS - GAME_PADDLE_SIZE - 1)) {
				if (game_bricks)
					game_moves++;
				paddlerow[game_paddle_col] = 0;
				if (paddlerow[game_paddle_col + GAME_PADDLE_SIZE] != 0)
					--game_row; // scoop up the ball
				paddlerow[game_paddle_col++ + GAME_PADDLE_SIZE] = GAME_VBOUNCE;
			}
		}
	}
	game_paddle_lastdir = direction;
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
	ir_selected = 0; // prevent accidental exit from game
	if (ir_left_down && jiffies_since(ir_left_down) >= (HZ/15)) {
		ir_left_down = JIFFIES();
		game_move(-1);
	} else if (ir_right_down && jiffies_since(ir_right_down) >= (HZ/15)) {
		ir_right_down = JIFFIES();
		game_move(1);
	}
	if (jiffies_since(game_ball_last_moved) < (HZ/game_speed))
		return (jiffies_since(hijack_last_moved) > jiffies_since(game_ball_last_moved)) ? NO_REFRESH : NEED_REFRESH;
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
	game_moves = 0;
	game_over = 0;
	game_speed = 16;
	game_animtime = 0;
	return NEED_REFRESH;
}

// LittleBlueThing's Boot Graphic menu entry
typedef struct bootg_s {
	const char		*label;
	unsigned char   type;
	unsigned char   *data;
} bootg_t;

static bootg_t bootg_options[] = {
	{"Boot Animation",'A', NULL},
	{"Empeg Animation",'A',empeg_ani},
	{"Car Logo",'L',(unsigned char*)(EMPEG_FLASHBASE+0xa000+4+EMPEG_SCREEN_SIZE)},
	{"Home Logo",'L',(unsigned char*)(EMPEG_FLASHBASE+0xa000+4)},
	{"BSOD",'L',nohd_img},
	{0,0,0 } };  // 0 termination required
#define BOOTG_MAX_OPTION 4


short bootg_sel = 0;
short bootg_dodisplay = 0;  // 0:nothing 1:logo/frame#1 2:animation
short bootg_domenu = 0;     // menu selection changed: redisplay
short bootg_inmenu = 0;     // showing menu?
int	bootg_frame = 0; // frame count in animation
unsigned int  bootg_framet = 0;  // time frame displayed
unsigned int* bootg_anim; // start of animation in memory

static void
bootg_move (int direction)
{
	bootg_sel += direction;
	if (bootg_sel < 0)
		bootg_sel = BOOTG_MAX_OPTION;
	else if (bootg_options[bootg_sel].type == 0)
		bootg_sel   = 0;
}

static int
bootg_display (int firsttime)
{
	unsigned int rowcol;
	hijack_buttondata_t data;

	if (firsttime || bootg_domenu) {
		// Show the menu text
		clear_hijack_displaybuf(COLOR0);
		(void) draw_string(ROWCOL(0,0), "==View boot graphics==\nKnob-press/Down to view\nRight/Left: change  Up: Exit", PROMPTCOLOR);
		rowcol = draw_string(ROWCOL(3,0), "[", PROMPTCOLOR);
		rowcol = draw_string_spaced(rowcol, bootg_options[bootg_sel].label, ENTRYCOLOR);
		(void)  draw_string(rowcol, "]", PROMPTCOLOR);
		bootg_inmenu = 1;

		// Do our own button handling
		hijack_buttonlist = intercept_all_buttons;
		hijack_initq(&hijack_userq, 'U');
		bootg_domenu= 0;
		bootg_dodisplay=0; // stop any ongoing animation
		return NEED_REFRESH;
	}	
	// Do our own button handling
	// modified to hijack standard behaviour
	if (hijack_button_deq(&hijack_userq, &data, 0)) {
		switch (data.button) {
		case IR_RIO_CANCEL_PRESSED:
		case IR_KW_STAR_PRESSED:
		case IR_TOP_BUTTON_PRESSED:
			if (bootg_inmenu==0)
				bootg_domenu = 1;
			else
				hijack_deactivate(HIJACK_IDLE_PENDING);
			break;
		case IR_KW_NEXTTRACK_PRESSED:
		case IR_RIO_NEXTTRACK_PRESSED:
		case IR_RIGHT_BUTTON_PRESSED:
		case IR_KNOB_RIGHT:
			bootg_move(1);
			bootg_domenu = 1;
			break;
		case IR_KW_PREVTRACK_PRESSED:
		case IR_RIO_PREVTRACK_PRESSED:
		case IR_LEFT_BUTTON_PRESSED:
		case IR_KNOB_LEFT:
			bootg_move(-1);
			bootg_domenu = 1;
			break;
		case IR_KW_CD_PRESSED:
		case IR_RIO_MENU_PRESSED:
		case IR_BOTTOM_BUTTON_PRESSED:
		case IR_KNOB_PRESSED:
			bootg_inmenu = 0;
			bootg_dodisplay = 1;
			break;
		}
	}

	if (bootg_dodisplay == 1 ) {
		// Logo or animation preparation
		switch (bootg_options[bootg_sel].type) {
		case 'L':
			// logo
			memcpy(hijack_displaybuf, bootg_options[bootg_sel].data, EMPEG_SCREEN_SIZE);
			bootg_dodisplay=0;
			return NEED_REFRESH;
		case 'A':
			// animation
			bootg_frame = 0;
			if (bootg_options[bootg_sel].data)
				bootg_anim = (unsigned int*)bootg_options[bootg_sel].data ;
			else
				bootg_anim = hijack_game_animptr; // aha - reuse :)
			if (bootg_anim)
				bootg_dodisplay=2; // 2 means ongoing display
			else
			{
				bootg_dodisplay=0; // No animation (maybe display message)
				clear_hijack_displaybuf(COLOR0);
				(void)draw_string(ROWCOL(0,0), "No boot animation found", PROMPTCOLOR);
			}
			return NEED_REFRESH;
		}
	}
	if (bootg_dodisplay == 2 ) {
		// ongoing animation
		if 	(jiffies_since(bootg_framet) < (HZ/ANIMATION_FPS))
			return NO_REFRESH;

		if (!bootg_anim[bootg_frame]) {
			bootg_frame = 0;
			bootg_dodisplay = 0; //stop animation
			return NO_REFRESH;  // Leave last frame on screen... (blanker still works)
		}

		// use consolidated code
		display_animation_frame((unsigned char *)hijack_displaybuf,
								(unsigned char *)bootg_anim + bootg_anim[bootg_frame]);
		bootg_frame += 1;
		bootg_framet = JIFFIES();
		return NEED_REFRESH;
	}
	return NO_REFRESH;
}

/* Time Alignment Setup Code - Christian Hack 2002 - christianh@pdd.edmi.com.au */
/* Allows user adjustment of channel delay for time alignment. Time alignment   */
/* is actually done in empeg_audio3.c. thru global var hijack_delaytime         */
static void
delaytime_move (int direction)
{
	if (direction == 0) {
		hijack_delaytime = 0;
	} else {
		/* Delay time is simply in 0.1ms increments */
		hijack_delaytime += direction;

		/* Value is in tenths of ms - negative values mean right channel delayed, positive = left */
		if (hijack_delaytime > 127)
			hijack_delaytime = 127;
		else if (hijack_delaytime < -127)
			hijack_delaytime = -127;
	}
	empeg_state_dirty = 1;
}

static int
delaytime_display (int firsttime)
{
	unsigned int rowcol, tmp;
	unsigned char buf[20];

	if (firsttime)
		ir_numeric_input = &hijack_delaytime;
	else if (!hijack_last_moved)
		return NO_REFRESH;
	hijack_last_moved = 0;
	clear_hijack_displaybuf(COLOR0);
	(void) draw_string(ROWCOL(0,0), delaytime_menu_label, PROMPTCOLOR);
	rowcol = draw_string(ROWCOL(2,0), "Shift: ", PROMPTCOLOR);
	if (hijack_delaytime) {
		/* Remove sign, it's decoded also in empeg_audio3.c */
		tmp = (hijack_delaytime >= 0) ? (hijack_delaytime) : (-hijack_delaytime);
		sprintf(buf, "%2d.%d msec %s", (tmp /  10), (tmp % 10), (hijack_delaytime > 0) ? "Right" : "Left");
		(void) draw_string_spaced(rowcol, buf, ENTRYCOLOR);

		/* Speed of sound is roughly 1200km/h = 333m/s */
		tmp = tmp * 333;
		sprintf(buf, "Distance: %d.%d cm", (tmp / 100), (tmp % 100));
		(void) draw_string(ROWCOL(3,0), buf, PROMPTCOLOR);
	} else {
		rowcol = draw_string_spaced(rowcol, "[Off]", ENTRYCOLOR);
	}
	return NEED_REFRESH;
}

// wrapper function used to set treble and bass on boot.
void
hijack_tone_init (void)
{
	(void) hijack_tone_set(hijack_db_table[hijack_bass_adj].value, hijack_bass_freq, hijack_bass_q,
		hijack_db_table[hijack_treble_adj].value, hijack_treble_freq, hijack_treble_q);
	//printk("hijack_tone_init completed.\n");
}

#ifdef CONFIG_HIJACK_TUNER
#ifdef CONFIG_HIJACK_TUNER_ADJUST
static void
retune_radio (void)
{
	switch (hijack_current_mixer_input) {
		case INPUT_RADIO_AM:
		case INPUT_RADIO_FM:
			hijack_enq_button_pair(IR_RIO_TUNER_PRESSED);
			hijack_enq_button_pair(IR_RIO_TUNER_PRESSED);
			break;
	}
}

static int ir_numeric_limit;

static void
numeric_move (int direction)
{
	int val = *ir_numeric_input;
	if (direction == 0)
		val = 0;
	else
		val += direction;
	if (val < 0)
		val = ir_numeric_limit;
	else if (val > ir_numeric_limit)
		val = 0;
	*ir_numeric_input = val;
	empeg_state_dirty = 1;
}

int hijack_if2_bw = 0;

static int
if2_bw_display (int firsttime)
{
	static const char *if2_bw_table[5] = {"default", "Dynamic", "Wide", "Medium", "Narrow"};
	unsigned int rowcol;

	if (firsttime) {
		ir_numeric_input = &hijack_if2_bw;
		ir_numeric_limit = 4;
	} else if (!hijack_last_moved)
		return NO_REFRESH;
	hijack_last_moved = 0;
	clear_hijack_displaybuf(COLOR0);
	(void) draw_string(ROWCOL(0,0), if2_bw_menu_label, PROMPTCOLOR);
	rowcol = draw_string(ROWCOL(2,30), "Bandwidth: ", PROMPTCOLOR);
	(void) draw_string_spaced(rowcol, if2_bw_table[hijack_if2_bw], ENTRYCOLOR);
	retune_radio();
	return NEED_REFRESH;
}

int hijack_agc = 0;

static int
agc_display (int firsttime)
{
	static const char *agc_table[5] = {"default", "16mV", "12mV", "8mV", "4mV"};
	unsigned int rowcol;

	if (firsttime) {
		ir_numeric_input = &hijack_agc;
		ir_numeric_limit = 4;
	} else if (!hijack_last_moved)
		return NO_REFRESH;
	hijack_last_moved = 0;
	clear_hijack_displaybuf(COLOR0);
	(void) draw_string(ROWCOL(0,0), agc_menu_label, PROMPTCOLOR);
	rowcol = draw_string(ROWCOL(2,30), "Threshold: ", PROMPTCOLOR);
	(void) draw_string_spaced(rowcol, agc_table[hijack_agc], ENTRYCOLOR);
	retune_radio();
	return NEED_REFRESH;
}

int hijack_dx_lo = 0;

static int
dx_lo_display (int firsttime)
{
	unsigned int rowcol;

	if (firsttime) {
		ir_numeric_input = &hijack_dx_lo;
		ir_numeric_limit = 1;
	} else if (!hijack_last_moved)
		return NO_REFRESH;
	hijack_last_moved = 0;
	clear_hijack_displaybuf(COLOR0);
	(void) draw_string(ROWCOL(0,0), dx_lo_menu_label, PROMPTCOLOR);
	rowcol = draw_string(ROWCOL(2,30), "Mode: ", PROMPTCOLOR);
	rowcol = draw_string_spaced(rowcol, hijack_dx_lo ? "LO(local)" : "DX(distant)", ENTRYCOLOR);
	retune_radio();
	return NEED_REFRESH;
}
#endif // CONFIG_HIJACK_TUNER
#endif // CONFIG_HIJACK_TUNER_ADJUST

static void
tone_move (int direction)
{
	int val = *ir_numeric_input;
	if (direction == 0) {
		val = 0;
	} else if (direction > 0) {
		if (val == 12)
			val = 0;
		else if (val != 6)
			++val;
	} else { // (direction < 0)
		if (val == 0)
			val = 12;
		else if (val != 7)
			--val;
	}
	*ir_numeric_input = val;
	empeg_state_dirty = 1;
	(void) hijack_tone_set(hijack_db_table[hijack_bass_adj].value, hijack_bass_freq, hijack_bass_q,
		hijack_db_table[hijack_treble_adj].value, hijack_treble_freq, hijack_treble_q);
}

static int
bass_display (int firsttime)
{
	unsigned int rowcol;

	if (firsttime)
		ir_numeric_input = &hijack_bass_adj;
	else if (!hijack_last_moved)
		return NO_REFRESH;
	hijack_last_moved = 0;
	clear_hijack_displaybuf(COLOR0);
	(void) draw_string(ROWCOL(0,0), bass_menu_label, PROMPTCOLOR);
	rowcol = draw_string(ROWCOL(2,30), "Adjust: ", PROMPTCOLOR);
	(void) draw_string_spaced(rowcol, hijack_db_table[hijack_bass_adj].name, ENTRYCOLOR);
	return NEED_REFRESH;
}

static int
treble_display (int firsttime)
{
	unsigned int rowcol;

	if (firsttime)
		ir_numeric_input = &hijack_treble_adj;
	else if (!hijack_last_moved)
		return NO_REFRESH;
	hijack_last_moved = 0;
	clear_hijack_displaybuf(COLOR0);
	(void) draw_string(ROWCOL(0,0), treble_menu_label, PROMPTCOLOR);
	rowcol = draw_string(ROWCOL(2,30), "Adjust: ", PROMPTCOLOR);
	(void) draw_string_spaced(rowcol, hijack_db_table[hijack_treble_adj].name, ENTRYCOLOR);
	return NEED_REFRESH;
}

static void
hightemp_move (int direction)
{
	if (hightemp_threshold == 0 && direction > 0)
		hightemp_threshold = 55 - HIGHTEMP_OFFSET;
	else
		hightemp_threshold += direction;
	if (hightemp_threshold < 0 || direction == 0)
		hightemp_threshold = 0;
	else if (hightemp_threshold > ((1<<HIGHTEMP_BITS)-1))
		hightemp_threshold  = ((1<<HIGHTEMP_BITS)-1);
	empeg_state_dirty = 1;
}

static int
hightemp_display (int firsttime)
{
	unsigned int rowcol;

	if (firsttime)
		ir_numeric_input = &hightemp_threshold;
	else if (jiffies_since(hijack_last_refresh) < (HZ*2) && !hijack_last_moved)
		return NO_REFRESH;
	hijack_last_moved = 0;
	clear_hijack_displaybuf(COLOR0);
	(void) draw_string(ROWCOL(0,0), hightemp_menu_label, PROMPTCOLOR);
	rowcol = draw_string(ROWCOL(1,0), "Threshold: ", PROMPTCOLOR);
	if (hightemp_threshold)
		(void)draw_temperature(rowcol, hightemp_threshold + HIGHTEMP_OFFSET, 32, ENTRYCOLOR);
	else
		rowcol = draw_string_spaced(rowcol, "[Off]", ENTRYCOLOR);
	rowcol = draw_string(ROWCOL(2,0), "Currently: ", PROMPTCOLOR);
	(void)draw_temperature(rowcol, hijack_read_temperature(), 32, PROMPTCOLOR);
	rowcol = draw_string(ROWCOL(3,0), "Corrected by: ", PROMPTCOLOR);
	(void)draw_temperature(rowcol, hijack_temperature_correction, 0, PROMPTCOLOR);
	return NEED_REFRESH;
}

#ifdef EMPEG_FIXTEMP
static void
fixtemp_pulse16 (void)
{
	unsigned long flags;
	int i;

	GPSR = EMPEG_DS1821;
	udelay(20);
	save_flags_clif(flags);
	for (i = 0; i < 16; ++i) {
		udelay(1);
		GPCR = EMPEG_DS1821;
		udelay(1);
		GPSR = EMPEG_DS1821;
	}
	restore_flags(flags);
}

static void
fixtemp_move (int direction)
{
}

static int
fixtemp_display (int firsttime)
{
	if (firsttime) {
		/*
		 * On entry, we assume user has pin-8 pulled to GND.
		 * So now pulse DQ low 16 times:
		 */
		fixtemp_pulse16();
		/*
		 * Now prompt user to pull pin-8 high again:
		 */
		clear_hijack_displaybuf(COLOR0);
		(void) draw_string(ROWCOL(1,0), "Apply +5V to pin-8,", PROMPTCOLOR);
		(void) draw_string(ROWCOL(2,0), "and turn the knob.", PROMPTCOLOR);
		hijack_last_moved = 0;
		return NEED_REFRESH;
	} else if (hijack_last_moved) {
		/*
		 * Now rewrite the config register in the internal eeprom:
		 */
		init_temperature(1);
		activate_dispfunc(hightemp_display, hightemp_move, 0);
	}
	return NO_REFRESH;
}
#endif /* EMPEG_FIXTEMP */

static void
do_reboot (struct display_dev *dev)
{
	if (hijack_reboot == 0) {
		hijack_reboot = 2;
	} else if (hijack_reboot == 1) {
		hijack_reboot = 2;
		clear_hijack_displaybuf(COLOR0);
		(void) draw_string(ROWCOL(2,32), "Rebooting..", PROMPTCOLOR);
		display_blat(dev, (char *)hijack_displaybuf);
		hijack_last_moved = JIFFIES();
		state_cleanse();	// Ensure flash savearea is updated
		remount_drives(0);
	}
	if (jiffies_since(hijack_last_moved) >= HZ && hijack_reboot == 2) {
		hijack_reboot = 3;
		state_cleanse();	// Ensure flash savearea is updated
	}
	if (jiffies_since(hijack_last_moved) >= (HZ+(HZ/2))) {
		unsigned long flags;
		save_flags_clif(flags);	// clif is necessary for machine_restart
		real_display_ioctl(dev, EMPEG_DISPLAY_POWER, 0);
		machine_restart(NULL);	// never returns
	}
}

static int
reboot_display (int firsttime)
{
	static unsigned short left_pressed, right_pressed;
	hijack_buttondata_t data;
	int rc;

	if (firsttime) {
		clear_hijack_displaybuf(COLOR0);
		(void) draw_string(ROWCOL(0,0), "Press & hold Left/Right\n  buttons to reboot.\n(or press 2 on the remote)\nAny other button aborts", PROMPTCOLOR);
		left_pressed = right_pressed = 0;
		hijack_buttonlist = intercept_all_buttons;
		hijack_initq(&hijack_userq, 'U');
		return NEED_REFRESH;
	}
	rc = NO_REFRESH;
	if (hijack_button_deq(&hijack_userq, &data, 0)) {
		switch (data.button) {
			case IR_LEFT_BUTTON_PRESSED:
				left_pressed = 1;
				break;
			case IR_RIGHT_BUTTON_PRESSED:
				right_pressed = 1;
				break;
			case IR_KW_2_PRESSED:
			case IR_RIO_2_PRESSED:
				left_pressed = right_pressed = 1;
				break;
			default:
				if (!IS_RELEASE(data.button)) {
					hijack_buttonlist = NULL;
					ir_selected = 1; // return to main menu
				}
				break;
		}
		if (left_pressed && right_pressed) {
			hijack_reboot = 1;
			rc = NEED_REFRESH;
		}
	}
	return rc;
}

static int
kill_and_pause_player (int firsttime)
{
	static int last_pid = 0;
	int signal = (hijack_player_pid == last_pid) ? SIGKILL : SIGTERM;

	last_pid = hijack_player_pid;
	hijack_force_pause_player = 1;
	hijack_deactivate(HIJACK_IDLE);
	kill_proc(hijack_player_pid, signal, 1);
	return NO_REFRESH;
}

static int
showbutton_display (int firsttime)
{
	static unsigned int *saved_table, prev[4], counter;
	hijack_buttondata_t data;
	unsigned long flags;
	int i;

	save_flags_cli(flags);	// needed while we muck with the ir_translate_table[]
	if (firsttime) {
		counter = 0;
		prev[0] = prev[1] = prev[2] = prev[3] = IR_NULL_BUTTON;
		hijack_buttonlist = intercept_all_buttons;
		hijack_initq(&hijack_userq, 'U');
		// disable IR translations
		saved_table = ir_translate_table;
		ir_translate_table = NULL;
	}
	if (hijack_button_deq(&hijack_userq, &data, 0)) {
		int released = IS_RELEASE(data.button);
		if (++counter > 99)
			counter = 0;
		if (prev[0] == IR_NULL_BUTTON && released) {
			// ignore it: left over from selecting us off the menu
		} else if (released && data.button == prev[1]) {
			ir_translate_table = saved_table;
			ir_selected = 1; // return to main menu
		} else {
			for (i = 2; i >= 0; --i)
				prev[i+1] = prev[i];
			prev[0] = data.button;
		}
	}
	restore_flags(flags);
#ifdef EMPEG_STALK_SUPPORTED
	if (firsttime)
		most_recent_stalk_code = 0xff;
#endif // EMPEG_STALK_SUPPORTED
	if (firsttime || prev[0] != IR_NULL_BUTTON) {
		unsigned long rowcol;
		clear_hijack_displaybuf(COLOR0);
		rowcol=draw_string(ROWCOL(0,0), showbutton_menu_label, PROMPTCOLOR);
		rowcol += (6<<16);
		(void) draw_number(rowcol, counter, " %02d ", ENTRYCOLOR);
		(void) draw_string(ROWCOL(1,0), "Repeat any button to exit", PROMPTCOLOR);
#ifdef EMPEG_STALK_SUPPORTED
		if (most_recent_stalk_code != 0xff) {
			unsigned char buf[16];
			sprintf(buf, "Stalk=0x%02x", most_recent_stalk_code);
			(void)draw_string(ROWCOL(2,4), buf, PROMPTCOLOR);
		} else
#endif // EMPEG_STALK_SUPPORTED
		if (prev[3] != IR_NULL_BUTTON)
			(void)draw_number(ROWCOL(2,4), prev[3], "%08X", PROMPTCOLOR);
		if (prev[2] != IR_NULL_BUTTON)
			(void)draw_number(ROWCOL(2,(EMPEG_SCREEN_COLS/2)), prev[2], " %08X ", PROMPTCOLOR);
		if (prev[1] != IR_NULL_BUTTON)
			(void)draw_number(ROWCOL(3,4), prev[1], "%08X", PROMPTCOLOR);
		if (prev[0] != IR_NULL_BUTTON)
			(void)draw_number(ROWCOL(3,(EMPEG_SCREEN_COLS/2)), prev[0], " %08X ", ENTRYCOLOR);
		return NEED_REFRESH;
	}
	return NO_REFRESH;
}

static menu_item_t menu_table [MENU_MAX_ITEMS] = {
	{"Auto Volume Adjust",		voladj_display,		voladj_move,		0},
	{"Boot Graphics",		    bootg_display,	bootg_move,		0},
	{"Break-Out Game",		game_display,		game_move,		0},
	{ showbutton_menu_label,	showbutton_display,	NULL,			0},
	{ buttonled_menu_label,		buttonled_display,	buttonled_move,		0},
	{ timeraction_menu_label,	timeraction_display,	timeraction_move,	0},
	{ timer_menu_label,		timer_display,		timer_move,		0},
	{ fsck_menu_label,		fsck_display,		fsck_move,		0},
#ifdef EMPEG_FIXTEMP
	{ "Fix Temperature Sensor",	fixtemp_display,	fixtemp_move,		0},
#endif
	{"Font Display",		kfont_display,		NULL,			0},
	{ forcepower_menu_label,	forcepower_display,	forcepower_move,	0},
	{ onedrive_menu_label,		onedrive_display,	onedrive_move,		0},
	{ hightemp_menu_label,		hightemp_display,	hightemp_move,		0},
	{ homework_menu_label,		homework_display,	homework_move,		0},
	{"Kill/Pause Player",		kill_and_pause_player,	NULL,			0},
#ifdef EMPEG_KNOB_SUPPORTED
	{ knobdata_menu_label,		knobdata_display,	knobdata_move,		0},
#endif // EMPEG_KNOB_SUPPORTED
	{ delaytime_menu_label,		delaytime_display,	delaytime_move,		0},
	{"Reboot Machine",		reboot_display,		NULL,			0},
	{ carvisuals_menu_label,	carvisuals_display,	carvisuals_move,	0},
	{ blankerfuzz_menu_label,	blankerfuzz_display,	blankerfuzz_move,	0},
	{ blanker_menu_label,		blanker_display,	blanker_move,		0},
	{ saveserial_menu_label,	saveserial_display,	saveserial_move,	0},
	{"Show Flash Savearea",		savearea_display,	savearea_move,		0},
	{ bass_menu_label,		bass_display,		tone_move,		0},
	{ treble_menu_label,		treble_display,		tone_move,		0},
#ifdef CONFIG_HIJACK_TUNER
#ifdef CONFIG_HIJACK_TUNER_ADJUST
	{ dx_lo_menu_label,		dx_lo_display,		numeric_move,		0},
	{ if2_bw_menu_label,		if2_bw_display,		numeric_move,		0},
	{ agc_menu_label,		agc_display,		numeric_move,		0},
#endif
#endif
	{"Vital Signs",			vitals_display,		NULL,			0},
	{ volumelock_menu_label,	volumelock_display,	volumelock_move,	0},
#ifdef EMPEG_KNOB_SUPPORTED
	{ wam_menu_label,		wam_display,		NULL,			0},
#endif // EMPEG_KNOB_SUPPORTED
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
	static int prev_menu_item;

	if (firsttime) {
		hijack_last_moved = JIFFIES();
		prev_menu_item = -1;
	}
	if (menu_item != prev_menu_item) {
		unsigned int text_row;
		clear_hijack_displaybuf(COLOR0);
		prev_menu_item = menu_item;
		for (text_row = 0; text_row < EMPEG_TEXT_ROWS; ++text_row) {
			unsigned int index = (menu_top + text_row) % menu_size;
			unsigned int color = (index == menu_item) ? ENTRYCOLOR : COLOR2;  // COLOR2 <> PROMPTCOLOR
			(void)draw_string_spaced(ROWCOL(text_row,0), menu_table[index].label, color);
		}
		return NEED_REFRESH;
	}
	if (ir_selected) {
		menu_item_t *item = &menu_table[menu_item];
		activate_dispfunc(item->dispfunc, item->movefunc, item->userdata);
	} else if (jiffies_since(hijack_last_moved) > (HZ*5)) {
		hijack_deactivate(HIJACK_IDLE_PENDING); // menu timed-out
	}
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
#ifdef EMPEG_KNOB_SUPPORTED
		ir_knob_down = 0;
#endif // EMPEG_KNOB_SUPPORTED
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
	if (hijack_status == HIJACK_ACTIVE && hijack_movefunc != NULL) {
		hijack_movefunc(direction);
		hijack_last_moved = JIFFIES();
	}
}

static void
hijack_move_repeat (void)
{
	if (ir_left_down && jiffies_since(ir_left_down) >= ir_move_repeat_delay) {
		ir_left_down = JIFFIES();
		hijack_move(-1);
		ir_lasttime = jiffies;
	} else if (ir_right_down && jiffies_since(ir_right_down) >= ir_move_repeat_delay) {
		ir_right_down = JIFFIES();
		hijack_move(+1);
		ir_lasttime = jiffies;
	}
}

#define TESTCOL		12
#define ROWOFFSET(row)	((row)*(EMPEG_SCREEN_COLS/2))
#define TESTOFFSET(row)	(ROWOFFSET(row)+TESTCOL)

#ifdef EMPEG_KNOB_SUPPORTED
static int
check_for_seek_pattern (const unsigned char *buf, const unsigned long *pattern)
{
	int row;

	for (row = 24; row <= 31; row++) {
		unsigned long test = *(unsigned long *)(buf + ROWOFFSET(row) + 4);
		if (test != *pattern++)
			return 0;	// Seek-Tool not active
	}
	return 1;	// Seek-Tool is active
}

static int
check_if_seek_tool_is_active (const unsigned char *buf)
{
	static const unsigned long playsym [8] = { 0x000002f0, 0x0002fff0, 0x02fffff0, 0xfffffff0, 0x02fffff0, 0x0002fff0, 0x000002f0, 0x00000000 };
	static const unsigned long pausesym[8] = { 0x00000000, 0x00000000, 0xfff0fff0, 0xfff0fff0, 0xfff0fff0, 0xfff0fff0, 0x00000000, 0x00000000 };

	if (!check_for_seek_pattern(buf, playsym) && !check_for_seek_pattern(buf, pausesym))
		return 0;
	if (0x11111111 == *(unsigned long *)(buf + 20))
		return 2;	// Seek-Tool is active, and has the knob
	return 1;		// Seek-Tool is on-screen, but does not have knob
}
#endif // EMPEG_KNOB_SUPPORTED

static void
popup_activate (unsigned int button, int seek_tool)
{
	button &= ~BUTTON_FLAGS;
	current_popup = ir_next_match(NULL, button);

	if (seek_tool) {
		seektool_default_translation.buttons[0] = button;
		current_popup = &seektool_default_translation.hdr;
	} else if (current_popup == NULL && button == IR_FAKE_POPUP0) {
		current_popup = &popup0_default_translation.hdr;
	}
	if (current_popup != NULL) {
		if (current_popup->old == IR_FAKE_POPUP0)
			current_popup->popup_index = (current_popup->popup_index & ~POPUP0_MASK) | popup0_index;
		activate_dispfunc(popup_display, popup_move, 0);
	}
}

static int
test_row (const void *rowaddr, unsigned long color)
{
	const unsigned long *first = rowaddr;
	const unsigned long *test  = first + (((EMPEG_SCREEN_COLS/2)-(TESTCOL*2))/sizeof(long));
	do {
		if (*--test != color)
			return 0;	// no match
	} while (test > first);
	return 1;	// matched
}

static inline int
check_if_equalizer_is_active (const unsigned char *buf)
{
	const unsigned char *row = buf + ROWOFFSET(31);
	int i;

	for (i = 4; i >= 0; --i) {
		if ((*row++ & 0x0f) || (*row++ & 0x11) != 0x11 || (*row++ & 0x11) != 0x11)
			return 0;	// equalizer settings not displayed
	}
	return 1;	// equalizer settings ARE displayed
}

static inline int
check_if_soundadj_is_active (const unsigned char *buf)
{
	return (   test_row(buf+TESTOFFSET( 8), 0x00000000) && test_row(buf+TESTOFFSET( 9), 0x11111111)
		&& test_row(buf+TESTOFFSET(16), 0x11111111) && test_row(buf+TESTOFFSET(17), 0x00000000));
}

static inline int
check_if_search_is_active (const unsigned char *buf)
{
	return ((   test_row(buf+TESTOFFSET(4), 0x00000000) && test_row(buf+TESTOFFSET(5), 0x11111111))
		|| (test_row(buf+TESTOFFSET(6), 0x00000000) && test_row(buf+TESTOFFSET(7), 0x11111111)));
}

static inline int
check_if_menu_is_active (const unsigned char *buf)
{
	if (test_row(buf+TESTOFFSET(2), 0x00000000)) {
		if ((test_row(buf+TESTOFFSET(0), 0x00000000) && test_row(buf+TESTOFFSET(1), 0x11111111))
		 || (test_row(buf+TESTOFFSET(3), 0x11111111) && test_row(buf+TESTOFFSET(4), 0x00000000)))
			return 1;
	}
	return 0;
}

static unsigned int
get_player_ui_flags (const unsigned char *buf)
{
	// Use screen-scraping to see if elements of the player user-interface are active:
	if (check_if_soundadj_is_active(buf))
		return BUTTON_FLAGS_SOUNDADJ|BUTTON_FLAGS_UI;
	if (check_if_menu_is_active(buf) || check_if_search_is_active(buf) || check_if_equalizer_is_active(buf))
		return BUTTON_FLAGS_UI;
	return BUTTON_FLAGS_NOTUI;
}

static void
switch_to_src (int from_src, int to_src)
{
	if (from_src != to_src) {
		unsigned int button = 0;
		switch (to_src) {
			case INPUT_AUX:
				if (from_src != INPUT_PCM) {
					if (!kenwood_disabled) {
						button = IR_KW_TAPE_PRESSED;
						break;
					}
					hijack_enq_button_pair(IR_RIO_SOURCE_PRESSED);
				}
				// fall thru
			case INPUT_PCM:
				button = IR_RIO_SOURCE_PRESSED;
				break;

			case INPUT_RADIO_AM:
				if (from_src != INPUT_RADIO_FM)
					hijack_enq_button_pair(IR_RIO_TUNER_PRESSED);
				// fall thru
			case INPUT_RADIO_FM:
				button = IR_RIO_TUNER_PRESSED;
				break;
		}
		if (button)
			hijack_enq_button_pair(button);
	}
}

static void
save_restore_src (int action)
{
	static int saved_powerstate = 0;
	static int saved_src = 0;
	int src, new;

	src = hijack_current_mixer_input;
	new = saved_src;
	if (action) {	// 0=restore, 1==save, 2==save_and_switch_to_AUX
		new = saved_src = src;
		if (action == 2)
			new = INPUT_AUX;
		saved_powerstate = empeg_powerstate;
		if (!empeg_powerstate) {
			hijack_enq_button_pair(IR_RIO_SELECTMODE_PRESSED);
			/*
			 * The player just ignores buttons after wakeup
			 * unless we delay them slightly.
			 * So here we inject a NULL button with a 0.5 second delay,
			 * in front of the subsequent switch_to_src() button codes.
			 */
			hijack_enq_button(&hijack_playerq, IR_NULL_BUTTON, 50);
		}
	}
	switch_to_src(src, new);
	if (!action) {
		if (empeg_powerstate && !saved_powerstate)
			hijack_enq_button_pair(IR_RIO_SOURCE_PRESSED|BUTTON_FLAGS_LONGPRESS);
	}
}

static void
do_nextsrc (void)
{
	unsigned int button;

	// main -> tuner [ -> aux ] -> main
	// main -> tuner -> aux -> main
	button = IR_RIO_SOURCE_PRESSED;
	switch (get_current_mixer_source()) {
		case IR_FLAGS_TUNER:
			if (nextsrc_aux_enabled) {
				if (kenwood_disabled)
					hijack_enq_button_pair(IR_RIO_SOURCE_PRESSED);
				else
					button = IR_KW_TAPE_PRESSED;
			}
			break;
		case IR_FLAGS_MAIN:
			if (empeg_tuner_present || hijack_fake_tuner)
				button = IR_RIO_TUNER_PRESSED;
			break;
	}
	hijack_enq_button_pair(button);
}

static void
preselect_menu_item (void *dispfunc)
{
	while (menu_table[menu_item].dispfunc != dispfunc)
		menu_move(+1);
	menu_move(+1); menu_move(-1);
}

static int
timer_check_expiry (struct display_dev *dev)
{
	static unsigned long beeping;
	int elapsed;

	if (!timer_timeout || 0 > (elapsed = jiffies_since(timer_started) - timer_timeout))
		return 0;
	if (dev->power) {
		if (timer_action == 0) {  // Toggle Standby?
			// Power down the player
			hijack_enq_button_pair(IR_RIO_SOURCE_PRESSED|BUTTON_FLAGS_LONGPRESS);
			timer_timeout = 0; // cancel the timer
			return 0;
		}
	} else {
		// A harmless method of waking up the player (?)
		hijack_enq_button_pair(IR_RIO_SELECTMODE_PRESSED);
		if (timer_action == 0) {  // Toggle Standby?
			timer_timeout = 0; // cancel the timer
			return 0;
		}
	}

	// Preselect timer in the menu:
	if (hijack_status == HIJACK_IDLE)
		preselect_menu_item(timer_display);

	// Beep Alarm
	elapsed /= HZ;
	if (elapsed < 1) {
		beeping = 0;
	} else if (((elapsed >> 2) & 7) == (beeping & 7)) {
		int volume = 3 * ((++beeping >> 2) & 15) + 15;
		hijack_beep(80, 400, volume);
	}
	if (dev->power && hijack_status == HIJACK_IDLE && jiffies_since(ir_lasttime) > (HZ*4)) {
		static unsigned long lasttime = 0;
		static int color = 0;
		if (jiffies_since(lasttime) >= HZ) {
			lasttime = jiffies;
			color = (color == COLOR3) ? -COLOR3 : COLOR3;
			clear_hijack_displaybuf(-color);
			(void) draw_string_spaced(ROWCOL(2,31), "Timer Expired", color);
		}
		return 1;
	}
	return 0;
}

static int hijack_check_buttonlist (unsigned long data, unsigned long delay)
{
	int i;

	if (hijack_buttonlist[0] < 2) {
		// empty table means capture EVERYTHING
		hijack_enq_button(&hijack_userq, data, delay);
		wake_up(&hijack_userq_waitq);
		return 1;
	}
	for (i = (hijack_buttonlist[0] - 1); i > 0; --i) {
		if (data == hijack_buttonlist[i]) {
			hijack_enq_button(&hijack_userq, data, delay);
			wake_up(&hijack_userq_waitq);
			return 1;
		}
	}
	return 0;
}

static char          message_text[40];
static unsigned long message_time = 0;

static void
message_move (int direction)
{
	hijack_deactivate(HIJACK_IDLE);
}

static int
message_display (int firsttime)
{
	static const hijack_geom_t geom = {8, 8+6+KFONT_HEIGHT, 2, EMPEG_SCREEN_COLS-2};
	unsigned int rowcol;

	if (firsttime) {
		create_overlay(&geom);
		rowcol = (geom.first_row+4)|((geom.first_col+6)<<16);
		rowcol = draw_string(rowcol, message_text, COLOR3);
		hijack_last_moved = JIFFIES();
		untrigger_blanker();
	} else if (jiffies_since(hijack_last_moved) >= message_time) {
		hijack_deactivate(HIJACK_IDLE);
	}
	return NO_REFRESH;	// gets overridden if overlay still active
}

void
show_message (const char *msg, unsigned long time)
{
	if (msg)
		strncpy(message_text, msg, sizeof(message_text)-1);
	message_text[sizeof(message_text)-1] = '\0';
	message_time = time;
	if (message_text[0]) {
		unsigned long flags;
		if (!hijack_silent)
			printk("show_message(\"%s\")\n", message_text);
		save_flags_cli(flags);
		activate_dispfunc(message_display, message_move, 0);
		restore_flags(flags);
	}
}

void
hijack_show_fid (const char *msg)
{
	unsigned long flags;
	unsigned int playlist;
	int track;
	unsigned char *sa;

	// get current fid:
	save_flags_cli(flags);
	sa = *empeg_state_writebuf;
	playlist = sa[0x45] << 8 | sa[0x44];
	track = *(unsigned int *)(void *)(sa + 0x24);
	restore_flags(flags);
	if (track < 0 || track > 9999)
		track = -1;
	sprintf(message_text, "%04x.%d %s", playlist, track, msg);
	show_message(NULL, 20*HZ);
}

static unsigned int ir_lastpressed = IR_NULL_BUTTON;

static int
quicktimer_display (int firsttime)
{
	static unsigned int buttonlist[3];
	static const hijack_geom_t geom = {8, 8+6+KFONT_HEIGHT, 12, EMPEG_SCREEN_COLS-12};
	hijack_buttondata_t data;
	unsigned int rowcol;

	timer_started = JIFFIES();
	if (firsttime) {
		if (ir_lastpressed != IR_NULL_BUTTON) {
			buttonlist[0] = 3;
			buttonlist[1] = ir_lastpressed;
			buttonlist[2] = RELEASECODE(ir_lastpressed);
			hijack_buttonlist = buttonlist;
		}
		if (timer_timeout) {
			timer_timeout = 0;
			hijack_beep(60, 100, 30);
		} else {
			timer_timeout = hijack_quicktimer_minutes * (60*HZ);
			hijack_beep(80, 100, 30);
		}
		ir_numeric_input = &timer_timeout;
		hijack_last_moved = JIFFIES();
		create_overlay(&geom);
	} else if (jiffies_since(hijack_last_moved) >= (HZ*3)) {
		if (timer_timeout)
			hijack_beep(90, 70, 30);
		hijack_deactivate(HIJACK_IDLE);
	} else {
		while (hijack_button_deq(&hijack_userq, &data, 0)) {
			if (!(data.button & 0x80000000))
				timer_timeout += hijack_quicktimer_minutes * (60*HZ);
			hijack_last_moved = JIFFIES();
		}
		rowcol = (geom.first_row+4)|((geom.first_col+6)<<16);
		rowcol = draw_string(rowcol, "Quick Timer: ", COLOR3);
		clear_text_row(rowcol, geom.last_col-4, 1);
		rowcol = draw_hhmmss(rowcol, timer_timeout / (60*HZ), ENTRYCOLOR);
	}
	return NO_REFRESH;	// gets overridden if overlay still active
}

#ifdef CONFIG_HIJACK_TUNER
static char
fidentry_incr (char b, int incr)
{
	int i;

	for (i = 0; i <= 0xf; ++i) {
		if (b == hexchars[i])
			return hexchars[(i + incr) & 0xf];
	}
	return (incr > 0) ? '0' : 'F';
}

static int
fidentry_display (int firsttime)
{
	static char fidx[9], b, lastb;
	static int d, cycling, fidentry_mode;
	static unsigned int buttonlist[1];
	static const char *fidentry_modes[] = {""      "-",    "+",    "!"};
	static const char *fidentry_label[] = {" REP", " ENQ", " APP", " INS"};
	static const hijack_geom_t geom = {8, 8+6+KFONT_HEIGHT, 10, EMPEG_SCREEN_COLS-10};
	hijack_buttondata_t data;
	unsigned int rowcol;

	timer_started = JIFFIES();
	if (firsttime) {
		cycling = 0;
		fidentry_mode = 0;
		d = 0;
		fidx[0] = lastb = b = '_';
		fidx[1] = '\0';
		if (ir_lastpressed != IR_NULL_BUTTON) {
			buttonlist[0] = 0;
			hijack_buttonlist = buttonlist;
		}
		hijack_last_moved = JIFFIES();
		create_overlay(&geom);
	} else if (jiffies_since(hijack_last_moved) >= (HZ*15)) {
		hijack_beep(90, 70, 30);
		hijack_deactivate(HIJACK_IDLE);
	} else {
		if (cycling && jiffies_since(hijack_last_moved) >= (HZ + (HZ/2))) {
			cycling = 0;
			lastb = '_';
			if (d < 7)
				fidx[++d] = lastb;
		} else if (hijack_button_deq(&hijack_userq, &data, 0) && !(data.button & 0x80000000)) {
			int incr = 1, was_cycling = cycling;
			cycling = 0;
			switch (data.button) {
				case IR_KW_0_PRESSED:
				case IR_RIO_0_PRESSED: b = '0'; break;
				case IR_KW_1_PRESSED:
				case IR_RIO_1_PRESSED: b = '1'; break;
				case IR_KW_2_PRESSED:
				case IR_RIO_2_PRESSED:
					if (hijack_decimal_fidentry) {
						b = '2';
						break;
					}
					cycling = 1;
					switch (lastb) {
						case '2': b = 'A'; break;
						case 'A': b = 'B'; break;
						case 'B': b = 'C'; break;
						case 'C': b = '2'; break;
						default:  b = '2';
							if (was_cycling && d < 7)
								++d;
							break;
					}
					break;
				case IR_KW_3_PRESSED:
				case IR_RIO_3_PRESSED:
					if (hijack_decimal_fidentry) {
						b = '3';
						break;
					}
					cycling = 1;
					switch (lastb) {
						case '3': b = 'D'; break;
						case 'D': b = 'E'; break;
						case 'E': b = 'F'; break;
						case 'F': b = '3'; break;
						default:  b = '3';
							if (was_cycling && d < 7)
								++d;
							break;
					}
					break;
				case IR_KW_4_PRESSED:
				case IR_RIO_4_PRESSED: b = '4'; break;
				case IR_KW_5_PRESSED:
				case IR_RIO_5_PRESSED: b = '5'; break;
				case IR_KW_6_PRESSED:
				case IR_RIO_6_PRESSED: b = '6'; break;
				case IR_KW_7_PRESSED:
				case IR_RIO_7_PRESSED: b = '7'; break;
				case IR_KW_8_PRESSED:
				case IR_RIO_8_PRESSED: b = '8'; break;
				case IR_KW_9_PRESSED:
				case IR_RIO_9_PRESSED: b = '9'; break;

				case IR_KW_NEXTTRACK_PRESSED:
				case IR_RIO_NEXTTRACK_PRESSED:
				case IR_RIGHT_BUTTON_PRESSED:
					b = '_';
					break;
				case IR_KW_PREVTRACK_PRESSED:
				case IR_RIO_PREVTRACK_PRESSED:
				case IR_LEFT_BUTTON_PRESSED:
					if (d > 0) {
						if (!was_cycling)
							--d;
						fidx[d] = lastb = '_';
					}
					goto refresh;
				case IR_KNOB_LEFT:
					cycling = 1;
					b = fidentry_incr(lastb, -1);
					break;
				case IR_KNOB_RIGHT:
					cycling = 1;
					b = fidentry_incr(lastb, +1);
					break;
				case IR_RIO_SELECTMODE_PRESSED:
					fidentry_mode = (fidentry_mode + 1) & 3;
					break;
				case IR_KW_STAR_PRESSED:
				case IR_RIO_CANCEL_PRESSED:
				case IR_TOP_BUTTON_PRESSED:
					hijack_deactivate(HIJACK_IDLE);
					break;
				case IR_KNOB_PRESSED:
				case IR_KW_DNPP_PRESSED:
				case IR_RIO_MENU_PRESSED:
					was_cycling = incr = 0;
					while (d > 0 && fidx[d] == '_')
						--d;
					if (d < 2)
						hijack_deactivate(HIJACK_IDLE);
					if (hijack_decimal_fidentry) {
						int fid;
						unsigned char *f = fidx;
						get_number(&f, &fid, 10, NULL);
						d = sprintf(fidx, "%X", fid) - 1;
					} else
						fidx[d] = '0';
					hijack_beep(90, 70, 30);
					hijack_serial_rx_insert("#", 1, 1);
					hijack_serial_rx_insert(fidx, d+1, 1);
					if (fidentry_mode)
						hijack_serial_rx_insert(fidentry_modes[fidentry_mode], 1, 1);
					hijack_serial_rx_insert("\n", 1, 1);
					hijack_deactivate(HIJACK_IDLE);
					return NO_REFRESH;
				default:
					goto refresh;
			}
			if (was_cycling && !cycling && d < 7)
				++d;
			fidx[d] = lastb = b;
			if (!cycling && incr && d < 7 && fidx[d] != '_')
				fidx[++d] = '_';
			hijack_last_moved = JIFFIES();
		}
	refresh:
		fidx[d+1] = '\0';
		rowcol = (geom.first_row+4)|((geom.first_col+6)<<16);
		rowcol = draw_string(rowcol, "Fid: ", COLOR3);
		rowcol = draw_string(rowcol, fidx, ENTRYCOLOR);
		rowcol = draw_string(rowcol, fidentry_label[fidentry_mode], COLOR3);
		clear_text_row(rowcol, geom.last_col-4, 1);
	}
	return NO_REFRESH;	// gets overridden if overlay still active
}
#endif CONFIG_HIJACK_TUNER

// This routine gets first shot at IR codes as soon as they leave the translator.
//
// In an ideal world, we would never use "jiffies" here, relying on the inter-code "delay" instead.
// But.. maybe later.  The timings are coarse enough that it shouldn't matter much.
//
// Note that ALL front-panel buttons send codes ONCE on press, but twice on RELEASE.
//
static void
hijack_handle_button (unsigned int button, unsigned long delay, unsigned int player_ui_flags, const unsigned char *player_buf)
{
	unsigned long old_releasewait;
	int hijacked = 0;

	//printk("HB: %08lx.%ld,ui=%d\n", button, delay, player_ui_flags);
	// filter out buttons that rely on UI or NONUI states
	if ((button & BUTTON_FLAGS_UISTATE) && !(button & player_ui_flags))
		return;		// this button doesn't exist in this state
	player_ui_flags &= ~BUTTON_FLAGS_NOTUI;	// convert to a boolean
	if (button & BUTTON_FLAGS_SHIFT)
		ir_shifted = !ir_shifted;
	button &= ~(BUTTON_FLAGS_UISTATE|BUTTON_FLAGS_SHIFT);

	untrigger_blanker();
	if (hijack_status == HIJACK_ACTIVE) {
#if 1 //fixme someday
		// special case to allow embedding PopUp's
		if ((button & ~BUTTON_FLAGS) < IR_FAKE_HIJACKMENU || (button & ~BUTTON_FLAGS) > IR_FAKE_POPUP3)
#endif
		{
			if (hijack_buttonlist && hijack_check_buttonlist(button, delay)) {
				hijacked = 1;
				goto done;
			}
		}
		if (hijack_dispfunc == userland_display)
			goto done;	// just pass all buttons straight through to userland
	}
	switch (button) {
		case IR_FAKE_HIJACKMENU:
			activate_dispfunc(menu_display, menu_move, 0);
			hijacked = 1;
			break;
		case IR_FAKE_BASSADJ:
			activate_dispfunc(bass_display, tone_move, 0);
			hijacked = 1;
			break;
		case IR_FAKE_TREBLEADJ:
			activate_dispfunc(treble_display, tone_move, 0);
			hijacked = 1;
			break;
		case IR_FAKE_VOLADJOFF:
		case IR_FAKE_VOLADJ1:
		case IR_FAKE_VOLADJ2:
		case IR_FAKE_VOLADJ3:
			hijack_voladj_enabled = (button - IR_FAKE_VOLADJOFF);
			hijack_set_voladj_parms();
			hijacked = 1;
			break;
		case IR_FAKE_VOLADJMENU:
			activate_dispfunc(voladj_prefix_display, voladj_move, 0);
			hijacked = 1;
			break;
		case IR_FAKE_NEXTSRC:
			do_nextsrc();
			hijacked = 1;
			break;
		case IR_FAKE_SAVEAUX:	// power-on, save current source, switch to aux
			save_restore_src(2);
			hijacked = 1;
			break;
		case IR_FAKE_SAVESRC:
			save_restore_src(1);
			hijacked = 1;
			break;
		case IR_FAKE_RESTORESRC:
			save_restore_src(0);
			hijacked = 1;
			break;
		case IR_FAKE_AM:
			switch_to_src(hijack_current_mixer_input, INPUT_RADIO_AM);
			hijacked = 1;
			break;
		case IR_FAKE_FM:
			switch_to_src(hijack_current_mixer_input, INPUT_RADIO_FM);
			hijacked = 1;
			break;
		case IR_FAKE_REBOOT:
			hijack_reboot = 1;
			break;
		case IR_FAKE_POPUP0:
		case IR_FAKE_POPUP1:
		case IR_FAKE_POPUP2:
		case IR_FAKE_POPUP3:
			popup_activate(button, 0);
			hijacked = 1;
			break;
		case IR_FAKE_CLOCK:
		{
			tm_t	tm;
			char	buf[24];
			const char *wdays[7] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
			extern const char *hijack_months[12];
			hijack_convert_time(CURRENT_TIME + hijack_time_offset, &tm);
			sprintf(buf, "%3s %02u:%02u %02u-%3s-%4u", wdays[tm.tm_wday], tm.tm_hour, tm.tm_min,
				tm.tm_mday, hijack_months[tm.tm_mon], tm.tm_year);
			show_message(buf, 6*HZ);
			hijacked = 1;
			break;
		}
		case IR_FAKE_QUICKTIMER:
			activate_dispfunc(quicktimer_display, timer_move, 0);
			hijacked = 1;
			break;
#ifdef CONFIG_HIJACK_TUNER
		case IR_FAKE_FIDENTRY:
			activate_dispfunc(fidentry_display, NULL, 0);
			hijacked = 1;
			break;
#endif
		case IR_FAKE_KNOBSEEK:
			if (hijack_dispfunc == knobseek_display)
				ir_selected = 1;
			else
				activate_dispfunc(knobseek_display, NULL, 0);
			hijacked = 1;
			break;
		case IR_FAKE_VISUALSEEK:
			if (hijack_dispfunc == knobseek_display)
				ir_selected = 1;
			else
				activate_dispfunc(knobseek_display, knobseek_move_visuals, 0);
			hijacked = 1;
			break;
#ifdef EMPEG_KNOB_SUPPORTED
		case IR_KNOB_PRESSED:
			hijacked = 1; // hijack it and later send it with the release
			ir_knob_busy = 0;
			ir_knob_down = JIFFIES();
			if (hijack_status == HIJACK_ACTIVE)
				hijacked = ir_selected = 1;
			break;
		case IR_KNOB_RELEASED:
			if (ir_knob_down) {
				if (ir_knob_busy) {
					ir_knob_busy = 0;
				} else if (hijack_status == HIJACK_IDLE) {
					int index = knobdata_index;
					if (player_ui_flags)
						index = 0;
					hijacked = 1;
					if (jiffies_since(ir_knob_down) < (HZ/2)) {	// short press?
						int seek_tool = check_if_seek_tool_is_active(player_buf);
						unsigned int button;
						if (seek_tool == 2)
							index = 0;
						button = knobdata_buttons[index];
						if (index == 2) {
							activate_dispfunc(voladj_prefix_display, voladj_move, 0);
						} else if (index == 1 || (index && seek_tool == 1)) {
							popup_activate(button, seek_tool);
						} else {
							hijack_enq_button_pair(button);
						}
					}
				} else { //fixme someday: get rid of "special case" logic here
					hijacked = 1;
					if (hijack_dispfunc == voladj_prefix_display) {
						hijack_deactivate(HIJACK_IDLE);
						hijack_enq_button_pair(IR_KNOB_PRESSED);
					}
				}
			}
			ir_knob_down = 0;
			break;
		case IR_KNOB_RIGHT:
			if (!empeg_powerstate) {
				hijacked = 1;	// ignore knob when in standby
			} else if (hijack_status != HIJACK_IDLE) {
				hijack_move(1);
				hijacked = 1;
			}
			break;
		case IR_KNOB_LEFT:
			if (!empeg_powerstate) {
				hijacked = 1;	// ignore knob when in standby
			} else if (hijack_status != HIJACK_IDLE) {
				hijack_move(-1);
				hijacked = 1;
			}
			break;
#endif // EMPEG_KNOB_SUPPORTED
		case IR_RIO_MENU_PRESSED:
			if (!player_ui_flags) {
				hijacked = 1; // hijack it and later send it with the release
				ir_menu_down = JIFFIES();
			}
			if (hijack_status == HIJACK_ACTIVE)
				hijacked = ir_selected = 1;
			break;
		case IR_RIO_MENU_RELEASED:
			if (hijack_status != HIJACK_IDLE) {
				hijacked = 1;
			} else if (ir_menu_down && !player_ui_flags) {
				hijack_enq_button(&hijack_playerq, IR_RIO_MENU_PRESSED, 0);
				ir_releasewait = IR_NULL_BUTTON;
			}
			ir_menu_down = 0;
			break;
		case IR_KW_CD_PRESSED:
			if (hijack_status == HIJACK_ACTIVE) {
				hijacked = ir_selected = 1;
			} else if (hijack_status == HIJACK_IDLE) {
				// ugly Kenwood remote hack: press/release CD quickly 3 times to activate menu
				if (ir_lastpressed != button || delay > HZ)
					ir_trigger_count = 1;
				else if (++ir_trigger_count >= 3)
					hijacked = 1;
			}
			break;

		case IR_RIO_CANCEL_PRESSED:
		case IR_KW_STAR_PRESSED:
		case IR_TOP_BUTTON_PRESSED:
			if (hijack_status != HIJACK_IDLE) {
				if (hijack_status == HIJACK_ACTIVE && ir_numeric_input && *ir_numeric_input)
					hijack_move(0);
				else
					hijack_deactivate(HIJACK_IDLE_PENDING);
				hijacked = 1;
			}
			break;
		case IR_KW_NEXTTRACK_PRESSED:
		case IR_RIO_NEXTTRACK_PRESSED:
			ir_move_repeat_delay = (hijack_movefunc == game_move) ? (HZ/15) : (HZ/3);
			ir_right_down = JIFFIES();
			if (hijack_status != HIJACK_IDLE) {
				hijack_move(1);
				hijacked = 1;
			}
			break;
		case IR_KW_PREVTRACK_PRESSED:
		case IR_RIO_PREVTRACK_PRESSED:
			ir_move_repeat_delay = (hijack_movefunc == game_move) ? (HZ/15) : (HZ/3);
			ir_left_down = JIFFIES();
			if (hijack_status != HIJACK_IDLE) {
				hijack_move(-1);
				hijacked = 1;
			}
			break;
		case IR_KW_PREVTRACK_RELEASED:
		case IR_RIO_PREVTRACK_RELEASED:
			ir_left_down = 0;
			if (hijack_status != HIJACK_IDLE)
				hijacked = 1;
			break;
		case IR_KW_NEXTTRACK_RELEASED:
		case IR_RIO_NEXTTRACK_RELEASED:
			ir_right_down = 0;
			if (hijack_status != HIJACK_IDLE)
				hijacked = 1;
			break;
	}
done:
	// wait for RELEASED code of most recently hijacked PRESSED code
	old_releasewait = ir_releasewait;
	ir_releasewait  = IR_NULL_BUTTON;
	if (button != old_releasewait) {
		if (!hijacked)
			hijack_enq_button(&hijack_playerq, button, delay);
		else if (!IS_RELEASE(button))
			ir_releasewait = RELEASECODE(button);
	}
}

static void
hijack_handle_buttons (const char *player_buf)
{
	hijack_buttondata_t	data;
	unsigned long		flags;
	unsigned int		player_ui_flags = BUTTON_FLAGS_UI;

	if (hijack_status == HIJACK_IDLE)
		player_ui_flags = get_player_ui_flags(player_buf);
	while (hijack_button_deq(&hijack_inputq, &data, 1)) {
		save_flags_cli(flags);
		hijack_handle_button(data.button, data.delay, player_ui_flags, player_buf);
		restore_flags(flags);
	}
	if (!hijack_playerq_deq(NULL))
		input_wakeup_waiters();		// wake-up the player software
}

static unsigned int ir_downkey = IR_NULL_BUTTON, ir_delayed_rotate = 0;
static unsigned long do_keypress_flash = 0, last_keypress_flash = 0;

static void
input_append_code2 (unsigned int rawbutton)
{
	unsigned int released	= IS_RELEASE(rawbutton);
	unsigned int button	= PRESSCODE(rawbutton);

	if (ir_downkey != IR_NULL_BUTTON && button != PRESSCODE(ir_downkey)) {
		unsigned int rcode = RELEASECODE(ir_downkey);
		if (rcode != IR_NULL_BUTTON)
			input_append_code2(rcode);
	}
	if (hijack_ir_debug) {
		printk("%lu: IA2:%08x.%c,dk=%08x,dr=%d,lk=%d\n", jiffies, rawbutton, released ? 'R' : 'P',
			ir_downkey, (ir_delayed_rotate != 0), (ir_current_longpress != NULL));
	}
	if (released) {
		if (ir_downkey == IR_NULL_BUTTON)	// or we could just send the code regardless.. ??
			return;				// already taken care of (we hope)
		ir_downkey = IR_NULL_BUTTON;
	} else {
		if (ir_downkey == rawbutton || rawbutton == IR_NULL_BUTTON)
			return;	// ignore repeated press with no intervening release
		if (rawbutton > IR_NULL_BUTTON || rawbutton < IR_FAKE_HIJACKMENU) {	// A real button?
			ir_lastpressed = button; // used for detection of CD-CD-CD sequence; also used by quicktimer
			if (rawbutton != IR_KNOB_LEFT && rawbutton != IR_KNOB_RIGHT)	// Knob left/right don't issue release codes
				ir_downkey = rawbutton;
		}
	}
	if (hijack_keypress_flash) {
		if (!released && button > 0xf && button < IR_FAKE_HIJACKMENU)	// don't flash for front panel, or releases
			do_keypress_flash = JIFFIES();
		else
			do_keypress_flash = 0;
	}
	if (ir_translate_table != NULL) {
		unsigned short	mixer		= get_current_mixer_source();
		unsigned short	carhome		= empeg_on_dc_power ? IR_FLAGS_CAR : IR_FLAGS_HOME;
		unsigned short	shifted		= ir_shifted ? IR_FLAGS_SHIFTED : IR_FLAGS_NOTSHIFTED;
		unsigned short	flags		= mixer | carhome | shifted;
		int		delayed_send	= 0;
		int		was_waiting	= (ir_current_longpress != NULL);
		ir_translation_t *t		= NULL;
		ir_current_longpress = NULL;
		while (NULL != (t = ir_next_match(t, button))) {
			unsigned short t_flags = t->flags;
			if (t_flags & IR_FLAGS_POPUP)
				break;	// no translations here for PopUp's //FIXME: continue instead of break?
			if (hijack_ir_debug)
				printk("%lu: IA2: tflags=%02x, flags=%02x, match=%d\n", jiffies, t_flags, flags, (t_flags & flags) == t_flags);
			if ((t_flags & flags) == flags) {
				if (released) {	// button release?
					if ((t_flags & IR_FLAGS_LONGPRESS) && was_waiting) {
						// We were timing for a possible longpress,
						//  but now know that it wasn't held down long enough.
						// Instead, we just let go of a shortpress for which
						//   the original "press" code has not been sent yet
						// So we must now look for a shortpress translation
						delayed_send = 1;
						continue; // look for shortpress translation instead
					//else we need to send the "release" sequence for a longpress
					}
					// We just matched the "release" for either a longpress or shortpress.
					// Send the "press" sequence (if shortpress), then the final "release" (either)
					if (delayed_send)
						hijack_enq_translations(t);
					hijack_enq_release(&hijack_inputq, t->new[t->count - 1], 0); // final button release
				} else { // button press?
					if ((t_flags & IR_FLAGS_LONGPRESS)) {
						// This *might* turn out to be a longpress, so we cannot translate it yet.
						// So set a flag and let the display_handler() time it for sending.
						ir_current_longpress = t;
					} else {
						hijack_enq_translations(t);
					}
				}
				ir_lasttime = jiffies;
				return;
			}
		}
		if (delayed_send) {
			// we just released a non-translated shortpress, but haven't sent the "press" yet
			hijack_enq_button(&hijack_inputq, button, 0);
		}
	}
	hijack_enq_button(&hijack_inputq, rawbutton, 0);
	ir_lasttime = jiffies;
}

static void
input_send_delayed_rotate (void)
{
	input_append_code2(ir_delayed_rotate);
	ir_lasttime = jiffies;
	ir_delayed_rotate = 0;
}

void  // invoked from multiple places (time-sensitive code) in empeg_input.c
input_append_code(void *dev, unsigned int button)  // empeg_input.c
{
	unsigned long flags;

	ir_lastevent = jiffies;
	save_flags_cli(flags);
	if (hijack_ir_debug)
		printk("%lu: IA1:%08x,dk=%08x,dr=%d,lk=%d\n", jiffies, button, ir_downkey, (ir_delayed_rotate != 0), (ir_current_longpress != NULL));

	if (ir_delayed_rotate) {
		if (button != IR_KNOB_PRESSED)
			input_send_delayed_rotate();
		ir_delayed_rotate = 0;
	}
	if (hijack_status == HIJACK_IDLE || (button != IR_KNOB_LEFT && button != IR_KNOB_RIGHT)) {
		input_append_code2(button);
	} else if (ir_downkey == IR_NULL_BUTTON) {
		ir_delayed_rotate = button;
		ir_lasttime = jiffies;
	}
	restore_flags(flags);
}

#ifdef EMPEG_STALK_SUPPORTED
static int
handle_stalk_packet (unsigned char *pkt)
{
	static unsigned int current_button = -1;
	unsigned char val;
	unsigned int button;

	// Check for valid stalk packet, and convert into a button press/release code:
	if (pkt[0] != 0x02 || (pkt[1] | 1) != 0x01)
		return 0;	// not a valid Stalk packet
	if (((pkt[1] + pkt[2]) & 0xff) != pkt[3]) {
		if (hijack_stalk_debug)
			printk("Stalk: in=%02x %02x %02x %02x == bad checksum\n", pkt[0], pkt[1], pkt[2], pkt[3]);
		return 0;	// bad checksum
	}

	if (current_button != -1) {
		button = current_button | 0x80000000;
		current_button = -1;
		input_append_code(NULL, button);
		if (hijack_stalk_debug)
			printk("Stalk: in=%02x %02x %02x %02x == %08x\n", pkt[0], pkt[1], pkt[2], pkt[3], button);
	}
	val = pkt[2];
	if (val == 0xff) {	// button pressed?
		if (hijack_stalk_debug)
			printk("Stalk: in=%02x %02x %02x %02x == no button\n", pkt[0], pkt[1], pkt[2], pkt[3]);
	} else {
		min_max_t *v, *vals;
		int i;
		most_recent_stalk_code = val;	// For button_codes_display
		vals = stalk_on_left ? lhs_stalk_vals : rhs_stalk_vals;
		for (i = 0; (v = &vals[i])->min != -1; ++i) {
			if (val >= v->min && val <= v->max) {
				button = stalk_buttons[i];
				if (pkt[1]) // shifted?
					button |= IR_STALK_SHIFTED;
				current_button = button;
				input_append_code(NULL, button);
				if (hijack_stalk_debug)
					printk("Stalk: in=%02x %02x %02x %02x == %08x\n", pkt[0], pkt[1], pkt[2], pkt[3], button);
				goto done;
			}
		}
		if (hijack_stalk_debug)
			printk("Stalk: in=%02x %02x %02x %02x == no match\n", pkt[0], pkt[1], pkt[2], pkt[3]);
		else
			printk("Stalk: no match for value=0x%02x\n", val);
	}
done:
	return 1;	// Stalk packet
}

void
hijack_intercept_stalk (unsigned int packet)
{
	if (hijack_trace_tuner)
		printk("stalk: in=%08x\n", htonl(packet));
	if (hijack_option_stalk_enabled) {
		if (!handle_stalk_packet((unsigned char *)&packet)) {
			hijack_serial_rx_insert((unsigned char *)&packet, sizeof(packet), 0);
		}
	}
}
#endif // EMPEG_STALK_SUPPORTED

static void
check_screen_grab (unsigned char *buf)
{
#ifdef CONFIG_NET_ETHERNET
	extern unsigned char *notify_screen_grab_buffer;	// arch/arm/notify.c
	if (notify_screen_grab_buffer) {
		extern struct semaphore notify_screen_grab_sem;
		memcpy(notify_screen_grab_buffer, buf, EMPEG_SCREEN_BYTES);
		notify_screen_grab_buffer = NULL;
		up(&notify_screen_grab_sem);
	}
#endif // CONFIG_NET_ETHERNET
}

enum {poweringup, booting, booted, waiting, started} player_state = booting;

// The Hijack equivalent of main()
//
// This routine covertly intercepts all display updates,
// giving us a chance to substitute our own display.
//
// Display updates are (usually) triggered from the audio drivers,
// using the tq_immediate task queue.  So we are running here on
// interrupt context, not process context.  This limits what we can do.

void	// invoked from empeg_display.c
hijack_handle_display (struct display_dev *dev, unsigned char *player_buf)
{
	static unsigned int power_changed = 0;	// jiffies since most recent change of dev->power, or 0 = none
	unsigned char *buf = player_buf;
	unsigned long flags;
	int refresh = NEED_REFRESH;

	last_player_buf = player_buf;

	if (hijack_reboot) {
		do_reboot(dev);
		return;
	}

	// Wait for the player software to start up before doing certain tasks
	switch (player_state) {
		case poweringup:
			if (jiffies > (3 * HZ))
				player_state = booting;
			break;
		case booting:
			if (hijack_player_started) {
				player_state = booted;
			}
			break;
		case booted:
			if (jiffies_since(hijack_player_started) >= (HZ+(HZ/2))) {
				player_state = waiting;
			}
			break;
		case waiting:
			if (dev->power) {
				if (carvisuals_enabled && empeg_on_dc_power)
					hijack_enq_button_pair(IR_BOTTOM_BUTTON_PRESSED|BUTTON_FLAGS_LONGPRESS);
#ifndef EMPEG_FIXTEMP
				init_temperature(1);
#endif
				// Send initial button sequences, if any
				input_append_code(IR_INTERNAL, IR_FAKE_INITIAL);
				input_append_code(IR_INTERNAL, RELEASECODE(IR_FAKE_INITIAL));
				init_notify();
				wake_up(&hijack_player_init_waitq);	// wake up any waiters
				player_state = started;
			}
			break;
		case started:
			break;
	}

	// Adjust ButtonLED levels
	if (dev->power != empeg_powerstate) {
		empeg_powerstate = dev->power;
		power_changed = JIFFIES();
	}

	if (player_state != poweringup) {
		if (buttonled_command || !power_changed || jiffies_since(power_changed) > (3*HZ/2)) {
			power_changed = 0;
			hijack_adjust_buttonled(dev->power);
		}
	}

	save_flags_cli(flags);
	if (ir_delayed_rotate && jiffies_since(ir_lastevent) >= (HZ/10))
		input_send_delayed_rotate();
	if (ir_current_longpress && jiffies_since(ir_lastevent) >= HZ) {
		//printk("%8lu: LPEXP: %08lx\n", jiffies, ir_current_longpress->old);
		hijack_enq_translations(ir_current_longpress);
		ir_current_longpress = NULL;
	}
	restore_flags(flags);

	// Handle any buttons that may be queued up
	hijack_handle_buttons(player_buf);

	save_flags_cli(flags);
	if (!dev->power) {  // do (almost) nothing else if unit is in standby mode
#ifdef EMPEG_STALK_SUPPORTED
		hijack_stalk_enabled = 0;
#endif // EMPEG_STALK_SUPPORTED
		hijack_deactivate(HIJACK_IDLE);
		(void)timer_check_expiry(dev);
		restore_flags(flags);
		check_screen_grab(player_buf);
		display_blat(dev, player_buf);
		return;
	}
#ifdef EMPEG_STALK_SUPPORTED
	hijack_stalk_enabled = 1;
#endif // EMPEG_STALK_SUPPORTED

#ifdef EMPEG_KNOB_SUPPORTED
	if (ir_knob_down && jiffies_since(ir_knob_down) > (HZ*3)) {
		ir_knob_busy = 1;
		ir_knob_down = jiffies - HZ;  // allow repeated cycling if knob is held down
		if (!ir_knob_down)
			ir_knob_down = -1;
		hijack_deactivate(HIJACK_IDLE);
		do_nextsrc();
	}
#endif // EMPEG_KNOB_SUPPORTED
	if (jiffies > (10*HZ) && (timer_check_expiry(dev) || hightemp_check_threshold())) {
		buf = (unsigned char *)hijack_displaybuf;
		untrigger_blanker();
	}
	switch (hijack_status) {
		case HIJACK_IDLE:
			if (ir_trigger_count >= 3
#ifdef EMPEG_KNOB_SUPPORTED
			 || (!ir_knob_busy && ir_knob_down && jiffies_since(ir_knob_down) >= HZ)
#endif // EMPEG_KNOB_SUPPORTED
			 || (ir_menu_down && jiffies_since(ir_menu_down) >= HZ)) {
				activate_dispfunc(menu_display, menu_move, 0);
			}
			break;
		case HIJACK_ACTIVE:
			buf = (unsigned char *)hijack_displaybuf;
			if (hijack_dispfunc == NULL) {  // userland app finished?
				activate_dispfunc(menu_display, menu_move, 0);
			} else {
				if (hijack_movefunc != NULL)
					hijack_move_repeat();
				restore_flags(flags);
				refresh = hijack_dispfunc(0);
				save_flags_cli(flags);
				if (ir_selected && !hijack_overlay_geom) {
					if (hijack_dispfunc != userland_display) {
						if (hijack_dispfunc == menu_display) {
							menu_item_t *item = &menu_table[menu_item];
							activate_dispfunc(item->dispfunc, item->movefunc, 0);
						} else if (hijack_dispfunc == forcepower_display || hijack_dispfunc == homework_display || hijack_dispfunc == saveserial_display) {
							activate_dispfunc(reboot_display, NULL, 0);
						} else if (hijack_dispfunc == menuexec_display) {
							activate_dispfunc(menuexec_display2, NULL, hijack_userdata);
						} else {
							activate_dispfunc(menu_display, menu_move, 0);
						}
					}
				}
			}
			if (refresh == SHOW_PLAYER) {
				refresh = NEED_REFRESH;
				buf = player_buf;
			} else if (hijack_overlay_geom) {
				refresh = NEED_REFRESH;
				buf = player_buf;
				hijack_do_overlay(player_buf, (unsigned char *)hijack_displaybuf, hijack_overlay_geom);
			}
			break;
		case HIJACK_ACTIVE_PENDING:
			ir_selected = 0;
			buf = (unsigned char *)hijack_displaybuf;
			if (ir_releasewait == IR_NULL_BUTTON)
				hijack_status = HIJACK_ACTIVE;
			break;
		case HIJACK_IDLE_PENDING:
			if (ir_releasewait == IR_NULL_BUTTON || jiffies_since(ir_lasttime) > (2*HZ)) // timeout == safeguard
				hijack_deactivate(HIJACK_IDLE);
			break;
		default: // (good) paranoia
			hijack_deactivate(HIJACK_IDLE_PENDING);
			break;
	}
	check_screen_grab(buf);
	restore_flags(flags);

	// Prevent screen burn-in on an inactive/unattended player:
	if (hijack_dispfunc == message_display) {
		untrigger_blanker();
	} else if (blanker_timeout) {
		int is_paused = 0;
		if (get_current_mixer_source() == IR_FLAGS_MAIN && ((*empeg_state_writebuf)[0x0c] & 0x02) == 0)
			is_paused = 1;
		if (jiffies_since(blanker_lastpoll) >= (4*HZ/3)) {  // use an oddball interval to avoid patterns
			blanker_lastpoll = jiffies;
			if (!is_paused && screen_compare((unsigned long *)blanker_lastbuf, (unsigned long *)buf)) {
				memcpy(blanker_lastbuf, buf, EMPEG_SCREEN_BYTES);
				untrigger_blanker();
			} else if (!blanker_triggered) {
				blanker_triggered = JIFFIES();
			}
		}
		if (blanker_triggered) {
			unsigned long minimum = blanker_timeout * (SCREEN_BLANKER_MULTIPLIER * HZ);
			if (jiffies_since(blanker_triggered) > minimum) {
				buf = player_buf;
				memset(buf, 0, EMPEG_SCREEN_BYTES);
				refresh = NEED_REFRESH;
				//if (get_current_mixer_source() == IR_FLAGS_MAIN && hijack_standby_minutes > 0) {
				if (is_paused && hijack_standby_minutes > 0) {
					if (jiffies_since(blanker_triggered) >= ((hijack_standby_minutes * 60 * HZ) + minimum)) {
						save_flags_cli(flags);
						hijack_enq_button_pair(IR_RIO_SOURCE_PRESSED|BUTTON_FLAGS_LONGPRESS);
						restore_flags(flags);
						blanker_triggered = jiffies - minimum;	// prevents repeating buttons over and over..
					}
				}
			}
		}
	}
	if (do_keypress_flash && jiffies_since(do_keypress_flash) > hijack_keypress_flash)
		do_keypress_flash = 0;
	if (refresh == NEED_REFRESH || last_keypress_flash != do_keypress_flash) {
		unsigned char tbuf[EMPEG_SCREEN_BYTES];
		last_keypress_flash = do_keypress_flash;
		if (do_keypress_flash) {
			unsigned char *t = tbuf;
			while (t != &tbuf[EMPEG_SCREEN_BYTES])
				*t++ = *buf++ ^ 0x33;
			buf = tbuf;
		}
		display_blat(dev, buf);
		hijack_last_refresh = jiffies;
	}
}

static char *
findchars (char *s, char *chars)
{
	char c, *k;

	while ((c = *s)) {
		for (k = chars; *k; ++k) {
			if (c == *k)
				return s;
		}
		++s;
	}
	return s;
}

static char *
skipchars (char *s, char *chars)
{
	char c, *k;

	skip: while ((c = *s)) {
		for (k = chars; *k; ++k) {
			if (c == *k) {
				++s;
				goto skip;
			}
		}
		break;
	}
	return s;
}

static int
match_char (unsigned char **s, const unsigned char c)
{
	*s = skipchars(*s, " \t");
	if (**s == c) {
		++*s;
		*s = skipchars(*s, " \t");
		return (**s != '\0');
	}
	return 0; // match failed
}

int
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
	for (digits = 0; *s && (cp = strchr(hexchars, *s)); ++digits) {
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
	if (!digits || (*s && nextchars && !strchr(nextchars, *s)))
		return 0; // failure
	*target = neg ? -data : data;
	*src = s;
	return 1; // success
}

void
printline (const char *msg, char *s)
{
	char c, *e = s;

	e = findchars(s, "\n\r");
	c = *e;
	*e = '\0';
	if (!hijack_silent)
		printk("%s: \"%s\"\n", msg, s);
	*e = c;
}

int
get_button_code (unsigned char **s_p, unsigned int *button, int eol_okay, int raw, const char *nextchars)
{
	button_name_t *bn = button_names;
	unsigned char *s = *s_p;
	unsigned int b;

	for (bn = button_names; bn->name[0]; ++bn) {
		if (!strxcmp(s, bn->name, 1)) {
			unsigned char *t = s + strlen(bn->name), c = *t;
			if ((!c && eol_okay) || strchr(nextchars, c)) {
				*s_p = t;
				*button = bn->code;
				/*
				 * Give a way to do button.R for "button release" from web interface:
				 */
				if (raw && c == '.' && t[1] == 'R')
					*button = RELEASECODE(*button);
				return 1;	// success
			}
		}
	}
	if (!get_number(s_p, &b, 16, nextchars))
		return 0;
	*button = raw ? b : PRESSCODE(b);
	return 1;
}

static char *
find_header (char *s, const char *header)
{
	if (!s || !*s || !(s = strstr(s, header)) || !*s || !*(s += strlen(header)))
		s = NULL;
	return s;
}

static int
ir_setup_translations2 (unsigned char *s, unsigned int *table, int *had_errors)
{
	int index = 0;

	// find start of translations
	if (!(s = find_header(s, "[ir_translate]")))
		return 0;
	while (*(s = skipchars(s, " \n\t\r")) && *s != '[') {
		unsigned int old = 0, new, good = 0;
		ir_translation_t *t = NULL;
		char *line = s;
		if (*s == ';') {
			good = 1; // ignore the comment
		} else if (get_button_code(&s, &old, 0, 0, ".=")) {
			unsigned short irflags = 0, flagmask, *defaults;
			irflags = old & (BUTTON_FLAGS ^ BUTTON_FLAGS_ALTNAME);
			old ^= irflags;
			if (old >= IR_FAKE_POPUP0 && old <= IR_FAKE_POPUP3) {
				old |= IR_FLAGS_POPUP;
			} else if (old >= IR_FAKE_INITIAL || old < IR_FAKE_HIJACKMENU) {
				if (*s == '.') {
					ir_flags_t *f;
					do {
						unsigned char c = *++s;
						c = TOUPPER(c);
						for (f = ir_flags; (f->symbol && f->symbol != c); ++f);
						irflags |= f->flag;
					} while (f->symbol);
				}
				for (defaults = ir_flag_defaults; (flagmask = *defaults++);) {
					if (!(irflags & flagmask))
						irflags |= flagmask;
				}
			}
			if (match_char(&s, '=')) {
				int saved = index;
				if (table) {
					t = (ir_translation_t *)&(table[index]);
					t->old   = old;
					t->flags = irflags;
					t->count = 0;
				}
				index += sizeof(ir_translation_t) / sizeof(unsigned long);
				do {
					if (!get_button_code(&s, &new, 1, 0, ".,; \t\n")) {
						index = saved; // error: completely ignore this line
						break;
					}
					if (*s == '.') {
						do {
							char c = *++s;
							switch (TOUPPER(c)) {
								case 'L': new |= BUTTON_FLAGS_LONGPRESS;	break;
								case 'S': new |= BUTTON_FLAGS_SHIFT;		break;
								case 'U': new |= BUTTON_FLAGS_UI;		break;
								case 'N': new |= BUTTON_FLAGS_NOTUI;		break;
								case 'V': new |= BUTTON_FLAGS_SOUNDADJ;		break;
								default: goto save_new;
							}
						} while (1);
					}
				save_new:
					if (t)
						t->new[t->count++] = new;
					++index;
				} while (match_char(&s, ','));
				if (*s && *(s = skipchars(s, " \t\r")) && *s != ';' && *s != '\n')
					index = saved; // error: completely ignore this line
				if (index != saved)
					good = 1;
			}
		}
		if (!table && !good) {
			printline("[ir_translate] ERROR", line);
			*had_errors = 1;
		}
		s = findchars(s, "\n");
	}
	if (index) {
		if (table)
			table[index] = -1;	// end of table marker
		++index;
	}
	return index * sizeof(unsigned long);
}

static int
ir_setup_translations (unsigned char *buf)
{
	unsigned int *table = NULL;
	unsigned long flags;
	int size, had_errors = 0;

	save_flags_cli(flags);
	if (ir_translate_table) {
		kfree(ir_translate_table);
		ir_translate_table = NULL;
	}
	restore_flags(flags);
	size = ir_setup_translations2(buf, NULL, &had_errors);	// first pass to calculate table size
	if (size > 0) {
		table = kmalloc(size, GFP_KERNEL);
		if (!table) {
			if (!hijack_silent)
				printk("ir_setup_translations failed: no memory\n");
		} else {
			memset(table, 0, size);
			(void)ir_setup_translations2(buf, table, &had_errors);// second pass actually saves the data
			save_flags_cli(flags);
			ir_translate_table = table;
			restore_flags(flags);
		}
	}
	return had_errors;
}

// returns enu index >= 0,  or -ERROR
static int
extend_menu (menu_item_t *new)
{
	int i;
	for (i = 0; i < MENU_MAX_ITEMS; ++i) {
		menu_item_t *item = &menu_table[i];
		if (item->label == NULL || !strxcmp(item->label, new->label, 0)) {
			if (item->label == NULL) // extending table?
				menu_size = i + 1;
			*item = *new;	// copy data regardless
			return i;	// success
		}
	}
	return -ENOMEM; // no room; menu is full
}

static void
remove_menu_entry (const char *label)
{
	int i, found = 0;
	for (i = 0; i < MENU_MAX_ITEMS; ++i) {
		menu_item_t *item = &menu_table[i];
		if (found) {
			menu_table[i-1] = *item;
			if (!item->label)
				break;
		} else if (!strxcmp(item->label, label, 0)) {
			if (!hijack_silent)
				printk("hijack: removed menu entry: \"%s\"\n", label);
			found = 1;
		}
	}
	if (found) {
		if (menu_item >= --menu_size)
			--menu_item;
		if (menu_top >= menu_size)
			--menu_top;
		memset(&menu_table[MENU_MAX_ITEMS-1], 0, sizeof(menu_item_t));
	}
}

// returns menu index >= 0,  or -ERROR
static int
menuexec_extend_menu (char *cmdline)
{
	int len, rc = -ENOMEM;
	unsigned long flags;
	menu_item_t item;
	char *label, *p, *eol;

	eol = findchars(cmdline, "\n\r");
	len = eol - cmdline;
	if (len < 3 || *cmdline == ' ')	// format of cmdline is:  "label command.."
		return -EINVAL;

	label = kmalloc(1 + eol - cmdline, GFP_KERNEL);
	if (label == NULL)
		return -ENOMEM;
	memcpy(label, cmdline, len);
	label[len] = '\0';

	for (p = label; *p && *p != ' '; ++p) {
		if (*p == '_')	// convert underscores to spaces within label portion
			*p = ' ';
	}
	if (!*p || !*(p+1) || *(p+1) == ' ') {	// ensure we have a command after the label
		kfree(label);
		return -EINVAL;
	}
	*p++ = '\0';				// zero terminate the label portion
	item.userdata = (unsigned long)p;	// everything else is the command
	item.label    = (const char *)label;
	item.dispfunc = menuexec_display;
	item.movefunc = menuexec_move;
	save_flags_cli(flags);
	rc = extend_menu(&item);
	restore_flags(flags);
	if (rc < 0)
		kfree(label);
	return rc;
}

// returns menu index >= 0,  or -ERROR
static int
userland_extend_menu (char *label, unsigned long userdata)
{
	int rc = -ENOMEM;
	unsigned long flags;
	menu_item_t item;

	item.label = kmalloc(strlen(label)+1, GFP_KERNEL);
	if (item.label == NULL)
		return -ENOMEM;
	strcpy((char *)item.label, label);
	item.dispfunc = userland_display;
	item.movefunc = NULL;
	item.userdata = userdata;
	save_flags_cli(flags);
	rc = extend_menu(&item);
	restore_flags(flags);
	if (rc < 0)
		kfree(item.label);
	return rc;
}

static void
menu_init (void)
{
	// Initialize menu_size, menu_item, and menu_top
	for (menu_size = 0; menu_table[menu_size].label != NULL; ++menu_size); // Calculate initial menu size
	while (menu_table[menu_item].label == NULL)
		--menu_item;
	menu_top = (menu_item ? menu_item : menu_size) - 1;
}

static void
hijack_release_menu_and_buttons (void)
{
	unsigned long flags;

	save_flags_cli(flags);
	if (hijack_dispfunc == userland_display) {
		if ((hijack_userdata & ~0xff) == (current->pid << 8)) {
			hijack_dispfunc = NULL;		// restart the main menu
			if (hijack_buttonlist) {	// release any buttons we had grabbed
				kfree(hijack_buttonlist);
				hijack_buttonlist = NULL;
			}
		}
	}
	restore_flags(flags);
}

static int
hijack_takeover_screen (void)
{
	while (1) {
		if (signal_pending(current))
			return -EINTR;
		if (hijack_dispfunc != userland_display) {
			activate_dispfunc(userland_display, NULL, current->pid << 8);
			return 0;
		}
		current->state = TASK_INTERRUPTIBLE;
		schedule_timeout(HZ/5);
		current->state = TASK_RUNNING;
	}
}

static int
hijack_wait_on_menu (char *argv[])
{
	struct wait_queue wait = {current, NULL};
	int i, rc, index, num_items, indexes[MENU_MAX_ITEMS];
	unsigned long flags, menudata;

	// (re)create the menu items, with our pid/index as userdata
	for (i = 0; *argv && i < MENU_MAX_ITEMS; ++i) {
		unsigned char label[32], size = 0, *argp = *argv++;
		do {
			if (copy_from_user(&label[size], &argp[size], 1))
				return -EFAULT;
		} while (label[size++] && size < sizeof(label));
		label[size-1] = '\0';
		save_flags_cli(flags);
		index = userland_extend_menu(label, (current->pid << 8) | i);
		restore_flags(flags);
		if (index < 0)
			return index;
		indexes[i] = index;
	}
	num_items = i;
	save_flags_cli(flags);
	add_wait_queue(&hijack_menu_waitq, &wait);
	while (1) {
		current->state = TASK_INTERRUPTIBLE;
		if (signal_pending(current)) {
			rc = -EINTR;
			break;
		}
		menudata = menu_table[menu_item].userdata;
		if (hijack_dispfunc == userland_display && (menudata & ~0xff) == (current->pid << 8)) {
			rc = menudata & 0xff;
			break;
		}
		restore_flags(flags);
		schedule();
		save_flags_cli(flags);
	}
	current->state = TASK_RUNNING;
	remove_wait_queue(&hijack_menu_waitq, &wait);
	if (rc < 0)
		hijack_release_menu_and_buttons();
	for (i = num_items - 1; i >= 0; --i)	{	// disable our menu items until next time
		menu_item_t *item = &menu_table[indexes[i]];
		remove_menu_entry(item->label);
	}
	restore_flags(flags);
	return rc;
}

static int
copy_buttonlist_from_user (unsigned long arg, unsigned int **buttonlist, unsigned long max_size)
{
	// data[0] specifies TOTAL number of table entries data[0..?]
	// data[0] cannot be zero; data[0]==1 means "capture everything"
	unsigned int *list = NULL, size;
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

int hijack_ioctl  (struct inode *inode, struct file *filp, unsigned int cmd, unsigned long arg)
{
	unsigned long flags;
	int rc;

	switch (cmd) {
		case EMPEG_HIJACK_TAKEOVER:	// take over the screen (possibly waits for another app to release it first)
			return hijack_takeover_screen();

		case EMPEG_HIJACK_WAITMENU:	// (re)create menu item(s) and wait for them to be selected
		{
			// Invocation:  char *menulabels[] = {"Label1", "Label2", NULL};
			//              rc = ioctl(fd, EMPEG_HIJACK_WAITMENU, &menu_labels)
			//              if (rc < 0) perror(); else selected_index = rc;
			// The screen is then YOURS until you issue another EMPEG_HIJACK_WAITMENU
			hijack_release_menu_and_buttons();
			if (arg)
				return hijack_wait_on_menu((char **)arg);
			return 0;
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
			unsigned int *buttonlist = NULL;
			if ((rc = copy_buttonlist_from_user(arg, &buttonlist, 256)))
				return rc;
			save_flags_cli(flags);
			if (hijack_buttonlist) {
				restore_flags(flags);
				kfree(buttonlist);
				return -EBUSY;
			}
			hijack_buttonlist = buttonlist;
			hijack_initq(&hijack_userq, 'U');
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
			unsigned int *buttonlist = NULL;
			int i;
			if ((rc = copy_buttonlist_from_user(arg, &buttonlist, 256)))
				return rc;
			save_flags_cli(flags);
			for (i = 1; i < buttonlist[0]; ++i)
				hijack_enq_button(&hijack_playerq, buttonlist[i], 0);
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
			geom.last_col |= 1;
			if (geom.first_row >= EMPEG_SCREEN_ROWS || geom.last_row >= EMPEG_SCREEN_ROWS
			 || geom.first_row >= geom.last_row     || geom.first_col >= geom.last_col
			 || geom.first_col >= EMPEG_SCREEN_COLS || geom.last_col >= EMPEG_SCREEN_COLS
			 || (geom.first_col & 1))
				return -EINVAL;
			save_flags_cli(flags);
			if (geom.first_row == 0 && geom.last_row == (EMPEG_SCREEN_ROWS-1) && geom.first_col == 0 && geom.last_col == (EMPEG_SCREEN_COLS-1))
				hijack_overlay_geom = NULL;	// full screen
			else
				hijack_overlay_geom = &geom;	// partial overlay
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
		case EMPEG_HIJACK_WAIT_FOR_PLAYER:	// Wait for player to finish starting up
		{
			struct wait_queue wait = {current, NULL};
			save_flags_cli(flags);
			add_wait_queue(&hijack_player_init_waitq, &wait);
			rc = 0;
			while (1) {
				current->state = TASK_INTERRUPTIBLE;
				if (player_state == started)
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
			remove_wait_queue(&hijack_player_init_waitq, &wait);
			restore_flags(flags);
			return rc;
		}
		case EMPEG_HIJACK_GETPLAYERBUFFER:	// get the contents of the player's screen
		{
			// Invocation:  rc = ioctl(fd, EMPEG_HIJACK_GETPLAYERBUFFER, (char *)&buf);
			if(copy_to_user((char *) arg, last_player_buf, EMPEG_SCREEN_BYTES))
				return -EFAULT;
			return 0;

		}
		case EMPEG_HIJACK_GETPLAYERUIFLAGS:	// return the player UI status
		{
			// Invocation:  rc = ioctl(fd, EMPEG_HIJACK_GETPLAYERUIFLAGS, (unsigned long *)&data);
			unsigned int ui_flags = get_player_ui_flags(last_player_buf);
			return put_user(ui_flags, (unsigned int *)arg);

		}
		case EMPEG_HIJACK_READ_GPLR:	// read GPSR
		{
			//
			// An ioctl to rawread the serial port flow control pins,
			// added at the request of vincent@applesolutions.com
			//
			return GPLR;
		}
		default:			// Everything else
		{
			return display_ioctl(inode, filp, cmd, arg);
		}
	}
}

// Pattern matching: returns 1 if n(ame) matches p(attern), 0 otherwise
int
hijack_glob_match (const char *n, const char *p)
{
	while (*n && (*n == *p || *p == '?')) {
		++n;
		++p;
	}
	if (*p == '*') {
		while (*++p == '*');
		while (*n) {
			while (*n && (*n != *p && *p != '?'))
				++n;
			if (*n && hijack_glob_match(n++, p))
				return 1;
		}
	}
	return (!(*n | *p));
}

static int
get_option_vals (int syntax_only, unsigned char **s, const hijack_option_t *opt)
{
	int i, rc = 0;	// failure

	if (opt->num_items == 0) {
		unsigned char *t;
		t = findchars(*s, "\n;\r");
		if ((t - *s) > opt->max)
			t = *s + opt->max;
		if (!syntax_only && opt->target) {
			char c = *t;
			*t = '\0';
			strcpy(opt->target, *s);
			*t = c;
		}
		rc = 1;	// success
	} else if (opt->num_items == -1) {	// button code
		unsigned int val;
		if (get_button_code(s, &val, 1, 0, ".,; \t\n")) {
			if (!syntax_only && opt->target)
				*(int *)(opt->target) = val;
			rc = 1;	// success
		}
	} else {
		for (i = 0; i < opt->num_items; ++i) {
			int val;
			if (!get_number(s, &val, 10, " ,;\t\n") || val < opt->min || val > opt->max)
				break;	// failure
			if (!syntax_only && opt->target)
				((int *)(opt->target))[i] = val;
			if ((i + 1) == opt->num_items)
				rc = 1;	// success
			else if (!match_char(s, ','))
				break;	// failure
		}
	}
	return rc; // success
}

int
hijack_get_set_option (unsigned char **s_p)
{
	const hijack_option_t *opt;
	unsigned char *s = *s_p;

	for (opt = &hijack_option_table[0]; (opt->name); ++opt) {
		if (!strxcmp(s, opt->name, 1)) {
			s += strlen(opt->name);
			if (match_char(&s, '=')) {
				unsigned char *test = s;
				if (!get_option_vals(1, &test, opt))	// first pass validates
					return -EINVAL;
				(void)get_option_vals(0, &s, opt);	// second pass saves
				*s_p = s;
				return 0;
			}
		}
	}
	return -EINVAL;
}

static int
hijack_find_player_option (unsigned char *buf, char *section, char *option)
{
	unsigned char *s;

	if ((s = find_header(buf, section))) {
		while (*(s = skipchars(s, " \n\t\r")) && *s != '[') {
			if (!strxcmp(s, option, 1))
				return 1;	// option was found
			s = findchars(s, "\n");
		}
	}
	return 0;	// option not found
}

static unsigned char *
hijack_exec_line (unsigned char *s)
{
	unsigned char *cmdline = s;

	memcpy(cmdline, "exec", 4);	// prefix the command with "exec"
	s = findchars(cmdline+5, "\n\r");
	if (s != (cmdline+5)) {
		char saved = *s;
		*s = '\0';
		hijack_exec(NULL, cmdline);
		*s = saved;
	}
	return s;
}

int
menuexec_daemon (void *not_used)	// invoked from init/main.c
{
	// kthread setup
	set_fs(KERNEL_DS);
	current->session = 1;
	current->pgrp = 1;
	strcpy(current->comm, "menuexec");
	sigfillset(&current->blocked);

	while (1) {
		down(&hijack_menuexec_sem);
		if (hijack_menuexec_command != NULL) {
			hijack_exec(NULL, hijack_menuexec_command);
			hijack_menuexec_command = NULL;
		}
	}
}

static int
hijack_get_options (unsigned char *buf)
{
	static const char menu_delete[] = "menu_remove=";
	static int already_ran_once = 0;
	int errors;
	unsigned char *s;

	// look for certain player options we use internally:
	kenwood_disabled = hijack_find_player_option(buf, "[kenwood]",  "disabled=1");
#ifdef EMPEG_STALK_SUPPORTED
	stalk_on_left    = hijack_find_player_option(buf, "[controls]", "stalk_side=left");
#endif // EMPEG_STALK_SUPPORTED

	// look for [hijack] options:
	if (!(s = find_header(buf, "[hijack]")))
		return 0;
	errors = 0;
	while (*(s = skipchars(s, " \n\t\r")) && *s != '[') {
		char *line = s;
		if (!strxcmp(s, ";@EXEC_ONCE ", 1)) {
			if (!already_ran_once) {
				char *cmd = s+7;
				s = hijack_exec_line(cmd);
				memcpy(cmd, "ONCE", 4);	// restore prefix (hijack_exec_line overwrote it)
			}
			goto nextline;
		}
		if (!strxcmp(s, ";@MENUEXEC ", 1)) {
			if (!already_ran_once) {
				if (menuexec_extend_menu(s+11) < 0) {
					printline("[hijack] ERROR", line);
					errors = 1;
				}
			}
			goto nextline;
		}
		if (!strxcmp(s, ";@EXEC ", 1)) {
			char *cmd = s+2;
			s = hijack_exec_line(cmd);
			memcpy(cmd, "EXEC", 4);	// restore prefix (hijack_exec_line overwrote it)
			goto nextline;
		}
		if (!strxcmp(s, ";@DELAY", 1)) {
			current->state = TASK_UNINTERRUPTIBLE;
			schedule_timeout(HZ);
			goto nextline;
		}
		if (*s == ';')
			goto nextline;
		if (!strxcmp(s, menu_delete, 1)) {
			unsigned char *label = s += sizeof(menu_delete)-1;
			s = findchars(s, "\n;\r");
			if (s != label) {
				char saved = *s;
				*s = '\0';
				remove_menu_entry(label);
				*s = saved;
				goto nextline;
			}
		}
		if (hijack_get_set_option(&s)) {
			printline("[hijack] ERROR", line);
			errors = 1;
		}
	nextline:
		s = findchars(s, "\n");
	}
	already_ran_once = 1;
	return errors;
}

static void
set_drive_spindown (ide_drive_t *drive)
{
	char buf[16];
	if (drive->present)
		(void) ide_wait_cmd(drive, WIN_SETIDLE1, ((hijack_spindown_seconds + 4) / 5), 0, 0, buf);
}

static void
set_drive_spindown_times (void)
{
	unsigned int *permset=(unsigned int*)(EMPEG_FLASHBASE+0x2000);
	int model = (permset[0] < 7);	// 1 == mk1; 0 == mk2(a)

	set_drive_spindown(&ide_hwifs[model].drives[!model]);
	set_drive_spindown(&ide_hwifs[0].drives[0]);
}

// edit the player's view of config.ini
static int
edit_config_ini (char *s, const char *lookfor)
{
	char *optname, *optend;
	int count = 0;

	while (*(s = skipchars(s, " \n\t\r"))) {
		if (!strxcmp(s, lookfor, 1)) {		// find next "lookfor" string
			// "insert in place" the new option
			s += strlen(lookfor);
			optname = skipchars(s, " \t");
			if (optname != s || *s == ';'){ // verify whitespace after "lookfor"
				s = optname;
				*(s - 1) = '\n';	// "uncomment" the portion after "lookfor"
				++count;		// keep track of how many substitutions we do
				optend = findchars(s, "=\r\n");
				if (*optend == '=')
					++optend;
				s = findchars(optend, "\r\n");
				if (*s) {
					// temporarily terminate the optname substring
					char saved = *optend, *t = s;
					*optend = '\0';
					// now find any old copies of the same option, and nuke'em
					while (*(t = skipchars(t, " \t\r\n"))) {
						if (!strxcmp(t, optname, 1)) {
							*t = ';';	// comment-out the option, in-place!
							break;
						}
						t = findchars(t, "\r\n");
					}
					*optend = saved;
				}
			}
		}
		s = findchars(s, "\r\n");
	}
	return count;
}

static void
reset_hijack_options (void)
{
	const hijack_option_t *h;
	kenwood_disabled = 0;
#ifdef EMPEG_STALK_SUPPORTED
	stalk_on_left = 0;
	lhs_stalk_vals[10] = (min_max_t){-1,-1};	// mark end of table (not part of "options").
	rhs_stalk_vals[10] = (min_max_t){-1,-1};	// mark end of table (not part of "options").
#endif // EMPEG_STALK_SUPPORTED
	for (h = hijack_option_table; h->name; ++h) {
		int n = h->num_items, *val = h->target;
		if (val) {
			if (n == 1 || n == -1) {
				*val = h->defaultval;
			} else if (n) {
				int *def = (int *)h->defaultval;
				while (n--)
					*val++ = *def++;
			} else {
				strcpy(h->target, (char *)h->defaultval);
			}
		}
	}
}

#ifdef CONFIG_EMPEG_I2C_FAN_CONTROL

int i2c_write8 (unsigned char device, unsigned char command, unsigned char *data, int count);	// empeg_dsp_i2c.c
int i2c_read8  (unsigned char device, unsigned char command, unsigned char *data, int count);	// empeg_dsp_i2c.c

#define FAN_CONTROL_DEVADDR	0x90	// i2c bus address for fan controller

#define FAN_CONTROL_START	0x51	// start temperature conversions
#define FAN_CONTROL_STOP	0x22	// stop temperature conversions
#define FAN_CONTROL_RESET	0x54	// software power-on reset command (no command ACK!)
#define FAN_CONTROL_HIGH	0xa1	// read/write thermostat "high" temperature
#define FAN_CONTROL_LOW		0xa2	// read/write thermostat "low" temperature
#define FAN_CONTROL_TEMP	0xaa	// read current temperature
#define FAN_CONTROL_CONFIG	0xac	// read/write configuration register

static void
fan_write8 (unsigned char command, unsigned char *data, int count)
{
	if (i2c_write8(FAN_CONTROL_DEVADDR, command, data, count)) {
		if (!hijack_silent)
			printk("Fan control error\n");
		show_message("Fan control error", 10*HZ);
	}
}

static void
set_fan_control (void)
{
	unsigned char tmp[2];

	fan_write8(FAN_CONTROL_STOP,  NULL, 0);		// stop conversions
	tmp[0] = 0x02;					// 9-bit continuous mode, T-Out active high
	fan_write8(FAN_CONTROL_CONFIG, tmp, 1);		// configure chip
	tmp[0] = fan_control_low;
	tmp[1] = 0;
	fan_write8(FAN_CONTROL_LOW,    tmp, 2);		// set low temp threshold
	tmp[0] = fan_control_high;
	//tmp[1] = 0;
	fan_write8(FAN_CONTROL_HIGH,   tmp, 2);		// set high temp threshold
	fan_write8(FAN_CONTROL_START, NULL, 0);		// (re-)start conversions
#if 1
	i2c_read8(FAN_CONTROL_DEVADDR,  FAN_CONTROL_TEMP,   tmp, sizeof(tmp));	// read current temperature
	printk("fan control temperature = %d\n", (short)tmp[0]);
#endif
}

#endif // CONFIG_EMPEG_I2C_FAN_CONTROL

// invoked from fs/read_write.c on each read of config.ini at each player start-up.
// This could be invoked multiple times if file is too large for a single read,
// so we use the f_pos parameter to ensure we only do setup stuff once.
void
hijack_process_config_ini (char *buf, off_t f_pos)
{
	static const char *acdc_labels[2] = {";@AC", ";@DC"};
	static const char *loopback_labels[2] = {";@NOLOOPBACK", ";@LOOPBACK"};
	unsigned int count;

#ifdef EMPEG_KNOB_SUPPORTED
	get_player_version();
#endif
	do {
		count  = edit_config_ini(buf, loopback_labels [hijack_loopback]);
		count += edit_config_ini(buf, acdc_labels     [empeg_on_dc_power]);
		count += edit_config_ini(buf, homework_labels [hijack_homework]);
	} while (count);

	if (f_pos)		// exit if not first read of this cycle
		return;

	printk("\n");
	reset_hijack_options();
	if (ir_setup_translations(buf))
		show_message("ir_translate config error", 5*HZ);
	if (hijack_get_options(buf))
		show_message("hijack config error", 5*HZ);
	if (ide_hwifs[0].drives[1].present || (MAX_HWIFS > 1 && ide_hwifs[1].drives[0].present)) {
		remove_menu_entry(onedrive_menu_label);
		hijack_onedrive = 0;
		empeg_state_dirty = 1;
	}
#ifdef CONFIG_HIJACK_TUNER
#ifdef CONFIG_HIJACK_TUNER_ADJUST
	if (!empeg_tuner_present) {
		remove_menu_entry(if2_bw_menu_label);
		remove_menu_entry(agc_menu_label);
		remove_menu_entry(dx_lo_menu_label);
	}
#endif
#endif
	if (!empeg_on_dc_power)
		remove_menu_entry(saveserial_menu_label);
	if (hijack_old_style) {
		PROMPTCOLOR = COLOR2;
		ENTRYCOLOR = COLOR3;
	} else {
		PROMPTCOLOR = COLOR3;
		ENTRYCOLOR = -COLOR3;
	}
	hijack_set_voladj_parms();
#ifdef CONFIG_NET_ETHERNET
	if (empeg_on_dc_power && !hijack_dc_servers) {
		// disable servers on DC power to free more buffer space
		hijack_kftpd_control_port = 0;
		hijack_khttpd_port = 0;
		hijack_ktelnetd_port = 0;
	}
	up(&hijack_kxxxd_startup_sem);	// start daemons now that we've parsed config.ini for port numbers
#endif // CONFIG_NET_ETHERNET
	set_drive_spindown_times();
#ifdef CONFIG_EMPEG_I2C_FAN_CONTROL
	if (fan_control_enabled)
		set_fan_control();
#endif // CONFIG_EMPEG_I2C_FAN_CONTROL
}

// This version number should be incremented ONLY when existing fields
// in the savearea are moved or resized.  It should NOT be incremented
// when we're just converting "spare" space into saved data (or vice versa).
#define LAYOUT_VERSION		1
#define SAVEAREA_LAYOUT		((LAYOUT_VERSION << 4) | (0xf & ~LAYOUT_VERSION))

// As of v2beta11, the player software uses 88/128 bytes, plus 2-byte checksum.
// Hijack (v234) now "steals" 16 bytes from the end, just before the checksum.

// This substruct is for data that MUST be kept independently for AC/DC power modes
typedef struct hijack_savearea_acdc_s {	// 32-bits total
	signed   delaytime		: 8;			// 8 bits

	unsigned spare4			: 4;			// 4 bits
	unsigned buttonled_level	: BUTTONLED_BITS;	// 3 bits
	unsigned volumelock_enabled	: 1;			// 1 bit

	unsigned voladj			: VOLADJ_BITS;		// 2 bits
	unsigned spare1			: 1;			// 1 bits
	unsigned knob			: 1+KNOBDATA_BITS;	// 5 bits

	unsigned bass_adj		: 4;			// 4 bits
	unsigned treble_adj		: 4;			// 4 bits
} hijack_savearea_acdc_t;

// The "master" 16-byte savearea struct, with AC/DC substructs, and common data fields:
typedef struct hijack_savearea_s {
	hijack_savearea_acdc_t ac;				// 32 bits
	hijack_savearea_acdc_t dc;				// 32 bits

	unsigned blanker_timeout	: BLANKER_BITS;		// 6 bits
	unsigned spare2			: 2;			// 2 bits

	unsigned blanker_sensitivity	: SENSITIVITY_BITS;	// 3 bits
	unsigned hightemp_threshold	: HIGHTEMP_BITS;	// 5 bits

	unsigned menu_item		: MENU_BITS;		// 5 bits
	unsigned restore_carvisuals	: 1;			// 1 bit
	unsigned fsck_disabled		: 1;			// 1 bit
	unsigned onedrive		: 1;			// 1 bit

	unsigned timer_action		: TIMERACTION_BITS;	// 1 bit
	unsigned homework		: 1;			// 1 bits
	unsigned saveserial		: 1;			// 1 bits
	unsigned spare1			: 1;			// 1 bit
	unsigned force_power		: FORCEPOWER_BITS;	// 4 bits

	unsigned spare16		: 16;			// 16 bits
	unsigned spare8			:  8;			//  8 bits
	unsigned layout_version		:  8;			//  8 bits
} hijack_savearea_t;

#define HIJACK_SAVEAREA_OFFSET (128 - 2 - sizeof(hijack_savearea_t))

hijack_savearea_t savearea;	// MUST be static for AC/DC options to persist in opposite mode!

void	// invoked from empeg_state.c
hijack_save_settings (unsigned char *buf)
{
	unsigned int knob;
	hijack_savearea_acdc_t	*acdc, preserved;

	// preserve the alternate power mode settings while we clear the "spare" fields:
	preserved = empeg_on_dc_power ? savearea.ac : savearea.dc;
	memset(&savearea, 0, sizeof(savearea));	// ensure all "spare" fields are zeroed
	if (empeg_on_dc_power) {
		savearea.ac = preserved;
		acdc = &savearea.dc;
	} else {
		savearea.dc = preserved;
		acdc = &savearea.ac;
	}

	// save state
	knob = (knobdata_index == 1) ? (1 << KNOBDATA_BITS) | popup0_index : knobdata_index;
	acdc->knob			= knob;
	acdc->delaytime			= hijack_delaytime;
	acdc->buttonled_level		= hijack_buttonled_on_level;
	acdc->volumelock_enabled	= hijack_volumelock_enabled;
	acdc->voladj			= hijack_voladj_enabled;
	acdc->bass_adj			= hijack_bass_adj;
	acdc->treble_adj		= hijack_treble_adj;
	savearea.blanker_timeout	= blanker_timeout;
	savearea.force_power		= hijack_force_power;
	savearea.blanker_sensitivity	= blanker_sensitivity;
	savearea.hightemp_threshold	= hightemp_threshold;
	savearea.menu_item		= menu_item;
	savearea.restore_carvisuals	= carvisuals_enabled;
	savearea.fsck_disabled		= hijack_fsck_disabled;
	savearea.onedrive		= hijack_onedrive;
	savearea.saveserial		= hijack_saveserial;
	savearea.timer_action		= timer_action;
	savearea.homework		= hijack_homework;
	savearea.layout_version		= SAVEAREA_LAYOUT;
	memcpy(buf+HIJACK_SAVEAREA_OFFSET, &savearea, sizeof(savearea));
}

static int
hijack_restore_settings (char *buf, char *msg)
{
	extern int		empeg_state_restore(unsigned char *);	// arch/arm/special/empeg_state.c
	hijack_savearea_acdc_t	*acdc;
	unsigned int		knob, failed, force_power, no_popup = 0;

	// retrieve the savearea, reverting to all zeros if the layout has changed
	memset(&savearea, 0, sizeof(savearea));
	failed = empeg_state_restore(buf);
	buf += HIJACK_SAVEAREA_OFFSET;
	if (!failed) {
		unsigned char layout_version = ((hijack_savearea_t *)buf)->layout_version;
		if (layout_version == SAVEAREA_LAYOUT) // valid layout_version?
			memcpy(&savearea, buf, sizeof(savearea));
		else
			failed = 2;
	}

	// first priority is getting/overriding the unit's AC/DC power mode
	empeg_on_dc_power = ((GPLR & EMPEG_EXTPOWER) != 0);
	hijack_force_power = force_power = savearea.force_power;
#ifdef EMPEG_KNOB_SUPPORTED
{
	extern void hijack_read_tuner_id (int *, int *);	// drivers/char/serial_sa1100.c
	int	tuner_id = -1;
	hijack_loopback = 0;

	hijack_read_tuner_id(&hijack_loopback, &tuner_id);
	printk("Tuner: loopback=%d, ID=%d\n", hijack_loopback, tuner_id);
	if (tuner_id == -1)
		tuner_id = 0;	// we use "0" to mean "no tuner" in the Force_Power menu
	else
		empeg_tuner_present = 1;
	if (force_power != FORCE_NOLOOPBACK) {
		if (force_power != FORCE_AC && force_power != FORCE_DC) {
			if (empeg_on_dc_power && hijack_loopback) {
				force_power = FORCE_AC;
				no_popup = 1;
			} else if (force_power >= FORCE_TUNER) {
				force_power -= FORCE_TUNER;
				if ((force_power >> 1) == tuner_id) {
					force_power = (force_power & 1) ? FORCE_DC : FORCE_AC;
				}
			}
		}
	}
}
#endif // EMPEG_KNOB_SUPPORTED
	if (force_power == FORCE_AC || force_power == FORCE_DC) {
		empeg_on_dc_power = (force_power == FORCE_DC);
		sprintf(msg, "Forced %s mode", acdc_text[empeg_on_dc_power]);
		printk("%s\n", msg);
		//
		// Suppress pop-up message for the normal "docked" situation
		//
		if (no_popup)
			msg[0] = '\0';
	}

	// Now that the powermode (AC/DC) is set, we can deal with everything else
	acdc = empeg_on_dc_power ? &savearea.dc : &savearea.ac;
	knob				= acdc->knob;
	hijack_delaytime		= acdc->delaytime;
	hijack_buttonled_on_level	= acdc->buttonled_level;
	hijack_volumelock_enabled	= acdc->volumelock_enabled;
	hijack_voladj_enabled		= acdc->voladj;
	hijack_bass_adj			= acdc->bass_adj;
	hijack_treble_adj		= acdc->treble_adj;
	if ((knob & (1 << KNOBDATA_BITS)) == 0) {
		popup0_index		= 0;
		knobdata_index		= knob & KNOBDATA_MASK;
	} else {
		popup0_index		= knob & POPUP0_MASK;
		knobdata_index		= 1;
	}

	blanker_timeout			= savearea.blanker_timeout;
	blanker_sensitivity		= savearea.blanker_sensitivity;
	hightemp_threshold		= savearea.hightemp_threshold;
	menu_item			= savearea.menu_item;
	carvisuals_enabled		= savearea.restore_carvisuals;
	hijack_fsck_disabled		= savearea.fsck_disabled;
	hijack_onedrive			= savearea.onedrive;
	hijack_saveserial		= savearea.saveserial;
	timer_action			= savearea.timer_action;
	hijack_homework			= savearea.homework;

	return failed;
}

char hijack_zoneinfo[128];

static long
lswap (void *z)
{
	unsigned char *p = z;
	return (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3];
}

// Compute current GMT time offset from the zoneinfo file:
//
// If we wanted to be able to keep correctly adjusted
// continuous time (over daylight savings changes),
// then we would really need to do this calculation
// *every* time we update our clock display.
// But doing so requires keeping the zoneinfo file data
// in memory, so.. no such luck.
//
static void
hijack_get_time_offset (int fd, long curtime)
{
	unsigned long zbuf = get_free_page(GFP_KERNEL);

	// See man (5) tzfile if you want to know what this routine is doing
	if (zbuf) {
		unsigned char *z = 32 + (char *)zbuf;
		extern asmlinkage int sys_read(int fd, void * buf, unsigned int count);
		long timecnt, typecnt, i;
		if (sys_read(fd, (void *)zbuf, 4096) >= 56) {
			timecnt = lswap(z);
			typecnt = lswap(z+4);
			z += 12;
			for (i = timecnt; i > 0 && curtime < lswap(z + (--i * 4)););
			z += timecnt * 4;
			z += timecnt + (z[i] * 6);
			hijack_time_offset = lswap(z);
			printk("Timezone: %s\n", hijack_zoneinfo+20);
		}
		free_page(zbuf);
	}
}

void
hijack_init_zoneinfo (void)
{
	unsigned char *tz = hijack_get_state_read_buffer() + 0x51;
	int fd, z;
	mm_segment_t old_fs = get_fs();
	set_fs(KERNEL_DS);

	// Figure out which timezone file the player is using,
	// based on the (up to) four directory indexes from flash.
	// We use this in fs/open.c to redirect failed /etc/localtime attempts.
	strcpy(hijack_zoneinfo, "/usr/share/zoneinfo");
	for (z = 0; z < 4; ++z) {
		int e, dcount = tz[z] + 2;		// 2 extra:  '.' and '..'
		struct dirent de;
		fd = sys_open(hijack_zoneinfo, O_RDONLY, 0);
		if (fd == -1)
			break;
		for (e = 0; e <= dcount; ++e) {
			extern asmlinkage int old_readdir(unsigned int fd, void * dirent, unsigned int count);
			if (old_readdir(fd, &de, 1) < 0) {
				hijack_get_time_offset(fd, CURRENT_TIME);
				sys_close(fd);
				goto done;
			}
		}
		sys_close(fd);
		strcat(hijack_zoneinfo, "/");
		strcat(hijack_zoneinfo, de.d_name);
	}
	hijack_zoneinfo[0] = '\0';
done:
	set_fs(old_fs);
}

void	// invoked once from empeg_display.c
hijack_init (void *animptr)
{
	extern void hijack_notify_init (void);
	int failed;
	char buf[128], msg[32];
	unsigned long anistart = HZ;

	hijack_time_offset = 0;
	hijack_zoneinfo[0] = '\0';
	hijack_khttpd_new_fid_dirs = 1;	// look for new fids directory structure
	hijack_player_config_ini_pid = 0;
	hijack_game_animptr = animptr;
	hijack_buttonled_level = 0;	// turn off button LEDs
	msg[0] = '\0';
	failed = hijack_restore_settings(buf, msg);

	menu_init();
	reset_hijack_options();
	hijack_initq(&hijack_inputq, 'I');
	hijack_initq(&hijack_playerq, 'P');
	hijack_initq(&hijack_userq, 'U');
	hijack_notify_init();
	if (failed) {
		if (failed == 2)
			show_message("Hijack Settings Reset", HZ*7);
		else
			show_message("Player Settings Lost", HZ*7);
	} else if (msg[0]) {
		show_message(msg, HZ);
	} else {
		anistart = HZ/2;
		show_message(hijack_vXXX_by_Mark_Lord, anistart);
	}
}
