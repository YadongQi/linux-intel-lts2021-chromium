// SPDX-License-Identifier: GPL-2.0-only
/*
 * linux/drivers/devfreq/governor_passive.c
 *
 * Copyright (C) 2016 Samsung Electronics
 * Author: Chanwoo Choi <cw00.choi@samsung.com>
 * Author: MyungJoo Ham <myungjoo.ham@samsung.com>
 */

#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/devfreq.h>
#include "governor.h"

#define HZ_PER_KHZ	1000

static unsigned long get_taget_freq_by_required_opp(struct device *p_dev,
						struct opp_table *p_opp_table,
						struct opp_table *opp_table,
						unsigned long freq)
{
	struct dev_pm_opp *opp = NULL, *p_opp = NULL;

	if (!p_dev || !p_opp_table || !opp_table || !freq)
		return 0;

	p_opp = devfreq_recommended_opp(p_dev, &freq, 0);
	if (IS_ERR(p_opp))
		return 0;

	opp = dev_pm_opp_xlate_required_opp(p_opp_table, opp_table, p_opp);
	dev_pm_opp_put(p_opp);

	if (IS_ERR(opp))
		return 0;

	freq = dev_pm_opp_get_freq(opp);
	dev_pm_opp_put(opp);

	return freq;
}

static int get_target_freq_with_cpufreq(struct devfreq *devfreq,
					unsigned long *target_freq)
{
	struct devfreq_passive_data *p_data =
				(struct devfreq_passive_data *)devfreq->data;
	struct devfreq_cpu_data *cpudata;
	unsigned long cpu, cpu_cur, cpu_min, cpu_max, cpu_percent;
	unsigned long dev_min, dev_max;
	unsigned long freq = 0;

	for_each_online_cpu(cpu) {
		cpudata = p_data->cpudata[cpu];
		if (!cpudata || cpudata->first_cpu != cpu)
			continue;

		/* Get target freq via required opps */
		cpu_cur = cpudata->cur_freq * HZ_PER_KHZ;
		freq = get_taget_freq_by_required_opp(cpudata->dev,
					cpudata->opp_table,
					devfreq->opp_table, cpu_cur);
		if (freq) {
			*target_freq = max(freq, *target_freq);
			continue;
		}

		/* Use Interpolation if required opps is not available */
		devfreq_get_freq_range(devfreq, &dev_min, &dev_max);

		cpu_min = cpudata->min_freq;
		cpu_max = cpudata->max_freq;
		cpu_cur = cpudata->cur_freq;

		cpu_percent = ((cpu_cur - cpu_min) * 100) / (cpu_max - cpu_min);
		freq = dev_min + mult_frac(dev_max - dev_min, cpu_percent, 100);

		*target_freq = max(freq, *target_freq);
	}

	return 0;
}

static int get_target_freq_with_devfreq(struct devfreq *devfreq,
					unsigned long *freq)
{
	struct devfreq_passive_data *p_data
			= (struct devfreq_passive_data *)devfreq->data;
	struct devfreq *parent_devfreq = (struct devfreq *)p_data->parent;
	unsigned long target_freq;
	int i;

	/* Get target freq via required opps */
	target_freq = get_taget_freq_by_required_opp(parent_devfreq->dev.parent,
						parent_devfreq->opp_table,
						devfreq->opp_table, *freq);
	if (target_freq)
		goto out;

	/* Use Interpolation if required opps is not available */
	for (i = 0; i < parent_devfreq->profile->max_state; i++)
		if (parent_devfreq->profile->freq_table[i] == *freq)
			break;

	if (i == parent_devfreq->profile->max_state)
		return -EINVAL;

	if (i < devfreq->profile->max_state) {
		target_freq = devfreq->profile->freq_table[i];
	} else {
		i = devfreq->profile->max_state;
		target_freq = devfreq->profile->freq_table[i - 1];
	}

out:
	*freq = target_freq;

	return 0;
}

