/*	$NetBSD: nvmm_x86_vmx.c,v 1.98 2026/09/07 15:58:43 christos Exp $	*/

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
 * Intel VMX backend, phase 2 of the FreeBSD port.
 *
 * Real so far, taken from NetBSD's nvmm_x86_vmx.c: the identification
 * (vmx_ident), the capability report, and VMXON/VMXOFF on every CPU at
 * load/unload and across suspend/resume. VMCS management and vcpu_run come
 * with the next phases; until then the other operations are stubs and the
 * frontend refuses the ioctls that would reach them.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/mman.h>
#include <sys/smp.h>

#include <machine/cpufunc.h>
#include <machine/md_var.h>
#include <machine/specialreg.h>
#include <x86/cputypes.h>

#include <dev/nvmm/nvmm.h>
#include <dev/nvmm/nvmm_os.h>
#include <dev/nvmm/nvmm_internal.h>
#include <dev/nvmm/x86/nvmm_x86.h>

/* FreeBSD's <machine/specialreg.h> already has these two. */
#ifndef MSR_IA32_FEATURE_CONTROL
#define MSR_IA32_FEATURE_CONTROL	0x003A
#endif
#ifndef IA32_FEATURE_CONTROL_LOCK
#define		IA32_FEATURE_CONTROL_LOCK	__BIT(0)
#endif
#define		IA32_FEATURE_CONTROL_IN_SMX	__BIT(1)
#define		IA32_FEATURE_CONTROL_OUT_SMX	__BIT(2)

#define MSR_IA32_VMX_BASIC		0x0480
#define		IA32_VMX_BASIC_IDENT		__BITS(30,0)
#define		IA32_VMX_BASIC_DATA_SIZE	__BITS(44,32)
#define		IA32_VMX_BASIC_MEM_WIDTH	__BIT(48)
#define		IA32_VMX_BASIC_DUAL		__BIT(49)
#define		IA32_VMX_BASIC_MEM_TYPE		__BITS(53,50)
#define			MEM_TYPE_UC		0
#define			MEM_TYPE_WB		6
#define		IA32_VMX_BASIC_IO_REPORT	__BIT(54)
#define		IA32_VMX_BASIC_TRUE_CTLS	__BIT(55)

#define MSR_IA32_VMX_PINBASED_CTLS		0x0481
#define MSR_IA32_VMX_PROCBASED_CTLS		0x0482
#define MSR_IA32_VMX_EXIT_CTLS			0x0483
#define MSR_IA32_VMX_ENTRY_CTLS			0x0484
#define MSR_IA32_VMX_PROCBASED_CTLS2		0x048B

#define MSR_IA32_VMX_TRUE_PINBASED_CTLS		0x048D
#define MSR_IA32_VMX_TRUE_PROCBASED_CTLS	0x048E
#define MSR_IA32_VMX_TRUE_EXIT_CTLS		0x048F
#define MSR_IA32_VMX_TRUE_ENTRY_CTLS		0x0490

#define MSR_IA32_VMX_CR0_FIXED0			0x0486
#define MSR_IA32_VMX_CR0_FIXED1			0x0487
#define MSR_IA32_VMX_CR4_FIXED0			0x0488
#define MSR_IA32_VMX_CR4_FIXED1			0x0489

#define MSR_IA32_VMX_EPT_VPID_CAP	0x048C
#define		IA32_VMX_EPT_VPID_XO			__BIT(0)
#define		IA32_VMX_EPT_VPID_WALKLENGTH_4		__BIT(6)
#define		IA32_VMX_EPT_VPID_UC			__BIT(8)
#define		IA32_VMX_EPT_VPID_WB			__BIT(14)
#define		IA32_VMX_EPT_VPID_2MB			__BIT(16)
#define		IA32_VMX_EPT_VPID_1GB			__BIT(17)
#define		IA32_VMX_EPT_VPID_INVEPT		__BIT(20)
#define		IA32_VMX_EPT_VPID_FLAGS_AD		__BIT(21)
#define		IA32_VMX_EPT_VPID_INVEPT_CONTEXT	__BIT(25)
#define		IA32_VMX_EPT_VPID_INVEPT_ALL		__BIT(26)
#define		IA32_VMX_EPT_VPID_INVVPID		__BIT(32)
#define		IA32_VMX_EPT_VPID_INVVPID_ADDR		__BIT(40)
#define		IA32_VMX_EPT_VPID_INVVPID_CONTEXT	__BIT(41)
#define		IA32_VMX_EPT_VPID_INVVPID_ALL		__BIT(42)

