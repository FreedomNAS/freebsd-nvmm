/*
 * Copyright (c) 2021 Maxime Villard, m00nbsd.net
 * Copyright (c) 2021 The DragonFly Project.
 * Copyright (c) 2026 Sarah Chiandotto (FreeBSD support)
 * All rights reserved.
 *
 * This code is part of the NVMM hypervisor.
 *
 * This code is derived from software contributed to The DragonFly Project
 * by Aaron LI <aly@aaronly.me>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _NVMM_OS_H_
#define _NVMM_OS_H_

#ifndef _KERNEL
#error "This file should not be included by userland programs."
#endif

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/mman.h>
#include <sys/proc.h>

#if defined(__NetBSD__)
#include <uvm/uvm_object.h>
#include <uvm/uvm_extern.h>
#include <uvm/uvm_page.h>
#elif defined(__DragonFly__)
#include <sys/lock.h>
#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pager.h>
#include <vm/vm_param.h> /* KERN_SUCCESS, etc. */
#include <vm/pmap.h> /* pmap_ept_transform, pmap_npt_transform */
#include <machine/cpu.h> /* hvm_break_wanted */
#include <machine/cpufunc.h> /* ffsl, ffs, etc. */
#elif defined(__FreeBSD__)
#include <sys/lock.h>
#include <sys/sx.h>
#include <sys/condvar.h>
#include <sys/malloc.h>
#include <sys/smp.h>
#include <sys/sched.h>
#include <sys/pcpu.h>
#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/pmap.h>
#include <vm/vm_extern.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <machine/atomic.h>
#include <machine/cpufunc.h>
#include <machine/segments.h>
#include <machine/md_var.h>
#include <dev/nvmm/nvmm_bitops.h>
#endif

/* Types. */
#if defined(__NetBSD__)
typedef struct vm_map		os_vmmap_t;
typedef struct vmspace		os_vmspace_t;
typedef struct uvm_object	os_vmobj_t;
typedef krwlock_t		os_rwl_t;
typedef kmutex_t		os_mtx_t;
#elif defined(__DragonFly__)
typedef struct vm_map		os_vmmap_t;
typedef struct vmspace		os_vmspace_t;
typedef struct vm_object	os_vmobj_t;
typedef struct lock		os_rwl_t;
typedef struct lock		os_mtx_t;
/* A few standard types. */
typedef vm_offset_t		vaddr_t;
typedef vm_offset_t		voff_t;
typedef vm_size_t		vsize_t;
typedef vm_paddr_t		paddr_t;
#elif defined(__FreeBSD__)
typedef struct vm_map		os_vmmap_t;
typedef struct vmspace		os_vmspace_t;
typedef struct vm_object	os_vmobj_t;
typedef struct sx		os_rwl_t;
/*
 * An sx lock, not a mutex: NVMM holds this lock across sleeping
 * operations (M_WAITOK allocations, vm_map changes, vm_fault() inside
 * vcpu_run).
 */
typedef struct sx		os_mtx_t;
typedef vm_offset_t		vaddr_t;
typedef vm_ooffset_t		voff_t;
typedef vm_size_t		vsize_t;
typedef vm_paddr_t		paddr_t;
#endif

/* Attributes. */
#if defined(__DragonFly__)
#define __cacheline_aligned	__cachealign
#define __diagused		__debugvar
#elif defined(__FreeBSD__)
#define __cacheline_aligned	__aligned(CACHE_LINE_SIZE)
#endif

/* Macros. */
#if defined(__DragonFly__)
#define __arraycount(__x)	NELEM(__x)
#define __insn_barrier()	__asm __volatile("":::"memory")
#elif defined(__FreeBSD__)
#define __arraycount(__x)	nitems(__x)
#define __insn_barrier()	__compiler_membar()
#endif

/* Bitops. */
#if defined(__NetBSD__)
#include <sys/bitops.h>
#elif defined(__DragonFly__)
#include <sys/bitops.h>
#ifdef __x86_64__
#undef  __BIT
#define __BIT(__n)		__BIT64(__n)
#undef  __BITS
#define __BITS(__m, __n)	__BITS64(__m, __n)
#endif /* __x86_64__ */
#endif

