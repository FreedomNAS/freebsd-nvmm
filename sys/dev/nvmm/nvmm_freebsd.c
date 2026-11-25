/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Sarah Chiandotto
 * All rights reserved.
 *
 * Based on DragonFly's sys/dev/virtual/nvmm/nvmm_dragonfly.c, written by
 * Aaron LI for The DragonFly Project, and on NetBSD's sys/dev/nvmm/nvmm.c
 * by Maxime Villard.
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
 * FreeBSD glue for NVMM: the os_* functions declared in nvmm_os.h, the
 * /dev/nvmm character device and the module event handler.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/conf.h>
#include <sys/eventhandler.h>
#include <sys/fcntl.h>
#include <sys/malloc.h>
#include <sys/mman.h>
#include <sys/module.h>
#include <sys/proc.h>
#include <sys/time.h>

#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/pmap.h>
#include <vm/vm_extern.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>

#include <dev/nvmm/nvmm.h>
#include <dev/nvmm/nvmm_os.h>
#include <dev/nvmm/nvmm_internal.h>
#include <dev/nvmm/nvmm_ioctl.h>

MALLOC_DEFINE(M_NVMM, "nvmm", "NVMM data");

int (*os_vmspace_pinit)(pmap_t) = NULL;

/* -------------------------------------------------------------------------- */

static vm_prot_t
os_prot_to_vm(int prot)
{
	vm_prot_t vmprot = VM_PROT_NONE;

	if (prot & PROT_READ)
		vmprot |= VM_PROT_READ;
	if (prot & PROT_WRITE)
		vmprot |= VM_PROT_WRITE;
	if (prot & PROT_EXEC)
		vmprot |= VM_PROT_EXECUTE;
	return vmprot;
}

os_vmspace_t *
os_vmspace_create(vaddr_t vmin, vaddr_t vmax)
{
	if (os_vmspace_pinit == NULL)
		return NULL;
	return vmspace_alloc(vmin, vmax, os_vmspace_pinit);
}

void
os_vmspace_destroy(os_vmspace_t *vm)
{
	vmspace_free(vm);
}

int
os_vmspace_fault(os_vmspace_t *vm, vaddr_t va, vm_prot_t prot)
{
	int rv;

	rv = vm_fault(&vm->vm_map, trunc_page(va), os_prot_to_vm(prot),
	    VM_FAULT_NORMAL, NULL);
	return rv == KERN_SUCCESS ? 0 : EFAULT;
}

os_vmobj_t *
os_vmobj_create(voff_t size)
{
	return vm_object_allocate(OBJT_SWAP, atop(round_page(size)));
}

void
os_vmobj_ref(os_vmobj_t *vmobj)
{
	vm_object_reference(vmobj);
}

void
os_vmobj_rel(os_vmobj_t *vmobj)
{
	vm_object_deallocate(vmobj);
}

static int
os_vmobj_map(os_vmmap_t *map, vaddr_t *addr, vsize_t size,
    os_vmobj_t *vmobj, voff_t offset, bool wired, bool fixed, bool shared,
    int prot, int maxprot, bool kernel)
{
	vm_prot_t vmprot, vmmaxprot;
	vm_offset_t start;
	int cow, rv;

	vmprot = os_prot_to_vm(prot);
	vmmaxprot = os_prot_to_vm(maxprot);
	cow = shared ? MAP_INHERIT_SHARE : 0;

	/* The mapping consumes this reference on success. */
	vm_object_reference(vmobj);

	if (fixed) {
		/* Replaces whatever is mapped in the range, like UVM_FLAG_UNMAP. */
		start = *addr;
		rv = vm_map_fixed(map, vmobj, offset, start, size, vmprot,
		    vmmaxprot, cow);
	} else {
		start = kernel ? vm_map_min(map) : *addr;
		rv = vm_map_find(map, vmobj, offset, &start, size, 0,
		    VMFS_ANY_SPACE, vmprot, vmmaxprot, cow);
	}
	if (rv != KERN_SUCCESS) {
		vm_object_deallocate(vmobj);
		return vm_mmap_to_errno(rv);
	}

	if (!shared) {
		rv = vm_map_inherit(map, start, start + size, VM_INHERIT_NONE);
		if (rv != KERN_SUCCESS) {
			vm_map_remove(map, start, start + size);
			return vm_mmap_to_errno(rv);
		}
	}

	if (wired) {
		rv = vm_map_wire(map, start, start + size,
		    (kernel ? VM_MAP_WIRE_SYSTEM : VM_MAP_WIRE_USER) |
		    VM_MAP_WIRE_NOHOLES);
		if (rv != KERN_SUCCESS) {
			vm_map_remove(map, start, start + size);
			return vm_mmap_to_errno(rv);
		}
	}

	*addr = start;
	return 0;
}

