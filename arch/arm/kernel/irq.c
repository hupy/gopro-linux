/*
 *  linux/arch/arm/kernel/irq.c
 *
 *  Copyright (C) 1992 Linus Torvalds
 *  Modifications for ARM processor Copyright (C) 1995-2000 Russell King.
 *
 *  Support for Dynamic Tick Timer Copyright (C) 2004-2005 Nokia Corporation.
 *  Dynamic Tick Timer written by Tony Lindgren <tony@atomide.com> and
 *  Tuukka Tikkanen <tuukka.tikkanen@elektrobit.com>.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  This file contains the code used by various IRQ handling routines:
 *  asking for different IRQ's should be done through these routines
 *  instead of just grabbing them. Thus setups with different IRQ numbers
 *  shouldn't result in any weird surprises, and installing new handlers
 *  should be easier.
 *
 *  IRQ's are in fact implemented a bit like signal handlers for the kernel.
 *  Naturally it's not a 1:1 relation, but there are similarities.
 */
#include <linux/kernel_stat.h>
#include <linux/module.h>
#include <linux/signal.h>
#include <linux/ioport.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/random.h>
#include <linux/smp.h>
#include <linux/init.h>
#include <linux/seq_file.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <linux/kallsyms.h>
#include <linux/proc_fs.h>
#include <linux/ftrace.h>

#include <asm/system.h>
#include <asm/mach/arch.h>
#include <asm/mach/irq.h>
#include <asm/mach/time.h>

/*
 * No architecture-specific irq_finish function defined in arm/arch/irqs.h.
 */
#ifndef irq_finish
#define irq_finish(irq) do { } while (0)
#endif

unsigned long irq_err_count;

int show_interrupts(struct seq_file *p, void *v)
{
	int i = *(loff_t *) v, cpu;
	struct irq_desc *desc;
	struct irqaction * action;
	unsigned long flags;
	int prec, n;

	for (prec = 3, n = 1000; prec < 10 && n <= nr_irqs; prec++)
		n *= 10;

#ifdef CONFIG_SMP
	if (prec < 4)
		prec = 4;
#endif

	if (i == 0) {
		char cpuname[12];

		seq_printf(p, "%*s ", prec, "");
		for_each_present_cpu(cpu) {
			sprintf(cpuname, "CPU%d", cpu);
			seq_printf(p, " %10s", cpuname);
		}
		seq_putc(p, '\n');
	}

	if (i < nr_irqs) {
		desc = irq_to_desc(i);
		raw_spin_lock_irqsave(&desc->lock, flags);
		action = desc->action;
		if (!action)
			goto unlock;

		seq_printf(p, "%*d: ", prec, i);
		for_each_present_cpu(cpu)
			seq_printf(p, "%10u ", kstat_irqs_cpu(i, cpu));
		seq_printf(p, " %10s", desc->irq_data.chip->name ? : "-");
		seq_printf(p, "  %s", action->name);
		for (action = action->next; action; action = action->next)
			seq_printf(p, ", %s", action->name);

		seq_putc(p, '\n');
unlock:
		raw_spin_unlock_irqrestore(&desc->lock, flags);
	} else if (i == nr_irqs) {
#ifdef CONFIG_FIQ
		show_fiq_list(p, prec);
#endif
#ifdef CONFIG_SMP
		show_ipi_list(p, prec);
#endif
#ifdef CONFIG_LOCAL_TIMERS
		show_local_irqs(p, prec);
#endif
		seq_printf(p, "%*s: %10lu\n", prec, "Err", irq_err_count);
	}
	return 0;
}

/*
 * do_IRQ handles all hardware IRQ's.  Decoded IRQs should not
 * come via this function.  Instead, they should provide their
 * own 'handler'
 */
asmlinkage void __exception_irq_entry
asm_do_IRQ(unsigned int irq, struct pt_regs *regs)
{
	struct pt_regs *old_regs = set_irq_regs(regs);

	irq_enter();

	/*
	 * Some hardware gives randomly wrong interrupts.  Rather
	 * than crashing, do something sensible.
	 */
	if (unlikely(irq >= nr_irqs)) {
		if (printk_ratelimit())
			printk(KERN_WARNING "Bad IRQ%u\n", irq);
		ack_bad_irq(irq);
	} else {
		generic_handle_irq(irq);
	}

	/* AT91 specific workaround */
	irq_finish(irq);

	irq_exit();
	set_irq_regs(old_regs);
}

