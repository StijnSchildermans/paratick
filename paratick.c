#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/interrupt.h>
#include <linux/tick.h>
#include <linux/irq_work.h>
#include <linux/random.h>
#include <linux/kernel_stat.h>
#include <linux/nmi.h>
#include <linux/sched/nohz.h>
#include <linux/sched/clock.h>


#define PARATICK_IRQ_VECTOR 	235
#define PARATICK_NAME 		"paratick"

#define TICK_DO_TIMER_NONE	-1
#define TICK_PERIOD 		4000000

#define PARATICK_INIT_MASK 	0x1
#define PARATICK_IDLE_MASK 	0x2
#define PARATICK_TIMER_MASK 	0x4



struct paratick_data {
	struct hrtimer timer;
	char flags;
	ktime_t last_tick;	
};



static DEFINE_PER_CPU(struct paratick_data, data);
static int paratick_do_timer_cpu __read_mostly = TICK_DO_TIMER_NONE;



static inline bool paratick_get_flag(struct paratick_data* pd, char mask)
{
	return pd->flags & mask;
}


static inline bool paratick_get_init(struct paratick_data* pd)
{
	return paratick_get_flag(pd, PARATICK_INIT_MASK);
}


static inline bool paratick_get_idle(struct paratick_data* pd)
{
	return paratick_get_flag(pd, PARATICK_IDLE_MASK);
}


static inline bool paratick_get_timer(struct paratick_data* pd)
{
	return paratick_get_flag(pd, PARATICK_TIMER_MASK);
}

static inline void paratick_set_flag(struct paratick_data* pd, char mask, bool val)
{
	char* flags = &pd->flags;
	if (val)
		*flags = *flags | mask;
	else
		*flags = *flags & (~mask); 
}


static inline void paratick_set_init(struct paratick_data* pd, bool val)
{
	paratick_set_flag(pd, PARATICK_INIT_MASK, val);
}


static inline void paratick_set_idle(struct paratick_data* pd, bool val)
{
	paratick_set_flag(pd, PARATICK_IDLE_MASK, val);
}


static inline void paratick_set_timer(struct paratick_data* pd, bool val)
{
	paratick_set_flag(pd, PARATICK_TIMER_MASK, val);
}


static inline bool local_timer_softirq_pending(void)
{
	return local_softirq_pending() & BIT(TIMER_SOFTIRQ);
}


static ktime_t paratick_next_event(ktime_t now)
{
        u64 basemono, next_rcu, next_tmr;
	unsigned long basejiff;

	ktime_t deadline, delta, next_tick;

	next_tick = TICK_PERIOD;
	basemono = last_jiffies_update;
	basejiff = jiffies;

	if (rcu_needs_cpu(basemono, &next_rcu) 
	    || arch_needs_cpu() 
	    || irq_work_needs_cpu() 
	    || local_timer_softirq_pending()
	    || unlikely(local_softirq_pending()))
		return next_tick;
	else 
	{
		next_tmr = get_next_timer_interrupt(basejiff, basemono);
		deadline = next_rcu < next_tmr? next_rcu : next_tmr;

		delta = deadline - now;

		if (paratick_do_timer_cpu == TICK_DO_TIMER_NONE)
		{	
			ktime_t max_deadline = timekeeping_max_deferment();
			if (max_deadline < delta)
				return max_deadline;
		}

		if (delta < next_tick)
		{
			timer_clear_idle();
			return next_tick;
		}
		else if (deadline == KTIME_MAX)
			return KTIME_MAX;
		else
			return delta;
	}
}

static void paratick_start_tick(struct paratick_data* pd, ktime_t now, ktime_t delta)
{
	paratick_set_timer(pd, true);
	hrtimer_cancel(&pd->timer);
	hrtimer_set_expires(&pd->timer, now-1);
	hrtimer_forward(&pd->timer, now, delta);
	hrtimer_start_expires(&pd->timer, HRTIMER_MODE_ABS_PINNED_HARD);
}


static void paratick_stop_tick(struct paratick_data* pd)
{
	hrtimer_cancel(&pd->timer);
	paratick_set_timer(pd, false);
}


void paratick_enter_idle(void)

{
	int cpu = smp_processor_id();
	struct paratick_data* pd = this_cpu_ptr(&data);
	paratick_set_idle(pd, true);
	if (paratick_do_timer_cpu == cpu) 
		paratick_do_timer_cpu = TICK_DO_TIMER_NONE;
	sched_clock_idle_sleep_event();
}


