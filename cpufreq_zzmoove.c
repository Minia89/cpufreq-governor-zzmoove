/*
 *  drivers/cpufreq/cpufreq_zzmoove.c
 *
 *  Copyright (C)  2001 Russell King
 *            (C)  2003 Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>.
 *                      Jun Nakajima <jun.nakajima@intel.com>
 *            (C)  2009 Alexander Clouter <alex@digriz.org.uk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/* ZZ Changelog:
 * 
 * Version 0.1 - first release
 *
 * - codebase latest version from midnight kernel (git link here!)
 * - modified frequency tables to match I9300 frequency range
 * - added hotplug functionality with strictly cpu switching
 *
 * Version 0.2 - improved
 *
 * - added "sleep" tuneables to be able to adjust values on idle (screen off) via sysfs.
 * - modified hotplug implementation to be able to tune cpus indepentently (including forced cpu off)
 * - introduced tuneables :
 *
 * sampling_rate_sleep_multiplier -> defaults+sysfs
 * up_threshold_sleep = 90; -> defaults+sysfs
 * down_threshold_sleep = 44; -> defaults+sysfs
 * smooth_up_sleep = 100; -> defaults+sysfs
 * up_threshold_hotplug1 -> defaults+sysfs (cpu1)
 * up_threshold_hotplug2 -> defaults+sysfs (cpu2)
 * up_threshold_hotplug3 -> defaults+sysfs (cpu3)
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/jiffies.h>
#include <linux/kernel_stat.h>
#include <linux/mutex.h>
#include <linux/hrtimer.h>
#include <linux/tick.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/earlysuspend.h>

// smooth up/downscaling via lookup tables
#define MN_SMOOTH 1

// cpu load trigger
#define DEF_SMOOTH_UP (75)

/*
 * dbs is used in this file as a shortform for demandbased switching
 * It helps to keep variable names smaller, simpler
 */

#define DEF_FREQUENCY_UP_THRESHOLD		(70)
// ZZ: up threshold for cpu1 (cpu0 stays allways on)
#define DEF_FREQUENCY_UP_THRESHOLD_HOTPLUG1	(68)
// ZZ: up threshold for cpu2 (cpu0 stays allways on)
#define DEF_FREQUENCY_UP_THRESHOLD_HOTPLUG2	(68)
// ZZ: up threshold for cpu3 (cpu0 stays allways on)
#define DEF_FREQUENCY_UP_THRESHOLD_HOTPLUG3	(68)
#define DEF_FREQUENCY_DOWN_THRESHOLD		(52)
// ZZ: down threshold for cpu1 (cpu0 stays allways on)
#define DEF_FREQUENCY_DOWN_THRESHOLD_HOTPLUG1	(55)
// ZZ: down threshold for cpu2 (cpu0 stays allways on)
#define DEF_FREQUENCY_DOWN_THRESHOLD_HOTPLUG2	(55)
// ZZ: up threshold for cpu3 (cpu0 stays allways on)
#define DEF_FREQUENCY_DOWN_THRESHOLD_HOTPLUG3	(55)

/*
 * The polling frequency of this governor depends on the capability of
 * the processor. Default polling frequency is 1000 times the transition
 * latency of the processor. The governor will work on any processor with
 * transition latency <= 10mS, using appropriate sampling
 * rate.
 * For CPUs with transition latency > 10mS (mostly drivers with CPUFREQ_ETERNAL)
 * this governor will not work.
 * All times here are in uS.
 */
#define MIN_SAMPLING_RATE_RATIO			(2)

static unsigned int min_sampling_rate;

// raise sampling rate to SR*multiplier on blank screen
static unsigned int sampling_rate_awake;
static unsigned int up_threshold_awake;
static unsigned int down_threshold_awake;
static unsigned int smooth_up_awake;
static unsigned int sampling_rate_asleep; //ZZ
static unsigned int up_threshold_asleep; //ZZ
static unsigned int down_threshold_asleep; //ZZ
static unsigned int smooth_up_asleep; //ZZ

#define SAMPLING_RATE_SLEEP_MULTIPLIER (2)

#define LATENCY_MULTIPLIER			(1000)
#define MIN_LATENCY_MULTIPLIER			(100)
#define DEF_SAMPLING_DOWN_FACTOR		(4)
#define MAX_SAMPLING_DOWN_FACTOR		(10)
#define TRANSITION_LATENCY_LIMIT		(10 * 1000 * 1000)

// ZZ: defaults for sleep
#define DEF_UP_THRESHOLD_SLEEP			(90)
#define DEF_DOWN_THRESHOLD_SLEEP		(44)
#define DEF_SMOOTH_UP_SLEEP			(100)

static void do_dbs_timer(struct work_struct *work);

struct cpu_dbs_info_s {
	cputime64_t prev_cpu_idle;
	cputime64_t prev_cpu_wall;
	cputime64_t prev_cpu_nice;
	struct cpufreq_policy *cur_policy;
	struct delayed_work work;
	unsigned int down_skip;
	unsigned int requested_freq;
	int cpu;
	unsigned int enable:1;
	/*
	 * percpu mutex that serializes governor limit change with
	 * do_dbs_timer invocation. We do not want do_dbs_timer to run
	 * when user is changing the governor or limits.
	 */
	struct mutex timer_mutex;
};
static DEFINE_PER_CPU(struct cpu_dbs_info_s, cs_cpu_dbs_info);

