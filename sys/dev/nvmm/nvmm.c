/*	$NetBSD: nvmm.c,v 1.51 2026/11/21 15:26:31 skrll Exp $	*/

/*
 * Copyright (c) 2018-2020 Maxime Villard, m00nbsd.net
 * Copyright (c) 2026 Sarah Chiandotto (FreeBSD port)
 * All rights reserved.
 *
 * This code is part of the NVMM hypervisor.
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

/*
 * Machine-independent frontend, from NetBSD's sys/dev/nvmm/nvmm.c. The
 * direct UVM, kmem and lock calls go through the os_* layer of
 * nvmm_os.h, the way DragonFly does it; the device, module and
 * suspend/resume glue lives in nvmm_<os>.c.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/mman.h>
#include <sys/proc.h>

#include <dev/nvmm/nvmm.h>
#include <dev/nvmm/nvmm_os.h>
#include <dev/nvmm/nvmm_internal.h>
#include <dev/nvmm/nvmm_ioctl.h>

static struct nvmm_machine machines[NVMM_MAX_MACHINES];
volatile unsigned int nmachines __cacheline_aligned;

static struct {
	os_mtx_t	lock;
	os_cv_t		suspendcv;
	os_cv_t		resumecv;
	unsigned	users;
} suspension;

volatile bool nvmm_suspending;

static const struct nvmm_impl *nvmm_impl_list[] = {
#if defined(__x86_64__)
#if !defined(__FreeBSD__)
	&nvmm_x86_svm,	/* x86 AMD SVM */
#endif
	&nvmm_x86_vmx	/* x86 Intel VMX */
#endif
};

const struct nvmm_impl *nvmm_impl __read_mostly = NULL;

struct nvmm_owner nvmm_root_owner;

/* -------------------------------------------------------------------------- */

static int
nvmm_enter_sig(void)
{
	int error;

	os_mtx_lock(&suspension.lock);
	while (nvmm_suspending) {
		error = os_cv_wait_sig(&suspension.resumecv, &suspension.lock);
		if (error)
			goto out;
	}
	OS_ASSERT(suspension.users < UINT_MAX);
	suspension.users++;
	error = 0;
out:	os_mtx_unlock(&suspension.lock);

	/*
	 * NetBSD returns 0 here even when the wait was interrupted, and
	 * nvmm_ioctl() then calls nvmm_exit() without a matching users++.
	 */
	return error;
}

static void
nvmm_enter(void)
{

	os_mtx_lock(&suspension.lock);
	while (nvmm_suspending)
		os_cv_wait(&suspension.resumecv, &suspension.lock);
	OS_ASSERT(suspension.users < UINT_MAX);
	suspension.users++;
	os_mtx_unlock(&suspension.lock);
}

static void
nvmm_exit(void)
{

	os_mtx_lock(&suspension.lock);
	OS_ASSERT(suspension.users > 0);
	if (--suspension.users == 0)
		os_cv_signal(&suspension.suspendcv);
	os_mtx_unlock(&suspension.lock);
}

/* -------------------------------------------------------------------------- */

static int
nvmm_machine_alloc(struct nvmm_machine **ret)
{
	struct nvmm_machine *mach;
	size_t i;

	for (i = 0; i < NVMM_MAX_MACHINES; i++) {
		mach = &machines[i];

		os_rwl_wlock(&mach->lock);
		if (mach->present) {
			os_rwl_unlock(&mach->lock);
			continue;
		}

		mach->present = true;
		mach->time = os_time();
		*ret = mach;
		os_atomic_inc_uint(&nmachines);
		return 0;
	}

	return ENOBUFS;
}

static void
nvmm_machine_free(struct nvmm_machine *mach)
{
	OS_ASSERT(os_rwl_wheld(&mach->lock));
	OS_ASSERT(mach->present);
	mach->present = false;
	os_atomic_dec_uint(&nmachines);
}

static int
nvmm_machine_get(struct nvmm_owner *owner, nvmm_machid_t machid,
    struct nvmm_machine **ret, bool writer)
{
	struct nvmm_machine *mach;

	if (__predict_false(machid >= NVMM_MAX_MACHINES)) {
		return EINVAL;
	}
	mach = &machines[machid];

	if (writer) {
		os_rwl_wlock(&mach->lock);
	} else {
		os_rwl_rlock(&mach->lock);
	}
	if (__predict_false(!mach->present)) {
		os_rwl_unlock(&mach->lock);
		return ENOENT;
	}
	if (__predict_false(mach->owner != owner &&
	    owner != &nvmm_root_owner)) {
		os_rwl_unlock(&mach->lock);
		return EPERM;
	}
	*ret = mach;

	return 0;
}

static void
nvmm_machine_put(struct nvmm_machine *mach)
{
	os_rwl_unlock(&mach->lock);
}

/* -------------------------------------------------------------------------- */

static int
nvmm_vcpu_alloc(struct nvmm_machine *mach, nvmm_cpuid_t cpuid,
    struct nvmm_cpu **ret)
{
	struct nvmm_cpu *vcpu;