int
os_vmobj_map_user(os_vmmap_t *map, vaddr_t *addr, vsize_t size,
    os_vmobj_t *vmobj, voff_t offset, bool wired, bool fixed, bool shared,
    int prot, int maxprot)
{
	return os_vmobj_map(map, addr, size, vmobj, offset, wired, fixed,
	    shared, prot, maxprot, false);
}

int
os_vmobj_map_kern(os_vmmap_t *map, vaddr_t *addr, vsize_t size,
    os_vmobj_t *vmobj, voff_t offset, bool wired, bool fixed, bool shared,
    int prot, int maxprot)
{
	return os_vmobj_map(map, addr, size, vmobj, offset, wired, fixed,
	    shared, prot, maxprot, true);
}

void
os_vmobj_unmap(os_vmmap_t *map, vaddr_t start, vaddr_t end, bool wired)
{
	if (wired) {
		(void)vm_map_unwire(map, start, end,
		    (map == kernel_map ? VM_MAP_WIRE_SYSTEM : VM_MAP_WIRE_USER) |
		    VM_MAP_WIRE_NOHOLES);
	}
	(void)vm_map_remove(map, start, end);
}

void *
os_pagemem_zalloc(size_t size)
{
	return (void *)kmem_malloc(round_page(size), M_WAITOK | M_ZERO);
}

void
os_pagemem_free(void *ptr, size_t size)
{
	kmem_free(ptr, round_page(size));
}

paddr_t
os_pa_zalloc(void)
{
	vm_page_t m;

	m = vm_page_alloc_noobj(VM_ALLOC_WIRED | VM_ALLOC_ZERO |
	    VM_ALLOC_WAITOK);
	return VM_PAGE_TO_PHYS(m);
}

void
os_pa_free(paddr_t pa)
{
	vm_page_t m;

	m = PHYS_TO_VM_PAGE(pa);
	vm_page_unwire_noq(m);
	vm_page_free(m);
}

int
os_contigpa_zalloc(paddr_t *pa, vaddr_t *va, size_t npages)
{
	void *addr;

	addr = kmem_alloc_contig(npages * PAGE_SIZE, M_WAITOK | M_ZERO, 0,
	    ~(vm_paddr_t)0, PAGE_SIZE, 0, VM_MEMATTR_WRITE_BACK);
	if (addr == NULL)
		return ENOMEM;

	*va = (vaddr_t)addr;
	*pa = vtophys(addr);
	return 0;
}

void
os_contigpa_free(paddr_t pa __unused, vaddr_t va, size_t npages)
{
	kmem_free((void *)va, npages * PAGE_SIZE);
}

time_t
os_time(void)
{
	return time_second;
}

/* -------------------------------------------------------------------------- */

static d_open_t nvmm_dev_open;
static d_ioctl_t nvmm_dev_ioctl;
static d_mmap_single_t nvmm_dev_mmap_single;

