/*	$OpenBSD: kern_lock.c,v 1.74 2024/05/29 18:55:45 claudio Exp $	*/

/*
 * Copyright (c) 2017 Visa Hankala
 * Copyright (c) 2014 David Gwynne <dlg@openbsd.org>
 * Copyright (c) 2004 Artur Grabowski <art@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sched.h>
#include <sys/atomic.h>
#include <sys/witness.h>
#include <sys/mutex.h>

#include <ddb/db_output.h>

#ifdef MP_LOCKDEBUG
#ifndef DDB
#error "MP_LOCKDEBUG requires DDB"
#endif

/* CPU-dependent timing, this needs to be settable from ddb. */
int __mp_lock_spinout = INT_MAX;
#endif /* MP_LOCKDEBUG */

#ifdef MULTIPROCESSOR

#include <sys/mplock.h>
struct __mp_lock kernel_lock;

/*
 * Functions for manipulating the kernel_lock.  We put them here
 * so that they show up in profiles.
 */

void
_kernel_lock_init(void)
{
	__mp_lock_init(&kernel_lock);
}

/*
 * Acquire/release the kernel lock.  Intended for use in the scheduler
 * and the lower half of the kernel.
 */

void
_kernel_lock(void)
{
	SCHED_ASSERT_UNLOCKED();
	__mp_lock(&kernel_lock);
}

void
_kernel_unlock(void)
{
	__mp_unlock(&kernel_lock);
}

int
_kernel_lock_held(void)
{
	if (panicstr || db_active)
		return 1;
	return (__mp_lock_held(&kernel_lock, curcpu()));
}

#ifdef __USE_MI_MPLOCK

/* Ticket lock implementation */

#include <machine/cpu.h>

void
___mp_lock_init(struct __mp_lock *mpl, const struct lock_type *type)
{
	memset(mpl->mpl_cpus, 0, sizeof(mpl->mpl_cpus));
	mpl->mpl_users = 0;
	mpl->mpl_ticket = 1;

#ifdef WITNESS
	mpl->mpl_lock_obj.lo_name = type->lt_name;
	mpl->mpl_lock_obj.lo_type = type;
	if (mpl == &kernel_lock)
		mpl->mpl_lock_obj.lo_flags = LO_WITNESS | LO_INITIALIZED |
		    LO_SLEEPABLE | (LO_CLASS_KERNEL_LOCK << LO_CLASSSHIFT);
	WITNESS_INIT(&mpl->mpl_lock_obj, type);
#endif
}

static __inline void
__mp_lock_spin(struct __mp_lock *mpl, u_int me)
{
	struct schedstate_percpu *spc = &curcpu()->ci_schedstate;
#ifdef MP_LOCKDEBUG
	int nticks = __mp_lock_spinout;
#endif

	spc->spc_spinning++;
	while (mpl->mpl_ticket != me) {
		CPU_BUSY_CYCLE();

#ifdef MP_LOCKDEBUG
		if (--nticks <= 0) {
			db_printf("%s: %p lock spun out\n", __func__, mpl);
			db_enter();
			nticks = __mp_lock_spinout;
		}
#endif
	}
	spc->spc_spinning--;
}

void
__mp_lock(struct __mp_lock *mpl)
{
	struct __mp_lock_cpu *cpu = &mpl->mpl_cpus[cpu_number()];
	unsigned long s;

#ifdef WITNESS
	if (!__mp_lock_held(mpl, curcpu()))
		WITNESS_CHECKORDER(&mpl->mpl_lock_obj,
		    LOP_EXCLUSIVE | LOP_NEWORDER, NULL);
#endif

	s = intr_disable();
	if (cpu->mplc_depth++ == 0)
		cpu->mplc_ticket = atomic_inc_int_nv(&mpl->mpl_users);
	intr_restore(s);

	__mp_lock_spin(mpl, cpu->mplc_ticket);
	membar_enter_after_atomic();

	WITNESS_LOCK(&mpl->mpl_lock_obj, LOP_EXCLUSIVE);
}