#define		PIN_CTLS_INT_EXITING		__BIT(0)
#define		PIN_CTLS_NMI_EXITING		__BIT(3)
#define		PIN_CTLS_VIRTUAL_NMIS		__BIT(5)

#define		PROC_CTLS_USE_TSC_OFFSETTING	__BIT(3)
#define		PROC_CTLS_HLT_EXITING		__BIT(7)
#define		PROC_CTLS_MWAIT_EXITING		__BIT(10)
#define		PROC_CTLS_RDPMC_EXITING		__BIT(11)
#define		PROC_CTLS_RCR3_EXITING		__BIT(15)
#define		PROC_CTLS_LCR3_EXITING		__BIT(16)
#define		PROC_CTLS_RCR8_EXITING		__BIT(19)
#define		PROC_CTLS_LCR8_EXITING		__BIT(20)
#define		PROC_CTLS_UNCOND_IO_EXITING	__BIT(24)
#define		PROC_CTLS_USE_MSR_BITMAPS	__BIT(28)
#define		PROC_CTLS_MONITOR_EXITING	__BIT(29)
#define		PROC_CTLS_ACTIVATE_CTLS2	__BIT(31)

#define		EXIT_CTLS_SAVE_DEBUG_CONTROLS	__BIT(2)
#define		EXIT_CTLS_HOST_LONG_MODE	__BIT(9)
#define		EXIT_CTLS_SAVE_PAT		__BIT(18)
#define		EXIT_CTLS_LOAD_PAT		__BIT(19)
#define		EXIT_CTLS_SAVE_EFER		__BIT(20)
#define		EXIT_CTLS_LOAD_EFER		__BIT(21)

#define		ENTRY_CTLS_LOAD_DEBUG_CONTROLS	__BIT(2)
#define		ENTRY_CTLS_SMM			__BIT(10)
#define		ENTRY_CTLS_DISABLE_DUAL		__BIT(11)
#define		ENTRY_CTLS_LOAD_PAT		__BIT(14)
#define		ENTRY_CTLS_LOAD_EFER		__BIT(15)

#define		PROC_CTLS2_ENABLE_EPT		__BIT(1)
#define		PROC_CTLS2_ENABLE_VPID		__BIT(5)
#define		PROC_CTLS2_UNRESTRICTED_GUEST	__BIT(7)
#define		PROC_CTLS2_INVPCID_ENABLE	__BIT(12)

#define VMX_PINBASED_CTLS_ONE	\
	(PIN_CTLS_INT_EXITING| \
	 PIN_CTLS_NMI_EXITING| \
	 PIN_CTLS_VIRTUAL_NMIS)

#define VMX_PINBASED_CTLS_ZERO	0

#define VMX_PROCBASED_CTLS_ONE	\
	(PROC_CTLS_USE_TSC_OFFSETTING| \
	 PROC_CTLS_HLT_EXITING| \
	 PROC_CTLS_MWAIT_EXITING | \
	 PROC_CTLS_RDPMC_EXITING | \
	 PROC_CTLS_RCR8_EXITING | \
	 PROC_CTLS_LCR8_EXITING | \
	 PROC_CTLS_UNCOND_IO_EXITING | /* no I/O bitmap */ \
	 PROC_CTLS_USE_MSR_BITMAPS | \
	 PROC_CTLS_MONITOR_EXITING | \
	 PROC_CTLS_ACTIVATE_CTLS2)

#define VMX_PROCBASED_CTLS_ZERO	\
	(PROC_CTLS_RCR3_EXITING| \
	 PROC_CTLS_LCR3_EXITING)

#define VMX_PROCBASED_CTLS2_ONE	\
	(PROC_CTLS2_ENABLE_EPT| \
	 PROC_CTLS2_ENABLE_VPID| \
	 PROC_CTLS2_UNRESTRICTED_GUEST)

#define VMX_PROCBASED_CTLS2_ZERO	0

