/*
 * Impulse - Load Sensitive CPU Frequency Governor
 *
 * Copyright (c) 2014-2015, Pranav Vashi <neobuddy89@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/cpufreq.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/rwsem.h>
#include <linux/sched.h>
#include <linux/sched/rt.h>
#include <linux/time.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <asm/cputime.h>
#ifdef CONFIG_STATE_NOTIFIER
#include <linux/state_notifier.h>
#endif

struct cpufreq_impulse_cpuinfo {
	struct timer_list cpu_timer;
	struct timer_list cpu_slack_timer;
	spinlock_t load_lock; /* protects the next 4 fields */
	u64 time_in_idle;
	u64 time_in_idle_timestamp;
	u64 cputime_speedadj;
	u64 cputime_speedadj_timestamp;
	u64 last_evaluated_jiffy;
	struct cpufreq_policy *policy;
	struct cpufreq_frequency_table *freq_table;
	spinlock_t target_freq_lock; /*protects target freq */
	unsigned int target_freq;
	unsigned int floor_freq;
	unsigned int max_freq;
	unsigned int min_freq;
	u64 floor_validate_time;
	u64 hispeed_validate_time; /* cluster hispeed_validate_time */
	u64 local_hvtime; /* per-cpu hispeed_validate_time */
	u64 max_freq_hyst_start_time;
	struct rw_semaphore enable_sem;
	int governor_enabled;
	struct cpufreq_impulse_tunables *cached_tunables;
};

static DEFINE_PER_CPU(struct cpufreq_impulse_cpuinfo, cpuinfo);

#ifdef CONFIG_STATE_NOTIFIER
static struct notifier_block notif;
#endif
static bool suspended;

/* realtime thread handles frequency scaling */
static struct task_struct *speedchange_task;
static cpumask_t speedchange_cpumask;
static spinlock_t speedchange_cpumask_lock;
static struct mutex gov_lock;

/* Target load.  Lower values result in higher CPU speeds. */
#define DEFAULT_TARGET_LOAD 90
static unsigned int default_target_loads[] = {DEFAULT_TARGET_LOAD};

#define DEFAULT_TIMER_RATE (20 * USEC_PER_MSEC)
#define DEFAULT_ABOVE_HISPEED_DELAY DEFAULT_TIMER_RATE
static unsigned int default_above_hispeed_delay[] = {
	DEFAULT_ABOVE_HISPEED_DELAY };

struct cpufreq_impulse_tunables {
	int usage_count;
	/* Hi speed to bump to from lo speed when load burst (default max) */
	unsigned int hispeed_freq;
	/* Go to hi speed when CPU load at or above this value. */
#define DEFAULT_GO_HISPEED_LOAD 99
	unsigned long go_hispeed_load;
/* Go to lowest speed when CPU load at or below this value. */
#define DEFAULT_GO_LOWSPEED_LOAD 5
	unsigned long go_lowspeed_load;

	/* Target load. Lower values result in higher CPU speeds. */
	spinlock_t target_loads_lock;
	unsigned int *target_loads;
	int ntarget_loads;
	/*
	 * The minimum amount of time to spend at a frequency before we can ramp
	 * down.
	 */
#define DEFAULT_MIN_SAMPLE_TIME (80 * USEC_PER_MSEC)
	unsigned long min_sample_time;
	/*
	 * The sample rate of the timer used to increase frequency
	 */
	unsigned long timer_rate;
	/*
	 * Wait this long before raising speed above hispeed, by default a
	 * single timer interval.
	 */
	spinlock_t above_hispeed_delay_lock;
	unsigned int *above_hispeed_delay;
	int nabove_hispeed_delay;
	/* Non-zero means indefinite speed boost active */
	int boost_val;
	/* Duration of a boot pulse in usecs */
	int boostpulse_duration_val;
	/* End time of boost pulse in ktime converted to usecs */
	u64 boostpulse_endtime;
	bool boosted;
	/*
	 * Max additional time to wait in idle, beyond timer_rate, at speeds
	 * above minimum before wakeup to reduce speed, or -1 if unnecessary.
	 */
#define DEFAULT_TIMER_SLACK (4 * DEFAULT_TIMER_RATE)
	int timer_slack_val;
	bool io_is_busy;

	/*
	 * Whether to align timer windows across all CPUs.
	 */
	bool align_windows;

	/*
	 * Stay at max freq for at least max_freq_hysteresis before dropping
	 * frequency.
	 */
	unsigned int max_freq_hysteresis;

	/* Improves frequency selection for more energy */
	bool powersave_bias;
};

/* For cases where we have single governor instance for system */
static struct cpufreq_impulse_tunables *common_tunables;

static struct attribute_group *get_sysfs_attr(void);

/* Round to starting jiffy of next evaluation window */
static u64 round_to_nw_start(u64 jif,
			     struct cpufreq_impulse_tunables *tunables)
{
	unsigned long step = usecs_to_jiffies(tunables->timer_rate);
	u64 ret;

	if (tunables->align_windows) {
		do_div(jif, step);
		ret = (jif + 1) * step;
	} else {
		ret = jiffies + usecs_to_jiffies(tunables->timer_rate);
	}

	return ret;
}

static void cpufreq_impulse_timer_resched(unsigned long cpu,
					bool slack_only)
{
	struct cpufreq_impulse_cpuinfo *pcpu = &per_cpu(cpuinfo, cpu);
	struct cpufreq_impulse_tunables *tunables =
		pcpu->policy->governor_data;
	u64 expires;
	unsigned long flags;

	spin_lock_irqsave(&pcpu->load_lock, flags);
	expires = round_to_nw_start(pcpu->last_evaluated_jiffy, tunables);

	if (!slack_only) {
		pcpu->time_in_idle =
			get_cpu_idle_time(smp_processor_id(),
					  &pcpu->time_in_idle_timestamp,
					  tunables->io_is_busy);
		pcpu->cputime_speedadj = 0;
		pcpu->cputime_speedadj_timestamp = pcpu->time_in_idle_timestamp;
		del_timer(&pcpu->cpu_timer);
		pcpu->cpu_timer.expires = expires;
		add_timer_on(&pcpu->cpu_timer, cpu);
	}

	if (tunables->timer_slack_val >= 0 &&
	    pcpu->target_freq > pcpu->policy->min) {
		expires += usecs_to_jiffies(tunables->timer_slack_val);
		del_timer(&pcpu->cpu_slack_timer);
		pcpu->cpu_slack_timer.expires = expires;
		add_timer_on(&pcpu->cpu_slack_timer, cpu);
	}

	spin_unlock_irqrestore(&pcpu->load_lock, flags);
}