	if (cpuid >= NVMM_MAX_VCPUS) {
		return EINVAL;
	}
	vcpu = &mach->cpus[cpuid];

	os_mtx_lock(&vcpu->lock);
	if (vcpu->present) {
		os_mtx_unlock(&vcpu->lock);
		return EBUSY;
	}

	vcpu->present = true;
	vcpu->comm = NULL;
	vcpu->hcpu_last = -1;
	*ret = vcpu;
	return 0;
}

static void
nvmm_vcpu_free(struct nvmm_machine *mach, struct nvmm_cpu *vcpu)
{
	OS_ASSERT_MTX_OWNED(&vcpu->lock);
	vcpu->present = false;
	if (vcpu->comm != NULL) {
		os_vmobj_unmap(os_kernel_map, (vaddr_t)vcpu->comm,
		    (vaddr_t)vcpu->comm + PAGE_SIZE, true);
		vcpu->comm = NULL;
	}
}

static int
nvmm_vcpu_get(struct nvmm_machine *mach, nvmm_cpuid_t cpuid,
    struct nvmm_cpu **ret)
{
	struct nvmm_cpu *vcpu;

	if (__predict_false(cpuid >= NVMM_MAX_VCPUS)) {
		return EINVAL;
	}
	vcpu = &mach->cpus[cpuid];

	os_mtx_lock(&vcpu->lock);
	if (__predict_false(!vcpu->present)) {
		os_mtx_unlock(&vcpu->lock);
		return ENOENT;
	}
	*ret = vcpu;

	return 0;
}

static void
nvmm_vcpu_put(struct nvmm_cpu *vcpu)
{
	os_mtx_unlock(&vcpu->lock);
}

/* -------------------------------------------------------------------------- */

void
nvmm_kill_machines(struct nvmm_owner *owner)
{
	struct nvmm_machine *mach;
	struct nvmm_cpu *vcpu;
	size_t i, j;
	int error;

	for (i = 0; i < NVMM_MAX_MACHINES; i++) {
		mach = &machines[i];

		os_rwl_wlock(&mach->lock);
		if (!mach->present || mach->owner != owner) {
			os_rwl_unlock(&mach->lock);
			continue;
		}

		/* Kill it. */
		for (j = 0; j < NVMM_MAX_VCPUS; j++) {
			error = nvmm_vcpu_get(mach, j, &vcpu);
			if (error)
				continue;
			(*nvmm_impl->vcpu_destroy)(mach, vcpu);
			nvmm_vcpu_free(mach, vcpu);
			nvmm_vcpu_put(vcpu);
			os_atomic_dec_uint(&mach->ncpus);
		}
		(*nvmm_impl->machine_destroy)(mach);

		/*
		 * Free the comm object too: NetBSD leaks it on this path
		 * (nvmm_machine_destroy() does free it).
		 */
		os_vmobj_rel(mach->commobj);
		os_vmspace_destroy(mach->vm);

		/* Drop the kernel VM object refs. */
		for (j = 0; j < NVMM_MAX_HMAPPINGS; j++) {
			if (!mach->hmap[j].present)
				continue;
			os_vmobj_rel(mach->hmap[j].vmobj);
		}

		nvmm_machine_free(mach);

		os_rwl_unlock(&mach->lock);
	}
}

/* -------------------------------------------------------------------------- */

static int
nvmm_capability(struct nvmm_owner *owner, struct nvmm_ioc_capability *args)
{
	args->cap.version = NVMM_KERN_VERSION;
	args->cap.state_size = nvmm_impl->state_size;
	args->cap.max_machines = NVMM_MAX_MACHINES;
	args->cap.max_vcpus = NVMM_MAX_VCPUS;
	args->cap.max_ram = NVMM_MAX_RAM;

	(*nvmm_impl->capability)(&args->cap);

	return 0;
}

static int
nvmm_machine_create(struct nvmm_owner *owner,
    struct nvmm_ioc_machine_create *args)
{
	struct nvmm_machine *mach;
	int error;

	error = nvmm_machine_alloc(&mach);
	if (error)
		return error;

	/* Curproc owns the machine. */
	mach->owner = owner;

	/* Zero out the host mappings. */
	memset(&mach->hmap, 0, sizeof(mach->hmap));

	/* Create the machine vmspace. */
	mach->gpa_begin = 0;
	mach->gpa_end = NVMM_MAX_RAM;
	mach->vm = os_vmspace_create(mach->gpa_begin, mach->gpa_end);
	if (mach->vm == NULL) {
		nvmm_machine_free(mach);
		nvmm_machine_put(mach);
		return ENOMEM;
	}

	/* Create the comm VM object. */
	mach->commobj = os_vmobj_create(NVMM_MAX_VCPUS * PAGE_SIZE);

	(*nvmm_impl->machine_create)(mach);

	args->machid = mach->machid;
	nvmm_machine_put(mach);

	return 0;
}

static int
nvmm_machine_destroy(struct nvmm_owner *owner,
    struct nvmm_ioc_machine_destroy *args)
{
	struct nvmm_machine *mach;
	struct nvmm_cpu *vcpu;
	int error;
	size_t i;