#define VMX_ENTRY_CTLS_ONE	\
	(ENTRY_CTLS_LOAD_DEBUG_CONTROLS| \
	 ENTRY_CTLS_LOAD_EFER| \
	 ENTRY_CTLS_LOAD_PAT)

#define VMX_ENTRY_CTLS_ZERO	\
	(ENTRY_CTLS_SMM| \
	 ENTRY_CTLS_DISABLE_DUAL)

#define VMX_EXIT_CTLS_ONE	\
	(EXIT_CTLS_SAVE_DEBUG_CONTROLS| \
	 EXIT_CTLS_HOST_LONG_MODE| \
	 EXIT_CTLS_SAVE_PAT| \
	 EXIT_CTLS_LOAD_PAT| \
	 EXIT_CTLS_SAVE_EFER| \
	 EXIT_CTLS_LOAD_EFER)

#define VMX_EXIT_CTLS_ZERO	0

#define VMX_NCPUIDS	32

static uint64_t vmx_pinbased_ctls __read_mostly;
static uint64_t vmx_procbased_ctls __read_mostly;
static uint64_t vmx_procbased_ctls2 __read_mostly;
static uint64_t vmx_entry_ctls __read_mostly;
static uint64_t vmx_exit_ctls __read_mostly;

static uint64_t vmx_cr0_fixed0 __read_mostly;
static uint64_t vmx_cr0_fixed1 __read_mostly;
static uint64_t vmx_cr4_fixed0 __read_mostly;
static uint64_t vmx_cr4_fixed1 __read_mostly;

static bool vmx_ept_has_ad __read_mostly;
static uint64_t vmx_xcr0_mask __read_mostly;

static const size_t vmx_vcpu_conf_sizes[NVMM_X86_VCPU_NCONF] = {
	[NVMM_VCPU_CONF_MD(NVMM_VCPU_CONF_CPUID)] =
	    sizeof(struct nvmm_vcpu_conf_cpuid),
	[NVMM_VCPU_CONF_MD(NVMM_VCPU_CONF_TPR)] =
	    sizeof(struct nvmm_vcpu_conf_tpr),
	[NVMM_VCPU_CONF_MD(NVMM_VCPU_CONF_XCR0_MASK)] =
	    sizeof(uint64_t),
};

/* -------------------------------------------------------------------------- */

/*
 * Identification runs at load time, before we know the CPU is what we
 * expect; on FreeBSD an MSR read that faults is a panic, so every read here
 * goes through rdmsr_safe() and a fault means "not supported".
 */
static bool
vmx_rdmsr(u_int msr, uint64_t *val)
{
	if (rdmsr_safe(msr, val) != 0) {
		os_printf("nvmm: cannot read MSR 0x%x\n", msr);
		return false;
	}
	return true;
}

static inline int
vmx_check_cr(uint64_t crval, uint64_t fixed0, uint64_t fixed1)
{
	/* Bits set to 1 in fixed0 are fixed to 1. */
	if ((crval & fixed0) != fixed0) {
		return -1;
	}
	/* Bits set to 0 in fixed1 are fixed to 0. */
	if (crval & ~fixed1) {
		return -1;
	}
	return 0;
}

#define CTLS_ONE_ALLOWED(msrval, bitoff) \
	((msrval & __BIT(32 + bitoff)) != 0)
#define CTLS_ZERO_ALLOWED(msrval, bitoff) \
	((msrval & __BIT(bitoff)) == 0)

static int
vmx_check_ctls(uint64_t msr_ctls, uint64_t msr_true_ctls, uint64_t set_one)
{
	uint64_t basic, val, true_val;
	bool has_true;
	size_t i;

	if (!vmx_rdmsr(MSR_IA32_VMX_BASIC, &basic))
		return -1;
	has_true = (basic & IA32_VMX_BASIC_TRUE_CTLS) != 0;

	if (!vmx_rdmsr(msr_ctls, &val))
		return -1;
	if (has_true) {
		if (!vmx_rdmsr(msr_true_ctls, &true_val))
			return -1;
	} else {
		true_val = val;
	}

	for (i = 0; i < 32; i++) {
		if (!(set_one & __BIT(i))) {
			continue;
		}
		if (!CTLS_ONE_ALLOWED(true_val, i)) {
			return -1;
		}
	}

	return 0;
}

