#ifndef __ASM_SMP_H
#define __ASM_SMP_H

#ifdef __SMP__

#include <linux/tasks.h>
#include <asm/init.h>
#include <asm/pal.h>

/* make a multiple of 64-bytes */
struct cpuinfo_alpha {
	unsigned long loops_per_sec;
	unsigned long last_asn;
	unsigned long *pgd_cache;
	unsigned long *pte_cache;
	unsigned long pgtable_cache_sz;
	unsigned long ipi_count;
	unsigned long prof_multiplier;
	unsigned long prof_counter;
  /* start of second 64-bytes */
	unsigned long irq_count;
	unsigned long bh_count;
	unsigned long __pad[6];
} __cacheline_aligned;

extern struct cpuinfo_alpha cpu_data[NR_CPUS];

#define PROC_CHANGE_PENALTY     20

/* Map from cpu id to sequential logical cpu number.  This will only
   not be idempotent when cpus failed to come on-line.  */
extern int cpu_number_map[NR_CPUS];

/* The reverse map from sequential logical cpu number to cpu id.  */
extern int __cpu_logical_map[NR_CPUS];
#define cpu_logical_map(cpu)  __cpu_logical_map[cpu]

/* HACK: Cabrio WHAMI return value is bogus if more than 8 bits used.. :-( */

static __inline__ unsigned char hard_smp_processor_id(void)
{
	register unsigned char __r0 __asm__("$0");
	__asm__ __volatile__(
		"call_pal %1 #whami"
		: "=r"(__r0)
		:"i" (PAL_whami)
		: "$1", "$22", "$23", "$24", "$25");
	return __r0;
}

#define smp_processor_id()	(current->processor)

#endif /* __SMP__ */

#define NO_PROC_ID	(-1)

#endif