void set_irq_flags(unsigned int irq, unsigned int iflags)
{
	struct irq_desc *desc;
	unsigned long flags;

	if (irq >= nr_irqs) {
		printk(KERN_ERR "Trying to set irq flags for IRQ%d\n", irq);
		return;
	}

	desc = irq_to_desc(irq);
	raw_spin_lock_irqsave(&desc->lock, flags);
	desc->status |= IRQ_NOREQUEST | IRQ_NOPROBE | IRQ_NOAUTOEN;
	if (iflags & IRQF_VALID)
		desc->status &= ~IRQ_NOREQUEST;
	if (iflags & IRQF_PROBE)
		desc->status &= ~IRQ_NOPROBE;
	if (!(iflags & IRQF_NOAUTOEN))
		desc->status &= ~IRQ_NOAUTOEN;
	raw_spin_unlock_irqrestore(&desc->lock, flags);
}

void __init init_IRQ(void)
{
	machine_desc->init_irq();
}

#ifdef CONFIG_SPARSE_IRQ
int __init arch_probe_nr_irqs(void)
{
	nr_irqs = machine_desc->nr_irqs ? machine_desc->nr_irqs : NR_IRQS;
	return nr_irqs;
}
#endif

#ifdef CONFIG_SMP
void balance_irqs(void)
{
	unsigned int i;
	struct irq_desc *desc;
	unsigned long flags;
	struct irq_data *d;

	local_irq_save(flags);
	for_each_irq_desc(i, desc) {
		if (!irq_can_set_affinity(i))
			continue;
		d = &desc->irq_data;
		raw_spin_lock_irq(&desc->lock);
		if (desc->status & (IRQ_AFFINITY_SET | IRQ_NO_BALANCING)) {
			if (!cpumask_intersects(d->affinity,
				cpu_online_mask)) {
				desc->status &= ~IRQ_AFFINITY_SET;
				goto balance_irqs_setall;
			}
			goto balance_irqs_set;
		}
balance_irqs_setall:
		cpumask_and(d->affinity, cpu_online_mask, irq_default_affinity);
balance_irqs_set:
		d->chip->irq_set_affinity(d, d->affinity, false);
		raw_spin_unlock_irq(&desc->lock);
	}
	local_irq_restore(flags);
}
#endif /* CONFIG_SMP */

#ifdef CONFIG_HOTPLUG_CPU
/*
 * The CPU has been marked offline.  Migrate IRQs off this CPU.  If
 * the affinity settings do not allow other CPUs, force them onto any
 * available CPU.
 */
void migrate_irqs(void)
{
	unsigned int i, cpu = smp_processor_id();
	struct irq_desc *desc;
	cpumask_var_t new_affinity;
	struct irq_data *d;

	if (!zalloc_cpumask_var(&new_affinity, GFP_KERNEL))
		BUG();
	for_each_irq_desc(i, desc) {
		if (!irq_can_set_affinity(i))
			continue;
		d = &desc->irq_data;
		if (cpumask_intersects(cpumask_of(cpu), d->affinity)) {
			pr_debug("IRQ%u: disabled in CPU%u\n", i, cpu);
			cpumask_and(new_affinity, d->affinity, cpu_online_mask);
			if(cpumask_empty(new_affinity)) {
				pr_warn("IRQ%u: no longer affine any CPU, "
					"boardcast to all online!\n", i);
				cpumask_copy(new_affinity, cpu_online_mask);
			}
			raw_spin_lock_irq(&desc->lock);
			cpumask_copy(d->affinity, new_affinity);
			desc->status &= ~IRQ_AFFINITY_SET;
			d->chip->irq_set_affinity(d, new_affinity, false);
			raw_spin_unlock_irq(&desc->lock);
		}
	}
	free_cpumask_var(new_affinity);
}
#endif /* CONFIG_HOTPLUG_CPU */