static int
vmx_init_ctls(uint64_t msr_ctls, uint64_t msr_true_ctls,
    uint64_t set_one, uint64_t set_zero, uint64_t *res)
{
	uint64_t basic, val, true_val;
	bool one_allowed, zero_allowed, has_true;
	size_t i;

	if (!vmx_rdmsr(MSR_IA32_VMX_BASIC, &basic))
		return -1;
	has_true = (basic & IA32_VMX_BASIC_TRUE_CTLS) != 0;

	if (!vmx_rdmsr(msr_ctls, &val))
		return -1;
	if (has_true) {
		if (!vmx_rdmsr(msr_true_ctls, &true_val))
			return -1;
	} else {
		true_val = val;
	}

	for (i = 0; i < 32; i++) {
		one_allowed = CTLS_ONE_ALLOWED(true_val, i);
		zero_allowed = CTLS_ZERO_ALLOWED(true_val, i);

		if (zero_allowed && !one_allowed) {
			if (set_one & __BIT(i))
				return -1;
			*res &= ~__BIT(i);
		} else if (one_allowed && !zero_allowed) {
			if (set_zero & __BIT(i))
				return -1;
			*res |= __BIT(i);
		} else {
			if (set_zero & __BIT(i)) {
				*res &= ~__BIT(i);
			} else if (set_one & __BIT(i)) {
				*res |= __BIT(i);
			} else if (!has_true) {
				*res &= ~__BIT(i);
			} else if (CTLS_ZERO_ALLOWED(val, i)) {
				*res &= ~__BIT(i);
			} else if (CTLS_ONE_ALLOWED(val, i)) {
				*res |= __BIT(i);
			} else {
				return -1;
			}
		}
	}

	return 0;
}

/*
 * Explicit checks for the three features this port requires, so that a
 * refusal names the missing one instead of a generic "proc-based-ctls2
 * requirements not satisfied". FreeBSD addition.
 */
static bool
vmx_ident_required_features(void)
{
	uint64_t procbased, procbased2;

	if (!vmx_rdmsr(MSR_IA32_VMX_PROCBASED_CTLS, &procbased))
		return false;
	if (!CTLS_ONE_ALLOWED(procbased, 31)) {
		os_printf("nvmm: VMX secondary controls not supported "
		    "(no EPT, no unrestricted guest)\n");
		return false;
	}
	if (!vmx_rdmsr(MSR_IA32_VMX_PROCBASED_CTLS2, &procbased2))
		return false;
	if (!CTLS_ONE_ALLOWED(procbased2, 1)) {
		os_printf("nvmm: EPT not supported by this CPU\n");
		return false;
	}
	if (!CTLS_ONE_ALLOWED(procbased2, 7)) {
		os_printf("nvmm: VMX unrestricted guest not supported by "
		    "this CPU\n");
		return false;
	}
	if (!CTLS_ONE_ALLOWED(procbased2, 5)) {
		os_printf("nvmm: VPID not supported by this CPU\n");
		return false;
	}
	return true;
}

