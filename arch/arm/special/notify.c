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
 
extern int do_remount(const char *dir,int flags,char *data);
extern int hijack_supress_notify, hijack_reboot;							// hijack.c
extern int get_number (unsigned char **src, int *target, unsigned int base, const char *nextchars);	// hijack.c
extern void hijack_button_enq_inputq (unsigned int button, unsigned int hold_time);			// hijack.c
extern void input_append_code(void *ignored, unsigned long button);					// hijack.c
extern void show_message (const char *message, unsigned long time);					// hijack.c
extern int strxcmp (const char *str, const char *pattern, int partial);					// kftpd.c
extern long sleep_on_timeout(struct wait_queue **p, long timeout);			// kernel/sched.c
extern signed long schedule_timeout(signed long timeout);						// kernel/sched.c

unsigned char notify_labels[] = "#AFGLMNSTV";	// 'F' must match next line
#define NOTIFY_FIDLINE		2		// index of 'F' in notify_labels[]
#define NOTIFY_MAX_LINES	(sizeof(notify_labels))
#define NOTIFY_MAX_LENGTH	64
static char notify_data[NOTIFY_MAX_LINES][NOTIFY_MAX_LENGTH] = {{0,},};
static const char notify_prefix[] = "  serial_notify_thread.cpp";
static const char dhcp_prefix[]   = "  dhcp_thread.cpp";

const char *
notify_fid (void)
{
	return &notify_data[NOTIFY_FIDLINE][3];
}

int
hijack_serial_notify (const unsigned char *s, int size)
{
	// "return 0" means "send data to serial port"
	// "return 1" means "discard without sending"
	//
	// Note that printk() will probably not work from within this routine
	static enum {want_title, want_data, want_eol} state = want_title;

	switch (state) {
		default:
			state = want_title;
			// fall thru
		case want_title:
		{
			const int notify_len = sizeof(notify_prefix) - 1;
			const int dhcp_len   = sizeof(dhcp_prefix)   - 1;

			if (size >= notify_len && !memcmp(s, notify_prefix, notify_len)) {
				state = want_data;
				return hijack_supress_notify;
			} else if (size >= dhcp_len && !memcmp(s, dhcp_prefix, dhcp_len)) {
				state = want_eol;
				return hijack_supress_notify;
			}
			break;
		}
		case want_data:
		{
			char		*line;
			unsigned long	flags;

			if (size > 3 && *s == '@' && *++s == '@' && *++s == ' ' && *++s) {
				size -= 3;
				while (size > 0 && (s[size-1] <= ' ' || s[size-1] > '~'))
					--size;
				if (size > (NOTIFY_MAX_LENGTH - 1))
					size = (NOTIFY_MAX_LENGTH - 1);
				if (size > 0) {
					unsigned char i, c = *s;
					notify_labels[sizeof(notify_labels)-1] = c;
					for (i = 0; c != notify_labels[i]; ++i);
					line = notify_data[i];
					save_flags_cli(flags);
					memcpy(line, s, size);
					line[size] = '\0';
					restore_flags(flags);
				}
				state = want_eol;
				return hijack_supress_notify;
			}
			break;
		}
		case want_eol:
		{
			if (s[size-1] == '\n')
				state = want_title;
			return hijack_supress_notify;
		}
	}
	return 0;
}

static void
insert_button_pair (unsigned int button, unsigned int hold_time)
{
	unsigned int release = 0x80000000;
	struct wait_queue *wq = NULL;

	button &= ~release;
	if (button < 0xf) {
		release = 1;
		if (button == IR_KNOB_LEFT && button == IR_KNOB_RIGHT)
			release = 0;
		else
			button &= ~1;
	}
	sleep_on_timeout(&wq, HZ/50);
	input_append_code (NULL, button);
	if (release) {
		sleep_on_timeout(&wq, hold_time);
		input_append_code (NULL, button|release);
	}
}

static int
remount_drives (int writeable)
{
	int rc0, rc1, flags = writeable ? (MS_NODIRATIME | MS_NOATIME) : MS_RDONLY;

	show_message(writeable ? "Remounting read-write.." : "Remounting read-only..", 99*HZ);

	lock_kernel();
	rc0 = do_remount("/drive0", flags, NULL);
	rc1 = do_remount("/drive1", flags, NULL);     // returns -EINVAL on 1-drive systems
	if (rc1 != -EINVAL && !rc0)
		rc0 = rc1;
	rc1 = do_remount("/", flags, NULL);
	unlock_kernel();
	sync();
	sync();

	if (!rc1)
		rc1 = rc0;
	show_message(rc1 ? "Failed" : "Done", HZ);
	return rc1;
}