static unsigned int dbs_enable;	/* number of CPUs using this policy */

/*
 * dbs_mutex protects dbs_enable in governor start/stop.
 */
static DEFINE_MUTEX(dbs_mutex);

static struct dbs_tuners {
	unsigned int sampling_rate;
	unsigned int sampling_rate_sleep_multiplier; /* ZZ: added tuneable */
	unsigned int sampling_down_factor;
	unsigned int up_threshold;
	unsigned int up_threshold_hotplug1; /* ZZ: added tuneable */
	unsigned int up_threshold_hotplug2; /* ZZ: added tuneable */
	unsigned int up_threshold_hotplug3; /* ZZ: added tuneable */
	unsigned int up_threshold_sleep; /* ZZ: added tuneable */
	unsigned int down_threshold;
	unsigned int down_threshold_hotplug1; /* ZZ: added tuneable */
	unsigned int down_threshold_hotplug2; /* ZZ: added tuneable */
	unsigned int down_threshold_hotplug3; /* ZZ: added tuneable */
	unsigned int down_threshold_sleep; /* ZZ: added tuneable */
	unsigned int ignore_nice;
	unsigned int freq_step;
	unsigned int smooth_up;
	unsigned int smooth_up_sleep; /* ZZ: added tuneable */

} dbs_tuners_ins = {
	.up_threshold = DEF_FREQUENCY_UP_THRESHOLD,
	.up_threshold_hotplug1 = DEF_FREQUENCY_UP_THRESHOLD_HOTPLUG1, /* ZZ: added tuneable */
	.up_threshold_hotplug2 = DEF_FREQUENCY_UP_THRESHOLD_HOTPLUG2, /* ZZ: added tuneable */
	.up_threshold_hotplug3 = DEF_FREQUENCY_UP_THRESHOLD_HOTPLUG3, /* ZZ: added tuneable */
	.up_threshold_sleep = DEF_UP_THRESHOLD_SLEEP, /* ZZ: added tuneable */
	.down_threshold = DEF_FREQUENCY_DOWN_THRESHOLD,
	.down_threshold_hotplug1 = DEF_FREQUENCY_DOWN_THRESHOLD_HOTPLUG1, /* ZZ: added tuneable */
	.down_threshold_hotplug2 = DEF_FREQUENCY_DOWN_THRESHOLD_HOTPLUG2, /* ZZ: added tuneable */
	.down_threshold_hotplug3 = DEF_FREQUENCY_DOWN_THRESHOLD_HOTPLUG3, /* ZZ: added tuneable */
	.down_threshold_sleep = DEF_DOWN_THRESHOLD_SLEEP, /* ZZ: added tuneable */
	.sampling_down_factor = DEF_SAMPLING_DOWN_FACTOR,
	.sampling_rate_sleep_multiplier = SAMPLING_RATE_SLEEP_MULTIPLIER, /* ZZ: added tuneable */
	.ignore_nice = 0,
	.freq_step = 5,
	.smooth_up = DEF_SMOOTH_UP,
	.smooth_up_sleep = DEF_SMOOTH_UP_SLEEP, /* ZZ: added tuneable */
};

/**
 * Smooth scaling conservative governor
 *            
 * This modification makes the governor use two lookup tables holding
 * current, next and previous frequency to directly get a correct
 * target frequency instead of calculating target frequencies with
 * up_threshold and step_up %. The two scaling lookup tables used 
 * contain different scaling steps/frequencies to achieve faster upscaling 
 * on higher CPU load.
 * 
 * CPU load triggering faster upscaling can be adjusted via SYSFS, 
 * VALUE between 1 and 100 (% CPU load):
 * echo VALUE > /sys/devices/system/cpu/cpufreq/conservative/smooth_up                
 */

#define MN_FREQ 0
#define MN_UP 1
#define MN_DOWN 2

/* Table modified for use with Samsung I9300 by ZaneZam November 2012 */
static int mn_freqs[13][3]={
    {1400000,1400000,1300000},
    {1300000,1400000,1200000},
    {1200000,1300000,1100000},
    {1100000,1200000,1000000},
    {1000000,1100000, 900000},
    { 900000,1000000, 800000},
    { 800000, 900000, 700000},
    { 700000, 800000, 600000},
    { 600000, 700000, 400000},
    { 500000, 600000, 300000},
    { 400000, 500000, 200000},
    { 300000, 400000, 200000},
    { 200000, 300000, 200000}
};

/* Table modified for use with Samsung I9300 by ZaneZam November 2012 */
static int mn_freqs_power[13][3]={
    {1400000,1400000,1300000},
    {1300000,1400000,1200000},
    {1200000,1400000,1100000},
    {1100000,1300000,1000000},
    {1000000,1200000, 900000},
    { 900000,1100000, 800000},
    { 800000,1000000, 700000},
    { 700000, 900000, 600000},
    { 600000, 800000, 500000},
    { 500000, 700000, 400000},
    { 400000, 600000, 300000},
    { 300000, 500000, 200000},
    { 200000, 400000, 200000}
};