static bool
vmx_ident(void)
{
	uint64_t msr;
	int ret;

	if (cpu_vendor_id != CPU_VENDOR_INTEL) {
		os_printf("nvmm: not an Intel CPU; only the VMX backend is "
		    "ported so far\n");
		return false;
	}
	if (!(cpu_feature2 & CPUID2_VMX)) {
		os_printf("nvmm: VMX not supported by this CPU\n");
		return false;
	}

	if (!vmx_rdmsr(MSR_IA32_FEATURE_CONTROL, &msr))
		return false;
	if ((msr & IA32_FEATURE_CONTROL_LOCK) != 0 &&
	    (msr & IA32_FEATURE_CONTROL_OUT_SMX) == 0) {
		os_printf("nvmm: VMX disabled in the firmware (BIOS/UEFI "
		    "setup)\n");
		return false;
	}

	if (!vmx_rdmsr(MSR_IA32_VMX_BASIC, &msr))
		return false;
	if ((msr & IA32_VMX_BASIC_IO_REPORT) == 0) {
		os_printf("nvmm: I/O reporting not supported\n");
		return false;
	}
	if (__SHIFTOUT(msr, IA32_VMX_BASIC_MEM_TYPE) != MEM_TYPE_WB) {
		os_printf("nvmm: WB memory not supported\n");
		return false;
	}

	if (!vmx_ident_required_features())
		return false;

	/* PG and PE are reported, even if Unrestricted Guests is supported. */
	if (!vmx_rdmsr(MSR_IA32_VMX_CR0_FIXED0, &vmx_cr0_fixed0) ||
	    !vmx_rdmsr(MSR_IA32_VMX_CR0_FIXED1, &vmx_cr0_fixed1))
		return false;
	vmx_cr0_fixed0 &= ~(CR0_PG|CR0_PE);
	vmx_cr0_fixed1 |= (CR0_PG|CR0_PE);
	ret = vmx_check_cr(rcr0(), vmx_cr0_fixed0, vmx_cr0_fixed1);
	if (ret == -1) {
		os_printf("nvmm: CR0 requirements not satisfied\n");
		return false;
	}

	if (!vmx_rdmsr(MSR_IA32_VMX_CR4_FIXED0, &vmx_cr4_fixed0) ||
	    !vmx_rdmsr(MSR_IA32_VMX_CR4_FIXED1, &vmx_cr4_fixed1))
		return false;
	ret = vmx_check_cr(rcr4() | CR4_VMXE, vmx_cr4_fixed0, vmx_cr4_fixed1);
	if (ret == -1) {
		os_printf("nvmm: CR4 requirements not satisfied\n");
		return false;
	}

	/* Init the CTLSs right now, and check for errors. */
	ret = vmx_init_ctls(
	    MSR_IA32_VMX_PINBASED_CTLS, MSR_IA32_VMX_TRUE_PINBASED_CTLS,
	    VMX_PINBASED_CTLS_ONE, VMX_PINBASED_CTLS_ZERO,
	    &vmx_pinbased_ctls);
	if (ret == -1) {
		os_printf("nvmm: pin-based-ctls requirements not satisfied\n");
		return false;
	}
	ret = vmx_init_ctls(
	    MSR_IA32_VMX_PROCBASED_CTLS, MSR_IA32_VMX_TRUE_PROCBASED_CTLS,
	    VMX_PROCBASED_CTLS_ONE, VMX_PROCBASED_CTLS_ZERO,
	    &vmx_procbased_ctls);
	if (ret == -1) {
		os_printf("nvmm: proc-based-ctls requirements not satisfied\n");
		return false;
	}
	ret = vmx_init_ctls(
	    MSR_IA32_VMX_PROCBASED_CTLS2, MSR_IA32_VMX_PROCBASED_CTLS2,
	    VMX_PROCBASED_CTLS2_ONE, VMX_PROCBASED_CTLS2_ZERO,
	    &vmx_procbased_ctls2);
	if (ret == -1) {
		os_printf("nvmm: proc-based-ctls2 requirements not "
		    "satisfied\n");
		return false;
	}
	ret = vmx_check_ctls(
	    MSR_IA32_VMX_PROCBASED_CTLS2, MSR_IA32_VMX_PROCBASED_CTLS2,
	    PROC_CTLS2_INVPCID_ENABLE);
	if (ret != -1) {
		vmx_procbased_ctls2 |= PROC_CTLS2_INVPCID_ENABLE;
	}
	ret = vmx_init_ctls(
	    MSR_IA32_VMX_ENTRY_CTLS, MSR_IA32_VMX_TRUE_ENTRY_CTLS,
	    VMX_ENTRY_CTLS_ONE, VMX_ENTRY_CTLS_ZERO,
	    &vmx_entry_ctls);
	if (ret == -1) {
		os_printf("nvmm: entry-ctls requirements not satisfied\n");
		return false;
	}
	ret = vmx_init_ctls(
	    MSR_IA32_VMX_EXIT_CTLS, MSR_IA32_VMX_TRUE_EXIT_CTLS,
	    VMX_EXIT_CTLS_ONE, VMX_EXIT_CTLS_ZERO,
	    &vmx_exit_ctls);
	if (ret == -1) {
		os_printf("nvmm: exit-ctls requirements not satisfied\n");
		return false;
	}

	if (!vmx_rdmsr(MSR_IA32_VMX_EPT_VPID_CAP, &msr))
		return false;
	if ((msr & IA32_VMX_EPT_VPID_WALKLENGTH_4) == 0) {
		os_printf("nvmm: 4-level page tree not supported\n");
		return false;
	}
	if ((msr & IA32_VMX_EPT_VPID_INVEPT) == 0) {
		os_printf("nvmm: INVEPT not supported\n");
		return false;
	}
	if ((msr & IA32_VMX_EPT_VPID_INVVPID) == 0) {
		os_printf("nvmm: INVVPID not supported\n");
		return false;
	}
	/*
	 * NetBSD hands this to its pmap (pmap_ept_has_ad). On FreeBSD it
	 * will select PMAP_EMULATE_AD_BITS for the EPT pmap (phase 3): Ivy
	 * Bridge has no EPT A/D bits, Haswell has them.
	 */
	vmx_ept_has_ad = (msr & IA32_VMX_EPT_VPID_FLAGS_AD) != 0;
	if (!(msr & IA32_VMX_EPT_VPID_UC) && !(msr & IA32_VMX_EPT_VPID_WB)) {
		os_printf("nvmm: EPT UC/WB memory types not supported\n");
		return false;
	}

	return true;
}