/* The caller shall take enable_sem write semaphore to avoid any timer race.
 * The cpu_timer and cpu_slack_timer must be deactivated when calling this
 * function.
 */
static void cpufreq_impulse_timer_start(
	struct cpufreq_impulse_tunables *tunables, int cpu)
{
	struct cpufreq_impulse_cpuinfo *pcpu = &per_cpu(cpuinfo, cpu);
	u64 expires = round_to_nw_start(pcpu->last_evaluated_jiffy, tunables);
	unsigned long flags;

	spin_lock_irqsave(&pcpu->load_lock, flags);
	pcpu->cpu_timer.expires = expires;
	add_timer_on(&pcpu->cpu_timer, cpu);
	if (tunables->timer_slack_val >= 0 &&
	    pcpu->target_freq > pcpu->policy->min) {
		expires += usecs_to_jiffies(tunables->timer_slack_val);
		pcpu->cpu_slack_timer.expires = expires;
		add_timer_on(&pcpu->cpu_slack_timer, cpu);
	}

	pcpu->time_in_idle =
		get_cpu_idle_time(cpu, &pcpu->time_in_idle_timestamp,
				  tunables->io_is_busy);
	pcpu->cputime_speedadj = 0;
	pcpu->cputime_speedadj_timestamp = pcpu->time_in_idle_timestamp;
	spin_unlock_irqrestore(&pcpu->load_lock, flags);
}

static unsigned int freq_to_above_hispeed_delay(
	struct cpufreq_impulse_tunables *tunables,
	unsigned int freq)
{
	int i;
	unsigned int ret;
	unsigned long flags;

	spin_lock_irqsave(&tunables->above_hispeed_delay_lock, flags);

	for (i = 0; i < tunables->nabove_hispeed_delay - 1 &&
			freq >= tunables->above_hispeed_delay[i+1]; i += 2)
		;

	ret = tunables->above_hispeed_delay[i];
	spin_unlock_irqrestore(&tunables->above_hispeed_delay_lock, flags);
	return ret;
}

static unsigned int freq_to_targetload(
	struct cpufreq_impulse_tunables *tunables, unsigned int freq)
{
	int i;
	unsigned int ret;
	unsigned long flags;

	spin_lock_irqsave(&tunables->target_loads_lock, flags);

	for (i = 0; i < tunables->ntarget_loads - 1 &&
		    freq >= tunables->target_loads[i+1]; i += 2)
		;

	ret = tunables->target_loads[i];
	spin_unlock_irqrestore(&tunables->target_loads_lock, flags);
	return ret;
}

/*
 * If increasing frequencies never map to a lower target load then
 * choose_freq() will find the minimum frequency that does not exceed its
 * target load given the current load.
 */
static unsigned int choose_freq(struct cpufreq_impulse_cpuinfo *pcpu,
		unsigned int loadadjfreq)
{
	unsigned int freq = pcpu->policy->cur;
	unsigned int prevfreq, freqmin, freqmax;
	unsigned int tl;
	int index;

	freqmin = 0;
	freqmax = UINT_MAX;

	do {
		prevfreq = freq;
		tl = freq_to_targetload(pcpu->policy->governor_data, freq);

		/*
		 * Find the lowest frequency where the computed load is less
		 * than or equal to the target load.
		 */

		if (cpufreq_frequency_table_target(
			    pcpu->policy, pcpu->freq_table, loadadjfreq / tl,
			    CPUFREQ_RELATION_C, &index))
			break;
		freq = pcpu->freq_table[index].frequency;

		if (freq > prevfreq) {
			/* The previous frequency is too low. */
			freqmin = prevfreq;

			if (freq >= freqmax) {
				/*
				 * Find the highest frequency that is less
				 * than freqmax.
				 */
				if (cpufreq_frequency_table_target(
					    pcpu->policy, pcpu->freq_table,
					    freqmax - 1, CPUFREQ_RELATION_H,
					    &index))
					break;
				freq = pcpu->freq_table[index].frequency;

				if (freq == freqmin) {
					/*
					 * The first frequency below freqmax
					 * has already been found to be too
					 * low.  freqmax is the lowest speed
					 * we found that is fast enough.
					 */
					freq = freqmax;
					break;
				}
			}
		} else if (freq < prevfreq) {
			/* The previous frequency is high enough. */
			freqmax = prevfreq;

			if (freq <= freqmin) {
				/*
				 * Find the lowest frequency that is higher
				 * than freqmin.
				 */
				if (cpufreq_frequency_table_target(
					    pcpu->policy, pcpu->freq_table,
					    freqmin + 1, CPUFREQ_RELATION_C,
					    &index))
					break;
				freq = pcpu->freq_table[index].frequency;

				/*
				 * If freqmax is the first frequency above
				 * freqmin then we have already found that
				 * this speed is fast enough.
				 */
				if (freq == freqmax)
					break;
			}
		}

		/* If same frequency chosen as previous then done. */
	} while (freq != prevfreq);

	return freq;
}

static u64 update_load(int cpu)
{
	struct cpufreq_impulse_cpuinfo *pcpu = &per_cpu(cpuinfo, cpu);
	struct cpufreq_impulse_tunables *tunables =
		pcpu->policy->governor_data;
	u64 now;
	u64 now_idle;
	unsigned int delta_idle;
	unsigned int delta_time;
	u64 active_time;

	now_idle = get_cpu_idle_time(cpu, &now, tunables->io_is_busy);
	delta_idle = (unsigned int)(now_idle - pcpu->time_in_idle);
	delta_time = (unsigned int)(now - pcpu->time_in_idle_timestamp);

	if (delta_time <= delta_idle)
		active_time = 0;
	else
		active_time = delta_time - delta_idle;

	pcpu->cputime_speedadj += active_time * pcpu->policy->cur;

	pcpu->time_in_idle = now_idle;
	pcpu->time_in_idle_timestamp = now;
	return now;
}

