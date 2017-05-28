/*
 *  linux/kernel/signal.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  1997-11-02  Modified for POSIX.1b signals by Richard Henderson
 *
 *  2003-06-02  Jim Houston - Concurrent Computer Corp.
 *		Changes to use preallocated sigqueue structures
 *		to allow signals to be sent reliably.
 */

#include <linux/config.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/smp_lock.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/tty.h>
#include <linux/binfmts.h>
#include <linux/security.h>
#include <linux/syscalls.h>
#include <linux/ptrace.h>
#include <asm/param.h>
#include <asm/uaccess.h>
#include <asm/unistd.h>
#include <asm/siginfo.h>

extern void k_getrusage(struct task_struct *, int, struct rusage *);

/*
 * SLAB caches for signal bits.
 */

static kmem_cache_t *sigqueue_cachep;

/*
 * In POSIX a signal is sent either to a specific thread (Linux task)
 * or to the process as a whole (Linux thread group).  How the signal
 * is sent determines whether it's to one thread or the whole group,
 * which determines which signal mask(s) are involved in blocking it
 * from being delivered until later.  When the signal is delivered,
 * either it's caught or ignored by a user handler or it has a default
 * effect that applies to the whole thread group (POSIX process).
 *
 * The possible effects an unblocked signal set to SIG_DFL can have are:
 *   ignore	- Nothing Happens
 *   terminate	- kill the process, i.e. all threads in the group,
 * 		  similar to exit_group.  The group leader (only) reports
 *		  WIFSIGNALED status to its parent.
 *   coredump	- write a core dump file describing all threads using
 *		  the same mm and then kill all those threads
 *   stop 	- stop all the threads in the group, i.e. TASK_STOPPED state
 *
 * SIGKILL and SIGSTOP cannot be caught, blocked, or ignored.
 * Other signals when not blocked and set to SIG_DFL behaves as follows.
 * The job control signals also have other special effects.
 *
 *	+--------------------+------------------+
 *	|  POSIX signal      |  default action  |
 *	+--------------------+------------------+
 *	|  SIGHUP            |  terminate	|
 *	|  SIGINT            |	terminate	|
 *	|  SIGQUIT           |	coredump 	|
 *	|  SIGILL            |	coredump 	|
 *	|  SIGTRAP           |	coredump 	|
 *	|  SIGABRT/SIGIOT    |	coredump 	|
 *	|  SIGBUS            |	coredump 	|
 *	|  SIGFPE            |	coredump 	|
 *	|  SIGKILL           |	terminate(+)	|
 *	|  SIGUSR1           |	terminate	|
 *	|  SIGSEGV           |	coredump 	|
 *	|  SIGUSR2           |	terminate	|
 *	|  SIGPIPE           |	terminate	|
 *	|  SIGALRM           |	terminate	|
 *	|  SIGTERM           |	terminate	|
 *	|  SIGCHLD           |	ignore   	|
 *	|  SIGCONT           |	ignore(*)	|
 *	|  SIGSTOP           |	stop(*)(+)  	|
 *	|  SIGTSTP           |	stop(*)  	|
 *	|  SIGTTIN           |	stop(*)  	|
 *	|  SIGTTOU           |	stop(*)  	|
 *	|  SIGURG            |	ignore   	|
 *	|  SIGXCPU           |	coredump 	|
 *	|  SIGXFSZ           |	coredump 	|
 *	|  SIGVTALRM         |	terminate	|
 *	|  SIGPROF           |	terminate	|
 *	|  SIGPOLL/SIGIO     |	terminate	|
 *	|  SIGSYS/SIGUNUSED  |	coredump 	|
 *	|  SIGSTKFLT         |	terminate	|
 *	|  SIGWINCH          |	ignore   	|
 *	|  SIGPWR            |	terminate	|
 *	|  SIGRTMIN-SIGRTMAX |	terminate       |
 *	+--------------------+------------------+
 *	|  non-POSIX signal  |  default action  |
 *	+--------------------+------------------+
 *	|  SIGEMT            |  coredump	|
 *	+--------------------+------------------+
 *
 * (+) For SIGKILL and SIGSTOP the action is "always", not just "default".
 * (*) Special job control effects:
 * When SIGCONT is sent, it resumes the process (all threads in the group)
 * from TASK_STOPPED state and also clears any pending/queued stop signals
 * (any of those marked with "stop(*)").  This happens regardless of blocking,
 * catching, or ignoring SIGCONT.  When any stop signal is sent, it clears
 * any pending/queued SIGCONT signals; this happens regardless of blocking,
 * catching, or ignored the stop signal, though (except for SIGSTOP) the
 * default action of stopping the process may happen later or never.
 */

#ifdef SIGEMT
#define M_SIGEMT	M(SIGEMT)
#else
#define M_SIGEMT	0
#endif

#if SIGRTMIN > BITS_PER_LONG
#define M(sig) (1ULL << ((sig)-1))
#else
#define M(sig) (1UL << ((sig)-1))
#endif
#define T(sig, mask) (M(sig) & (mask))

#define SIG_KERNEL_ONLY_MASK (\
	M(SIGKILL)   |  M(SIGSTOP)                                   )

#define SIG_KERNEL_STOP_MASK (\
	M(SIGSTOP)   |  M(SIGTSTP)   |  M(SIGTTIN)   |  M(SIGTTOU)   )

#define SIG_KERNEL_COREDUMP_MASK (\
        M(SIGQUIT)   |  M(SIGILL)    |  M(SIGTRAP)   |  M(SIGABRT)   | \
        M(SIGFPE)    |  M(SIGSEGV)   |  M(SIGBUS)    |  M(SIGSYS)    | \
        M(SIGXCPU)   |  M(SIGXFSZ)   |  M_SIGEMT                     )

#define SIG_KERNEL_IGNORE_MASK (\
        M(SIGCONT)   |  M(SIGCHLD)   |  M(SIGWINCH)  |  M(SIGURG)    )

#define sig_kernel_only(sig) \
		(((sig) < SIGRTMIN)  && T(sig, SIG_KERNEL_ONLY_MASK))
#define sig_kernel_coredump(sig) \
		(((sig) < SIGRTMIN)  && T(sig, SIG_KERNEL_COREDUMP_MASK))
#define sig_kernel_ignore(sig) \
		(((sig) < SIGRTMIN)  && T(sig, SIG_KERNEL_IGNORE_MASK))
#define sig_kernel_stop(sig) \
		(((sig) < SIGRTMIN)  && T(sig, SIG_KERNEL_STOP_MASK))

#define sig_user_defined(t, signr) \
	(((t)->sighand->action[(signr)-1].sa.sa_handler != SIG_DFL) &&	\
	 ((t)->sighand->action[(signr)-1].sa.sa_handler != SIG_IGN))

#define sig_fatal(t, signr) \
	(!T(signr, SIG_KERNEL_IGNORE_MASK|SIG_KERNEL_STOP_MASK) && \
	 (t)->sighand->action[(signr)-1].sa.sa_handler == SIG_DFL)

/*
 *  检查进程是否忽略信号。
 */
static int sig_ignored(struct task_struct *t, int sig)
{
	void __user * handler;

	/*
	 * Tracers always want to know about signals..
	 */
	/*
	 * 进程没有被跟踪, PT_PTRACED标志被清0
	 */
	if (t->ptrace & PT_PTRACED)
		return 0;

	/*
	 * Blocked signals are never ignored, since the
	 * signal handler may change by the time it is
	 * unblocked.
	 */
	/*
	 * 信号没有被阻塞。
	 */
	if (sigismember(&t->blocked, sig))
		return 0;

	/* Is it explicitly or implicitly ignored? */
	/*
	 * 显式的忽略了信号。或者隐含的忽略了信号。
	 */
	handler = t->sighand->action[sig-1].sa.sa_handler;
	return   handler == SIG_IGN ||
		(handler == SIG_DFL && sig_kernel_ignore(sig));
}

/*
 * Re-calculate pending state from the set of locally pending
 * signals, globally pending signals, and blocked signals.
 */
static inline int has_pending_signals(sigset_t *signal, sigset_t *blocked)
{
	unsigned long ready;
	long i;

	switch (_NSIG_WORDS) {
	default:
		for (i = _NSIG_WORDS, ready = 0; --i >= 0 ;)
			ready |= signal->sig[i] &~ blocked->sig[i];
		break;

	case 4: ready  = signal->sig[3] &~ blocked->sig[3];
		ready |= signal->sig[2] &~ blocked->sig[2];
		ready |= signal->sig[1] &~ blocked->sig[1];
		ready |= signal->sig[0] &~ blocked->sig[0];
		break;

	case 2: ready  = signal->sig[1] &~ blocked->sig[1];
		ready |= signal->sig[0] &~ blocked->sig[0];
		break;

	case 1: ready  = signal->sig[0] &~ blocked->sig[0];
	}
	return ready !=	0;
}

#define PENDING(p,b) has_pending_signals(&(p)->signal, (b))

/**
 * 检查进程是否有挂起的信号*（检查t->pending->signal），或者进程所属的线程组有挂起的信号（检查t->signal->shared_pending->signal）。
 * 然后把t->thread_info->flags的TIF_SIGPENDING标志置位(如果有就置位)。
 */
fastcall void recalc_sigpending_tsk(struct task_struct *t)
{
	if (t->signal->group_stop_count > 0 ||
	    PENDING(&t->pending, &t->blocked) ||
	    PENDING(&t->signal->shared_pending, &t->blocked))
		set_tsk_thread_flag(t, TIF_SIGPENDING);
	else
		clear_tsk_thread_flag(t, TIF_SIGPENDING);
}

/**
 * 等价于recalc_sigpending_tsk(current);
 */
void recalc_sigpending(void)
{
	recalc_sigpending_tsk(current);
}

/* Given the mask, find the first available signal that should be serviced. */

static int
next_signal(struct sigpending *pending, sigset_t *mask)
{
	unsigned long i, *s, *m, x;
	int sig = 0;
	
	s = pending->signal.sig;
	m = mask->sig;
	switch (_NSIG_WORDS) {
	default:
		for (i = 0; i < _NSIG_WORDS; ++i, ++s, ++m)
			if ((x = *s &~ *m) != 0) {
				sig = ffz(~x) + i*_NSIG_BPW + 1;
				break;
			}
		break;

	case 2: if ((x = s[0] &~ m[0]) != 0)
			sig = 1;
		else if ((x = s[1] &~ m[1]) != 0)
			sig = _NSIG_BPW + 1;
		else
			break;
		sig += ffz(~x);
		break;

	case 1: if ((x = *s &~ *m) != 0)
			sig = ffz(~x) + 1;
		break;
	}
	
	return sig;
}

static struct sigqueue *__sigqueue_alloc(struct task_struct *t, int flags)
{
	struct sigqueue *q = NULL;

	if (atomic_read(&t->user->sigpending) <
			t->signal->rlim[RLIMIT_SIGPENDING].rlim_cur)
		q = kmem_cache_alloc(sigqueue_cachep, flags);
	if (q) {
		INIT_LIST_HEAD(&q->list);
		q->flags = 0;
		q->lock = NULL;
		q->user = get_uid(t->user);
		atomic_inc(&q->user->sigpending);
	}
	return(q);
}

static inline void __sigqueue_free(struct sigqueue *q)
{
	if (q->flags & SIGQUEUE_PREALLOC)
		return;
	atomic_dec(&q->user->sigpending);
	free_uid(q->user);
	kmem_cache_free(sigqueue_cachep, q);
}

/**
 * 从挂起信号队列中删除所有的挂起信号。
 */
static void flush_sigqueue(struct sigpending *queue)
{
	struct sigqueue *q;

	sigemptyset(&queue->signal);
	while (!list_empty(&queue->list)) {
		q = list_entry(queue->list.next, struct sigqueue , list);
		list_del_init(&q->list);
		__sigqueue_free(q);
	}
}

/*
 * Flush all pending signals for a task.
 */

/**
 * 删除发送给进程的所有信号。这是通过清除t->thread_info->flags中的TIF_SIGPENDING标志，
 * 并在t->pending和t->signal->shared_pending上两次调用flush_sigqueue实现的。
 */
void
flush_signals(struct task_struct *t)
{
	unsigned long flags;

	spin_lock_irqsave(&t->sighand->siglock, flags);
	clear_tsk_thread_flag(t,TIF_SIGPENDING);
	flush_sigqueue(&t->pending);
	flush_sigqueue(&t->signal->shared_pending);
	spin_unlock_irqrestore(&t->sighand->siglock, flags);
}

/*
 * This function expects the tasklist_lock write-locked.
 */