	error = nvmm_machine_get(owner, args->machid, &mach, true);
	if (error)
		return error;

	for (i = 0; i < NVMM_MAX_VCPUS; i++) {
		error = nvmm_vcpu_get(mach, i, &vcpu);
		if (error)
			continue;

		(*nvmm_impl->vcpu_destroy)(mach, vcpu);
		nvmm_vcpu_free(mach, vcpu);
		nvmm_vcpu_put(vcpu);
		os_atomic_dec_uint(&mach->ncpus);
	}

	(*nvmm_impl->machine_destroy)(mach);

	/* Free the comm VM object. */
	os_vmobj_rel(mach->commobj);

	/* Free the machine vmspace. */
	os_vmspace_destroy(mach->vm);

	/* Drop the kernel VM object refs. */
	for (i = 0; i < NVMM_MAX_HMAPPINGS; i++) {
		if (!mach->hmap[i].present)
			continue;
		os_vmobj_rel(mach->hmap[i].vmobj);
	}

	nvmm_machine_free(mach);
	nvmm_machine_put(mach);

	return 0;
}

static int
nvmm_machine_configure(struct nvmm_owner *owner,
    struct nvmm_ioc_machine_configure *args)
{
	struct nvmm_machine *mach;
	size_t allocsz;
	uint64_t op;
	void *data;
	int error;

	op = NVMM_MACH_CONF_MD(args->op);
	if (__predict_false(op >= nvmm_impl->mach_conf_max)) {
		return EINVAL;
	}

	allocsz = nvmm_impl->mach_conf_sizes[op];
	data = os_mem_alloc(allocsz);

	error = nvmm_machine_get(owner, args->machid, &mach, true);
	if (error) {
		os_mem_free(data, allocsz);
		return error;
	}

	error = copyin(args->conf, data, allocsz);
	if (error) {
		goto out;
	}

	error = (*nvmm_impl->machine_configure)(mach, op, data);

out:
	nvmm_machine_put(mach);
	os_mem_free(data, allocsz);
	return error;
}

static int
nvmm_vcpu_create(struct nvmm_owner *owner, struct nvmm_ioc_vcpu_create *args)
{
	struct nvmm_machine *mach;
	struct nvmm_cpu *vcpu;
	int error;

	error = nvmm_machine_get(owner, args->machid, &mach, false);
	if (error)
		return error;

	error = nvmm_vcpu_alloc(mach, args->cpuid, &vcpu);
	if (error)
		goto out;

	/*
	 * Map the comm page on the kernel side, as wired. Userland maps the
	 * same page with mmap() on /dev/nvmm (NetBSD ABI, see
	 * nvmm_comm_object()).
	 */
	error = os_vmobj_map_kern(os_kernel_map, (vaddr_t *)&vcpu->comm,
	    PAGE_SIZE, mach->commobj, args->cpuid * PAGE_SIZE,
	    true /* wired */, false /* !fixed */, true /* shared */,
	    PROT_READ | PROT_WRITE, PROT_READ | PROT_WRITE);
	if (error) {
		vcpu->comm = NULL;
		nvmm_vcpu_free(mach, vcpu);
		nvmm_vcpu_put(vcpu);
		goto out;
	}
	memset(vcpu->comm, 0, PAGE_SIZE);

	error = (*nvmm_impl->vcpu_create)(mach, vcpu);
	if (error) {
		nvmm_vcpu_free(mach, vcpu);
		nvmm_vcpu_put(vcpu);
		goto out;
	}

	nvmm_vcpu_put(vcpu);
	os_atomic_inc_uint(&mach->ncpus);

out:
	nvmm_machine_put(mach);
	return error;
}

static int
nvmm_vcpu_destroy(struct nvmm_owner *owner, struct nvmm_ioc_vcpu_destroy *args)
{
	struct nvmm_machine *mach;
	struct nvmm_cpu *vcpu;
	int error;

	error = nvmm_machine_get(owner, args->machid, &mach, false);
	if (error)
		return error;

	error = nvmm_vcpu_get(mach, args->cpuid, &vcpu);
	if (error)
		goto out;

	(*nvmm_impl->vcpu_destroy)(mach, vcpu);
	nvmm_vcpu_free(mach, vcpu);
	nvmm_vcpu_put(vcpu);
	os_atomic_dec_uint(&mach->ncpus);

out:
	nvmm_machine_put(mach);
	return error;
}

static int
nvmm_vcpu_configure(struct nvmm_owner *owner,
    struct nvmm_ioc_vcpu_configure *args)
{
	struct nvmm_machine *mach;
	struct nvmm_cpu *vcpu;
	size_t allocsz;
	uint64_t op;
	void *data;
	int error;

	op = NVMM_VCPU_CONF_MD(args->op);
	if (__predict_false(op >= nvmm_impl->vcpu_conf_max))
		return EINVAL;

	allocsz = nvmm_impl->vcpu_conf_sizes[op];
	data = os_mem_alloc(allocsz);

