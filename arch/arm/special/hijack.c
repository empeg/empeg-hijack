// Empeg hacks by Mark Lord <mlord@pobox.com>
//
#define HIJACK_VERSION	"v263"
const char hijack_vXXX_by_Mark_Lord[] = "Hijack "HIJACK_VERSION" by Mark Lord";

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

#include <linux/empeg.h>
#include <asm/uaccess.h>

#include <asm/arch/hijack.h>		// for ioctls, IR_ definitions, etc..
#include <linux/soundcard.h>		// for SOUND_MASK_*
#include "../../../drivers/block/ide.h"	// for ide_hwifs[]
#include "empeg_display.h"
#include "empeg_mixer.h"

extern void display_sendcontrol_part1(void);				// arch/arm/special/empeg_display.c
extern void display_sendcontrol_part2(int);				// arch/arm/special/empeg_display.c
extern int remount_drives (int writeable);				// arch/arm/special/notify.c
extern int sys_newfstat(int, struct stat *);
extern int sys_sync(void);						// fs/buffer.c
extern int get_loadavg(char * buffer);					// fs/proc/array.c
extern void machine_restart(void *);					// arch/arm/kernel/process.c
extern int real_input_append_code(unsigned long data);			// arch/arm/special/empeg_input.c
extern int empeg_state_dirty;						// arch/arm/special/empeg_state.c
extern void state_cleanse(void);					// arch/arm/special/empeg_state.c
extern void hijack_serial_insert (const char *buf, int size, int port);	// drivers/char/serial_sa1100.c
extern void hijack_voladj_intinit(int, int, int, int, int);		// arch/arm/special/empeg_audio3.c
extern void hijack_beep (int pitch, int duration_msecs, int vol_percent);// arch/arm/special/empeg_audio3.c
extern unsigned long jiffies_since(unsigned long past_jiffies);		// arch/arm/special/empeg_input.c
extern void display_blat(struct display_dev *dev, unsigned char *source_buffer); // empeg_display.c
extern tm_t *hijack_convert_time(time_t, tm_t *);			// from arch/arm/special/notify.c

extern void empeg_mixer_select_input(int input);			// arch/arm/special/empeg_mixer.c
extern void hijack_tone_set(int, int, int, int, int, int);					// arch/arm/special/empeg_mixer.c
extern int empeg_readtherm(volatile unsigned int *timerbase, volatile unsigned int *gpiobase);	// arch/arm/special/empeg_therm.S
extern int empeg_inittherm(volatile unsigned int *timerbase, volatile unsigned int *gpiobase);	// arch/arm/special/empeg_therm.S
       int display_ioctl (struct inode *inode, struct file *filp, unsigned int cmd, unsigned long arg); //arch/arm/special/empeg_display.c

#ifdef CONFIG_NET_ETHERNET	// Mk2 or later? (Mk1 has no ethernet)
#define EMPEG_KNOB_SUPPORTED	// Mk2 and later have a front-panel knob
#endif
int	hijack_standby_time = 0;	// jiffies since we entered standby (max 12 hours)
int	hijack_got_config_ini = 0;	// used by restore visuals, to avoid triggering on a boot logo
int	kenwood_disabled;		// used by Nextsrc button
int	empeg_on_dc_power;		// used in arch/arm/special/empeg_power.c
int	empeg_tuner_present = 0;	// used by NextSrc button, perhaps has other uses
int	hijack_fsck_disabled = 0;	// used in fs/ext2/super.c
int	hijack_onedrive = 0;		// used in drivers/block/ide-probe.c
int	hijack_reboot = 0;		// set to "1" to cause reboot on next display refresh
int	hijack_player_is_restarting = 0;// used in fs/read_write.c, fs/exec.c
unsigned int hijack_player_started = 0;	// set to jiffies when player startup is detected on serial port (notify.c)

static unsigned int PROMPTCOLOR = COLOR3, ENTRYCOLOR = -COLOR3;

#define NEED_REFRESH		0
#define NO_REFRESH		1
#define SHOW_PLAYER		2

#define HIJACK_IDLE		0
#define HIJACK_IDLE_PENDING	1
#define HIJACK_ACTIVE_PENDING	2
#define HIJACK_ACTIVE		3

static unsigned int homevisuals_enabled = 0;	// fixme: temporary workaround
static unsigned int carvisuals_enabled = 0;
static unsigned int restore_visuals = 0;
static unsigned int info_screenrow = 0;
static unsigned int hijack_status = HIJACK_IDLE;
static unsigned long hijack_last_moved = 0, hijack_last_refresh = 0, blanker_triggered = 0, blanker_lastpoll = 0;
static unsigned char blanker_lastbuf[EMPEG_SCREEN_BYTES] = {0,};
const unsigned char hexchars[] = "0123456789ABCDEFabcdef";

static int  (*hijack_dispfunc)(int) = NULL;
static void (*hijack_movefunc)(int) = NULL;

// This is broken.. we have run out of bits here because much of this
//  because most of these flags really need to be processed by handle_buttons()
//  rather than by input_append_code().  To fix it, we must defer translations
//  to later in the chain somehow..
#define BUTTON_FLAGS_LONGPRESS	(0x80000000)	// send this out as a long press
#define BUTTON_FLAGS_SHIFT	(0x40000000)	// toggle shift state when sending
#define BUTTON_FLAGS_UI		(0x20000000)	// send only if player menus are idle
#define BUTTON_FLAGS_NOTUI	(0x10000000)	// send only if player menus are active
#define BUTTON_FLAGS_ALTNAME	(0x08000000)	// use alternate name when displaying
#define BUTTON_FLAGS_UISTATE	(BUTTON_FLAGS_UI|BUTTON_FLAGS_NOTUI)
#define BUTTON_FLAGS		(0xff000000)
#define IR_NULL_BUTTON		(~BUTTON_FLAGS)

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

static int stalk_on_left = 0;
static min_max_t rhs_stalk_vals[11];	// 10 sets of values, followed by {-1,-1} terminator.
static min_max_t lhs_stalk_vals[11];	// 10 sets of values, followed by {-1,-1} terminator.

static const min_max_t rhs_stalk_default[] = {
	{0x00, 0x07},	// IR_KOFF_PRESSED
	{0x10, 0x1c},	// IR_KSOURCE_PRESSED
	{0x24, 0x30},	// IR_KATT_PRESSED
	{0x34, 0x40},	// IR_KFRONT_PRESSED
	{0x42, 0x4e},	// IR_KNEXT_PRESSED
	{0x54, 0x60},	// IR_KPREV_PRESSED
	{0x68, 0x74},	// IR_KVOLUP_PRESSED
	{0x7e, 0x8a},	// IR_KVOLDOWN_PRESSED
	{0x94, 0xa0},	// IR_KREAR_PRESSED
	{0xa0, 0xb5}};	// IR_KBOTTOM_PRESSED

static const min_max_t lhs_stalk_default[] = {
	{0x00, 0x07},	// IR_KOFF_PRESSED
	{0x10, 0x1c},	// IR_KSOURCE_PRESSED
	{0x24, 0x30},	// IR_KATT_PRESSED
	{0x34, 0x40},	// IR_KREAR_PRESSED
	{0x42, 0x4e},	// IR_KPREV_PRESSED
	{0x54, 0x60},	// IR_KNEXT_PRESSED
	{0x68, 0x74},	// IR_KVOLDOWN_PRESSED
	{0x7e, 0x8a},	// IR_KVOLUP_PRESSED
	{0x94, 0xa0},	// IR_KFRONT_PRESSED
	{0xa0, 0xb5}};	// IR_KBOTTOM_PRESSED

static unsigned long ir_lastevent = 0, ir_lasttime = 0, ir_selected = 0;
static unsigned int ir_releasewait = IR_NULL_BUTTON, ir_trigger_count = 0;;
static unsigned long ir_menu_down = 0, ir_left_down = 0, ir_right_down = 0, ir_4_down = 0;
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
#define IR_FAKE_HIJACKMENU	(IR_NULL_BUTTON-16)	// This MUST be the lowest numbered FAKE code
#define ALT			BUTTON_FLAGS_ALTNAME

typedef struct ir_translation_s {
	unsigned int	old;		// original button, IR_NULL_BUTTON means "end-of-table"
	unsigned short	flags;		// boolean flags
	unsigned char	count;		// how many codes in new[]
	unsigned char	popup_index;	// for PopUp translations only: most recent menu position
	unsigned int	new[0];		// start of macro table with replacement buttons to send
} ir_translation_t;

// a default translation for PopUp0 menu:
static struct {
		ir_translation_t	hdr;
		unsigned int		buttons[8];
	} popup0_default_translation =
	{	{IR_FAKE_POPUP0, 0, 8, 0},
		{IR_FAKE_CLOCK,		IR_RIO_INFO_PRESSED,
		 IR_FAKE_KNOBSEEK,	IR_RIO_MARK_PRESSED|ALT,
		 IR_FAKE_NEXTSRC,	IR_RIO_0_PRESSED|ALT,
		 IR_FAKE_VOLADJMENU,	IR_RIO_VISUAL_PRESSED }
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
	{"VolAdj",	IR_FAKE_VOLADJMENU},
	{"HijackMenu",	IR_FAKE_HIJACKMENU},

	{"Initial",	IR_FAKE_INITIAL},
	{"null",	IR_NULL_BUTTON},
	{"Source",	IR_RIO_SOURCE_PRESSED},
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
	{"Pause",	IR_RIO_PLAY_PRESSED|ALT},
	{"Play",	IR_RIO_PLAY_PRESSED},
	{"Hush",	IR_RIO_PLAY_PRESSED|ALT|BUTTON_FLAGS_LONGPRESS},

	{"Top",		IR_TOP_BUTTON_PRESSED},
	{"Bottom",	IR_BOTTOM_BUTTON_PRESSED},
	{"Left",	IR_LEFT_BUTTON_PRESSED},
	{"Right",	IR_RIGHT_BUTTON_PRESSED},
	{"KnobLeft",	IR_KNOB_LEFT},
	{"KnobRight",	IR_KNOB_RIGHT},
	{"Knob",	IR_KNOB_PRESSED},

	{"AM",		IR_KW_AM_PRESSED},
	{"FM",		IR_KW_FM_PRESSED},
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

	{"Visual+",	IR_RIO_VISUAL_PRESSED|ALT},
	{"Visual-",	IR_KSNEXT_PRESSED|ALT},	// Stalk
	{"Visual",	IR_RIO_VISUAL_PRESSED},

	{"\0",		IR_NULL_BUTTON}		// end-of-table-marker
	};