void __exit_sighand(struct task_struct *tsk)
{
	struct sighand_struct * sighand = tsk->sighand;

	/* Ok, we're done with the signal handlers */
	tsk->sighand = NULL;
	if (atomic_dec_and_test(&sighand->count))
		kmem_cache_free(sighand_cachep, sighand);
}

void exit_sighand(struct task_struct *tsk)
{
	write_lock_irq(&tasklist_lock);
	__exit_sighand(tsk);
	write_unlock_irq(&tasklist_lock);
}

/*
 * This function expects the tasklist_lock write-locked.
 */
/**
 * 删除所有的挂起信号并释放进程的signal_struct描述符。如果描述符不被其他线程使用，删除该描述符。
 */
void __exit_signal(struct task_struct *tsk)
{
	struct signal_struct * sig = tsk->signal;
	struct sighand_struct * sighand = tsk->sighand;

	if (!sig)
		BUG();
	if (!atomic_read(&sig->count))
		BUG();
	spin_lock(&sighand->siglock);
	if (atomic_dec_and_test(&sig->count)) {
		if (tsk == sig->curr_target)
			sig->curr_target = next_thread(tsk);
		tsk->signal = NULL;
		spin_unlock(&sighand->siglock);
		flush_sigqueue(&sig->shared_pending);
	} else {
		/*
		 * If there is any task waiting for the group exit
		 * then notify it:
		 */
		if (sig->group_exit_task && atomic_read(&sig->count) == sig->notify_count) {
			wake_up_process(sig->group_exit_task);
			sig->group_exit_task = NULL;
		}
		if (tsk == sig->curr_target)
			sig->curr_target = next_thread(tsk);
		tsk->signal = NULL;
		/*
		 * Accumulate here the counters for all threads but the
		 * group leader as they die, so they can be added into
		 * the process-wide totals when those are taken.
		 * The group leader stays around as a zombie as long
		 * as there are other threads.  When it gets reaped,
		 * the exit.c code will add its counts into these totals.
		 * We won't ever get here for the group leader, since it
		 * will have been the last reference on the signal_struct.
		 */
		sig->utime = cputime_add(sig->utime, tsk->utime);
		sig->stime = cputime_add(sig->stime, tsk->stime);
		sig->min_flt += tsk->min_flt;
		sig->maj_flt += tsk->maj_flt;
		sig->nvcsw += tsk->nvcsw;
		sig->nivcsw += tsk->nivcsw;
		spin_unlock(&sighand->siglock);
		sig = NULL;	/* Marker for below.  */
	}
	clear_tsk_thread_flag(tsk,TIF_SIGPENDING);
	flush_sigqueue(&tsk->pending);
	if (sig) {
		/*
		 * We are cleaning up the signal_struct here.  We delayed
		 * calling exit_itimers until after flush_sigqueue, just in
		 * case our thread-local pending queue contained a queued
		 * timer signal that would have been cleared in
		 * exit_itimers.  When that called sigqueue_free, it would
		 * attempt to re-take the tasklist_lock and deadlock.  This
		 * can never happen if we ensure that all queues the
		 * timer's signal might be queued on have been flushed
		 * first.  The shared_pending queue, and our own pending
		 * queue are the only queues the timer could be on, since
		 * there are no other threads left in the group and timer
		 * signals are constrained to threads inside the group.
		 */
		exit_itimers(sig);
		kmem_cache_free(signal_cachep, sig);
	}
}

void exit_signal(struct task_struct *tsk)
{
	write_lock_irq(&tasklist_lock);
	__exit_signal(tsk);
	write_unlock_irq(&tasklist_lock);
}

/*
 * Flush all handlers for a task.
 */

void
flush_signal_handlers(struct task_struct *t, int force_default)
{
	int i;
	struct k_sigaction *ka = &t->sighand->action[0];
	for (i = _NSIG ; i != 0 ; i--) {
		if (force_default || ka->sa.sa_handler != SIG_IGN)
			ka->sa.sa_handler = SIG_DFL;
		ka->sa.sa_flags = 0;
		sigemptyset(&ka->sa.sa_mask);
		ka++;
	}
}


/* Notify the system that a driver wants to block all signals for this
 * process, and wants to be notified if any signals at all were to be
 * sent/acted upon.  If the notifier routine returns non-zero, then the
 * signal will be acted upon after all.  If the notifier routine returns 0,
 * then then signal will be blocked.  Only one block per process is
 * allowed.  priv is a pointer to private data that the notifier routine
 * can use to determine if the signal should be blocked or not.  */

void
block_all_signals(int (*notifier)(void *priv), void *priv, sigset_t *mask)
{
	unsigned long flags;

	spin_lock_irqsave(&current->sighand->siglock, flags);
	current->notifier_mask = mask;
	current->notifier_data = priv;
	current->notifier = notifier;
	spin_unlock_irqrestore(&current->sighand->siglock, flags);
}

/* Notify the system that blocking has ended. */

void
unblock_all_signals(void)
{
	unsigned long flags;

	spin_lock_irqsave(&current->sighand->siglock, flags);
	current->notifier = NULL;
	current->notifier_data = NULL;
	recalc_sigpending();
	spin_unlock_irqrestore(&current->sighand->siglock, flags);
}

static inline int collect_signal(int sig, struct sigpending *list, siginfo_t *info)
{
	struct sigqueue *q, *first = NULL;
	int still_pending = 0;

	if (unlikely(!sigismember(&list->signal, sig)))
		return 0;

	/*
	 * Collect the siginfo appropriate to this signal.  Check if
	 * there is another siginfo for the same signal.
	*/
	/*
	 * 遍历信号队列，找到第一个目标信号元素
	 * 如果存在多个相同信号，signal成员不清除相应的位(实时信号)
	 * 一般的信号，从signal中清楚相应的位
	 */
	list_for_each_entry(q, &list->list, list) {
		if (q->info.si_signo == sig) {
			if (first) {
				still_pending = 1;
				break;
			}
			first = q;
		}
	}
	if (first) {
		list_del_init(&first->list);
		copy_siginfo(info, &first->info);
		__sigqueue_free(first);
		if (!still_pending)
			sigdelset(&list->signal, sig);
	} else {

		/* Ok, it wasn't in the queue.  This must be
		   a fast-pathed signal or we must have been
		   out of queue space.  So zero out the info.
		 */
		sigdelset(&list->signal, sig);
		info->si_signo = sig;
		info->si_errno = 0;
		info->si_code = 0;
		info->si_pid = 0;
		info->si_uid = 0;
	}
	return 1;
}

static int __dequeue_signal(struct sigpending *pending, sigset_t *mask,
			siginfo_t *info)
{
	int sig = 0;

	/*
	 * 从信号队列中取得下一个待处理的信号
	 */
	sig = next_signal(pending, mask);
	if (sig) {
		if (current->notifier) {
			if (sigismember(current->notifier_mask, sig)) {
				if (!(current->notifier)(current->notifier_data)) {
					clear_thread_flag(TIF_SIGPENDING);
					return 0;
				}
			}
		}

		/*
		 * 清除current->pending.signal  或者current->signal->share_pending.signal中对应的位
		 */
		if (!collect_signal(sig, pending, info))
			sig = 0;
				
	}
	/**
	 * 更新TIF_SIGPENDING标志
	 */
	recalc_sigpending();

	return sig;
}

/*
 * Dequeue a signal and return the element to the caller, which is 
 * expected to free it.
 *
 * All callers have to hold the siglock.
 */
/**
 * dequeue_signal被do_signal函数重复调用。
 * 返回0表示所有挂起的信号都被处理，非0表示挂起的信号正等待被处理。
 */
int dequeue_signal(struct task_struct *tsk, sigset_t *mask, siginfo_t *info)
{
	/**
	 * 首先考虑私有挂起信号队列中的所有信号。并从最低编号的挂起信号开始。
	 */
	int signr = __dequeue_signal(&tsk->pending, mask, info);
	if (!signr)
		/**
		 * 然后考虑共享队列中的信号。
		 */
		signr = __dequeue_signal(&tsk->signal->shared_pending,
					 mask, info);
 	if (signr && unlikely(sig_kernel_stop(signr))) {
 		/*
 		 * Set a marker that we have dequeued a stop signal.  Our
 		 * caller might release the siglock and then the pending
 		 * stop signal it is about to process is no longer in the
 		 * pending bitmasks, but must still be cleared by a SIGCONT
 		 * (and overruled by a SIGKILL).  So those cases clear this
 		 * shared flag after we've set it.  Note that this flag may
 		 * remain set after the signal we return is ignored or
 		 * handled.  That doesn't matter because its only purpose
 		 * is to alert stop-signal processing code when another
 		 * processor has come along and cleared the flag.
 		 */
 		tsk->signal->flags |= SIGNAL_STOP_DEQUEUED;
 	}
	if ( signr &&
	     ((info->si_code & __SI_MASK) == __SI_TIMER) &&
	     info->si_sys_private){
		do_schedule_next_timer(info);
	}
	/*
	 * 返回要处理的信号
	 * 返回0表示所有挂起的信号都被处理，非0表示挂起的信号正等待被处理。
	 */
	return signr;
}

/*
 * Tell a process that it has a new active signal..
 *
 * NOTE! we rely on the previous spin_lock to
 * lock interrupts for us! We can only be called with
 * "siglock" held, and the local interrupt must
 * have been disabled when that got acquired!
 *
 * No need to set need_resched since signal event passing
 * goes through ->blocked
 */
/**
 * 通知进程有新的挂起信号。
 */
void signal_wake_up(struct task_struct *t, int resume)
{
	unsigned int mask;

	/**
	 * 首先设置TIF_SIGPENDING标志。
	 */
	set_tsk_thread_flag(t, TIF_SIGPENDING);

	/*
	 * For SIGKILL, we want to wake it up in the stopped/traced case.
	 * We don't check t->state here because there is a race with it
	 * executing another processor and just now entering stopped state.
	 * By using wake_up_state, we ensure the process will wake up and
	 * handle its death signal.
	 */
	/*
	 * 如果进程处于TASK_INTERRUPTIBLE 或TASK_STOPPED状态，而且信号是SIGKILL
	 * 就调用try_to_wake_up唤醒进程
	 */
	/**
	 * 只要是TASK_INTERRUPTIBLE状态，都可以唤醒。
	 */
	mask = TASK_INTERRUPTIBLE;
	/**
	 * 如果信号是SIGKILL，就处理TASK_STOPPED和TASK_TRACED状态。
	 */
	if (resume)
		mask |= TASK_STOPPED | TASK_TRACED;
	/**
	 * 如果try_to_wake_up返回0，说明进程是可运行的。
	 * 检查进程是否在另外的CPU上运行，如果是就向CPU发送中断，以强制当前进程重新调度。
	 * 这样，调度程序返回时，会检查目标进程挂起的信号。
	 */
	if (!wake_up_state(t, mask))
		kick_process(t);
}

/*
 * Remove signals in mask from the pending set and queue.
 * Returns 1 if any signals were found.
 *
 * All callers must be holding the siglock.
 */
/**
 * 从挂起信号队列中删除与mask位掩码对应的挂起信号
 */
static int rm_from_queue(unsigned long mask, struct sigpending *s)
{
	struct sigqueue *q, *n;

	if (!sigtestsetmask(&s->signal, mask))
		return 0;

	sigdelsetmask(&s->signal, mask);
	list_for_each_entry_safe(q, n, &s->list, list) {
		if (q->info.si_signo < SIGRTMIN &&
		    (mask & sigmask(q->info.si_signo))) {
			list_del_init(&q->list);
			__sigqueue_free(q);
		}
	}
	return 1;
}

/*
 * Bad permissions for sending the signal
 */
static int check_kill_permission(int sig, struct siginfo *info,
				 struct task_struct *t)
{
	int error = -EINVAL;
	/**
	 * 检查参数是否正确，如果不正确，会导致个层函数Group_send_sig_info也退出
	 */
	if (sig < 0 || sig > _NSIG)
		return error;
	error = -EPERM;
	/**
	 * 如果是用户态进程发送的，检测是否有这个权限。
	 * 必须满足以下几个条件之一：
	 *     1、发送进程的拥有者有适当的权限，如管理员
	 *     2、信号为SIGCONT且目标进程与发送进程处于同一个注册会话中。
	 *     3、两个进程属于同一个用户。
	 */
	if ((!info || ((unsigned long)info != 1 &&
			(unsigned long)info != 2 && SI_FROMUSER(info)))
	    && ((sig != SIGCONT) ||
		(current->signal->session != t->signal->session))
	    && (current->euid ^ t->suid) && (current->euid ^ t->uid)
	    && (current->uid ^ t->suid) && (current->uid ^ t->uid)
	    && !capable(CAP_KILL))
	    /**
	     * 不允许发送信号，返回EPERM
	     */
		return error;
	/**
	 * 可以使用自定义的安全策略，防止发送KILL信号。
	 * security_task_kill检查这样的安全策略。
	 */
	return security_task_kill(t, info, sig);
}