void
__mp_unlock(struct __mp_lock *mpl)
{
	struct __mp_lock_cpu *cpu = &mpl->mpl_cpus[cpu_number()];
	unsigned long s;

#ifdef MP_LOCKDEBUG
	if (!__mp_lock_held(mpl, curcpu())) {
		db_printf("__mp_unlock(%p): not held lock\n", mpl);
		db_enter();
	}
#endif

	WITNESS_UNLOCK(&mpl->mpl_lock_obj, LOP_EXCLUSIVE);

	s = intr_disable();
	if (--cpu->mplc_depth == 0) {
		membar_exit();
		mpl->mpl_ticket++;
	}
	intr_restore(s);
}

int
__mp_release_all(struct __mp_lock *mpl)
{
	struct __mp_lock_cpu *cpu = &mpl->mpl_cpus[cpu_number()];
	unsigned long s;
	int rv;
#ifdef WITNESS
	int i;
#endif

	s = intr_disable();
	rv = cpu->mplc_depth;
#ifdef WITNESS
	for (i = 0; i < rv; i++)
		WITNESS_UNLOCK(&mpl->mpl_lock_obj, LOP_EXCLUSIVE);
#endif
	cpu->mplc_depth = 0;
	membar_exit();
	mpl->mpl_ticket++;
	intr_restore(s);

	return (rv);
}

int
__mp_release_all_but_one(struct __mp_lock *mpl)
{
	struct __mp_lock_cpu *cpu = &mpl->mpl_cpus[cpu_number()];
	int rv = cpu->mplc_depth - 1;
#ifdef WITNESS
	int i;

	for (i = 0; i < rv; i++)
		WITNESS_UNLOCK(&mpl->mpl_lock_obj, LOP_EXCLUSIVE);
#endif

#ifdef MP_LOCKDEBUG
	if (!__mp_lock_held(mpl, curcpu())) {
		db_printf("__mp_release_all_but_one(%p): not held lock\n", mpl);
		db_enter();
	}
#endif

	cpu->mplc_depth = 1;

	return (rv);
}

void
__mp_acquire_count(struct __mp_lock *mpl, int count)
{
	while (count--)
		__mp_lock(mpl);
}

int
__mp_lock_held(struct __mp_lock *mpl, struct cpu_info *ci)
{
	struct __mp_lock_cpu *cpu = &mpl->mpl_cpus[CPU_INFO_UNIT(ci)];

	return (cpu->mplc_ticket == mpl->mpl_ticket && cpu->mplc_depth > 0);
}

#endif /* __USE_MI_MPLOCK */

#endif /* MULTIPROCESSOR */


#ifdef __USE_MI_MUTEX
void
__mtx_init(struct mutex *mtx, int wantipl)
{
	mtx->mtx_owner = NULL;
	mtx->mtx_wantipl = wantipl;
	mtx->mtx_oldipl = IPL_NONE;
}

#ifdef MULTIPROCESSOR
void
mtx_enter(struct mutex *mtx)
{
	struct schedstate_percpu *spc = &curcpu()->ci_schedstate;
#ifdef MP_LOCKDEBUG
	int nticks = __mp_lock_spinout;
#endif

	WITNESS_CHECKORDER(MUTEX_LOCK_OBJECT(mtx),
	    LOP_EXCLUSIVE | LOP_NEWORDER, NULL);

	spc->spc_spinning++;
	while (mtx_enter_try(mtx) == 0) {
		do {
			CPU_BUSY_CYCLE();
#ifdef MP_LOCKDEBUG
			if (--nticks == 0) {
				db_printf("%s: %p lock spun out\n",
				    __func__, mtx);
				db_enter();
				nticks = __mp_lock_spinout;
			}
#endif
		} while (mtx->mtx_owner != NULL);
	}
	spc->spc_spinning--;
}