/* Maps. */
#if defined(__NetBSD__) || defined(__DragonFly__) || defined(__FreeBSD__)
#define os_kernel_map		kernel_map
#define os_curproc_map		&curproc->p_vmspace->vm_map
#endif

/* R/W locks. */
#if defined(__NetBSD__)
#define os_rwl_init(lock)	rw_init(lock)
#define os_rwl_destroy(lock)	rw_destroy(lock)
#define os_rwl_rlock(lock)	rw_enter(lock, RW_READER);
#define os_rwl_wlock(lock)	rw_enter(lock, RW_WRITER);
#define os_rwl_unlock(lock)	rw_exit(lock)
#define os_rwl_wheld(lock)	rw_write_held(lock)
#elif defined(__DragonFly__)
#define os_rwl_init(lock)	lockinit(lock, "nvmmrw", 0, 0)
#define os_rwl_destroy(lock)	lockuninit(lock)
#define os_rwl_rlock(lock)	lockmgr(lock, LK_SHARED);
#define os_rwl_wlock(lock)	lockmgr(lock, LK_EXCLUSIVE);
#define os_rwl_unlock(lock)	lockmgr(lock, LK_RELEASE)
#define os_rwl_wheld(lock)	(lockstatus(lock, curthread) == LK_EXCLUSIVE)
#elif defined(__FreeBSD__)
#define os_rwl_init(lock)	sx_init(lock, "nvmmrw")
#define os_rwl_destroy(lock)	sx_destroy(lock)
#define os_rwl_rlock(lock)	sx_slock(lock);
#define os_rwl_wlock(lock)	sx_xlock(lock);
#define os_rwl_unlock(lock)	sx_unlock(lock)
#define os_rwl_wheld(lock)	sx_xlocked(lock)
#endif

/* Mutexes. */
#if defined(__NetBSD__)
#define os_mtx_init(lock)	mutex_init(lock, MUTEX_DEFAULT, IPL_NONE)
#define os_mtx_destroy(lock)	mutex_destroy(lock)
#define os_mtx_lock(lock)	mutex_enter(lock)
#define os_mtx_unlock(lock)	mutex_exit(lock)
#elif defined(__DragonFly__)
#define os_mtx_init(lock)	lockinit(lock, "nvmmmtx", 0, 0)
#define os_mtx_destroy(lock)	lockuninit(lock)
#define os_mtx_lock(lock)	lockmgr(lock, LK_EXCLUSIVE)
#define os_mtx_unlock(lock)	lockmgr(lock, LK_RELEASE)
#elif defined(__FreeBSD__)
#define os_mtx_init(lock)	sx_init(lock, "nvmmmtx")
#define os_mtx_destroy(lock)	sx_destroy(lock)
#define os_mtx_lock(lock)	sx_xlock(lock)
#define os_mtx_unlock(lock)	sx_xunlock(lock)
#endif

/* Condition variables (used by the NetBSD suspend/resume code). */
#if defined(__NetBSD__)
#include <sys/condvar.h>
typedef kcondvar_t		os_cv_t;
#define os_cv_init(cv, name)	cv_init(cv, name)
#define os_cv_destroy(cv)	cv_destroy(cv)
#define os_cv_wait(cv, lock)	cv_wait(cv, lock)
#define os_cv_wait_sig(cv, lock) cv_wait_sig(cv, lock)
#define os_cv_signal(cv)	cv_signal(cv)
#define os_cv_broadcast(cv)	cv_broadcast(cv)
#elif defined(__FreeBSD__)
typedef struct cv		os_cv_t;
#define os_cv_init(cv, name)	cv_init(cv, name)
#define os_cv_destroy(cv)	cv_destroy(cv)
#define os_cv_wait(cv, lock)	cv_wait(cv, lock)
#define os_cv_wait_sig(cv, lock) cv_wait_sig(cv, lock)
#define os_cv_signal(cv)	cv_signal(cv)
#define os_cv_broadcast(cv)	cv_broadcast(cv)
#endif