/* -------------------------------------------------------------------------- */

/*
 * VMXON regions, one page per CPU, indexed by FreeBSD cpuid. NetBSD panics
 * when VMXON fails; here each CPU records its outcome in 'on' and 'error',
 * and the CPU that called the rendezvous turns a failure into a refused
 * load (decision F2-1 in PORTING.md).
 */
struct vmxon {
	uint32_t ident;
#define VMXON_IDENT_REVISION	__BITS(30,0)

	uint8_t data[PAGE_SIZE - 4];
} __packed;

CTASSERT(sizeof(struct vmxon) == PAGE_SIZE);

#define VMXCPU_OK		0
#define VMXCPU_VMXE_IN_USE	1	/* CR4.VMXE already set */
#define VMXCPU_FIRMWARE		2	/* feature control locked, VMX off */
#define VMXCPU_VMXON_FAILED	3
#define VMXCPU_VMXOFF_FAILED	4

struct vmxoncpu {
	struct vmxon *va;
	vm_paddr_t pa;
	bool on;
	int error;
};

static struct vmxoncpu vmxoncpu[MAXCPU];
static uint32_t vmx_revision __read_mostly;

/*
 * NetBSD has these in nvmm_x86_vmxfunc.S and panics on failure; inline here
 * until vmlaunch/vmresume bring the assembly file (phase 5). VMfailInvalid
 * sets CF, VMfailValid sets ZF: "setna" catches both.
 */
static inline bool
vmx_vmxon(vm_paddr_t *pa)
{
	bool fail;

	__asm volatile("vmxon %1\n\tsetna %0"
	    : "=q" (fail) : "m" (*pa) : "cc", "memory");
	return !fail;
}

static inline bool
vmx_vmxoff(void)
{
	bool fail;

	__asm volatile("vmxoff\n\tsetna %0"
	    : "=q" (fail) : : "cc", "memory");
	return !fail;
}

/* Rendezvous action: is another hypervisor already in VMX operation? */
static void
vmx_probe_cpu(void *arg __unused)
{
	struct vmxoncpu *vc = &vmxoncpu[curcpu];

	vc->error = (rcr4() & CR4_VMXE) ? VMXCPU_VMXE_IN_USE : VMXCPU_OK;
}

/* Rendezvous action, from NetBSD's vmx_change_cpu(enable = true). */
static void
vmx_enable_cpu(void *arg __unused)
{
	struct vmxoncpu *vc = &vmxoncpu[curcpu];
	uint64_t msr;

	vc->error = VMXCPU_OK;
	if (vc->on)
		return;

	msr = rdmsr(MSR_IA32_FEATURE_CONTROL);
	if ((msr & IA32_FEATURE_CONTROL_LOCK) == 0) {
		/* Lock now, with VMX-outside-SMX enabled. */
		wrmsr(MSR_IA32_FEATURE_CONTROL, msr |
		    IA32_FEATURE_CONTROL_LOCK |
		    IA32_FEATURE_CONTROL_OUT_SMX);
	} else if ((msr & IA32_FEATURE_CONTROL_OUT_SMX) == 0) {
		/* vmx_ident checked the boot CPU only. */
		vc->error = VMXCPU_FIRMWARE;
		return;
	}

	load_cr4(rcr4() | CR4_VMXE);
	vc->va->ident = __SHIFTIN(vmx_revision, VMXON_IDENT_REVISION);
	if (!vmx_vmxon(&vc->pa)) {
		load_cr4(rcr4() & ~CR4_VMXE);
		vc->error = VMXCPU_VMXON_FAILED;
		return;
	}
	vc->on = true;
}