static void cpufreq_impulse_timer(unsigned long data)
{
	u64 now;
	unsigned int delta_time;
	u64 cputime_speedadj;
	int cpu_load;
	struct cpufreq_impulse_cpuinfo *pcpu =
		&per_cpu(cpuinfo, data);
	struct cpufreq_impulse_tunables *tunables =
		pcpu->policy->governor_data;
	unsigned int new_freq;
	unsigned int loadadjfreq;
	unsigned int index;
	unsigned long flags;
	unsigned int this_hispeed_freq;

	//struct cpufreq_govinfo int_info;

	if (!down_read_trylock(&pcpu->enable_sem))
		return;
	if (!pcpu->governor_enabled)
		goto exit;

	if (cpu_is_offline(data))
		goto exit;

	spin_lock_irqsave(&pcpu->load_lock, flags);
	now = update_load(data);
	delta_time = (unsigned int)(now - pcpu->cputime_speedadj_timestamp);
	cputime_speedadj = pcpu->cputime_speedadj;
	pcpu->last_evaluated_jiffy = get_jiffies_64();
	spin_unlock_irqrestore(&pcpu->load_lock, flags);

	if (WARN_ON_ONCE(!delta_time))
		goto rearm;

	do_div(cputime_speedadj, delta_time);
	loadadjfreq = (unsigned int)cputime_speedadj * 100;

	//int_info.cpu = data;
	//int_info.load = loadadjfreq / pcpu->policy->max;
	//int_info.sampling_rate_us = tunables->timer_rate;
	//atomic_notifier_call_chain(&cpufreq_govinfo_notifier_list,
					//CPUFREQ_LOAD_CHANGE, &int_info);

	spin_lock_irqsave(&pcpu->target_freq_lock, flags);
	cpu_load = loadadjfreq / pcpu->policy->cur;
	tunables->boosted = tunables->boost_val ||
			now < tunables->boostpulse_endtime ||
			cpu_load >= tunables->go_hispeed_load;
	tunables->boosted = tunables->boosted && !suspended;
	this_hispeed_freq = max(tunables->hispeed_freq, pcpu->policy->min);

	if (cpu_load <= tunables->go_lowspeed_load &&
		!tunables->boost_val) {
		tunables->boosted = false;
		new_freq = pcpu->policy->cpuinfo.min_freq;
	} else {
		new_freq = choose_freq(pcpu, loadadjfreq);
	}

	if (tunables->boosted)
		new_freq = max(new_freq, this_hispeed_freq);

	if (pcpu->policy->cur >= this_hispeed_freq &&
	    new_freq > pcpu->policy->cur &&
	    now - pcpu->hispeed_validate_time <
	    freq_to_above_hispeed_delay(tunables, pcpu->policy->cur)) {
		spin_unlock_irqrestore(&pcpu->target_freq_lock, flags);
		goto rearm;
	}

	pcpu->local_hvtime = now;

	if (cpufreq_frequency_table_target(pcpu->policy, pcpu->freq_table,
					   new_freq, CPUFREQ_RELATION_C,
					   &index)) {
		spin_unlock_irqrestore(&pcpu->target_freq_lock, flags);
		goto rearm;
	}

	new_freq = pcpu->freq_table[index].frequency;

	if (new_freq < pcpu->target_freq &&
	    now - pcpu->max_freq_hyst_start_time <
	    tunables->max_freq_hysteresis) {
		spin_unlock_irqrestore(&pcpu->target_freq_lock, flags);
		goto rearm;
	}

	/*
	 * Do not scale below floor_freq unless we have been at or above the
	 * floor frequency for the minimum sample time since last validated.
	 */
	if (new_freq < pcpu->floor_freq) {
		if (now - pcpu->floor_validate_time <
				tunables->min_sample_time) {
			spin_unlock_irqrestore(&pcpu->target_freq_lock, flags);
			goto rearm;
		}
	}

	/*
	 * Update the timestamp for checking whether speed has been held at
	 * or above the selected frequency for a minimum of min_sample_time,
	 * if not boosted to this_hispeed_freq.  If boosted to this_hispeed_freq then we
	 * allow the speed to drop as soon as the boostpulse duration expires
	 * (or the indefinite boost is turned off).
	 */

	if (!tunables->boosted || new_freq > this_hispeed_freq) {
		pcpu->floor_freq = new_freq;
		pcpu->floor_validate_time = now;
	}

	if (new_freq == pcpu->policy->max)
		pcpu->max_freq_hyst_start_time = now;

	if (pcpu->target_freq == new_freq &&
			pcpu->target_freq <= pcpu->policy->cur) {
		spin_unlock_irqrestore(&pcpu->target_freq_lock, flags);
		goto rearm;
	}

	pcpu->target_freq = new_freq;
	spin_unlock_irqrestore(&pcpu->target_freq_lock, flags);
	spin_lock_irqsave(&speedchange_cpumask_lock, flags);
	cpumask_set_cpu(data, &speedchange_cpumask);
	spin_unlock_irqrestore(&speedchange_cpumask_lock, flags);
	wake_up_process(speedchange_task);

rearm:
	if (!timer_pending(&pcpu->cpu_timer))
		cpufreq_impulse_timer_resched(data, false);

exit:
	up_read(&pcpu->enable_sem);
	return;
}

static void cpufreq_impulse_idle_end(void)
{
	struct cpufreq_impulse_cpuinfo *pcpu =
		&per_cpu(cpuinfo, smp_processor_id());

	if (!down_read_trylock(&pcpu->enable_sem))
		return;
	if (!pcpu->governor_enabled) {
		up_read(&pcpu->enable_sem);
		return;
	}

	/* Arm the timer for 1-2 ticks later if not already. */
	if (!timer_pending(&pcpu->cpu_timer)) {
		cpufreq_impulse_timer_resched(smp_processor_id(), false);
	} else if (time_after_eq(jiffies, pcpu->cpu_timer.expires)) {
		del_timer(&pcpu->cpu_timer);
		del_timer(&pcpu->cpu_slack_timer);
		cpufreq_impulse_timer(smp_processor_id());
	}

	up_read(&pcpu->enable_sem);
}