#undef ALT

#define KNOBDATA_BITS 3
#define KNOBDATA_SIZE (1 << KNOBDATA_BITS)
static int knobdata_index = 0;
static int popup0_index = 0;		// (PopUp0) saved/restored index

#ifdef EMPEG_KNOB_SUPPORTED

static int hijack_knobseek = 0;
static unsigned long ir_knob_busy = 0, ir_knob_down = 0;

// Mmm.. this *could* be eliminated entirely, in favour of IR-translations and PopUp's..
// But for now, we leave it in because it is (1) a Major convenience, and (2) can be modified "on the fly".
static const char *knobdata_labels[] = {"[default]", button_names[0].name, "VolAdj+", "Details", "Info", "Mark", "Shuffle", "NextSrc"};
static const unsigned int knobdata_buttons[1<<KNOBDATA_BITS] = {
	IR_KNOB_PRESSED,
	IR_FAKE_POPUP0,
	IR_KNOB_PRESSED,
	IR_RIO_INFO_PRESSED|BUTTON_FLAGS_LONGPRESS,
	IR_RIO_INFO_PRESSED,
	IR_RIO_MARK_PRESSED,
	IR_RIO_SHUFFLE_PRESSED,
	IR_FAKE_NEXTSRC,
	};

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
	unsigned short		head;
	unsigned short		tail;
	unsigned long		last_deq;
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
struct semaphore hijack_kftpd_startup_sem	= MUTEX_LOCKED;	// sema for waking up kftpd after we read config.ini
struct semaphore hijack_khttpd_startup_sem	= MUTEX_LOCKED;	// sema for waking up khttpd after we read config.ini
#endif // CONFIG_NET_ETHERNET

static hijack_buttonq_t hijack_inputq, hijack_playerq, hijack_userq;

// Externally tuneable parameters for config.ini; the voladj_parms are also tuneable
//
static	int hijack_buttonled_off_level;		// button brightness when player is "off"
static	int hijack_button_pacing;		// minimum spacing between press/release pairs within playerq
static	int hijack_dc_servers;			// 1 == allow kftpd/khttpd when on DC power
	int hijack_disable_emplode;		// 1 == block TCP port 8300 (Emplode/Emptool)
	int hijack_extmute_off;			// buttoncode to inject when EXT-MUTE goes inactive
	int hijack_extmute_on;			// buttoncode to inject when EXT-MUTE goes active
static	int hijack_ir_debug;			// printk() for every ir press/release code
static	int hijack_spindown_seconds;		// drive spindown timeout in seconds
	int hijack_fake_tuner;			// pretend we have a tuner, when we really don't have one
	int hijack_trace_tuner;			// dump incoming tuner/stalk packets onto console
#ifdef CONFIG_NET_ETHERNET
	char hijack_kftpd_password[16];		// kftpd password
	int hijack_kftpd_control_port;		// kftpd control port
	int hijack_kftpd_data_port;		// kftpd data port
	int hijack_kftpd_verbose;		// kftpd verbosity
	int hijack_rootdir_dotdot;		// 1 == show '..' in rootdir listings
	int hijack_kftpd_show_dotfiles;		// 1 == show '.*' in rootdir listings
	int hijack_khttpd_show_dotfiles;		// 1 == show '.*' in rootdir listings
	int hijack_max_connections;		// restricts memory use
	int hijack_khttpd_port;			// khttpd port
	int hijack_khttpd_verbose;		// khttpd verbosity
	int hijack_khttpd_dirs;			// 1 == enable directory listings
	int hijack_khttpd_files;			// 1 == enable file downloads, except "streaming"
	int hijack_khttpd_playlists;		// 1 == enable "?.html" or "?.m3u" functionality
	int hijack_khttpd_commands;		// 1 == enable "?commands" capability
#endif // CONFIG_NET_ETHERNET
static	int hijack_old_style;			// 1 == don't highlite menu items
static	int hijack_quicktimer_minutes;		// increment size for quicktimer function
static	int hijack_stalk_debug;			// trace button in/out actions to/from Stalk?
static	int hijack_standby_minutes;		// number of minutes after screen blanks before we go into standby
	int hijack_suppress_notify;		// 1 == suppress player "notify" and "dhcp" text on serial port
	int hijack_time_offset;			// adjust system time-of-day clock by this many minutes
	int hijack_temperature_correction;	// adjust all h/w temperature readings by this celcius amount
	int hijack_standbyLED_on, hijack_standbyLED_off;	// on/off duty cycle for standby LED


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

typedef struct hijack_option_s {
	char	*name;
	void	*target;
	int	defaultval;  // or (void *)
	int	num_items;
	int	min;
	int	max;
} hijack_option_t;

char hijack_khttpd_style[64];

static const hijack_option_t hijack_option_table[] =
{
// config.ini string		address-of-variable		default			howmany	min	max
//===========================	==========================	=========		=======	===	================
{"buttonled_off",		&hijack_buttonled_off_level,	1,			1,	0,	7},
{"button_pacing",		&hijack_button_pacing,		20,			1,	0,	HZ},
{"dc_servers",			&hijack_dc_servers,		0,			1,	0,	1},
{"disable_emplode",		&hijack_disable_emplode,	0,			1,	0,	1},
{"spindown_seconds",		&hijack_spindown_seconds,	30,			1,	0,	(239 * 5)},
{"extmute_off",			&hijack_extmute_off,		0,			1,	0,	IR_NULL_BUTTON},
{"extmute_on",			&hijack_extmute_on,		0,			1,	0,	IR_NULL_BUTTON},
{"fake_tuner",			&hijack_fake_tuner,		0,			1,	0,	1},
{"ir_debug",			&hijack_ir_debug,		0,			1,	0,	1},
#ifdef CONFIG_NET_ETHERNET
{"kftpd_control_port",		&hijack_kftpd_control_port,	21,			1,	0,	65535},
{"kftpd_data_port",		&hijack_kftpd_data_port,	20,			1,	0,	65535},
{"kftpd_password",		&hijack_kftpd_password,		(int)"",		0,	0,	sizeof(hijack_kftpd_password)-1},
{"kftpd_verbose",		&hijack_kftpd_verbose,		0,			1,	0,	1},
{"rootdir_dotdot",		&hijack_rootdir_dotdot,		0,			1,	0,	1},
{"kftpd_show_dotfiles",		&hijack_kftpd_show_dotfiles,	0,			1,	0,	1},
{"khttpd_show_dotfiles",	&hijack_khttpd_show_dotfiles,	1,			1,	0,	1},
{"khttpd_port",			&hijack_khttpd_port,		80,			1,	0,	65535},
{"khttpd_verbose",		&hijack_khttpd_verbose,		0,			1,	0,	2},
{"khttpd_dirs",			&hijack_khttpd_dirs,		1,			1,	0,	1},
{"khttpd_files",		&hijack_khttpd_files,		1,			1,	0,	1},
{"khttpd_playlists",		&hijack_khttpd_playlists,	1,			1,	0,	1},
{"khttpd_commands",		&hijack_khttpd_commands,	1,			1,	0,	1},
{"khttpd_style",		&hijack_khttpd_style,		(int)"/default.xsl",	0,	0,	sizeof(hijack_khttpd_style)-1},
{"max_connections",		&hijack_max_connections,	4,			1,	0,	20},
#endif // CONFIG_NET_ETHERNET
{"old_style",			&hijack_old_style,		0,			1,	0,	1},
{button_names[0].name,		button_names[0].name,		(int)"PopUp0",		0,	0,	8},
{button_names[1].name,		button_names[1].name,		(int)"PopUp1",		0,	0,	8},
{button_names[2].name,		button_names[2].name,		(int)"PopUp2",		0,	0,	8},
{button_names[3].name,		button_names[3].name,		(int)"PopUp3",		0,	0,	8},
{"quicktimer_minutes",		&hijack_quicktimer_minutes,	30,			1,	1,	120},
{"stalk_debug",			&hijack_stalk_debug,		0,			1,	0,	1},
{"stalk_lhs",			lhs_stalk_vals,			(int)lhs_stalk_default,	20,	0,	0xff},
{"stalk_rhs",			rhs_stalk_vals,			(int)rhs_stalk_default,	20,	0,	0xff},
{"standbyLED_on",		&hijack_standbyLED_on,		-1,			1,	-1,	HZ*60},
{"standbyLED_off",		&hijack_standbyLED_off,		10*HZ,			1,	0,	HZ*60},
{"standby_minutes",		&hijack_standby_minutes,	30,			1,	0,	240},
{"supress_notify",		&hijack_suppress_notify,	0,			1,	0,	1},
{"suppress_notify",		&hijack_suppress_notify,	0,			1,	0,	1},
{"temperature_correction",	&hijack_temperature_correction,	-4,			1,	-20,	+20},
{"time_offset",			&hijack_time_offset,		0,			1,	-24*60,	24*60},
{"trace_tuner",			&hijack_trace_tuner,		0,			1,	0,	1},
{button_names[4].name,		&hijack_voladj_parms[0][0],	(int)voladj_ldefault,	5,	0,	0x7ffe},
{button_names[5].name,		&hijack_voladj_parms[1][0],	(int)voladj_mdefault,	5,	0,	0x7ffe},
{button_names[6].name,		&hijack_voladj_parms[2][0],	(int)voladj_hdefault,	5,	0,	0x7ffe},
{"bass_freq",			&hijack_bass_freq,		0x6a7d,			1,	0x0000,	0xffff}, // For now, these are the internal register format.
{"bass_q",			&hijack_bass_q,			0x4b00,			1,	0x0000,	0xffff}, // Useful in config.ini to set my suggested values as default
{"treble_freq",			&hijack_treble_freq,		0x6d50,			1,	0x0000,	0xffff}, // and for testing purposes. We shouldn't advertise these settings
{"treble_q",			&hijack_treble_q,		0xc800,			1,	0x0000,	0xffff}, // as we don't know what they really mean.  --genixia
{"volume_boost_FM",		&hijack_volboost[0],		0,			1,	-100,	100},
{"volume_boost_MP3",		&hijack_volboost[1],		0,			1,	-100,	100},
{"volume_boost_AUX",		&hijack_volboost[2],		0,			1,	-100,	100},
{"volume_boost_AM",		&hijack_volboost[3],		0,			1,	-100,	100},
{"disable_bassboost_FM",	&hijack_disable_bassboost_FM,	0,			1,	0,	1,},
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
static const char homevisuals_menu_label[] = "Restore Visuals (v2beta11)";	// fixme: workaround for beta11
static const char blankerfuzz_menu_label[] = "Screen Blanker Sensitivity";
static const char blanker_menu_label	[] = "Screen Blanker Timeout";
static const char bass_menu_label       [] = "Tone: Bass Adjust";
static const char treble_menu_label     [] = "Tone: Treble Adjust";

#define HIJACK_USERQ_SIZE	8
static const unsigned int intercept_all_buttons[] = {1};
static const unsigned int *hijack_buttonlist = NULL;
//static unsigned long hijack_userq[HIJACK_USERQ_SIZE];
//static unsigned short hijack_userq_head = 0, hijack_userq_tail = 0;
static struct wait_queue *hijack_userq_waitq = NULL, *hijack_menu_waitq = NULL;

#define SCREEN_BLANKER_MULTIPLIER 15
#define BLANKER_BITS 6
static int blanker_timeout = 0;

#define SENSITIVITY_MULTIPLIER 5
#define SENSITIVITY_BITS 3
static int blanker_sensitivity = 0;

#define HIGHTEMP_OFFSET	34
#define HIGHTEMP_BITS	5
static int hightemp_threshold = 0;

#define FORCEPOWER_BITS 2
static int hijack_force_power = 0;

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
	//{0x00,0x02,0x7f,0x00,0x00,0x00}, // 1
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
	//{0x01,0x99,0x09,0x06,0x00,0x00}, // ?
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
	//{0x40,0x41,0x41,0x3f,0x00,0x00}, // J
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
	//{0x02,0x01,0x01,0x02,0x02,0x01}  // ~
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

static int
draw_char (unsigned short pixel_row, short pixel_col, unsigned char c, unsigned char foreground, unsigned char background)
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
hijack_initq (hijack_buttonq_t *q)
{
	q->head = q->tail = q->last_deq = 0;
}

static unsigned long
PRESSCODE (unsigned int button)
{
	button &= ~BUTTON_FLAGS;
	if (button > 0xf || button == IR_KNOB_LEFT || button == IR_KNOB_RIGHT)
		return button;
	return button & ~1;
}

static unsigned long
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

	save_flags_cli(flags);
	head = q->head;
	if (head != q->tail && hold_time < hijack_button_pacing && q == &hijack_playerq && !IS_RELEASE(button))
		hold_time = hijack_button_pacing;	// ensure we have sufficient delay between press/release pairs
	if (hijack_ir_debug)
		printk("%lu: ENQ.%c: %08x.%ld\n", jiffies, (q == &hijack_playerq) ? 'P' : ((q == &hijack_inputq) ? 'I' : 'U'), button, hold_time);
	if (++head >= HIJACK_BUTTONQ_SIZE)
		head = 0;
	if (head != q->tail) {
		hijack_buttondata_t *data = &q->data[q->head = head];
		data->button = button;
		data->delay  = hold_time;
	}
	restore_flags(flags);
}