static int mn_get_next_freq(unsigned int curfreq, unsigned int updown, unsigned int load) {
    int i=0;
    if (load < dbs_tuners_ins.smooth_up)
    {
        for(i = 0; i < 13; i++)
        {
            if(curfreq == mn_freqs[i][MN_FREQ])
                return mn_freqs[i][updown]; // updown 1|2
        }
    }
    else
    {
        for(i = 0; i < 13; i++)
        {
            if(curfreq == mn_freqs_power[i][MN_FREQ])
                return mn_freqs_power[i][updown]; // updown 1|2
        }
    }
    return (curfreq); // not found
    }

static inline cputime64_t get_cpu_idle_time_jiffy(unsigned int cpu,
							cputime64_t *wall)
{
	cputime64_t idle_time;
	cputime64_t cur_wall_time;
	cputime64_t busy_time;

	cur_wall_time = jiffies64_to_cputime64(get_jiffies_64());
	busy_time = cputime64_add(kstat_cpu(cpu).cpustat.user,
			kstat_cpu(cpu).cpustat.system);

	busy_time = cputime64_add(busy_time, kstat_cpu(cpu).cpustat.irq);
	busy_time = cputime64_add(busy_time, kstat_cpu(cpu).cpustat.softirq);
	busy_time = cputime64_add(busy_time, kstat_cpu(cpu).cpustat.steal);
	busy_time = cputime64_add(busy_time, kstat_cpu(cpu).cpustat.nice);

	idle_time = cputime64_sub(cur_wall_time, busy_time);
	if (wall)
		*wall = (cputime64_t)jiffies_to_usecs(cur_wall_time);

	return (cputime64_t)jiffies_to_usecs(idle_time);
}

static inline cputime64_t get_cpu_idle_time(unsigned int cpu, cputime64_t *wall)
{
	u64 idle_time = get_cpu_idle_time_us(cpu, wall);

	if (idle_time == -1ULL)
		return get_cpu_idle_time_jiffy(cpu, wall);

	return idle_time;
}

/* keep track of frequency transitions */
static int
dbs_cpufreq_notifier(struct notifier_block *nb, unsigned long val,
		     void *data)
{
	struct cpufreq_freqs *freq = data;
	struct cpu_dbs_info_s *this_dbs_info = &per_cpu(cs_cpu_dbs_info,
							freq->cpu);

	struct cpufreq_policy *policy;

	if (!this_dbs_info->enable)
		return 0;

	policy = this_dbs_info->cur_policy;

	/*
	 * we only care if our internally tracked freq moves outside
	 * the 'valid' ranges of freqency available to us otherwise
	 * we do not change it
	*/
	if (this_dbs_info->requested_freq > policy->max
			|| this_dbs_info->requested_freq < policy->min)
		this_dbs_info->requested_freq = freq->new;

	return 0;
}

static struct notifier_block dbs_cpufreq_notifier_block = {
	.notifier_call = dbs_cpufreq_notifier
};

/************************** sysfs interface ************************/
static ssize_t show_sampling_rate_min(struct kobject *kobj,
				      struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", min_sampling_rate);
}

define_one_global_ro(sampling_rate_min);

/* cpufreq_zzmoove Governor Tunables */
#define show_one(file_name, object)					\
static ssize_t show_##file_name						\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	return sprintf(buf, "%u\n", dbs_tuners_ins.object);		\
}
show_one(sampling_rate, sampling_rate);
show_one(sampling_rate_sleep_multiplier, sampling_rate_sleep_multiplier); // ZZ: added tuneable
show_one(sampling_down_factor, sampling_down_factor);
show_one(up_threshold, up_threshold);
show_one(up_threshold_sleep, up_threshold_sleep); // ZZ
show_one(up_threshold_hotplug1, up_threshold_hotplug1); // ZZ: added tuneable
show_one(up_threshold_hotplug2, up_threshold_hotplug2); // ZZ: added tuneable
show_one(up_threshold_hotplug3, up_threshold_hotplug3); // ZZ: added tuneable
show_one(down_threshold, down_threshold);
show_one(down_threshold_sleep, down_threshold_sleep); // ZZ
show_one(down_threshold_hotplug1, down_threshold_hotplug1); // ZZ: added tuneable
show_one(down_threshold_hotplug2, down_threshold_hotplug2); // ZZ: added tuneable
show_one(down_threshold_hotplug3, down_threshold_hotplug3); // ZZ: added tuneable
show_one(ignore_nice_load, ignore_nice);
show_one(freq_step, freq_step);
show_one(smooth_up, smooth_up);
show_one(smooth_up_sleep, smooth_up_sleep); // ZZ

static ssize_t store_sampling_down_factor(struct kobject *a,
					  struct attribute *b,
					  const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > MAX_SAMPLING_DOWN_FACTOR || input < 1)
		return -EINVAL;

	dbs_tuners_ins.sampling_down_factor = input;
	return count;
}

