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
 
#ifdef CONFIG_NET_ETHERNET
extern int crc32 (char *buf, int len);	// drivers/net/smc9194_tifon.c
#else
int crc32( char * s, int length ) { 
  /* indices */
  int perByte;
  int perBit;
  /* crc polynomial for Ethernet */
  const unsigned long poly = 0xedb88320;
  /* crc value - preinitialized to all 1's */
  unsigned long crc_value = 0xffffffff; 

  for ( perByte = 0; perByte < length; perByte ++ ) {
    unsigned char	c;
	
    c = *(s++);
    for ( perBit = 0; perBit < 8; perBit++ ) {
      crc_value = (crc_value>>1)^
	(((crc_value^c)&0x01)?poly:0);
      c >>= 1;
    }
  }
  return	crc_value;
} 
#endif
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
#define NOTIFY_MAX_LINES	(sizeof(notify_labels)) // gives one extra for '\0'
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

static char *
pngcpy (char *png, const void *src, int len)
{
	const char *s = src;
	while (--len >= 0)
		*png++ = *s++;
	return png;
}

static char *
deflate0 (char *p, const char *screen, unsigned long *checksum)
{
	const char *end_of_screen = screen + EMPEG_SCREEN_BYTES;
	unsigned long s1 = 1, s2 = 0;

	do {
		const char *end_of_col = screen + (EMPEG_SCREEN_COLS/2);
		*p++ = 0;	// filter type 0
		s2 += s1;
		do {
			unsigned char b, c;
			b = *screen++;
			c = *screen++;
			b = (b & 0x30) | (b << 6);
			b |= ((c & 3) << 2) | ((c >> 4) & 3);
			s1 += b;
			s2 += s1;
			*p++ = b;
		} while (screen != end_of_col);
	} while (screen != end_of_screen);
	s1 %= 65521;
	s2 %= 65521;
	*checksum = htonl((s2 << 16) | s1);
	return p;
}

static const char png_ihdr[] = {137,'P','N','G','\r','\n',26,'\n',0,0,0,13,'I','H','D','R',
				0,0,0,EMPEG_SCREEN_COLS,0,0,0,EMPEG_SCREEN_ROWS,2,0,0,0,0,0xb5,0xf9,0x37,0x58};
static const char png_idat[] = {0,0,4,0x2b,'I','D','A','T',0x48,0x0d,0x01,0x20,0x04,0xdf,0xfb};
static const char png_iend[] = {0,0,0,0,'I','E','N','D',0xae,0x42,0x60,0x82};

static void
make_png (char *displaybuf, char *png)
{
	unsigned long crc, checksum;
	char *p, *crcstart;

	p = pngcpy(png, png_ihdr, sizeof(png_ihdr));
	crcstart = p + 4;	// len field is not part of CRC
	p = pngcpy(p , png_idat, sizeof(png_idat));
	p = deflate0(p, displaybuf, &checksum);
	p = pngcpy(p, &checksum, 4);
	crc = crc32(crcstart, p - crcstart) ^ 0xffffffff;
	crc = htonl(crc);
	p = pngcpy(p, &crc, 4);
	p = pngcpy(p, png_iend, sizeof(png_iend));
}

// these two are shared with arch/arm/special/hijack.c
struct semaphore	notify_screen_grab_sem = MUTEX_LOCKED;
unsigned char		*notify_screen_grab_buffer = NULL;

#define PNG_BYTES 1124

// /proc/empeg_screen read() routine:
static int
hijack_proc_screen_read (char *buf, char **start, off_t offset, int len, int unused)
{
	unsigned long	flags;
	unsigned char	*displaybuf;

	if (offset)
		return -EINVAL;
	displaybuf = kmalloc(EMPEG_SCREEN_BYTES, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	save_flags_cli(flags);
	notify_screen_grab_sem = MUTEX_LOCKED;
	notify_screen_grab_buffer = displaybuf;
	down(&notify_screen_grab_sem);
	restore_flags(flags);

	make_png(displaybuf, buf);
	kfree(displaybuf);

	return PNG_BYTES;
}

// /proc/empeg_screen directory entry:
static struct proc_dir_entry proc_screen_entry = {
	0,			/* inode (dynamic) */
	16,			/* length of name */
	"empeg_screen.png",	/* name */
	S_IFREG | S_IRUGO, 	/* mode */
	1, 0, 0, 		/* links, owner, group */
	PNG_BYTES,		/* size */
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