/* forward decl */
static void do_notify_parent_cldstop(struct task_struct *tsk,
				     struct task_struct *parent,
				     int why);

/*
 * Handle magic process-wide effects of stop/continue signals.
 * Unlike the signal actions, these happen immediately at signal-generation
 * time regardless of blocking, ignoring, or handling.  This does the
 * actual continuing for SIGCONT, but not the actual stopping for stop
 * signals.  The process stop is done as a signal action for SIG_DFL.
 */
/**
 * 检查信号类型，如果有必要，就让目标线程组的其他信号无效。
 */
static void handle_stop_signal(int sig, struct task_struct *p)
{
	struct task_struct *t;

	/**
	 * 线程组正在被杀死。函数直接返回。
	 */
	if (p->flags & SIGNAL_GROUP_EXIT)
		/*
		 * The process is in the middle of dying already.
		 */
		return;

	/**
	 * 如果是SIGSTOP，SIGTSTP，SIGTTIN，SIGTTOUT信号
	 * 就调用rm_from_queue从共享挂起信号队列中删除所有SIGCONT
	 * 然后从属于线程组的进程的私有挂起信号队列中删除上述信号。
	 */
	if (sig_kernel_stop(sig)) {
		/*
		 * This is a stop signal.  Remove SIGCONT from all queues.
		 */
		rm_from_queue(sigmask(SIGCONT), &p->signal->shared_pending);
		t = p;
		do {
			rm_from_queue(sigmask(SIGCONT), &t->pending);
			t = next_thread(t);
		} while (t != p);
	} else if (sig == SIGCONT) {
		/**
		 * 如果是SIGCONT信号
		 * 就调用rm_from_queue从共享挂起信号队列中删除所有SIGSTOP，SIGTSTP，SIGTTIN，SIGTTOUT
		 * 然后从属于线程组的进程的私有挂起信号队列中删除上述信号，并唤醒进程
		 */

		/*
		 * Remove all stop signals from all queues,
		 * and wake all threads.
		 */
		if (unlikely(p->signal->group_stop_count > 0)) {
			/*
			 * There was a group stop in progress.  We'll
			 * pretend it finished before we got here.  We are
			 * obliged to report it to the parent: if the
			 * SIGSTOP happened "after" this SIGCONT, then it
			 * would have cleared this pending SIGCONT.  If it
			 * happened "before" this SIGCONT, then the parent
			 * got the SIGCHLD about the stop finishing before
			 * the continue happened.  We do the notification
			 * now, and it's as if the stop had finished and
			 * the SIGCHLD was pending on entry to this kill.
			 */
			p->signal->group_stop_count = 0;
			p->signal->flags = SIGNAL_STOP_CONTINUED;
			spin_unlock(&p->sighand->siglock);
			if (p->ptrace & PT_PTRACED)
				do_notify_parent_cldstop(p, p->parent,
							 CLD_STOPPED);
			else
				do_notify_parent_cldstop(
					p->group_leader,
					p->group_leader->real_parent,
							 CLD_STOPPED);
			spin_lock(&p->sighand->siglock);
		}
		rm_from_queue(SIG_KERNEL_STOP_MASK, &p->signal->shared_pending);
		t = p;
		do {
			unsigned int state;
			rm_from_queue(SIG_KERNEL_STOP_MASK, &t->pending);
			
			/*
			 * If there is a handler for SIGCONT, we must make
			 * sure that no thread returns to user mode before
			 * we post the signal, in case it was the only
			 * thread eligible to run the signal handler--then
			 * it must not do anything between resuming and
			 * running the handler.  With the TIF_SIGPENDING
			 * flag set, the thread will pause and acquire the
			 * siglock that we hold now and until we've queued
			 * the pending signal. 
			 *
			 * Wake up the stopped thread _after_ setting
			 * TIF_SIGPENDING
			 */
			state = TASK_STOPPED;
			if (sig_user_defined(t, SIGCONT) && !sigismember(&t->blocked, SIGCONT)) {
				set_tsk_thread_flag(t, TIF_SIGPENDING);
				state |= TASK_INTERRUPTIBLE;
			}
			wake_up_state(t, state);

			t = next_thread(t);
		} while (t != p);

		if (p->signal->flags & SIGNAL_STOP_STOPPED) {
			/*
			 * We were in fact stopped, and are now continued.
			 * Notify the parent with CLD_CONTINUED.
			 */
			p->signal->flags = SIGNAL_STOP_CONTINUED;
			p->signal->group_exit_code = 0;
			spin_unlock(&p->sighand->siglock);
			if (p->ptrace & PT_PTRACED)
				do_notify_parent_cldstop(p, p->parent,
							 CLD_CONTINUED);
			else
				do_notify_parent_cldstop(
					p->group_leader,
					p->group_leader->real_parent,
							 CLD_CONTINUED);
			spin_lock(&p->sighand->siglock);
		} else {
			/*
			 * We are not stopped, but there could be a stop
			 * signal in the middle of being processed after
			 * being removed from the queue.  Clear that too.
			 */
			p->signal->flags = 0;
		}
	} else if (sig == SIGKILL) {
		/*
		 * Make sure that any pending stop signal already dequeued
		 * is undone by the wakeup for SIGKILL.
		 */
		p->signal->flags = 0;
	}
}

/**
 * 在挂起信号队列中插入一个新元素。
 * sig-信号编号
 * info-或者是siginfo_t的地址，或者是以下值
 *     0：信号是由用户态进程发送的。
 *     1：信号是由内核发送的。
 *     2：由内核发送SIGSTOP或者SIGKILL信号。
 * t-目标进程描述符
 * signals-挂起信号队列的地址。
 */
static int send_signal(int sig, struct siginfo *info, struct task_struct *t,
			struct sigpending *signals)
{
	struct sigqueue * q = NULL;
	int ret = 0;

	/*
	 * fast-pathed signals for kernel-internal things like SIGSTOP
	 * or SIGKILL.
	 */
	/**
	 * info == 2，表示是由内核通过force_sig_specific函数发送的SIGKILL或者SIGSTOP。
	 * 此时，直接跳转到out_set，内核强制执行与这些信号相关的操作。而不会将信号加入到挂起队列。
	 * 此处这样处理有一个重要的目的：如果恶意进程占用了太多内核，而SIGKILL和SIGSTOP信号
	 * 还必须分配内存的话，SIGKILL信号就可能无法传递。管理员也就无法杀死恶意进程了。
	 */
	if ((unsigned long)info == 2)
		goto out_set;

	/* Real-time signals must be queued if sent by sigqueue, or
	   some other real-time mechanism.  It is implementation
	   defined whether kill() does so.  We attempt to do so, on
	   the principle of least surprise, but since kill is not
	   allowed to fail with EAGAIN when low on memory we just
	   make sure at least one signal gets delivered and don't
	   pass on the info struct.  */

	/**
	 * 如果进程的挂起信号数量小于进程的资源限制，就为新信号分配sigqueue数据结构。
	 * 如果分配成功了，还会递增user的引用计数和挂起信号数量。
	 */
	q = __sigqueue_alloc(t, GFP_ATOMIC);
	/**
	 * 如果超出限制，或者不能分配内存了，就跳到out_set
	 */
	if (q) {
		/**
		 * 在挂起信号队列中增加sigqueue.
		 */
		list_add_tail(&q->list, &signals->list);
		/**
		 * 填充info数据结构。
		 */
		switch ((unsigned long) info) {
		case 0:
			q->info.si_signo = sig;
			q->info.si_errno = 0;
			q->info.si_code = SI_USER;
			q->info.si_pid = current->pid;
			q->info.si_uid = current->uid;
			break;
		case 1:
			q->info.si_signo = sig;
			q->info.si_errno = 0;
			q->info.si_code = SI_KERNEL;
			q->info.si_pid = 0;
			q->info.si_uid = 0;
			break;
		default:
			/**
			 * 复制由调用者传递的siginfo_t表。
			 */
			copy_siginfo(&q->info, info);
			break;
		}
	} else {
		/**
		 * 挂起信号超过限制，或者分配内存失败。
		 * 此时，不再向信号挂起队列中增加元素。
		 */

		/**
		 * 信号是实时的，并且是通过内核函数发送的，就返回EAGAIN
		 */
		if (sig >= SIGRTMIN && info && (unsigned long)info != 1
		   && info->si_code != SI_USER)
		/*
		 * Queue overflow, abort.  We may abort if the signal was rt
		 * and sent by user using something other than kill().
		 */
			return -EAGAIN;
		if (((unsigned long)info > 1) && (info->si_code == SI_TIMER))
			/*
			 * Set up a return to indicate that we dropped 
			 * the signal.
			 */
			ret = info->si_sys_private;
	}

out_set:
	/**
	 * sigaddset把队列位掩码中与信号相对的位置1。
	 */
	sigaddset(&signals->signal, sig);
	return ret;
}

#define LEGACY_QUEUE(sigptr, sig) \
	(((sig) < SIGRTMIN) && sigismember(&(sigptr)->signal, (sig)))

/**
 * 向指定的进程发送信号。
 * sig-信号编号
 * info-或者是siginfo_t的地址，或者是以下值
 *     0：信号是由用户态进程发送的。
 *     1：信号是由内核发送的。
 *     2：由内核发送SIGSTOP或者SIGKILL信号。
 * t-目标进程描述符
 * 注意：必须在关本地中断和获得t->sighand->siglock自旋锁的情况下调用本函数。
 */
static int
specific_send_sig_info(int sig, struct siginfo *info, struct task_struct *t)
{
	int ret = 0;

	/**
	 * 再次确保本地中断被关闭。
	 */
	if (!irqs_disabled())
		BUG();
	/**
	 * 再次确保siglock被获得。
	 */
	assert_spin_locked(&t->sighand->siglock);

	if (((unsigned long)info > 2) && (info->si_code == SI_TIMER))
		/*
		 * Set up a return to indicate that we dropped the signal.
		 */
		ret = info->si_sys_private;

	/* Short-circuit ignored signals.  */
	/**
	 * 检查进程是否忽略信号。如果忽略就返回0。
	 * 当以下条件满足时，信号被忽略：
	 *     1：进程没有被跟踪
	 *     2：信号没有被阻塞。
	 *     3：显式的忽略了信号。或者隐含的忽略了信号。
	 */
	if (sig_ignored(t, sig))
		goto out;

	/* Support queueing exactly one non-rt signal, so that we
	   can get more detailed information about the cause of
	   the signal. */
	/**
	 * 检查信号是否是非实时的。
	 * 而且在进程的私有挂起信号队列上已经有另外一个相同的挂起信号。
	 * 如果是这样，也返回。
	 */
	if (LEGACY_QUEUE(&t->pending, sig))
		goto out;

	/**
	 * 调用send_signal把信号添加到进程的挂起信号集合中。
	 */
	ret = send_signal(sig, info, t, &t->pending);
	/**
	 * 如果send_signal成功，并且信号不被阻塞
	 * 就调用signal_wake_up函数通知进程有新的挂起信号。
	 */
	if (!ret && !sigismember(&t->blocked, sig))
		signal_wake_up(t, sig == SIGKILL);
out:
	return ret;
}

/*
 * Force a signal that the process can't ignore: if necessary
 * we unblock the signal and change any SIG_IGN to SIG_DFL.
 */

/**
 * 与force_sig类似，但是还使用siginfo_t结构中的扩展信息
 */
int
force_sig_info(int sig, struct siginfo *info, struct task_struct *t)
{
	unsigned long int flags;
	int ret;

	spin_lock_irqsave(&t->sighand->siglock, flags);
	if (sigismember(&t->blocked, sig) || t->sighand->action[sig-1].sa.sa_handler == SIG_IGN) {
		t->sighand->action[sig-1].sa.sa_handler = SIG_DFL;
		sigdelset(&t->blocked, sig);
		recalc_sigpending_tsk(t);
	}
	ret = specific_send_sig_info(sig, info, t);
	spin_unlock_irqrestore(&t->sighand->siglock, flags);

	return ret;
}

/**
 * 与force_sig类似，但是优化了对SIGSTOP和SIGKILL信号的处理
 */ 