/* Malloc. */
#if defined(__NetBSD__)
#include <sys/kmem.h>
#define os_mem_alloc(size)	kmem_alloc(size, KM_SLEEP)
#define os_mem_zalloc(size)	kmem_zalloc(size, KM_SLEEP)
#define os_mem_free(ptr, size)	kmem_free(ptr, size)
#elif defined(__DragonFly__)
#include <sys/malloc.h>
MALLOC_DECLARE(M_NVMM);
#define os_mem_alloc(size)	kmalloc(size, M_NVMM, M_WAITOK)
#define os_mem_zalloc(size)	kmalloc(size, M_NVMM, M_WAITOK | M_ZERO)
#define os_mem_free(ptr, size)	kfree(ptr, M_NVMM)
#elif defined(__FreeBSD__)
MALLOC_DECLARE(M_NVMM);
#define os_mem_alloc(size)	malloc(size, M_NVMM, M_WAITOK)
#define os_mem_zalloc(size)	malloc(size, M_NVMM, M_WAITOK | M_ZERO)
#define os_mem_free(ptr, size)	free(ptr, M_NVMM)
#endif

/* Printf. */
#if defined(__NetBSD__)
#define os_printf		printf
#elif defined(__DragonFly__)
#define os_printf		kprintf
#elif defined(__FreeBSD__)
#define os_printf		printf
#endif

/* Atomics. */
#if defined(__NetBSD__)
#include <sys/atomic.h>
#define os_atomic_inc_uint(x)	atomic_inc_uint(x)
#define os_atomic_dec_uint(x)	atomic_dec_uint(x)
#define os_atomic_load_uint(x)	atomic_load_relaxed(x)
#define os_atomic_inc_64(x)	atomic_inc_64(x)
#define os_atomic_load_bool(x)	atomic_load_relaxed(x)
#define os_atomic_store_bool(x, v) atomic_store_relaxed(x, v)
#elif defined(__DragonFly__)
#include <machine/atomic.h>
#define os_atomic_inc_uint(x)	atomic_add_int(x, 1)
#define os_atomic_dec_uint(x)	atomic_subtract_int(x, 1)
#define os_atomic_load_uint(x)	atomic_load_int(x)
#define os_atomic_inc_64(x)	atomic_add_64(x, 1)
#elif defined(__FreeBSD__)
#define os_atomic_inc_uint(x)	atomic_add_int(x, 1)
#define os_atomic_dec_uint(x)	atomic_subtract_int(x, 1)
#define os_atomic_load_uint(x)	atomic_load_int(x)
#define os_atomic_inc_64(x)	atomic_add_64(x, 1)
#define os_atomic_load_bool(x)	atomic_load_bool(x)
#define os_atomic_store_bool(x, v) atomic_store_bool(x, v)
#endif

/* Pmap. */
#if defined(__NetBSD__)
#if __NetBSD_Prereq__(10, 0, 0)
#include <machine/pmap_private.h> /* TLBSHOOT_NVMM */
#endif
#define os_vmspace_pdirpa(vm)	((vm)->vm_map.pmap->pm_pdirpa[0])
#define os_pmap_mach(pm)	((pm)->pm_data)
#elif defined(__DragonFly__)
#define os_vmspace_pdirpa(vm)	(vtophys(vmspace_pmap(vm)->pm_pml4))
/* os_pmap_mach() not used */
#elif defined(__FreeBSD__)
#define os_vmspace_pdirpa(vm)	\
	(vtophys((vm_offset_t)vmspace_pmap(vm)->pm_pmltop))
/* os_pmap_mach() not used */
#endif