static int cpufreq_impulse_speedchange_task(void *data)
{
	unsigned int cpu;
	cpumask_t tmp_mask;
	unsigned long flags;
	struct cpufreq_impulse_cpuinfo *pcpu;
	struct cpufreq_impulse_tunables *tunables;

	while (1) {
		set_current_state(TASK_INTERRUPTIBLE);
		spin_lock_irqsave(&speedchange_cpumask_lock, flags);

		if (cpumask_empty(&speedchange_cpumask)) {
			spin_unlock_irqrestore(&speedchange_cpumask_lock,
					       flags);
			schedule();

			if (kthread_should_stop())
				break;

			spin_lock_irqsave(&speedchange_cpumask_lock, flags);
		}

		set_current_state(TASK_RUNNING);
		tmp_mask = speedchange_cpumask;
		cpumask_clear(&speedchange_cpumask);
		spin_unlock_irqrestore(&speedchange_cpumask_lock, flags);

		for_each_cpu(cpu, &tmp_mask) {
			unsigned int j;
			unsigned int max_freq = 0;
			struct cpufreq_impulse_cpuinfo *pjcpu;
			u64 hvt = ~0ULL;

			pcpu = &per_cpu(cpuinfo, cpu);
			if (!down_read_trylock(&pcpu->enable_sem))
				continue;
			if (!pcpu->governor_enabled) {
				up_read(&pcpu->enable_sem);
				continue;
			}

			for_each_cpu(j, pcpu->policy->cpus) {
				pjcpu = &per_cpu(cpuinfo, j);

				if (pjcpu->target_freq > max_freq) {
					max_freq = pjcpu->target_freq;
					hvt = pjcpu->local_hvtime;
				} else if (pjcpu->target_freq == max_freq) {
					hvt = min(hvt, pjcpu->local_hvtime);
				}
			}

			if (max_freq != pcpu->policy->cur) {
				tunables = pcpu->policy->governor_data;
				if (tunables->powersave_bias || suspended)
					__cpufreq_driver_target(pcpu->policy,
								max_freq,
								CPUFREQ_RELATION_C);
				else
					__cpufreq_driver_target(pcpu->policy,
								max_freq,
								CPUFREQ_RELATION_H);
				for_each_cpu(j, pcpu->policy->cpus) {
					pjcpu = &per_cpu(cpuinfo, j);
					pjcpu->hispeed_validate_time = hvt;
				}
			}

			up_read(&pcpu->enable_sem);
		}
	}

	return 0;
}

static void cpufreq_impulse_boost(struct cpufreq_impulse_tunables *tunables)
{
	int i;
	int anyboost = 0;
	unsigned long flags[2];
	struct cpufreq_impulse_cpuinfo *pcpu;

	tunables->boosted = true;

	spin_lock_irqsave(&speedchange_cpumask_lock, flags[0]);

	for_each_online_cpu(i) {
		pcpu = &per_cpu(cpuinfo, i);
		if (tunables != pcpu->policy->governor_data)
			continue;

		spin_lock_irqsave(&pcpu->target_freq_lock, flags[1]);
		if (pcpu->target_freq < tunables->hispeed_freq) {
			pcpu->target_freq = tunables->hispeed_freq;
			cpumask_set_cpu(i, &speedchange_cpumask);
			pcpu->hispeed_validate_time =
				ktime_to_us(ktime_get());
			anyboost = 1;
		}

		/*
		 * Set floor freq and (re)start timer for when last
		 * validated.
		 */

		pcpu->floor_freq = tunables->hispeed_freq;
		pcpu->floor_validate_time = ktime_to_us(ktime_get());
		spin_unlock_irqrestore(&pcpu->target_freq_lock, flags[1]);
	}

	spin_unlock_irqrestore(&speedchange_cpumask_lock, flags[0]);

	if (anyboost)
		wake_up_process(speedchange_task);
}

static int cpufreq_impulse_notifier(
	struct notifier_block *nb, unsigned long val, void *data)
{
	struct cpufreq_freqs *freq = data;
	struct cpufreq_impulse_cpuinfo *pcpu;
	int cpu;
	unsigned long flags;

	if (val == CPUFREQ_PRECHANGE) {
		pcpu = &per_cpu(cpuinfo, freq->cpu);
		if (!down_read_trylock(&pcpu->enable_sem))
			return 0;
		if (!pcpu->governor_enabled) {
			up_read(&pcpu->enable_sem);
			return 0;
		}

		for_each_cpu(cpu, pcpu->policy->cpus) {
			struct cpufreq_impulse_cpuinfo *pjcpu =
				&per_cpu(cpuinfo, cpu);
			if (cpu != freq->cpu) {
				if (!down_read_trylock(&pjcpu->enable_sem))
					continue;
				if (!pjcpu->governor_enabled) {
					up_read(&pjcpu->enable_sem);
					continue;
				}
			}
			spin_lock_irqsave(&pjcpu->load_lock, flags);
			update_load(cpu);
			spin_unlock_irqrestore(&pjcpu->load_lock, flags);
			if (cpu != freq->cpu)
				up_read(&pjcpu->enable_sem);
		}

		up_read(&pcpu->enable_sem);
	}
	return 0;
}

static struct notifier_block cpufreq_notifier_block = {
	.notifier_call = cpufreq_impulse_notifier,
};

static unsigned int *get_tokenized_data(const char *buf, int *num_tokens)
{
	const char *cp;
	int i;
	int ntokens = 1;
	unsigned int *tokenized_data;
	int err = -EINVAL;

	cp = buf;
	while ((cp = strpbrk(cp + 1, " :")))
		ntokens++;

	if (!(ntokens & 0x1))
		goto err;

	tokenized_data = kmalloc(ntokens * sizeof(unsigned int), GFP_KERNEL);
	if (!tokenized_data) {
		err = -ENOMEM;
		goto err;
	}

	cp = buf;
	i = 0;
	while (i < ntokens) {
		if (sscanf(cp, "%u", &tokenized_data[i++]) != 1)
			goto err_kfree;

		cp = strpbrk(cp, " :");
		if (!cp)
			break;
		cp++;
	}

	if (i != ntokens)
		goto err_kfree;

	*num_tokens = ntokens;
	return tokenized_data;

err_kfree:
	kfree(tokenized_data);
err:
	return ERR_PTR(err);
}

static ssize_t show_target_loads(
	struct cpufreq_impulse_tunables *tunables,
	char *buf)
{
	int i;
	ssize_t ret = 0;
	unsigned long flags;

	spin_lock_irqsave(&tunables->target_loads_lock, flags);

	for (i = 0; i < tunables->ntarget_loads; i++)
		ret += sprintf(buf + ret, "%u%s", tunables->target_loads[i],
			       i & 0x1 ? ":" : " ");

	sprintf(buf + ret - 1, "\n");
	spin_unlock_irqrestore(&tunables->target_loads_lock, flags);
	return ret;
}