static struct cdevsw nvmm_cdevsw = {
	.d_version = D_VERSION,
	.d_name = "nvmm",
	.d_open = nvmm_dev_open,
	.d_ioctl = nvmm_dev_ioctl,
	.d_mmap_single = nvmm_dev_mmap_single,
};

static struct cdev *nvmm_dev;

/*
 * Set once MOD_LOAD has fully succeeded. FreeBSD sends MOD_UNLOAD after a
 * failed MOD_LOAD too, and nvmm_fini() must not run on a module that was
 * never initialized.
 */
static bool nvmm_loaded;

/*
 * Suspend/resume (decision F2-3 in PORTING.md). NetBSD hooks nvmm_suspend()
 * and nvmm_resume() to pmf; on FreeBSD the power_suspend/power_resume events
 * run in thread context around an ACPI sleep, before the CPUs save their
 * context and after they are all back. vmm_resume_p is not used: mp_x86.c
 * calls it on the APs only, never on CPU 0.
 */
static eventhandler_tag nvmm_suspend_tag;
static eventhandler_tag nvmm_resume_tag;
static bool nvmm_pm_suspended;

static void
nvmm_power_suspend(void *arg __unused)
{
	nvmm_suspend();
	nvmm_pm_suspended = true;
}

static void
nvmm_power_resume(void *arg __unused)
{
	/* A resume event can come without a suspend (aborted sleep). */
	if (!nvmm_pm_suspended)
		return;
	nvmm_pm_suspended = false;
	nvmm_resume();
}

static void
nvmm_dev_dtor(void *arg)
{
	struct nvmm_owner *owner = arg;

	OS_ASSERT(owner != NULL);
	nvmm_close(owner);
	if (owner != &nvmm_root_owner)
		os_mem_free(owner, sizeof(*owner));
}

static int
nvmm_dev_open(struct cdev *dev __unused, int oflags, int devtype __unused,
    struct thread *td)
{
	struct nvmm_owner *owner;
	int error;

	if (__predict_false(nvmm_impl == NULL))
		return ENXIO;

	/*
	 * NetBSD and DragonFly refuse an open without O_CLOEXEC. FreeBSD
	 * does not pass O_CLOEXEC to d_open (it becomes a flag of the file
	 * descriptor), so the check is left to libnvmm, which always sets it.
	 */

	if ((oflags & (FREAD | FWRITE)) == FWRITE) {
		/* Opened write-only: nvmmctl(8), which sees all machines. */
		owner = &nvmm_root_owner;
	} else {
		owner = os_mem_alloc(sizeof(*owner));
		owner->pid = td->td_proc->p_pid;
	}

	error = devfs_set_cdevpriv(owner, nvmm_dev_dtor);
	if (error) {
		if (owner != &nvmm_root_owner)
			os_mem_free(owner, sizeof(*owner));
		return error;
	}

	return 0;
}

static int
nvmm_dev_ioctl(struct cdev *dev __unused, u_long cmd, caddr_t data,
    int fflag __unused, struct thread *td __unused)
{
	struct nvmm_owner *owner;
	int error;

	error = devfs_get_cdevpriv((void **)&owner);
	if (error)
		return error;

	return nvmm_ioctl(owner, cmd, data);
}

static int
nvmm_dev_mmap_single(struct cdev *dev __unused, vm_ooffset_t *offset,
    vm_size_t size, vm_object_t *objp, int nprot)
{
	struct nvmm_owner *owner;
	os_vmobj_t *vmobj;
	uint64_t objoff;
	int error;

	error = devfs_get_cdevpriv((void **)&owner);
	if (error)
		return error;

	error = nvmm_comm_object(owner, *offset, size, nprot, &vmobj, &objoff);
	if (error)
		return error;

	/* vm_mmap_cdev() consumes the reference taken by nvmm_comm_object(). */
	*objp = vmobj;
	*offset = objoff;
	return 0;
}

/* -------------------------------------------------------------------------- */

