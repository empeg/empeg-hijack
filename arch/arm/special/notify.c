// Empeg hacks by Mark Lord <mlord@pobox.com>

#define __KERNEL_SYSCALLS__
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/major.h>
#include <linux/mm.h>
#include <linux/malloc.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/unistd.h>
#include <asm/smplock.h>
#include <asm/arch/hijack.h>
#include <linux/proc_fs.h>

#include "fast_crc32.c"					// 10X the CPU usage of non-table-lookup version
//extern unsigned long crc32 (char *buf, int len);	// drivers/net/smc9194_tifon.c (10X slower, 1KB smaller)

#include "empeg_mixer.h"

extern int hijack_current_mixer_input;
extern int hijack_current_mixer_volume;
extern int hijack_player_started;
extern int hijack_do_command (void *sparms, char *buf);
extern int strxcmp (const char *str, const char *pattern, int partial);					// khttpd.c
extern int do_remount(const char *dir,int flags,char *data);						// fs/super.c
extern int get_filesystem_info(char *);									// fs/super.c
extern int hijack_suppress_notify, hijack_reboot;							// hijack.c
extern void show_message (const char *message, unsigned long time);					// hijack.c
extern long sleep_on_timeout(struct wait_queue **p, long timeout);					// kernel/sched.c
extern signed long schedule_timeout(signed long timeout);						// kernel/sched.c

#define NOTIFY_MAX_LINES	11		// number of chars in notify_chars[] below
const
unsigned char *notify_names[NOTIFY_MAX_LINES] = {"FidTime", "Artist", "FID", "Genre", "MixerInput", "Track", "Sound", "Title", "Volume", "L", "Other"};
unsigned char  notify_chars[NOTIFY_MAX_LINES] = "#AFGMNSTVLO";
#define NOTIFY_FIDLINE		2		// index of 'F' in notify_chars[]
#define NOTIFY_MAX_LENGTH	64
static char notify_data[NOTIFY_MAX_LINES][NOTIFY_MAX_LENGTH] = {{0,},};

const char *
notify_fid (void)
{
	return &notify_data[NOTIFY_FIDLINE][2];
}

int
hijack_serial_notify (const unsigned char *s, int size)
{
	// "return 0" means "send data to serial port"
	// "return 1" means "discard without sending"
	//
	// Note that printk() will probably not work from within this routine
	//
	// FIXME: move this code to sys_write() to avoid printk conflicts!
	//
	static enum	{want_title, want_data, want_eol} state = want_title;
	char		*line;
	unsigned long	flags;
	switch (state) {
		default:
			state = want_title;
			// fall thru
		case want_title:
			if (!strxcmp(s, "  serial_notify_thread.cpp", 1)) {
				state = want_data;
				return hijack_suppress_notify;
			} else if (!strxcmp(s, "  dhcp_thread.cpp", 1)) {
				state = want_eol;
				return hijack_suppress_notify;
			} else if (!hijack_player_started && !strxcmp(s, "Vcb: 0x", 1)) {
				hijack_player_started = jiffies;
			} else if (!strxcmp(s, "Switching to baud rate: 4800, disabling logging", 1)) {
				hijack_player_started = jiffies;
				strcpy(notify_data[NOTIFY_MAX_LINES-1],"Needed in config.ini: [serial]car_rate=115200");
			}
			break;
		case want_data:
			if (size > 3 && *s == '@' && *++s == '@' && *++s == ' ' && *++s) {
				size -= 3;
				while (size > 0 && (s[size-1] <= ' ' || s[size-1] > '~'))
					--size;
				if (size > NOTIFY_MAX_LENGTH)
					size = NOTIFY_MAX_LENGTH;
				if (size > 0) {
					unsigned char i, c = *s;
					notify_chars[sizeof(notify_chars)-1] = c;	// overwrite 'O' to simplify search
					for (i = 0; c != notify_chars[i]; ++i);	// search for correct entry
					line = notify_data[i];			
					save_flags_cli(flags);
					if (--size > 0)
						memcpy(line, s+1, size);
					line[size] = '\0';
					restore_flags(flags);
				}
				state = want_eol;
				return hijack_suppress_notify;
			}
			break;
		case want_eol:
			if (s[size-1] == '\n')
				state = want_title;
			return hijack_suppress_notify;
	}
	return 0;
}