	error = nvmm_machine_get(owner, args->machid, &mach, false);
	if (error) {
		os_mem_free(data, allocsz);
		return error;
	}

	error = nvmm_vcpu_get(mach, args->cpuid, &vcpu);
	if (error) {
		nvmm_machine_put(mach);
		os_mem_free(data, allocsz);
		return error;
	}

	error = copyin(args->conf, data, allocsz);
	if (error) {
		goto out;
	}

	error = (*nvmm_impl->vcpu_configure)(vcpu, op, data);

out:
	nvmm_vcpu_put(vcpu);
	nvmm_machine_put(mach);
	os_mem_free(data, allocsz);
	return error;
}

static int
nvmm_vcpu_setstate(struct nvmm_owner *owner,
    struct nvmm_ioc_vcpu_setstate *args)
{
	struct nvmm_machine *mach;
	struct nvmm_cpu *vcpu;
	int error;

	error = nvmm_machine_get(owner, args->machid, &mach, false);
	if (error)
		return error;

	error = nvmm_vcpu_get(mach, args->cpuid, &vcpu);
	if (error)
		goto out;

	(*nvmm_impl->vcpu_setstate)(vcpu);
	nvmm_vcpu_put(vcpu);

out:
	nvmm_machine_put(mach);
	return error;
}

static int
nvmm_vcpu_getstate(struct nvmm_owner *owner,
    struct nvmm_ioc_vcpu_getstate *args)
{
	struct nvmm_machine *mach;
	struct nvmm_cpu *vcpu;
	int error;

	error = nvmm_machine_get(owner, args->machid, &mach, false);
	if (error)
		return error;

	error = nvmm_vcpu_get(mach, args->cpuid, &vcpu);
	if (error)
		goto out;

	(*nvmm_impl->vcpu_getstate)(vcpu);
	nvmm_vcpu_put(vcpu);

out:
	nvmm_machine_put(mach);
	return error;
}

static int
nvmm_vcpu_inject(struct nvmm_owner *owner, struct nvmm_ioc_vcpu_inject *args)
{
	struct nvmm_machine *mach;
	struct nvmm_cpu *vcpu;
	int error;

	error = nvmm_machine_get(owner, args->machid, &mach, false);
	if (error)
		return error;

	error = nvmm_vcpu_get(mach, args->cpuid, &vcpu);
	if (error)
		goto out;

	error = (*nvmm_impl->vcpu_inject)(vcpu);
	nvmm_vcpu_put(vcpu);

out:
	nvmm_machine_put(mach);
	return error;
}

static int
nvmm_do_vcpu_run(struct nvmm_machine *mach, struct nvmm_cpu *vcpu,
    struct nvmm_vcpu_exit *exit)
{
	os_vmspace_t *vm = mach->vm;
	int ret;

	while (1) {
		/* Got a signal? Or pending resched? Leave. */
		if (__predict_false(nvmm_return_needed(vcpu, exit))) {
			return 0;
		}

		/* Run the VCPU. */
		ret = (*nvmm_impl->vcpu_run)(mach, vcpu, exit);
		if (__predict_false(ret != 0)) {
			return ret;
		}

		/* Process nested page faults. */
		if (__predict_true(exit->reason != NVMM_VCPU_EXIT_MEMORY)) {
			break;
		}
		if (exit->u.mem.gpa >= mach->gpa_end) {
			break;
		}
		if (os_vmspace_fault(vm, exit->u.mem.gpa, exit->u.mem.prot)) {
			break;
		}
	}

	return 0;
}

static int
nvmm_vcpu_run(struct nvmm_owner *owner, struct nvmm_ioc_vcpu_run *args)
{
	struct nvmm_machine *mach;
	struct nvmm_cpu *vcpu = NULL;
	int error;

	error = nvmm_machine_get(owner, args->machid, &mach, false);
	if (error)
		return error;

	error = nvmm_vcpu_get(mach, args->cpuid, &vcpu);
	if (error)
		goto out;

	error = nvmm_do_vcpu_run(mach, vcpu, &args->exit);
	vcpu->comm->stop = 0;
	nvmm_vcpu_put(vcpu);

out:
	nvmm_machine_put(mach);
	return error;
}

/* -------------------------------------------------------------------------- */

static os_vmobj_t *
nvmm_hmapping_getvmobj(struct nvmm_machine *mach, uintptr_t hva, size_t size,
   size_t *off)
{
	struct nvmm_hmapping *hmapping;
	size_t i;

	for (i = 0; i < NVMM_MAX_HMAPPINGS; i++) {
		hmapping = &mach->hmap[i];
		if (!hmapping->present) {
			continue;
		}
		if (hva >= hmapping->hva &&
		    hva + size <= hmapping->hva + hmapping->size) {
			*off = hva - hmapping->hva;
			return hmapping->vmobj;
		}
	}

	return NULL;
}