static int devfreq_passive_get_target_freq(struct devfreq *devfreq,
					   unsigned long *freq)
{
	struct devfreq_passive_data *p_data =
				(struct devfreq_passive_data *)devfreq->data;
	int ret;

	if (!p_data)
		return -EINVAL;

	/*
	 * If the devfreq device with passive governor has the specific method
	 * to determine the next frequency, should use the get_target_freq()
	 * of struct devfreq_passive_data.
	 */
	if (p_data->get_target_freq)
		return p_data->get_target_freq(devfreq, freq);

	switch (p_data->parent_type) {
	case DEVFREQ_PARENT_DEV:
		ret = get_target_freq_with_devfreq(devfreq, freq);
		break;
	case CPUFREQ_PARENT_DEV:
		ret = get_target_freq_with_cpufreq(devfreq, freq);
		break;
	default:
		ret = -EINVAL;
		dev_err(&devfreq->dev, "Invalid parent type\n");
		break;
	}

	return ret;
}

static int cpufreq_passive_notifier_call(struct notifier_block *nb,
					 unsigned long event, void *ptr)
{
	struct devfreq_passive_data *data =
			container_of(nb, struct devfreq_passive_data, nb);
	struct devfreq *devfreq = (struct devfreq *)data->this;
	struct devfreq_cpu_data *cpudata;
	struct cpufreq_freqs *freqs = ptr;
	unsigned int cur_freq;
	int ret;

	if (event != CPUFREQ_POSTCHANGE || !freqs ||
		!data->cpudata[freqs->policy->cpu])
		return 0;

	cpudata = data->cpudata[freqs->policy->cpu];
	if (cpudata->cur_freq == freqs->new)
		return 0;

	cur_freq = cpudata->cur_freq;
	cpudata->cur_freq = freqs->new;

	mutex_lock(&devfreq->lock);
	ret = devfreq_update_target(devfreq, freqs->new);
	mutex_unlock(&devfreq->lock);
	if (ret) {
		cpudata->cur_freq = cur_freq;
		dev_err(&devfreq->dev, "failed to update the frequency.\n");
		return ret;
	}

	return 0;
}

static int cpufreq_passive_register_notifier(struct devfreq *devfreq)
{
	struct devfreq_passive_data *p_data
			= (struct devfreq_passive_data *)devfreq->data;
	struct device *dev = devfreq->dev.parent;
	struct opp_table *opp_table = NULL;
	struct devfreq_cpu_data *cpudata;
	struct cpufreq_policy *policy;
	struct device *cpu_dev;
	unsigned int cpu;
	int ret;

	cpus_read_lock();

	p_data->nb.notifier_call = cpufreq_passive_notifier_call;
	ret = cpufreq_register_notifier(&p_data->nb, CPUFREQ_TRANSITION_NOTIFIER);
	if (ret) {
		dev_err(dev, "failed to register cpufreq notifier\n");
		p_data->nb.notifier_call = NULL;
		goto out;
	}

	for_each_online_cpu(cpu) {
		if (p_data->cpudata[cpu])
			continue;

		policy = cpufreq_cpu_get(cpu);
		if (policy) {
			cpudata = kzalloc(sizeof(*cpudata), GFP_KERNEL);
			if (!cpudata) {
				ret = -ENOMEM;
				goto out;
			}

			cpu_dev = get_cpu_device(cpu);
			if (!cpu_dev) {
				dev_err(dev, "failed to get cpu device\n");
				ret = -ENODEV;
				goto out;
			}

			opp_table = dev_pm_opp_get_opp_table(cpu_dev);
			if (IS_ERR(opp_table)) {
				ret = PTR_ERR(opp_table);
				goto out;
			}

			cpudata->dev = cpu_dev;
			cpudata->opp_table = opp_table;
			cpudata->first_cpu = cpumask_first(policy->related_cpus);
			cpudata->cur_freq = policy->cur;
			cpudata->min_freq = policy->cpuinfo.min_freq;
			cpudata->max_freq = policy->cpuinfo.max_freq;

			p_data->cpudata[cpu] = cpudata;
			cpufreq_cpu_put(policy);
		} else {
			ret = -EPROBE_DEFER;
			goto out;
		}
	}
out:
	cpus_read_unlock();
	if (ret)
		return ret;

	mutex_lock(&devfreq->lock);
	ret = devfreq_update_target(devfreq, 0L);
	mutex_unlock(&devfreq->lock);
	if (ret)
		dev_err(dev, "failed to update the frequency\n");

	return ret;
}