/* Rendezvous action, from NetBSD's vmx_change_cpu(enable = false). */
static void
vmx_disable_cpu(void *arg __unused)
{
	struct vmxoncpu *vc = &vmxoncpu[curcpu];

	vc->error = VMXCPU_OK;
	if (!vc->on)
		return;

	if (!vmx_vmxoff()) {
		/*
		 * Still in VMX operation: clearing CR4.VMXE now would #GP.
		 * Leave the CPU as it is and let the caller report it.
		 */
		vc->error = VMXCPU_VMXOFF_FAILED;
		return;
	}
	load_cr4(rcr4() & ~CR4_VMXE);
	vc->on = false;
}

static const char *
vmx_cpu_strerror(int error)
{
	switch (error) {
	case VMXCPU_VMXE_IN_USE:
		return "VMX already in use by another hypervisor (CR4.VMXE set)";
	case VMXCPU_FIRMWARE:
		return "VMX disabled in the firmware";
	case VMXCPU_VMXON_FAILED:
		return "VMXON failed";
	case VMXCPU_VMXOFF_FAILED:
		return "VMXOFF failed";
	default:
		return "unknown error";
	}
}

/* Prints every CPU that failed the last rendezvous; returns their number. */
static u_int
vmx_report_cpus(void)
{
	u_int cpu, nfailed = 0;

	CPU_FOREACH(cpu) {
		if (vmxoncpu[cpu].error == VMXCPU_OK)
			continue;
		os_printf("nvmm: CPU %u: %s\n", cpu,
		    vmx_cpu_strerror(vmxoncpu[cpu].error));
		nfailed++;
	}
	return nfailed;
}

/* VMXOFF everywhere; a CPU that cannot leave VMX keeps its region. */
static void
vmx_disable_all(void)
{
	smp_rendezvous(NULL, vmx_disable_cpu, NULL, NULL);
	(void)vmx_report_cpus();
}

/* VMXON everywhere, or nowhere. */
static int
vmx_enable_all(void)
{
	smp_rendezvous(NULL, vmx_enable_cpu, NULL, NULL);
	if (vmx_report_cpus() == 0)
		return 0;

	vmx_disable_all();
	return ENXIO;
}

static void
vmx_free_regions(void)
{
	u_int cpu;

	CPU_FOREACH(cpu) {
		struct vmxoncpu *vc = &vmxoncpu[cpu];

		if (vc->va == NULL || vc->on)
			continue;
		os_pagemem_free(vc->va, PAGE_SIZE);
		vc->va = NULL;
		vc->pa = 0;
	}
}

static int
vmx_init(void)
{
	uint64_t msr;
	u_int cpu;
	int error;

	/*
	 * NetBSD uses x86_xsave_features, the XCR0 bits the CPU supports.
	 * FreeBSD's xsave_mask is the subset the kernel enabled in XCR0 and
	 * sized its save areas for, which is what a guest may be given.
	 * Bit 63 is masked off as in NetBSD (reserved, not understood).
	 */
	vmx_xcr0_mask = use_xsave ? (xsave_mask & __BITS(62, 0)) : 0;

	if (bootverbose) {
		os_printf("nvmm: VMX EPT A/D bits %s, XCR0 mask 0x%jx\n",
		    vmx_ept_has_ad ? "present" : "absent (emulated)",
		    (uintmax_t)vmx_xcr0_mask);
	}

	/*
	 * Refuse to share VMX with another hypervisor: our VMXON would fail
	 * and our VMXOFF would pull VMX from under it (decision F2-2).
	 */
	smp_rendezvous(NULL, vmx_probe_cpu, NULL, NULL);
	if (vmx_report_cpus() != 0) {
		os_printf("nvmm: not loading\n");
		return EBUSY;
	}

	if (!vmx_rdmsr(MSR_IA32_VMX_BASIC, &msr))
		return ENXIO;
	vmx_revision = __SHIFTOUT(msr, IA32_VMX_BASIC_IDENT);

	memset(vmxoncpu, 0, sizeof(vmxoncpu));
	CPU_FOREACH(cpu) {
		/* kmem pages are page-aligned; one page is contiguous. */
		vmxoncpu[cpu].va = os_pagemem_zalloc(PAGE_SIZE);
		vmxoncpu[cpu].pa = vtophys(vmxoncpu[cpu].va);
	}

	error = vmx_enable_all();
	if (error) {
		os_printf("nvmm: VMXON refused, not loading\n");
		vmx_free_regions();
		return error;
	}

	if (bootverbose)
		os_printf("nvmm: VMXON on %d CPUs\n", mp_ncpus);
	return 0;
}