static ssize_t store_sampling_rate(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1)
		return -EINVAL;

	dbs_tuners_ins.sampling_rate = max(input, min_sampling_rate);
	return count;
}

// ZZ: added tuneable
static ssize_t store_sampling_rate_sleep_multiplier(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > SAMPLING_RATE_SLEEP_MULTIPLIER || input < 1)
		return -EINVAL;

	dbs_tuners_ins.sampling_rate_sleep_multiplier = input;
	return count;
}

static ssize_t store_up_threshold(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > 100 ||
			input <= dbs_tuners_ins.down_threshold)
		return -EINVAL;

	dbs_tuners_ins.up_threshold = input;
	return count;
}

// ZZ: added tuneble
static ssize_t store_up_threshold_sleep(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > 100 ||
			input <= dbs_tuners_ins.down_threshold_sleep)
		return -EINVAL;

	dbs_tuners_ins.up_threshold_sleep = input;
	return count;
}

// ZZ: added tuneable
static ssize_t store_up_threshold_hotplug1(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > 100 || (input <= dbs_tuners_ins.down_threshold && input != 0))
		return -EINVAL;

	dbs_tuners_ins.up_threshold_hotplug1 = input;
	return count;
}

// ZZ: added tuneable
static ssize_t store_up_threshold_hotplug2(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > 100 || (input <= dbs_tuners_ins.down_threshold && input != 0))
		return -EINVAL;

	dbs_tuners_ins.up_threshold_hotplug2 = input;
	return count;
}

// ZZ: added tuneable
static ssize_t store_up_threshold_hotplug3(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > 100 || (input <= dbs_tuners_ins.down_threshold && input != 0))
		return -EINVAL;

	dbs_tuners_ins.up_threshold_hotplug3 = input;
	return count;
}

static ssize_t store_down_threshold(struct kobject *a, struct attribute *b,
				    const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	/* cannot be lower than 11 otherwise freq will not fall */
	if (ret != 1 || input < 11 || input > 100 ||
			input >= dbs_tuners_ins.up_threshold)
		return -EINVAL;

	dbs_tuners_ins.down_threshold = input;
	return count;
}

// ZZ: added tuneable
static ssize_t store_down_threshold_sleep(struct kobject *a, struct attribute *b,
				    const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	/* cannot be lower than 11 otherwise freq will not fall */
	if (ret != 1 || input < 11 || input > 100 ||
			input >= dbs_tuners_ins.up_threshold_sleep)
		return -EINVAL;

	dbs_tuners_ins.down_threshold_sleep = input;
	return count;
}

// ZZ: added tuneable
static ssize_t store_down_threshold_hotplug1(struct kobject *a, struct attribute *b,
				    const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	/* cannot be lower than 11 otherwise freq will not fall */
	if (ret != 1 || input < 11 || input > 100 ||
			input >= dbs_tuners_ins.up_threshold)
		return -EINVAL;

	dbs_tuners_ins.down_threshold_hotplug1 = input;
	return count;
}

// ZZ: added tuneable
static ssize_t store_down_threshold_hotplug2(struct kobject *a, struct attribute *b,
				    const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	/* cannot be lower than 11 otherwise freq will not fall */
	if (ret != 1 || input < 11 || input > 100 ||
			input >= dbs_tuners_ins.up_threshold)
		return -EINVAL;

	dbs_tuners_ins.down_threshold_hotplug2 = input;
	return count;
}

// ZZ: added tuneable
static ssize_t store_down_threshold_hotplug3(struct kobject *a, struct attribute *b,
				    const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	/* cannot be lower than 11 otherwise freq will not fall */
	if (ret != 1 || input < 11 || input > 100 ||
			input >= dbs_tuners_ins.up_threshold)
		return -EINVAL;

	dbs_tuners_ins.down_threshold_hotplug3 = input;
	return count;
}

static ssize_t store_ignore_nice_load(struct kobject *a, struct attribute *b,
				      const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	unsigned int j;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	if (input > 1)
		input = 1;

	if (input == dbs_tuners_ins.ignore_nice) /* nothing to do */
		return count;

	dbs_tuners_ins.ignore_nice = input;

	/* we need to re-evaluate prev_cpu_idle */
	for_each_online_cpu(j) {
		struct cpu_dbs_info_s *dbs_info;
		dbs_info = &per_cpu(cs_cpu_dbs_info, j);
		dbs_info->prev_cpu_idle = get_cpu_idle_time(j,
						&dbs_info->prev_cpu_wall);
		if (dbs_tuners_ins.ignore_nice)
			dbs_info->prev_cpu_nice = kstat_cpu(j).cpustat.nice;
	}
	return count;
}

static ssize_t store_freq_step(struct kobject *a, struct attribute *b,
			       const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1)
		return -EINVAL;

	if (input > 100)
		input = 100;

	/* no need to test here if freq_step is zero as the user might actually
	 * want this, they would be crazy though :) */
	dbs_tuners_ins.freq_step = input;
	return count;
}