void
force_sig_specific(int sig, struct task_struct *t)
{
	unsigned long int flags;

	spin_lock_irqsave(&t->sighand->siglock, flags);
	if (t->sighand->action[sig-1].sa.sa_handler == SIG_IGN)
		t->sighand->action[sig-1].sa.sa_handler = SIG_DFL;
	sigdelset(&t->blocked, sig);
	recalc_sigpending_tsk(t);
	specific_send_sig_info(sig, (void *)2, t);
	spin_unlock_irqrestore(&t->sighand->siglock, flags);
}

/*
 * Test if P wants to take SIG.  After we've checked all threads with this,
 * it's equivalent to finding no threads not blocking SIG.  Any threads not
 * blocking SIG were ruled out because they are not running and already
 * have pending signals.  Such threads will dequeue from the shared queue
 * as soon as they're available, so putting the signal on the shared queue
 * will be equivalent to sending it to one such thread.
 */
#define wants_signal(sig, p, mask) 			\
	(!sigismember(&(p)->blocked, sig)		\
	 && !((p)->state & mask)			\
	 && !((p)->flags & PF_EXITING)			\
	 && (task_curr(p) || !signal_pending(p)))

/**
 * 扫描线程组中的进程，查找能够接收新信号的进程。
 */
static void
__group_complete_signal(int sig, struct task_struct *p)
{
	unsigned int mask;
	struct task_struct *t;

	/*
	 * Don't bother traced and stopped tasks (but
	 * SIGKILL will punch through that).
	 */
	/**
	 * 不选择TASK_STOPPED | TASK_TRACED状态的进程
	 */
	mask = TASK_STOPPED | TASK_TRACED;
	/**
	 * 只要是SIGKILL信号，就随便选择一个进程，而不管这个进程的状态。
	 */
	if (sig == SIGKILL)
		mask = 0;

	/*
	 * Now find a thread we can wake up to take the signal off the queue.
	 *
	 * If the main thread wants the signal, it gets first crack.
	 * Probably the least surprising to the average bear.
	 */
	/**
	 * 如果接收信号的进程满足所有规则，则优先选择这个进程。
	 * wants_signal判断以下条件：
	 *	 进程不阻塞信号
	 *     进程没有被杀死即PF_EXITING标志没有设置。
	 *     进程或者正在CPU上运行(运行状态不在mask中)。或者它的TIF_SIGPENDING标志没有被设置。
	 *     唤醒有TIF_SIGPENDING标志的进程是没有意义的。
	 */
	if (wants_signal(sig, p, mask))
		t = p;
	else if (thread_group_empty(p))
		/*
		 * There is just one thread and it does not need to be woken.
		 * It will dequeue unblocked signals before it runs again.
		 */
		return;
	else {
		/*
		 * Otherwise try to find a suitable thread.
		 */
		/**
		 * 从线程组中，上次接收信号的进程开始搜索
		 */
		t = p->signal->curr_target;
		if (t == NULL)
			/* restart balancing at this thread */
			t = p->signal->curr_target = p;
		BUG_ON(t->tgid != p->tgid);

		while (!wants_signal(sig, t, mask)) {
			t = next_thread(t);
			if (t == p->signal->curr_target)
				/*
				 * No thread needs to be woken.
				 * Any eligible threads will see
				 * the signal in the queue soon.
				 */
				return;
		}
		/**
		 * 搜索到一个符合条件的进程，将它选择出来，唤醒它，并将它设置到curr_target中。
		 */
		p->signal->curr_target = t;
	}

	/*
	 * Found a killable thread.  If the signal will be fatal,
	 * then start taking the whole group down immediately.
	 */
	/**
	 * 找到了一个合适的进程。就开始向被选中的进程传递信号。
	 */
	/**
	 * 如果信号是致命的，就向进程组的所有轻量级进程发送SIGKILL信号杀死整个线程组。
	 */
	if (sig_fatal(p, sig) && !(p->signal->flags & SIGNAL_GROUP_EXIT) &&
	    !sigismember(&t->real_blocked, sig) &&
	    (sig == SIGKILL || !(t->ptrace & PT_PTRACED))) {
		/*
		 * This signal will be fatal to the whole group.
		 */
		if (!sig_kernel_coredump(sig)) {
			/*
			 * Start a group exit and wake everybody up.
			 * This way we don't have other threads
			 * running and doing things after a slower
			 * thread has the fatal signal pending.
			 */
			p->signal->flags = SIGNAL_GROUP_EXIT;
			p->signal->group_exit_code = sig;
			p->signal->group_stop_count = 0;
			t = p;
			do {
				sigaddset(&t->pending.signal, SIGKILL);
				signal_wake_up(t, 1);
				t = next_thread(t);
			} while (t != p);
			return;
		}

		/*
		 * There will be a core dump.  We make all threads other
		 * than the chosen one go into a group stop so that nothing
		 * happens until it gets scheduled, takes the signal off
		 * the shared queue, and does the core dump.  This is a
		 * little more complicated than strictly necessary, but it
		 * keeps the signal state that winds up in the core dump
		 * unchanged from the death state, e.g. which thread had
		 * the core-dump signal unblocked.
		 */
		rm_from_queue(SIG_KERNEL_STOP_MASK, &t->pending);
		rm_from_queue(SIG_KERNEL_STOP_MASK, &p->signal->shared_pending);
		p->signal->group_stop_count = 0;
		p->signal->group_exit_task = t;
		t = p;
		do {
			p->signal->group_stop_count++;
			signal_wake_up(t, 0);
			t = next_thread(t);
		} while (t != p);
		wake_up_process(p->signal->group_exit_task);
		return;
	}

	/*
	 * The signal is already in the shared-pending queue.
	 * Tell the chosen thread to wake up and dequeue it.
	 */
	/**
	 * 信号不是致命的，则通知被选中的进程：有新的挂起信号到来。
	 */
	signal_wake_up(t, sig == SIGKILL);
	return;
}

/*
 * 真正的发送信号
 */
static int
__group_send_sig_info(int sig, struct siginfo *info, struct task_struct *p)
{
	int ret = 0;

	assert_spin_locked(&p->sighand->siglock);
	/**
	 * 调用handle_stop_signal，检查信号类型，这些类型可能使目标线程组的其他挂起信号无效。
	 */
	handle_stop_signal(sig, p);

	if (((unsigned long)info > 2) && (info->si_code == SI_TIMER))
		/*
		 * Set up a return to indicate that we dropped the signal.
		 */
		ret = info->si_sys_private;

	/* Short-circuit ignored signals.  */
	/**
	 * 如果线程组忽略信号，就返回0。
	 */
	if (sig_ignored(p, sig))
		return ret;

	/**
	 * 如果是非实时进程，并且在共享队列中已经有同一个信号，就什么都不做。直接返回0
	 */
	if (LEGACY_QUEUE(&p->signal->shared_pending, sig))
		/* This is a non-RT signal and we already have one queued.  */
		return ret;

	/*
	 * Put this signal on the shared-pending queue, or fail with EAGAIN.
	 * We always use the shared queue for process-wide signals,
	 * to avoid several races.
	 */
	/**
	 * 调用send_signal把信号添加到共享挂起信号队列中。
	 */
	ret = send_signal(sig, info, p, &p->signal->shared_pending);
	if (unlikely(ret))
		return ret;

	/**
	 * __group_complete_signal唤醒线程组中的一个轻量级进程。
	 */
	__group_complete_signal(sig, p);
	return 0;
}

/*
 * Nuke all other threads in the group.
 */
void zap_other_threads(struct task_struct *p)
{
	struct task_struct *t;

	p->signal->flags = SIGNAL_GROUP_EXIT;
	p->signal->group_stop_count = 0;

	if (thread_group_empty(p))
		return;

	for (t = next_thread(p); t != p; t = next_thread(t)) {
		/*
		 * Don't bother with already dead threads
		 */
		if (t->exit_state)
			continue;

		/*
		 * We don't want to notify the parent, since we are
		 * killed as part of a thread group due to another
		 * thread doing an execve() or similar. So set the
		 * exit signal to -1 to allow immediate reaping of
		 * the process.  But don't detach the thread group
		 * leader.
		 */
		if (t != p->group_leader)
			t->exit_signal = -1;

		sigaddset(&t->pending.signal, SIGKILL);
		rm_from_queue(SIG_KERNEL_STOP_MASK, &t->pending);
		signal_wake_up(t, 1);
	}
}

/*
 * Must be called with the tasklist_lock held for reading!
 */
/**
 * 向整个线程组发送信号。
 */
int group_send_sig_info(int sig, struct siginfo *info, struct task_struct *p)
{
	unsigned long flags;
	int ret;

	/**
	 * 首先检查必要的权限。
	 */
	ret = check_kill_permission(sig, info, p);
	/**
	 * 再判断sig是否为0，是0也不产生任何信号。因为0是无效的信号。check_kill_permission中只判断了<0的情况。
	 * 但是信号0有特殊的作用：用户可以发送，以检查是否有权限发送信号。
	 * p->sighand == NULL表示进程正在被杀死，那么函数也返回。
	 */
	if (!ret && sig && p->sighand) {
		/**
		 * 通过了权限检查，那么关闭自旋锁和本地中断，并调用__group_send_sig_info真正的发送信号
		 */
		spin_lock_irqsave(&p->sighand->siglock, flags);
		ret = __group_send_sig_info(sig, info, p);
		spin_unlock_irqrestore(&p->sighand->siglock, flags);
	}

	return ret;
}

/*
 * kill_pg_info() sends a signal to a process group: this is what the tty
 * control characters do (^C, ^Z etc)
 */

int __kill_pg_info(int sig, struct siginfo *info, pid_t pgrp)
{
	struct task_struct *p = NULL;
	int retval, success;

	if (pgrp <= 0)
		return -EINVAL;

	success = 0;
	retval = -ESRCH;
	do_each_task_pid(pgrp, PIDTYPE_PGID, p) {
		int err = group_send_sig_info(sig, info, p);
		success |= !err;
		retval = err;
	} while_each_task_pid(pgrp, PIDTYPE_PGID, p);
	return success ? 0 : retval;
}

/**
 * 与kill_pg类似，但是还使用扩展信息
 */
int
kill_pg_info(int sig, struct siginfo *info, pid_t pgrp)
{
	int retval;

	read_lock(&tasklist_lock);
	retval = __kill_pg_info(sig, info, pgrp);
	read_unlock(&tasklist_lock);

	return retval;
}

/**
 * 与kill_proc类似，但是还使用扩展信息。
 */
int
kill_proc_info(int sig, struct siginfo *info, pid_t pid)
{
	int error;
	struct task_struct *p;

	read_lock(&tasklist_lock);
	p = find_task_by_pid(pid);
	error = -ESRCH;
	if (p)
		error = group_send_sig_info(sig, info, p);
	read_unlock(&tasklist_lock);
	return error;
}


/*
 * kill_something_info() interprets pid in interesting ways just like kill(2).
 *
 * POSIX specifies that kill(-1,sig) is unspecified, but what we have
 * is probably wrong.  Should make it like BSD or SYSV.
 */

static int kill_something_info(int sig, struct siginfo *info, int pid)
{
	if (!pid) {
		return kill_pg_info(sig, info, process_group(current));
	} else if (pid == -1) {
		int retval = 0, count = 0;
		struct task_struct * p;

		read_lock(&tasklist_lock);
		/*对所有进程调用group_send_sig_info*/
		for_each_process(p) {
			if (p->pid > 1 && p->tgid != current->tgid) {
				int err = group_send_sig_info(sig, info, p);
				++count;
				if (err != -EPERM)
					retval = err;
			}
		}
		read_unlock(&tasklist_lock);
		return count ? retval : -ESRCH;
	} else if (pid < 0) {
		return kill_pg_info(sig, info, -pid);
	} else {
		return kill_proc_info(sig, info, pid);
	}
}

/*
 * These are for backward compatibility with the rest of the kernel source.
 */

/*
 * These two are the most common entry points.  They send a signal
 * just to the specific thread.
 */
/**
 * 与send_sig类似，但是还使用siginfo_t结构中的扩展信息
 */
int
send_sig_info(int sig, struct siginfo *info, struct task_struct *p)
{
	int ret;
	unsigned long flags;

	/*
	 * Make sure legacy kernel users don't send in bad values
	 * (normal paths check this in check_kill_permission).
	 */
	if (sig < 0 || sig > _NSIG)
		return -EINVAL;

	/*
	 * We need the tasklist lock even for the specific
	 * thread case (when we don't need to follow the group
	 * lists) in order to avoid races with "p->sighand"
	 * going away or changing from under us.
	 */
	read_lock(&tasklist_lock);  
	spin_lock_irqsave(&p->sighand->siglock, flags);
	ret = specific_send_sig_info(sig, info, p);
	spin_unlock_irqrestore(&p->sighand->siglock, flags);
	read_unlock(&tasklist_lock);
	return ret;
}