static int
nvmm_hmapping_validate(struct nvmm_machine *mach, uintptr_t hva, size_t size)
{
	struct nvmm_hmapping *hmapping;
	size_t i;
	uintptr_t hva_end;
	uintptr_t hmap_end;

	if ((hva % PAGE_SIZE) != 0 || (size % PAGE_SIZE) != 0) {
		return EINVAL;
	}
	if (hva == 0) {
		return EINVAL;
	}

	/*
	 * Overflow tests MUST be done very carefully to avoid compiler
	 * optimizations from effectively deleting the test.
	 */
	hva_end = hva + size;
	if (hva_end <= hva)
		return EINVAL;

	/*
	 * Overlap tests
	 */
	for (i = 0; i < NVMM_MAX_HMAPPINGS; i++) {
		hmapping = &mach->hmap[i];

		if (!hmapping->present) {
			continue;
		}
		hmap_end = hmapping->hva + hmapping->size;

		if (hva >= hmapping->hva && hva_end <= hmap_end)
			break;
		if (hva >= hmapping->hva && hva < hmap_end)
			return EEXIST;
		if (hva_end > hmapping->hva && hva_end <= hmap_end)
			return EEXIST;
		if (hva <= hmapping->hva && hva_end >= hmap_end)
			return EEXIST;
	}

	return 0;
}

static struct nvmm_hmapping *
nvmm_hmapping_alloc(struct nvmm_machine *mach)
{
	struct nvmm_hmapping *hmapping;
	size_t i;

	for (i = 0; i < NVMM_MAX_HMAPPINGS; i++) {
		hmapping = &mach->hmap[i];
		if (!hmapping->present) {
			hmapping->present = true;
			return hmapping;
		}
	}

	return NULL;
}

static int
nvmm_hmapping_free(struct nvmm_machine *mach, uintptr_t hva, size_t size)
{
	struct nvmm_hmapping *hmapping;
	size_t i;

	for (i = 0; i < NVMM_MAX_HMAPPINGS; i++) {
		hmapping = &mach->hmap[i];
		if (!hmapping->present || hmapping->hva != hva ||
		    hmapping->size != size) {
			continue;
		}

		os_vmobj_unmap(os_curproc_map, hmapping->hva,
		    hmapping->hva + hmapping->size, false);
		os_vmobj_rel(hmapping->vmobj);

		hmapping->vmobj = NULL;
		hmapping->present = false;

		return 0;
	}

	return ENOENT;
}

static int
nvmm_hva_map(struct nvmm_owner *owner, struct nvmm_ioc_hva_map *args)
{
	struct nvmm_machine *mach;
	struct nvmm_hmapping *hmapping;
	vaddr_t uva;
	int error;

	error = nvmm_machine_get(owner, args->machid, &mach, true);
	if (error)
		return error;

	error = nvmm_hmapping_validate(mach, args->hva, args->size);
	if (error)
		goto out;

	hmapping = nvmm_hmapping_alloc(mach);
	if (hmapping == NULL) {
		error = ENOBUFS;
		goto out;
	}

	hmapping->hva = args->hva;
	hmapping->size = args->size;
	hmapping->vmobj = os_vmobj_create(hmapping->size);
	uva = hmapping->hva;

	/*
	 * Map the VM object into the user address space, as pageable. The
	 * mapping takes its own reference.
	 */
	error = os_vmobj_map_user(os_curproc_map, &uva, hmapping->size,
	    hmapping->vmobj, 0, false /* !wired */, true /* fixed */,
	    true /* shared */, PROT_READ | PROT_WRITE, PROT_READ | PROT_WRITE);
	if (error) {
		/*
		 * NetBSD leaves the slot marked present with a dead object
		 * here; release it instead.
		 */
		os_vmobj_rel(hmapping->vmobj);
		hmapping->vmobj = NULL;
		hmapping->present = false;
	}

out:
	nvmm_machine_put(mach);
	return error;
}

static int
nvmm_hva_unmap(struct nvmm_owner *owner, struct nvmm_ioc_hva_unmap *args)
{
	struct nvmm_machine *mach;
	int error;

	error = nvmm_machine_get(owner, args->machid, &mach, true);
	if (error)
		return error;

	error = nvmm_hmapping_free(mach, args->hva, args->size);

	nvmm_machine_put(mach);
	return error;
}

/* -------------------------------------------------------------------------- */