/* CPU. */
#if defined(__NetBSD__)
#include <sys/cpu.h>
typedef struct cpu_info		os_cpu_t;
#define OS_MAXCPUS		MAXCPUS
#define OS_CPU_FOREACH(cpu)	for (CPU_INFO_FOREACH(, cpu))
#define os_cpu_number(cpu)	cpu_index(cpu)
#define os_curcpu()		curcpu()
#define os_curcpu_number()	cpu_number()
#define os_curcpu_tss_sel()	curcpu()->ci_tss_sel
#define os_curcpu_tss()		curcpu()->ci_tss
#define os_curcpu_gdt()		curcpu()->ci_gdt
#define os_curcpu_idt()		curcpu()->ci_idtvec.iv_idt
#elif defined(__DragonFly__)
#include <sys/globaldata.h>
#include <machine/segments.h>
typedef struct globaldata	os_cpu_t;
#define OS_MAXCPUS		SMP_MAXCPU
#define OS_CPU_FOREACH(cpu)	\
	for (int idx = 0; idx < ncpus && (cpu = globaldata_find(idx)); idx++)
#define os_cpu_number(cpu)	(cpu)->gd_cpuid
#define os_curcpu()		mycpu
#define os_curcpu_number()	mycpuid
#define os_curcpu_tss_sel()	GSEL(GPROC0_SEL, SEL_KPL)
#define os_curcpu_tss()		&mycpu->gd_prvspace->common_tss
#define os_curcpu_gdt()		mdcpu->gd_gdt
#define os_curcpu_idt()		r_idt_arr[mycpuid].rd_base
#elif defined(__FreeBSD__)
typedef struct pcpu		os_cpu_t;
#define OS_MAXCPUS		MAXCPU
#define OS_CPU_FOREACH(cpu)						\
	for (u_int __idx = 0; __idx <= mp_maxid; __idx++)		\
		if (CPU_ABSENT(__idx) ||				\
		    ((cpu) = pcpu_find(__idx)) == NULL) {		\
		} else
#define os_cpu_number(cpu)	(cpu)->pc_cpuid
#define os_curcpu()		get_pcpu()
#define os_curcpu_number()	curcpu
#define os_curcpu_tss_sel()	GSEL(GPROC0_SEL, SEL_KPL)
#define os_curcpu_tss()		PCPU_GET(tssp)
#define os_curcpu_gdt()		(*PCPU_PTR(gdt))
#define os_curcpu_idt()		r_idt.rd_base
#endif

/* Cpusets. */
#if defined(__NetBSD__)
typedef kcpuset_t		os_cpuset_t;
#define os_cpuset_init(s)	kcpuset_create(s, true)
#define os_cpuset_destroy(s)	kcpuset_destroy(s)
#define os_cpuset_isset(s, c)	kcpuset_isset(s, c)
#define os_cpuset_clear(s, c)	kcpuset_clear(s, c)
#define os_cpuset_setrunning(s)	kcpuset_copy(s, kcpuset_running)
#elif defined(__DragonFly__)
#include <sys/cpumask.h>
#include <machine/smp.h> /* smp_active_mask */
typedef cpumask_t		os_cpuset_t;
#define os_cpuset_init(s)	\
	({ *(s) = kmalloc(sizeof(cpumask_t), M_NVMM, M_WAITOK | M_ZERO); })
#define os_cpuset_destroy(s)	kfree((s), M_NVMM)
#define os_cpuset_isset(s, c)	CPUMASK_TESTBIT(*(s), c)
#define os_cpuset_clear(s, c)	ATOMIC_CPUMASK_NANDBIT(*(s), c)
#define os_cpuset_setrunning(s)	ATOMIC_CPUMASK_ORMASK(*(s), smp_active_mask)
#elif defined(__FreeBSD__)
#include <sys/cpuset.h>
typedef cpuset_t		os_cpuset_t;
#define os_cpuset_init(s)	\
	({ *(s) = malloc(sizeof(cpuset_t), M_NVMM, M_WAITOK | M_ZERO); })
#define os_cpuset_destroy(s)	free((s), M_NVMM)
#define os_cpuset_isset(s, c)	CPU_ISSET(c, s)
#define os_cpuset_clear(s, c)	CPU_CLR_ATOMIC(c, s)
#define os_cpuset_setrunning(s)	CPU_OR_ATOMIC(s, &all_cpus)
#endif