void paratick_exit_idle(void)
{
	struct paratick_data* pd = this_cpu_ptr(&data);
	
	local_irq_disable();
	timer_clear_idle();
	paratick_set_idle(pd, false);
	if (paratick_get_timer(pd))
		paratick_stop_tick(pd);
	local_irq_enable();
}


void paratick_start_idle(void)
{
	int cpu = smp_processor_id();
	struct paratick_data* pd = this_cpu_ptr(&data);
	ktime_t now, next_event;
	
	if (paratick_get_init(pd))
	{
		local_irq_disable();

		now = ktime_get();
		next_event = paratick_next_event(now);

		if (next_event < KTIME_MAX)
			paratick_start_tick(pd, now, next_event);
		else if (paratick_get_timer(pd))
			paratick_stop_tick(pd);
		
		if (next_event > TICK_PERIOD)
			nohz_balance_enter_idle(cpu);

		local_irq_enable();
	}
}


void paratick_account_process_ticks(ktime_t now, int user)
{
	struct task_struct* p = current;
	ktime_t* last = &this_cpu_ptr(&data)->last_tick;
	int num_ticks = (now - *last) / TICK_PERIOD;

	while (num_ticks > 0)
	{
		profile_tick(CPU_PROFILING);	
		account_process_tick(p,user);
		num_ticks--;
	}
	*last = now;
}


void paratick_update_process_times(ktime_t now, int user)
{
	paratick_account_process_ticks(now, user);
	run_local_timers();
	rcu_sched_clock_irq(user);
	if (in_irq()) 
		irq_work_tick();
	scheduler_tick();
	if (IS_ENABLED(CONFIG_POSIX_TIMERS)) 
		run_posix_cpu_timers();
	this_cpu_add(net_rand_state.s1, rol32(jiffies, 24) + user);
}


void paratick_irq_enter(void)
{
	struct paratick_data* pd = this_cpu_ptr(&data);
	ktime_t now = ktime_get();
	unsigned long flags;

	if (paratick_get_init(pd) && paratick_get_idle(pd))
	{
		local_irq_save(flags);
		tick_do_update_jiffies64(now);
		local_irq_restore(flags);
	}
	touch_softlockup_watchdog_sched();
}


void paratick_irq_exit(void)
{
	sched_clock_idle_sleep_event();
}


void paratick_paratick(void)
{
	int cpu = smp_processor_id();
	int user = user_mode(get_irq_regs());
	ktime_t now = ktime_get();
	struct paratick_data* pd = this_cpu_ptr(&data);
	
	if (unlikely(paratick_do_timer_cpu == TICK_DO_TIMER_NONE) && !paratick_get_idle(pd))
		paratick_do_timer_cpu = cpu;
        if (paratick_do_timer_cpu == cpu) 
		tick_do_update_jiffies64(now);
	
	paratick_update_process_times(now, user);
}


enum hrtimer_restart paratick_sched_timer(struct hrtimer *timer)
{
        struct paratick_data* pd = this_cpu_ptr(&data); 
	int cpu = smp_processor_id();
        ktime_t now, next_event;
       
	now = ktime_get();
	next_event = paratick_next_event(now);

	if (next_event < KTIME_MAX) 
	{
		hrtimer_forward(timer, now, next_event);
		return HRTIMER_RESTART;
	}
	paratick_set_timer(pd, false);
	nohz_balance_enter_idle(cpu);
	return HRTIMER_NORESTART;
}


void handle_paratick_irq(struct irq_desc* desc)
{
	paratick_paratick();
	ack_APIC_irq();
}


void setup_paratick_timer(void)
{
	struct paratick_data* pd = this_cpu_ptr(&data);
	hrtimer_init(&pd->timer,CLOCK_MONOTONIC,HRTIMER_MODE_ABS_HARD);
	pd->timer.function = paratick_sched_timer;
	paratick_set_init(pd, true);
}


int __init paratick_init(void)
{
	int cpu;
	ktime_t now = ktime_get();
	struct irq_desc* (*descs)[256];
	struct irq_desc* desc = alloc_desc(PARATICK_IRQ_VECTOR,0,0,NULL,NULL);
	desc->handle_irq = handle_paratick_irq;

	for (cpu = 0; cpu < NR_CPUS; cpu++){
		descs = &per_cpu(vector_irq,cpu);
	        (*descs)[PARATICK_IRQ_VECTOR] = desc;
		per_cpu(data,cpu).last_tick = now;
	}
	return 0;
}


void __exit paratick_exit(void)
{

}



module_init(paratick_init);
module_exit(paratick_exit);