static ssize_t store_target_loads(
	struct cpufreq_impulse_tunables *tunables,
	const char *buf, size_t count)
{
	int ntokens;
	unsigned int *new_target_loads = NULL;
	unsigned long flags;

	new_target_loads = get_tokenized_data(buf, &ntokens);
	if (IS_ERR(new_target_loads))
		return PTR_RET(new_target_loads);

	spin_lock_irqsave(&tunables->target_loads_lock, flags);
	if (tunables->target_loads != default_target_loads)
		kfree(tunables->target_loads);
	tunables->target_loads = new_target_loads;
	tunables->ntarget_loads = ntokens;
	spin_unlock_irqrestore(&tunables->target_loads_lock, flags);
	return count;
}

static ssize_t show_above_hispeed_delay(
	struct cpufreq_impulse_tunables *tunables, char *buf)
{
	int i;
	ssize_t ret = 0;
	unsigned long flags;

	spin_lock_irqsave(&tunables->above_hispeed_delay_lock, flags);

	for (i = 0; i < tunables->nabove_hispeed_delay; i++)
		ret += sprintf(buf + ret, "%u%s",
			       tunables->above_hispeed_delay[i],
			       i & 0x1 ? ":" : " ");

	sprintf(buf + ret - 1, "\n");
	spin_unlock_irqrestore(&tunables->above_hispeed_delay_lock, flags);
	return ret;
}

static ssize_t store_above_hispeed_delay(
	struct cpufreq_impulse_tunables *tunables,
	const char *buf, size_t count)
{
	int ntokens, i;
	unsigned int *new_above_hispeed_delay = NULL;
	unsigned long flags;

	new_above_hispeed_delay = get_tokenized_data(buf, &ntokens);
	if (IS_ERR(new_above_hispeed_delay))
		return PTR_RET(new_above_hispeed_delay);

	/* Make sure frequencies are in ascending order. */
	for (i = 3; i < ntokens; i += 2) {
		if (new_above_hispeed_delay[i] <=
			new_above_hispeed_delay[i - 2]) {
			kfree(new_above_hispeed_delay);
			return -EINVAL;
		}
	}

	spin_lock_irqsave(&tunables->above_hispeed_delay_lock, flags);
	if (tunables->above_hispeed_delay != default_above_hispeed_delay)
		kfree(tunables->above_hispeed_delay);
	tunables->above_hispeed_delay = new_above_hispeed_delay;
	tunables->nabove_hispeed_delay = ntokens;
	spin_unlock_irqrestore(&tunables->above_hispeed_delay_lock, flags);
	return count;

}

static ssize_t show_hispeed_freq(struct cpufreq_impulse_tunables *tunables,
		char *buf)
{
	return sprintf(buf, "%u\n", tunables->hispeed_freq);
}

static ssize_t store_hispeed_freq(struct cpufreq_impulse_tunables *tunables,
		const char *buf, size_t count)
{
	int ret;
	long unsigned int val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	tunables->hispeed_freq = val;
	return count;
}

#define show_store_one(file_name)					\
static ssize_t show_##file_name(					\
	struct cpufreq_impulse_tunables *tunables, char *buf)	\
{									\
	return snprintf(buf, PAGE_SIZE, "%u\n", tunables->file_name);	\
}									\
static ssize_t store_##file_name(					\
		struct cpufreq_impulse_tunables *tunables,		\
		const char *buf, size_t count)				\
{									\
	int ret;							\
	long unsigned int val;						\
									\
	ret = kstrtoul(buf, 0, &val);				\
	if (ret < 0)							\
		return ret;						\
	tunables->file_name = val;					\
	return count;							\
}
show_store_one(max_freq_hysteresis);
show_store_one(align_windows);

static ssize_t show_go_hispeed_load(struct cpufreq_impulse_tunables
		*tunables, char *buf)
{
	return sprintf(buf, "%lu\n", tunables->go_hispeed_load);
}

static ssize_t store_go_hispeed_load(struct cpufreq_impulse_tunables
		*tunables, const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	tunables->go_hispeed_load = val;
	return count;
}

static ssize_t show_go_lowspeed_load(struct cpufreq_impulse_tunables
		*tunables, char *buf)
{
	return sprintf(buf, "%lu\n", tunables->go_lowspeed_load);
}

static ssize_t store_go_lowspeed_load(struct cpufreq_impulse_tunables
		*tunables, const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	tunables->go_lowspeed_load = val;
	return count;
}

static ssize_t show_min_sample_time(struct cpufreq_impulse_tunables
		*tunables, char *buf)
{
	return sprintf(buf, "%lu\n", tunables->min_sample_time);
}

static ssize_t store_min_sample_time(struct cpufreq_impulse_tunables
		*tunables, const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	tunables->min_sample_time = val;
	return count;
}

static ssize_t show_timer_rate(struct cpufreq_impulse_tunables *tunables,
		char *buf)
{
	return sprintf(buf, "%lu\n", tunables->timer_rate);
}

static ssize_t store_timer_rate(struct cpufreq_impulse_tunables *tunables,
		const char *buf, size_t count)
{
	int ret;
	unsigned long val, val_round;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	val_round = jiffies_to_usecs(usecs_to_jiffies(val));
	if (val != val_round)
		pr_warn("timer_rate not aligned to jiffy. Rounded up to %lu\n",
			val_round);

	tunables->timer_rate = val_round;
	return count;
}

static ssize_t show_timer_slack(struct cpufreq_impulse_tunables *tunables,
		char *buf)
{
	return sprintf(buf, "%d\n", tunables->timer_slack_val);
}

static ssize_t store_timer_slack(struct cpufreq_impulse_tunables *tunables,
		const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtol(buf, 10, &val);
	if (ret < 0)
		return ret;

	tunables->timer_slack_val = val;
	return count;
}

static ssize_t show_boost(struct cpufreq_impulse_tunables *tunables,
			  char *buf)
{
	return sprintf(buf, "%d\n", tunables->boost_val);
}

static ssize_t store_boost(struct cpufreq_impulse_tunables *tunables,
			   const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	tunables->boost_val = val;

	if (tunables->boost_val) {
		if (!tunables->boosted)
			cpufreq_impulse_boost(tunables);
	} else {
		tunables->boostpulse_endtime = ktime_to_us(ktime_get());
	}

	return count;
}

static ssize_t store_boostpulse(struct cpufreq_impulse_tunables *tunables,
				const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	tunables->boostpulse_endtime = ktime_to_us(ktime_get()) +
		tunables->boostpulse_duration_val;
	if (!tunables->boosted)
		cpufreq_impulse_boost(tunables);
	return count;
}