int
mtx_enter_try(struct mutex *mtx)
{
	struct cpu_info *owner, *ci = curcpu();
	int s;

	/* Avoid deadlocks after panic or in DDB */
	if (panicstr || db_active)
		return (1);

	if (mtx->mtx_wantipl != IPL_NONE)
		s = splraise(mtx->mtx_wantipl);

	owner = atomic_cas_ptr(&mtx->mtx_owner, NULL, ci);
#ifdef DIAGNOSTIC
	if (__predict_false(owner == ci))
		panic("mtx %p: locking against myself", mtx);
#endif
	if (owner == NULL) {
		membar_enter_after_atomic();
		if (mtx->mtx_wantipl != IPL_NONE)
			mtx->mtx_oldipl = s;
#ifdef DIAGNOSTIC
		ci->ci_mutex_level++;
#endif
		WITNESS_LOCK(MUTEX_LOCK_OBJECT(mtx), LOP_EXCLUSIVE);
		return (1);
	}

	if (mtx->mtx_wantipl != IPL_NONE)
		splx(s);

	return (0);
}
#else
void
mtx_enter(struct mutex *mtx)
{
	struct cpu_info *ci = curcpu();

	/* Avoid deadlocks after panic or in DDB */
	if (panicstr || db_active)
		return;

	WITNESS_CHECKORDER(MUTEX_LOCK_OBJECT(mtx),
	    LOP_EXCLUSIVE | LOP_NEWORDER, NULL);

#ifdef DIAGNOSTIC
	if (__predict_false(mtx->mtx_owner == ci))
		panic("mtx %p: locking against myself", mtx);
#endif

	if (mtx->mtx_wantipl != IPL_NONE)
		mtx->mtx_oldipl = splraise(mtx->mtx_wantipl);

	mtx->mtx_owner = ci;

#ifdef DIAGNOSTIC
	ci->ci_mutex_level++;
#endif
	WITNESS_LOCK(MUTEX_LOCK_OBJECT(mtx), LOP_EXCLUSIVE);
}

int
mtx_enter_try(struct mutex *mtx)
{
	mtx_enter(mtx);
	return (1);
}
#endif

void
mtx_leave(struct mutex *mtx)
{
	int s;

	/* Avoid deadlocks after panic or in DDB */
	if (panicstr || db_active)
		return;

	MUTEX_ASSERT_LOCKED(mtx);
	WITNESS_UNLOCK(MUTEX_LOCK_OBJECT(mtx), LOP_EXCLUSIVE);

#ifdef DIAGNOSTIC
	curcpu()->ci_mutex_level--;
#endif

	s = mtx->mtx_oldipl;
#ifdef MULTIPROCESSOR
	membar_exit();
#endif
	mtx->mtx_owner = NULL;
	if (mtx->mtx_wantipl != IPL_NONE)
		splx(s);
}

#ifdef DDB
void
db_mtx_enter(struct db_mutex *mtx)
{
	struct cpu_info *ci = curcpu(), *owner;
	unsigned long s;

#ifdef DIAGNOSTIC
	if (__predict_false(mtx->mtx_owner == ci))
		panic("%s: mtx %p: locking against myself", __func__, mtx);
#endif

	s = intr_disable();

	for (;;) {
		owner = atomic_cas_ptr(&mtx->mtx_owner, NULL, ci);
		if (owner == NULL)
			break;
		CPU_BUSY_CYCLE();
	}
	membar_enter_after_atomic();

	mtx->mtx_intr_state = s;

#ifdef DIAGNOSTIC
	ci->ci_mutex_level++;
#endif
}

void
db_mtx_leave(struct db_mutex *mtx)
{
#ifdef DIAGNOSTIC
	struct cpu_info *ci = curcpu();
#endif
	unsigned long s;

#ifdef DIAGNOSTIC
	if (__predict_false(mtx->mtx_owner != ci))
		panic("%s: mtx %p: not owned by this CPU", __func__, mtx);
	ci->ci_mutex_level--;
#endif

	s = mtx->mtx_intr_state;
#ifdef MULTIPROCESSOR
	membar_exit();
#endif
	mtx->mtx_owner = NULL;
	intr_restore(s);
}
#endif /* DDB */
#endif /* __USE_MI_MUTEX */

#ifdef WITNESS
void
_mtx_init_flags(struct mutex *m, int ipl, const char *name, int flags,
    const struct lock_type *type)
{
	struct lock_object *lo = MUTEX_LOCK_OBJECT(m);

	lo->lo_flags = MTX_LO_FLAGS(flags);
	if (name != NULL)
		lo->lo_name = name;
	else
		lo->lo_name = type->lt_name;
	WITNESS_INIT(lo, type);

	_mtx_init(m, ipl);
}
#endif /* WITNESS */