static ssize_t store_smooth_up(struct kobject *a,
					  struct attribute *b,
					  const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > 100 || input < 1)
		return -EINVAL;

	dbs_tuners_ins.smooth_up = input;
	return count;
}

// ZZ: added tuneable
static ssize_t store_smooth_up_sleep(struct kobject *a,
					  struct attribute *b,
					  const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > 100 || input < 1)
		return -EINVAL;

	dbs_tuners_ins.smooth_up_sleep = input;
	return count;
}

define_one_global_rw(sampling_rate);
define_one_global_rw(sampling_rate_sleep_multiplier); // ZZ
define_one_global_rw(sampling_down_factor);
define_one_global_rw(up_threshold);
define_one_global_rw(up_threshold_sleep); //ZZ
define_one_global_rw(up_threshold_hotplug1); //ZZ
define_one_global_rw(up_threshold_hotplug2); //ZZ
define_one_global_rw(up_threshold_hotplug3); // ZZ
define_one_global_rw(down_threshold);
define_one_global_rw(down_threshold_sleep); //ZZ
define_one_global_rw(down_threshold_hotplug1); //ZZ
define_one_global_rw(down_threshold_hotplug2); // ZZ
define_one_global_rw(down_threshold_hotplug3); // ZZ
define_one_global_rw(ignore_nice_load);
define_one_global_rw(freq_step);
define_one_global_rw(smooth_up);
define_one_global_rw(smooth_up_sleep); // ZZ

static struct attribute *dbs_attributes[] = {
	&sampling_rate_min.attr,
	&sampling_rate.attr,
	&sampling_rate_sleep_multiplier.attr, //ZZ
	&sampling_down_factor.attr,
	&up_threshold_hotplug1.attr, //ZZ
	&up_threshold_hotplug2.attr, //ZZ
	&up_threshold_hotplug3.attr, //ZZ
	&down_threshold.attr,
	&down_threshold_sleep.attr, //ZZ
	&down_threshold_hotplug1.attr, //ZZ
	&down_threshold_hotplug2.attr, //ZZ
	&down_threshold_hotplug3.attr, //ZZ
	&ignore_nice_load.attr,
	&freq_step.attr,
	&smooth_up.attr,
	&smooth_up_sleep.attr, // ZZ
	&up_threshold.attr,
	&up_threshold_sleep.attr, //ZZ
	NULL
};

static struct attribute_group dbs_attr_group = {
	.attrs = dbs_attributes,
	.name = "zzmoove",
};

/************************** sysfs end ************************/