extern int hijack_reboot;

int
hijack_do_command (const char *buffer, unsigned int count)
{
	int	rc = 0;
	char	*nextline, *kbuf;

	// make a zero-terminated writeable copy to simplify parsing:
	if (!(kbuf = kmalloc(count + 1, GFP_KERNEL)))
		return -ENOMEM;
	memcpy(kbuf, buffer, count);
	kbuf[count] = '\0';
	nextline = kbuf;

	while (!rc && *nextline) {
		unsigned char *s;
		s = nextline;
		while (*nextline && *++nextline != '\n' && *nextline != ';');
		*nextline = '\0';

		if (!strxcmp(s, "BUTTON ", 1)) {
			unsigned int button;
			s += 7;
			if (*s && get_number(&s, &button, 16, NULL)) {
				unsigned int hold_time = 5;
				if (s[0] == '.' && s[1] == 'L')
					hold_time = HZ+(HZ/5);
				insert_button_pair(button, hold_time);
			}
		} else if (!strxcmp(s, "RO", 0)) {
			rc = remount_drives(0);
		} else if (!strxcmp(s, "RW", 0)) {
			rc = remount_drives(1);
		} else if (!strxcmp(s, "REBOOT", 0)) {
			remount_drives(0);
			hijack_reboot = 1;
		} else if (!strxcmp(s, "POPUP ", 1)) {
			int secs;
			s += 6;
			if (*s && get_number(&s, &secs, 10, NULL) && *s++ == ' ' && *s)
				show_message(s, secs * HZ);
			else
				rc = -EINVAL;
		//} else if (!strxcmp(s, "SERIAL ", 1)) {
		//	/* fixme: not implemented yet */
		} else {
			rc = -EINVAL;
		}
	}
	kfree(kbuf);
	return rc;
}

static int
proc_notify_write (struct file *file, const char *buffer, unsigned long count, void *data)
{
	int rc = hijack_do_command(buffer, count); 
	if (!rc)
		rc = count;
	return rc;
}

