#define __KERNEL_SYSCALLS__

#include <linux/sched.h>
#include <linux/unistd.h>
#include <linux/smp_lock.h>
#include <linux/mm.h>
#include <linux/malloc.h>
#include <linux/string.h>
#include <asm/uaccess.h>

#undef KEXEC_CAPTURE

typedef struct hijack_execve_parms_s {
#ifdef KEXEC_CAPTURE
	int	capture;	// 0 == do NOT capture stdout/stderr
	int	stdout_r;	// readable file descriptor for stdout/stderr (for parent thread)
	int	stdout_w;	// writeable file descriptor for stdout/stderr (for execve child)
#endif
	char	*cmdline;	// (copy of) original cmdline
	char	**argv;		// argv[] array (including argv[0] and ending null)
	int	argc;		// arg count
} hijack_execve_parms_t;

static char **
split_cmdline (char *cmdline, int *argc)
{
	char **argv = NULL;
	int second_pass = 0;

	// Make two full passes: once to count args (for kmalloc), and again to separate/save them.
	do {
		int count = 0;
		char *p = cmdline;
		while (*p) {
			while (*p == ' ' || *p == '\t')			// skip over whitespace
				p++;
			if (*p) {
				char end1 = ' ', end2 = '\t';
				if (*p == '"' || *p == '\'')		// arg within quotes: find matching quote
					end1 = end2 = *p++;
				if (second_pass)
					argv[count] = p;
				++count;
				while (*p && *p != end1 && *p != end2) { // find end of this arg
					if (*p == '\\' && !*++p)	// skip over back-quoted char
						break;
					++p;
				}
				if (*p) {	// ensure arg is zero-terminated
					if (second_pass)
						*p = '\0';
					++p;
				}
			}
		}
		if (!second_pass)
			argv = kmalloc((1 + count) * sizeof(char *), GFP_KERNEL);
		argv[count] = NULL;
		*argc = count;
	} while (!second_pass++);
	return argv;
}

hijack_execve_parms_t *
hijack_create_execve_parms (const char *cmdline, int capture)
{
	hijack_execve_parms_t *parms;

	parms = kmalloc(sizeof(hijack_execve_parms_t), GFP_KERNEL);
	memset(parms, 0, sizeof(hijack_execve_parms_t));
	parms->cmdline = kmalloc(strlen(cmdline) + 1, GFP_KERNEL);
	strcpy(parms->cmdline, cmdline);
	parms->argv = split_cmdline(parms->cmdline, &(parms->argc));
#ifdef KEXEC_CAPTURE
	if (capture) {
		parms->capture = 1;
		int stdout_fds[2];
		if (do_pipe(stdout_fds) < 0) {
			printk("hijack_exec: couldn't create stdout pipe");
			return NULL;
		}
		parms->stdout_r = stdout_fds[0];
		parms->stdout_w = stdout_fds[1];
	}
#endif
	return parms;
}

static int
hijack_do_execve (void *arg)
{
	hijack_execve_parms_t *parms = arg;
	static char *envp[] = {"HOME=/", "TERM=linux", "PATH=/sbin:/usr/sbin:/bin:/usr/bin", NULL};
	int i;
	struct fs_struct * fs;

	// Copy root dir and cwd from init
	fs = current->fs;
	dput(fs->root);
	dput(fs->pwd);
	fs->root = dget(init_task.fs->root);
	fs->pwd = dget(init_task.fs->pwd);

	// Close our copies of user's open files
	for (i = 0; i < current->files->max_fds; i++ ) {
		if (current->files->fd[i])
			close(i);
	}

#ifdef KEXEC_CAPTURE
	/* connect up stdout and stderr to pipes if required */
	if (parms->capture) {
		extern asmlinkage int sys_dup2(unsigned int oldfd, unsigned int newfd);
		sys_dup2(parms->stdout_w, 1);	// connect stdout (fd 1) to pipe
		sys_dup2(parms->stdout_w, 2);	// connect stderr (fd 2) to pipe (same as stdout)
		close(parms->stdout_w);		// drop the original stdout fd
		close(parms->stdout_r);		// we don't use this fd here at all
	}
#endif

	// Drop the "current user" thing
	free_uid(current);

	// Grab all possible privileges
	current->uid = current->euid = current->fsuid = 0;
	cap_set_full(current->cap_inheritable);
	cap_set_full(current->cap_effective);

	// Allow execve args to live in kernel space
	set_fs(KERNEL_DS);

	if (execve(parms->argv[0], parms->argv, envp) < 0) {
		printk("hijack_do_execve: execve() failed, errno = %d\n", errno);
		return -errno;
	}
	return 0;	// never executed
}

static int
hijack_waitpid (pid_t pid, int *statusp)
{
        int		waitpid_result;
        sigset_t	old_sig;
	mm_segment_t	old_fs;

	/* Block everything but SIGKILL/SIGSTOP */
	spin_lock_irq(&current->sigmask_lock);
	old_sig = current->blocked;
	siginitsetinv(&current->blocked, sigmask(SIGKILL) | sigmask(SIGSTOP));
	recalc_sigpending(current);
	spin_unlock_irq(&current->sigmask_lock);

	old_fs = get_fs();
	set_fs(KERNEL_DS);
	waitpid_result = waitpid(pid, statusp, __WCLONE);	// BUG? Will this will "reap" player/kftpd/khttpd PIDs.. (and vice-versa)??
	set_fs(old_fs);

	/* Allow signals again.. */
	spin_lock_irq(&current->sigmask_lock);
	current->blocked = old_sig;
	recalc_sigpending(current);
	spin_unlock_irq(&current->sigmask_lock);

	return waitpid_result;
}

#define WIFEXITED(status)	(((status) & 0x7f) == 0)
#define WEXITSTATUS(status)	(((status) & 0xff00) >> 8)

static int
hijack_execve (hijack_execve_parms_t *parms)
{
	int pid, status, rc;

	// create a child thread to do the execve() while we wait for it
	pid = kernel_thread(hijack_do_execve, (void*) parms, 0);
	if (pid < 0) {
		printk("hijack_execve[%s]: fork failed, errno %d\n", parms->argv[0], -pid);
		return pid;
	}

#ifdef KEXEC_CAPTURE
	// close un-needed pipe endpoint
	if (parms->capture)
		(void) close(parms->stdout_w);
#endif
	rc = hijack_waitpid(pid, &status);	// BUG: Ugh.. cannot do this here if capturing.
	if (rc != pid) {
		printk ("hijack_execve: waitpid(%d,0x%p,0x%x) failed, returning %d.\n", pid, &status, __WCLONE, rc);
	} else if (WIFEXITED(status)) {
		rc = (signed char)(WEXITSTATUS(status));
	} else {
		rc = 0;
	}
	return rc;
}

int
hijack_exec (const char *cmdline)
{
	hijack_execve_parms_t *parms = hijack_create_execve_parms(cmdline, 0);
	int rc = hijack_execve(parms);
	kfree(parms->cmdline);
	kfree(parms->argv);
	kfree(parms);
	printk("hijack_exec(\"%s\"), rc=%d (%s)\n", cmdline, rc, rc ? "ERROR" : "okay");
	return rc;
}