static int
nvmm_gpa_map(struct nvmm_owner *owner, struct nvmm_ioc_gpa_map *args)
{
	struct nvmm_machine *mach;
	os_vmobj_t *vmobj;
	gpaddr_t gpa;
	gpaddr_t gpa_end;
	size_t off;
	int error;

	error = nvmm_machine_get(owner, args->machid, &mach, false);
	if (error)
		return error;

	if ((args->prot & ~(PROT_READ|PROT_WRITE|PROT_EXEC)) != 0) {
		error = EINVAL;
		goto out;
	}

	/*
	 * Overflow tests MUST be done very carefully to avoid compiler
	 * optimizations from effectively deleting the test.
	 */
	gpa = args->gpa;
	gpa_end = gpa + args->size;
	if (gpa_end <= gpa) {
		error = EINVAL;
		goto out;
	}

	if ((gpa % PAGE_SIZE) != 0 || (args->size % PAGE_SIZE) != 0 ||
	    (args->hva % PAGE_SIZE) != 0) {
		error = EINVAL;
		goto out;
	}
	if (args->hva == 0) {
		error = EINVAL;
		goto out;
	}

	if (gpa < mach->gpa_begin || gpa >= mach->gpa_end) {
		error = EINVAL;
		goto out;
	}
	if (gpa_end  > mach->gpa_end) {
		error = EINVAL;
		goto out;
	}

	vmobj = nvmm_hmapping_getvmobj(mach, args->hva, args->size, &off);
	if (vmobj == NULL) {
		error = EINVAL;
		goto out;
	}

	/*
	 * Map the VM object into the machine address space, as pageable.
	 * The mapping takes its own reference.
	 */
	error = os_vmobj_map_user(os_vmspace_get_vmmap(mach->vm), &gpa,
	    args->size, vmobj, off, false /* !wired */, true /* fixed */,
	    false /* !shared */, args->prot,
	    PROT_READ | PROT_WRITE | PROT_EXEC);
	if (error) {
		goto out;
	}
	if (gpa != args->gpa) {
		os_vmobj_unmap(os_vmspace_get_vmmap(mach->vm), gpa,
		    gpa + args->size, false);
		os_printf("nvmm: gpa_map placed the mapping elsewhere\n");
		error = EINVAL;
		goto out;
	}

out:
	nvmm_machine_put(mach);
	return error;
}

static int
nvmm_gpa_unmap(struct nvmm_owner *owner, struct nvmm_ioc_gpa_unmap *args)
{
	struct nvmm_machine *mach;
	gpaddr_t gpa;
	gpaddr_t gpa_end;
	int error;

	error = nvmm_machine_get(owner, args->machid, &mach, false);
	if (error)
		return error;

	/*
	 * Overflow tests MUST be done very carefully to avoid compiler
	 * optimizations from effectively deleting the test.
	 */
	gpa = args->gpa;
	gpa_end = gpa + args->size;
	if (gpa_end <= gpa) {
		error = EINVAL;
		goto out;
	}

	if ((gpa % PAGE_SIZE) != 0 || (args->size % PAGE_SIZE) != 0) {
		error = EINVAL;
		goto out;
	}
	if (gpa < mach->gpa_begin || gpa >= mach->gpa_end) {
		error = EINVAL;
		goto out;
	}
	/* NetBSD has >= here, which refuses the last page; DragonFly fixed it. */
	if (gpa_end > mach->gpa_end) {
		error = EINVAL;
		goto out;
	}

	/* Unmap the memory from the machine. */
	os_vmobj_unmap(os_vmspace_get_vmmap(mach->vm), gpa, gpa + args->size,
	    false);

out:
	nvmm_machine_put(mach);
	return error;
}

/* -------------------------------------------------------------------------- */

static int
nvmm_ctl_mach_info(struct nvmm_owner *owner, struct nvmm_ioc_ctl *args)
{
	struct nvmm_ctl_mach_info ctl;
	struct nvmm_machine *mach;
	int error;
	size_t i;

	if (args->size != sizeof(ctl))
		return EINVAL;
	error = copyin(args->data, &ctl, sizeof(ctl));
	if (error)
		return error;

	error = nvmm_machine_get(owner, ctl.machid, &mach, true);
	if (error)
		return error;

	ctl.nvcpus = mach->ncpus;

	ctl.nram = 0;
	for (i = 0; i < NVMM_MAX_HMAPPINGS; i++) {
		if (!mach->hmap[i].present)
			continue;
		ctl.nram += mach->hmap[i].size;
	}

	ctl.pid = mach->owner->pid;
	ctl.time = mach->time;

	nvmm_machine_put(mach);

	error = copyout(&ctl, args->data, sizeof(ctl));
	if (error)
		return error;

	return 0;
}

static int
nvmm_ctl(struct nvmm_owner *owner, struct nvmm_ioc_ctl *args)
{
	switch (args->op) {
	case NVMM_CTL_MACH_INFO:
		return nvmm_ctl_mach_info(owner, args);
	default:
		return EINVAL;
	}
}

/* -------------------------------------------------------------------------- */

const struct nvmm_impl *
nvmm_ident(void)
{
	size_t i;

	for (i = 0; i < __arraycount(nvmm_impl_list); i++) {
		if ((*nvmm_impl_list[i]->ident)())
			return nvmm_impl_list[i];
	}

	return NULL;
}

static void
nvmm_fini_locks(void)
{
	size_t i, n;

	for (i = 0; i < NVMM_MAX_MACHINES; i++) {
		os_rwl_destroy(&machines[i].lock);
		for (n = 0; n < NVMM_MAX_VCPUS; n++) {
			os_mtx_destroy(&machines[i].cpus[n].lock);
		}
	}

	/* NetBSD does not destroy these. */
	os_cv_destroy(&suspension.resumecv);
	os_cv_destroy(&suspension.suspendcv);
	os_mtx_destroy(&suspension.lock);
}