/*
 * nvmm.ko and bhyve's vmm.ko must never be active together: both put the
 * CPUs in VMX operation, and vmm.ko clears CR4.VMXE when it unloads,
 * which faults while the CPU is still in VMX operation.
 *
 * - If vmm is present (module or compiled in), nvmm refuses to load.
 * - The other way round, nvmm.ko also declares the "vmm" interface
 *   (MODULE_VERSION below), so the kernel linker refuses vmm.ko with
 *   "interface vmm.1 already present in the KLD 'nvmm.ko'" before any of
 *   its code runs, and bhyve fails to start instead of panicking the host.
 */
static bool
nvmm_vmm_present(void)
{
	bool present;

	MOD_SLOCK;
	present = module_lookupbyname("vmm") != NULL;
	MOD_SUNLOCK;

	return present;
}

static int
nvmm_load(void)
{
	struct make_dev_args args;
	int error;

	if (nvmm_vmm_present()) {
		os_printf("nvmm: vmm (bhyve) is loaded; nvmm and vmm cannot "
		    "be used together, unload vmm first\n");
		return EBUSY;
	}

	if (nvmm_ident() == NULL) {
		os_printf("nvmm: this CPU is not supported, not loading\n");
		return ENOTSUP;
	}

	error = nvmm_init();
	if (error)
		return error;

	make_dev_args_init(&args);
	args.mda_flags = MAKEDEV_CHECKNAME | MAKEDEV_WAITOK;
	args.mda_devsw = &nvmm_cdevsw;
	args.mda_uid = UID_ROOT;
	args.mda_gid = GID_WHEEL;
	args.mda_mode = 0600;
	error = make_dev_s(&args, &nvmm_dev, "nvmm");
	if (error) {
		os_printf("nvmm: unable to create /dev/nvmm (error %d)\n",
		    error);
		nvmm_fini();
		return error;
	}

	nvmm_suspend_tag = EVENTHANDLER_REGISTER(power_suspend,
	    nvmm_power_suspend, NULL, EVENTHANDLER_PRI_ANY);
	nvmm_resume_tag = EVENTHANDLER_REGISTER(power_resume,
	    nvmm_power_resume, NULL, EVENTHANDLER_PRI_ANY);

	nvmm_loaded = true;
	os_printf("nvmm: attached, using backend %s\n", nvmm_impl->name);
	return 0;
}

static int
nvmm_unload(void)
{
	if (!nvmm_loaded)
		return 0;
	if (os_atomic_load_uint(&nmachines) > 0)
		return EBUSY;

	EVENTHANDLER_DEREGISTER(power_resume, nvmm_resume_tag);
	EVENTHANDLER_DEREGISTER(power_suspend, nvmm_suspend_tag);

	if (nvmm_dev != NULL) {
		/* Waits for running methods and runs the cdevpriv dtors. */
		destroy_dev(nvmm_dev);
		nvmm_dev = NULL;
	}

	nvmm_fini();
	nvmm_loaded = false;
	return 0;
}

static int
nvmm_modevent(module_t mod __unused, int type, void *data __unused)
{
	switch (type) {
	case MOD_LOAD:
		return nvmm_load();
	case MOD_QUIESCE:
		if (os_atomic_load_uint(&nmachines) > 0)
			return EBUSY;
		return 0;
	case MOD_UNLOAD:
		return nvmm_unload();
	case MOD_SHUTDOWN:
		return 0;
	default:
		return EOPNOTSUPP;
	}
}

static moduledata_t nvmm_moddata = {
	.name = "nvmm",
	.evhand = nvmm_modevent,
	.priv = NULL,
};

DECLARE_MODULE(nvmm, nvmm_moddata, SI_SUB_DRIVERS, SI_ORDER_ANY);
MODULE_VERSION(nvmm, NVMM_KERN_VERSION);
MODULE_VERSION(vmm, 1);