/**
 * 向单一进程发送信号
 */
int
send_sig(int sig, struct task_struct *p, int priv)
{
	return send_sig_info(sig, (void*)(long)(priv != 0), p);
}

/*
 * This is the entry point for "process-wide" signals.
 * They will go to an appropriate thread in the thread group.
 */
/**
 * 向某一个进程组发送信号，该线程组由它的一个成员进程标识。
 */
int
send_group_sig_info(int sig, struct siginfo *info, struct task_struct *p)
{
	int ret;
	read_lock(&tasklist_lock);
	ret = group_send_sig_info(sig, info, p);
	read_unlock(&tasklist_lock);
	return ret;
}

/**
 * 发送既不能被进程显式忽略，也不能被进程阻塞的信号
 */
void
force_sig(int sig, struct task_struct *p)
{
	force_sig_info(sig, (void*)1L, p);
}

/*
 * When things go south during signal handling, we
 * will force a SIGSEGV. And if the signal that caused
 * the problem was already a SIGSEGV, we'll want to
 * make sure we don't even try to deliver the signal..
 */
int
force_sigsegv(int sig, struct task_struct *p)
{
	if (sig == SIGSEGV) {
		unsigned long flags;
		spin_lock_irqsave(&p->sighand->siglock, flags);
		p->sighand->action[sig - 1].sa.sa_handler = SIG_DFL;
		spin_unlock_irqrestore(&p->sighand->siglock, flags);
	}
	force_sig(SIGSEGV, p);
	return 0;
}

/**
 * 向一个进程组中的所有线程组发送信号
 */
int
kill_pg(pid_t pgrp, int sig, int priv)
{
	return kill_pg_info(sig, (void *)(long)(priv != 0), pgrp);
}

/**
 * 向某一个线程组发送信号，该线程组由它的一个成员进程的PID来标识
 */
int
kill_proc(pid_t pid, int sig, int priv)
{
	return kill_proc_info(sig, (void *)(long)(priv != 0), pid);
}

/*
 * These functions support sending signals using preallocated sigqueue
 * structures.  This is needed "because realtime applications cannot
 * afford to lose notifications of asynchronous events, like timer
 * expirations or I/O completions".  In the case of Posix Timers 
 * we allocate the sigqueue structure from the timer_create.  If this
 * allocation fails we are able to report the failure to the application
 * with an EAGAIN error.
 */
 
struct sigqueue *sigqueue_alloc(void)
{
	struct sigqueue *q;

	if ((q = __sigqueue_alloc(current, GFP_KERNEL)))
		q->flags |= SIGQUEUE_PREALLOC;
	return(q);
}

void sigqueue_free(struct sigqueue *q)
{
	unsigned long flags;
	BUG_ON(!(q->flags & SIGQUEUE_PREALLOC));
	/*
	 * If the signal is still pending remove it from the
	 * pending queue.
	 */
	if (unlikely(!list_empty(&q->list))) {
		read_lock(&tasklist_lock);  
		spin_lock_irqsave(q->lock, flags);
		if (!list_empty(&q->list))
			list_del_init(&q->list);
		spin_unlock_irqrestore(q->lock, flags);
		read_unlock(&tasklist_lock);
	}
	q->flags &= ~SIGQUEUE_PREALLOC;
	__sigqueue_free(q);
}

int
send_sigqueue(int sig, struct sigqueue *q, struct task_struct *p)
{
	unsigned long flags;
	int ret = 0;

	/*
	 * We need the tasklist lock even for the specific
	 * thread case (when we don't need to follow the group
	 * lists) in order to avoid races with "p->sighand"
	 * going away or changing from under us.
	 */
	BUG_ON(!(q->flags & SIGQUEUE_PREALLOC));
	read_lock(&tasklist_lock);  
	spin_lock_irqsave(&p->sighand->siglock, flags);
	
	if (unlikely(!list_empty(&q->list))) {
		/*
		 * If an SI_TIMER entry is already queue just increment
		 * the overrun count.
		 */
		if (q->info.si_code != SI_TIMER)
			BUG();
		q->info.si_overrun++;
		goto out;
	} 
	/* Short-circuit ignored signals.  */
	if (sig_ignored(p, sig)) {
		ret = 1;
		goto out;
	}

	q->lock = &p->sighand->siglock;
	list_add_tail(&q->list, &p->pending.list);
	sigaddset(&p->pending.signal, sig);
	if (!sigismember(&p->blocked, sig))
		signal_wake_up(p, sig == SIGKILL);

out:
	spin_unlock_irqrestore(&p->sighand->siglock, flags);
	read_unlock(&tasklist_lock);
	return(ret);
}

int
send_group_sigqueue(int sig, struct sigqueue *q, struct task_struct *p)
{
	unsigned long flags;
	int ret = 0;

	BUG_ON(!(q->flags & SIGQUEUE_PREALLOC));
	read_lock(&tasklist_lock);
	spin_lock_irqsave(&p->sighand->siglock, flags);
	handle_stop_signal(sig, p);

	/* Short-circuit ignored signals.  */
	if (sig_ignored(p, sig)) {
		ret = 1;
		goto out;
	}

	if (unlikely(!list_empty(&q->list))) {
		/*
		 * If an SI_TIMER entry is already queue just increment
		 * the overrun count.  Other uses should not try to
		 * send the signal multiple times.
		 */
		if (q->info.si_code != SI_TIMER)
			BUG();
		q->info.si_overrun++;
		goto out;
	} 

	/*
	 * Put this signal on the shared-pending queue.
	 * We always use the shared queue for process-wide signals,
	 * to avoid several races.
	 */
	q->lock = &p->sighand->siglock;
	list_add_tail(&q->list, &p->signal->shared_pending.list);
	sigaddset(&p->signal->shared_pending.signal, sig);

	__group_complete_signal(sig, p);
out:
	spin_unlock_irqrestore(&p->sighand->siglock, flags);
	read_unlock(&tasklist_lock);
	return(ret);
}

/*
 * Wake up any threads in the parent blocked in wait* syscalls.
 */
static inline void __wake_up_parent(struct task_struct *p,
				    struct task_struct *parent)
{
	wake_up_interruptible_sync(&parent->signal->wait_chldexit);
}

/*
 * Let a parent know about the death of a child.
 * For a stopped/continued status change, use do_notify_parent_cldstop instead.
 */

void do_notify_parent(struct task_struct *tsk, int sig)
{
	struct siginfo info;
	unsigned long flags;
	struct sighand_struct *psig;

	BUG_ON(sig == -1);

 	/* do_notify_parent_cldstop should have been called instead.  */
 	BUG_ON(tsk->state & (TASK_STOPPED|TASK_TRACED));

	BUG_ON(!tsk->ptrace &&
	       (tsk->group_leader != tsk || !thread_group_empty(tsk)));

	info.si_signo = sig;
	info.si_errno = 0;
	info.si_pid = tsk->pid;
	info.si_uid = tsk->uid;

	/* FIXME: find out whether or not this is supposed to be c*time. */
	info.si_utime = cputime_to_jiffies(cputime_add(tsk->utime,
						       tsk->signal->utime));
	info.si_stime = cputime_to_jiffies(cputime_add(tsk->stime,
						       tsk->signal->stime));

	info.si_status = tsk->exit_code & 0x7f;
	if (tsk->exit_code & 0x80)
		info.si_code = CLD_DUMPED;
	else if (tsk->exit_code & 0x7f)
		info.si_code = CLD_KILLED;
	else {
		info.si_code = CLD_EXITED;
		info.si_status = tsk->exit_code >> 8;
	}

	psig = tsk->parent->sighand;
	spin_lock_irqsave(&psig->siglock, flags);
	if (sig == SIGCHLD &&
	    (psig->action[SIGCHLD-1].sa.sa_handler == SIG_IGN ||
	     (psig->action[SIGCHLD-1].sa.sa_flags & SA_NOCLDWAIT))) {
		/*
		 * We are exiting and our parent doesn't care.  POSIX.1
		 * defines special semantics for setting SIGCHLD to SIG_IGN
		 * or setting the SA_NOCLDWAIT flag: we should be reaped
		 * automatically and not left for our parent's wait4 call.
		 * Rather than having the parent do it as a magic kind of
		 * signal handler, we just set this to tell do_exit that we
		 * can be cleaned up without becoming a zombie.  Note that
		 * we still call __wake_up_parent in this case, because a
		 * blocked sys_wait4 might now return -ECHILD.
		 *
		 * Whether we send SIGCHLD or not for SA_NOCLDWAIT
		 * is implementation-defined: we do (if you don't want
		 * it, just use SIG_IGN instead).
		 */
		tsk->exit_signal = -1;
		if (psig->action[SIGCHLD-1].sa.sa_handler == SIG_IGN)
			sig = 0;
	}
	if (sig > 0 && sig <= _NSIG)
		__group_send_sig_info(sig, &info, tsk->parent);
	__wake_up_parent(tsk, tsk->parent);
	spin_unlock_irqrestore(&psig->siglock, flags);
}

/**
 * 通知监控进程，当前进程进行了信号处理。
 */
static void
do_notify_parent_cldstop(struct task_struct *tsk, struct task_struct *parent,
			 int why)
{
	struct siginfo info;
	unsigned long flags;
	struct sighand_struct *sighand;

	info.si_signo = SIGCHLD;
	info.si_errno = 0;
	info.si_pid = tsk->pid;
	info.si_uid = tsk->uid;

	/* FIXME: find out whether or not this is supposed to be c*time. */
	info.si_utime = cputime_to_jiffies(tsk->utime);
	info.si_stime = cputime_to_jiffies(tsk->stime);

 	info.si_code = why;
 	switch (why) {
 	case CLD_CONTINUED:
 		info.si_status = SIGCONT;
 		break;
 	case CLD_STOPPED:
 		info.si_status = tsk->signal->group_exit_code & 0x7f;
 		break;
 	case CLD_TRAPPED:
 		info.si_status = tsk->exit_code & 0x7f;
 		break;
 	default:
 		BUG();
 	}

	sighand = parent->sighand;
	spin_lock_irqsave(&sighand->siglock, flags);
	/*
	 * 如果父进程没有设置SIGCHLD或者SA_NOCLDSTOP标志
	 * 则向父进程发送SIGCHLD
	 */
	if (sighand->action[SIGCHLD-1].sa.sa_handler != SIG_IGN &&
	    !(sighand->action[SIGCHLD-1].sa.sa_flags & SA_NOCLDSTOP))
		__group_send_sig_info(SIGCHLD, &info, parent);
	/*
	 * Even if SIGCHLD is not generated, we must wake up wait4 calls.
	 */
	__wake_up_parent(tsk, parent);
	spin_unlock_irqrestore(&sighand->siglock, flags);
}

/*
 * This must be called with current->sighand->siglock held.
 *
 * This should be the path for all ptrace stops.
 * We always set current->last_siginfo while stopped here.
 * That makes it a way to test a stopped process for
 * being ptrace-stopped vs being job-control-stopped.
 *
 * If we actually decide not to stop at all because the tracer is gone,
 * we leave nostop_code in current->exit_code.
 */
static void ptrace_stop(int exit_code, int nostop_code, siginfo_t *info)
{
	/*
	 * If there is a group stop in progress,
	 * we must participate in the bookkeeping.
	 */
	if (current->signal->group_stop_count > 0)
		--current->signal->group_stop_count;

	current->last_siginfo = info;
	current->exit_code = exit_code;

	/* Let the debugger run.  */
	set_current_state(TASK_TRACED);
	spin_unlock_irq(&current->sighand->siglock);
	read_lock(&tasklist_lock);
	/*
	 * 调用do_notify_parent_cldstop 和schedule 让监控进程知道进程的信号处理
	 */
	if (likely(current->ptrace & PT_PTRACED) &&
	    likely(current->parent != current->real_parent ||
		   !(current->ptrace & PT_ATTACHED)) &&
	    (likely(current->parent->signal != current->signal) ||
	     !unlikely(current->signal->flags & SIGNAL_GROUP_EXIT))) {
		do_notify_parent_cldstop(current, current->parent,
					 CLD_TRAPPED);
		read_unlock(&tasklist_lock);
		schedule();
	} else {
		/*
		 * By the time we got the lock, our tracer went away.
		 * Don't stop here.
		 */
		read_unlock(&tasklist_lock);
		set_current_state(TASK_RUNNING);
		current->exit_code = nostop_code;
	}

	/*
	 * We are back.  Now reacquire the siglock before touching
	 * last_siginfo, so that we are sure to have synchronized with
	 * any signal-sending on another CPU that wants to examine it.
	 */
	spin_lock_irq(&current->sighand->siglock);
	current->last_siginfo = NULL;

	/*
	 * Queued signals ignored us while we were stopped for tracing.
	 * So check for any that we should take before resuming user mode.
	 */
	recalc_sigpending();
}