int
nvmm_init(void)
{
	size_t i, n;
	int error;

	nvmm_impl = nvmm_ident();
	if (nvmm_impl == NULL)
		return ENOTSUP;

	for (i = 0; i < NVMM_MAX_MACHINES; i++) {
		machines[i].machid = i;
		os_rwl_init(&machines[i].lock);
		for (n = 0; n < NVMM_MAX_VCPUS; n++) {
			machines[i].cpus[n].present = false;
			machines[i].cpus[n].cpuid = n;
			os_mtx_init(&machines[i].cpus[n].lock);
		}
	}

	os_mtx_init(&suspension.lock);
	os_cv_init(&suspension.suspendcv, "nvmmsus");
	os_cv_init(&suspension.resumecv, "nvmmres");
	suspension.users = 0;

	error = (*nvmm_impl->init)();
	if (error) {
		nvmm_fini_locks();
		nvmm_impl = NULL;
		return error;
	}

	return 0;
}

void
nvmm_fini(void)
{
	nvmm_fini_locks();

	(*nvmm_impl->fini)();
	nvmm_impl = NULL;
}

/*
 * Lock-free snapshot for statistics: a machine or vCPU created or destroyed
 * meanwhile may or may not be counted.
 */
void
nvmm_count(unsigned int *nmach, unsigned int *nvcpu)
{
	size_t i, n;

	*nmach = *nvcpu = 0;
	for (i = 0; i < NVMM_MAX_MACHINES; i++) {
		if (!os_atomic_load_bool(&machines[i].present))
			continue;
		(*nmach)++;
		for (n = 0; n < NVMM_MAX_VCPUS; n++) {
			if (os_atomic_load_bool(&machines[i].cpus[n].present))
				(*nvcpu)++;
		}
	}
}

/* -------------------------------------------------------------------------- */

/*
 * The mmap() side of the NetBSD comm page ABI: userland maps the page of
 * vCPU 'cpuid' of machine 'machid' at offset NVMM_COMM_OFF(machid, cpuid)
 * of /dev/nvmm. Returns a referenced VM object and the offset inside it;
 * the OS glue hands them to its mmap path.
 */
int
nvmm_comm_object(struct nvmm_owner *owner, uint64_t off, size_t size,
    int prot, os_vmobj_t **vmobjp, uint64_t *objoffp)
{
	struct nvmm_machine *mach;
	nvmm_machid_t machid;
	nvmm_cpuid_t cpuid;
	int error;

	if (prot & PROT_EXEC)
		return EACCES;
	if (size != PAGE_SIZE)
		return EINVAL;
	if ((off & ~NVMM_COMM_OFF(0xFF, 0xFF)) != 0)
		return EINVAL;

	cpuid = NVMM_COMM_CPUID(off);
	if (__predict_false(cpuid >= NVMM_MAX_VCPUS))
		return EINVAL;

	machid = NVMM_COMM_MACHID(off);
	error = nvmm_machine_get(owner, machid, &mach, false);
	if (error)
		return error;

	os_vmobj_ref(mach->commobj);
	*vmobjp = mach->commobj;
	*objoffp = cpuid * PAGE_SIZE;

	nvmm_machine_put(mach);
	return 0;
}

/*
 * Phase 1 of the FreeBSD port: only the capability query is wired up.
 * The other requests answer ENOSYS until their backend parts are ported.
 */
static const bool nvmm_ioctl_full = false;

static int
nvmm_ioctl_internal(struct nvmm_owner *owner, unsigned long cmd, void *data)
{
	OS_ASSERT(owner != NULL);

	if (!nvmm_ioctl_full && cmd != NVMM_IOC_CAPABILITY)
		return ENOSYS;

	switch (cmd) {
	case NVMM_IOC_CAPABILITY:
		return nvmm_capability(owner, data);
	case NVMM_IOC_MACHINE_CREATE:
		return nvmm_machine_create(owner, data);
	case NVMM_IOC_MACHINE_DESTROY:
		return nvmm_machine_destroy(owner, data);
	case NVMM_IOC_MACHINE_CONFIGURE:
		return nvmm_machine_configure(owner, data);
	case NVMM_IOC_VCPU_CREATE:
		return nvmm_vcpu_create(owner, data);
	case NVMM_IOC_VCPU_DESTROY:
		return nvmm_vcpu_destroy(owner, data);
	case NVMM_IOC_VCPU_CONFIGURE:
		return nvmm_vcpu_configure(owner, data);
	case NVMM_IOC_VCPU_SETSTATE:
		return nvmm_vcpu_setstate(owner, data);
	case NVMM_IOC_VCPU_GETSTATE:
		return nvmm_vcpu_getstate(owner, data);
	case NVMM_IOC_VCPU_INJECT:
		return nvmm_vcpu_inject(owner, data);
	case NVMM_IOC_VCPU_RUN:
		return nvmm_vcpu_run(owner, data);
	case NVMM_IOC_GPA_MAP:
		return nvmm_gpa_map(owner, data);
	case NVMM_IOC_GPA_UNMAP:
		return nvmm_gpa_unmap(owner, data);
	case NVMM_IOC_HVA_MAP:
		return nvmm_hva_map(owner, data);
	case NVMM_IOC_HVA_UNMAP:
		return nvmm_hva_unmap(owner, data);
	case NVMM_IOC_CTL:
		return nvmm_ctl(owner, data);
	default:
		return EINVAL;
	}
}

