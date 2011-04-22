/*
 * Deadline Scheduling Class (SCHED_DEADLINE)
 *
 * Earliest Deadline First (EDF) + Constant Bandwidth Server (CBS).
 *
 * Tasks that periodically executes their instances for less than their
 * runtime won't miss any of their deadlines.
 * Tasks that are not periodic or sporadic or that tries to execute more
 * than their reserved bandwidth will be slowed down (and may potentially
 * miss some of their deadlines), and won't affect any other task.
 *
 * Copyright (C) 2010 Dario Faggioli <raistlin@linux.it>,
 *                    Juri Lelli <juri.lelli@gmail.com>,
 *                    Michael Trimarchi <trimarchimichael@yahoo.it>,
 *                    Fabio Checconi <fabio@gandalf.sssup.it>
 */

static const struct sched_class dl_sched_class;

static inline int dl_time_before(u64 a, u64 b)
{
	return (s64)(a - b) < 0;
}

/*
 * Tells if entity @a should preempt entity @b.
 */
static inline
int dl_entity_preempt(struct sched_dl_entity *a, struct sched_dl_entity *b)
{
	/*
	 * A system task marked with SF_HEAD flag will always
	 * preempt a non 'special' one.
	 */
	return a->flags & SF_HEAD ||
	       (!(b->flags & SF_HEAD) &&
		dl_time_before(a->deadline, b->deadline));
}

static inline struct task_struct *dl_task_of(struct sched_dl_entity *dl_se)
{
	return container_of(dl_se, struct task_struct, dl);
}

static inline struct rq *rq_of_dl_rq(struct dl_rq *dl_rq)
{
	return container_of(dl_rq, struct rq, dl);
}

static inline struct dl_rq *dl_rq_of_se(struct sched_dl_entity *dl_se)
{
	struct task_struct *p = dl_task_of(dl_se);
	struct rq *rq = task_rq(p);

	return &rq->dl;
}

#ifdef CONFIG_SMP

static inline int dl_overloaded(struct rq *rq)
{
	return atomic_read(&rq->rd->dlo_count);
}

static inline void dl_set_overload(struct rq *rq)
{
	if (!rq->online)
		return;

	cpumask_set_cpu(rq->cpu, rq->rd->dlo_mask);
	/*
	 * Must be visible before the overload count is
	 * set (as in sched_rt.c).
	 */
	wmb();
	atomic_inc(&rq->rd->dlo_count);
}

static inline void dl_clear_overload(struct rq *rq)
{
	if (!rq->online)
		return;

	atomic_dec(&rq->rd->dlo_count);
	cpumask_clear_cpu(rq->cpu, rq->rd->dlo_mask);
}

static void update_dl_migration(struct dl_rq *dl_rq)
{
	if (dl_rq->dl_nr_migratory && dl_rq->dl_nr_total > 1) {
		if (!dl_rq->overloaded) {
			dl_set_overload(rq_of_dl_rq(dl_rq));
			dl_rq->overloaded = 1;
		}
	} else if (dl_rq->overloaded) {
		dl_clear_overload(rq_of_dl_rq(dl_rq));
		dl_rq->overloaded = 0;
	}
}

static void inc_dl_migration(struct sched_dl_entity *dl_se, struct dl_rq *dl_rq)
{
	dl_rq = &rq_of_dl_rq(dl_rq)->dl;

	dl_rq->dl_nr_total++;
	if (dl_se->nr_cpus_allowed > 1)
		dl_rq->dl_nr_migratory++;

	update_dl_migration(dl_rq);
}

static void dec_dl_migration(struct sched_dl_entity *dl_se, struct dl_rq *dl_rq)
{
	dl_rq = &rq_of_dl_rq(dl_rq)->dl;

	dl_rq->dl_nr_total--;
	if (dl_se->nr_cpus_allowed > 1)
		dl_rq->dl_nr_migratory--;

	update_dl_migration(dl_rq);
}

/*
 * The list of pushable -deadline task is not a plist, like in
 * sched_rt.c, it is an rb-tree with tasks ordered by deadline.
 */
static void enqueue_pushable_dl_task(struct rq *rq, struct task_struct *p)
{
	struct dl_rq *dl_rq = &rq->dl;
	struct rb_node **link = &dl_rq->pushable_dl_tasks_root.rb_node;
	struct rb_node *parent = NULL;
	struct task_struct *entry;
	int leftmost = 1;

	BUG_ON(!RB_EMPTY_NODE(&p->pushable_dl_tasks));

	while (*link) {
		parent = *link;
		entry = rb_entry(parent, struct task_struct,
				 pushable_dl_tasks);
		if (!dl_entity_preempt(&entry->dl, &p->dl))
			link = &parent->rb_left;
		else {
			link = &parent->rb_right;
			leftmost = 0;
		}
	}

	if (leftmost)
		dl_rq->pushable_dl_tasks_leftmost = &p->pushable_dl_tasks;

	rb_link_node(&p->pushable_dl_tasks, parent, link);
	rb_insert_color(&p->pushable_dl_tasks, &dl_rq->pushable_dl_tasks_root);
}

static void dequeue_pushable_dl_task(struct rq *rq, struct task_struct *p)
{
	struct dl_rq *dl_rq = &rq->dl;

	if (RB_EMPTY_NODE(&p->pushable_dl_tasks))
		return;

	if (dl_rq->pushable_dl_tasks_leftmost == &p->pushable_dl_tasks) {
		struct rb_node *next_node;

		next_node = rb_next(&p->pushable_dl_tasks);
		dl_rq->pushable_dl_tasks_leftmost = next_node;
	}

	rb_erase(&p->pushable_dl_tasks, &dl_rq->pushable_dl_tasks_root);
	RB_CLEAR_NODE(&p->pushable_dl_tasks);
}

static inline int has_pushable_dl_tasks(struct rq *rq)
{
	return !RB_EMPTY_ROOT(&rq->dl.pushable_dl_tasks_root);
}

#else

static inline
void enqueue_pushable_dl_task(struct rq *rq, struct task_struct *p)
{
}

static inline
void dequeue_pushable_dl_task(struct rq *rq, struct task_struct *p)
{
}

static inline
void inc_dl_migration(struct sched_dl_entity *dl_se, struct dl_rq *dl_rq)
{
}

static inline
void dec_dl_migration(struct sched_dl_entity *dl_se, struct dl_rq *dl_rq)
{
}

#endif /* CONFIG_SMP */

static inline int on_dl_rq(struct sched_dl_entity *dl_se)
{
	return !RB_EMPTY_NODE(&dl_se->rb_node);
}

static void enqueue_task_dl(struct rq *rq, struct task_struct *p, int flags);
static void __dequeue_task_dl(struct rq *rq, struct task_struct *p, int flags);
static void check_preempt_curr_dl(struct rq *rq, struct task_struct *p,
				  int flags);
static int push_dl_task(struct rq *rq);

/*
 * We are being explicitly informed that a new instance is starting,
 * and this means that:
 *  - the absolute deadline of the entity has to be placed at
 *    current time + relative deadline;
 *  - the runtime of the entity has to be set to the maximum value.
 *
 * The capability of specifying such event is useful whenever a -deadline
 * entity wants to (try to!) synchronize its behaviour with the scheduler's
 * one, and to (try to!) reconcile itself with its own scheduling
 * parameters.
 */