void ptrace_notify(int exit_code)
{
	siginfo_t info;

	BUG_ON((exit_code & (0x7f | ~0xffff)) != SIGTRAP);

	memset(&info, 0, sizeof info);
	info.si_signo = SIGTRAP;
	info.si_code = exit_code;
	info.si_pid = current->pid;
	info.si_uid = current->uid;

	/* Let the debugger run.  */
	spin_lock_irq(&current->sighand->siglock);
	ptrace_stop(exit_code, 0, &info);
	spin_unlock_irq(&current->sighand->siglock);
}

#ifndef HAVE_ARCH_GET_SIGNAL_TO_DELIVER

static void
finish_stop(int stop_count)
{
	/*
	 * If there are no other threads in the group, or if there is
	 * a group stop in progress and we are the last to stop,
	 * report to the parent.  When ptraced, every thread reports itself.
	 */
	if (stop_count < 0 || (current->ptrace & PT_PTRACED)) {
		read_lock(&tasklist_lock);
		do_notify_parent_cldstop(current, current->parent,
					 CLD_STOPPED);
		read_unlock(&tasklist_lock);
	}
	else if (stop_count == 0) {
		read_lock(&tasklist_lock);
		/*
		 * 如果是第一个被停止至的进程stop_count == 0，且领头进程的父进程没有设置SIGCHLD或者SA_NOCLDSTOP标志
		 * 则向父进程发送SIGCHLD
		 */
		do_notify_parent_cldstop(current->group_leader,
					 current->group_leader->real_parent,
					 CLD_STOPPED);
		read_unlock(&tasklist_lock);
	}

	/*
	 * 调用schedule() 让出CPU
	 */
	schedule();
	/*
	 * Now we don't run again until continued.
	 */
	current->exit_code = 0;
}

/*
 * This performs the stopping for SIGSTOP and other stop signals.
 * We have to stop all threads in the thread group.
 * Returns nonzero if we've actually stopped and released the siglock.
 * Returns zero if we didn't stop and still hold the siglock.
 */
/**
 * 处理组停止信号。停止整个线程组中的线程。
 */
static int
do_signal_stop(int signr)
{
	struct signal_struct *sig = current->signal;
	struct sighand_struct *sighand = current->sighand;
	int stop_count = -1;

	/**
	 * 检查当前线程是否是线程组中第一个被停止的进程
	 * 如果不是，那么进程组中的其他线程已经调用了do_signal_stop，不用再次执行do_signal_stop停止整个线程组了。
	 */
	if (!likely(sig->flags & SIGNAL_STOP_DEQUEUED))
		return 0;

	/**
	 * 是第一个被停止的线程。
	 */
	if (sig->group_stop_count > 0) {
		/*
		 * There is a group stop in progress.  We don't need to
		 * start another one.
		 */
		signr = sig->group_exit_code;
		stop_count = --sig->group_stop_count;
		current->exit_code = signr;
		set_current_state(TASK_STOPPED);
		if (stop_count == 0)
			sig->flags = SIGNAL_STOP_STOPPED;
		spin_unlock_irq(&sighand->siglock);
	}
	else if (thread_group_empty(current)) {
		/*
		 * Lock must be held through transition to stopped state.
		 */
		current->exit_code = current->signal->group_exit_code = signr;
		set_current_state(TASK_STOPPED);
		sig->flags = SIGNAL_STOP_STOPPED;
		spin_unlock_irq(&sighand->siglock);
	}
	else {
		/*
		 * There is no group stop already in progress.
		 * We must initiate one now, but that requires
		 * dropping siglock to get both the tasklist lock
		 * and siglock again in the proper order.  Note that
		 * this allows an intervening SIGCONT to be posted.
		 * We need to check for that and bail out if necessary.
		 */
		struct task_struct *t;

		spin_unlock_irq(&sighand->siglock);

		/* signals can be posted during this window */

		read_lock(&tasklist_lock);
		spin_lock_irq(&sighand->siglock);

		if (!likely(sig->flags & SIGNAL_STOP_DEQUEUED)) {
			/*
			 * Another stop or continue happened while we
			 * didn't have the lock.  We can just swallow this
			 * signal now.  If we raced with a SIGCONT, that
			 * should have just cleared it now.  If we raced
			 * with another processor delivering a stop signal,
			 * then the SIGCONT that wakes us up should clear it.
			 */
			return 0;
		}

		/*
		 * 如果是第一个被定制的进程sig->group_stop_count == 0
		 */
		/**
		 * 检查当前线程是否是线程组中第一个被停止的进程
		 * 如果不是，那么进程组中的其他线程已经调用了do_signal_stop，不用再次执行do_signal_stop停止整个线程组了。
		 */		 
		if (sig->group_stop_count == 0) {
			sig->group_exit_code = signr;
			stop_count = 0;
			/*
			 *  将进程组中所有的进程都设加入到运行队列中
			 */
			for (t = next_thread(current); t != current;
			     t = next_thread(t))
				/*
				 * Setting state to TASK_STOPPED for a group
				 * stop is always done with the siglock held,
				 * so this check has no races.
				 */
				if (t->state < TASK_STOPPED) {
					stop_count++;
					/**
					 * signal_wake_up (也就是try_to_wake_up) 会将指定的进程加入到CPU 运行的等待队列里面
					 * 但是不会马上切换出去，
					 * 当程序返回到用户态之前，就会检查是否有信号需要处理，就又会走到这里
					 * 但是那时候group_stop_count 就不为0 了, 进入了sig->group_stop_count > 0 的分支
					 * 被唤醒的线程需要检查group_stop_count，以确认正在执行组停止操作。然后把状态设置成TASK_STOPPED
					 */
					signal_wake_up(t, 0);
				}
			/*
			 * 将停止进程的数量更新到group_stop_count 字段
			 */
			sig->group_stop_count = stop_count;
		}
		else {
			/* A race with another thread while unlocked.  */
			signr = sig->group_exit_code;
			stop_count = --sig->group_stop_count;
		}

		current->exit_code = signr;
		set_current_state(TASK_STOPPED);
		if (stop_count == 0)
			sig->flags = SIGNAL_STOP_STOPPED;

		spin_unlock_irq(&sighand->siglock);
		read_unlock(&tasklist_lock);
	}

	/*
	 * 通知父进程并让出CPU
	 */
	finish_stop(stop_count);
	return 1;
}

/*
 * Do appropriate magic when group_stop_count > 0.
 * We return nonzero if we stopped, after releasing the siglock.
 * We return zero if we still hold the siglock and should look
 * for another signal without checking group_stop_count again.
 */
/*
 * 共享信号队列的STOP处理走这里
 */
static inline int handle_group_stop(void)
{
	int stop_count;

	if (current->signal->group_exit_task == current) {
		/*
		 * Group stop is so we can do a core dump,
		 * We are the initiating thread, so get on with it.
		 */
		current->signal->group_exit_task = NULL;
		return 0;
	}

	if (current->signal->flags & SIGNAL_GROUP_EXIT)
		/*
		 * Group stop is so another thread can do a core dump,
		 * or else we are racing against a death signal.
		 * Just punt the stop so we can get the next signal.
		 */
		return 0;

	/*
	 * There is a group stop in progress.  We stop
	 * without any associated signal being in our queue.
	 */
	stop_count = --current->signal->group_stop_count;
	if (stop_count == 0)
		current->signal->flags = SIGNAL_STOP_STOPPED;
	current->exit_code = current->signal->group_exit_code;
	set_current_state(TASK_STOPPED);
	spin_unlock_irq(&current->sighand->siglock);
	finish_stop(stop_count);
	return 1;
}

int get_signal_to_deliver(siginfo_t *info, struct k_sigaction *return_ka,
			  struct pt_regs *regs, void *cookie)
{
	sigset_t *mask = &current->blocked;
	int signr = 0;

relock:
	spin_lock_irq(&current->sighand->siglock);
	for (;;) {
		struct k_sigaction *ka;

		/*
		 * 如果是共享队列的STOP 信号被激活组停止，走到这里进行停止操作
		 */
		if (unlikely(current->signal->group_stop_count > 0) &&
		    handle_group_stop())
			goto relock;

		/**
		 * 从私有挂起信号队列和共享挂起信号队列中获得待处理的信号
		 * 先从私有队列中查找，并且从最低编号的挂起信号开始。
		 * 然后在共享队列中查找。
		 * 找到非阻塞的挂起信号号，它还会修改数据结构，以表示信号不再是挂起的。
		 */
		signr = dequeue_signal(current, mask, info);

		/**
		 * 没有信号了，会返回0
		 */
		if (!signr)
			break; /* will return 0 */

		/**
		 * 找到一个需要处理的信号，开始处理它。
		 */
		 
		/**
		 * 如果进程正在被监控。就调用ptrace_stop通知调试进程：进程正在处理信号。
		 */
		if ((current->ptrace & PT_PTRACED) && signr != SIGKILL) {
			ptrace_signal_deliver(regs, cookie);

			/* Let the debugger run.  */
			/*
			 * 调用do_notify_parent_cldstop 和schedule 让监控进程知道进程的信号处理
			 */
			ptrace_stop(signr, signr, info);

			/* We're back.  Did the debugger cancel the sig?  */
			signr = current->exit_code;
			if (signr == 0)
				continue;

			current->exit_code = 0;

			/* Update the siginfo structure if the signal has
			   changed.  If the debugger wanted something
			   specific in the siginfo structure then it should
			   have updated *info via PTRACE_SETSIGINFO.  */
			if (signr != info->si_signo) {
				info->si_signo = signr;
				info->si_errno = 0;
				info->si_code = SI_USER;
				info->si_pid = current->parent->pid;
				info->si_uid = current->parent->uid;
			}

			/* If the (new) signal is now blocked, requeue it.  */
			if (sigismember(&current->blocked, signr)) {
				specific_send_sig_info(signr, info, current);
				continue;
			}
		}

		/**
		 * 将要处理信号的k_sigaction数据结构地址赋给局部变量ka,  得到信号上要执行的操作
		 */
		ka = &current->sighand->action[signr-1];
		/**
		 * 忽略信号，处理下一个
		 */
		if (ka->sa.sa_handler == SIG_IGN) /* Do nothing.  */
			continue;
		/**
		 * 不是忽略，也不是执行缺省操作。
		 * 那就是执行用户自定义的信号处理程序。
		 */
		if (ka->sa.sa_handler != SIG_DFL) {
			/* Run the handler.  */
			*return_ka = *ka;

			/**
			 * SA_ONESHOT表示用户设置的处理函数仅仅下一次处理过程有用。
			 * 这一次信号处理完后就应该恢复默认操作。
			 */
			if (ka->sa.sa_flags & SA_ONESHOT)
				ka->sa.sa_handler = SIG_DFL;

			/**
			 * 返回待处理信号给上一层函数，上一层函数会建立栈帧，返回到用户态执行用户处理程序。
			 */
			break; /* will return non-zero "signr" value */
		}

		/*
		 * Now we are doing the default action for this signal.
		 */
		/**
		 * 执行到此，说明需要执行默认操作。
		 */

		/**
		 * SIGCONT,SIGCHLD,SIGWINCH,SIGURG几个信号的默认操作就是不操作
		 */
		if (sig_kernel_ignore(signr)) /* Default is nothing. */
			continue;

		/* Init gets no signals it doesn't want.  */
		/**
		 * 对init进程来说，忽略所有信号，难道谁想杀死init或者....。
		 * 对进程0来说，根本就不允许发送信号，所以在此不判断为0的情况。
		 * 对进程0和1分开处理有一个原因：进程0永远不死亡，而进程1允许通过exit自杀。
		 * 为什么必须将进程0和进程1分别处理，现在还不完全清楚。
		 */
		if (current->pid == 1)
			continue;

		/**
		 * SIGSTOP,SIGTSTP,SIGTTIN,SIGTTOU这几个信号的默认操作可能停止线程组的所有进程。
		 * 为此把进程状态置为TASK_STOPPED，并随后调用schedule.
		 */
		if (sig_kernel_stop(signr)) {
			/*
			 * The default action is to stop all threads in
			 * the thread group.  The job control signals
			 * do nothing in an orphaned pgrp, but SIGSTOP
			 * always works.  Note that siglock needs to be
			 * dropped during the call to is_orphaned_pgrp()
			 * because of lock ordering with tasklist_lock.
			 * This allows an intervening SIGCONT to be posted.
			 * We need to check for that and bail out if necessary.
			 */
			/**
			 * SIGSTOP总会停止整个线程组，而其他几个信号只停止不在“孤儿进程组”中的线程组。
			 * POSIX标准规定，只要进程组中有一个进程有父进程。尽管进程处于不同的进程组中但在同一个会话中，那么这个进程组就不是孤儿。
			 * 因此，如果父进程死亡，但是启动该进程的用户仍然在线，那么该进程组就不是一个孤儿。
			 * xie.baoyou注：到底孤儿进程是如何出现的呢？登录会话都结束了，竟然还有子进程存在，这样的孤儿还不容易杀死？？
			 */
			if (signr != SIGSTOP) {/*不是SIGSTOP信号，就要判断上面的情况*/
				spin_unlock_irq(&current->sighand->siglock);

				/* signals can be posted during this window */
				/**
				 * 只停止不在孤儿进程组中的进程，如果进程在孤儿进程组中，就不处理，继续处理下一个信号。
				 */
				if (is_orphaned_pgrp(process_group(current)))
					goto relock;

				spin_lock_irq(&current->sighand->siglock);
			}

			/**
			 * do_signal_stop会将进程状态置为TASK_STOPPED。
			 * 并检查current是否是线程组中第一个被停止的进程，如果是，它激活“组停止”。
			 * 注：它一般会调用schedule，所以锁会被打开。也就需要回到relock，重新获得锁。
			 */
			if (likely(do_signal_stop(signr))) {
				/* It released the siglock.  */
				goto relock;
			}

			/*
			 * We didn't actually stop, due to a race
			 * with SIGCONT or something like that.
			 */
			continue;
		}

		/**
		 * 在dump和kill前，当然需要把锁打开了。
		 */
		spin_unlock_irq(&current->sighand->siglock);

		/*
		 * Anything else is fatal, maybe with a core dump.
		 */
		current->flags |= PF_SIGNALED;
		/**
		 * 对缺省操作为dump的信号，就创建转储信息。然后杀死这个线程组。
		 */
		if (sig_kernel_coredump(signr)) {
			/*
			 * If it was able to dump core, this kills all
			 * other threads in the group and synchronizes with
			 * their demise.  If we lost the race with another
			 * thread getting here, it set group_exit_code
			 * first and our do_group_exit call below will use
			 * that value and ignore the one we pass it.
			 */
			do_coredump((long)signr, signr, regs);
		}

		/*
		 * Death signals, no core dump.
		 */
		/**
		 * 其他信号的缺省操作是Terminate。都是杀死线程组。
		 */
		do_group_exit(signr);
		/* NOTREACHED */
	}
	spin_unlock_irq(&current->sighand->siglock);
	return signr;
}

