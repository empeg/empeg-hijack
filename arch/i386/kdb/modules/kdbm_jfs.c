#include <linux/kdb.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/jfs.h>

#define FETCH(dest, source) \
do {							\
	int i;						\
	unsigned char *p = (unsigned char *)(dest);	\
							\
	for (i=0; i<sizeof(*(dest)); i++)		\
		*p++ = kdbgetword((source)+i, 1);	\
} while (0)

const char *bh_listnames[] = {
	"BUF_CLEAN",
	"BUF_LOCKED",
	"BUF_DIRTY",
	"BUF_JOURNAL"
};

const char *jfs_listnames[] = {
	"BJ_None",
	"BJ_Data",
	"BJ_Metadata",
	"BJ_Forget",
	"BJ_IO",
	"BJ_Shadow",
	"BJ_LogCtl",
	"BJ_Reserved"
};

const char *statenames[] = {
	"T_RUNNING",
	"T_LOCKED",
	"T_RUNDOWN",
	"T_FLUSH",
	"T_COMMIT",
	"T_FINISH"
};

const char *bh_statenames = "UDLRPTJWQAF";
int bh_states[] = {0,1,2,3,6,7,8,9,10,11,12,-1};

const char *bh_listname(int list)
{
	if (list >= NR_LIST)
		return "*INVALID*";
	return bh_listnames[list];
}

const char *jfs_listname(int list)
{
	if (list >= BJ_Types)
		return "*INVALID*";
	return jfs_listnames[list];
}

const char *transaction_state(int state)
{
	static char statenum[16];
	if (state > T_FINISHED) {
		sprintf(statenum, "[%d]", state);
		return statenum;
	}
	return statenames[state];
}

const char *bh_state(int state)
{
	static char buffer[33];
	char *result = buffer;
	int i = 0;
	do {
		if (state & (1<<bh_states[i]))
			result[i] = bh_statenames[i];
		else
			result[i] = '.';
		i++;
	} while (bh_states[i] >= 0);
	result[i] = 0;
	return result;
}


int
kdbm_bh(int argc, const char **argv, const char **envp, struct pt_regs *regs)
{
	struct buffer_head bh;
	unsigned long addr;
	long	offset=0;
	int nextarg;
	int diag;

	if (argc != 1)
		return KDB_ARGCOUNT;

	nextarg = 1;
	diag = kdbgetaddrarg(argc, argv, &nextarg, &addr, &offset, NULL, regs);
	if (diag)
		return diag;

	addr += offset;

	FETCH(&bh, addr);

	kdb_printf("struct buffer_head 0x%lx for %d bytes\n", addr, sizeof(bh));
	kdb_printf("  dev   = %-11s          blk   = 0x%08lx   size  = 0x%04lx\n",
	       kdevname(bh.b_dev), bh.b_blocknr, bh.b_size);
	kdb_printf("  data  = %p\n", bh.b_data);
	kdb_printf("  list  = %-3d (%-11s)    state = %7s       count = %-5d\n",
	       bh.b_list, bh_listname(bh.b_list),
	       bh_state(bh.b_state),
	       bh.b_count);
	kdb_printf("  jlist = %-3d (%-11s)    transaction      = %p\n",
	       bh.b_jlist, jfs_listname(bh.b_jlist), bh.b_transaction);
	kdb_printf("  frozen data       = %p    cp_transaction   = %p\n",
	       bh.b_frozen_data, bh.b_cp_transaction);
	kdb_printf("  committed data    = %p    next_transaction = %p\n",
	       bh.b_committed_data, bh.b_next_transaction);
	kdb_printf("  alloc transacion  = %d    alloc2_transact  = %d\n",
		   bh.b_alloc_transaction, bh.b_alloc2_transaction);
	kdb_printf("  free transaction  = %d    free2_transaction= %d\n",
	       bh.b_free_transaction, bh.b_free2_transaction);
	return 0;
}

int
kdbm_bhi(int argc, const char **argv, const char **envp, struct pt_regs *regs)
{
	show_buffers();
	return 0;
}


static void show_journal(journal_t *journal, unsigned long where)
{
	kdb_printf("journal at %08lx for %d bytes:\n", where, sizeof(*journal));

	kdb_printf(" State: ");
	if (journal->j_flags & JFS_UNMOUNT)
		kdb_printf("Unmount ");
	if (journal->j_flags & JFS_SYNC)
		kdb_printf("Sync ");
	if (journal->j_locked)
		kdb_printf("Locked ");
	kdb_printf("\n");

	kdb_printf(" Log head: %ld  Tail: %ld  Free: %ld     "
	       "Valid: %ld to %ld (+%d)\n",
	       journal->j_head, journal->j_tail, journal->j_free,
	       journal->j_first, journal->j_last, journal->j_maxlen);

	kdb_printf(" Transactions IDs:\n");
	kdb_printf("  Oldest:     %-8ld          Next:       %-8ld\n",
		   journal->j_tail_sequence,
		   journal->j_transaction_sequence);
	kdb_printf("    Committed:  %-8ld          Req commit: %-8ld\n",
		   journal->j_commit_sequence,
		   journal->j_commit_request);
	kdb_printf("\n");
}