static inline void setup_new_dl_entity(struct sched_dl_entity *dl_se,
				       struct sched_dl_entity *pi_se)
{
	struct dl_rq *dl_rq = dl_rq_of_se(dl_se);
	struct rq *rq = rq_of_dl_rq(dl_rq);

	WARN_ON(!dl_se->dl_new || dl_se->dl_throttled);

	dl_se->deadline = rq->clock + pi_se->dl_deadline;
	dl_se->runtime = pi_se->dl_runtime;
	dl_se->dl_new = 0;
#ifdef CONFIG_SCHEDSTATS
	trace_sched_stat_new_dl(dl_task_of(dl_se), rq->clock, dl_se->flags);
#endif
}

/*
 * Pure Earliest Deadline First (EDF) scheduling does not deal with the
 * possibility of a entity lasting more than what it declared, and thus
 * exhausting its runtime.
 *
 * Here we are interested in making runtime overrun possible, but we do
 * not want a entity which is misbehaving to affect the scheduling of all
 * other entities.
 * Therefore, a budgeting strategy called Constant Bandwidth Server (CBS)
 * is used, in order to confine each entity within its own bandwidth.
 *
 * This function deals exactly with that, and ensures that when the runtime
 * of a entity is replenished, its deadline is also postponed. That ensures
 * the overrunning entity can't interfere with other entity in the system and
 * can't make them miss their deadlines. Reasons why this kind of overruns
 * could happen are, typically, a entity voluntarily trying to overcume its
 * runtime, or it just underestimated it during sched_setscheduler_ex().
 */
static void replenish_dl_entity(struct sched_dl_entity *dl_se,
				struct sched_dl_entity *pi_se)
{
	struct dl_rq *dl_rq = dl_rq_of_se(dl_se);
	struct rq *rq = rq_of_dl_rq(dl_rq);
	int reset = 0;

	/*
	 * We Keep moving the deadline away until we get some
	 * available runtime for the entity. This ensures correct
	 * handling of situations where the runtime overrun is
	 * arbitrary large.
	 */
	while (dl_se->runtime <= 0) {
		dl_se->deadline += pi_se->dl_period;
		dl_se->runtime += pi_se->dl_runtime;
	}

	/*
	 * At this point, the deadline really should be "in
	 * the future" with respect to rq->clock. If it's
	 * not, we are, for some reason, lagging too much!
	 * Anyway, after having warn userspace abut that,
	 * we still try to keep the things running by
	 * resetting the deadline and the budget of the
	 * entity.
	 */
	if (dl_time_before(dl_se->deadline, rq->clock)) {
		WARN_ON_ONCE(1);
		dl_se->deadline = rq->clock + pi_se->dl_deadline;
		dl_se->runtime = pi_se->dl_runtime;
		reset = 1;
	}
#ifdef CONFIG_SCHEDSTATS
	trace_sched_stat_repl_dl(dl_task_of(dl_se), rq->clock, reset);
#endif
}

/*
 * Here we check if --at time t-- an entity (which is probably being
 * [re]activated or, in general, enqueued) can use its remaining runtime
 * and its current deadline _without_ exceeding the bandwidth it is
 * assigned (function returns true if it can).
 *
 * For this to hold, we must check if:
 *   runtime / (deadline - t) < dl_runtime / dl_period .
 *
 * Notice that the bandwidth check is done against the period. For
 * task with deadline equal to period this is the same of using
 * dl_deadline instead of dl_period in the equation above.
 */
static bool dl_entity_overflow(struct sched_dl_entity *dl_se,
			       struct sched_dl_entity *pi_se, u64 t)
{
	u64 left, right;

	/*
	 * left and right are the two sides of the equation above,
	 * after a bit of shuffling to use multiplications instead
	 * of divisions.
	 *
	 * Note that none of the time values involved in the two
	 * multiplications are absolute: dl_deadline and dl_runtime
	 * are the relative deadline and the maximum runtime of each
	 * instance, runtime is the runtime left for the last instance
	 * and (deadline - t), since t is rq->clock, is the time left
	 * to the (absolute) deadline. Therefore, overflowing the u64
	 * type is very unlikely to occur in both cases.
	 */
	left = pi_se->dl_deadline * dl_se->runtime;
	right = (dl_se->deadline - t) * pi_se->dl_runtime;

	return dl_time_before(right, left);
}

/*
 * When a -deadline entity is queued back on the runqueue, its runtime and
 * deadline might need updating.
 *
 * The policy here is that we update the deadline of the entity only if:
 *  - the current deadline is in the past,
 *  - using the remaining runtime with the current deadline would make
 *    the entity exceed its bandwidth.
 */
static void update_dl_entity(struct sched_dl_entity *dl_se,
			     struct sched_dl_entity *pi_se)
{
	struct dl_rq *dl_rq = dl_rq_of_se(dl_se);
	struct rq *rq = rq_of_dl_rq(dl_rq);
	int overflow = 0;

	/*
	 * The arrival of a new instance needs special treatment, i.e.,
	 * the actual scheduling parameters have to be "renewed".
	 */
	if (dl_se->dl_new) {
		setup_new_dl_entity(dl_se, pi_se);
		return;
	}

	if (dl_time_before(dl_se->deadline, rq->clock) ||
	    dl_entity_overflow(dl_se, pi_se, rq->clock)) {
		dl_se->deadline = rq->clock + pi_se->dl_deadline;
		dl_se->runtime = pi_se->dl_runtime;
		overflow = 1;
	}
#ifdef CONFIG_SCHEDSTATS
	trace_sched_stat_updt_dl(dl_task_of(dl_se), rq->clock, overflow);
#endif
}

/*
 * If the entity depleted all its runtime, and if we want it to sleep
 * while waiting for some new execution time to become available, we
 * set the bandwidth enforcement timer to the replenishment instant
 * and try to activate it.
 *
 * Notice that it is important for the caller to know if the timer
 * actually started or not (i.e., the replenishment instant is in
 * the future or in the past).
 */