// Send a release code
static void
hijack_enq_release (hijack_buttonq_t *q, unsigned int rawbutton, unsigned long hold_time)
{
	unsigned int button = RELEASECODE(rawbutton);
	if (button != IR_NULL_BUTTON) {
		button |= (rawbutton & BUTTON_FLAGS_UISTATE);
		if (rawbutton & BUTTON_FLAGS_LONGPRESS)
			hold_time = HZ;
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

static int
hijack_button_deq (hijack_buttonq_t *q, hijack_buttondata_t *rdata, int nowait)
{
	hijack_buttondata_t *data;
	unsigned short tail;
	unsigned long flags;

	save_flags_cli(flags);
	if ((tail = q->tail) != q->head) {	// anything in the queue?
		if (++tail >= HIJACK_BUTTONQ_SIZE)
			tail = 0;
		data = &q->data[tail];
		if (nowait || !data->delay || jiffies_since(q->last_deq) >= data->delay) {
			rdata->button = data->button;
			rdata->delay  = data->delay;
			q->tail = tail;
			q->last_deq = jiffies;
			restore_flags(flags);
			return 1;	// button was available
		}
	}
	restore_flags(flags);
	return 0;	// no buttons available
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
	hijack_buttonlist = NULL;
	ir_trigger_count = 0;
	hijack_overlay_geom = NULL;
	hijack_status = new_status;
	restore_flags(flags);
}

static void
activate_dispfunc (int (*dispfunc)(int), void (*movefunc)(int))
{
	hijack_deactivate(HIJACK_ACTIVE_PENDING);
	hijack_dispfunc = dispfunc;
	hijack_movefunc = movefunc;
	blanker_triggered = 0;
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
		hijack_last_moved = jiffies ? jiffies : -1;
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

static unsigned long inittherm_lasttime = 0;
static unsigned long hijack_last_readtherm = 0;
static int           hijack_last_temperature = 0;

static void
init_temperature (int firsttime)
{
	unsigned long flags;

	// restart the thermometer once every five minutes or so
	save_flags_clif(flags);
	if (firsttime || jiffies_since(inittherm_lasttime) >= (HZ*5*60)) {
		(void)empeg_inittherm(&OSMR0,&GPLR);
		hijack_last_readtherm = inittherm_lasttime = jiffies;
		restore_flags(flags);
		hijack_last_readtherm = inittherm_lasttime = jiffies;
	} else {
		restore_flags(flags);
	}
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

	save_flags_clif(flags);
	init_temperature(0);
	if (jiffies_since(hijack_last_readtherm) < (HZ*5))
	{
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

static unsigned int savearea_display_offset = 0;
static unsigned char *last_savearea;
static unsigned long *last_updated  = NULL;
unsigned char **empeg_state_writebuf;	// initialized in empeg_state.c

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
	if (firsttime) {
		if (!last_savearea)
			last_savearea = kmalloc(128, GFP_KERNEL);
		if (!last_updated)
			last_updated = kmalloc(128 * sizeof(long), GFP_KERNEL);
		if (!last_savearea || !last_updated) {
			printk("savearea_display: no memory\n");
			ir_selected = 1;
			return NO_REFRESH;
		}
		memcpy(last_savearea, empeg_statebuf, 128);
		memset(last_updated, 0, 128 * sizeof(long));
		rc = NEED_REFRESH;
	} else if (jiffies_since(hijack_last_refresh) >= (HZ/4)) {
		rc = NEED_REFRESH;
	}
	if (rc == NEED_REFRESH) {
		unsigned int i, rowcol = ROWCOL(0,0), offset = savearea_display_offset;
		for (i = 0; i < 32; ++i) {
			unsigned long elapsed;
			unsigned int addr = (offset + i) & 0x7f, color = COLOR2;
			unsigned char b = empeg_statebuf[addr];
			if (b != last_savearea[addr])
				last_updated[addr] = jiffies ? jiffies : -1;
			last_savearea[addr] = b;
			if (last_updated[addr]) {
				elapsed = jiffies_since(last_updated[addr]);
				if (elapsed < (10*HZ))
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

#ifdef DEBUG_JIFFIES
static int
debug_display (int firsttime)
{
	unsigned char buf[64];
	unsigned long flags;
	unsigned int rowcol = ROWCOL(0,0), bug = 0, ossr, oscr, osmr0;
	static unsigned long counter, oldjiffies;

	if (firsttime)
		counter = oldjiffies = 0;
	else if ((++counter & 0xf))
		return NO_REFRESH;
	clear_hijack_displaybuf(COLOR0);
	save_flags_clif(flags);
		ossr  = OSSR;
		oscr  = OSCR;
		osmr0 = OSMR0;
	restore_flags(flags);
	if (osmr0 > oscr) {
		if ((unsigned)(osmr0 - oscr) > (unsigned)(2 * 36864))
			bug = 1;
	} else {
		if ((unsigned)(oscr - osmr0) < (unsigned)(-(2 * 36864)))
			bug = 2;
	}
	if (bug) {
		if (jiffies == oldjiffies)
			bug = -bug;
		sprintf(buf, "BUG-%d", bug);
		rowcol = draw_string_spaced(rowcol, buf, -COLOR3);
	}
	sprintf(buf, "cr:%08x,mr:%08x", oscr, osmr0);
	if ((counter & 511) == 0)
		printk("DEBUG: bug=%d, jiffies=%lu, cr:%08x, mr:%08x, sr:%08x\n", bug, jiffies, oscr, osmr0, ossr);
	draw_string(ROWCOL(2,0), buf, PROMPTCOLOR);

	sprintf(buf, "sr:%08x", ossr);
	draw_string(ROWCOL(3,0), buf, PROMPTCOLOR);
	oldjiffies = jiffies;
	sprintf(buf, "Jiffies=%lu",  oldjiffies);
	rowcol = draw_string(rowcol, buf, PROMPTCOLOR);
	clear_text_row(rowcol, EMPEG_SCREEN_COLS, 0);

	sprintf(buf, "in=%u, ti=%u",  kstat_irqs(4), kstat_irqs(26));
	draw_string(ROWCOL(1,0), buf, PROMPTCOLOR);
	return NEED_REFRESH;
}
#endif // DEBUG_JIFFIES

static int
vitals_display (int firsttime)
{
	extern int nr_free_pages;
	extern const char *notify_fid(void);
	unsigned int *permset=(unsigned int*)(EMPEG_FLASHBASE+0x2000);
	unsigned char buf[80];
	int rowcol, temp, i, count, model = 0x2a;
	unsigned char *sa;
	unsigned long flags;

	if (!firsttime && jiffies_since(hijack_last_refresh) < HZ)
		return NO_REFRESH;
	clear_hijack_displaybuf(COLOR0);

	// Model, Drives, Temperature:
	if (permset[0] < 7)
		model = 1;
	else if (permset[0] < 9)
		model = 2;
	count = sprintf(buf, "Mk%x:%d", model, get_drive_size(0,0));
	model = (model == 1);	// 0 == Mk2(a); 1 == Mk1
	if (ide_hwifs[model].drives[!model].present)
		sprintf(buf+count, "+%d", get_drive_size(model,!model));
	rowcol = draw_string(ROWCOL(0,0), buf, PROMPTCOLOR);
	temp = hijack_read_temperature();
	sprintf(buf, "G, %+dC/%+dF", temp, temp * 180 / 100 + 32);
	rowcol = draw_string(rowcol, buf, PROMPTCOLOR);

	// Current Playlist and Fid:
	save_flags_cli(flags);
	sa = *empeg_state_writebuf;
	sprintf(buf, "\nPlaylist:%02x%02x, Fid:%s", sa[0x45], sa[0x44], notify_fid());
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

static const char *powermode_text[FORCEPOWER_BITS<<1] = {"[Normal]", "[Normal]", "Force AC/Home", "Force DC/Car"};

static void
forcepower_move (int direction)
{
	if (direction < 0) {
		if (--hijack_force_power < 1)
			hijack_force_power = 3;
	} else if (++hijack_force_power == 1) {
		hijack_force_power = 2;
	} else if (hijack_force_power > 3) {
		hijack_force_power = 0;
	}
	empeg_state_dirty = 1;
}

static int
forcepower_display (int firsttime)
{
	unsigned int rowcol;

	if (!firsttime && !hijack_last_moved)
		return NO_REFRESH;
	hijack_last_moved = 0;
	clear_hijack_displaybuf(COLOR0);
	(void)draw_string(ROWCOL(0,0), forcepower_menu_label, PROMPTCOLOR);
	rowcol = draw_string(ROWCOL(1,0), "Current Mode: ", PROMPTCOLOR);
	(void)draw_string(rowcol, empeg_on_dc_power ? "DC/Car" : "AC/Home", PROMPTCOLOR);
	rowcol = draw_string(ROWCOL(3,0), "On reboot: ", PROMPTCOLOR);
	(void)draw_string_spaced(rowcol, powermode_text[hijack_force_power], ENTRYCOLOR);
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
	timer_started = jiffies;
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

static void	// fixme: temporary workaround
homevisuals_move (int direction)
{
	homevisuals_enabled = !homevisuals_enabled;
	empeg_state_dirty = 1;
}

static int	// fixme: temporary workaround
homevisuals_display (int firsttime)
{
	unsigned int rowcol;

	if (!firsttime && !hijack_last_moved)
		return NO_REFRESH;
	hijack_last_moved = 0;
	clear_hijack_displaybuf(COLOR0);
	(void)draw_string(ROWCOL(0,0), homevisuals_menu_label, PROMPTCOLOR);
	rowcol = draw_string(ROWCOL(2,20), "Restore: ", PROMPTCOLOR);
	(void)   draw_string_spaced(rowcol, disabled_enabled[homevisuals_enabled], ENTRYCOLOR);
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
	static unsigned long lasttime = 0;
	static int dimmer = 0;
	int sense;

	sense = !(GPLR & EMPEG_SERIALCTS);   // EMPEG_SERIALCTS is active low.

	if (!sense) {
		if (jiffies_since(lasttime) > (HZ/3))
			dimmer = sense;
		else
			dimmer = !sense;
	} else {
		lasttime = jiffies;
		dimmer = sense;
	}	
	return dimmer;
}

static void	// invoked from empeg_display.c
hijack_adjust_buttonled (int power)
{
	extern unsigned int display_sendcontrol_serial;
	static unsigned long lasttime = 0, command = 0, saved_serial = 0;
	const unsigned char bright_levels[1<<BUTTONLED_BITS] = {0, 1, 2, 4, 7, 14, 24, 255};
	int brightness;
	unsigned long flags;

	save_flags_clif(flags);

	// illumination command already in progress?
	if (command) {
		if (jiffies_since(lasttime) <= (HZ/8)) {
			restore_flags(flags);
			return;
		}
		if (display_sendcontrol_serial == saved_serial)	// display untouched since last time?
			display_sendcontrol_part2(command);
		else
			hijack_buttonled_level = 256;		// force illumination to be reset
		command = 0;
	}

	// start a new command if level needs adjustment:
	if (power)
		brightness = hijack_buttonled_on_level;
	else if (hijack_buttonled_on_level)
		brightness = hijack_buttonled_off_level;
	else
		brightness = 0;
	if (headlight_sense_is_active())
		brightness = (brightness + 1) / 2;
	brightness = bright_levels[brightness];
	if (hijack_buttonled_level != brightness) {
		if (brightness == 255)
			command = 244;
		else if (hijack_buttonled_level > brightness)
			command = 242;	// turn off LEDs and ramp up again
		else if (hijack_buttonled_level < brightness)
			command = 243;	// brighten LEDs by 5/255 (2%)
		if (command) {
			display_sendcontrol_part1();
			saved_serial = display_sendcontrol_serial;
			lasttime = jiffies;
			switch (command) {
				case 242: hijack_buttonled_level = 0;	break;
				case 243: hijack_buttonled_level++;	break;
				case 244: hijack_buttonled_level = 255;	break;
			}
		}
	}
	restore_flags(flags);
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
	else
		(void) draw_number(rowcol, level, "%2u", ENTRYCOLOR);
	return NEED_REFRESH;
}

#ifdef EMPEG_KNOB_SUPPORTED

static void
knobdata_move (int direction)
{
	knobdata_index = direction ? (knobdata_index + direction) & (KNOBDATA_SIZE-1) : 0;
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

static unsigned long knobseek_lasttime;

static void
knobseek_move (int direction)
{
	unsigned int button;

	if (jiffies_since(knobseek_lasttime) >= (HZ/4)) {
		knobseek_lasttime = jiffies;
		button = (direction > 0) ? IR_RIGHT_BUTTON_PRESSED : IR_LEFT_BUTTON_PRESSED;
		hijack_enq_button_pair(button);
	}
}

static int
knobseek_display (int firsttime)
{
	static const hijack_geom_t geom = {8, 8+6+KFONT_HEIGHT, 20, EMPEG_SCREEN_COLS-20};

	if (firsttime) {
		unsigned int rowcol = (geom.first_row+4)|((geom.first_col+6)<<16);
		create_overlay(&geom);
		rowcol = draw_string(rowcol, "Knob \"Seek\" Mode", COLOR3);
		hijack_knobseek = 1;
		knobseek_lasttime = hijack_last_moved = jiffies ? jiffies : -1;
	} else if (jiffies_since(hijack_last_moved) >= (HZ*5)) {
		hijack_knobseek = 0;
		hijack_deactivate(HIJACK_IDLE);
	}
	return NO_REFRESH;	// gets overridden if overlay still active
}

#endif // EMPEG_KNOB_SUPPORTED

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
			popup0_index = current_popup->popup_index & 7;
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
		hijack_last_moved = jiffies ? jiffies : -1;
		ir_numeric_input = &popup_index;	// allows cancel/top to reset it to 0
		hijack_buttonlist = popup_buttonlist;
		hijack_initq(&hijack_userq);
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
				popup_got_press = jiffies ? jiffies : -1;
				hijack_enq_button(&hijack_playerq, b, 0);
				if (button & BUTTON_FLAGS_LONGPRESS)
					hijack_enq_release(&hijack_playerq, b, HZ);
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

static void
popup_activate (unsigned int button)
{
	button &= ~BUTTON_FLAGS;
	current_popup = ir_next_match(NULL, button);
	if (current_popup == NULL && button == IR_FAKE_POPUP0)
		current_popup = &popup0_default_translation.hdr;
	if (current_popup != NULL) {
		if (current_popup->old == IR_FAKE_POPUP0)
			current_popup->popup_index = (current_popup->popup_index & ~7) | popup0_index;
		activate_dispfunc(popup_display, popup_move);
	}
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
static unsigned long game_ball_last_moved, game_animtime;
       unsigned int *hijack_game_animptr = NULL;	// written by empeg_display.c

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
			(void)draw_string_spaced(ROWCOL(2,4), hijack_vXXX_by_Mark_Lord, -COLOR3);
			return NEED_REFRESH;
		}
		if (jiffies_since(game_ball_last_moved) < (HZ*3))
			return NO_REFRESH;
		ir_selected = 1; // return to menu
		return NEED_REFRESH;
	}
	if (jiffies_since(game_animtime) < (HZ/(ANIMATION_FPS-2))) {
		(void)draw_string(ROWCOL(2,44), "You Win!", COLOR3);
		return NEED_REFRESH;
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
	s = (unsigned char *)hijack_game_animptr + hijack_game_animptr[framenr];
	d = (unsigned char *)hijack_displaybuf;
	for(a=0;a<2048;a+=2) {
		*d++=((*s&0xc0)>>2)|((*s&0x30)>>4);
		*d++=((*s&0x0c)<<2)|((*s&0x03));
		s++;
	}
	framenr += frameadj;
	game_animtime = jiffies ? jiffies : -1;
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
		ir_left_down = jiffies ? jiffies : -1;
		game_move(-1);
	} else if (ir_right_down && jiffies_since(ir_right_down) >= (HZ/15)) {
		ir_right_down = jiffies ? jiffies : -1;
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
	game_over = 0;
	game_speed = 16;
	game_animtime = 0;
	return NEED_REFRESH;
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
	else if (!hijack_last_moved)
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

#define CALCULATOR_BUTTONS_SIZE	(1 + (13 * 4))
static const unsigned int calculator_buttons[CALCULATOR_BUTTONS_SIZE] = {CALCULATOR_BUTTONS_SIZE,
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

static const unsigned char calculator_operators[] = {'+','-','*','/','%','='};

static long
calculator_do_op (long total, long value, long operator)
{
	switch (calculator_operators[operator]) {
		case '+': total += value; break;
		case '-': total -= value; break;
		case '*': total *= value; break;
		case '/': total  = value ? total / value : 0; break;
		case '%': total  = value ? total % value : 0; break;
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
	unsigned char opstring[3] = {' ','\0'};
	hijack_buttondata_t data;

	if (firsttime) {
		total = value = operator = prev = 0;
		hijack_buttonlist = calculator_buttons;;
		hijack_initq(&hijack_userq);
	} else while (hijack_button_deq(&hijack_userq, &data, 0)) {
		for (i = CALCULATOR_BUTTONS_SIZE-1; i > 0; --i) {
			if (data.button == calculator_buttons[i])
				break;
		}
		if (i & 1) { // very clever:  if (first_or_third_column_from_table) {
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
	clear_hijack_displaybuf(COLOR0);
	(void) draw_string(ROWCOL(0,0), "Menu/CD: +-*/%=", PROMPTCOLOR);
	(void) draw_string(ROWCOL(1,0), "Cancel/*: CE,CA,Quit", PROMPTCOLOR);
	(void) draw_number(ROWCOL(2,8), total, "%11d ", ENTRYCOLOR);
	opstring[0] = calculator_operators[operator];
	(void) draw_string(ROWCOL(3,0), opstring, ENTRYCOLOR);
	(void) draw_number(ROWCOL(3,8), value, "%11d ", ENTRYCOLOR);
	return NEED_REFRESH;
}

static void
do_reboot (struct display_dev *dev)
{
	unsigned long wait = HZ;

	if (hijack_reboot++ == 1) {
		clear_hijack_displaybuf(COLOR0);
		(void) draw_string(ROWCOL(2,32), "Rebooting..", PROMPTCOLOR);
		display_blat(dev, (char *)hijack_displaybuf);
		hijack_last_moved = jiffies;
		state_cleanse();	// Ensure flash savearea is updated
		if (remount_drives(0))
			wait = 0;	// no need for a delay if remount did nothing
	}
	if (jiffies_since(hijack_last_moved) >= HZ) {
		unsigned long flags;
		state_cleanse();	// Ensure flash savearea is updated
		save_flags_clif(flags);	// clif is necessary for machine_restart
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
		hijack_initq(&hijack_userq);
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
		hijack_initq(&hijack_userq);
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
	if (firsttime || prev[0] != IR_NULL_BUTTON) {
		unsigned long rowcol;
		clear_hijack_displaybuf(COLOR0);
		rowcol=draw_string(ROWCOL(0,0), showbutton_menu_label, PROMPTCOLOR);
		rowcol += (6<<16);
		(void) draw_number(rowcol, counter, " %02d ", ENTRYCOLOR);
		(void) draw_string(ROWCOL(1,0), "Repeat any button to exit", PROMPTCOLOR);
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
	{"Break-Out Game",		game_display,		game_move,		0},
	{ showbutton_menu_label,	showbutton_display,	NULL,			0},
	{ buttonled_menu_label,		buttonled_display,	buttonled_move,		0},
	{"Calculator",			calculator_display,	NULL,			0},
	{ timeraction_menu_label,	timeraction_display,	timeraction_move,	0},
	{ timer_menu_label,		timer_display,		timer_move,		0},
	{ fsck_menu_label,		fsck_display,		fsck_move,		0},
	{"Font Display",		kfont_display,		NULL,			0},
	{ forcepower_menu_label,	forcepower_display,	forcepower_move,	0},
	{ onedrive_menu_label,		onedrive_display,	onedrive_move,		0},
	{ hightemp_menu_label,		hightemp_display,	hightemp_move,		0},
	{ homework_menu_label,		homework_display,	homework_move,		0},
#ifdef EMPEG_KNOB_SUPPORTED
	{ knobdata_menu_label,		knobdata_display,	knobdata_move,		0},
#endif // EMPEG_KNOB_SUPPORTED
	{ delaytime_menu_label,		delaytime_display,	delaytime_move,		0},
	{"Reboot Machine",		reboot_display,		NULL,			0},
	{ carvisuals_menu_label,	carvisuals_display,	carvisuals_move,	0},
	{ homevisuals_menu_label,	homevisuals_display,	homevisuals_move,	0},
	{ blankerfuzz_menu_label,	blankerfuzz_display,	blankerfuzz_move,	0},
	{ blanker_menu_label,		blanker_display,	blanker_move,		0},
	{"Show Flash Savearea",		savearea_display,	savearea_move,		0},
	{ bass_menu_label,		bass_display,		tone_move,		0},
	{ treble_menu_label,		treble_display,		tone_move,		0},
	{"Vital Signs",			vitals_display,		NULL,			0},
#ifdef DEBUG_JIFFIES
	{"Vitals v2beta11",		debug_display,		NULL,			0},
#endif // DEBUG_JIFFIES
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
		hijack_last_moved = jiffies ? jiffies : -1;
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
		activate_dispfunc(item->dispfunc, item->movefunc);
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
		hijack_last_moved = jiffies ? jiffies : -1;
	}
}

static void
hijack_move_repeat (void)
{
	if (ir_left_down && jiffies_since(ir_left_down) >= ir_move_repeat_delay) {
		ir_left_down = jiffies ? jiffies : -1;
		hijack_move(-1);
		ir_lasttime = jiffies;
	} else if (ir_right_down && jiffies_since(ir_right_down) >= ir_move_repeat_delay) {
		ir_right_down = jiffies ? jiffies : -1;
		hijack_move(+1);
		ir_lasttime = jiffies;
	}
}

#define TESTCOL		12
#define ROWOFFSET(row)	((row)*(EMPEG_SCREEN_COLS/2))
#define TESTOFFSET(row)	(ROWOFFSET(row)+TESTCOL)

static int
test_row (const void *rowaddr, unsigned long color)
{
	const unsigned long *first = rowaddr;
	const unsigned long *test  = first + (((EMPEG_SCREEN_COLS/2)-(TESTCOL*2))/sizeof(long));
	do {
		if (*--test != color)
			return 0;
	} while (test > first);
	return 1;
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
	return (test_row(buf+TESTOFFSET( 8), 0x00000000) && test_row(buf+TESTOFFSET( 9), 0x11111111)
		&& test_row(buf+TESTOFFSET(16), 0x11111111) && test_row(buf+TESTOFFSET(17), 0x00000000));
}

static inline int
check_if_search_is_active (const unsigned char *buf)
{
	return ((test_row(buf+TESTOFFSET(4), 0x00000000) && test_row(buf+TESTOFFSET(5), 0x11111111))
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

static int
player_ui_is_active (const unsigned char *buf)
{
	// Use screen-scraping to see if the player user-interface is active:
	return	(check_if_menu_is_active(buf)   || check_if_soundadj_is_active(buf) ||
		 check_if_search_is_active(buf) || check_if_equalizer_is_active(buf));
}

static unsigned short
get_current_mixer_source (void)
{
	unsigned short source;
	extern int hijack_current_mixer_input;
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

static void
do_nextsrc (void)
{
	unsigned int button;
	unsigned long flags;

	// main -> tuner -> aux -> main
	save_flags_cli(flags);
	button = IR_RIO_SOURCE_PRESSED;
	switch (get_current_mixer_source()) {
		case IR_FLAGS_TUNER:
			if (kenwood_disabled)
				hijack_enq_button_pair(IR_RIO_SOURCE_PRESSED);
			else
				button = IR_KW_TAPE_PRESSED;
			break;
		case IR_FLAGS_MAIN:
			if (empeg_tuner_present || hijack_fake_tuner)
				button = IR_RIO_TUNER_PRESSED;
			break;
	}
	restore_flags(flags);
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
		hijack_enq_button_pair(IR_RIO_VOLPLUS_PRESSED);
		//hijack_enq_button_pair(IR_RIO_VOLMINUS_PRESSED);
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

static int
message_display (int firsttime)
{
	static const hijack_geom_t geom = {8, 8+6+KFONT_HEIGHT, 2, EMPEG_SCREEN_COLS-2};
	unsigned int rowcol;

	if (firsttime) {
		create_overlay(&geom);
		rowcol = (geom.first_row+4)|((geom.first_col+6)<<16);
		rowcol = draw_string(rowcol, message_text, COLOR3);
		hijack_last_moved = jiffies ? jiffies : -1;
		blanker_triggered = 0;
	} else if (jiffies_since(hijack_last_moved) >= message_time) {
		hijack_deactivate(HIJACK_IDLE);
	}
	return NO_REFRESH;	// gets overridden if overlay still active
}

void
show_message (const char *message, unsigned long time)
{
	strncpy(message_text, message, sizeof(message_text)-1);
	message_text[sizeof(message_text)-1] = '\0';
	message_time = time;
	if (message && *message) {
		unsigned long flags;
		save_flags_cli(flags);
		activate_dispfunc(message_display, NULL);
		restore_flags(flags);
	}
}

static int
quicktimer_display (int firsttime)
{
	static const unsigned int quicktimer_buttonlist[] = {5, IR_KW_4_PRESSED, IR_KW_4_RELEASED, IR_RIO_4_PRESSED, IR_RIO_4_RELEASED};
	static const hijack_geom_t geom = {8, 8+6+KFONT_HEIGHT, 12, EMPEG_SCREEN_COLS-12};
	hijack_buttondata_t data;
	unsigned int rowcol;

	timer_started = jiffies;
	if (firsttime) {
		if (timer_timeout) {
			timer_timeout = 0;
			hijack_beep(60, 100, 30);
		} else {
			timer_timeout = hijack_quicktimer_minutes * (60*HZ);
			hijack_beep(80, 100, 30);
		}
		ir_numeric_input = &timer_timeout;
		hijack_buttonlist = quicktimer_buttonlist;
		hijack_last_moved = jiffies ? jiffies : -1;
		create_overlay(&geom);
	} else if (jiffies_since(hijack_last_moved) >= (HZ*3)) {
		hijack_deactivate(HIJACK_IDLE);
	} else {
		while (hijack_button_deq(&hijack_userq, &data, 0)) {
			if (!(data.button & 0x80000000))
				timer_timeout += hijack_quicktimer_minutes * (60*HZ);
			hijack_last_moved = jiffies ? jiffies : -1;
		}
		rowcol = (geom.first_row+4)|((geom.first_col+6)<<16);
		rowcol = draw_string(rowcol, "Quick Timer: ", COLOR3);
		clear_text_row(rowcol, geom.last_col-4, 1);
		rowcol = draw_hhmmss(rowcol, timer_timeout / (60*HZ), ENTRYCOLOR);
	}
	return NO_REFRESH;	// gets overridden if overlay still active
}

// This routine gets first shot at IR codes as soon as they leave the translator.
//
// In an ideal world, we would never use "jiffies" here, relying on the inter-code "delay" instead.
// But.. maybe later.  The timings are coarse enough that it shouldn't matter much.
//
// Note that ALL front-panel buttons send codes ONCE on press, but twice on RELEASE.
//
static void
hijack_handle_button (unsigned int button, unsigned long delay, int any_ui_is_active)
{
	static unsigned int ir_lastpressed = IR_NULL_BUTTON;
	unsigned long old_releasewait;
	int hijacked = 0;

	//printk("HB: %08lx.%ld,ui=%d\n", button, delay, any_ui_is_active);
	// filter out buttons that rely on UI or NONUI states
	if ((button & BUTTON_FLAGS_UISTATE)) {
		if (!(button & (any_ui_is_active ? BUTTON_FLAGS_UI : BUTTON_FLAGS_NOTUI)))
			return;		// this button doesn't exist in this state
	}
	if (button & BUTTON_FLAGS_SHIFT)
		ir_shifted = !ir_shifted;
	button &= ~(BUTTON_FLAGS_UISTATE|BUTTON_FLAGS_SHIFT);

	blanker_triggered = 0;
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
			activate_dispfunc(menu_display, menu_move);
			hijacked = 1;
			break;
		case IR_FAKE_BASSADJ:
			activate_dispfunc(bass_display, tone_move);
			hijacked = 1;
			break;
		case IR_FAKE_TREBLEADJ:
			activate_dispfunc(treble_display, tone_move);
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
			activate_dispfunc(voladj_prefix_display, voladj_move);
			hijacked = 1;
			break;
		case IR_FAKE_NEXTSRC:
			do_nextsrc();
			hijacked = 1;
			break;
		case IR_FAKE_POPUP0:
		case IR_FAKE_POPUP1:
		case IR_FAKE_POPUP2:
		case IR_FAKE_POPUP3:
			popup_activate(button);
			hijacked = 1;
			break;
#ifdef EMPEG_KNOB_SUPPORTED
		case IR_KNOB_PRESSED:
			hijacked = 1; // hijack it and later send it with the release
			ir_knob_busy = 0;
			ir_knob_down = jiffies ? jiffies : -1;
			if (hijack_status == HIJACK_ACTIVE)
				hijacked = ir_selected = 1;
			break;
		case IR_KNOB_RELEASED:
			if (ir_knob_down) {
				if (ir_knob_busy) {
					ir_knob_busy = 0;
				} else if (hijack_status == HIJACK_IDLE) {
					int index = knobdata_index;
					if (any_ui_is_active)
						index = 0;
					hijacked = 1;
					if (jiffies_since(ir_knob_down) < (HZ/2)) {	// short press?
						if (index == 1)
							popup_activate(knobdata_buttons[1]);
						else if (index == 2)
							activate_dispfunc(voladj_prefix_display, voladj_move);
						else
							hijack_enq_button_pair(knobdata_buttons[index]);
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
		case IR_FAKE_CLOCK:
		{
			tm_t	tm;
			char	buf[24];
			const char *wdays[7] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
			extern const char *hijack_months[12];
			hijack_convert_time(CURRENT_TIME + (hijack_time_offset * 60), &tm);
			sprintf(buf, "%3s %02u:%02u %02u-%3s-%4u", wdays[tm.tm_wday], tm.tm_hour, tm.tm_min,
				tm.tm_mday, hijack_months[tm.tm_mon], tm.tm_year);
			show_message(buf, 4*HZ);
			hijacked = 1;
			break;
		}
		case IR_FAKE_KNOBSEEK:
			activate_dispfunc(knobseek_display, knobseek_move);
			hijacked = 1;
			break;
		case IR_KNOB_RIGHT:
			if (hijack_status != HIJACK_IDLE) {
				hijack_move(1);
				hijacked = 1;
			}
			break;
		case IR_KNOB_LEFT:
			if (hijack_status != HIJACK_IDLE) {
				hijack_move(-1);
				hijacked = 1;
			}
			break;
#endif // EMPEG_KNOB_SUPPORTED
		case IR_RIO_MENU_PRESSED:
			if (!any_ui_is_active) {
				hijacked = 1; // hijack it and later send it with the release
				ir_menu_down = jiffies ? jiffies : -1;
			}
			if (hijack_status == HIJACK_ACTIVE)
				hijacked = ir_selected = 1;
			break;
		case IR_RIO_MENU_RELEASED:
			if (hijack_status != HIJACK_IDLE) {
				hijacked = 1;
			} else if (ir_menu_down && !any_ui_is_active) {
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
				if ((ir_lastpressed & ~0x80000000) != button || delay > HZ)
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
			ir_right_down = jiffies ? jiffies : -1;
			if (hijack_status != HIJACK_IDLE) {
				hijack_move(1);
				hijacked = 1;
			}
			break;
		case IR_KW_PREVTRACK_PRESSED:
		case IR_RIO_PREVTRACK_PRESSED:
			ir_move_repeat_delay = (hijack_movefunc == game_move) ? (HZ/15) : (HZ/3);
			ir_left_down = jiffies ? jiffies : -1;
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
		case IR_RIO_4_PRESSED:
			if (!any_ui_is_active) {
				hijacked = 1; // hijack it and later send it with the release
				ir_4_down = jiffies ? jiffies : -1;
			}
			break;
		case IR_RIO_4_RELEASED:
			if (hijack_status != HIJACK_IDLE) {
				hijacked = 1;
			} else if (!any_ui_is_active && ir_4_down) {
				if (get_current_mixer_source() != IR_FLAGS_TUNER) {
					hijacked = 1;
					activate_dispfunc(quicktimer_display, timer_move);
				} else {
					hijack_enq_button(&hijack_playerq, IR_RIO_4_PRESSED, 0);
					ir_releasewait = IR_NULL_BUTTON;
				}
			}
			ir_4_down = 0;
			break;
		case IR_KW_4_PRESSED:
		case IR_KW_4_RELEASED:
			if (!any_ui_is_active && get_current_mixer_source() != IR_FLAGS_TUNER) {
				hijacked = 1;
				if (button == IR_KW_4_RELEASED)
					activate_dispfunc(quicktimer_display, timer_move);
			}
			break;
	}
done:
	ir_lastpressed = button; // used for detection of CD-CD-CD sequence

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
inject_stalk_button (unsigned int button)
{
	unsigned char pkt[4] = {0x02, 0x00, 0xff, 0xff};
	min_max_t *vals, *v;
	int i;
	unsigned int rawbutton = button;	//fixme

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
		vals = stalk_on_left ? lhs_stalk_vals : rhs_stalk_vals;
		v = &vals[i];
		pkt[2] = (v->min + v->max) / 2;
	}
	pkt[3] = pkt[1] + pkt[2];
	if (hijack_stalk_debug)
		printk("Stalk: out=%02x %02x %02x %02x == %08x\n", pkt[0], pkt[1], pkt[2], pkt[3], rawbutton); //fixme
	hijack_serial_insert(pkt, sizeof(pkt), 0);
}

static void
hijack_handle_buttons (const char *player_buf)
{
	hijack_buttondata_t	data;
	unsigned long		flags;
	int			any_ui_is_active = 1;

	if (hijack_status == HIJACK_IDLE)
		any_ui_is_active = player_ui_is_active(player_buf);
	while (hijack_button_deq(&hijack_inputq, &data, 1)) {
		save_flags_cli(flags);
		hijack_handle_button(data.button, data.delay, any_ui_is_active);
		restore_flags(flags);
	}
	while (hijack_button_deq(&hijack_playerq, &data, 0)) {
		unsigned int button = data.button;
		if ((button & IR_STALK_MASK) == IR_STALK_MATCH)
			inject_stalk_button(button);
		else
			(void)real_input_append_code(data.button);
	}
}

static unsigned int ir_downkey = IR_NULL_BUTTON, ir_delayed_rotate = 0;

static void
input_append_code2 (unsigned int rawbutton)
{
	unsigned int released = IS_RELEASE(rawbutton);

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
		if (rawbutton != IR_KNOB_LEFT && rawbutton != IR_KNOB_RIGHT) {
			if (rawbutton > IR_NULL_BUTTON || rawbutton < IR_FAKE_HIJACKMENU)
				ir_downkey = rawbutton;
		}
	}
	if (ir_translate_table != NULL) {
		unsigned short	mixer		= get_current_mixer_source();
		unsigned short	carhome		= empeg_on_dc_power ? IR_FLAGS_CAR : IR_FLAGS_HOME;
		unsigned short	shifted		= ir_shifted ? IR_FLAGS_SHIFTED : IR_FLAGS_NOTSHIFTED;
		unsigned short	flags		= mixer | carhome | shifted;
		int		delayed_send	= 0;
		int		was_waiting	= (ir_current_longpress != NULL);
		ir_translation_t *t		= NULL;
		unsigned int	button;

		button = PRESSCODE(rawbutton);
		ir_current_longpress = NULL;
		while (NULL != (t = ir_next_match(t, button))) {
			unsigned short t_flags = t->flags;
			if (t_flags & IR_FLAGS_POPUP)
				break;	// no translations here for PopUp's
			if (hijack_ir_debug)
				printk("%lu: IA2: tflags=%02x, flags=%02x, match=%d\n", jiffies, t_flags, flags, (t_flags & flags) == t_flags);
			if ((t_flags & flags) == flags) {
				if (released) {	// button release?
					if ((t_flags & IR_FLAGS_LONGPRESS) && was_waiting) {
						// We were timing for a possible longpress,
						//  but now know that it wasn't held down long enough.
						// Instead, we just let go of a shortpress for which
						//   the original "press" code has been sent yet
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
				ir_lasttime = ir_lastevent = jiffies;
				return;
			}
		}
		if (delayed_send) {
			// we just released a non-translated shortpress, but haven't sent the "press" yet
			hijack_enq_button(&hijack_inputq, button, 0);
		}
	}
	hijack_enq_button(&hijack_inputq, rawbutton, 0);
	ir_lasttime = ir_lastevent = jiffies;
}

static void
input_send_delayed_rotate (void)
{
	input_append_code2(ir_delayed_rotate);
	ir_lasttime = ir_lastevent = jiffies;
	ir_delayed_rotate = 0;
}

void  // invoked from multiple places (time-sensitive code) in empeg_input.c
input_append_code(void *dev, unsigned int button)  // empeg_input.c
{
	unsigned long flags;

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
		ir_lasttime = ir_lastevent = jiffies;
	}
	restore_flags(flags);
}

static int
handle_stalk_packet (unsigned char *pkt)
{
	static unsigned int current_button = -1;
	unsigned char val;
	unsigned int button;


	// Check for valid stalk packet, and convert into a button press/release code:
	if (pkt[0] != 0x02 || (pkt[1] | 1) != 0x01 || (pkt[1] + pkt[2]) != pkt[3])
		return 0;	// not a valid Stalk packet

	if (current_button != -1) {
		button = current_button | 0x80000000;
		current_button = -1;
		input_append_code(NULL, button);
		if (hijack_stalk_debug)
			printk("Stalk: in=%02x %02x %02x %02x == %08x\n", pkt[0], pkt[1], pkt[2], pkt[3], button); //fixme
	}
	val = pkt[2];
	if (val == 0xff) {	// button pressed?
		if (hijack_stalk_debug)
			printk("Stalk: in=%02x %02x %02x %02x == no button\n", pkt[0], pkt[1], pkt[2], pkt[3]); //fixme
	} else {
		min_max_t *v, *vals;
		int i;
		vals = stalk_on_left ? lhs_stalk_vals : rhs_stalk_vals;
		for (i = 0; (v = &vals[i])->min != -1; ++i) {
			if (val >= v->min && val <= v->max) {
				button = stalk_buttons[i];
				if (pkt[1]) // shifted?
					button |= IR_STALK_SHIFTED;
				current_button = button;
				input_append_code(NULL, button);
				if (hijack_stalk_debug)
					printk("Stalk: in=%02x %02x %02x %02x == %08x\n", pkt[0], pkt[1], pkt[2], pkt[3], button); //fixme
				goto done;
			}
		}
		if (hijack_stalk_debug)
			printk("Stalk: in=%02x %02x %02x %02x == no match\n", pkt[0], pkt[1], pkt[2], pkt[3]); //fixme
		else
			printk("Stalk: no match for value=0x%02x\n", val);
	}
done:
	return 1;	// Stalk packet
}

void
hijack_intercept_tuner (unsigned int packet)
{
	empeg_tuner_present = 1;
	if (hijack_trace_tuner)
		printk("tuner: in=%08x\n", htonl(packet));
	if (hijack_fake_tuner) {
		hijack_fake_tuner = 0;
		printk("tuner: \"fake_tuner=0\"\n");
	} else if (!handle_stalk_packet((unsigned char *)&packet)) {
		hijack_serial_insert((unsigned char *)&packet, sizeof(packet), 0);
	}
}

static int
look_for_trackinfo_or_tuner (unsigned char *buf, int row)
{
	if (row) {
		// look for a solid line, any color(s)
		unsigned long *screen = (unsigned long *)(&buf[row * (EMPEG_SCREEN_COLS / 2)]);
		unsigned long test = (*(unsigned long *)screen) & 0x33333333;
		return (((test | (test << 1)) & 0x22222222) == 0x22222222);
	} else {
		// look for the distinctive Tuner "chickenfoot" character
		static const unsigned long chickenfoot[] =
			{0x000f0000, 0xf00f00f0, 0x0f0f0f00, 0x00fff000, 0x000f0000, 0x000f0000, 0x000f0000};
		const unsigned long *foot = &chickenfoot[0];
		for (row = 0; row < (sizeof(chickenfoot) / sizeof(unsigned long)); ++row) {
			unsigned long *screen = (unsigned long *)(&buf[(row + 15) * (EMPEG_SCREEN_COLS / 2)]);
			if (*screen != *foot++)
				return 0;
		}
		return 1;
	}
}

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

// This routine covertly intercepts all display updates,
// giving us a chance to substitute our own display.
//
void	// invoked from empeg_display.c
hijack_handle_display (struct display_dev *dev, unsigned char *player_buf)
{
	static int sent_initial_buttons = 0;
	unsigned char *buf = player_buf;
	unsigned long flags;
	int refresh = NEED_REFRESH;

	if (hijack_reboot) {
		do_reboot(dev);
		return;
	}
#ifdef DEBUG_JIFFIES
	// fixme: lockup detection logic
	static unsigned long oldjiffies = 0, oldcount = 0;
	if (jiffies == oldjiffies) {
		if (++oldcount >= 5) {
			activate_dispfunc(debug_display, NULL);
			preselect_menu_item(timer_display);
		}
	} else {
		if (oldcount >= 5) {
			char buf[32];
			sprintf(buf, "unstuck jiffies %lu", oldcount);
			show_message(buf, 20*60*HZ);
		}
		oldjiffies = jiffies;
		oldcount = 0;
	}
#endif // DEBUG_JIFFIES

	// Send initial button sequences, if any
	if (!sent_initial_buttons && hijack_player_started) {
		sent_initial_buttons = 1;
		input_append_code(IR_INTERNAL, IR_FAKE_INITIAL);
		input_append_code(IR_INTERNAL, RELEASECODE(IR_FAKE_INITIAL));
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

	// Keep rough track of how long the power has been off
	if (dev->power)
		hijack_standby_time = 0;
	else if (!hijack_standby_time)
		hijack_standby_time = jiffies ? jiffies : -1;
	else if (jiffies_since(hijack_standby_time) >= (60*60*13))
		hijack_standby_time = jiffies - (60*60);	// prevent wraparound from eventually screwing us

	// Manage buttonLED illumination level
	if (!hijack_standby_time || jiffies_since(hijack_standby_time) > (HZ/4))
		hijack_adjust_buttonled(dev->power);

	save_flags_cli(flags);
	if (!dev->power) {  // do (almost) nothing if unit is in standby mode
		hijack_deactivate(HIJACK_IDLE);
		(void)timer_check_expiry(dev);
		restore_flags(flags);
		check_screen_grab(player_buf);
		display_blat(dev, player_buf);
		return;
	}
	if (restore_visuals && hijack_got_config_ini) {
		if (look_for_trackinfo_or_tuner(player_buf, info_screenrow)) {
			while (restore_visuals) {
				--restore_visuals;
				hijack_enq_button_pair(IR_RIO_INFO_PRESSED);
			}
		}
	}

#ifdef EMPEG_KNOB_SUPPORTED
	if (ir_knob_down && jiffies_since(ir_knob_down) > (HZ*2)) {
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
		blanker_triggered = 0;
	}
	switch (hijack_status) {
		case HIJACK_IDLE:
			if (ir_trigger_count >= 3
#ifdef EMPEG_KNOB_SUPPORTED
			 || (!ir_knob_busy && ir_knob_down && jiffies_since(ir_knob_down) >= HZ)
#endif // EMPEG_KNOB_SUPPORTED
			 || (ir_menu_down && jiffies_since(ir_menu_down) >= HZ)) {
				activate_dispfunc(menu_display, menu_move);
			} else if (ir_4_down && jiffies_since(ir_4_down) >= (3*HZ/2)) {
				activate_dispfunc(quicktimer_display, timer_move);
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
				if (ir_selected && !hijack_overlay_geom) {
					if (hijack_dispfunc != userland_display) {
						if (hijack_dispfunc == menu_display) {
							menu_item_t *item = &menu_table[menu_item];
							activate_dispfunc(item->dispfunc, item->movefunc);
						} else if (hijack_dispfunc == forcepower_display || hijack_dispfunc == homework_display) {
							activate_dispfunc(reboot_display, NULL);
						} else {
							activate_dispfunc(menu_display, menu_move);
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
		blanker_triggered = 0;
	} else if (blanker_timeout) {
		if (jiffies_since(blanker_lastpoll) >= (4*HZ/3)) {  // use an oddball interval to avoid patterns
			int is_paused = 0;
			if (get_current_mixer_source() == IR_FLAGS_MAIN && ((*empeg_state_writebuf)[0x0c] & 0x02) == 0)
				is_paused = 1;
			blanker_lastpoll = jiffies;
			if (!is_paused && screen_compare((unsigned long *)blanker_lastbuf, (unsigned long *)buf)) {
				memcpy(blanker_lastbuf, buf, EMPEG_SCREEN_BYTES);
				blanker_triggered = 0;
			} else if (!blanker_triggered) {
				blanker_triggered = jiffies ? jiffies : -1;
			}
		}
		if (blanker_triggered) {
			unsigned long minimum = blanker_timeout * (SCREEN_BLANKER_MULTIPLIER * HZ);
			if (jiffies_since(blanker_triggered) > minimum) {
				buf = player_buf;
				memset(buf, 0, EMPEG_SCREEN_BYTES);
				refresh = NEED_REFRESH;
				if (get_current_mixer_source() == IR_FLAGS_MAIN && hijack_standby_minutes > 0) {
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
	if (refresh == NEED_REFRESH) {
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
			printline("[ir_translate] ERROR: ", line);
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
			hijack_initq(&hijack_userq);
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
	int i, rc = 0;
	if (opt->num_items == 0) {
		unsigned char *t;
		t = findchars(*s, "\n;\r");
		if ((t - *s) > opt->max)
			t = *s + opt->max;
		if (!syntax_only) {
			char c = *t;
			*t = '\0';
			strcpy(opt->target, *s);
			*t = c;
		}
		rc = 1;	// success
	} else {
		for (i = 0; i < opt->num_items; ++i) {
			int val;
			if (!get_number(s, &val, 10, " ,;\t\n") || val < opt->min || val > opt->max)
				break;	// failure
			if (!syntax_only && opt->num_items)
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
hijack_get_options (unsigned char *buf)
{
	static const char menu_delete[] = "menu_remove=";
	int errors;
	unsigned char *s;

	// look for [kenwood] disabled=0
	if ((s = find_header(buf, "[kenwood]"))) {
		while (*(s = skipchars(s, " \n\t\r")) && *s != '[') {
			if (!strxcmp(s, "disabled=1", 1)) {
				kenwood_disabled = 1;
				break;
			}
			s = findchars(s, "\n");
		}
	}

	// look for [controls] stalk_side=left
	if ((s = find_header(buf, "[controls]"))) {
		while (*(s = skipchars(s, " \n\t\r")) && *s != '[') {
			if (!strxcmp(s, "stalk_side=left", 1)) {
				stalk_on_left = 1;
				break;
			}
			s = findchars(s, "\n");
		}
	}

	// look for [hijack] options:
	if (!(s = find_header(buf, "[hijack]")))
		return 0;
	errors = 0;
	while (*(s = skipchars(s, " \n\t\r")) && *s != '[') {
		char *line = s;
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
			printline("[hijack] ERROR: ", line);
			errors = 1;
		}
	nextline:
		s = findchars(s, "\n");
	}
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
			if (optname != s) {		// verify whitespace after "lookfor"
				s = optname;
				*(s - 1) = '\n';	// "uncomment" the portion after "lookfor"
				++count;		// keep track of how many substitutions we do
				optend = findchars(s, "=\r\n");
				if (*optend == '=')
					++optend;
				s = findchars(optend, "\r\n");
				if (*s) {		// search for old copies of same command, and nuke'em
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
	const hijack_option_t *h = hijack_option_table;
	kenwood_disabled = 0;
	stalk_on_left = 0;
	lhs_stalk_vals[10] = (min_max_t){-1,-1};	// mark end of table (not part of "options").
	rhs_stalk_vals[10] = (min_max_t){-1,-1};	// mark end of table (not part of "options").
	while (h->name) {
		int n = h->num_items, *val = h->target;
		if (n == 1) {
			*val = h->defaultval;
		} else if (n) {
			int *def = (int *)h->defaultval;
			while (n--)
				*val++ = *def++;
		} else {
			strcpy(h->target, (char *)h->defaultval);
		}
		++h;
	}
}

// invoked from fs/read_write.c on each read of config.ini at each player start-up.
// This could be invoked multiple times if file is too large for a single read,
// so we use the f_pos parameter to ensure we only do setup stuff once.
void
hijack_process_config_ini (char *buf, int invocation_count)
{
	static const char *acdc_labels[2] = {";@AC", ";@DC"};

	hijack_got_config_ini = 1;
	(void) edit_config_ini(buf, acdc_labels[empeg_on_dc_power]);
	(void) edit_config_ini(buf, homework_labels[ hijack_homework]);
	if (invocation_count != 1)		// exit if not first read of this cycle
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
	}
	up(&hijack_kftpd_startup_sem);	// wake-up kftpd now that we've parsed config.ini for port numbers
	up(&hijack_khttpd_startup_sem);	// wake-up kftpd now that we've parsed config.ini for port numbers
#endif // CONFIG_NET_ETHERNET
	set_drive_spindown_times();
}

// fixme: temporary work-around for v2.00-beta11 bug
static void
hijack_fix_homevisuals (unsigned char *buf)
{
	if (!empeg_on_dc_power && homevisuals_enabled) {
		switch ((buf[0x0e] & 7) - 1) { // examine the saved mixer source
			case INPUT_RADIO_FM: // Tuner FM
				if ((buf[0x4c] & 0x10) && !(buf[0x42] & 3)) {	// Radio visual supposed to be active?
					info_screenrow = 8;
					restore_visuals = 1;			// comes up as OFF: restore it
				}
				break;
			case INPUT_PCM: // Main/Mp3
				if ((buf[0x4c] & 0x04)) {			// Track, Seek, or Now&Next ?
					info_screenrow = 8;
					restore_visuals = (buf[0x4d] & 7) - 2; // comes up as TRANSIENT: restore it
				}
				break;
			case INPUT_AUX: // Aux
				if ((buf[0x4c] & 0x08) && (buf[0x41] & 3) == 2) {	// Aux visual supposed to be active?
					info_screenrow = 8;
					restore_visuals = 1;			// comes up as TRANSIENT: restore it
				}
				break;
			case INPUT_RADIO_AM: // Tuner AM
				if ((buf[0x4c] & 0x20) && !(buf[0x43] & 3)) {	// Radio visual supposed to be active?
					info_screenrow = 8;
					restore_visuals = 1;			// comes up as OFF: restore it
				}
				break;
		}
	}
}

void
hijack_fix_visuals (unsigned char *buf)
{
	restore_visuals = 0;
	hijack_fix_homevisuals(buf);	// fixme: temporary work-around for v2.00-beta11 bug
	if (empeg_on_dc_power && carvisuals_enabled) {
		switch ((buf[0x0e] & 7) - 1) { // examine the saved mixer source
			case INPUT_RADIO_FM: // Tuner FM
				if ((buf[0x4c] & 0x10) == 0) {	// FM visuals visible ?
					info_screenrow = 0;	// "chickenfoot"
					restore_visuals = (buf[0x42] - 1) & 3;
					buf[0x4c] = buf[0x4c] |  0x10;
					buf[0x42] = buf[0x42] & ~0x03;
				}
				break;
			case INPUT_PCM: // Main/Mp3
				if ((buf[0x4c] & 0x04) == 0) {	// MP3 visuals visible ?
					info_screenrow = 15;
					restore_visuals = (buf[0x40] & 3) + 2 + (buf[0x4d] & 1);
					//printk("1restore=%d, x40=%02x, x4c=%02x, x4d=%02x\n", restore_visuals, buf[0x40], buf[0x4c], buf[0x4d]);
					// switch to "track" mode on startup (because it's easy to detect later on),
					// and then restore the original mode when the track info appears in the screen buffer.
					buf[0x40] = (buf[0x40] & ~0x03) | 0x02;
					buf[0x4c] =  buf[0x4c]          | 0x04;
					buf[0x4d] = (buf[0x4d] & ~0x07) | 0x03;
					//printk("2restore=%d, x40=%02x, x4c=%02x, x4d=%02x\n", restore_visuals, buf[0x40], buf[0x4c], buf[0x4d]);
				}
				break;
			case INPUT_AUX: // Aux
				if ((buf[0x4c] & 0x08) == 0) {	// AUX visuals visible ?
					info_screenrow = 8;
					restore_visuals = (buf[0x41] & 3) + 1;
					buf[0x41] = (buf[0x41] & ~0x03) | 0x02;
					buf[0x4c] =  buf[0x4c]          | 0x08;
				}
				break;
			case INPUT_RADIO_AM: // Tuner AM
				if ((buf[0x4c] & 0x20) == 0) {	// AM visuals visible ?
					info_screenrow = 0;	// "chickenfoot"
					restore_visuals = (buf[0x43] - 1) & 3;
					buf[0x43] = buf[0x43] & ~0x03;
					buf[0x4c] = buf[0x4c] |  0x20;
				}
				break;
		}
		if (restore_visuals)
			empeg_state_dirty = 1;
	}
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
	signed 	 delaytime		: 8;			// 8 bits

	unsigned knob			: 1+KNOBDATA_BITS;	// 4 bits
	unsigned buttonled_level	: BUTTONLED_BITS;	// 3 bits
	unsigned spare1			: 1;			// 1 bit

	unsigned voladj			: VOLADJ_BITS;		// 2 bits
	unsigned spare6			: 6;			// 6 bits

	unsigned bass_adj		: 4;			// 4 bits
	unsigned treble_adj		: 4;			// 4 bits
} hijack_savearea_acdc_t;

// The "master" 16-byte savearea struct, with AC/DC substructs, and common data fields:
typedef struct hijack_savearea_s {
	hijack_savearea_acdc_t ac;				// 32 bits
	hijack_savearea_acdc_t dc;				// 32 bits

	unsigned blanker_timeout	: BLANKER_BITS;		// 6 bits
	unsigned force_power		: FORCEPOWER_BITS;	// 2 bits

	unsigned blanker_sensitivity	: SENSITIVITY_BITS;	// 3 bits
	unsigned hightemp_threshold	: HIGHTEMP_BITS;	// 5 bits

	unsigned menu_item		: MENU_BITS;		// 5 bits
	unsigned restore_visuals	: 1;			// 1 bit
	unsigned fsck_disabled		: 1;			// 1 bit
	unsigned onedrive		: 1;			// 1 bit

	unsigned timer_action		: TIMERACTION_BITS;	// 1 bit
	unsigned homework		: 1;			// 1 bits
	unsigned fix_beta11		: 1;			// 1 bits
	unsigned spare5			: 5;			// 5 bits

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
	knob = (knobdata_index == 1) ? (1<<KNOBDATA_BITS) | popup0_index : knobdata_index;
	acdc->knob			= knob;
	acdc->delaytime			= hijack_delaytime;
	acdc->buttonled_level		= hijack_buttonled_on_level;
	acdc->voladj			= hijack_voladj_enabled;
	acdc->bass_adj			= hijack_bass_adj;
	acdc->treble_adj		= hijack_treble_adj;
	savearea.blanker_timeout	= blanker_timeout;
	savearea.force_power		= hijack_force_power;
	savearea.blanker_sensitivity	= blanker_sensitivity;
	savearea.hightemp_threshold	= hightemp_threshold;
	savearea.menu_item		= menu_item;
	savearea.restore_visuals	= carvisuals_enabled;
	savearea.fsck_disabled		= hijack_fsck_disabled;
	savearea.onedrive		= hijack_onedrive;
	savearea.timer_action		= timer_action;
	savearea.homework		= hijack_homework;
	savearea.fix_beta11		= homevisuals_enabled;
	savearea.layout_version		= SAVEAREA_LAYOUT;
	memcpy(buf+HIJACK_SAVEAREA_OFFSET, &savearea, sizeof(savearea));
}

static int
hijack_restore_settings (char *buf)
{
	extern int empeg_state_restore(unsigned char *);	// arch/arm/special/empeg_state.c
	hijack_savearea_acdc_t	*acdc;
	unsigned int knob, failed;

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
	hijack_force_power		= savearea.force_power;
	if (hijack_force_power & 2)
		empeg_on_dc_power	= hijack_force_power & 1;
	else
		empeg_on_dc_power	= ((GPLR & EMPEG_EXTPOWER) != 0);

	// Now that the powermode (AC/DC) is set, we can deal with everything else
	acdc = empeg_on_dc_power ? &savearea.dc : &savearea.ac;
	knob				= acdc->knob;
	hijack_delaytime		= acdc->delaytime;
	hijack_buttonled_on_level	= acdc->buttonled_level;
	hijack_voladj_enabled		= acdc->voladj;
	hijack_bass_adj			= acdc->bass_adj;
	hijack_treble_adj		= acdc->treble_adj;
	if ((knob & (1 << KNOBDATA_BITS)) == 0) {
		popup0_index		= 0;
		knobdata_index		= knob;
	} else {
		popup0_index		= knob & ((1 << KNOBDATA_BITS) - 1);
		knobdata_index		= 1;
	}

	blanker_timeout			= savearea.blanker_timeout;
	blanker_sensitivity		= savearea.blanker_sensitivity;
	hightemp_threshold		= savearea.hightemp_threshold;
	menu_item			= savearea.menu_item;
	carvisuals_enabled		= savearea.restore_visuals;
	hijack_fsck_disabled		= savearea.fsck_disabled;
	hijack_onedrive			= savearea.onedrive;
	timer_action			= savearea.timer_action;
	hijack_homework			= savearea.homework;
	homevisuals_enabled		= savearea.fix_beta11;

	return failed;
}

unsigned long	// invoked once from empeg_display.c
hijack_init (void *animptr)
{
	extern void hijack_notify_init (void);
	int failed;
	char buf[128];
	const unsigned long animstart = HZ/3;

	hijack_game_animptr = animptr;
	hijack_buttonled_level = 0;	// turn off button LEDs
	failed = hijack_restore_settings(buf);
	menu_init();
	reset_hijack_options();
	init_temperature(1);
	hijack_initq(&hijack_inputq);
	hijack_initq(&hijack_playerq);
	hijack_initq(&hijack_userq);
	hijack_notify_init();
	if (!failed)
		show_message(hijack_vXXX_by_Mark_Lord, animstart);
	else if (failed == 2)
		show_message("Hijack Settings Reset", HZ*7);
	else
		show_message("Player Settings Lost", HZ*7);
	return animstart;
}