#define	EPOCH_YR		(1970)		/* Unix EPOCH = Jan 1 1970 00:00:00 */
#define	SECS_PER_HOUR		(60L * 60L)
#define	SECS_PER_DAY		(24L * SECS_PER_HOUR)
#define	IS_LEAPYEAR(year)	(!((year) % 4) && (((year) % 100) || !((year) % 400)))
#define	DAYS_PER_YEAR(year)	(IS_LEAPYEAR(year) ? 366 : 365)

const int DAYS_PER_MONTH[2][12] = {
	{ 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 },	// normal year
	{ 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 } };	// leap year

const char *hijack_months[12] =
	{ "Jan", "Feb", "Mar", "Apr", "May", "Jun",
	  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

tm_t *
hijack_convert_time (time_t time, tm_t *tm)
{
	unsigned long clock, day;
	const int *days_per_month;
	int month = 0, year = EPOCH_YR;

	clock   = (unsigned long)time % SECS_PER_DAY;
	day = (unsigned long)time / SECS_PER_DAY;
	tm->tm_sec   =  clock % 60;
	tm->tm_min   = (clock % SECS_PER_HOUR) / 60;
	tm->tm_hour  =  clock / SECS_PER_HOUR;
	tm->tm_wday  = (day + 4) % 7;	/* day 0 was a thursday */
	while ( day >= DAYS_PER_YEAR(year) ) {
		day -= DAYS_PER_YEAR(year);
		year++;
	}
	tm->tm_year  = year;
	tm->tm_yday  = day;
	days_per_month = DAYS_PER_MONTH[IS_LEAPYEAR(year)];
	month          = 0;
	while ( day >= days_per_month[month] ) {
		day -= days_per_month[month];
		month++;
	}
	tm->tm_mon   = month;
	tm->tm_mday  = day + 1;
	return tm;
}

int
remount_drives (int writeable)
{
	int	did_something = 0, len, flags;
	char	buf[400], *b, *message, *match, mount_opts[] = "nocheck";

	if (writeable) {
		flags = (MS_NODIRATIME | MS_NOATIME);
		message = "Remounting read-write";
		match = "ext2 ro";
	} else {
		flags = MS_RDONLY;
		message = "Remounting read-only";
		match = "ext2 rw";
	}
		
	lock_kernel();
	len = get_filesystem_info(buf);
	buf[len] = '\0';
	for (b = buf; *b;) {
		unsigned char *fsname, *fstype;
		while (*++b != ' ');
		fsname = ++b;
		while (*++b != ' ');
		*b = '\0';
		fstype = ++b;
		while (*++b != ' ');
		while (*++b != ' ');
		*b++ = '\0';
		if (!strxcmp(fstype, match, 1)) {
			if (!did_something) {
				did_something = 1;
				show_message(message, 99*HZ);
			}
			printk("hijack: %s: %s\n", message, fsname); 
			(void)do_remount(fsname, flags, mount_opts);
		}
		while (*b && *b++ != '\n');
	}
	unlock_kernel();
	if (did_something)
		show_message(writeable ? "Remounted read-write" : "Remounted read-only", HZ);
	return !did_something;
}

#ifdef CONFIG_NET_ETHERNET

static inline char *
pngcpy (char *png, const void *src, int len)
{
	const char *s = src;
	while (--len >= 0)
		*png++ = *s++;
	return png;
}

static inline char *
deflate0 (char *p, const char *screen, unsigned long *checksum)
{
	const char *end_of_screen = screen + EMPEG_SCREEN_BYTES;
	unsigned long s1 = 1, s2 = 0;

	// "type 0" zlib deflate algorithm: null filter, no compression
	do {
		const char *end_of_col = screen + (EMPEG_SCREEN_COLS/2);
		*p++ = 0;	// filter type 0
		s2 += s1;
		do {
			// pack 4 pixels per byte, ordered from msb to lsb
			unsigned char b, c;
			b = *screen++;
			b = (b & 0x30) | (b << 6);
			c = *screen++;
			b |= ((c & 3) << 2) | ((c >> 4) & 3);
			*p++ = b;
			s1 += b;
			s2 += s1;
		} while (screen != end_of_col);
	} while (screen != end_of_screen);
	s1 %= 65521;
	s2 %= 65521;
	*checksum = htonl((s2 << 16) | s1);
	return p;
}

static const char png_hdr[] = {137,'P','N','G','\r','\n',26,'\n',0,0,0,13,'I','H','D','R',
			0,0,0,EMPEG_SCREEN_COLS,0,0,0,EMPEG_SCREEN_ROWS,2,0,0,0,0,0xb5,0xf9,0x37,0x58,
			0,0,4,0x2b,'I','D','A','T',0x48,0x0d,0x01,0x20,0x04,0xdf,0xfb};
static const char png_iend[] = {0,0,0,0,'I','E','N','D',0xae,0x42,0x60,0x82};

// these two are shared with arch/arm/special/hijack.c
struct semaphore	notify_screen_grab_sem = MUTEX_LOCKED;
unsigned char		*notify_screen_grab_buffer = NULL;
static struct semaphore	one_at_a_time = MUTEX;


#define PNG_BYTES 1124

// /proc/empeg_screen read() routine:
static int
hijack_proc_screen_png_read (char *buf, char **start, off_t offset, int len, int unused)
{
	unsigned long		flags, crc, checksum;
	unsigned char		*displaybuf, *p, *crcstart;

	if (offset || !buf)
		return -EINVAL;

	// "buf" is guaranteed to be a 4096 byte scratchpad for our use,
	//   so we can use the latter half for our screen capture buffer.
	displaybuf = buf + ((PNG_BYTES + 63) & ~63);	// use an aligned offset

	down(&one_at_a_time);		// stop other processes from grabbing the screen
	save_flags_cli(flags);
	notify_screen_grab_buffer = displaybuf;
	restore_flags(flags);

	// Prepare an "image/png" snapshot of the screen:
	p = pngcpy(buf, png_hdr, sizeof(png_hdr));
	crcstart = p - 11;		// len field is not part of IDAT CRC

	down(&notify_screen_grab_sem);	// wait for screen capture
	up(&one_at_a_time);		// allow other processes to grab the screen

	p = deflate0(p, displaybuf, &checksum);
	p = pngcpy(p, &checksum, 4);
	crc = crc32(crcstart, p - crcstart) ^ 0xffffffff;
	crc = htonl(crc);
	p = pngcpy(p, &crc, 4);
	p = pngcpy(p, png_iend, sizeof(png_iend));

	return PNG_BYTES;	// same as (p - buf)
}

// /proc/empeg_screen directory entry:
static struct proc_dir_entry proc_screen_png_entry = {
	0,			/* inode (dynamic) */
	16,			/* length of name */
	"empeg_screen.png",	/* name */
	S_IFREG | S_IRUGO, 	/* mode */
	1, 0, 0, 		/* links, owner, group */
	PNG_BYTES,		/* size */
	NULL, 			/* use default operations */
	&hijack_proc_screen_png_read, /* get_info() */
};

// /proc/empeg_screen read() routine:
static int
hijack_proc_screen_raw_read (char *buf, char **start, off_t offset, int len, int unused)
{
	unsigned long flags;

	if (offset || !buf)
		return -EINVAL;

	// "buf" is guaranteed to be a 4096 byte scratchpad for our use,
	//   so we just dump directly to it.

	down(&one_at_a_time);		// stop other processes from grabbing the screen
	save_flags_cli(flags);
	notify_screen_grab_buffer = buf;
	restore_flags(flags);

	down(&notify_screen_grab_sem);	// wait for screen capture
	up(&one_at_a_time);		// allow other processes to grab the screen

	return EMPEG_SCREEN_BYTES;
}

// /proc/empeg_screen directory entry:
static struct proc_dir_entry proc_screen_raw_entry = {
	0,			/* inode (dynamic) */
	16,			/* length of name */
	"empeg_screen.raw",	/* name */
	S_IFREG | S_IRUGO, 	/* mode */
	1, 0, 0, 		/* links, owner, group */
	EMPEG_SCREEN_BYTES,	/* size */
	NULL, 			/* use default operations */
	&hijack_proc_screen_raw_read, /* get_info() */
};

static int
proc_notify_write (struct file *file, const char *buffer, unsigned long count, void *data)
{
	int rc;
	unsigned char *kbuf;

	// make a zero-terminated writeable copy to simplify parsing:
	if (!(kbuf = kmalloc(count + 1, GFP_KERNEL))) {
		rc = -ENOMEM;
	} else {
		memcpy(kbuf, buffer, count);
		kbuf[count] = '\0';
		rc = hijack_do_command(NULL, kbuf); 
		kfree(kbuf);
		if (!rc)
			rc = count;
	}
	return rc;
}

#else

#define proc_notify_write NULL

#endif // CONFIG_NET_ETHERNET

extern struct file_operations  flash_fops;
static struct inode_operations kflash_ops = {
	&flash_fops,
	NULL,
};

static struct proc_dir_entry proc_flash5_entry = {
	0,			/* inode (dynamic) */
	15,			/* length of name */
	"empeg_bootlogos",	/* name */
	S_IFBLK|S_IRUSR|S_IWUSR,/* mode */
	1, 0, 0,		/* links, owner, group */
	MKDEV(60,5),		/* size holds device number */
	&kflash_ops,		/* inode operations */
};

#ifdef CONFIG_NET_ETHERNET
static struct proc_dir_entry proc_flash6_entry = {
	0,			/* inode (dynamic) */
	11,			/* length of name */
	"flash_0c000",		/* name */
	S_IFBLK|S_IRUSR|S_IWUSR,/* mode */
	1, 0, 0,		/* links, owner, group */
	MKDEV(60,6),		/* size holds device number */
	&kflash_ops,		/* inode operations */
};
static struct proc_dir_entry proc_flash7_entry = {
	0,			/* inode (dynamic) */
	11,			/* length of name */
	"flash_0e000",		/* name */
	S_IFBLK|S_IRUSR|S_IWUSR,/* mode */
	1, 0, 0,		/* links, owner, group */
	MKDEV(60,7),		/* size holds device number */
	&kflash_ops,		/* inode operations */
};
static struct proc_dir_entry proc_flash8_entry = {
	0,			/* inode (dynamic) */
	12,			/* length of name */
	"empeg_kernel",		/* name */
	S_IFBLK|S_IRUSR|S_IWUSR,/* mode */
	1, 0, 0,		/* links, owner, group */
	MKDEV(60,8),		/* size holds device number */
	&kflash_ops,		/* inode operations */
};
static struct proc_dir_entry proc_flash9_entry = {
	0,			/* inode (dynamic) */
	11,			/* length of name */
	"flash_b0000",		/* name */
	S_IFBLK|S_IRUSR|S_IWUSR,/* mode */
	1, 0, 0,		/* links, owner, group */
	MKDEV(60,9),		/* size holds device number */
	&kflash_ops,		/* inode operations */
};
#endif

static int
proc_notify_read (char *buf, char **start, off_t offset, int len, int unused)
{
	int i;

	len = 0;
	for (i = 0; i < NOTIFY_MAX_LINES; ++i) {
		const char *name;
		char *data, tmp[16];
		unsigned long flags;
		save_flags_cli(flags);	// protect access to notify_data[]
		data = notify_data[i];
		name = notify_names[i];
		switch (name[0]) {
			case 'M':	// mixer input is not notified until modified
				switch (hijack_current_mixer_input) {
					case INPUT_RADIO_FM: data = "FM" ; break;
					case INPUT_PCM:      data = "PCM"; break;
					case INPUT_AUX:      data = "AUX"; break;
					case INPUT_RADIO_AM: data = "AM" ; break;
					default:             data = ""   ; break;
				}
				break;
			case 'V':	// volume is not notified until adjusted up/down
				data = tmp;
				sprintf(data, "%u", hijack_current_mixer_volume);
				break;
			//case 'S':	// mute is not always correct until adjusted after boot
		}
		len += sprintf(buf+len, "notify_%s = \"%s\";\n", name, data);
		restore_flags(flags);
	}
	return len;
}

// notify proc directory entry:
static struct proc_dir_entry proc_notify_entry = {
	0,				// inode (dynamic)
	12,				// length of name
	"empeg_notify",			// name
	S_IFREG|S_IRUGO|S_IWUSR,	// mode
	1, 0, 0, 			// links, owner, group
	0, 				// size
	NULL, 				// use default operations
	proc_notify_read,		// get_info (simple readproc)
	NULL,				// fill_inode
	NULL,NULL,NULL,			// next, parent, subdir
	NULL,				// callback data
	NULL,				// readproc (using simpler get_info() instead)
	proc_notify_write,		// writeproc
	NULL,				// readlink
	0,				// usage count
	0				// deleted flag
};

void hijack_notify_init (void)
{
#ifdef CONFIG_NET_ETHERNET
	proc_register(&proc_root, &proc_screen_raw_entry);
	proc_register(&proc_root, &proc_screen_png_entry);
#endif // CONFIG_NET_ETHERNET
	proc_register(&proc_root, &proc_notify_entry);
#ifdef CONFIG_NET_ETHERNET
	proc_register(&proc_root, &proc_flash9_entry);
	proc_register(&proc_root, &proc_flash8_entry);
	proc_register(&proc_root, &proc_flash7_entry);
	proc_register(&proc_root, &proc_flash6_entry);
#endif // CONFIG_NET_ETHERNET
	proc_register(&proc_root, &proc_flash5_entry);
}
