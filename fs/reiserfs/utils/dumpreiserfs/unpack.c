#include <stdio.h>
#include <errno.h>
#include <malloc.h>
#include <string.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/stat.h>

/*uint32_t htonl (uint32_t hostlong);
uint32_t ntohl (uint32_t netlong);*/

int opt_skip_unfms = 0;
int opt_do_not_write = 0;

int waiting_read (int fd, char * buf, int count)
{
    int rd, done = 0;

    while (count) {
	rd = read (fd, buf, count);
	if (rd < 1)
	    return rd;
	buf += rd;
	count -= rd;
	done += rd;
    }
    return done;
}

int main (int argc, char ** argv)
{
    uint16_t blocksize, reclen16;
    uint32_t blocknumber32;
    int c;
    char * buf;
    int fd;
    int res;
    struct stat st;
  
    if (argc < 2) {
	printf ("Usage: gunzip -c | unpack [-s][-n] /dev/dest\n");
	return 0;
    }

    while ((c = getopt (argc, argv, "sn")) != EOF) {
	switch (c) {
	case 's': /* skip writing of unformatted nodes */
	    opt_skip_unfms = 1;
	    break;
	case 'n':
	    opt_do_not_write = 1;
	    break;
	default:
	    printf ("Usage: gunzip -c | unpack [-s] /dev/dest\n");
	    return 0;
	}
    }

    /* get file system's block size */
    read (0, &blocksize, sizeof (uint16_t));
    blocksize = ntohs (blocksize);
    fprintf (stderr, "blocksize = %d\n", blocksize);

    buf = (char *)malloc (blocksize);
    if (!buf) {
	perror ("malloc failed");
	return 1;
    }

    /* we need to skip the below:
       reiserfs: read_bitmaps: 0 blocks differ in true and cautious bitmaps
       reiserfs: read_bitmaps: 1 blocks differ in true and cautious bitmaps
    */

/*  
    read (0, buf, strlen ("reiserfs: read_bitmaps: 0 blocks differ in true and cautious bitmaps\n"));
    if (strncmp (buf, "reiserfs", strlen ("reiserfs"))) {
    fprintf (stderr, "Bad signature 1\n");
    return 1;
    }
*/
    /*  
	read (0, buf, strlen ("reiserfs: read_bitmaps: 1 blocks differ in true and cautious bitmaps\n"));
	if (strncmp (buf, "reiserfs", strlen ("reiserfs"))) {
	fprintf (stderr, "Bad signature 2\n");
	return 1;
	}*/

    if (is_mounted (argv[optind])) {
	/* check forced on clean filesystem, maybe we can rebuild it (if it is mounted read-only). Later. */
	die ("unpack: '%s' contains a mounted file system\n", argv[optind]);
    }

    if (stat (argv[optind], &st) == -1)
	die ("unpack: stat failed: %s", strerror (errno));
    if (!S_ISBLK (st.st_mode))
	die ("unpck: %s is not a block device", argv[optind]);

    fd = open (argv[optind], O_CREAT | O_RDWR);
    if (fd == -1) {
	perror ("open failed");
	return 1;
    }

    while ((res = waiting_read (0, (char *)&blocknumber32, sizeof (uint32_t))) == sizeof (uint32_t)) {
	/* read block number from stdin */
/*
  if (blocknumber32 == 0) {
  printf ("exit\n");
  exit (0);
  }
*/
	blocknumber32 = ntohl (blocknumber32);

	/* read 16 bit record length */
	if (waiting_read (0, (char *)&reclen16, sizeof (uint16_t)) != sizeof (uint16_t)) {
	    perror ("read reclen failed");
	    return 1;
	}
	reclen16 = ntohs (reclen16);

	fprintf (stderr, "%d reclen %d\n", blocknumber32, reclen16);

	/* read the record itself */
	if ((res = waiting_read (0, buf, reclen16)) != reclen16) {
	    fprintf (stderr, "read record failed (%d %d)\n", res, reclen16);
	    return 1;
	}


	/* the only one requirement to this block: does not look like
           leaf node. If you unpacked damaged partition already you
           might consider using -s to save time */
	if ((opt_skip_unfms && reclen16 == 2) || opt_do_not_write == 1)
	    continue;


	/* write to argv[1] */
	if (reiserfs_llseek (fd, (loff_t)blocknumber32 * (loff_t)blocksize, SEEK_SET) == (loff_t)-1) {
	    perror ("llseek failed");
	    return 1;
	}
	if (write (fd, buf, reclen16) != reclen16) {
	    perror ("write failed");
	    return 1;
	}
    }

    fprintf (stderr, "done\n");
    return 0;
}