static ssize_t show_boostpulse_duration(struct cpufreq_impulse_tunables
		*tunables, char *buf)
{
	return sprintf(buf, "%d\n", tunables->boostpulse_duration_val);
}

static ssize_t store_boostpulse_duration(struct cpufreq_impulse_tunables
		*tunables, const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	tunables->boostpulse_duration_val = val;
	return count;
}

static ssize_t show_io_is_busy(struct cpufreq_impulse_tunables *tunables,
		char *buf)
{
	return sprintf(buf, "%u\n", tunables->io_is_busy);
}

static ssize_t store_io_is_busy(struct cpufreq_impulse_tunables *tunables,
		const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	tunables->io_is_busy = val;
	return count;
}

static ssize_t show_powersave_bias(struct cpufreq_impulse_tunables *tunables,
		char *buf)
{
	return sprintf(buf, "%u\n", tunables->powersave_bias);
}

static ssize_t store_powersave_bias(struct cpufreq_impulse_tunables *tunables,
		const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	tunables->powersave_bias = val;
	return count;
}

/*
 * Create show/store routines
 * - sys: One governor instance for complete SYSTEM
 * - pol: One governor instance per struct cpufreq_policy
 */
#define show_gov_pol_sys(file_name)					\
static ssize_t show_##file_name##_gov_sys				\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	return show_##file_name(common_tunables, buf);			\
}									\
									\
static ssize_t show_##file_name##_gov_pol				\
(struct cpufreq_policy *policy, char *buf)				\
{									\
	return show_##file_name(policy->governor_data, buf);		\
}

#define store_gov_pol_sys(file_name)					\
static ssize_t store_##file_name##_gov_sys				\
(struct kobject *kobj, struct attribute *attr, const char *buf,		\
	size_t count)							\
{									\
	return store_##file_name(common_tunables, buf, count);		\
}									\
									\
static ssize_t store_##file_name##_gov_pol				\
(struct cpufreq_policy *policy, const char *buf, size_t count)		\
{									\
	return store_##file_name(policy->governor_data, buf, count);	\
}

#define show_store_gov_pol_sys(file_name)				\
show_gov_pol_sys(file_name);						\
store_gov_pol_sys(file_name)

show_store_gov_pol_sys(target_loads);
show_store_gov_pol_sys(above_hispeed_delay);
show_store_gov_pol_sys(hispeed_freq);
show_store_gov_pol_sys(go_hispeed_load);
show_store_gov_pol_sys(go_lowspeed_load);
show_store_gov_pol_sys(min_sample_time);
show_store_gov_pol_sys(timer_rate);
show_store_gov_pol_sys(timer_slack);
show_store_gov_pol_sys(boost);
store_gov_pol_sys(boostpulse);
show_store_gov_pol_sys(boostpulse_duration);
show_store_gov_pol_sys(io_is_busy);
show_store_gov_pol_sys(max_freq_hysteresis);
show_store_gov_pol_sys(align_windows);
show_store_gov_pol_sys(powersave_bias);

