// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023-2025 Sultan Alsawaf <sultan@kerneltoast.com>.
 */

/**
 * DOC: Capacity Aware Superset Scheduler (CASS) description
 *
 * The Capacity Aware Superset Scheduler (CASS) optimizes runqueue selection of
 * CFS tasks. By using CPU capacity as a basis for comparing the relative
 * utilization between different CPUs, CASS fairly balances load across CPUs of
 * varying capacities. This results in improved multi-core performance,
 * especially when CPUs are overutilized because CASS doesn't clip a CPU's
 * utilization when it eclipses the CPU's capacity.
 *
 * As a superset of capacity aware scheduling, CASS implements a hierarchy of
 * criteria to determine the better CPU to wake a task upon between CPUs that
 * have the same relative utilization. This way, single-core performance,
 * latency, and cache affinity are all optimized where possible.
 *
 * CASS doesn't feature explicit energy awareness but its basic load balancing
 * principle results in decreased overall energy, often better than what is
 * possible with explicit energy awareness. By fairly balancing load based on
 * relative utilization, all CPUs are kept at their lowest P-state necessary to
 * satisfy the overall load at any given moment.
 */

struct cass_cpu_cand {
	int cpu;
	unsigned int exit_lat;
	unsigned long cap;          /* Actual capacity (with thermal pressure) */
	unsigned long cap_no_therm; /* Original capacity (without thermal pressure) */
	unsigned long cap_orig;     /* Raw capacity from arch_scale */
	unsigned long eff_util;     /* Effective util for overload check */
	unsigned long hard_util;    /* RT + DL + IRQ load */
	unsigned long util;         /* Relative utilization (scaled) */
};

static __always_inline
void cass_cpu_util(struct cass_cpu_cand *c, int this_cpu, bool sync)
{
	struct rq *rq = cpu_rq(c->cpu);
	struct cfs_rq *cfs_rq = &rq->cfs;
	unsigned long est;

	/* Get this CPU's utilization from CFS tasks */
	c->util = READ_ONCE(cfs_rq->avg.util_avg);
	
	if (sched_feat(UTIL_EST)) {
		est = READ_ONCE(cfs_rq->avg.util_est.enqueued);
		if (est > c->util) {
			/* Don't deduct @current's util from estimated util */
			sync = false;
			c->util = est;
		}
	}

	/*
	 * Deduct @current's util from this CPU if this is a sync wake, unless
	 * @current is an RT task; RT tasks don't have per-entity load tracking.
	 */
	if (sync && c->cpu == this_cpu && !rt_task(current))
		c->util -= min(c->util, task_util(current));

	/* * Calculate Hard Load (RT + DL + IRQ).
	 * cpu_util_rt takes (cpu), while dl/irq from patches take (rq).
	 */
	c->hard_util = cpu_util_rt(c->cpu);

	/*
	 * Get the current capacity of this CPU adjusted for thermal
	 * pressure as well as IRQ and RT-task time.
	 */
	c->cap = capacity_of(c->cpu);

	/* * Calculate capacity WITHOUT thermal pressure.
	 * cap_orig (Max) - Hard Load = Capacity if device was cold.
	 */
	c->cap_orig = arch_scale_cpu_capacity(NULL, c->cpu);
	c->cap_no_therm = c->cap_orig - min(c->hard_util, c->cap_orig - 1);
}

/*
 * The margin used when comparing utilization with CPU capacity.
 *
 * (default: ~20%)
 */
#ifndef fits_capacity
#define fits_capacity(cap, max) ((cap) * 1280 < (max) * 1024)
#endif

