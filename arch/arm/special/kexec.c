// kexec.c:  adapted from kmod.c

#define __KERNEL_SYSCALLS__

#include <linux/sched.h>
#include <linux/unistd.h>
#include <linux/smp_lock.h>
#include <asm/uaccess.h>

#define WIFEXITED(status)	(((status) & 0x7f) == 0)
#define WEXITSTATUS(status)	(((status) & 0xff00) >> 8)

asmlinkage int sys_chdir(const char *);

static inline void
use_init_filesystem_context (void)
{
	struct fs_struct * fs;

	lock_kernel();
	fs = current->fs;
	dput(fs->root);
	dput(fs->pwd);
	fs->root = dget(init_task.fs->root);
	fs->pwd = dget(init_task.fs->pwd);
	fs->umask = 0022;
	unlock_kernel();
}

typedef struct kexec_parms {
	char	*cwd;
	char	*cmdline;
} kexec_parms_t;

static int
do_kexec (void *arg)
{
	kexec_parms_t *parms = arg;
	static char shell[] = "/bin/sh";
	static char *envp[] = { "HOME=/", "TERM=linux", "PATH=/sbin:/usr/sbin:/bin:/usr/bin", NULL };
	char *argv[] = { shell, "-c", parms->cmdline, NULL };
	int i;

	current->session = 1;
	current->pgrp = 1;
	use_init_filesystem_context();

	// Block parent process signals
	spin_lock_irq(&current->sigmask_lock);
	flush_signals(current);
	flush_signal_handlers(current);
	spin_unlock_irq(&current->sigmask_lock);

	// Close any inherited files
	for (i = 0; i < current->files->max_fds; i++ ) {
		if (current->files->fd[i])
			close(i);
	}

	// Drop the "current user" thing
	free_uid(current);

	// Give us all effective privileges
	current->uid = current->euid = current->fsuid = 0;
	cap_set_full(current->cap_inheritable);
	cap_set_full(current->cap_effective);

	// Allow syscall args to come from kernel space
	set_fs(KERNEL_DS);

	// Provide the basic food groups
	if (0 == open("/dev/null", O_RDONLY, 0)) {		// stdin
		if (1 == open("/dev/console", O_RDWR, 0))	// stdout
			(void)dup(1);				// stderr
	}

	// Set cwd, if specified
	if (parms->cwd)
		(void)sys_chdir(parms->cwd);

	// Go, go, go... never returns
	errno = execve(shell, argv, envp);

	printk(KERN_ERR "do_kexec: failed, errno = %d\n", errno);
	return -errno;
}

int hijack_exec (char *cwd, char *cmdline)
{
	int		rc, pid, status;
	sigset_t	old_sig;
	kexec_parms_t	parms;
	mm_segment_t	old_fs;

	parms.cwd     = cwd;
	parms.cmdline = cmdline;

	pid = kernel_thread(do_kexec, &parms, 0);
	if (pid < 0) {
		printk(KERN_ERR "kexec: fork failed, errno %d\n", -pid);
		return pid;
	}

	/* Block everything but SIGKILL/SIGSTOP */
	spin_lock_irq(&current->sigmask_lock);
	old_sig = current->blocked;
	siginitsetinv(&current->blocked, sigmask(SIGKILL) | sigmask(SIGSTOP));
	recalc_sigpending(current);
	spin_unlock_irq(&current->sigmask_lock);

	old_fs = get_fs();
	set_fs(KERNEL_DS);
	rc = waitpid(pid, &status, __WCLONE);
	set_fs(old_fs);

	/* Allow signals again.. */
	spin_lock_irq(&current->sigmask_lock);
	current->blocked = old_sig;
	recalc_sigpending(current);
	spin_unlock_irq(&current->sigmask_lock);

	if (rc != pid) {
		printk(KERN_ERR "kexec: waitpid(%d,...) failed, errno %d\n", pid, -rc);
	} else if (WIFEXITED(status)) {
		rc = (signed char)(WEXITSTATUS(status));
	} else {
		rc = 0;
	}
	printk("hijack_exec(\"%s\", \"%s\"), rc=%d (%s)\n", cwd ? cwd : "", cmdline, rc, rc ? "ERROR" : "okay");
	return rc;
}

