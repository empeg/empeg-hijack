#define MODULE
#include <linux/kdb.h>
#include <linux/module.h>
#include <linux/mm.h>

struct __vmflags {
	unsigned long mask;
	char *name;
} vmflags[] = {
	{ VM_READ, "READ" },
	{ VM_WRITE, "WRITE" },
	{ VM_EXEC, "EXEC" },
	{ VM_SHARED, "SHARED" },
	{ VM_MAYREAD, "MAYREAD" },
	{ VM_MAYWRITE, "MAYWRITE" },
	{ VM_MAYEXEC, "MAYEXEC" },
	{ VM_MAYSHARE, "MAYSHARE" },
	{ VM_GROWSDOWN, "GROWSDOWN" },
	{ VM_GROWSUP, "GROWSUP" },
	{ VM_SHM, "SHM" },
	{ VM_DENYWRITE, "DENYWRITE" },
	{ VM_EXECUTABLE, "EXECUTABLE" },
	{ VM_LOCKED, "LOCKED" },
	{ VM_IO , "IO " },
	{ 0, "" }
};

int
kdbm_vm(int argc, const char **argv, const char **envp, struct pt_regs *regs)
{
	struct vm_area_struct vp;
	unsigned char *bp = (unsigned char *)&vp;
	unsigned long addr;
	long	offset=0;
	int nextarg;
	int diag;
	struct __vmflags *tp;
	int i;
	
	if (argc != 1) 
		return KDB_ARGCOUNT;

	nextarg = 1;
	diag = kdbgetaddrarg(argc, argv, &nextarg, &addr, &offset, NULL, regs);
	if (diag) 
		return diag;

	addr += offset;

	for (i=0; i<sizeof(struct vm_area_struct); i++) {
		*bp++ = kdbgetword(addr+i, 1);
	}

	printk("struct vm_area_struct at 0x%lx for %d bytes\n", 
		addr, sizeof(struct vm_area_struct));
	printk("vm_start = 0x%lx   vm_end = 0x%lx\n", vp.vm_start, vp.vm_end);
	printk("page_prot = 0x%x   avl_height = %d    vm_offset = 0x%x\n",
		vp.vm_page_prot, vp.vm_avl_height, vp.vm_offset);
	printk("flags:  ");
	for(tp=vmflags; tp->mask; tp++) {
		if (vp.vm_flags & tp->mask) {
			printk("%s ", tp->name);
		}
	}
	printk("\n");

	return 0;
}

int
init_module(void)
{
	kdb_register("vm", kdbm_vm, "<vaddr>", "Display vm_area_struct", 0);
	
	return 0;
}

void
cleanup_module(void)
{
	kdb_unregister("vm");
}