/* Returns true if @a is a better CPU than @b */
static __always_inline
bool cass_cpu_better(const struct cass_cpu_cand *a,
		     const struct cass_cpu_cand *b, unsigned long p_util,
		     int this_cpu, int prev_cpu, int prev_llc_id, bool sync)
{
#define cass_cmp(a, b) ({ res = (a) - (b); })
#define cass_eq(a, b) ({ res = (a) == (b); })
	long res;

	/* Prefer the CPU that's not overloaded */
	if (cass_cmp(b->eff_util / b->cap, a->eff_util / a->cap))
		goto done;

	/* Prefer the CPU that's less overloaded if they're both overloaded */
	if (b->eff_util > b->cap && a->eff_util > a->cap &&
	    cass_cmp(b->eff_util * SCHED_CAPACITY_SCALE / b->cap,
		     a->eff_util * SCHED_CAPACITY_SCALE / a->cap))
		goto done;

	/* Prefer the CPU that fits the task */
	if (cass_cmp(fits_capacity(p_util, a->cap),
		     fits_capacity(p_util, b->cap)))
		goto done;

	/* Prefer the CPU with lower relative utilization */
	if (cass_cmp(b->util, a->util))
		goto done;

	/* Prefer the CPU that is idle */
	if (cass_cmp(!!a->exit_lat, !!b->exit_lat))
		goto done;

	/* Prefer the current CPU for sync wakes */
	if (sync && (cass_eq(a->cpu, this_cpu) || !cass_cmp(b->cpu, this_cpu)))
		goto done;

	/* Prefer the CPU with higher capacity */
	if (cass_cmp(a->cap, b->cap))
		goto done;

	/* Prefer the CPU with lower idle exit latency */
	if (cass_cmp(b->exit_lat, a->exit_lat))
		goto done;

	/* Prefer the previous CPU */
	if (cass_eq(a->cpu, prev_cpu) || !cass_cmp(b->cpu, prev_cpu))
		goto done;

	/*
	 * Prefer the CPU that shares a cache with the previous CPU.
	 *
	 * prev_llc_id is negative when all CPUs share the same LLC (DynamIQ)
	 * or when sched domains are torn down. In both cases, sd_llc_id cannot
	 * differentiate candidates. So, we skip it.
	 */
	if (prev_llc_id >= 0 &&
	    cass_cmp(per_cpu(sd_llc_id, a->cpu) == prev_llc_id,
		     per_cpu(sd_llc_id, b->cpu) == prev_llc_id))
		goto done;

	/* @a isn't a better CPU than @b. @res must be <=0 to indicate such. */
done:
	/* @a is a better CPU than @b if @res is positive */
	return res > 0;
}