static void
vmx_fini(void)
{
	vmx_disable_all();
	vmx_free_regions();
}

static void
vmx_suspend(void)
{
	vmx_disable_all();
}

static void
vmx_resume(void)
{
	if (vmx_enable_all() != 0)
		os_printf("nvmm: VMXON failed after resume\n");
}

static void
vmx_capability(struct nvmm_capability *cap)
{
	cap->arch.mach_conf_support = 0;
	cap->arch.vcpu_conf_support =
	    NVMM_CAP_ARCH_VCPU_CONF_CPUID |
	    NVMM_CAP_ARCH_VCPU_CONF_TPR;
	cap->arch.xcr0_mask = vmx_xcr0_mask;
	cap->arch.mxcsr_mask = cpu_mxcsr_mask;
	cap->arch.conf_cpuid_maxops = VMX_NCPUIDS;
}

/* -------------------------------------------------------------------------- */

/*
 * Stubs. The frontend answers ENOSYS before reaching any of these in
 * phase 1; they exist so that the nvmm_impl table is complete.
 */

static void
vmx_machine_create(struct nvmm_machine *mach __unused)
{
}

static void
vmx_machine_destroy(struct nvmm_machine *mach __unused)
{
}

static int
vmx_machine_configure(struct nvmm_machine *mach __unused, uint64_t op __unused,
    void *data __unused)
{
	return ENOSYS;
}

static int
vmx_vcpu_create(struct nvmm_machine *mach __unused,
    struct nvmm_cpu *vcpu __unused)
{
	return ENOSYS;
}

static void
vmx_vcpu_destroy(struct nvmm_machine *mach __unused,
    struct nvmm_cpu *vcpu __unused)
{
}

static int
vmx_vcpu_configure(struct nvmm_cpu *vcpu __unused, uint64_t op __unused,
    void *data __unused)
{
	return ENOSYS;
}

static void
vmx_vcpu_setstate(struct nvmm_cpu *vcpu __unused)
{
}

static void
vmx_vcpu_getstate(struct nvmm_cpu *vcpu __unused)
{
}

static int
vmx_vcpu_inject(struct nvmm_cpu *vcpu __unused)
{
	return ENOSYS;
}

static int
vmx_vcpu_run(struct nvmm_machine *mach __unused,
    struct nvmm_cpu *vcpu __unused, struct nvmm_vcpu_exit *exit __unused)
{
	return ENOSYS;
}

const struct nvmm_impl nvmm_x86_vmx = {
	.name = "x86-vmx",
	.ident = vmx_ident,
	.init = vmx_init,
	.fini = vmx_fini,
	.suspend_interrupt = NULL,
	.suspend = vmx_suspend,
	.resume = vmx_resume,
	.capability = vmx_capability,
	.mach_conf_max = NVMM_X86_MACH_NCONF,
	.mach_conf_sizes = NULL,
	.vcpu_conf_max = NVMM_X86_VCPU_NCONF,
	.vcpu_conf_sizes = vmx_vcpu_conf_sizes,
	.state_size = sizeof(struct nvmm_x64_state),
	.machine_create = vmx_machine_create,
	.machine_destroy = vmx_machine_destroy,
	.machine_configure = vmx_machine_configure,
	.vcpu_create = vmx_vcpu_create,
	.vcpu_destroy = vmx_vcpu_destroy,
	.vcpu_configure = vmx_vcpu_configure,
	.vcpu_setstate = vmx_vcpu_setstate,
	.vcpu_getstate = vmx_vcpu_getstate,
	.vcpu_inject = vmx_vcpu_inject,
	.vcpu_run = vmx_vcpu_run,
	.vcpu_suspend = NULL,
	.vcpu_resume = NULL,
};