static void dbs_check_cpu(struct cpu_dbs_info_s *this_dbs_info)
{
	unsigned int load = 0;
	unsigned int max_load = 0;

	struct cpufreq_policy *policy;
	unsigned int j;

	policy = this_dbs_info->cur_policy;

	/*
	 * Every sampling_rate, we check, if current idle time is less
	 * than 20% (default), then we try to increase frequency
	 * Every sampling_rate*sampling_down_factor, we check, if current
	 * idle time is more than 80%, then we try to decrease frequency
	 *
	 * Any frequency increase takes it to the maximum frequency.
	 * Frequency reduction happens at minimum steps of
	 * 5% (default) of maximum frequency
	 */

	/* Get Absolute Load */
	for_each_cpu(j, policy->cpus) {
		struct cpu_dbs_info_s *j_dbs_info;
		cputime64_t cur_wall_time, cur_idle_time;
		unsigned int idle_time, wall_time;

		j_dbs_info = &per_cpu(cs_cpu_dbs_info, j);

		cur_idle_time = get_cpu_idle_time(j, &cur_wall_time);

		wall_time = (unsigned int) cputime64_sub(cur_wall_time,
				j_dbs_info->prev_cpu_wall);
		j_dbs_info->prev_cpu_wall = cur_wall_time;

		idle_time = (unsigned int) cputime64_sub(cur_idle_time,
				j_dbs_info->prev_cpu_idle);
		j_dbs_info->prev_cpu_idle = cur_idle_time;

		if (dbs_tuners_ins.ignore_nice) {
			cputime64_t cur_nice;
			unsigned long cur_nice_jiffies;

			cur_nice = cputime64_sub(kstat_cpu(j).cpustat.nice,
					 j_dbs_info->prev_cpu_nice);
			/*
			 * Assumption: nice time between sampling periods will
			 * be less than 2^32 jiffies for 32 bit sys
			 */
			cur_nice_jiffies = (unsigned long)
					cputime64_to_jiffies64(cur_nice);

			j_dbs_info->prev_cpu_nice = kstat_cpu(j).cpustat.nice;
			idle_time += jiffies_to_usecs(cur_nice_jiffies);
		}

		if (unlikely(!wall_time || wall_time < idle_time))
			continue;

		load = 100 * (wall_time - idle_time) / wall_time;

		if (load > max_load)
			max_load = load;
	}

	/*
	 * break out if we 'cannot' reduce the speed as the user might
	 * want freq_step to be zero
	 */
	if (dbs_tuners_ins.freq_step == 0)
		return;

	/*
	 * Check for frequency increase is greater than hotplug value and wake up cpus accordingly
	 * Following will bring up 3 cores in a row (cpu0 is always on!) - not optimal but it works for now
	 * Modification by ZaneZam November 2012
	 * v0.2 - changed logic to be able to tune threshold per cpu and to be able to switch off cpu's via sysfs
	 */
	
	if (num_online_cpus() < 2) {
			if (dbs_tuners_ins.up_threshold_hotplug1 != 0 || dbs_tuners_ins.up_threshold_hotplug2 != 0 || dbs_tuners_ins.up_threshold_hotplug3 != 0) { /* don't mutex if no cpu is enabled */
			mutex_unlock(&this_dbs_info->timer_mutex); /* this seems to be a very good idea, without it lockups are possible! */
			}
			if (dbs_tuners_ins.up_threshold_hotplug1 != 0 && max_load > dbs_tuners_ins.up_threshold_hotplug1) {
			cpu_up(1);
			}
			if (dbs_tuners_ins.up_threshold_hotplug2 != 0 && max_load > dbs_tuners_ins.up_threshold_hotplug2) {
			cpu_up(2);
			}
			if (dbs_tuners_ins.up_threshold_hotplug3 != 0 && max_load > dbs_tuners_ins.up_threshold_hotplug3) {
			cpu_up(3);
			}
			if (dbs_tuners_ins.up_threshold_hotplug1 != 0 || dbs_tuners_ins.up_threshold_hotplug2 != 0 || dbs_tuners_ins.up_threshold_hotplug3 != 0) { /* don't mutex if no cpu is enabled */
			mutex_lock(&this_dbs_info->timer_mutex); /* this seems to be a very good idea, without it lockups are possible! */
			}
	} else if (num_online_cpus() < 3) {
			if (dbs_tuners_ins.up_threshold_hotplug2 != 0 || dbs_tuners_ins.up_threshold_hotplug3 != 0) { /* don't mutex if no cpu is enabled */
			mutex_unlock(&this_dbs_info->timer_mutex);
			}
			if (dbs_tuners_ins.up_threshold_hotplug2 != 0 && max_load > dbs_tuners_ins.up_threshold_hotplug2) {
			cpu_up(2);
			}
			if (dbs_tuners_ins.up_threshold_hotplug3 != 0 && max_load > dbs_tuners_ins.up_threshold_hotplug3) {
			cpu_up(3);
			}
			if (dbs_tuners_ins.up_threshold_hotplug2 != 0 || dbs_tuners_ins.up_threshold_hotplug3 != 0) { /* don't mutex if no cpu is enabled */
			mutex_lock(&this_dbs_info->timer_mutex);
			}
	} else if (num_online_cpus() < 4) {
			if (dbs_tuners_ins.up_threshold_hotplug3 != 0) { /* don't mutex if no cpu is enabled */
			mutex_unlock(&this_dbs_info->timer_mutex);
			}
			if (dbs_tuners_ins.up_threshold_hotplug3 != 0 && max_load > dbs_tuners_ins.up_threshold_hotplug3) {
			cpu_up(3);
			}
			if (dbs_tuners_ins.up_threshold_hotplug3 != 0) { /* don't mutex if no cpu is enabled */
			mutex_lock(&this_dbs_info->timer_mutex);
			}
	}
	
	/* Check for frequency increase */
	if (max_load > dbs_tuners_ins.up_threshold) {
		this_dbs_info->down_skip = 0;

		/* if we are already at full speed then break out early */
		if (this_dbs_info->requested_freq == policy->max)
			return;

        this_dbs_info->requested_freq = mn_get_next_freq(policy->cur, MN_UP, max_load);

		__cpufreq_driver_target(policy, this_dbs_info->requested_freq,
			CPUFREQ_RELATION_H);
		return;
	}

	/*
	 * Check for frequency decrease is lower than hotplug value and put cpus to sleep accordingly
	 * Following will disable 3 cores in a row (cpu0 is always on!) - not optimal but it works for now
	 * Modification by ZaneZam November 2012
	 * v0.2 - changed logic to be able to tune threshold  per cpu and to be able to switch off cpu's via sysfs
	 */
	
	if (num_online_cpus() > 3) {
			mutex_unlock(&this_dbs_info->timer_mutex); /* this seems to be a very good idea, without it lockups are possible */
			if (max_load < dbs_tuners_ins.down_threshold_hotplug3) {
			cpu_down(3);
			}
			if (max_load < dbs_tuners_ins.down_threshold_hotplug2) {
			cpu_down(2);
			}
			if (max_load < dbs_tuners_ins.down_threshold_hotplug1) {
			cpu_down(1);
			}
			mutex_lock(&this_dbs_info->timer_mutex); /* this seems to be a very good idea, without it lockups are possible */
	} else if (num_online_cpus() > 2) {
			mutex_unlock(&this_dbs_info->timer_mutex);
			if (max_load < dbs_tuners_ins.down_threshold_hotplug2) {
			cpu_down(2);
			}
			if (max_load < dbs_tuners_ins.down_threshold_hotplug1) {
			cpu_down(1);
			}
			mutex_lock(&this_dbs_info->timer_mutex);
	} else if (num_online_cpus() > 1) {
			mutex_unlock(&this_dbs_info->timer_mutex);
			if (max_load < dbs_tuners_ins.down_threshold_hotplug1) {
			cpu_down(1);
			}
			mutex_lock(&this_dbs_info->timer_mutex);
	}

	/*
	 * The optimal frequency is the frequency that is the lowest that
	 * can support the current CPU usage without triggering the up
	 * policy. To be safe, we focus 10 points under the threshold.
	 */
	if (max_load < (dbs_tuners_ins.down_threshold - 10)) {

		/*
		 * if we cannot reduce the frequency anymore, break out early
		 */
		if (policy->cur == policy->min)
			return;

        this_dbs_info->requested_freq = mn_get_next_freq(policy->cur, MN_DOWN, max_load);

		__cpufreq_driver_target(policy, this_dbs_info->requested_freq,
				CPUFREQ_RELATION_H);
		return;
	}
}