static void show_transaction_brief(transaction_t *transaction,
				   unsigned long where,
				   const char *kind)
{
	kdb_printf("%s%s at 0x%08lx (journal 0x%08x) for %d bytes:\n",
	       kind ? kind : "", kind ? " transaction" : "Transaction",
	       where, transaction->t_journal, sizeof(*transaction));

	kdb_printf(" ID: %-5ld   State: %9s   "
	       "Buffers: %-5d Log at: %-5ld\n",
	       transaction->t_tid, transaction_state(transaction->t_state),
	       transaction->t_nr_buffers, transaction->t_log_start);
	kdb_printf(" Updates: %-5d   Credits: %-5d\n",
	       transaction->t_updates, transaction->t_outstanding_credits);
}

static void show_transaction_list(unsigned long where, const char *kind,
				  unsigned long offset)
{
	unsigned long bp, first, count=0;

	if (!where)
		return;

	first = where;
	
	do {
		FETCH(&bp, where+offset);
		where = bp;
		count++;
	} while (bp != first);

	kdb_printf (" %9s list: %5ld buffers\n", kind, count);
}

#define SHOW_BUFS(bh, kind, field) \
do {								    \
	unsigned long offset, bp;				    \
	bp = (unsigned long) (bh);				    \
	offset = ((char *) &((struct buffer_head *)(bp))->field) -  \
		 ((char *) (bp));				    \
	show_transaction_list((bp), kind, offset);		    \
} while (0)

static void show_transaction_full(transaction_t *transaction,
				  unsigned long where,
				  const char *kind)
{
	show_transaction_brief(transaction, where, kind);

	SHOW_BUFS(transaction->t_datalist,        "Data",      b_tnext);
	SHOW_BUFS(transaction->t_buffers,         "Metadata",  b_tnext);
	SHOW_BUFS(transaction->t_forget,          "Forget",    b_tnext);
	SHOW_BUFS(transaction->t_checkpoint_list, "Checkpoint",b_cpnext);
	SHOW_BUFS(transaction->t_iobuf_list,      "Iobuf",     b_tnext);
	SHOW_BUFS(transaction->t_shadow_list,     "Shadow",    b_tnext);
	SHOW_BUFS(transaction->t_log_list,        "Log",       b_tnext);
}

int
kdbm_jfs(int argc, const char **argv, const char **envp, struct pt_regs *regs)
{
	journal_t journal;
	transaction_t transaction;
	unsigned long jaddr, taddr;

	long	offset=0;
	int nextarg;
	int diag;

	if (argc != 1)
		return KDB_ARGCOUNT;

	nextarg = 1;
	diag = kdbgetaddrarg(argc, argv, &nextarg, &jaddr, &offset, NULL,regs);
	if (diag)
		return diag;

	jaddr += offset;

	FETCH(&journal, jaddr);
	show_journal(&journal, jaddr);

	taddr = (unsigned long) journal.j_running_transaction;
	if (taddr) {
		FETCH(&transaction, taddr);
		show_transaction_brief(&transaction, taddr, "Running");
	}

	taddr = (unsigned long) journal.j_committing_transaction;
	if (taddr) {
		FETCH(&transaction, taddr);
		show_transaction_brief(&transaction, taddr, "Committing");
	}

	taddr = (unsigned long) journal.j_checkpoint_transactions;
	if (taddr) do {
		FETCH(&transaction, taddr);
		show_transaction_brief(&transaction, taddr, "Checkpointed");
		taddr = (unsigned long) transaction.t_cpnext;
	} while (taddr != (unsigned long) journal.j_checkpoint_transactions);

	return 0;
}

int
kdbm_tr(int argc, const char **argv, const char **envp, struct pt_regs *regs)
{
	transaction_t transaction;
	unsigned long taddr;

	long	offset=0;
	int nextarg;
	int diag;

	if (argc != 1)
		return KDB_ARGCOUNT;

	nextarg = 1;
	diag = kdbgetaddrarg(argc, argv, &nextarg, &taddr, &offset, NULL,regs);
	if (diag)
		return diag;

	taddr += offset;

	FETCH(&transaction, taddr);
	show_transaction_full(&transaction, taddr, NULL);

	return 0;
}

int
init_module(void)
{
	kdb_register("bh",  kdbm_bh,  "<vaddr>", "Display buffer_head", 0);
	kdb_register("bhi", kdbm_bhi, "", "Summarise buffer_head info", 0);
	kdb_register("jfs", kdbm_jfs, "<vaddr>", "Display journal_t", 0);
	kdb_register("tr",  kdbm_tr,  "<vaddr>", "Display transaction_t", 0);

	return 0;
}

void
cleanup_module(void)
{
	kdb_unregister("bh");
	kdb_unregister("bhi");
	kdb_unregister("jfs");
	kdb_unregister("tr");
}