#define gov_sys_attr_rw(_name)						\
static struct global_attr _name##_gov_sys =				\
__ATTR(_name, 0644, show_##_name##_gov_sys, store_##_name##_gov_sys)

#define gov_pol_attr_rw(_name)						\
static struct freq_attr _name##_gov_pol =				\
__ATTR(_name, 0644, show_##_name##_gov_pol, store_##_name##_gov_pol)

#define gov_sys_pol_attr_rw(_name)					\
	gov_sys_attr_rw(_name);						\
	gov_pol_attr_rw(_name)

gov_sys_pol_attr_rw(target_loads);
gov_sys_pol_attr_rw(above_hispeed_delay);
gov_sys_pol_attr_rw(hispeed_freq);
gov_sys_pol_attr_rw(go_hispeed_load);
gov_sys_pol_attr_rw(go_lowspeed_load);
gov_sys_pol_attr_rw(min_sample_time);
gov_sys_pol_attr_rw(timer_rate);
gov_sys_pol_attr_rw(timer_slack);
gov_sys_pol_attr_rw(boost);
gov_sys_pol_attr_rw(boostpulse_duration);
gov_sys_pol_attr_rw(io_is_busy);
gov_sys_pol_attr_rw(max_freq_hysteresis);
gov_sys_pol_attr_rw(align_windows);
gov_sys_pol_attr_rw(powersave_bias);

static struct global_attr boostpulse_gov_sys =
	__ATTR(boostpulse, 0200, NULL, store_boostpulse_gov_sys);

static struct freq_attr boostpulse_gov_pol =
	__ATTR(boostpulse, 0200, NULL, store_boostpulse_gov_pol);

/* One Governor instance for entire system */
static struct attribute *impulse_attributes_gov_sys[] = {
	&target_loads_gov_sys.attr,
	&above_hispeed_delay_gov_sys.attr,
	&hispeed_freq_gov_sys.attr,
	&go_hispeed_load_gov_sys.attr,
	&go_lowspeed_load_gov_sys.attr,
	&min_sample_time_gov_sys.attr,
	&timer_rate_gov_sys.attr,
	&timer_slack_gov_sys.attr,
	&boost_gov_sys.attr,
	&boostpulse_gov_sys.attr,
	&boostpulse_duration_gov_sys.attr,
	&io_is_busy_gov_sys.attr,
	&max_freq_hysteresis_gov_sys.attr,
	&align_windows_gov_sys.attr,
	&powersave_bias_gov_sys.attr,
	NULL,
};

static struct attribute_group impulse_attr_group_gov_sys = {
	.attrs = impulse_attributes_gov_sys,
	.name = "impulse",
};

/* Per policy governor instance */
static struct attribute *impulse_attributes_gov_pol[] = {
	&target_loads_gov_pol.attr,
	&above_hispeed_delay_gov_pol.attr,
	&hispeed_freq_gov_pol.attr,
	&go_hispeed_load_gov_pol.attr,
	&go_lowspeed_load_gov_pol.attr,
	&min_sample_time_gov_pol.attr,
	&timer_rate_gov_pol.attr,
	&timer_slack_gov_pol.attr,
	&boost_gov_pol.attr,
	&boostpulse_gov_pol.attr,
	&boostpulse_duration_gov_pol.attr,
	&io_is_busy_gov_pol.attr,
	&max_freq_hysteresis_gov_pol.attr,
	&align_windows_gov_pol.attr,
	&powersave_bias_gov_pol.attr,
	NULL,
};

static struct attribute_group impulse_attr_group_gov_pol = {
	.attrs = impulse_attributes_gov_pol,
	.name = "impulse",
};

static struct attribute_group *get_sysfs_attr(void)
{
	if (have_governor_per_policy())
		return &impulse_attr_group_gov_pol;
	else
		return &impulse_attr_group_gov_sys;
}

static int cpufreq_impulse_idle_notifier(struct notifier_block *nb,
					     unsigned long val,
					     void *data)
{
	if (val == IDLE_END)
		cpufreq_impulse_idle_end();

	return 0;
}

static struct notifier_block cpufreq_impulse_idle_nb = {
	.notifier_call = cpufreq_impulse_idle_notifier,
};

static void save_tunables(struct cpufreq_policy *policy,
			  struct cpufreq_impulse_tunables *tunables)
{
	int cpu;
	struct cpufreq_impulse_cpuinfo *pcpu;

	if (have_governor_per_policy())
		cpu = cpumask_first(policy->related_cpus);
	else
		cpu = 0;

	pcpu = &per_cpu(cpuinfo, cpu);
	WARN_ON(pcpu->cached_tunables && pcpu->cached_tunables != tunables);
	pcpu->cached_tunables = tunables;
}

static struct cpufreq_impulse_tunables *alloc_tunable(
					struct cpufreq_policy *policy)
{
	struct cpufreq_impulse_tunables *tunables;

	tunables = kzalloc(sizeof(*tunables), GFP_KERNEL);
	if (!tunables) {
		pr_err("%s: POLICY_INIT: kzalloc failed\n", __func__);
		return ERR_PTR(-ENOMEM);
	}

	tunables->above_hispeed_delay = default_above_hispeed_delay;
	tunables->nabove_hispeed_delay =
		ARRAY_SIZE(default_above_hispeed_delay);
	tunables->go_hispeed_load = DEFAULT_GO_HISPEED_LOAD;
	tunables->go_lowspeed_load = DEFAULT_GO_LOWSPEED_LOAD;
	tunables->target_loads = default_target_loads;
	tunables->ntarget_loads = ARRAY_SIZE(default_target_loads);
	tunables->min_sample_time = DEFAULT_MIN_SAMPLE_TIME;
	tunables->timer_rate = DEFAULT_TIMER_RATE;
	tunables->boostpulse_duration_val = DEFAULT_MIN_SAMPLE_TIME;
	tunables->timer_slack_val = DEFAULT_TIMER_SLACK;

	spin_lock_init(&tunables->target_loads_lock);
	spin_lock_init(&tunables->above_hispeed_delay_lock);

	save_tunables(policy, tunables);
	return tunables;
}

static struct cpufreq_impulse_tunables *restore_tunables(
						struct cpufreq_policy *policy)
{
	int cpu;

	if (have_governor_per_policy())
		cpu = cpumask_first(policy->related_cpus);
	else
		cpu = 0;

	return per_cpu(cpuinfo, cpu).cached_tunables;
}

static int cpufreq_governor_impulse(struct cpufreq_policy *policy,
		unsigned int event)
{
	int rc;
	unsigned int j;
	struct cpufreq_impulse_cpuinfo *pcpu;
	struct cpufreq_frequency_table *freq_table;
	struct cpufreq_impulse_tunables *tunables;
	unsigned long flags;
	unsigned int anyboost;

	if (have_governor_per_policy())
		tunables = policy->governor_data;
	else
		tunables = common_tunables;

	BUG_ON(!tunables && (event != CPUFREQ_GOV_POLICY_INIT));

	switch (event) {
	case CPUFREQ_GOV_POLICY_INIT:
		if (have_governor_per_policy()) {
			WARN_ON(tunables);
		} else if (tunables) {
			tunables->usage_count++;
			policy->governor_data = tunables;
			return 0;
		}

		tunables = restore_tunables(policy);
		if (!tunables) {
			tunables = alloc_tunable(policy);
			if (IS_ERR(tunables))
				return PTR_ERR(tunables);
		}

		tunables->usage_count = 1;
		policy->governor_data = tunables;
		if (!have_governor_per_policy()) {
			WARN_ON(cpufreq_get_global_kobject());
			common_tunables = tunables;
		}

		rc = sysfs_create_group(get_governor_parent_kobj(policy),
				get_sysfs_attr());
		if (rc) {
			kfree(tunables);
			policy->governor_data = NULL;
			if (!have_governor_per_policy()) {
				common_tunables = NULL;
				cpufreq_put_global_kobject();
			}
			return rc;
		}

		if (!policy->governor->initialized) {
			idle_notifier_register(&cpufreq_impulse_idle_nb);
			cpufreq_register_notifier(&cpufreq_notifier_block,
					CPUFREQ_TRANSITION_NOTIFIER);
		}

		break;

	case CPUFREQ_GOV_POLICY_EXIT:
		if (!--tunables->usage_count) {
			if (policy->governor->initialized == 1) {
				cpufreq_unregister_notifier(&cpufreq_notifier_block,
						CPUFREQ_TRANSITION_NOTIFIER);
				idle_notifier_unregister(&cpufreq_impulse_idle_nb);
			}

			sysfs_remove_group(get_governor_parent_kobj(policy),
					get_sysfs_attr());
			if (!have_governor_per_policy())
				cpufreq_put_global_kobject();
			common_tunables = NULL;
		}

		policy->governor_data = NULL;
		break;

	case CPUFREQ_GOV_START:
		mutex_lock(&gov_lock);

		freq_table = cpufreq_frequency_get_table(policy->cpu);
		if (!tunables->hispeed_freq)
			tunables->hispeed_freq = policy->max;

		for_each_cpu(j, policy->cpus) {
			pcpu = &per_cpu(cpuinfo, j);
			pcpu->policy = policy;
			pcpu->target_freq = policy->cur;
			pcpu->freq_table = freq_table;
			pcpu->floor_freq = pcpu->target_freq;
			pcpu->floor_validate_time =
				ktime_to_us(ktime_get());
			pcpu->hispeed_validate_time =
				pcpu->floor_validate_time;
			pcpu->local_hvtime = pcpu->floor_validate_time;
			pcpu->max_freq = policy->max;
			pcpu->min_freq = policy->min;
			down_write(&pcpu->enable_sem);
			del_timer_sync(&pcpu->cpu_timer);
			del_timer_sync(&pcpu->cpu_slack_timer);
			pcpu->last_evaluated_jiffy = get_jiffies_64();
			if (cpu_online(j))
				cpufreq_impulse_timer_start(tunables, j);
			pcpu->governor_enabled = 1;
			up_write(&pcpu->enable_sem);
		}

		mutex_unlock(&gov_lock);
		break;

	case CPUFREQ_GOV_STOP:
		mutex_lock(&gov_lock);
		for_each_cpu(j, policy->cpus) {
			pcpu = &per_cpu(cpuinfo, j);
			down_write(&pcpu->enable_sem);
			pcpu->governor_enabled = 0;
			pcpu->target_freq = 0;
			del_timer_sync(&pcpu->cpu_timer);
			del_timer_sync(&pcpu->cpu_slack_timer);
			up_write(&pcpu->enable_sem);
		}

		mutex_unlock(&gov_lock);
		break;

	case CPUFREQ_GOV_LIMITS:
		if (policy->max < policy->cur)
			__cpufreq_driver_target(policy,
					policy->max, CPUFREQ_RELATION_H);
		else if (policy->min > policy->cur)
			__cpufreq_driver_target(policy,
					policy->min, CPUFREQ_RELATION_C);
		for_each_cpu(j, policy->cpus) {
			pcpu = &per_cpu(cpuinfo, j);

			down_read(&pcpu->enable_sem);
			if (pcpu->governor_enabled == 0) {
				up_read(&pcpu->enable_sem);
				continue;
			}

			spin_lock_irqsave(&pcpu->target_freq_lock, flags);
			if (policy->max < pcpu->target_freq) {
				pcpu->target_freq = policy->max;
			} else if (policy->min >= pcpu->target_freq) {
				pcpu->target_freq = policy->min;
				anyboost = 1;
			}

			spin_unlock_irqrestore(&pcpu->target_freq_lock, flags);

			if (policy->min < pcpu->min_freq)
				cpufreq_impulse_timer_resched(j, true);
			pcpu->min_freq = policy->min;

			up_read(&pcpu->enable_sem);

			/* Reschedule timer only if policy->max is raised.
			 * Delete the timers, else the timer callback may
			 * return without re-arm the timer when failed
			 * acquire the semaphore. This race may cause timer
			 * stopped unexpectedly.
			 */

			if (policy->max > pcpu->max_freq) {
				down_write(&pcpu->enable_sem);
				del_timer_sync(&pcpu->cpu_timer);
				del_timer_sync(&pcpu->cpu_slack_timer);
				cpufreq_impulse_timer_resched(j, false);
				up_write(&pcpu->enable_sem);
			} else if (anyboost) {
				u64 now = ktime_to_us(ktime_get());

				cpumask_set_cpu(j, &speedchange_cpumask);
				pcpu->hispeed_validate_time = now;
				pcpu->floor_freq = policy->min;
				pcpu->floor_validate_time = now;
			}

			pcpu->max_freq = policy->max;
		}
		if (anyboost)
			wake_up_process(speedchange_task);

		break;
	}
	return 0;
}

#ifdef CONFIG_STATE_NOTIFIER
static int state_notifier_callback(struct notifier_block *this,
				unsigned long event, void *data)
{
	switch (event) {
		case STATE_NOTIFIER_ACTIVE:
			suspended = false;
			break;
		case STATE_NOTIFIER_SUSPEND:
			suspended = true;
			break;
		default:
			break;
	}

	return NOTIFY_OK;
}
#endif

#ifndef CONFIG_CPU_FREQ_DEFAULT_GOV_IMPULSE
static
#endif
struct cpufreq_governor cpufreq_gov_impulse = {
	.name = "impulse",
	.governor = cpufreq_governor_impulse,
	.max_transition_latency = 10000000,
	.owner = THIS_MODULE,
};

static void cpufreq_impulse_nop_timer(unsigned long data)
{
}

static int __init cpufreq_impulse_init(void)
{
	unsigned int i;
	struct cpufreq_impulse_cpuinfo *pcpu;
	struct sched_param param = { .sched_priority = MAX_RT_PRIO-1 };

	/* Initalize per-cpu timers */
	for_each_possible_cpu(i) {
		pcpu = &per_cpu(cpuinfo, i);
		init_timer_deferrable(&pcpu->cpu_timer);
		pcpu->cpu_timer.function = cpufreq_impulse_timer;
		pcpu->cpu_timer.data = i;
		init_timer(&pcpu->cpu_slack_timer);
		pcpu->cpu_slack_timer.function = cpufreq_impulse_nop_timer;
		spin_lock_init(&pcpu->load_lock);
		spin_lock_init(&pcpu->target_freq_lock);
		init_rwsem(&pcpu->enable_sem);
	}

	spin_lock_init(&speedchange_cpumask_lock);
	mutex_init(&gov_lock);
	speedchange_task =
		kthread_create(cpufreq_impulse_speedchange_task, NULL,
			       "cfimpulse");
	if (IS_ERR(speedchange_task))
		return PTR_ERR(speedchange_task);

	sched_setscheduler_nocheck(speedchange_task, SCHED_FIFO, &param);
	get_task_struct(speedchange_task);

	/* NB: wake up so the thread does not look hung to the freezer */
	wake_up_process(speedchange_task);

#ifdef CONFIG_STATE_NOTIFIER
	notif.notifier_call = state_notifier_callback;
	if (state_register_client(&notif))
		pr_err("Cannot register State notifier callback for impulse governor.\n");
#endif

	return cpufreq_register_governor(&cpufreq_gov_impulse);
}

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_IMPULSE
fs_initcall(cpufreq_impulse_init);
#else
module_init(cpufreq_impulse_init);
#endif

static void __exit cpufreq_impulse_exit(void)
{
	int cpu;
	struct cpufreq_impulse_cpuinfo *pcpu;

#ifdef CONFIG_STATE_NOTIFIER
	state_unregister_client(&notif);
#endif

	cpufreq_unregister_governor(&cpufreq_gov_impulse);
	kthread_stop(speedchange_task);
	put_task_struct(speedchange_task);

	for_each_possible_cpu(cpu) {
		pcpu = &per_cpu(cpuinfo, cpu);
		kfree(pcpu->cached_tunables);
		pcpu->cached_tunables = NULL;
	}
}

module_exit(cpufreq_impulse_exit);

MODULE_AUTHOR("Pranav Vashi <neobuddy89@gmail.com>");
MODULE_DESCRIPTION("'cpufreq_impulse' - A cpufreq governor for "
	"Latency sensitive workloads");
MODULE_LICENSE("GPLv2");