static void do_dbs_timer(struct work_struct *work)
{
	struct cpu_dbs_info_s *dbs_info =
		container_of(work, struct cpu_dbs_info_s, work.work);
	unsigned int cpu = dbs_info->cpu;

	/* We want all CPUs to do sampling nearly on same jiffy */
	int delay = usecs_to_jiffies(dbs_tuners_ins.sampling_rate);

	delay -= jiffies % delay;

	mutex_lock(&dbs_info->timer_mutex);

	dbs_check_cpu(dbs_info);

	schedule_delayed_work_on(cpu, &dbs_info->work, delay);
	mutex_unlock(&dbs_info->timer_mutex);
}

static inline void dbs_timer_init(struct cpu_dbs_info_s *dbs_info)
{
	/* We want all CPUs to do sampling nearly on same jiffy */
	int delay = usecs_to_jiffies(dbs_tuners_ins.sampling_rate);
	delay -= jiffies % delay;

	dbs_info->enable = 1;
	INIT_DELAYED_WORK_DEFERRABLE(&dbs_info->work, do_dbs_timer);
	schedule_delayed_work_on(dbs_info->cpu, &dbs_info->work, delay);
}

static inline void dbs_timer_exit(struct cpu_dbs_info_s *dbs_info)
{
	dbs_info->enable = 0;
	cancel_delayed_work_sync(&dbs_info->work);
}

static void powersave_early_suspend(struct early_suspend *handler)
{
  mutex_lock(&dbs_mutex);
  sampling_rate_awake = dbs_tuners_ins.sampling_rate;
  up_threshold_awake = dbs_tuners_ins.up_threshold;
  down_threshold_awake = dbs_tuners_ins.down_threshold;
  smooth_up_awake = dbs_tuners_ins.smooth_up;
  sampling_rate_asleep = dbs_tuners_ins.sampling_rate_sleep_multiplier; //ZZ
  up_threshold_asleep = dbs_tuners_ins.up_threshold_sleep; // ZZ
  down_threshold_asleep = dbs_tuners_ins.down_threshold_sleep; // ZZ
  smooth_up_asleep = dbs_tuners_ins.smooth_up_sleep; // ZZ
  dbs_tuners_ins.sampling_rate *= sampling_rate_asleep; // ZZ
  dbs_tuners_ins.up_threshold = up_threshold_asleep; // ZZ
  dbs_tuners_ins.down_threshold = down_threshold_asleep; // ZZ
  dbs_tuners_ins.smooth_up = smooth_up_asleep; // ZZ
  mutex_unlock(&dbs_mutex);
}

static void powersave_late_resume(struct early_suspend *handler)
{
  mutex_lock(&dbs_mutex);
  dbs_tuners_ins.sampling_rate = sampling_rate_awake;
  dbs_tuners_ins.up_threshold = up_threshold_awake;
  dbs_tuners_ins.down_threshold = down_threshold_awake;
  dbs_tuners_ins.smooth_up = smooth_up_awake;
  mutex_unlock(&dbs_mutex);
}

static struct early_suspend _powersave_early_suspend = {
  .suspend = powersave_early_suspend,
  .resume = powersave_late_resume,
  .level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN,
};