#endif

EXPORT_SYMBOL(recalc_sigpending);
EXPORT_SYMBOL_GPL(dequeue_signal);
EXPORT_SYMBOL(flush_signals);
EXPORT_SYMBOL(force_sig);
EXPORT_SYMBOL(kill_pg);
EXPORT_SYMBOL(kill_proc);
EXPORT_SYMBOL(ptrace_notify);
EXPORT_SYMBOL(send_sig);
EXPORT_SYMBOL(send_sig_info);
EXPORT_SYMBOL(sigprocmask);
EXPORT_SYMBOL(block_all_signals);
EXPORT_SYMBOL(unblock_all_signals);


/*
 * System call entry points.
 */

asmlinkage long sys_restart_syscall(void)
{
	struct restart_block *restart = &current_thread_info()->restart_block;
	return restart->fn(restart);
}

long do_no_restart_syscall(struct restart_block *param)
{
	return -EINTR;
}

/*
 * We don't need to get the kernel lock - this is all local to this
 * particular thread.. (and that's good, because this is _heavily_
 * used by various programs)
 */

/*
 * This is also useful for kernel threads that want to temporarily
 * (or permanently) block certain signals.
 *
 * NOTE! Unlike the user-mode sys_sigprocmask(), the kernel
 * interface happily blocks "unblockable" signals like SIGKILL
 * and friends.
 */
int sigprocmask(int how, sigset_t *set, sigset_t *oldset)
{
	int error;
	sigset_t old_block;

	spin_lock_irq(&current->sighand->siglock);
	old_block = current->blocked;
	error = 0;
	switch (how) {
	case SIG_BLOCK:
		sigorsets(&current->blocked, &current->blocked, set);
		break;
	case SIG_UNBLOCK:
		signandsets(&current->blocked, &current->blocked, set);
		break;
	case SIG_SETMASK:
		current->blocked = *set;
		break;
	default:
		error = -EINVAL;
	}
	recalc_sigpending();
	spin_unlock_irq(&current->sighand->siglock);
	if (oldset)
		*oldset = old_block;
	return error;
}

asmlinkage long
sys_rt_sigprocmask(int how, sigset_t __user *set, sigset_t __user *oset, size_t sigsetsize)
{
	int error = -EINVAL;
	sigset_t old_set, new_set;

	/* XXX: Don't preclude handling different sized sigset_t's.  */
	if (sigsetsize != sizeof(sigset_t))
		goto out;

	if (set) {
		error = -EFAULT;
		if (copy_from_user(&new_set, set, sizeof(*set)))
			goto out;
		sigdelsetmask(&new_set, sigmask(SIGKILL)|sigmask(SIGSTOP));

		error = sigprocmask(how, &new_set, &old_set);
		if (error)
			goto out;
		if (oset)
			goto set_old;
	} else if (oset) {
		spin_lock_irq(&current->sighand->siglock);
		old_set = current->blocked;
		spin_unlock_irq(&current->sighand->siglock);

	set_old:
		error = -EFAULT;
		if (copy_to_user(oset, &old_set, sizeof(*oset)))
			goto out;
	}
	error = 0;
out:
	return error;
}

long do_sigpending(void __user *set, unsigned long sigsetsize)
{
	long error = -EINVAL;
	sigset_t pending;

	if (sigsetsize > sizeof(sigset_t))
		goto out;

	spin_lock_irq(&current->sighand->siglock);
	sigorsets(&pending, &current->pending.signal,
		  &current->signal->shared_pending.signal);
	spin_unlock_irq(&current->sighand->siglock);

	/* Outside the lock because only this thread touches it.  */
	sigandsets(&pending, &current->blocked, &pending);

	error = -EFAULT;
	if (!copy_to_user(set, &pending, sigsetsize))
		error = 0;

out:
	return error;
}	

asmlinkage long
sys_rt_sigpending(sigset_t __user *set, size_t sigsetsize)
{
	return do_sigpending(set, sigsetsize);
}

#ifndef HAVE_ARCH_COPY_SIGINFO_TO_USER

int copy_siginfo_to_user(siginfo_t __user *to, siginfo_t *from)
{
	int err;

	if (!access_ok (VERIFY_WRITE, to, sizeof(siginfo_t)))
		return -EFAULT;
	if (from->si_code < 0)
		return __copy_to_user(to, from, sizeof(siginfo_t))
			? -EFAULT : 0;
	/*
	 * If you change siginfo_t structure, please be sure
	 * this code is fixed accordingly.
	 * It should never copy any pad contained in the structure
	 * to avoid security leaks, but must copy the generic
	 * 3 ints plus the relevant union member.
	 */
	err = __put_user(from->si_signo, &to->si_signo);
	err |= __put_user(from->si_errno, &to->si_errno);
	err |= __put_user((short)from->si_code, &to->si_code);
	switch (from->si_code & __SI_MASK) {
	case __SI_KILL:
		err |= __put_user(from->si_pid, &to->si_pid);
		err |= __put_user(from->si_uid, &to->si_uid);
		break;
	case __SI_TIMER:
		 err |= __put_user(from->si_tid, &to->si_tid);
		 err |= __put_user(from->si_overrun, &to->si_overrun);
		 err |= __put_user(from->si_ptr, &to->si_ptr);
		break;
	case __SI_POLL:
		err |= __put_user(from->si_band, &to->si_band);
		err |= __put_user(from->si_fd, &to->si_fd);
		break;
	case __SI_FAULT:
		err |= __put_user(from->si_addr, &to->si_addr);
#ifdef __ARCH_SI_TRAPNO
		err |= __put_user(from->si_trapno, &to->si_trapno);
#endif
		break;
	case __SI_CHLD:
		err |= __put_user(from->si_pid, &to->si_pid);
		err |= __put_user(from->si_uid, &to->si_uid);
		err |= __put_user(from->si_status, &to->si_status);
		err |= __put_user(from->si_utime, &to->si_utime);
		err |= __put_user(from->si_stime, &to->si_stime);
		break;
	case __SI_RT: /* This is not generated by the kernel as of now. */
	case __SI_MESGQ: /* But this is */
		err |= __put_user(from->si_pid, &to->si_pid);
		err |= __put_user(from->si_uid, &to->si_uid);
		err |= __put_user(from->si_ptr, &to->si_ptr);
		break;
	default: /* this is just in case for now ... */
		err |= __put_user(from->si_pid, &to->si_pid);
		err |= __put_user(from->si_uid, &to->si_uid);
		break;
	}
	return err;
}

#endif

asmlinkage long
sys_rt_sigtimedwait(const sigset_t __user *uthese,
		    siginfo_t __user *uinfo,
		    const struct timespec __user *uts,
		    size_t sigsetsize)
{
	int ret, sig;
	sigset_t these;
	struct timespec ts;
	siginfo_t info;
	long timeout = 0;

	/* XXX: Don't preclude handling different sized sigset_t's.  */
	if (sigsetsize != sizeof(sigset_t))
		return -EINVAL;

	if (copy_from_user(&these, uthese, sizeof(these)))
		return -EFAULT;
		
	/*
	 * Invert the set of allowed signals to get those we
	 * want to block.
	 */
	sigdelsetmask(&these, sigmask(SIGKILL)|sigmask(SIGSTOP));
	signotset(&these);

	if (uts) {
		if (copy_from_user(&ts, uts, sizeof(ts)))
			return -EFAULT;
		if (ts.tv_nsec >= 1000000000L || ts.tv_nsec < 0
		    || ts.tv_sec < 0)
			return -EINVAL;
	}

	spin_lock_irq(&current->sighand->siglock);
	sig = dequeue_signal(current, &these, &info);
	if (!sig) {
		timeout = MAX_SCHEDULE_TIMEOUT;
		if (uts)
			timeout = (timespec_to_jiffies(&ts)
				   + (ts.tv_sec || ts.tv_nsec));

		if (timeout) {
			/* None ready -- temporarily unblock those we're
			 * interested while we are sleeping in so that we'll
			 * be awakened when they arrive.  */
			current->real_blocked = current->blocked;
			sigandsets(&current->blocked, &current->blocked, &these);
			recalc_sigpending();
			spin_unlock_irq(&current->sighand->siglock);

			current->state = TASK_INTERRUPTIBLE;
			timeout = schedule_timeout(timeout);

			spin_lock_irq(&current->sighand->siglock);
			sig = dequeue_signal(current, &these, &info);
			current->blocked = current->real_blocked;
			siginitset(&current->real_blocked, 0);
			recalc_sigpending();
		}
	}
	spin_unlock_irq(&current->sighand->siglock);

	if (sig) {
		ret = sig;
		if (uinfo) {
			if (copy_siginfo_to_user(uinfo, &info))
				ret = -EFAULT;
		}
	} else {
		ret = -EAGAIN;
		if (timeout)
			ret = -EINTR;
	}

	return ret;
}

/**
 * Kill系统调用处理函数
 * pid>0 表示把sig信号发送到其PID==pid的进程所属的线程组。
 * pid==0表示把信号发送到与调用进程同组的进程所有线程组。
 * pid==-1表示把信号发送到所有进程。除了swapper。init和current
 * pid < -1把信号发送到进程组-pid中进程的所有线程线。
 * 虽然kill能够发送编号在32-64之间的实时信号。但是它不能确保把一个新的元素加入到目标进程的挂起信号队列。
 * 因此，发送实时信号需要通过rt_sigqueueinfo系统调用进行。
 */
asmlinkage long
sys_kill(int pid, int sig)
{
	struct siginfo info;

	/**
	 * 建立最小的siginfo_t表。
	 */
	info.si_signo = sig;
	info.si_errno = 0;
	info.si_code = SI_USER;
	info.si_pid = current->tgid;
	info.si_uid = current->uid;

	/**
	 * kill_something_info真正处理信号发送
	 */
	return kill_something_info(sig, &info, pid);
}

/**
 *  sys_tgkill - send signal to one specific thread
 *  @tgid: the thread group ID of the thread
 *  @pid: the PID of the thread
 *  @sig: signal to be sent
 *
 *  This syscall also checks the tgid and returns -ESRCH even if the PID
 *  exists but it's not belonging to the target process anymore. This
 *  method solves the problem of threads exiting and PIDs getting reused.
 */