static int cpufreq_passive_unregister_notifier(struct devfreq *devfreq)
{
	struct devfreq_passive_data *p_data
			= (struct devfreq_passive_data *)devfreq->data;
	struct devfreq_cpu_data *cpudata;
	int cpu;

	if (p_data->nb.notifier_call)
		cpufreq_unregister_notifier(&p_data->nb, CPUFREQ_TRANSITION_NOTIFIER);

	for_each_possible_cpu(cpu) {
		cpudata = p_data->cpudata[cpu];
		if (cpudata) {
			if (cpudata->opp_table)
				dev_pm_opp_put_opp_table(cpudata->opp_table);
			kfree(cpudata);
			cpudata = NULL;
		}
	}

	return 0;
}

static int devfreq_passive_notifier_call(struct notifier_block *nb,
				unsigned long event, void *ptr)
{
	struct devfreq_passive_data *data
			= container_of(nb, struct devfreq_passive_data, nb);
	struct devfreq *devfreq = (struct devfreq *)data->this;
	struct devfreq *parent = (struct devfreq *)data->parent;
	struct devfreq_freqs *freqs = (struct devfreq_freqs *)ptr;
	unsigned long freq = freqs->new;
	int ret = 0;

	mutex_lock_nested(&devfreq->lock, SINGLE_DEPTH_NESTING);
	switch (event) {
	case DEVFREQ_PRECHANGE:
		if (parent->previous_freq > freq)
			ret = devfreq_update_target(devfreq, freq);

		break;
	case DEVFREQ_POSTCHANGE:
		if (parent->previous_freq < freq)
			ret = devfreq_update_target(devfreq, freq);
		break;
	}
	mutex_unlock(&devfreq->lock);

	if (ret < 0)
		dev_warn(&devfreq->dev,
			"failed to update devfreq using passive governor\n");

	return NOTIFY_DONE;
}

static int devfreq_passive_event_handler(struct devfreq *devfreq,
				unsigned int event, void *data)
{
	struct devfreq_passive_data *p_data
			= (struct devfreq_passive_data *)devfreq->data;
	struct devfreq *parent = (struct devfreq *)p_data->parent;
	struct notifier_block *nb = &p_data->nb;
	int ret = 0;

	if (p_data->parent_type == DEVFREQ_PARENT_DEV && !parent)
		return -EPROBE_DEFER;

	switch (event) {
	case DEVFREQ_GOV_START:
		if (!p_data->this)
			p_data->this = devfreq;

		if (p_data->parent_type == DEVFREQ_PARENT_DEV) {
			nb->notifier_call = devfreq_passive_notifier_call;
			ret = devfreq_register_notifier(parent, nb,
						DEVFREQ_TRANSITION_NOTIFIER);
		} else if (p_data->parent_type == CPUFREQ_PARENT_DEV) {
			ret = cpufreq_passive_register_notifier(devfreq);
		} else {
			ret = -EINVAL;
		}
		break;
	case DEVFREQ_GOV_STOP:
		if (p_data->parent_type == DEVFREQ_PARENT_DEV)
			WARN_ON(devfreq_unregister_notifier(parent, nb,
						DEVFREQ_TRANSITION_NOTIFIER));
		else if (p_data->parent_type == CPUFREQ_PARENT_DEV)
			WARN_ON(cpufreq_passive_unregister_notifier(devfreq));
		else
			ret = -EINVAL;
		break;
	default:
		break;
	}

	return ret;
}

static struct devfreq_governor devfreq_passive = {
	.name = DEVFREQ_GOV_PASSIVE,
	.flags = DEVFREQ_GOV_FLAG_IMMUTABLE,
	.get_target_freq = devfreq_passive_get_target_freq,
	.event_handler = devfreq_passive_event_handler,
};

static int __init devfreq_passive_init(void)
{
	return devfreq_add_governor(&devfreq_passive);
}
subsys_initcall(devfreq_passive_init);

static void __exit devfreq_passive_exit(void)
{
	int ret;

	ret = devfreq_remove_governor(&devfreq_passive);
	if (ret)
		pr_err("%s: failed remove governor %d\n", __func__, ret);
}
module_exit(devfreq_passive_exit);

MODULE_AUTHOR("Chanwoo Choi <cw00.choi@samsung.com>");
MODULE_AUTHOR("MyungJoo Ham <myungjoo.ham@samsung.com>");
MODULE_DESCRIPTION("DEVFREQ Passive governor");
MODULE_LICENSE("GPL v2");