static int start_dl_timer(struct sched_dl_entity *dl_se, bool boosted)
{
	struct dl_rq *dl_rq = dl_rq_of_se(dl_se);
	struct rq *rq = rq_of_dl_rq(dl_rq);
	ktime_t now, act;
	ktime_t soft, hard;
	unsigned long range;
	s64 delta;

	/*
	 * If the task wants to stay -deadline even if it exhausted
	 * its runtime we allow that by not starting the timer.
	 * update_curr_dl() will thus queue it back after replenishment
	 * and deadline postponing.
	 * This won't affect the other -deadline tasks, but if we are
	 * a CPU-hog, lower scheduling classes will starve!
	 */
	if (boosted || dl_se->flags & SF_BWRECL_DL)
		return 0;

	/*
	 * We want the timer to fire at the deadline, but considering
	 * that it is actually coming from rq->clock and not from
	 * hrtimer's time base reading.
	 */
	act = ns_to_ktime(dl_se->deadline);
	now = hrtimer_cb_get_time(&dl_se->dl_timer);
	delta = ktime_to_ns(now) - rq->clock;
	act = ktime_add_ns(act, delta);

	/*
	 * If the expiry time already passed, e.g., because the value
	 * chosen as the deadline is too small, don't even try to
	 * start the timer in the past!
	 */
	if (ktime_us_delta(act, now) < 0)
		return 0;

	hrtimer_set_expires(&dl_se->dl_timer, act);

	soft = hrtimer_get_softexpires(&dl_se->dl_timer);
	hard = hrtimer_get_expires(&dl_se->dl_timer);
	range = ktime_to_ns(ktime_sub(hard, soft));
	__hrtimer_start_range_ns(&dl_se->dl_timer, soft,
				 range, HRTIMER_MODE_ABS, 0);

	trace_sched_start_timer_dl(dl_task_of(dl_se), rq->clock,
				   ktime_to_ns(now), ktime_to_ns(soft),
				   range);

	return hrtimer_active(&dl_se->dl_timer);
}

static void __setprio(struct rq *rq, struct task_struct *p, int prio);

/*
 * This is the bandwidth enforcement timer callback. If here, we know
 * a task is not on its dl_rq, since the fact that the timer was running
 * means the task is throttled and needs a runtime replenishment.
 *
 * However, what we actually do depends on the fact the task is active,
 * (it is on its rq) or has been removed from there by a call to
 * dequeue_task_dl(). In the former case we must issue the runtime
 * replenishment and add the task back to the dl_rq; in the latter, we just
 * do nothing but clearing dl_throttled, so that runtime and deadline
 * updating (and the queueing back to dl_rq) will be done by the
 * next call to enqueue_task_dl().
 */
static enum hrtimer_restart dl_task_timer(struct hrtimer *timer)
{
	unsigned long flags;
	struct sched_dl_entity *dl_se = container_of(timer,
						     struct sched_dl_entity,
						     dl_timer);
	struct task_struct *p = dl_task_of(dl_se);
	struct rq *rq = task_rq_lock(p, &flags);

	/*
	 * We need to take care of a possible races here. In fact, the
	 * task might have changed its scheduling policy to something
	 * different from SCHED_DEADLINE (through sched_setscheduler()).
	 * However, if we changed scheduling class for reclaiming, it
	 * is correct to handle this replenishment, since this is what
	 * will put us back into the -deadline scheduling class.
	 */
	if (!__dl_task(p))
		goto unlock;

	trace_sched_timer_dl(p, rq->clock, p->se.on_rq, task_current(rq, p));

	if (unlikely(p->sched_class != &dl_sched_class))
		__setprio(rq, p, MAX_DL_PRIO-1);

	dl_se->dl_throttled = 0;
	if (p->se.on_rq) {
		enqueue_task_dl(rq, p, ENQUEUE_REPLENISH);
		check_preempt_curr_dl(rq, p, 0);

		/*
		 * Queueing this task back might have overloaded rq,
		 * check if we need to kick someone away.
		 */
		if (rq->dl.overloaded)
			push_dl_task(rq);
	}

unlock:
	task_rq_unlock(rq, &flags);

	return HRTIMER_NORESTART;
}