/* Preemption. */
#if defined(__NetBSD__)
#define os_preempt_disable()	kpreempt_disable()
#define os_preempt_enable()	kpreempt_enable()
#define os_preempt_disabled()	kpreempt_disabled()
#elif defined(__DragonFly__)
/*
 * In DragonFly, a thread in kernel mode will not be preemptively migrated
 * to another CPU or preemptively switched to another normal kernel thread,
 * but can be preemptively switched to an interrupt thread (which switches
 * back to the kernel thread it preempted the instant it is done or blocks).
 *
 * However, we still need to use a critical section to prevent this nominal
 * interrupt thread preemption to avoid exposing interrupt threads to
 * guest DB and FP register state.  We operate under the assumption that
 * the hard interrupt code won't mess with this state.
 */
#define os_preempt_disable()	crit_enter()
#define os_preempt_enable()	crit_exit()
#define os_preempt_disabled()	(curthread->td_critcount != 0)
#elif defined(__FreeBSD__)
#define os_preempt_disable()	critical_enter()
#define os_preempt_enable()	critical_exit()
#define os_preempt_disabled()	(curthread->td_critnest != 0)
#endif

/* Asserts. */
#if defined(__NetBSD__)
#define OS_ASSERT		KASSERT
#define OS_ASSERT_MTX_OWNED(m)	OS_ASSERT(mutex_owned(m))
#elif defined(__DragonFly__)
#define OS_ASSERT		KKASSERT
#define OS_ASSERT_MTX_OWNED(m)	\
	OS_ASSERT(lockstatus(m, curthread) == LK_EXCLUSIVE)
#elif defined(__FreeBSD__)
#define OS_ASSERT(x)		KASSERT(x, ("%s", #x))
#define OS_ASSERT_MTX_OWNED(m)	sx_assert(m, SA_XLOCKED)
#endif

/* Userspace access. */
#if defined(__NetBSD__) || defined(__DragonFly__) || defined(__FreeBSD__)
#define os_copy_from_user	copyin
#define os_copy_to_user		copyout
#endif

/* Vmspace. */
#if defined(__NetBSD__) || defined(__DragonFly__) || defined(__FreeBSD__)
#define os_vmspace_get_vmmap(_vm_)	(&(_vm_)->vm_map)
#endif

/* Misc. */
#if defined(__DragonFly__)
#define uimin(a, b)		((u_int)a < (u_int)b ? (u_int)a : (u_int)b)
#elif defined(__FreeBSD__)
#define uimin(a, b)		min((u_int)(a), (u_int)(b))
#endif

os_vmspace_t *	os_vmspace_create(vaddr_t, vaddr_t);
void		os_vmspace_destroy(os_vmspace_t *);
int		os_vmspace_fault(os_vmspace_t *, vaddr_t, vm_prot_t);

os_vmobj_t *	os_vmobj_create(voff_t);
void		os_vmobj_ref(os_vmobj_t *);
void		os_vmobj_rel(os_vmobj_t *);

int		os_vmobj_map_user(os_vmmap_t *, vaddr_t *, vsize_t,
		    os_vmobj_t *, voff_t, bool, bool, bool, int, int);
int		os_vmobj_map_kern(os_vmmap_t *, vaddr_t *, vsize_t,
		    os_vmobj_t *, voff_t, bool, bool, bool, int, int);
void		os_vmobj_unmap(os_vmmap_t *, vaddr_t, vaddr_t, bool);

void *		os_pagemem_zalloc(size_t);
void		os_pagemem_free(void *, size_t);

paddr_t		os_pa_zalloc(void);
void		os_pa_free(paddr_t);

int		os_contigpa_zalloc(paddr_t *, vaddr_t *, size_t);
void		os_contigpa_free(paddr_t, vaddr_t, size_t);

time_t		os_time(void);

#if defined(__FreeBSD__)
/*
 * FreeBSD picks the pmap type when the vmspace is allocated, it cannot
 * turn a regular pmap into an EPT one afterwards the way NetBSD's
 * pmap_ept_transform() does. The backend sets this hook in its init
 * function; os_vmspace_create() fails while it is NULL.
 */