static int
proc_notify_read (char *buf, char **start, off_t offset, int len, int unused)
{
	int	i;

	len = 0;
	for (i = 0; i < NOTIFY_MAX_LINES; ++i) {
		char *n = notify_data[i];
		if (*n) {
			unsigned long flags;
			save_flags_cli(flags);
			len += sprintf(buf+len, "%s\n", n);
			restore_flags(flags);
		}
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

typedef struct tiff_field_s {
	unsigned short	tag;
	unsigned short	type;
	unsigned long	count;
	unsigned long	value_or_offset;
} tiff_field_t;

typedef struct grayscale_tiff_s {
	// header
	unsigned short	II;			// 'II'
	unsigned short	fortytwo;		// 42 (decimal)
	unsigned long	ifd0_offset;		// 10
	unsigned short	padding;		//  0  // to align the tiff fields on 4-byte boundaries

	// "ifd0"				// tag,type,count,value
	unsigned short	ifd0_count;		// 12  // count of tiff_field's that follow
	tiff_field_t	cols;			// 256, 3,    1,   128
	tiff_field_t	rows;			// 257, 3,    1,    32
	tiff_field_t	bitspersample;		// 258, 3,    1,     4	// ??
	tiff_field_t	compression;		// 259, 3,    1,     1	// byte packed (2 pixels/byte)
	tiff_field_t	photometric;		// 262, 3,    1,     1	// black is zero
	tiff_field_t	bitorder;		// 266, 3,    1,     1	// columns go from msb to lsb of bytes
	tiff_field_t	stripoffsets;		// 273, 3,    1,    12 + (12 * sizeof(tiff_field_t)) + (5 * 4)
	tiff_field_t	rowsperstrip;		// 278, 3,    1,    32
	tiff_field_t	stripbytecounts;	// 279, 3,    1,    128*32*2/8 
	tiff_field_t	xresolution;		// 282, 5,    1,    12 + (12 * sizeof(tiff_field_t)) + (1 * 4)
	tiff_field_t	yresolution;		// 283, 5,    1,    12 + (12 * sizeof(tiff_field_t)) + (3 * 4)
	tiff_field_t	resolutionunit;		// 296, 3,    1,     2  // resolution is in inches
	unsigned long	ifd1_offset;		//        0		// no ifd1 in this file

	// longer field values
	unsigned long	xresolution_numerator;	// 128 * 13		// 128 pixels in 13/4" of space
	unsigned long	xresolution_denominator;//        4 
	unsigned long	yresolution_numerator;	//  32 * 13		// 32 pixels in 13/16" of space
	unsigned long	yresolution_denominator;//       16

	// image data
	unsigned char	strip0[128*32*4/8];	// the row by row image data
} grayscale_tiff_t;

static grayscale_tiff_t proc_screen_tiff = {
	'I'|('I'<<8),						// II
	42,							// fortytwo
	10,							// ifd0_offset
	0,							// padding
	12,							// ifd0_count
	{256,3,1,128},						// cols
	{257,3,1, 32},						// rows
	{258,3,1,  4},						// bitspersample
	{259,3,1,  1},						// compression
	{262,3,1,  1},						// photometric
	{266,3,1,  1},						// bitorder
	{273,3,1, 12 + (12 * sizeof(tiff_field_t)) + (5 * 4)},	// stripoffsets
	{278,3,1, 32},						// rowsperstrip
	{279,3,1,128 * 32 * 4 / 8},				// stripbytecounts
	{282,5,1, 12 + (12 * sizeof(tiff_field_t)) + (1 * 4)},	// xresolution
	{283,5,1, 12 + (12 * sizeof(tiff_field_t)) + (3 * 4)},	// yresolution
	{296,3,1,  2},						// resolutionunit
	       0,						// ifd1_offset
	128 * 13,						// yresolution_numerator
	       4,						// yresolution_denominator
	 32 * 13,						// xresolution_numerator
	      16,						// xresolution_denominator
	{0,}							// strip0[128*32*2/8]
	};

// these three are shared with arch/arm/special/hijack.c
struct semaphore	notify_screen_grab_sem = MUTEX_LOCKED;
unsigned char		notify_screen_grab_buf[EMPEG_SCREEN_BYTES];
int			notify_screen_grab_needed = 0;

// /proc/empeg_screen read() routine:
static int
hijack_proc_screen_read (char *buf, char **start, off_t offset, int len, int unused)
{
	int		i, remaining;
	unsigned char	*t, *d;
	unsigned long	flags;

	if (offset == 0) {
		save_flags_cli(flags);
		notify_screen_grab_sem = MUTEX_LOCKED;
		notify_screen_grab_needed = 1;
		down(&notify_screen_grab_sem);
		restore_flags(flags);

		t = proc_screen_tiff.strip0;
		d = notify_screen_grab_buf;
		for (i = 0; i < sizeof(proc_screen_tiff.strip0); ++i) {
			static const unsigned char colors1[4] = {0x00,0x03,0x07,0x0f};
			static const unsigned char colors2[4] = {0x00,0x30,0x70,0xf0};
			unsigned char b = *d++ & 0x33;
			*t++ = colors2[b & 0xf] | colors1[b >> 4];
		}
	}
	remaining = sizeof(proc_screen_tiff) - offset;
	if (len > remaining)
	        len = (remaining < 0) ? 0 : remaining;
	if (len > 0) {
	        *start = buf + offset;
	        memcpy(buf, ((unsigned char *)&proc_screen_tiff) + offset, len);
	}
	return len;
}

// /proc/empeg_screen directory entry:
static struct proc_dir_entry proc_screen_entry = {
	0,			/* inode (dynamic) */
	17,			/* length of name */
	"empeg_screen.tiff",	/* name */
	S_IFREG | S_IRUGO, 	/* mode */
	1, 0, 0, 		/* links, owner, group */
	sizeof(proc_screen_tiff), /* size */
	NULL, 			/* use default operations */
	&hijack_proc_screen_read, /* get_info() */
};

extern struct file_operations  flash_fops;
static struct inode_operations kflash_ops = {
	&flash_fops,
	NULL,
};

static struct proc_dir_entry proc_kflash_entry = {
	0,			/* inode (dynamic) */
	12,			/* length of name */
	"empeg_kernel",		/* name */
	S_IFBLK|S_IRUSR|S_IWUSR,/* mode */
	1, 0, 0, 		/* links, owner, group */
	MKDEV(60,8),		/* size holds device number */
	&kflash_ops,		/* inode operations */
};

void hijack_notify_init (void)
{
	proc_register(&proc_root, &proc_notify_entry);
	proc_register(&proc_root, &proc_screen_entry);
	proc_register(&proc_root, &proc_kflash_entry);
}