static void init_dl_task_timer(struct sched_dl_entity *dl_se)
{
	struct hrtimer *timer = &dl_se->dl_timer;

	if (hrtimer_active(timer)) {
		hrtimer_try_to_cancel(timer);
		return;
	}

	hrtimer_init(timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	timer->function = dl_task_timer;
}

static
int dl_runtime_exceeded(struct rq *rq, struct sched_dl_entity *dl_se)
{
	int dmiss = dl_time_before(dl_se->deadline, rq->clock);
	int rorun = dl_se->runtime <= 0;

	/*
	 * Record statistics about last and maximum deadline
	 * misses and runtime overruns.
	 */
	if (dmiss) {
		u64 damount = rq->clock - dl_se->deadline;

		dl_se->stats.dmiss = 1;
		dl_se->stats.last_dmiss = damount;

		schedstat_set(dl_se->stats.dmiss_max,
			      max(dl_se->stats.dmiss_max, damount));
	}
	if (rorun) {
		u64 ramount = -dl_se->runtime;

		dl_se->stats.rorun = 1;
		dl_se->stats.last_rorun = ramount;

		schedstat_set(dl_se->stats.rorun_max,
			      max(dl_se->stats.rorun_max, ramount));
	}

	/*
	 * No need for checking if it's time to enforce the
	 * bandwidth for the tasks that are:
	 *  - maximum priority (SF_HEAD),
	 *  - not overrunning nor missing a deadline.
	 */
	if (dl_se->flags & SF_HEAD || (!rorun && !dmiss))
		return 0;

	/*
	 * If we are beyond our current deadline and we are still
	 * executing, then we have already used some of the runtime of
	 * the next instance. Thus, if we do not account that, we are
	 * stealing bandwidth from the system at each deadline miss!
	 */
	if (dmiss) {
		dl_se->runtime = rorun ? dl_se->runtime : 0;
		dl_se->runtime -= rq->clock - dl_se->deadline;
	}

	return 1;
}

static inline void throttle_curr_dl(struct rq *rq, struct task_struct *curr)
{
	curr->dl.dl_throttled = 1;

	if (curr->dl.flags & SF_BWRECL_RT)
		__setprio(rq, curr, MAX_RT_PRIO-1 - curr->rt_priority);
	else if (curr->dl.flags & SF_BWRECL_NR)
		__setprio(rq, curr, DEFAULT_PRIO);
}

/*
 * Update the current task's runtime statistics (provided it is still
 * a -deadline task and has not been removed from the dl_rq).
 */
static void update_curr_dl(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	struct sched_dl_entity *dl_se = &curr->dl;
	u64 delta_exec;

	if (!dl_task(curr) || !on_dl_rq(dl_se))
		return;

	delta_exec = rq->clock - curr->se.exec_start;
	if (unlikely((s64)delta_exec < 0))
		delta_exec = 0;

	schedstat_set(curr->se.statistics.exec_max,
		      max(curr->se.statistics.exec_max, delta_exec));

	curr->se.sum_exec_runtime += delta_exec;
	schedstat_add(&rq->dl, exec_clock, delta_exec);
	account_group_exec_runtime(curr, delta_exec);
	trace_sched_stat_runtime_dl(curr, rq->clock, delta_exec);

	curr->se.exec_start = rq->clock;
	cpuacct_charge(curr, delta_exec);

	sched_dl_avg_update(rq, delta_exec);

	dl_se->stats.tot_rtime += delta_exec;
	dl_se->runtime -= delta_exec;
	if (dl_runtime_exceeded(rq, dl_se)) {
		__dequeue_task_dl(rq, curr, 0);
		if (likely(start_dl_timer(dl_se, !!curr->pi_top_task)))
			throttle_curr_dl(rq, curr);
		else
			enqueue_task_dl(rq, curr, ENQUEUE_REPLENISH);

		resched_task(curr);
	}
}

#ifdef CONFIG_SMP

static struct task_struct *pick_next_earliest_dl_task(struct rq *rq, int cpu);

static inline int next_deadline(struct rq *rq)
{
	struct task_struct *next = pick_next_earliest_dl_task(rq, rq->cpu);

	if (next && dl_prio(next->prio))
		return next->dl.deadline;
	else
		return 0;
}

static void inc_dl_deadline(struct dl_rq *dl_rq, u64 deadline)
{
	struct rq *rq = rq_of_dl_rq(dl_rq);

	if (dl_rq->earliest_dl.curr == 0 ||
	    dl_time_before(deadline, dl_rq->earliest_dl.curr)) {
		/*
		 * If the dl_rq had no -deadline tasks, or if the new task
		 * has shorter deadline than the current one on dl_rq, we
		 * know that the previous earliest becomes our next earliest,
		 * as the new task becomes the earliest itself.
		 */
		dl_rq->earliest_dl.next = dl_rq->earliest_dl.curr;
		dl_rq->earliest_dl.curr = deadline;
		schedstat_inc(&rq->dl, nr_dummy);
	} else if (dl_rq->earliest_dl.next == 0 ||
		   dl_time_before(deadline, dl_rq->earliest_dl.next)) {
		/*
		 * On the other hand, if the new -deadline task has a
		 * a later deadline than the earliest one on dl_rq, but
		 * it is earlier than the next (if any), we must
		 * recompute the next-earliest.
		 */
		dl_rq->earliest_dl.next = next_deadline(rq);
	}
}

static void dec_dl_deadline(struct dl_rq *dl_rq, u64 deadline)
{
	struct rq *rq = rq_of_dl_rq(dl_rq);

	/*
	 * Since we may have removed our earliest (and/or next earliest)
	 * task we must recompute them.
	 */
	if (!dl_rq->dl_nr_running) {
		dl_rq->earliest_dl.curr = 0;
		dl_rq->earliest_dl.next = 0;
		schedstat_inc(&rq->dl, nr_dummy);
	} else {
		struct rb_node *leftmost = dl_rq->rb_leftmost;
		struct sched_dl_entity *entry;

		entry = rb_entry(leftmost, struct sched_dl_entity, rb_node);
		dl_rq->earliest_dl.curr = entry->deadline;
		dl_rq->earliest_dl.next = next_deadline(rq);
		schedstat_inc(&rq->dl, nr_dummy);
	}
}

#else

static inline void inc_dl_deadline(struct dl_rq *dl_rq, u64 deadline) {}
static inline void dec_dl_deadline(struct dl_rq *dl_rq, u64 deadline) {}

#endif /* CONFIG_SMP */

static inline
void inc_dl_tasks(struct sched_dl_entity *dl_se, struct dl_rq *dl_rq)
{
	int prio = dl_task_of(dl_se)->prio;
	u64 deadline = dl_se->deadline;

	WARN_ON(!dl_prio(prio));
	dl_rq->dl_nr_running++;

	inc_dl_deadline(dl_rq, deadline);
	inc_dl_migration(dl_se, dl_rq);
}

static inline
void dec_dl_tasks(struct sched_dl_entity *dl_se, struct dl_rq *dl_rq)
{
	int prio = dl_task_of(dl_se)->prio;

	WARN_ON(!dl_prio(prio));
	WARN_ON(!dl_rq->dl_nr_running);
	dl_rq->dl_nr_running--;

	dec_dl_deadline(dl_rq, dl_se->deadline);
	dec_dl_migration(dl_se, dl_rq);
}

static void __enqueue_dl_entity(struct sched_dl_entity *dl_se)
{
	struct dl_rq *dl_rq = dl_rq_of_se(dl_se);
	struct rb_node **link = &dl_rq->rb_root.rb_node;
	struct rb_node *parent = NULL;
	struct sched_dl_entity *entry;
	int leftmost = 1;

	BUG_ON(!RB_EMPTY_NODE(&dl_se->rb_node));

	while (*link) {
		parent = *link;
		entry = rb_entry(parent, struct sched_dl_entity, rb_node);
		if (dl_entity_preempt(dl_se, entry))
			link = &parent->rb_left;
		else {
			link = &parent->rb_right;
			leftmost = 0;
		}
	}

	if (leftmost)
		dl_rq->rb_leftmost = &dl_se->rb_node;

	rb_link_node(&dl_se->rb_node, parent, link);
	rb_insert_color(&dl_se->rb_node, &dl_rq->rb_root);

	inc_dl_tasks(dl_se, dl_rq);
}

static void __dequeue_dl_entity(struct sched_dl_entity *dl_se)
{
	struct dl_rq *dl_rq = dl_rq_of_se(dl_se);

	if (RB_EMPTY_NODE(&dl_se->rb_node))
		return;

	if (dl_rq->rb_leftmost == &dl_se->rb_node) {
		struct rb_node *next_node;

		next_node = rb_next(&dl_se->rb_node);
		dl_rq->rb_leftmost = next_node;
	}

	rb_erase(&dl_se->rb_node, &dl_rq->rb_root);
	RB_CLEAR_NODE(&dl_se->rb_node);

	dec_dl_tasks(dl_se, dl_rq);
}

static void
enqueue_dl_entity(struct sched_dl_entity *dl_se,
		  struct sched_dl_entity *pi_se, int flags)
{
	BUG_ON(on_dl_rq(dl_se));

	/*
	 * If this is a wakeup or a new instance, the scheduling
	 * parameters of the task might need updating. Otherwise,
	 * we want a replenishment of its runtime.
	 */
	if (!dl_se->dl_new && flags & ENQUEUE_REPLENISH)
		replenish_dl_entity(dl_se, pi_se);
	else
		update_dl_entity(dl_se, pi_se);

	__enqueue_dl_entity(dl_se);
}

static void dequeue_dl_entity(struct sched_dl_entity *dl_se)
{
	__dequeue_dl_entity(dl_se);
}

static void enqueue_task_dl(struct rq *rq, struct task_struct *p, int flags)
{
	struct task_struct *pi_task = p->pi_top_task;
	struct sched_dl_entity *pi_se = &p->dl;

	/*
	 * Use the scheduling parameters of the top pi-waiter
	 * task if we have one and its (relative) deadline is
	 * smaller than our one... OTW we keep our runtime and
	 * deadline.
	 */
	if (pi_task && dl_entity_preempt(&pi_task->dl, &p->dl))
		pi_se = &pi_task->dl;

	cycles_t x = get_cycles();

	/*
	 * If p is throttled, we do nothing. In fact, if it exhausted
	 * its budget it needs a replenishment and, since it now is on
	 * its rq, the bandwidth timer callback (which clearly has not
	 * run yet) will take care of this.
	 */
	if (p->dl.dl_throttled)
		return;

	enqueue_dl_entity(&p->dl, pi_se, flags);

	if (!task_current(rq, p) && p->dl.nr_cpus_allowed > 1)
		enqueue_pushable_dl_task(rq, p);

	schedstat_add(&rq->dl, enqueue_cycles, get_cycles() - x);
	schedstat_inc(&rq->dl, nr_enqueue);
}

static void __dequeue_task_dl(struct rq *rq, struct task_struct *p, int flags)
{
	dequeue_dl_entity(&p->dl);
	dequeue_pushable_dl_task(rq, p);
}

static void dequeue_task_dl(struct rq *rq, struct task_struct *p, int flags)
{
	cycles_t x = get_cycles();

	if (likely(!p->dl.dl_throttled)) {
		update_curr_dl(rq);
		__dequeue_task_dl(rq, p, flags);
	}

	schedstat_add(&rq->dl, dequeue_cycles, get_cycles() - x);
	schedstat_inc(&rq->dl, nr_dequeue);
}

/*
 * This function makes the task sleep until at least the absolute time
 * instant specified in @rqtp.
 * In fact, since we want to wake up the task with its full runtime,
 * @rqtp might be too early (or the task might already have overrun
 * its runtime when calling this), the sleeping time may  be longer
 * than asked.
 *
 * This is intended to be used at the end of a periodic -deadline task
 * instance, or any time a task want to be sure it'll wake up with
 * its full runtime.
 */
static long wait_interval_dl(struct task_struct *p, struct timespec *rqtp,
			     struct timespec *rmtp)
{
	unsigned long flags;
	struct sched_dl_entity *dl_se = &p->dl;
	struct rq *rq = task_rq_lock(p, &flags);
	struct timespec lrqtp;
	u64 wakeup;

	/*
	 * If no wakeup time is provided, sleep at least up to the
	 * next activation period. This guarantee the budget will
	 * be renewed.
	 */
	if (!rqtp) {
		wakeup = dl_se->deadline +
			 dl_se->dl_period - dl_se->dl_deadline;
		goto unlock;
	}

	/*
	 * If the tasks wants to wake up _before_ its absolute deadline
	 * we must be sure that reusing its (actual) runtime and deadline
	 * at that time _would_ overcome its bandwidth limitation, so
	 * that we know it will be given new parameters.
	 *
	 * If this is not true, we postpone the wake-up time up to the right
	 * instant. This involves a division (to calculate the reverse of the
	 * task's bandwidth), but it is worth to notice that it is quite
	 * unlikely that we get into here very often.
	 */
	wakeup = timespec_to_ns(rqtp);
	if (dl_time_before(wakeup, dl_se->deadline) &&
	    !dl_entity_overflow(dl_se, dl_se, wakeup)) {
		u64 ibw = (u64)dl_se->runtime * dl_se->dl_period;

		ibw = div_u64(ibw, dl_se->dl_runtime);
		wakeup = dl_se->deadline - ibw;
	}

unlock:
	task_rq_unlock(rq, &flags);

	lrqtp = ns_to_timespec(wakeup);
	dl_se->dl_new = 1;

	return hrtimer_nanosleep(&lrqtp, rmtp, HRTIMER_MODE_ABS,
				 CLOCK_MONOTONIC);
}

/*
 * Yield task semantic for -deadline tasks is:
 *
 *   get off from the CPU until our next instance, with
 *   a new runtime.
 */
static void yield_task_dl(struct rq *rq)
{
	struct task_struct *p = rq->curr;

	/*
	 * We make the task go to sleep until its current deadline by
	 * forcing its runtime to zero. This way, update_curr_dl() stops
	 * it and the bandwidth timer will wake it up and will give it
	 * new scheduling parameters (thanks to dl_new=1).
	 */
	if (p->dl.runtime > 0) {
		rq->curr->dl.dl_new = 1;
		p->dl.runtime = 0;
	}
	update_curr_dl(rq);
}

#ifdef CONFIG_SMP
static int find_later_rq(struct task_struct *task);
static int latest_cpu_find(struct cpumask *span,
			   struct task_struct *task,
			   struct cpumask *later_mask);

static int
select_task_rq_dl(struct rq *rq, struct task_struct *p, int sd_flag, int flags)
{
	if (sd_flag != SD_BALANCE_WAKE)
		return smp_processor_id();

	/*
	 * If we are dealing with a -deadline task, we must
	 * decide where to wake it up.
	 * If it has a later deadline and the current task
	 * on this rq can't move (provided the waking task
	 * can!) we prefer to send it somewhere else. On the
	 * other hand, if it has a shorter deadline, we
	 * try to make it stay here, it might be important.
	 */
	if (unlikely(dl_task(rq->curr)) &&
	    (rq->curr->dl.nr_cpus_allowed < 2 ||
	     dl_entity_preempt(&rq->curr->dl, &p->dl)) &&
	    (p->dl.nr_cpus_allowed > 1)) {
		int cpu = find_later_rq(p);

		return (cpu == -1) ? task_cpu(p) : cpu;
	}

	return task_cpu(p);
}

static void check_preempt_equal_dl(struct rq *rq, struct task_struct *p)
{
	/*
	 * Current can't be migrated, useles to reschedule,
	 * let's hope p can move out.
	 */
	if (rq->curr->dl.nr_cpus_allowed == 1 ||
	    latest_cpu_find(rq->rd->span, rq->curr, NULL) == -1) {
		schedstat_inc(&rq->dl, nr_dummy);
		return;
	}

	/*
	 * p is migratable, so let's not schedule it and
	 * see if it is pushed or pulled somewhere else.
	 */
	if (p->dl.nr_cpus_allowed != 1 &&
	    latest_cpu_find(rq->rd->span, p, NULL) != -1) {
		schedstat_inc(&rq->dl, nr_dummy);
		return;
	}

	resched_task(rq->curr);
}

#endif /* CONFIG_SMP */

static void check_preempt_curr_dl(struct rq *rq, struct task_struct *p,
				  int flags)
{
	if (!dl_task(rq->curr) || (dl_task(p) &&
	    dl_entity_preempt(&p->dl, &rq->curr->dl))) {
		resched_task(rq->curr);
		return;
	}

#ifdef CONFIG_SMP
	/*
	 * In the unlikely case current and p have the same deadline
	 * let us try to decide what's the best thing to do...
	 */
	if ((s64)(p->dl.deadline - rq->curr->dl.deadline) == 0 &&
	    !need_resched())
		check_preempt_equal_dl(rq, p);
#endif /* CONFIG_SMP */
}

#ifdef CONFIG_SCHED_HRTICK
static void start_hrtick_dl(struct rq *rq, struct task_struct *p)
{
	s64 delta = p->dl.dl_runtime - p->dl.runtime;

	if (delta > 10000)
		hrtick_start(rq, delta);
}
#else
static void start_hrtick_dl(struct rq *rq, struct task_struct *p)
{
}
#endif

static struct sched_dl_entity *__pick_dl_last_entity(struct dl_rq *dl_rq)
{
	struct rb_node *last = rb_last(&dl_rq->rb_root);

	if (!last)
		return NULL;

	return rb_entry(last, struct sched_dl_entity, rb_node);
}

static struct sched_dl_entity *pick_next_dl_entity(struct rq *rq,
						   struct dl_rq *dl_rq)
{
	struct rb_node *left = dl_rq->rb_leftmost;

	if (!left)
		return NULL;

	return rb_entry(left, struct sched_dl_entity, rb_node);
}

struct task_struct *pick_next_task_dl(struct rq *rq)
{
	struct sched_dl_entity *dl_se;
	struct task_struct *p;
	struct dl_rq *dl_rq;

	dl_rq = &rq->dl;

	if (unlikely(!dl_rq->dl_nr_running))
		return NULL;

	dl_se = pick_next_dl_entity(rq, dl_rq);
	BUG_ON(!dl_se);

	p = dl_task_of(dl_se);
	p->se.exec_start = rq->clock;

	/* Running task will never be pushed. */
	if (p)
		dequeue_pushable_dl_task(rq, p);

#ifdef CONFIG_SCHED_HRTICK
	if (hrtick_enabled(rq))
		start_hrtick_dl(rq, p);
#endif

#ifdef CONFIG_SMP
	rq->post_schedule = has_pushable_dl_tasks(rq);
#endif /* CONFIG_SMP */

	return p;
}

static void put_prev_task_dl(struct rq *rq, struct task_struct *p)
{
	if (unlikely(p->dl.dl_throttled))
		return;

	update_curr_dl(rq);
	p->se.exec_start = 0;

	if (on_dl_rq(&p->dl) && p->dl.nr_cpus_allowed > 1)
		enqueue_pushable_dl_task(rq, p);
}

static void task_tick_dl(struct rq *rq, struct task_struct *p, int queued)
{
	update_curr_dl(rq);

#ifdef CONFIG_SCHED_HRTICK
	if (hrtick_enabled(rq) && queued && p->dl.runtime > 0)
		start_hrtick_dl(rq, p);
#endif
}

static void task_fork_dl(struct task_struct *p)
{
	/*
	 * The child of a -deadline task will be SCHED_DEADLINE, but
	 * as a throttled task. This means the parent (or someone else)
	 * must call sched_setscheduler_ex() on it, or it won't even
	 * start.
	 */
	p->dl.dl_throttled = 1;
	p->dl.dl_new = 0;
}

static void task_dead_dl(struct task_struct *p)
{
	struct dl_bw *dl_b = &task_rq(p)->rd->dl_bw;

	/*
	 * Since we are TASK_DEAD we won't slip out of the domain!
	 */
	raw_spin_lock_irq(&dl_b->lock);
	dl_b->total_bw -= p->dl.dl_bw;
	raw_spin_unlock_irq(&dl_b->lock);

	/*
	 * We are no longer holding any lock here, so it is safe to
	 * We are not holding any lock here, so it is safe to
	 * wait for the bandwidth timer to be removed.
	 */
	hrtimer_cancel(&p->dl.dl_timer);
}

static void set_curr_task_dl(struct rq *rq)
{
	struct task_struct *p = rq->curr;

	p->se.exec_start = rq->clock;

	/* You can't push away the running task */
	dequeue_pushable_dl_task(rq, p);
}

#ifdef CONFIG_SMP

/* Only try algorithms three times */
#define DL_MAX_TRIES 3

static int pick_dl_task(struct rq *rq, struct task_struct *p, int cpu)
{
	if (!task_running(rq, p) &&
	    (cpu < 0 || cpumask_test_cpu(cpu, &p->cpus_allowed)) &&
	    (p->dl.nr_cpus_allowed > 1))
		return 1;

	return 0;
}

/* Returns the second earliest -deadline task, NULL otherwise */
static struct task_struct *pick_next_earliest_dl_task(struct rq *rq, int cpu)
{
	struct rb_node *next_node = rq->dl.rb_leftmost;
	struct sched_dl_entity *dl_se;
	struct task_struct *p = NULL;

next_node:
	next_node = rb_next(next_node);
	if (next_node) {
		dl_se = rb_entry(next_node, struct sched_dl_entity, rb_node);
		p = dl_task_of(dl_se);

		if (pick_dl_task(rq, p, cpu))
			return p;

		goto next_node;
	}

	return NULL;
}

static int latest_cpu_find(struct cpumask *span,
			   struct task_struct *task,
			   struct cpumask *later_mask)
{
	const struct sched_dl_entity *dl_se = &task->dl;
	int cpu, found = -1, best = 0;
	u64 max_dl = 0;

	for_each_cpu(cpu, span) {
		struct rq *rq = cpu_rq(cpu);
		struct dl_rq *dl_rq = &rq->dl;

		if (cpumask_test_cpu(cpu, &task->cpus_allowed) &&
		    (!dl_rq->dl_nr_running || dl_time_before(dl_se->deadline,
		     dl_rq->earliest_dl.curr))) {
			if (later_mask)
				cpumask_set_cpu(cpu, later_mask);
			if (!best && !dl_rq->dl_nr_running) {
				best = 1;
				found = cpu;
			} else if (!best &&
				   dl_time_before(max_dl,
						  dl_rq->earliest_dl.curr)) {
				max_dl = dl_rq->earliest_dl.curr;
				found = cpu;
			}
		} else if (later_mask)
			cpumask_clear_cpu(cpu, later_mask);
	}

	return found;
}

static DEFINE_PER_CPU(cpumask_var_t, local_cpu_mask_dl);

static int find_later_rq(struct task_struct *task)
{
	struct sched_domain *sd;
	struct cpumask *later_mask = __get_cpu_var(local_cpu_mask_dl);
	int this_cpu = smp_processor_id();
	int best_cpu, cpu = task_cpu(task);
	struct dl_rq *dl_rq = dl_rq_of_se(&task->dl);

	if (task->dl.nr_cpus_allowed == 1)
		return -1;

	best_cpu = latest_cpu_find(task_rq(task)->rd->span, task, later_mask);
	schedstat_inc(dl_rq, nr_dummy);
	if (best_cpu == -1)
		return -1;

	/*
	 * If we are here, some target has been found,
	 * the most suitable of which is cached in best_cpu.
	 * This is, among the runqueues where the current tasks
	 * have later deadlines than the task's one, the rq
	 * with the latest possible one.
	 *
	 * Now we check how well this matches with task's
	 * affinity and system topology.
	 *
	 * The last cpu where the task run is our first
	 * guess, since it is most likely cache-hot there.
	 */
	if (cpumask_test_cpu(cpu, later_mask))
		return cpu;

	/*
	 * Check if this_cpu is to be skipped (i.e., it is
	 * not in the mask) or not.
	 */
	if (!cpumask_test_cpu(this_cpu, later_mask))
		this_cpu = -1;

	for_each_domain(cpu, sd) {
		if (sd->flags & SD_WAKE_AFFINE) {

			/*
			 * If possible, preempting this_cpu is
			 * cheaper than migrating.
			 */
			if (this_cpu != -1 &&
			    cpumask_test_cpu(this_cpu, sched_domain_span(sd)))
				return this_cpu;

			/*
			 * Last chance: if best_cpu is valid and is
			 * in the mask, that becomes our choice.
			 */
			if (best_cpu < nr_cpu_ids &&
			    cpumask_test_cpu(best_cpu, sched_domain_span(sd)))
				return best_cpu;
		}
	}

	/*
	 * At this point, all our guesses failed, we just return
	 * 'something', and let the caller sort the things out.
	 */
	if (this_cpu != -1)
		return this_cpu;

	cpu = cpumask_any(later_mask);
	if (cpu < nr_cpu_ids)
		return cpu;

	return -1;
}

/* Locks the rq it finds */
static struct rq *find_lock_later_rq(struct task_struct *task, struct rq *rq)
{
	struct rq *later_rq = NULL;
	int tries;
	int cpu;

	for (tries = 0; tries < DL_MAX_TRIES; tries++) {
		cpu = find_later_rq(task);

		if ((cpu == -1) || (cpu == rq->cpu))
			break;

		later_rq = cpu_rq(cpu);

		/* Retry if something changed. */
		if (double_lock_balance(rq, later_rq)) {
			if (unlikely(task_rq(task) != rq ||
				     !cpumask_test_cpu(later_rq->cpu,
						       &task->cpus_allowed) ||
				     task_running(rq, task) ||
				     !task->se.on_rq)) {
				raw_spin_unlock(&later_rq->lock);
				later_rq = NULL;
				break;
			}
		}

		/*
		 * If the rq we found has no -deadline task, or
		 * its earliest one has a later deadline than our
		 * task, the rq is a good one.
		 */
		if (!later_rq->dl.dl_nr_running ||
		    dl_time_before(task->dl.deadline,
				   later_rq->dl.earliest_dl.curr))
			break;

		/* Otherwise we try again. */
		double_unlock_balance(rq, later_rq);
		later_rq = NULL;
	}

	return later_rq;
}

static struct task_struct *pick_next_pushable_dl_task(struct rq *rq)
{
	struct task_struct *p;

	if (!has_pushable_dl_tasks(rq))
		return NULL;

	p = rb_entry(rq->dl.pushable_dl_tasks_leftmost,
		     struct task_struct, pushable_dl_tasks);

	BUG_ON(rq->cpu != task_cpu(p));
	BUG_ON(task_current(rq, p));
	BUG_ON(p->dl.nr_cpus_allowed <= 1);

	BUG_ON(!p->se.on_rq);
	BUG_ON(!dl_task(p));

	return p;
}

/*
 * See if the non running -deadline tasks on this rq
 * can be sent to some other CPU where they can preempt
 * and start executing.
 */
static int push_dl_task(struct rq *rq)
{
	cycles_t x = get_cycles();
	struct task_struct *next_task;
	struct rq *later_rq;
	int ret = 0;

	if (!rq->dl.overloaded)
		/*return 0;*/
		goto out;

	next_task = pick_next_pushable_dl_task(rq);
	if (!next_task)
		/*return 0;*/
		goto out;

retry:
	if (unlikely(next_task == rq->curr)) {
		WARN_ON(1);
		/*return 0;*/
		goto out;
	}

	/*
	 * If next_task preempts rq->curr, and rq->curr
	 * can move away, it makes sense to just reschedule
	 * without going further in pushing next_task.
	 */
	if (dl_task(rq->curr) &&
	    dl_time_before(next_task->dl.deadline, rq->curr->dl.deadline) &&
	    rq->curr->dl.nr_cpus_allowed > 1) {
		resched_task(rq->curr);
		/*return 0;*/
		goto out;
	}

	/* We might release rq lock */
	get_task_struct(next_task);

	/* Will lock the rq it'll find */
	later_rq = find_lock_later_rq(next_task, rq);

	trace_sched_push_task_dl(next_task, rq->clock,
				 later_rq ? later_rq->cpu : -1);

	if (!later_rq) {
		struct task_struct *task;

		/*
		 * We must check all this again, since
		 * find_lock_later_rq releases rq->lock and it is
		 * then possible that next_task has migrated.
		 */
		task = pick_next_pushable_dl_task(rq);
		if (task_cpu(next_task) == rq->cpu && task == next_task) {
			/*
			 * The task is still there. We don't try
			 * again, some other cpu will pull it when ready.
			 */
			dequeue_pushable_dl_task(rq, next_task);
			/*goto out;*/
			ret = 1;
			goto put;
		}

		if (!task) {
 			/* No more tasks */
			ret = 1;
			goto put;
		}

		schedstat_inc(&rq->dl, nr_retry_push);
		put_task_struct(next_task);
		next_task = task;
		goto retry;
	}

	deactivate_task(rq, next_task, 0);
	schedstat_inc(&rq->dl, nr_pushed_away);
	set_task_cpu(next_task, later_rq->cpu);
	activate_task(later_rq, next_task, 0);
	ret = 1;

	resched_task(later_rq->curr);

	double_unlock_balance(rq, later_rq);

put:
	put_task_struct(next_task);
out:
	schedstat_add(&rq->dl, push_cycles, get_cycles() - x);
	schedstat_inc(&rq->dl, nr_push);
 
	return ret;
}

static void push_dl_tasks(struct rq *rq)
{
	/* Terminates as it moves a -deadline task */
	while (push_dl_task(rq))
		;
}

static int pull_dl_task(struct rq *this_rq)
{
	cycles_t x = get_cycles();
	int this_cpu = this_rq->cpu, ret = 0, cpu;
	struct task_struct *p;
	struct rq *src_rq;
	u64 dmin = LONG_MAX;

	if (likely(!dl_overloaded(this_rq)))
		/*return 0;*/
		goto out;

	for_each_cpu(cpu, this_rq->rd->dlo_mask) {
		if (this_cpu == cpu)
			continue;

		src_rq = cpu_rq(cpu);

		/*
		 * It looks racy, abd it is! However, as in sched_rt.c,
		 * we are fine with this.
		 */
		if (this_rq->dl.dl_nr_running &&
		    dl_time_before(this_rq->dl.earliest_dl.curr,
				   src_rq->dl.earliest_dl.next))
			continue;

		/* Might drop this_rq->lock */
		double_lock_balance(this_rq, src_rq);

		/*
		 * If there are no more pullable tasks on the
		 * rq, we're done with it.
		 */
		if (src_rq->dl.dl_nr_running <= 1)
			goto skip;

		p = pick_next_earliest_dl_task(src_rq, this_cpu);
		if (p)
			trace_sched_pull_task_dl(p, this_rq->clock,
						 src_rq->cpu);

		/*
		 * We found a task to be pulled if:
		 *  - it preempts our current (if there's one),
		 *  - it will preempt the last one we pulled (if any).
		 */
		if (p && dl_time_before(p->dl.deadline, dmin) &&
		    (!this_rq->dl.dl_nr_running ||
		     dl_time_before(p->dl.deadline,
				    this_rq->dl.earliest_dl.curr))) {
			WARN_ON(p == src_rq->curr);
			WARN_ON(!p->se.on_rq);

			/*
			 * Then we pull iff p has actually an earlier
			 * deadline than the current task of its runqueue.
			 */
			if (dl_time_before(p->dl.deadline,
					   src_rq->curr->dl.deadline))
				goto skip;

			ret = 1;

			deactivate_task(src_rq, p, 0);
			schedstat_inc(&this_rq->dl, nr_pulled_here);
			set_task_cpu(p, this_cpu);
			activate_task(this_rq, p, 0);
			dmin = p->dl.deadline;

			/* Is there any other task even earlier? */
		}
skip:
		double_unlock_balance(this_rq, src_rq);
	}
out:
	schedstat_add(&this_rq->dl, pull_cycles, get_cycles() - x);
	schedstat_inc(&this_rq->dl, nr_pull);

	return ret;
}

static void pre_schedule_dl(struct rq *rq, struct task_struct *prev)
{
	/* Try to pull other tasks here */
	if (dl_task(prev))
		pull_dl_task(rq);
}

static void post_schedule_dl(struct rq *rq)
{
	push_dl_tasks(rq);
}

/*
 * Since the task is not running and a reschedule is not going to happen
 * anytime soon on its runqueue, we try pushing it away now.
 */
static void task_woken_dl(struct rq *rq, struct task_struct *p)
{
	if (!task_running(rq, p) &&
	    !test_tsk_need_resched(rq->curr) &&
	    has_pushable_dl_tasks(rq) &&
	    p->dl.nr_cpus_allowed > 1 &&
	    dl_task(rq->curr) &&
	    (rq->curr->dl.nr_cpus_allowed < 2 ||
	     dl_entity_preempt(&rq->curr->dl, &p->dl))) {
		push_dl_tasks(rq);
	}
}

static void set_cpus_allowed_dl(struct task_struct *p,
				const struct cpumask *new_mask)
{
	int weight = cpumask_weight(new_mask);

	BUG_ON(!dl_task(p));

	/*
	 * Update only if the task is actually running (i.e.,
	 * it is on the rq AND it is not throttled).
	 */
	if (on_dl_rq(&p->dl) && (weight != p->dl.nr_cpus_allowed)) {
		struct rq *rq = task_rq(p);

		if (!task_current(rq, p)) {
			/*
			 * If the task was on the pushable list,
			 * make sure it stays there only if the new
			 * mask allows that.
			 */
			if (p->dl.nr_cpus_allowed > 1)
				dequeue_pushable_dl_task(rq, p);

			if (weight > 1)
				enqueue_pushable_dl_task(rq, p);
		}

		if ((p->dl.nr_cpus_allowed <= 1) && (weight > 1)) {
			rq->dl.dl_nr_migratory++;
		} else if ((p->dl.nr_cpus_allowed > 1) && (weight <= 1)) {
			BUG_ON(!rq->dl.dl_nr_migratory);
			rq->dl.dl_nr_migratory--;
		}

		update_dl_migration(&rq->dl);
	}

	cpumask_copy(&p->cpus_allowed, new_mask);
	p->dl.nr_cpus_allowed = weight;
}

/* Assumes rq->lock is held */
static void rq_online_dl(struct rq *rq)
{
	if (rq->dl.overloaded)
		dl_set_overload(rq);
}

/* Assumes rq->lock is held */
static void rq_offline_dl(struct rq *rq)
{
	if (rq->dl.overloaded)
		dl_clear_overload(rq);
}

static inline void init_sched_dl_class(void)
{
	unsigned int i;

	for_each_possible_cpu(i)
		zalloc_cpumask_var_node(&per_cpu(local_cpu_mask_dl, i),
					GFP_KERNEL, cpu_to_node(i));
}
#endif /* CONFIG_SMP */

static void switched_from_dl(struct rq *rq, struct task_struct *p,
			     int running)
{
	if (hrtimer_active(&p->dl.dl_timer) && !dl_policy(p->policy))
		hrtimer_try_to_cancel(&p->dl.dl_timer);

#ifdef CONFIG_SMP
	/*
	 * Since this might be the only -deadline task on the rq,
	 * this is the right place to try to pull some other one
	 * from an overloaded cpu, if any.
	 */
	if (!rq->dl.dl_nr_running)
		pull_dl_task(rq);
#endif
}

/*
 * When switching to -deadline, we may overload the rq, then
 * we try to push someone off, if possible.
 */
static void switched_to_dl(struct rq *rq, struct task_struct *p,
			   int running)
{
	int check_resched = 1;

	/*
	 * If p is throttled, don't consider the possibility
	 * of preempting rq->curr, the check will be done right
	 * after its runtime will get replenished.
	 */
	if (unlikely(p->dl.dl_throttled))
		return;

	if (!running) {
#ifdef CONFIG_SMP
		if (rq->dl.overloaded && push_dl_task(rq) && rq != task_rq(p))
			/* Only reschedule if pushing failed */
			check_resched = 0;
#endif /* CONFIG_SMP */
		if (check_resched)
			check_preempt_curr_dl(rq, p, 0);
	}
}

/*
 * If the scheduling parameters of a -deadline task changed,
 * a push or pull operation might be needed.
 */
static void prio_changed_dl(struct rq *rq, struct task_struct *p,
			    int oldprio, int running)
{
	if (running) {
#ifdef CONFIG_SMP
		/*
		 * This might be too much, but unfortunately
		 * we don't have the old deadline value, and
		 * we can't argue if the task is increasing
		 * or lowering its prio, so...
		 */
		if (!rq->dl.overloaded)
			pull_dl_task(rq);

		/*
		 * If we now have a earlier deadline task than p,
		 * then reschedule, provided p is still on this
		 * runqueue.
		 */
		if (dl_time_before(rq->dl.earliest_dl.curr, p->dl.deadline) &&
		    rq->curr == p)
			resched_task(p);
#else
		/*
		 * Again, we don't know if p has a earlier
		 * or later deadline, so let's blindly set a
		 * (maybe not needed) rescheduling point.
		 */
		resched_task(p);
#endif /* CONFIG_SMP */
	} else
		switched_to_dl(rq, p, running);
}

static const struct sched_class dl_sched_class = {
	.next			= &rt_sched_class,
	.enqueue_task		= enqueue_task_dl,
	.dequeue_task		= dequeue_task_dl,
	.yield_task		= yield_task_dl,
	.wait_interval		= wait_interval_dl,

	.check_preempt_curr	= check_preempt_curr_dl,

	.pick_next_task		= pick_next_task_dl,
	.put_prev_task		= put_prev_task_dl,

#ifdef CONFIG_SMP
	.select_task_rq		= select_task_rq_dl,

	.set_cpus_allowed       = set_cpus_allowed_dl,
	.rq_online              = rq_online_dl,
	.rq_offline             = rq_offline_dl,
	.pre_schedule		= pre_schedule_dl,
	.post_schedule		= post_schedule_dl,
	.task_woken		= task_woken_dl,
#endif

	.set_curr_task		= set_curr_task_dl,
	.task_tick		= task_tick_dl,
	.task_fork              = task_fork_dl,
	.task_dead		= task_dead_dl,

	.prio_changed           = prio_changed_dl,
	.switched_from		= switched_from_dl,
	.switched_to		= switched_to_dl,
};

#ifdef CONFIG_SCHED_DEBUG
extern void print_dl_rq(struct seq_file *m, int cpu, struct dl_rq *dl_rq);

static void print_dl_stats(struct seq_file *m, int cpu)
{
	struct dl_rq *dl_rq = &cpu_rq(cpu)->dl;

	rcu_read_lock();
	print_dl_rq(m, cpu, dl_rq);
	rcu_read_unlock();
}
#endif /* CONFIG_SCHED_DEBUG */