int
nvmm_ioctl(struct nvmm_owner *owner, unsigned long cmd, void *data)
{
	int error;

	error = nvmm_enter_sig();
	if (error)
		return error;
	error = nvmm_ioctl_internal(owner, cmd, data);
	nvmm_exit();

	return error;
}

/*
 * Called by the OS glue when the descriptor goes away, under the same
 * suspend protection as the ioctls.
 */
void
nvmm_close(struct nvmm_owner *owner)
{
	nvmm_enter();
	nvmm_kill_machines(owner);
	nvmm_exit();
}

/* -------------------------------------------------------------------------- */

static void
nvmm_suspend_vcpu(struct nvmm_machine *mach, struct nvmm_cpu *vcpu)
{

	os_mtx_lock(&vcpu->lock);
	if (vcpu->present && nvmm_impl->vcpu_suspend)
		(*nvmm_impl->vcpu_suspend)(mach, vcpu);
	os_mtx_unlock(&vcpu->lock);
}

static void
nvmm_resume_vcpu(struct nvmm_machine *mach, struct nvmm_cpu *vcpu)
{

	os_mtx_lock(&vcpu->lock);
	if (vcpu->present && nvmm_impl->vcpu_resume)
		(*nvmm_impl->vcpu_resume)(mach, vcpu);
	os_mtx_unlock(&vcpu->lock);
}

static void
nvmm_suspend_machine(struct nvmm_machine *mach)
{

	os_rwl_wlock(&mach->lock);
	if (mach->present) {
		if (nvmm_impl->vcpu_suspend) {
			size_t cpuid;

			for (cpuid = 0; cpuid < NVMM_MAX_VCPUS; cpuid++)
				nvmm_suspend_vcpu(mach, &mach->cpus[cpuid]);
		}
		if (nvmm_impl->machine_suspend)
			(*nvmm_impl->machine_suspend)(mach);
	}
	os_rwl_unlock(&mach->lock);
}

static void
nvmm_resume_machine(struct nvmm_machine *mach)
{

	os_rwl_wlock(&mach->lock);
	if (mach->present) {
		if (nvmm_impl->vcpu_resume) {
			size_t cpuid;

			for (cpuid = 0; cpuid < NVMM_MAX_VCPUS; cpuid++)
				nvmm_resume_vcpu(mach, &mach->cpus[cpuid]);
		}
		if (nvmm_impl->machine_resume)
			(*nvmm_impl->machine_resume)(mach);
	}
	os_rwl_unlock(&mach->lock);
}

void
nvmm_suspend(void)
{
	size_t i;

	/*
	 * Prevent new users (via ioctl) from starting.
	 */
	os_mtx_lock(&suspension.lock);
	OS_ASSERT(!nvmm_suspending);
	os_atomic_store_bool(&nvmm_suspending, true);
	os_mtx_unlock(&suspension.lock);

	/*
	 * Interrupt any running VMs so they will break out of run
	 * loops or anything else and not start up again until we've
	 * resumed.
	 */
	if (nvmm_impl->suspend_interrupt)
		(*nvmm_impl->suspend_interrupt)();

	/*
	 * Wait for any running VMs or other ioctls to finish running
	 * or handling any other ioctls.
	 */
	os_mtx_lock(&suspension.lock);
	while (suspension.users)
		os_cv_wait(&suspension.suspendcv, &suspension.lock);
	os_mtx_unlock(&suspension.lock);

	/*
	 * Suspend all the machines.
	 */
	if (nvmm_impl->machine_suspend || nvmm_impl->vcpu_suspend) {
		for (i = 0; i < NVMM_MAX_MACHINES; i++)
			nvmm_suspend_machine(&machines[i]);
	}

	/*
	 * Take any systemwide suspend action.
	 */
	if (nvmm_impl->suspend)
		(*nvmm_impl->suspend)();
}

void
nvmm_resume(void)
{
	size_t i;

	OS_ASSERT(os_atomic_load_bool(&nvmm_suspending));
	OS_ASSERT(suspension.users == 0);

	/*
	 * Take any systemwide resume action.
	 */
	if (nvmm_impl->resume)
		(*nvmm_impl->resume)();

	/*
	 * Resume all the machines.
	 */
	if (nvmm_impl->machine_resume || nvmm_impl->vcpu_resume) {
		for (i = 0; i < NVMM_MAX_MACHINES; i++)
			nvmm_resume_machine(&machines[i]);
	}

	/*
	 * Allow new users (via ioctl) to start again.
	 */
	os_mtx_lock(&suspension.lock);
	os_atomic_store_bool(&nvmm_suspending, false);
	os_cv_broadcast(&suspension.resumecv);
	os_mtx_unlock(&suspension.lock);
}