static int cpufreq_governor_dbs(struct cpufreq_policy *policy,
				   unsigned int event)
{
	unsigned int cpu = policy->cpu;
	struct cpu_dbs_info_s *this_dbs_info;
	unsigned int j;
	int rc;

	this_dbs_info = &per_cpu(cs_cpu_dbs_info, cpu);

	switch (event) {
	case CPUFREQ_GOV_START:
		if ((!cpu_online(cpu)) || (!policy->cur))
			return -EINVAL;

		mutex_lock(&dbs_mutex);

		for_each_cpu(j, policy->cpus) {
			struct cpu_dbs_info_s *j_dbs_info;
			j_dbs_info = &per_cpu(cs_cpu_dbs_info, j);
			j_dbs_info->cur_policy = policy;

			j_dbs_info->prev_cpu_idle = get_cpu_idle_time(j,
						&j_dbs_info->prev_cpu_wall);
			if (dbs_tuners_ins.ignore_nice) {
				j_dbs_info->prev_cpu_nice =
						kstat_cpu(j).cpustat.nice;
			}
		}
		this_dbs_info->down_skip = 0;
		this_dbs_info->requested_freq = policy->cur;

		mutex_init(&this_dbs_info->timer_mutex);
		dbs_enable++;
		/*
		 * Start the timerschedule work, when this governor
		 * is used for first time
		 */
		if (dbs_enable == 1) {
			unsigned int latency;
			/* policy latency is in nS. Convert it to uS first */
			latency = policy->cpuinfo.transition_latency / 1000;
			if (latency == 0)
				latency = 1;

			rc = sysfs_create_group(cpufreq_global_kobject,
						&dbs_attr_group);
			if (rc) {
				mutex_unlock(&dbs_mutex);
				return rc;
			}

			/*
			 * conservative does not implement micro like ondemand
			 * governor, thus we are bound to jiffes/HZ
			 */
			min_sampling_rate =
				MIN_SAMPLING_RATE_RATIO * jiffies_to_usecs(3);
			/* Bring kernel and HW constraints together */
			min_sampling_rate = max(min_sampling_rate,
					MIN_LATENCY_MULTIPLIER * latency);
			dbs_tuners_ins.sampling_rate =
				max(min_sampling_rate,
				    latency * LATENCY_MULTIPLIER);
			sampling_rate_awake = dbs_tuners_ins.sampling_rate;
			up_threshold_awake = dbs_tuners_ins.up_threshold;
			down_threshold_awake = dbs_tuners_ins.down_threshold;
			smooth_up_awake = dbs_tuners_ins.smooth_up;
			cpufreq_register_notifier(
					&dbs_cpufreq_notifier_block,
					CPUFREQ_TRANSITION_NOTIFIER);
		}
		mutex_unlock(&dbs_mutex);

		dbs_timer_init(this_dbs_info);
        register_early_suspend(&_powersave_early_suspend);
		break;

	case CPUFREQ_GOV_STOP:
		dbs_timer_exit(this_dbs_info);

		mutex_lock(&dbs_mutex);
		dbs_enable--;
		mutex_destroy(&this_dbs_info->timer_mutex);

		/*
		 * Stop the timerschedule work, when this governor
		 * is used for first time
		 */
		if (dbs_enable == 0)
			cpufreq_unregister_notifier(
					&dbs_cpufreq_notifier_block,
					CPUFREQ_TRANSITION_NOTIFIER);

		mutex_unlock(&dbs_mutex);
		if (!dbs_enable)
			sysfs_remove_group(cpufreq_global_kobject,
					   &dbs_attr_group);

        unregister_early_suspend(&_powersave_early_suspend);
		break;

	case CPUFREQ_GOV_LIMITS:
		mutex_lock(&this_dbs_info->timer_mutex);
		if (policy->max < this_dbs_info->cur_policy->cur)
			__cpufreq_driver_target(
					this_dbs_info->cur_policy,
					policy->max, CPUFREQ_RELATION_H);
		else if (policy->min > this_dbs_info->cur_policy->cur)
			__cpufreq_driver_target(
					this_dbs_info->cur_policy,
					policy->min, CPUFREQ_RELATION_L);
		mutex_unlock(&this_dbs_info->timer_mutex);

		break;
	}
	return 0;
}

#ifndef CONFIG_CPU_FREQ_DEFAULT_GOV_ZZMOOVE
static
#endif
struct cpufreq_governor cpufreq_gov_zzmoove = {
	.name			= "zzmoove",
	.governor		= cpufreq_governor_dbs,
	.max_transition_latency	= TRANSITION_LATENCY_LIMIT,
	.owner			= THIS_MODULE,
};

static int __init cpufreq_gov_dbs_init(void)
{
	return cpufreq_register_governor(&cpufreq_gov_zzmoove);
}

static void __exit cpufreq_gov_dbs_exit(void)
{
	cpufreq_unregister_governor(&cpufreq_gov_zzmoove);
}

// zzmoove is based on the modified conservative (original author
// Alexander Clouter <alex@digriz.org.uk>) smoove governor from Michael Weingaertner <mialwe@googlemail.com>
// (source: https://github.com/mialwe/mngb/)
// Modified by Zane Zaminsky November 2012 to be hotplug-able and optimzed for use with Samsung I9300.
// CPU Hotplug modifications partially taken from ktoonservative governor from
// ktoonsez KT747-JB kernel (https://github.com/ktoonsez/KT747-JB)

MODULE_AUTHOR("Zane Zaminsky <cyxman@yahoo.com>");
MODULE_DESCRIPTION("'cpufreq_zzmoove' - A dynamic cpufreq governor based "
		"on smoove governor from Michael Weingaertner which was originally based on "
		"cpufreq_conservative from Alexander Clouter. Optimized for use with Samsung I9300 "
		"using frequency lookup tables and CPU hotplug - ported/modified for I9300 "
		"by ZaneZam November 2012");
MODULE_LICENSE("GPL");

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_ZZMOOVE
fs_initcall(cpufreq_gov_dbs_init);
#else
module_init(cpufreq_gov_dbs_init);
#endif
module_exit(cpufreq_gov_dbs_exit);