extern int (*os_vmspace_pinit)(pmap_t);
/* Installs the EPT pinit; has_ad = the CPU's EPT sets accessed/dirty bits. */
void		os_vmspace_ept_setup(bool has_ad);
/* IPI vector that interrupts a CPU running a guest (EPT pmap, kicks). */
int		os_ipi_vector(void);
#ifdef NVMM_DEBUG
/* Test-only: one guest page through the EPT, to or from userland. */
int		os_vmspace_debug_access(os_vmspace_t *, gpaddr_t, bool, void *);
#endif
#endif

static inline bool
os_return_needed(void)
{
#if defined(__NetBSD__)
	if (preempt_needed()) {
		return true;
	}
	if (curlwp->l_flag & LW_USERRET) {
		return true;
	}
	return false;
#elif defined(__DragonFly__)
	if (__predict_false(hvm_break_wanted())) {
		return true;
	}
	if (__predict_false(curthread->td_lwp->lwp_mpflags & LWP_MP_URETMASK)) {
		return true;
	}
	return false;
#elif defined(__FreeBSD__)
	struct thread *td = curthread;

	/* Same test as bhyve's vcpu_should_yield(). */
	if (__predict_false(td->td_ast != 0 || td->td_owepreempt != 0)) {
		return true;
	}
	return false;
#endif
}

/* IPIs. */
#if defined(__NetBSD__)

#include <sys/xcall.h>
#define OS_IPI_FUNC(func)	void func(void *arg, void *unused)

static inline void
os_ipi_unicast(os_cpu_t *cpu, void (*func)(void *, void *), void *arg)
{
	xc_wait(xc_unicast(XC_HIGHPRI, func, arg, NULL, cpu));
}

static inline void
os_ipi_broadcast(void (*func)(void *, void *), void *arg)
{
	xc_wait(xc_broadcast(0, func, arg, NULL));
}

static inline void
os_ipi_kickall(void)
{
	/*
	 * XXX: this is probably too expensive. NetBSD should have a dummy
	 * interrupt handler that just IRETs without doing anything.
	 */
	pmap_tlb_shootdown(pmap_kernel(), -1, PTE_G, TLBSHOOT_NVMM);
}

#elif defined(__DragonFly__)

#include <sys/thread2.h>
#define OS_IPI_FUNC(func)	void func(void *arg)

static inline void
os_ipi_unicast(os_cpu_t *cpu, void (*func)(void *), void *arg)
{
	int seq;

	seq = lwkt_send_ipiq(cpu, func, arg);
	lwkt_wait_ipiq(cpu, seq);
}

static inline void
os_ipi_broadcast(void (*func)(void *), void *arg)
{
	cpumask_t mask;
	int i;

	for (i = 0; i < ncpus; i++) {
		CPUMASK_ASSBIT(mask, i);
		lwkt_cpusync_simple(mask, func, arg);
	}
}

/* os_ipi_kickall() not used */

/*
 * On DragonFly, no need to bind the thread, because any normal kernel
 * thread will not migrate to another CPU or be preempted (except by an
 * interrupt thread).
 */
#define curlwp_bind()		((int)0)
#define curlwp_bindx(bound)	/* nothing */

#elif defined(__FreeBSD__)

#define OS_IPI_FUNC(func)	void func(void *arg)

static inline void
os_ipi_unicast(os_cpu_t *cpu, void (*func)(void *), void *arg)
{
	cpuset_t set;

	CPU_SETOF(cpu->pc_cpuid, &set);
	smp_rendezvous_cpus(set, smp_no_rendezvous_barrier, func,
	    smp_no_rendezvous_barrier, arg);
}

static inline void
os_ipi_broadcast(void (*func)(void *), void *arg)
{
	smp_rendezvous(smp_no_rendezvous_barrier, func,
	    smp_no_rendezvous_barrier, arg);
}

/* os_ipi_kickall() not used */

/* NetBSD's curlwp_bind() keeps the thread on its CPU: sched_pin(). */
#define curlwp_bind()		({ sched_pin(); 0; })
#define curlwp_bindx(bound)	sched_unpin()

#endif

#endif /* _NVMM_OS_H_ */