static int cass_best_cpu(struct task_struct *p, int prev_cpu, bool sync, bool rt)
{
	/* Initialize @best such that @best always has a valid CPU at the end */
	struct cass_cpu_cand cands[2], *best = cands;
	int this_cpu = raw_smp_processor_id();
	unsigned long p_util;
	bool has_idle = false;
	int cidx = 0, cpu, prev_llc_id;

	/* Get the utilization for this task (RT has no PELT) */
	p_util = rt ? 0 : task_util_est(p);

	/*
	 * When the LLC spans all CPUs (e.g. DynamIQ), every candidate shares
	 * the cache with prev_cpu and the comparison can never produce a winner.
	 * When sched domains are torn down, sd_llc_id holds stale values. We
	 * identify this by making sd_llc RCU-safe. There is no need of rcu_read_lock()
	 * here since this is a non-preemptible context.
	 *
	 * In such scenarios, set prev_llc_id to -1 so that cass_cpu_better() skips
	 * the check entirely, avoiding unnecessary per_cpu() reads in the hot-path.
	 */
	if (unlikely(!rcu_dereference(per_cpu(sd_llc, prev_cpu))) ||
	    per_cpu(sd_llc_size, prev_cpu) >= nr_cpu_ids)
		prev_llc_id = -1;
	else
		prev_llc_id = per_cpu(sd_llc_id, prev_cpu);

	/*
	 * Find the best CPU to wake @p on. Although idle_get_state() requires
	 * an RCU read lock, an RCU read lock isn't needed because we're not
	 * preemptible and RCU-sched is unified with normal RCU. Therefore,
	 * non-preemptible contexts are implicitly RCU-safe.
	 *
	 * Note: Explicit rcu_read_lock() is omitted here because we are running
	 * in a non-preemptible context (sched path), which implies RCU safety.
	 */
	for_each_cpu_and(cpu, &p->cpus_allowed, cpu_active_mask) {
		/* Use the free candidate slot */
		struct cass_cpu_cand *curr = &cands[cidx];
		struct cpuidle_state *idle_state;
		struct rq *rq = cpu_rq(cpu);

		/*
		 * Check if this CPU is idle.
		 * Uses available_idle_cpu() if supported (standard in most 4.14+ android kernels).
		 * Fallback logic included in sync check.
		 */
		curr->cpu = cpu;
		if ((sync && cpu == this_cpu && rq->nr_running == 1) ||
		    (cpu_online(cpu) && idle_cpu(cpu)))  {
			/*
			 * Discard any previous non-idle candidate.
			 * We removed Prime logic, so simpler idle preference.
			 */
			if (!has_idle)
				best = curr;
			has_idle = true;

			/* Nonzero exit latency indicates this CPU is idle */
			curr->exit_lat = 1;

			/* Add on the actual idle exit latency, if any */
			idle_state = idle_get_state(rq);
			if (idle_state)
				curr->exit_lat += idle_state->exit_latency;
		} else {
			/* Skip non-idle CPUs if there's an idle candidate */
			if (has_idle)
				continue;

			/* Zero exit latency indicates this CPU isn't idle */
			curr->exit_lat = 0;
		}

		/* Get this CPU's utilization and capacity stats */
		cass_cpu_util(curr, this_cpu, sync);

		/*
		 * Add @p's utilization to this CPU if it's not @p's CPU, to
		 * find what this CPU's relative utilization would look like
		 * if @p were on it.
		 */
		if (cpu != task_cpu(p))
			curr->util += p_util;

		/*
		 * Calculate the effective utilization for this CPU candidate;
		 * i.e., the utilization calculated by the CPU governor. This is
		 * needed to evaluate whether or not a throttled CPU is
		 * overloaded.
		 */
		curr->eff_util = curr->util + curr->hard_util;

		/*
		 * Calculate the relative utilization for this CPU candidate.
		 * IMPORTANT: Uses cap_no_therm (capacity without thermal pressure)
		 * to prevent thermal throttling from distorting load balance.
		 */
		curr->util = curr->util * SCHED_CAPACITY_SCALE / curr->cap_no_therm;

		/*
		 * Check if this CPU is better than the best CPU found so far.
		 * If @best == @curr then there's no need to compare them, but
		 * cidx still needs to be changed to the other candidate slot.
		 */
		if (best == curr ||
		    cass_cpu_better(curr, best, p_util, this_cpu, prev_cpu,
				    prev_llc_id, sync)) {
			best = curr;
			cidx ^= 1;
		}
	}

	return best->cpu;
}

static int cass_select_task_rq(struct task_struct *p, int prev_cpu, int sd_flag,
			       int wake_flags, bool rt)
{
	bool sync;

	/* Don't balance on exec since we don't know what @p will look like */
	if (sd_flag & SD_BALANCE_EXEC)
		return prev_cpu;

	/*
	 * If there aren't any valid CPUs which are active, then just return the
	 * first valid CPU since it's possible for certain types of tasks to run
	 * on inactive CPUs.
	 */
	if (unlikely(!cpumask_intersects(&p->cpus_allowed, cpu_active_mask)))
		return cpumask_first(&p->cpus_allowed);

	/* cass_best_cpu() needs the CFS task's utilization, so sync it up */
	if (!rt && !(sd_flag & SD_BALANCE_FORK))
		sync_entity_load_avg(&p->se);

	sync = (wake_flags & WF_SYNC) && !(current->flags & PF_EXITING);
	return cass_best_cpu(p, prev_cpu, sync, rt);
}

static int cass_select_task_rq_fair(struct task_struct *p, int prev_cpu,
				    int sd_flag, int wake_flags,
				    int sibling_count_hint)
{
	return cass_select_task_rq(p, prev_cpu, sd_flag, wake_flags, false);
}

int cass_select_task_rq_rt(struct task_struct *p, int prev_cpu, int sd_flag,
			   int wake_flags, int sibling_count_hint)
{
	return cass_select_task_rq(p, prev_cpu, sd_flag, wake_flags, true);
}