/**
 * Tgkill的系统调用处理函数。
 */
asmlinkage long sys_tgkill(int tgid, int pid, int sig)
{
	struct siginfo info;
	int error;
	struct task_struct *p;

	/* This is only valid for single tasks */
	if (pid <= 0 || tgid <= 0)
		return -EINVAL;

	info.si_signo = sig;
	info.si_errno = 0;
	info.si_code = SI_TKILL;
	info.si_pid = current->tgid;
	info.si_uid = current->uid;

	read_lock(&tasklist_lock);
	p = find_task_by_pid(pid);
	error = -ESRCH;
	if (p && (p->tgid == tgid)) {
		error = check_kill_permission(sig, &info, p);
		/*
		 * The null signal is a permissions and process existence
		 * probe.  No signal is actually delivered.
		 */
		if (!error && sig && p->sighand) {
			spin_lock_irq(&p->sighand->siglock);
			handle_stop_signal(sig, p);
			error = specific_send_sig_info(sig, &info, p);
			spin_unlock_irq(&p->sighand->siglock);
		}
	}
	read_unlock(&tasklist_lock);
	return error;
}

/*
 *  Send a signal to only one task, even if it's a CLONE_THREAD task.
 */
/**
 * Tkill的系统调用处理函数
 */
asmlinkage long
sys_tkill(int pid, int sig)
{
	struct siginfo info;
	int error;
	struct task_struct *p;

	/* This is only valid for single tasks */
	if (pid <= 0)
		return -EINVAL;

	info.si_signo = sig;
	info.si_errno = 0;
	info.si_code = SI_TKILL;
	info.si_pid = current->tgid;
	info.si_uid = current->uid;

	read_lock(&tasklist_lock);
	p = find_task_by_pid(pid);
	error = -ESRCH;
	if (p) {
		error = check_kill_permission(sig, &info, p);
		/*
		 * The null signal is a permissions and process existence
		 * probe.  No signal is actually delivered.
		 */
		if (!error && sig && p->sighand) {
			spin_lock_irq(&p->sighand->siglock);
			handle_stop_signal(sig, p);
			error = specific_send_sig_info(sig, &info, p);
			spin_unlock_irq(&p->sighand->siglock);
		}
	}
	read_unlock(&tasklist_lock);
	return error;
}

/**
 * Rt_sigqueueinfo的系统调用处理函数
 */
asmlinkage long
sys_rt_sigqueueinfo(int pid, int sig, siginfo_t __user *uinfo)
{
	siginfo_t info;

	if (copy_from_user(&info, uinfo, sizeof(siginfo_t)))
		return -EFAULT;

	/* Not even root can pretend to send signals from the kernel.
	   Nor can they impersonate a kill(), which adds source info.  */
	if (info.si_code >= 0)
		return -EPERM;
	info.si_signo = sig;

	/* POSIX.1b doesn't mention process groups.  */
	return kill_proc_info(sig, &info, pid);
}

/**
 * 把信号操作挂接到进程描述符
 */
int
do_sigaction(int sig, const struct k_sigaction *act, struct k_sigaction *oact)
{
	struct k_sigaction *k;

	if (sig < 1 || sig > _NSIG || (act && sig_kernel_only(sig)))
		return -EINVAL;

	/**
	 * 信号编号从1开始，所以存到action[sig-1]而不是action[sig]
	 */
	k = &current->sighand->action[sig-1];

	spin_lock_irq(&current->sighand->siglock);
	if (signal_pending(current)) {
		/*
		 * If there might be a fatal signal pending on multiple
		 * threads, make sure we take it before changing the action.
		 */
		spin_unlock_irq(&current->sighand->siglock);
		return -ERESTARTNOINTR;
	}

	if (oact)
		*oact = *k;

	/**
	 * POSIX规定，当缺省操作是"Ignore"时，把信号操作设置成SIG_IGN或SIG_DFL将引起同类型的任一信号被丢弃。
	 */
	if (act) {
		/*
		 * POSIX 3.3.1.3:
		 *  "Setting a signal action to SIG_IGN for a signal that is
		 *   pending shall cause the pending signal to be discarded,
		 *   whether or not it is blocked."
		 *
		 *  "Setting a signal action to SIG_DFL for a signal that is
		 *   pending and whose default action is to ignore the signal
		 *   (for example, SIGCHLD), shall cause the pending signal to
		 *   be discarded, whether or not it is blocked"
		 */
		if (act->sa.sa_handler == SIG_IGN ||
		    (act->sa.sa_handler == SIG_DFL &&
		     sig_kernel_ignore(sig))) {
			/*
			 * This is a fairly rare case, so we only take the
			 * tasklist_lock once we're sure we'll need it.
			 * Now we must do this little unlock and relock
			 * dance to maintain the lock hierarchy.
			 */
			struct task_struct *t = current;
			spin_unlock_irq(&t->sighand->siglock);
			read_lock(&tasklist_lock);
			spin_lock_irq(&t->sighand->siglock);
			*k = *act;
			/**
			 * SIGKILL和SIGSTOP不能被屏蔽，所以此处特殊处理一下。
			 */
			sigdelsetmask(&k->sa.sa_mask,
				      sigmask(SIGKILL) | sigmask(SIGSTOP));
			rm_from_queue(sigmask(sig), &t->signal->shared_pending);
			do {
				rm_from_queue(sigmask(sig), &t->pending);
				recalc_sigpending_tsk(t);
				t = next_thread(t);
			} while (t != current);
			spin_unlock_irq(&current->sighand->siglock);
			read_unlock(&tasklist_lock);
			return 0;
		}

		*k = *act;
		sigdelsetmask(&k->sa.sa_mask,
			      sigmask(SIGKILL) | sigmask(SIGSTOP));
	}

	spin_unlock_irq(&current->sighand->siglock);
	return 0;
}

int 
do_sigaltstack (const stack_t __user *uss, stack_t __user *uoss, unsigned long sp)
{
	stack_t oss;
	int error;

	if (uoss) {
		oss.ss_sp = (void __user *) current->sas_ss_sp;
		oss.ss_size = current->sas_ss_size;
		oss.ss_flags = sas_ss_flags(sp);
	}

	if (uss) {
		void __user *ss_sp;
		size_t ss_size;
		int ss_flags;

		error = -EFAULT;
		if (verify_area(VERIFY_READ, uss, sizeof(*uss))
		    || __get_user(ss_sp, &uss->ss_sp)
		    || __get_user(ss_flags, &uss->ss_flags)
		    || __get_user(ss_size, &uss->ss_size))
			goto out;

		error = -EPERM;
		if (on_sig_stack(sp))
			goto out;

		error = -EINVAL;
		/*
		 *
		 * Note - this code used to test ss_flags incorrectly
		 *  	  old code may have been written using ss_flags==0
		 *	  to mean ss_flags==SS_ONSTACK (as this was the only
		 *	  way that worked) - this fix preserves that older
		 *	  mechanism
		 */
		if (ss_flags != SS_DISABLE && ss_flags != SS_ONSTACK && ss_flags != 0)
			goto out;

		if (ss_flags == SS_DISABLE) {
			ss_size = 0;
			ss_sp = NULL;
		} else {
			error = -ENOMEM;
			if (ss_size < MINSIGSTKSZ)
				goto out;
		}

		current->sas_ss_sp = (unsigned long) ss_sp;
		current->sas_ss_size = ss_size;
	}

	if (uoss) {
		error = -EFAULT;
		if (copy_to_user(uoss, &oss, sizeof(oss)))
			goto out;
	}

	error = 0;
out:
	return error;
}

#ifdef __ARCH_WANT_SYS_SIGPENDING

asmlinkage long
sys_sigpending(old_sigset_t __user *set)
{
	return do_sigpending(set, sizeof(*set));
}

#endif

#ifdef __ARCH_WANT_SYS_SIGPROCMASK
/* Some platforms have their own version with special arguments others
   support only sys_rt_sigprocmask.  */
/*
 * sigprocmask服务例程
 * oset - 进程地址空间的一个指针，执行存放以前位掩码的一个位数组
 * set - 进程地址空间的一个指针，指向包含新位掩码的位数组
 * how - 一个标志，可以有下列的一个值
 *	SIG_BLOCK	 set位掩码数组，指定必须加到阻塞信号的位掩码数组中的信号
 *	SIG_UNBLOCK set位掩码数组，指定必须从阻塞信号的位掩码数组中删除的信号
 *	SIG_SETMASK  set位掩码数组，指定阻塞信号新的位掩码数组
 */
asmlinkage long
sys_sigprocmask(int how, old_sigset_t __user *set, old_sigset_t __user *oset)
{
	int error;
	old_sigset_t old_set, new_set;

	if (set) {
		error = -EFAULT;
		if (copy_from_user(&new_set, set, sizeof(*set)))
			goto out;
		new_set &= ~(sigmask(SIGKILL) | sigmask(SIGSTOP));

		spin_lock_irq(&current->sighand->siglock);
		old_set = current->blocked.sig[0];

		error = 0;
		switch (how) {
		default:
			error = -EINVAL;
			break;
		case SIG_BLOCK:
			sigaddsetmask(&current->blocked, new_set);
			break;
		case SIG_UNBLOCK:
			sigdelsetmask(&current->blocked, new_set);
			break;
		case SIG_SETMASK:
			current->blocked.sig[0] = new_set;
			break;
		}

		recalc_sigpending();
		spin_unlock_irq(&current->sighand->siglock);
		if (error)
			goto out;
		if (oset)
			goto set_old;
	} else if (oset) {
		old_set = current->blocked.sig[0];
	set_old:
		error = -EFAULT;
		if (copy_to_user(oset, &old_set, sizeof(*oset)))
			goto out;
	}
	error = 0;
out:
	return error;
}
#endif /* __ARCH_WANT_SYS_SIGPROCMASK */

#ifdef __ARCH_WANT_SYS_RT_SIGACTION
asmlinkage long
sys_rt_sigaction(int sig,
		 const struct sigaction __user *act,
		 struct sigaction __user *oact,
		 size_t sigsetsize)
{
	struct k_sigaction new_sa, old_sa;
	int ret = -EINVAL;

	/* XXX: Don't preclude handling different sized sigset_t's.  */
	if (sigsetsize != sizeof(sigset_t))
		goto out;

	if (act) {
		if (copy_from_user(&new_sa.sa, act, sizeof(new_sa.sa)))
			return -EFAULT;
	}

	ret = do_sigaction(sig, act ? &new_sa : NULL, oact ? &old_sa : NULL);

	if (!ret && oact) {
		if (copy_to_user(oact, &old_sa.sa, sizeof(old_sa.sa)))
			return -EFAULT;
	}
out:
	return ret;
}
#endif /* __ARCH_WANT_SYS_RT_SIGACTION */

#ifdef __ARCH_WANT_SYS_SGETMASK

/*
 * For backwards compatibility.  Functionality superseded by sigprocmask.
 */
asmlinkage long
sys_sgetmask(void)
{
	/* SMP safe */
	return current->blocked.sig[0];
}

asmlinkage long
sys_ssetmask(int newmask)
{
	int old;

	spin_lock_irq(&current->sighand->siglock);
	old = current->blocked.sig[0];

	siginitset(&current->blocked, newmask & ~(sigmask(SIGKILL)|
						  sigmask(SIGSTOP)));
	recalc_sigpending();
	spin_unlock_irq(&current->sighand->siglock);

	return old;
}
#endif /* __ARCH_WANT_SGETMASK */

#ifdef __ARCH_WANT_SYS_SIGNAL
/*
 * For backwards compatibility.  Functionality superseded by sigaction.
 */
asmlinkage unsigned long
sys_signal(int sig, __sighandler_t handler)
{
	struct k_sigaction new_sa, old_sa;
	int ret;

	new_sa.sa.sa_handler = handler;
	new_sa.sa.sa_flags = SA_ONESHOT | SA_NOMASK;

	ret = do_sigaction(sig, &new_sa, &old_sa);

	return ret ? ret : (unsigned long)old_sa.sa.sa_handler;
}
#endif /* __ARCH_WANT_SYS_SIGNAL */

#ifdef __ARCH_WANT_SYS_PAUSE

asmlinkage long
sys_pause(void)
{
	current->state = TASK_INTERRUPTIBLE;
	schedule();
	return -ERESTARTNOHAND;
}

#endif

void __init signals_init(void)
{
	sigqueue_cachep =
		kmem_cache_create("sigqueue",
				  sizeof(struct sigqueue),
				  __alignof__(struct sigqueue),
				  SLAB_PANIC, NULL, NULL);
}
