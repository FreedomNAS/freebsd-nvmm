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
 * Intel VMX backend, phase 3 of the FreeBSD port.
 *
 * Real so far, taken from NetBSD's nvmm_x86_vmx.c: identification,
 * capability, VMXON/VMXOFF on every CPU (load/unload, suspend/resume),
 * machines and vCPUs (VMCS, VPID, MSR bitmap, configuration, state,
 * event injection). vcpu_run and the exit handlers come with phase 5;
 * until then the frontend refuses the ioctls that would reach them.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mman.h>
#include <sys/mutex.h>
#include <sys/smp.h>
#include <sys/smr.h>

#include <machine/cpufunc.h>
#include <machine/fpu.h>
#include <machine/md_var.h>
#include <machine/psl.h>
#include <machine/segments.h>
#include <machine/specialreg.h>
#include <x86/cputypes.h>

#include <dev/nvmm/nvmm.h>
#include <dev/nvmm/nvmm_os.h>
#include <dev/nvmm/nvmm_internal.h>
#include <dev/nvmm/x86/nvmm_x86.h>
#include <dev/nvmm/x86/nvmm_x86_freebsd.h>

/*
 * The VMX instruction wrappers jump here when an instruction fails. On
 * NetBSD these two labels live in nvmm_x86_vmxfunc.S; until that file is
 * built (phase 5, with vmlaunch/vmresume) they are C functions. A failing
 * VMREAD/VMWRITE/VMPTRLD/VMCLEAR/INVEPT/INVVPID is a bug in this code.
 */
/* nvmm_x86_vmxfunc.S */
int vmx_vmlaunch(uint64_t *gprs);
int vmx_vmresume(uint64_t *gprs);
void vmx_resume_rip(void);

void vmx_insn_failvalid(void) __dead2;
void vmx_insn_failinvalid(void) __dead2;

void
vmx_insn_failvalid(void)
{
	panic("nvmm: VMX instruction failed (VMfailValid)");
}

void
vmx_insn_failinvalid(void)
{
	panic("nvmm: VMX instruction failed (VMfailInvalid)");
}


struct ept_desc {
	uint64_t eptp;
	uint64_t mbz;
} __packed;

struct vpid_desc {
	uint64_t vpid;
	uint64_t addr;
} __packed;

static inline void
vmx_invept(uint64_t op, struct ept_desc *desc)
{
	__asm volatile (
		"invept		%[desc],%[op];"
		"jz		vmx_insn_failvalid;"
		"jc		vmx_insn_failinvalid;"
		:
		: [desc] "m" (*desc), [op] "r" (op)
		: "memory", "cc"
	);
}

static inline void
vmx_invvpid(uint64_t op, struct vpid_desc *desc)
{
	__asm volatile (
		"invvpid	%[desc],%[op];"
		"jz		vmx_insn_failvalid;"
		"jc		vmx_insn_failinvalid;"
		:
		: [desc] "m" (*desc), [op] "r" (op)
		: "memory", "cc"
	);
}

static inline void
vmx_cli(void)
{
	__asm volatile ("cli" ::: "memory");
}

static inline void
vmx_sti(void)
{
	__asm volatile ("sti" ::: "memory");
}

static inline uint64_t
vmx_vmread(uint64_t field)
{
	uint64_t value;

	__asm volatile (
		"vmread		%[field],%[value];"
		"jz		vmx_insn_failvalid;"
		"jc		vmx_insn_failinvalid;"
		: [value] "=r" (value)
		: [field] "r" (field)
		: "cc"
	);

	return value;
}

static inline void
vmx_vmwrite(uint64_t field, uint64_t value)
{
	__asm volatile (
		"vmwrite	%[value],%[field];"
		"jz		vmx_insn_failvalid;"
		"jc		vmx_insn_failinvalid;"
		:
		: [field] "r" (field), [value] "r" (value)
		: "cc"
	);
}

static inline paddr_t __diagused
vmx_vmptrst(void)
{
	paddr_t pa;

	__asm volatile (
		"vmptrst	%[pa];"
		:
		: [pa] "m" (*(paddr_t *)&pa)
		: "memory"
	);

	return pa;
}

static inline void
vmx_vmptrld(paddr_t *pa)
{
	__asm volatile (
		"vmptrld	%[pa];"
		"jz		vmx_insn_failvalid;"
		"jc		vmx_insn_failinvalid;"
		:
		: [pa] "m" (*pa)
		: "memory", "cc"
	);
}

static inline void
vmx_vmclear(paddr_t *pa)
{
	__asm volatile (
		"vmclear	%[pa];"
		"jz		vmx_insn_failvalid;"
		"jc		vmx_insn_failinvalid;"
		:
		: [pa] "m" (*pa)
		: "memory", "cc"
	);
}

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
#define		IA32_VMX_EPT_VPID_ADVANCED_VMEXIT_INFO	__BIT(22)
#define		IA32_VMX_EPT_VPID_SHSTK			__BIT(23)
#define		IA32_VMX_EPT_VPID_INVEPT_CONTEXT	__BIT(25)
#define		IA32_VMX_EPT_VPID_INVEPT_ALL		__BIT(26)
#define		IA32_VMX_EPT_VPID_INVVPID		__BIT(32)
#define		IA32_VMX_EPT_VPID_INVVPID_ADDR		__BIT(40)
#define		IA32_VMX_EPT_VPID_INVVPID_CONTEXT	__BIT(41)
#define		IA32_VMX_EPT_VPID_INVVPID_ALL		__BIT(42)
#define		IA32_VMX_EPT_VPID_INVVPID_CONTEXT_NOG	__BIT(43)

/* -------------------------------------------------------------------------- */

/* 16-bit control fields */
#define VMCS_VPID				0x00000000
#define VMCS_PIR_VECTOR				0x00000002
#define VMCS_EPTP_INDEX				0x00000004
/* 16-bit guest-state fields */
#define VMCS_GUEST_ES_SELECTOR			0x00000800
#define VMCS_GUEST_CS_SELECTOR			0x00000802
#define VMCS_GUEST_SS_SELECTOR			0x00000804
#define VMCS_GUEST_DS_SELECTOR			0x00000806
#define VMCS_GUEST_FS_SELECTOR			0x00000808
#define VMCS_GUEST_GS_SELECTOR			0x0000080A
#define VMCS_GUEST_LDTR_SELECTOR		0x0000080C
#define VMCS_GUEST_TR_SELECTOR			0x0000080E
#define VMCS_GUEST_INTR_STATUS			0x00000810
#define VMCS_PML_INDEX				0x00000812
/* 16-bit host-state fields */
#define VMCS_HOST_ES_SELECTOR			0x00000C00
#define VMCS_HOST_CS_SELECTOR			0x00000C02
#define VMCS_HOST_SS_SELECTOR			0x00000C04
#define VMCS_HOST_DS_SELECTOR			0x00000C06
#define VMCS_HOST_FS_SELECTOR			0x00000C08
#define VMCS_HOST_GS_SELECTOR			0x00000C0A
#define VMCS_HOST_TR_SELECTOR			0x00000C0C
/* 64-bit control fields */
#define VMCS_IO_BITMAP_A			0x00002000
#define VMCS_IO_BITMAP_B			0x00002002
#define VMCS_MSR_BITMAP				0x00002004
#define VMCS_EXIT_MSR_STORE_ADDRESS		0x00002006
#define VMCS_EXIT_MSR_LOAD_ADDRESS		0x00002008
#define VMCS_ENTRY_MSR_LOAD_ADDRESS		0x0000200A
#define VMCS_EXECUTIVE_VMCS			0x0000200C
#define VMCS_PML_ADDRESS			0x0000200E
#define VMCS_TSC_OFFSET				0x00002010
#define VMCS_VIRTUAL_APIC			0x00002012
#define VMCS_APIC_ACCESS			0x00002014
#define VMCS_PIR_DESC				0x00002016
#define VMCS_VM_CONTROL				0x00002018
#define VMCS_EPTP				0x0000201A
#define		EPTP_TYPE			__BITS(2,0)
#define			EPTP_TYPE_UC		0
#define			EPTP_TYPE_WB		6
#define		EPTP_WALKLEN			__BITS(5,3)
#define		EPTP_FLAGS_AD			__BIT(6)
#define		EPTP_SSS			__BIT(7)
#define		EPTP_PHYSADDR			__BITS(63,12)
#define VMCS_EOI_EXIT0				0x0000201C
#define VMCS_EOI_EXIT1				0x0000201E
#define VMCS_EOI_EXIT2				0x00002020
#define VMCS_EOI_EXIT3				0x00002022
#define VMCS_EPTP_LIST				0x00002024
#define VMCS_VMREAD_BITMAP			0x00002026
#define VMCS_VMWRITE_BITMAP			0x00002028
#define VMCS_VIRTUAL_EXCEPTION			0x0000202A
#define VMCS_XSS_EXIT_BITMAP			0x0000202C
#define VMCS_ENCLS_EXIT_BITMAP			0x0000202E
#define VMCS_SUBPAGE_PERM_TABLE_PTR		0x00002030
#define VMCS_TSC_MULTIPLIER			0x00002032
#define VMCS_ENCLV_EXIT_BITMAP			0x00002036
/* 64-bit read-only fields */
#define VMCS_GUEST_PHYSICAL_ADDRESS		0x00002400
/* 64-bit guest-state fields */
#define VMCS_LINK_POINTER			0x00002800
#define VMCS_GUEST_IA32_DEBUGCTL		0x00002802
#define VMCS_GUEST_IA32_PAT			0x00002804
#define VMCS_GUEST_IA32_EFER			0x00002806
#define VMCS_GUEST_IA32_PERF_GLOBAL_CTRL	0x00002808
#define VMCS_GUEST_PDPTE0			0x0000280A
#define VMCS_GUEST_PDPTE1			0x0000280C
#define VMCS_GUEST_PDPTE2			0x0000280E
#define VMCS_GUEST_PDPTE3			0x00002810
#define VMCS_GUEST_BNDCFGS			0x00002812
#define VMCS_GUEST_RTIT_CTL			0x00002814
#define VMCS_GUEST_PKRS				0x00002818
/* 64-bit host-state fields */
#define VMCS_HOST_IA32_PAT			0x00002C00
#define VMCS_HOST_IA32_EFER			0x00002C02
#define VMCS_HOST_IA32_PERF_GLOBAL_CTRL		0x00002C04
#define VMCS_HOST_IA32_PKRS			0x00002C06
/* 32-bit control fields */
#define VMCS_PINBASED_CTLS			0x00004000
#define		PIN_CTLS_INT_EXITING		__BIT(0)
#define		PIN_CTLS_NMI_EXITING		__BIT(3)
#define		PIN_CTLS_VIRTUAL_NMIS		__BIT(5)
#define		PIN_CTLS_ACTIVATE_PREEMPT_TIMER	__BIT(6)
#define		PIN_CTLS_PROCESS_POSTED_INTS	__BIT(7)
#define VMCS_PROCBASED_CTLS			0x00004002
#define		PROC_CTLS_INT_WINDOW_EXITING	__BIT(2)
#define		PROC_CTLS_USE_TSC_OFFSETTING	__BIT(3)
#define		PROC_CTLS_HLT_EXITING		__BIT(7)
#define		PROC_CTLS_INVLPG_EXITING	__BIT(9)
#define		PROC_CTLS_MWAIT_EXITING		__BIT(10)
#define		PROC_CTLS_RDPMC_EXITING		__BIT(11)
#define		PROC_CTLS_RDTSC_EXITING		__BIT(12)
#define		PROC_CTLS_RCR3_EXITING		__BIT(15)
#define		PROC_CTLS_LCR3_EXITING		__BIT(16)
#define		PROC_CTLS_RCR8_EXITING		__BIT(19)
#define		PROC_CTLS_LCR8_EXITING		__BIT(20)
#define		PROC_CTLS_USE_TPR_SHADOW	__BIT(21)
#define		PROC_CTLS_NMI_WINDOW_EXITING	__BIT(22)
#define		PROC_CTLS_DR_EXITING		__BIT(23)
#define		PROC_CTLS_UNCOND_IO_EXITING	__BIT(24)
#define		PROC_CTLS_USE_IO_BITMAPS	__BIT(25)
#define		PROC_CTLS_MONITOR_TRAP_FLAG	__BIT(27)
#define		PROC_CTLS_USE_MSR_BITMAPS	__BIT(28)
#define		PROC_CTLS_MONITOR_EXITING	__BIT(29)
#define		PROC_CTLS_PAUSE_EXITING		__BIT(30)
#define		PROC_CTLS_ACTIVATE_CTLS2	__BIT(31)
#define VMCS_EXCEPTION_BITMAP			0x00004004
#define VMCS_PF_ERROR_MASK			0x00004006
#define VMCS_PF_ERROR_MATCH			0x00004008
#define VMCS_CR3_TARGET_COUNT			0x0000400A
#define VMCS_EXIT_CTLS				0x0000400C
#define		EXIT_CTLS_SAVE_DEBUG_CONTROLS	__BIT(2)
#define		EXIT_CTLS_HOST_LONG_MODE	__BIT(9)
#define		EXIT_CTLS_LOAD_PERFGLOBALCTRL	__BIT(12)
#define		EXIT_CTLS_ACK_INTERRUPT		__BIT(15)
#define		EXIT_CTLS_SAVE_PAT		__BIT(18)
#define		EXIT_CTLS_LOAD_PAT		__BIT(19)
#define		EXIT_CTLS_SAVE_EFER		__BIT(20)
#define		EXIT_CTLS_LOAD_EFER		__BIT(21)
#define		EXIT_CTLS_SAVE_PREEMPT_TIMER	__BIT(22)
#define		EXIT_CTLS_CLEAR_BNDCFGS		__BIT(23)
#define		EXIT_CTLS_CONCEAL_PT		__BIT(24)
#define		EXIT_CTLS_CLEAR_RTIT_CTL	__BIT(25)
#define		EXIT_CTLS_LOAD_CET		__BIT(28)
#define		EXIT_CTLS_LOAD_PKRS		__BIT(29)
#define VMCS_EXIT_MSR_STORE_COUNT		0x0000400E
#define VMCS_EXIT_MSR_LOAD_COUNT		0x00004010
#define VMCS_ENTRY_CTLS				0x00004012
#define		ENTRY_CTLS_LOAD_DEBUG_CONTROLS	__BIT(2)
#define		ENTRY_CTLS_LONG_MODE		__BIT(9)
#define		ENTRY_CTLS_SMM			__BIT(10)
#define		ENTRY_CTLS_DISABLE_DUAL		__BIT(11)
#define		ENTRY_CTLS_LOAD_PERFGLOBALCTRL	__BIT(13)
#define		ENTRY_CTLS_LOAD_PAT		__BIT(14)
#define		ENTRY_CTLS_LOAD_EFER		__BIT(15)
#define		ENTRY_CTLS_LOAD_BNDCFGS		__BIT(16)
#define		ENTRY_CTLS_CONCEAL_PT		__BIT(17)
#define		ENTRY_CTLS_LOAD_RTIT_CTL	__BIT(18)
#define		ENTRY_CTLS_LOAD_CET		__BIT(20)
#define		ENTRY_CTLS_LOAD_PKRS		__BIT(22)
#define VMCS_ENTRY_MSR_LOAD_COUNT		0x00004014
#define VMCS_ENTRY_INTR_INFO			0x00004016
#define		INTR_INFO_VECTOR		__BITS(7,0)
#define		INTR_INFO_TYPE			__BITS(10,8)
#define			INTR_TYPE_EXT_INT	0
#define			INTR_TYPE_NMI		2
#define			INTR_TYPE_HW_EXC	3
#define			INTR_TYPE_SW_INT	4
#define			INTR_TYPE_PRIV_SW_EXC	5
#define			INTR_TYPE_SW_EXC	6
#define			INTR_TYPE_OTHER		7
#define		INTR_INFO_ERROR			__BIT(11)
#define		INTR_INFO_VALID			__BIT(31)
#define VMCS_ENTRY_EXCEPTION_ERROR		0x00004018
#define VMCS_ENTRY_INSTRUCTION_LENGTH		0x0000401A
#define VMCS_TPR_THRESHOLD			0x0000401C
#define VMCS_PROCBASED_CTLS2			0x0000401E
#define		PROC_CTLS2_VIRT_APIC_ACCESSES	__BIT(0)
#define		PROC_CTLS2_ENABLE_EPT		__BIT(1)
#define		PROC_CTLS2_DESC_TABLE_EXITING	__BIT(2)
#define		PROC_CTLS2_ENABLE_RDTSCP	__BIT(3)
#define		PROC_CTLS2_VIRT_X2APIC		__BIT(4)
#define		PROC_CTLS2_ENABLE_VPID		__BIT(5)
#define		PROC_CTLS2_WBINVD_EXITING	__BIT(6)
#define		PROC_CTLS2_UNRESTRICTED_GUEST	__BIT(7)
#define		PROC_CTLS2_APIC_REG_VIRT	__BIT(8)
#define		PROC_CTLS2_VIRT_INT_DELIVERY	__BIT(9)
#define		PROC_CTLS2_PAUSE_LOOP_EXITING	__BIT(10)
#define		PROC_CTLS2_RDRAND_EXITING	__BIT(11)
#define		PROC_CTLS2_INVPCID_ENABLE	__BIT(12)
#define		PROC_CTLS2_VMFUNC_ENABLE	__BIT(13)
#define		PROC_CTLS2_VMCS_SHADOWING	__BIT(14)
#define		PROC_CTLS2_ENCLS_EXITING	__BIT(15)
#define		PROC_CTLS2_RDSEED_EXITING	__BIT(16)
#define		PROC_CTLS2_PML_ENABLE		__BIT(17)
#define		PROC_CTLS2_EPT_VIOLATION	__BIT(18)
#define		PROC_CTLS2_CONCEAL_VMX_FROM_PT	__BIT(19)
#define		PROC_CTLS2_XSAVES_ENABLE	__BIT(20)
#define		PROC_CTLS2_MODE_BASED_EXEC_EPT	__BIT(22)
#define		PROC_CTLS2_SUBPAGE_PERMISSIONS	__BIT(23)
#define		PROC_CTLS2_PT_USES_GPA		__BIT(24)
#define		PROC_CTLS2_USE_TSC_SCALING	__BIT(25)
#define		PROC_CTLS2_WAIT_PAUSE_ENABLE	__BIT(26)
#define		PROC_CTLS2_ENCLV_EXITING	__BIT(28)
#define VMCS_PLE_GAP				0x00004020
#define VMCS_PLE_WINDOW				0x00004022
/* 32-bit read-only data fields */
#define VMCS_INSTRUCTION_ERROR			0x00004400
#define VMCS_EXIT_REASON			0x00004402
#define VMCS_EXIT_INTR_INFO			0x00004404
#define VMCS_EXIT_INTR_ERRCODE			0x00004406
#define VMCS_IDT_VECTORING_INFO			0x00004408
#define VMCS_IDT_VECTORING_ERROR		0x0000440A
#define VMCS_EXIT_INSTRUCTION_LENGTH		0x0000440C
#define VMCS_EXIT_INSTRUCTION_INFO		0x0000440E
/* 32-bit guest-state fields */
#define VMCS_GUEST_ES_LIMIT			0x00004800
#define VMCS_GUEST_CS_LIMIT			0x00004802
#define VMCS_GUEST_SS_LIMIT			0x00004804
#define VMCS_GUEST_DS_LIMIT			0x00004806
#define VMCS_GUEST_FS_LIMIT			0x00004808
#define VMCS_GUEST_GS_LIMIT			0x0000480A
#define VMCS_GUEST_LDTR_LIMIT			0x0000480C
#define VMCS_GUEST_TR_LIMIT			0x0000480E
#define VMCS_GUEST_GDTR_LIMIT			0x00004810
#define VMCS_GUEST_IDTR_LIMIT			0x00004812
#define VMCS_GUEST_ES_ACCESS_RIGHTS		0x00004814
#define VMCS_GUEST_CS_ACCESS_RIGHTS		0x00004816
#define VMCS_GUEST_SS_ACCESS_RIGHTS		0x00004818
#define VMCS_GUEST_DS_ACCESS_RIGHTS		0x0000481A
#define VMCS_GUEST_FS_ACCESS_RIGHTS		0x0000481C
#define VMCS_GUEST_GS_ACCESS_RIGHTS		0x0000481E
#define VMCS_GUEST_LDTR_ACCESS_RIGHTS		0x00004820
#define VMCS_GUEST_TR_ACCESS_RIGHTS		0x00004822
#define VMCS_GUEST_INTERRUPTIBILITY		0x00004824
#define		INT_STATE_STI			__BIT(0)
#define		INT_STATE_MOVSS			__BIT(1)
#define		INT_STATE_SMI			__BIT(2)
#define		INT_STATE_NMI			__BIT(3)
#define		INT_STATE_ENCLAVE		__BIT(4)
#define VMCS_GUEST_ACTIVITY			0x00004826
#define VMCS_GUEST_SMBASE			0x00004828
#define VMCS_GUEST_IA32_SYSENTER_CS		0x0000482A
#define VMCS_PREEMPTION_TIMER_VALUE		0x0000482E
/* 32-bit host state fields */
#define VMCS_HOST_IA32_SYSENTER_CS		0x00004C00
/* Natural-Width control fields */
#define VMCS_CR0_MASK				0x00006000
#define VMCS_CR4_MASK				0x00006002
#define VMCS_CR0_SHADOW				0x00006004
#define VMCS_CR4_SHADOW				0x00006006
#define VMCS_CR3_TARGET0			0x00006008
#define VMCS_CR3_TARGET1			0x0000600A
#define VMCS_CR3_TARGET2			0x0000600C
#define VMCS_CR3_TARGET3			0x0000600E
/* Natural-Width read-only fields */
#define VMCS_EXIT_QUALIFICATION			0x00006400
#define VMCS_IO_RCX				0x00006402
#define VMCS_IO_RSI				0x00006404
#define VMCS_IO_RDI				0x00006406
#define VMCS_IO_RIP				0x00006408
#define VMCS_GUEST_LINEAR_ADDRESS		0x0000640A
/* Natural-Width guest-state fields */
#define VMCS_GUEST_CR0				0x00006800
#define VMCS_GUEST_CR3				0x00006802
#define VMCS_GUEST_CR4				0x00006804
#define VMCS_GUEST_ES_BASE			0x00006806
#define VMCS_GUEST_CS_BASE			0x00006808
#define VMCS_GUEST_SS_BASE			0x0000680A
#define VMCS_GUEST_DS_BASE			0x0000680C
#define VMCS_GUEST_FS_BASE			0x0000680E
#define VMCS_GUEST_GS_BASE			0x00006810
#define VMCS_GUEST_LDTR_BASE			0x00006812
#define VMCS_GUEST_TR_BASE			0x00006814
#define VMCS_GUEST_GDTR_BASE			0x00006816
#define VMCS_GUEST_IDTR_BASE			0x00006818
#define VMCS_GUEST_DR7				0x0000681A
#define VMCS_GUEST_RSP				0x0000681C
#define VMCS_GUEST_RIP				0x0000681E
#define VMCS_GUEST_RFLAGS			0x00006820
#define VMCS_GUEST_PENDING_DBG_EXCEPTIONS	0x00006822
#define VMCS_GUEST_IA32_SYSENTER_ESP		0x00006824
#define VMCS_GUEST_IA32_SYSENTER_EIP		0x00006826
#define VMCS_GUEST_IA32_S_CET			0x00006828
#define VMCS_GUEST_SSP				0x0000682A
#define VMCS_GUEST_IA32_INTR_SSP_TABLE		0x0000682C
/* Natural-Width host-state fields */
#define VMCS_HOST_CR0				0x00006C00
#define VMCS_HOST_CR3				0x00006C02
#define VMCS_HOST_CR4				0x00006C04
#define VMCS_HOST_FS_BASE			0x00006C06
#define VMCS_HOST_GS_BASE			0x00006C08
#define VMCS_HOST_TR_BASE			0x00006C0A
#define VMCS_HOST_GDTR_BASE			0x00006C0C
#define VMCS_HOST_IDTR_BASE			0x00006C0E
#define VMCS_HOST_IA32_SYSENTER_ESP		0x00006C10
#define VMCS_HOST_IA32_SYSENTER_EIP		0x00006C12
#define VMCS_HOST_RSP				0x00006C14
#define VMCS_HOST_RIP				0x00006C16
#define VMCS_HOST_IA32_S_CET			0x00006C18
#define VMCS_HOST_SSP				0x00006C1A
#define VMCS_HOST_IA32_INTR_SSP_TABLE		0x00006C1C

/* VMX basic exit reasons. */
#define VMCS_EXITCODE_EXC_NMI			0
#define VMCS_EXITCODE_EXT_INT			1
#define VMCS_EXITCODE_SHUTDOWN			2
#define VMCS_EXITCODE_INIT			3
#define VMCS_EXITCODE_SIPI			4
#define VMCS_EXITCODE_SMI			5
#define VMCS_EXITCODE_OTHER_SMI			6
#define VMCS_EXITCODE_INT_WINDOW		7
#define VMCS_EXITCODE_NMI_WINDOW		8
#define VMCS_EXITCODE_TASK_SWITCH		9
#define VMCS_EXITCODE_CPUID			10
#define VMCS_EXITCODE_GETSEC			11
#define VMCS_EXITCODE_HLT			12
#define VMCS_EXITCODE_INVD			13
#define VMCS_EXITCODE_INVLPG			14
#define VMCS_EXITCODE_RDPMC			15
#define VMCS_EXITCODE_RDTSC			16
#define VMCS_EXITCODE_RSM			17
#define VMCS_EXITCODE_VMCALL			18
#define VMCS_EXITCODE_VMCLEAR			19
#define VMCS_EXITCODE_VMLAUNCH			20
#define VMCS_EXITCODE_VMPTRLD			21
#define VMCS_EXITCODE_VMPTRST			22
#define VMCS_EXITCODE_VMREAD			23
#define VMCS_EXITCODE_VMRESUME			24
#define VMCS_EXITCODE_VMWRITE			25
#define VMCS_EXITCODE_VMXOFF			26
#define VMCS_EXITCODE_VMXON			27
#define VMCS_EXITCODE_CR			28
#define VMCS_EXITCODE_DR			29
#define VMCS_EXITCODE_IO			30
#define VMCS_EXITCODE_RDMSR			31
#define VMCS_EXITCODE_WRMSR			32
#define VMCS_EXITCODE_FAIL_GUEST_INVALID	33
#define VMCS_EXITCODE_FAIL_MSR_INVALID		34
#define VMCS_EXITCODE_MWAIT			36
#define VMCS_EXITCODE_TRAP_FLAG			37
#define VMCS_EXITCODE_MONITOR			39
#define VMCS_EXITCODE_PAUSE			40
#define VMCS_EXITCODE_FAIL_MACHINE_CHECK	41
#define VMCS_EXITCODE_TPR_BELOW			43
#define VMCS_EXITCODE_APIC_ACCESS		44
#define VMCS_EXITCODE_VEOI			45
#define VMCS_EXITCODE_GDTR_IDTR			46
#define VMCS_EXITCODE_LDTR_TR			47
#define VMCS_EXITCODE_EPT_VIOLATION		48
#define VMCS_EXITCODE_EPT_MISCONFIG		49
#define VMCS_EXITCODE_INVEPT			50
#define VMCS_EXITCODE_RDTSCP			51
#define VMCS_EXITCODE_PREEMPT_TIMEOUT		52
#define VMCS_EXITCODE_INVVPID			53
#define VMCS_EXITCODE_WBINVD			54
#define VMCS_EXITCODE_XSETBV			55
#define VMCS_EXITCODE_APIC_WRITE		56
#define VMCS_EXITCODE_RDRAND			57
#define VMCS_EXITCODE_INVPCID			58
#define VMCS_EXITCODE_VMFUNC			59
#define VMCS_EXITCODE_ENCLS			60
#define VMCS_EXITCODE_RDSEED			61
#define VMCS_EXITCODE_PAGE_LOG_FULL		62
#define VMCS_EXITCODE_XSAVES			63
#define VMCS_EXITCODE_XRSTORS			64
#define VMCS_EXITCODE_SPP			66
#define VMCS_EXITCODE_UMWAIT			67
#define VMCS_EXITCODE_TPAUSE			68


/* -------------------------------------------------------------------------- */

static void vmx_vcpu_state_provide(struct nvmm_cpu *, uint64_t);
static void vmx_vcpu_state_commit(struct nvmm_cpu *);

#define VMX_MSRLIST_STAR		0
#define VMX_MSRLIST_LSTAR		1
#define VMX_MSRLIST_CSTAR		2
#define VMX_MSRLIST_SFMASK		3
#define VMX_MSRLIST_KERNELGSBASE	4
#define VMX_MSRLIST_EXIT_NMSR		5
#define VMX_MSRLIST_L1DFLUSH		5

/* On entry, we may do +1 to include L1DFLUSH. */
static size_t vmx_msrlist_entry_nmsr __read_mostly = VMX_MSRLIST_EXIT_NMSR;

struct vmcs {
	uint32_t ident;
#define VMCS_IDENT_REVISION	__BITS(30,0)
#define VMCS_IDENT_SHADOW	__BIT(31)

	uint32_t abort;
	uint8_t data[PAGE_SIZE - 8];
} __packed;

CTASSERT(sizeof(struct vmcs) == PAGE_SIZE);

struct msr_entry {
	uint32_t msr;
	uint32_t rsvd;
	uint64_t val;
} __packed;

#define VPID_MAX	0xFFFF

/* Make sure we never run out of VPIDs. */
CTASSERT(VPID_MAX-1 >= NVMM_MAX_MACHINES * NVMM_MAX_VCPUS);

#define VMX_INVVPID_ADDRESS		0
#define VMX_INVVPID_CONTEXT		1
#define VMX_INVVPID_ALL			2
#define VMX_INVVPID_CONTEXT_NOGLOBAL	3

#define VMX_INVEPT_CONTEXT		1
#define VMX_INVEPT_ALL			2

static uint64_t vmx_tlb_flush_op __read_mostly;
static uint64_t vmx_ept_flush_op __read_mostly;
static uint64_t vmx_eptp_type __read_mostly;

static uint64_t vmx_pinbased_ctls __read_mostly;
static uint64_t vmx_procbased_ctls __read_mostly;
static uint64_t vmx_procbased_ctls2 __read_mostly;
static uint64_t vmx_entry_ctls __read_mostly;
static uint64_t vmx_exit_ctls __read_mostly;

static uint64_t vmx_cr0_fixed0 __read_mostly;
static uint64_t vmx_cr0_fixed1 __read_mostly;
static uint64_t vmx_cr4_fixed0 __read_mostly;
static uint64_t vmx_cr4_fixed1 __read_mostly;

/* NetBSD keeps this in its pmap (pmap_ept_has_ad). */
static bool vmx_ept_has_ad __read_mostly;

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

/*
 * VPID ("ASID") bitmap. NetBSD takes an adaptive mutex inside
 * vmx_vmcs_enter(), i.e. with preemption disabled; on FreeBSD that is a
 * critical section, where only a spin mutex may be taken.
 */
static uint8_t *vmx_asidmap __read_mostly;
static uint32_t vmx_maxasid __read_mostly;
static struct mtx vmx_asidlock;

static uint64_t vmx_xcr0_mask __read_mostly;

#define VMX_NCPUIDS	32

#define VMCS_NPAGES	1
#define VMCS_SIZE	(VMCS_NPAGES * PAGE_SIZE)

#define MSRBM_NPAGES	1
#define MSRBM_SIZE	(MSRBM_NPAGES * PAGE_SIZE)

#define CR0_STATIC_MASK \
	(CR0_ET | CR0_NW | CR0_CD)

#define CR4_VALID \
	(CR4_VME |			\
	 CR4_PVI |			\
	 CR4_TSD |			\
	 CR4_DE |			\
	 CR4_PSE |			\
	 CR4_PAE |			\
	 CR4_MCE |			\
	 CR4_PGE |			\
	 CR4_PCE |			\
	 CR4_OSFXSR |			\
	 CR4_OSXMMEXCPT |		\
	 CR4_UMIP |			\
	 /* CR4_LA57 excluded */	\
	 /* CR4_VMXE excluded */	\
	 /* CR4_SMXE excluded */	\
	 CR4_FSGSBASE |			\
	 CR4_PCIDE |			\
	 CR4_OSXSAVE |			\
	 CR4_SMEP |			\
	 CR4_SMAP			\
	 /* CR4_PKE excluded */		\
	 /* CR4_CET excluded */		\
	 /* CR4_PKS excluded */)
#define CR4_INVALID \
	(0xFFFFFFFFFFFFFFFFULL & ~CR4_VALID)

#define EFER_TLB_FLUSH \
	(EFER_NXE|EFER_LMA|EFER_LME)
#define CR0_TLB_FLUSH \
	(CR0_PG|CR0_WP|CR0_CD|CR0_NW)
#define CR4_TLB_FLUSH \
	(CR4_PSE|CR4_PAE|CR4_PGE|CR4_PCIDE|CR4_SMEP)

/* -------------------------------------------------------------------------- */

static const size_t vmx_vcpu_conf_sizes[NVMM_X86_VCPU_NCONF] = {
	[NVMM_VCPU_CONF_MD(NVMM_VCPU_CONF_CPUID)] =
	    sizeof(struct nvmm_vcpu_conf_cpuid),
	[NVMM_VCPU_CONF_MD(NVMM_VCPU_CONF_TPR)] =
	    sizeof(struct nvmm_vcpu_conf_tpr),
	[NVMM_VCPU_CONF_MD(NVMM_VCPU_CONF_XCR0_MASK)] =
	    sizeof(uint64_t),
};

/*
 * FreeBSD: no host TLB generation bumped by a pmap callback as in NetBSD.
 * FreeBSD's EPT pmap counts its invalidations in pm_eptgen itself; each
 * host CPU remembers the generation it last flushed this EPT at, as
 * bhyve's vmx->eptgen[]. -1: never, so the first entry on a CPU always
 * flushes (the EPT root page may have served another machine before).
 */
struct vmx_machdata {
	long eptgen[MAXCPU];
};
struct vmx_cpudata {
	/* General */
	uint64_t asid;
	bool gtlb_want_flush;
	bool gtsc_want_update;

	/* VMCS */
	struct vmcs *vmcs;
	paddr_t vmcs_pa;
	size_t vmcs_refcnt;
	os_cpu_t *vmcs_ci;
	bool vmcs_launched;

	/* MSR bitmap */
	uint8_t *msrbm;
	paddr_t msrbm_pa;

	/* Host state */
	uint64_t hdr[4];	/* FreeBSD: host DR0-DR3, DR6, DR7 and */
	uint64_t hdr6;		/* DEBUGCTL, saved around the guest as */
	uint64_t hdr7;		/* bhyve does (NetBSD has its own */
	uint64_t hdebugctl;	/* per-lwp save of debug registers) */
	uint64_t hxcr0;
	uint64_t star;
	uint64_t lstar;
	uint64_t cstar;
	uint64_t sfmask;
	uint64_t kernelgsbase;

	/* Intr state */
	bool int_window_exit;
	bool nmi_window_exit;
	bool evt_pending;

	/* Guest state */
	struct msr_entry *gmsr;
	paddr_t gmsr_pa;
	uint64_t gmsr_misc_enable;
	uint64_t gcr2;
	uint64_t gcr8;
	uint64_t gxcr0;
	uint64_t gprs[NVMM_X64_NGPR];
	uint64_t drs[NVMM_X64_NDR];
	uint64_t gtsc;

	/* VCPU configuration. */
	bool cpuidpresent[VMX_NCPUIDS];
	struct nvmm_vcpu_conf_cpuid cpuid[VMX_NCPUIDS];
	struct nvmm_vcpu_conf_tpr tpr;
	uint64_t xcr0_mask;

	/* FreeBSD: allocated size, which depends on the XSAVE area. */
	size_t cpudata_size;

	/*
	 * Guest XSAVE state.  Must be the last member because it may
	 * be extended variably by whatever CPU we're running on.  We
	 * add a flexible array member afterward to ward UB-exploiting
	 * compilers away from memset/memcpy calls that access it.
	 */
	struct xsave_header gfpu __aligned(64);
	uint8_t gfpu_ext[];
};

static const struct {
	uint64_t selector;
	uint64_t attrib;
	uint64_t limit;
	uint64_t base;
} vmx_guest_segs[NVMM_X64_NSEG] = {
	[NVMM_X64_SEG_ES] = {
		VMCS_GUEST_ES_SELECTOR,
		VMCS_GUEST_ES_ACCESS_RIGHTS,
		VMCS_GUEST_ES_LIMIT,
		VMCS_GUEST_ES_BASE
	},
	[NVMM_X64_SEG_CS] = {
		VMCS_GUEST_CS_SELECTOR,
		VMCS_GUEST_CS_ACCESS_RIGHTS,
		VMCS_GUEST_CS_LIMIT,
		VMCS_GUEST_CS_BASE
	},
	[NVMM_X64_SEG_SS] = {
		VMCS_GUEST_SS_SELECTOR,
		VMCS_GUEST_SS_ACCESS_RIGHTS,
		VMCS_GUEST_SS_LIMIT,
		VMCS_GUEST_SS_BASE
	},
	[NVMM_X64_SEG_DS] = {
		VMCS_GUEST_DS_SELECTOR,
		VMCS_GUEST_DS_ACCESS_RIGHTS,
		VMCS_GUEST_DS_LIMIT,
		VMCS_GUEST_DS_BASE
	},
	[NVMM_X64_SEG_FS] = {
		VMCS_GUEST_FS_SELECTOR,
		VMCS_GUEST_FS_ACCESS_RIGHTS,
		VMCS_GUEST_FS_LIMIT,
		VMCS_GUEST_FS_BASE
	},
	[NVMM_X64_SEG_GS] = {
		VMCS_GUEST_GS_SELECTOR,
		VMCS_GUEST_GS_ACCESS_RIGHTS,
		VMCS_GUEST_GS_LIMIT,
		VMCS_GUEST_GS_BASE
	},
	[NVMM_X64_SEG_GDT] = {
		0, /* doesn't exist */
		0, /* doesn't exist */
		VMCS_GUEST_GDTR_LIMIT,
		VMCS_GUEST_GDTR_BASE
	},
	[NVMM_X64_SEG_IDT] = {
		0, /* doesn't exist */
		0, /* doesn't exist */
		VMCS_GUEST_IDTR_LIMIT,
		VMCS_GUEST_IDTR_BASE
	},
	[NVMM_X64_SEG_LDT] = {
		VMCS_GUEST_LDTR_SELECTOR,
		VMCS_GUEST_LDTR_ACCESS_RIGHTS,
		VMCS_GUEST_LDTR_LIMIT,
		VMCS_GUEST_LDTR_BASE
	},
	[NVMM_X64_SEG_TR] = {
		VMCS_GUEST_TR_SELECTOR,
		VMCS_GUEST_TR_ACCESS_RIGHTS,
		VMCS_GUEST_TR_LIMIT,
		VMCS_GUEST_TR_BASE
	}
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

static uint32_t vmx_revision __read_mostly;

static
OS_IPI_FUNC(vmx_vmclear_ipi)
{
	paddr_t vmcs_pa = (paddr_t)arg;

	vmx_vmclear(&vmcs_pa);
}

static void
vmx_vmclear_remote(os_cpu_t *ci, paddr_t vmcs_pa)
{
	int bound;

	OS_ASSERT(os_preempt_disabled());

	bound = curlwp_bind();
	os_preempt_enable();

	os_ipi_unicast(ci, vmx_vmclear_ipi, (void *)vmcs_pa);

	os_preempt_disable();
	curlwp_bindx(bound);
	(void)bound;	/* FreeBSD's curlwp_bindx() is sched_unpin() */
}

static void
vmx_vmcs_enter(struct nvmm_cpu *vcpu)
{
	struct vmx_cpudata *cpudata = vcpu->cpudata;
	os_cpu_t *vmcs_ci;

	cpudata->vmcs_refcnt++;
	if (cpudata->vmcs_refcnt > 1) {
		OS_ASSERT(os_preempt_disabled());
		OS_ASSERT(vmx_vmptrst() == cpudata->vmcs_pa);
		return;
	}

	vmcs_ci = cpudata->vmcs_ci;
	cpudata->vmcs_ci = (void *)0x00FFFFFFFFFFFFFF; /* clobber */

	os_preempt_disable();

	if (vmcs_ci == NULL) {
		/* This VMCS is loaded for the first time. */
		vmx_vmclear(&cpudata->vmcs_pa);
		cpudata->vmcs_launched = false;
	} else if (vmcs_ci != os_curcpu()) {
		/* This VMCS is active on a remote CPU. */
		vmx_vmclear_remote(vmcs_ci, cpudata->vmcs_pa);
		cpudata->vmcs_launched = false;
	} else {
		/* This VMCS is active on curcpu, nothing to do. */
	}

	vmx_vmptrld(&cpudata->vmcs_pa);
}

static void
vmx_vmcs_leave(struct nvmm_cpu *vcpu)
{
	struct vmx_cpudata *cpudata = vcpu->cpudata;

	OS_ASSERT(os_preempt_disabled());
	OS_ASSERT(vmx_vmptrst() == cpudata->vmcs_pa);
	OS_ASSERT(cpudata->vmcs_refcnt > 0);
	cpudata->vmcs_refcnt--;

	if (cpudata->vmcs_refcnt > 0) {
		return;
	}

	cpudata->vmcs_ci = os_curcpu();
	os_preempt_enable();
}

static void
vmx_vmcs_destroy(struct nvmm_cpu *vcpu)
{
	struct vmx_cpudata *cpudata = vcpu->cpudata;

	OS_ASSERT(os_preempt_disabled());
	OS_ASSERT(vmx_vmptrst() == cpudata->vmcs_pa);
	OS_ASSERT(cpudata->vmcs_refcnt == 1);
	cpudata->vmcs_refcnt--;

	vmx_vmclear(&cpudata->vmcs_pa);
	os_preempt_enable();
}

/* -------------------------------------------------------------------------- */

static void
vmx_event_waitexit_enable(struct nvmm_cpu *vcpu, bool nmi)
{
	struct vmx_cpudata *cpudata = vcpu->cpudata;
	uint64_t ctls1;

	ctls1 = vmx_vmread(VMCS_PROCBASED_CTLS);

	if (nmi) {
		// XXX INT_STATE_NMI?
		ctls1 |= PROC_CTLS_NMI_WINDOW_EXITING;
		cpudata->nmi_window_exit = true;
	} else {
		ctls1 |= PROC_CTLS_INT_WINDOW_EXITING;
		cpudata->int_window_exit = true;
	}

	vmx_vmwrite(VMCS_PROCBASED_CTLS, ctls1);
}

static void
vmx_event_waitexit_disable(struct nvmm_cpu *vcpu, bool nmi)
{
	struct vmx_cpudata *cpudata = vcpu->cpudata;
	uint64_t ctls1;

	ctls1 = vmx_vmread(VMCS_PROCBASED_CTLS);

	if (nmi) {
		ctls1 &= ~PROC_CTLS_NMI_WINDOW_EXITING;
		cpudata->nmi_window_exit = false;
	} else {
		ctls1 &= ~PROC_CTLS_INT_WINDOW_EXITING;
		cpudata->int_window_exit = false;
	}

	vmx_vmwrite(VMCS_PROCBASED_CTLS, ctls1);
}

static inline bool
vmx_excp_has_rf(uint8_t vector)
{
	switch (vector) {
	case 1:		/* #DB */
	case 4:		/* #OF */
	case 8:		/* #DF */
	case 18:	/* #MC */
		return false;
	default:
		return true;
	}
}

static inline int
vmx_excp_has_error(uint8_t vector)
{
	switch (vector) {
	case 8:		/* #DF */
	case 10:	/* #TS */
	case 11:	/* #NP */
	case 12:	/* #SS */
	case 13:	/* #GP */
	case 14:	/* #PF */
	case 17:	/* #AC */
	case 21:	/* #CP */
	case 30:	/* #SX */
		return 1;
	default:
		return 0;
	}
}

static int
vmx_vcpu_inject(struct nvmm_cpu *vcpu)
{
	struct nvmm_comm_page *comm = vcpu->comm;
	struct vmx_cpudata *cpudata = vcpu->cpudata;
	int type = 0, err = 0, ret = EINVAL;
	uint64_t rflags, info, error;
	u_int evtype;
	uint8_t vector;

	evtype = comm->event.type;
	vector = comm->event.vector;
	error = comm->event.u.excp.error;
	__compiler_membar();

	vmx_vmcs_enter(vcpu);

	switch (evtype) {
	case NVMM_VCPU_EVENT_EXCP:
		if (vector == 2 || vector >= 32)
			goto out;
		if (vector == 3 || vector == 0)
			goto out;
		if (vmx_excp_has_rf(vector)) {
			rflags = vmx_vmread(VMCS_GUEST_RFLAGS);
			vmx_vmwrite(VMCS_GUEST_RFLAGS, rflags | PSL_RF);
		}
		type = INTR_TYPE_HW_EXC;
		err = vmx_excp_has_error(vector);
		break;
	case NVMM_VCPU_EVENT_INTR:
		type = INTR_TYPE_EXT_INT;
		if (vector == 2) {
			type = INTR_TYPE_NMI;
			vmx_event_waitexit_enable(vcpu, true);
		}
		err = 0;
		break;
	default:
		goto out;
	}

	info =
	    __SHIFTIN(vector, INTR_INFO_VECTOR) |
	    __SHIFTIN(type, INTR_INFO_TYPE) |
	    __SHIFTIN(err, INTR_INFO_ERROR) |
	    __SHIFTIN(1, INTR_INFO_VALID);
	vmx_vmwrite(VMCS_ENTRY_INTR_INFO, info);
	vmx_vmwrite(VMCS_ENTRY_EXCEPTION_ERROR, error);

	cpudata->evt_pending = true;
	ret = 0;

out:
	vmx_vmcs_leave(vcpu);
	return ret;
}

/* -------------------------------------------------------------------------- */

static void
vmx_inject_ud(struct nvmm_cpu *vcpu)
{
	struct nvmm_comm_page *comm = vcpu->comm;
	int ret __unused;

	comm->event.type = NVMM_VCPU_EVENT_EXCP;
	comm->event.vector = 6;
	comm->event.u.excp.error = 0;

	ret = vmx_vcpu_inject(vcpu);
	OS_ASSERT(ret == 0);
}

static void
vmx_inject_gp(struct nvmm_cpu *vcpu)
{
	struct nvmm_comm_page *comm = vcpu->comm;
	int ret __unused;

	comm->event.type = NVMM_VCPU_EVENT_EXCP;
	comm->event.vector = 13;
	comm->event.u.excp.error = 0;

	ret = vmx_vcpu_inject(vcpu);
	OS_ASSERT(ret == 0);
}

static inline int
vmx_vcpu_event_commit(struct nvmm_cpu *vcpu)
{
	if (__predict_true(!vcpu->comm->event_commit)) {
		return 0;
	}
	vcpu->comm->event_commit = false;
	return vmx_vcpu_inject(vcpu);
}

static inline void
vmx_inkernel_advance(void)
{
	uint64_t rip, inslen, intstate, rflags;

	/*
	 * Maybe we should also apply single-stepping and debug exceptions.
	 * Matters for guest-ring3, because it can execute 'cpuid' under a
	 * debugger.
	 */

	inslen = vmx_vmread(VMCS_EXIT_INSTRUCTION_LENGTH);
	rip = vmx_vmread(VMCS_GUEST_RIP);
	vmx_vmwrite(VMCS_GUEST_RIP, rip + inslen);

	rflags = vmx_vmread(VMCS_GUEST_RFLAGS);
	vmx_vmwrite(VMCS_GUEST_RFLAGS, rflags & ~PSL_RF);

	intstate = vmx_vmread(VMCS_GUEST_INTERRUPTIBILITY);
	vmx_vmwrite(VMCS_GUEST_INTERRUPTIBILITY,
	    intstate & ~(INT_STATE_STI|INT_STATE_MOVSS));
}

static void
vmx_exit_invalid(struct nvmm_vcpu_exit *exit, uint64_t code)
{
	exit->u.inv.hwcode = code;
	exit->reason = NVMM_VCPU_EXIT_INVALID;
}

/*
 * NMI that arrived while the guest ran. NetBSD calls its NMI handlers on
 * a fake trapframe; FreeBSD, as bhyve, raises it again on the host with
 * "int $2" while interrupts are still disabled, which runs the normal
 * NMI path (watchdog, DTrace, debugger).
 */
static void
vmx_exit_exc_nmi(struct nvmm_machine *mach __unused,
    struct nvmm_cpu *vcpu __unused, struct nvmm_vcpu_exit *exit)
{
	uint64_t qual;

	qual = vmx_vmread(VMCS_EXIT_INTR_INFO);

	if ((qual & INTR_INFO_VALID) == 0) {
		goto error;
	}
	if (__SHIFTOUT(qual, INTR_INFO_TYPE) != INTR_TYPE_NMI) {
		goto error;
	}

	__asm volatile("int $2");

	exit->reason = NVMM_VCPU_EXIT_NONE;
	return;

error:
	vmx_exit_invalid(exit, VMCS_EXITCODE_EXC_NMI);
}

static void
vmx_exit_insn(struct nvmm_vcpu_exit *exit, uint64_t reason)
{
	uint64_t inslen, rip;

	inslen = vmx_vmread(VMCS_EXIT_INSTRUCTION_LENGTH);
	rip = vmx_vmread(VMCS_GUEST_RIP);
	exit->u.insn.npc = rip + inslen;
	exit->reason = reason;
}

static void
vmx_exit_hlt(struct nvmm_machine *mach __unused, struct nvmm_cpu *vcpu,
    struct nvmm_vcpu_exit *exit)
{
	struct vmx_cpudata *cpudata = vcpu->cpudata;
	uint64_t rflags;

	if (cpudata->int_window_exit) {
		rflags = vmx_vmread(VMCS_GUEST_RFLAGS);
		if (rflags & PSL_I) {
			vmx_event_waitexit_disable(vcpu, false);
		}
	}

	vmx_inkernel_advance();
	exit->reason = NVMM_VCPU_EXIT_HALTED;
}

#define VMX_QUAL_IO_SIZE	__BITS(2,0)
#define		IO_SIZE_8	0
#define		IO_SIZE_16	1
#define		IO_SIZE_32	3
#define VMX_QUAL_IO_IN		__BIT(3)
#define VMX_QUAL_IO_STR		__BIT(4)
#define VMX_QUAL_IO_REP		__BIT(5)
#define VMX_QUAL_IO_DX		__BIT(6)
#define VMX_QUAL_IO_PORT	__BITS(31,16)

#define VMX_INFO_IO_ADRSIZE	__BITS(9,7)
#define		IO_ADRSIZE_16	0
#define		IO_ADRSIZE_32	1
#define		IO_ADRSIZE_64	2
#define VMX_INFO_IO_SEG		__BITS(17,15)

static void
vmx_exit_io(struct nvmm_machine *mach __unused, struct nvmm_cpu *vcpu,
    struct nvmm_vcpu_exit *exit)
{
	uint64_t qual, info, inslen, rip;

	qual = vmx_vmread(VMCS_EXIT_QUALIFICATION);
	info = vmx_vmread(VMCS_EXIT_INSTRUCTION_INFO);

	exit->reason = NVMM_VCPU_EXIT_IO;

	exit->u.io.in = (qual & VMX_QUAL_IO_IN) != 0;
	exit->u.io.port = __SHIFTOUT(qual, VMX_QUAL_IO_PORT);

	OS_ASSERT(__SHIFTOUT(info, VMX_INFO_IO_SEG) < 6);
	exit->u.io.seg = __SHIFTOUT(info, VMX_INFO_IO_SEG);

	if (__SHIFTOUT(info, VMX_INFO_IO_ADRSIZE) == IO_ADRSIZE_64) {
		exit->u.io.address_size = 8;
	} else if (__SHIFTOUT(info, VMX_INFO_IO_ADRSIZE) == IO_ADRSIZE_32) {
		exit->u.io.address_size = 4;
	} else if (__SHIFTOUT(info, VMX_INFO_IO_ADRSIZE) == IO_ADRSIZE_16) {
		exit->u.io.address_size = 2;
	}

	if (__SHIFTOUT(qual, VMX_QUAL_IO_SIZE) == IO_SIZE_32) {
		exit->u.io.operand_size = 4;
	} else if (__SHIFTOUT(qual, VMX_QUAL_IO_SIZE) == IO_SIZE_16) {
		exit->u.io.operand_size = 2;
	} else if (__SHIFTOUT(qual, VMX_QUAL_IO_SIZE) == IO_SIZE_8) {
		exit->u.io.operand_size = 1;
	}

	exit->u.io.rep = (qual & VMX_QUAL_IO_REP) != 0;
	exit->u.io.str = (qual & VMX_QUAL_IO_STR) != 0;

	if (exit->u.io.in && exit->u.io.str) {
		exit->u.io.seg = NVMM_X64_SEG_ES;
	}

	inslen = vmx_vmread(VMCS_EXIT_INSTRUCTION_LENGTH);
	rip = vmx_vmread(VMCS_GUEST_RIP);
	exit->u.io.npc = rip + inslen;

	vmx_vcpu_state_provide(vcpu,
	    NVMM_X64_STATE_GPRS | NVMM_X64_STATE_SEGS |
	    NVMM_X64_STATE_CRS | NVMM_X64_STATE_MSRS);
}

#define VMX_EPT_VIOLATION_READ		__BIT(0)
#define VMX_EPT_VIOLATION_WRITE		__BIT(1)
#define VMX_EPT_VIOLATION_EXECUTE	__BIT(2)

static void
vmx_exit_epf(struct nvmm_machine *mach __unused, struct nvmm_cpu *vcpu,
    struct nvmm_vcpu_exit *exit)
{
	uint64_t perm;
	gpaddr_t gpa;

	gpa = vmx_vmread(VMCS_GUEST_PHYSICAL_ADDRESS);

	exit->reason = NVMM_VCPU_EXIT_MEMORY;
	perm = vmx_vmread(VMCS_EXIT_QUALIFICATION);
	if (perm & VMX_EPT_VIOLATION_WRITE)
		exit->u.mem.prot = PROT_WRITE;
	else if (perm & VMX_EPT_VIOLATION_EXECUTE)
		exit->u.mem.prot = PROT_EXEC;
	else
		exit->u.mem.prot = PROT_READ;
	exit->u.mem.gpa = gpa;
	exit->u.mem.inst_len = 0;

	vmx_vcpu_state_provide(vcpu,
	    NVMM_X64_STATE_GPRS | NVMM_X64_STATE_SEGS |
	    NVMM_X64_STATE_CRS | NVMM_X64_STATE_MSRS);
}

/* -------------------------------------------------------------------------- */

#define VMX_CPUID_MAX_BASIC		0x16
#define VMX_CPUID_MAX_HYPERVISOR	0x40000000
#define VMX_CPUID_MAX_EXTENDED		0x80000008
static uint32_t vmx_cpuid_max_basic __read_mostly;
static uint32_t vmx_cpuid_max_extended __read_mostly;

static void
vmx_inkernel_exec_cpuid(struct vmx_cpudata *cpudata, uint64_t eax, uint64_t ecx)
{
	u_int descs[4];

	x86_cpuid2(eax, ecx, descs);
	cpudata->gprs[NVMM_X64_GPR_RAX] = descs[0];
	cpudata->gprs[NVMM_X64_GPR_RBX] = descs[1];
	cpudata->gprs[NVMM_X64_GPR_RCX] = descs[2];
	cpudata->gprs[NVMM_X64_GPR_RDX] = descs[3];
}

static void
vmx_inkernel_handle_cpuid(struct nvmm_machine *mach, struct nvmm_cpu *vcpu,
    uint64_t eax, uint64_t ecx)
{
	struct vmx_cpudata *cpudata = vcpu->cpudata;
	unsigned int ncpus;
	uint64_t cr4;

	/*
	 * `If a value entered for CPUID.EAX is higher than the maximum
	 *  input value for basic or extended function for that
	 *  processor then the data for the highest basic information
	 *  leaf is returned.'
	 *
	 * --Intel 64 and IA-32 Architectures Software Developer's
	 *   Manual, Vol. 2A, Order Number: 325383-077US, April 2022,
	 *   Sec. 3.2 `Instructions (A-L)', CPUID--CPU Identification,
	 *   p. 3-214.
	 *
	 * We take the same to hold for the hypervisor range,
	 * 0x40000000-0x4fffffff.
	 *
	 * (Sync with nvmm_x86_svm.c.)
	 */
	if (eax < 0x40000000) {		/* basic CPUID range */
		if (__predict_false(eax > vmx_cpuid_max_basic)) {
			eax = vmx_cpuid_max_basic;
			vmx_inkernel_exec_cpuid(cpudata, eax, ecx);
		}
	} else if (eax < 0x80000000) {	/* hypervisor CPUID range */
		if (__predict_false(eax > VMX_CPUID_MAX_HYPERVISOR)) {
			eax = vmx_cpuid_max_basic;
			vmx_inkernel_exec_cpuid(cpudata, eax, ecx);
		}
	} else {			/* extended CPUID range */
		if (__predict_false(eax > vmx_cpuid_max_extended)) {
			eax = vmx_cpuid_max_basic;
			vmx_inkernel_exec_cpuid(cpudata, eax, ecx);
		}
	}

	switch (eax) {

	/*
	 * basic CPUID range
	 */
	case 0x00000000:
		cpudata->gprs[NVMM_X64_GPR_RAX] = vmx_cpuid_max_basic;
		break;
	case 0x00000001:
		cpudata->gprs[NVMM_X64_GPR_RAX] &= nvmm_cpuid_00000001.eax;

		cpudata->gprs[NVMM_X64_GPR_RBX] &= ~CPUID_LOCAL_APIC_ID;
		cpudata->gprs[NVMM_X64_GPR_RBX] |= __SHIFTIN(vcpu->cpuid,
		    CPUID_LOCAL_APIC_ID);

		cpudata->gprs[NVMM_X64_GPR_RCX] &= nvmm_cpuid_00000001.ecx;
		cpudata->gprs[NVMM_X64_GPR_RCX] |= CPUID2_RAZ;
		if (vmx_procbased_ctls2 & PROC_CTLS2_INVPCID_ENABLE) {
			cpudata->gprs[NVMM_X64_GPR_RCX] |= CPUID2_PCID;
		}

		cpudata->gprs[NVMM_X64_GPR_RDX] &= nvmm_cpuid_00000001.edx;

		/* CPUID2_OSXSAVE depends on CR4. */
		cr4 = vmx_vmread(VMCS_GUEST_CR4);
		if (!(cr4 & CR4_OSXSAVE)) {
			cpudata->gprs[NVMM_X64_GPR_RCX] &= ~CPUID2_OSXSAVE;
		}
		break;
	case 0x00000002:
		break;
	case 0x00000003:
		cpudata->gprs[NVMM_X64_GPR_RAX] = 0;
		cpudata->gprs[NVMM_X64_GPR_RBX] = 0;
		cpudata->gprs[NVMM_X64_GPR_RCX] = 0;
		cpudata->gprs[NVMM_X64_GPR_RDX] = 0;
		break;
	case 0x00000004: /* Deterministic Cache Parameters */
		break; /* TODO? */
	case 0x00000005: /* MONITOR/MWAIT */
	case 0x00000006: /* Thermal and Power Management */
		cpudata->gprs[NVMM_X64_GPR_RAX] = 0;
		cpudata->gprs[NVMM_X64_GPR_RBX] = 0;
		cpudata->gprs[NVMM_X64_GPR_RCX] = 0;
		cpudata->gprs[NVMM_X64_GPR_RDX] = 0;
		break;
	case 0x00000007: /* Structured Extended Feature Flags Enumeration */
		switch (ecx) {
		case 0:
			cpudata->gprs[NVMM_X64_GPR_RAX] = 0;
			cpudata->gprs[NVMM_X64_GPR_RBX] &= nvmm_cpuid_00000007.ebx;
			cpudata->gprs[NVMM_X64_GPR_RCX] &= nvmm_cpuid_00000007.ecx;
			cpudata->gprs[NVMM_X64_GPR_RDX] &= nvmm_cpuid_00000007.edx;
			if (vmx_procbased_ctls2 & PROC_CTLS2_INVPCID_ENABLE) {
				cpudata->gprs[NVMM_X64_GPR_RBX] |= CPUID_SEF_INVPCID;
			}
			break;
		default:
			cpudata->gprs[NVMM_X64_GPR_RAX] = 0;
			cpudata->gprs[NVMM_X64_GPR_RBX] = 0;
			cpudata->gprs[NVMM_X64_GPR_RCX] = 0;
			cpudata->gprs[NVMM_X64_GPR_RDX] = 0;
			break;
		}
		break;
	case 0x00000008: /* Empty */
	case 0x00000009: /* Direct Cache Access Information */
		cpudata->gprs[NVMM_X64_GPR_RAX] = 0;
		cpudata->gprs[NVMM_X64_GPR_RBX] = 0;
		cpudata->gprs[NVMM_X64_GPR_RCX] = 0;
		cpudata->gprs[NVMM_X64_GPR_RDX] = 0;
		break;
	case 0x0000000A: /* Architectural Performance Monitoring */
		cpudata->gprs[NVMM_X64_GPR_RAX] = 0;
		cpudata->gprs[NVMM_X64_GPR_RBX] = 0;
		cpudata->gprs[NVMM_X64_GPR_RCX] = 0;
		cpudata->gprs[NVMM_X64_GPR_RDX] = 0;
		break;
	case 0x0000000B: /* Extended Topology Enumeration */
		switch (ecx) {
		case 0: /* Threads */
			cpudata->gprs[NVMM_X64_GPR_RAX] = 0;
			cpudata->gprs[NVMM_X64_GPR_RBX] = 0;
			cpudata->gprs[NVMM_X64_GPR_RCX] =
			    __SHIFTIN(ecx, CPUID_TOP_LVLNUM) |
			    __SHIFTIN(CPUID_TOP_LVLTYPE_SMT, CPUID_TOP_LVLTYPE);
			cpudata->gprs[NVMM_X64_GPR_RDX] = vcpu->cpuid;
			break;
		case 1: /* Cores */
			ncpus = os_atomic_load_uint(&mach->ncpus);
			cpudata->gprs[NVMM_X64_GPR_RAX] = ilog2(ncpus);
			cpudata->gprs[NVMM_X64_GPR_RBX] = ncpus;
			cpudata->gprs[NVMM_X64_GPR_RCX] =
			    __SHIFTIN(ecx, CPUID_TOP_LVLNUM) |
			    __SHIFTIN(CPUID_TOP_LVLTYPE_CORE, CPUID_TOP_LVLTYPE);
			cpudata->gprs[NVMM_X64_GPR_RDX] = vcpu->cpuid;
			break;
		default:
			cpudata->gprs[NVMM_X64_GPR_RAX] = 0;
			cpudata->gprs[NVMM_X64_GPR_RBX] = 0;
			cpudata->gprs[NVMM_X64_GPR_RCX] = 0; /* LVLTYPE_INVAL */
			cpudata->gprs[NVMM_X64_GPR_RDX] = 0;
			break;
		}
		break;
	case 0x0000000C: /* Empty */
		cpudata->gprs[NVMM_X64_GPR_RAX] = 0;
		cpudata->gprs[NVMM_X64_GPR_RBX] = 0;
		cpudata->gprs[NVMM_X64_GPR_RCX] = 0;
		cpudata->gprs[NVMM_X64_GPR_RDX] = 0;
		break;
	case 0x0000000D: /* Processor Extended State Enumeration */
		if (cpudata->xcr0_mask == 0) {
			cpudata->gprs[NVMM_X64_GPR_RAX] = 0;
			cpudata->gprs[NVMM_X64_GPR_RBX] = 0;
			cpudata->gprs[NVMM_X64_GPR_RCX] = 0;
			cpudata->gprs[NVMM_X64_GPR_RDX] = 0;
			break;
		}
		switch (ecx) {
		case 0:
			cpudata->gprs[NVMM_X64_GPR_RAX] =
			    cpudata->xcr0_mask & 0xFFFFFFFF;
			cpudata->gprs[NVMM_X64_GPR_RBX] =
			    nvmm_x86_xsave_size(cpudata->gxcr0);
			cpudata->gprs[NVMM_X64_GPR_RCX] =
			    nvmm_x86_xsave_size(cpudata->xcr0_mask);
			cpudata->gprs[NVMM_X64_GPR_RDX] =
			    cpudata->xcr0_mask >> 32;
			break;
		case 1:
			cpudata->gprs[NVMM_X64_GPR_RAX] &=
			    (CPUID_PES1_XSAVEOPT | CPUID_PES1_XSAVEC |
			     CPUID_PES1_XGETBV);
			cpudata->gprs[NVMM_X64_GPR_RBX] = 0;
			cpudata->gprs[NVMM_X64_GPR_RCX] = 0;
			cpudata->gprs[NVMM_X64_GPR_RDX] = 0;
			break;
		case 2 ... 62:
			/*
			 * CPUID[EAX=0x0d,ECX=n], 2 <= n <= 62: size
			 * and offset of nth component in XSAVE area.
			 * If the nth bit of XCR0 is disabled in the
			 * vCPU configuration, we return all-zero
			 * instead.
			 */
			if ((cpudata->xcr0_mask & __BIT(ecx)) == 0) {
				cpudata->gprs[NVMM_X64_GPR_RAX] = 0;
				cpudata->gprs[NVMM_X64_GPR_RBX] = 0;
				cpudata->gprs[NVMM_X64_GPR_RCX] = 0;
				cpudata->gprs[NVMM_X64_GPR_RDX] = 0;
			}
			break;
		default:
			cpudata->gprs[NVMM_X64_GPR_RAX] = 0;
			cpudata->gprs[NVMM_X64_GPR_RBX] = 0;
			cpudata->gprs[NVMM_X64_GPR_RCX] = 0;
			cpudata->gprs[NVMM_X64_GPR_RDX] = 0;
			break;
		}
		break;
	case 0x0000000E: /* Empty */
	case 0x0000000F: /* Intel RDT Monitoring Enumeration */
	case 0x00000010: /* Intel RDT Allocation Enumeration */
		cpudata->gprs[NVMM_X64_GPR_RAX] = 0;
		cpudata->gprs[NVMM_X64_GPR_RBX] = 0;
		cpudata->gprs[NVMM_X64_GPR_RCX] = 0;
		cpudata->gprs[NVMM_X64_GPR_RDX] = 0;
		break;
	case 0x00000011: /* Empty */
	case 0x00000012: /* Intel SGX Capability Enumeration */
	case 0x00000013: /* Empty */
	case 0x00000014: /* Intel Processor Trace Enumeration */
		cpudata->gprs[NVMM_X64_GPR_RAX] = 0;
		cpudata->gprs[NVMM_X64_GPR_RBX] = 0;
		cpudata->gprs[NVMM_X64_GPR_RCX] = 0;
		cpudata->gprs[NVMM_X64_GPR_RDX] = 0;
		break;
	case 0x00000015: /* TSC and Nominal Core Crystal Clock Information */
	case 0x00000016: /* Processor Frequency Information */
		break;

	/*
	 * hypervisor CPUID range
	 */
	case 0x40000000: /* Hypervisor Information */
		cpudata->gprs[NVMM_X64_GPR_RAX] = VMX_CPUID_MAX_HYPERVISOR;
		cpudata->gprs[NVMM_X64_GPR_RBX] = 0;
		cpudata->gprs[NVMM_X64_GPR_RCX] = 0;
		cpudata->gprs[NVMM_X64_GPR_RDX] = 0;
		memcpy(&cpudata->gprs[NVMM_X64_GPR_RBX], "___ ", 4);
		memcpy(&cpudata->gprs[NVMM_X64_GPR_RCX], "NVMM", 4);
		memcpy(&cpudata->gprs[NVMM_X64_GPR_RDX], " ___", 4);
		break;

	/*
	 * extended CPUID range
	 */
	case 0x80000000:
		cpudata->gprs[NVMM_X64_GPR_RAX] = vmx_cpuid_max_extended;
		break;
	case 0x80000001:
		cpudata->gprs[NVMM_X64_GPR_RAX] &= nvmm_cpuid_80000001.eax;
		cpudata->gprs[NVMM_X64_GPR_RBX] &= nvmm_cpuid_80000001.ebx;
		cpudata->gprs[NVMM_X64_GPR_RCX] &= nvmm_cpuid_80000001.ecx;
		cpudata->gprs[NVMM_X64_GPR_RDX] &= nvmm_cpuid_80000001.edx;
		break;
	case 0x80000002: /* Processor Brand String */
	case 0x80000003: /* Processor Brand String */
	case 0x80000004: /* Processor Brand String */
	case 0x80000005: /* Reserved Zero */
	case 0x80000006: /* Cache Information */
		break;
	case 0x80000007: /* TSC Information */
		cpudata->gprs[NVMM_X64_GPR_RAX] &= nvmm_cpuid_80000007.eax;
		cpudata->gprs[NVMM_X64_GPR_RBX] &= nvmm_cpuid_80000007.ebx;
		cpudata->gprs[NVMM_X64_GPR_RCX] &= nvmm_cpuid_80000007.ecx;
		cpudata->gprs[NVMM_X64_GPR_RDX] &= nvmm_cpuid_80000007.edx;
		break;
	case 0x80000008: /* Address Sizes */
		cpudata->gprs[NVMM_X64_GPR_RAX] &= nvmm_cpuid_80000008.eax;
		cpudata->gprs[NVMM_X64_GPR_RBX] &= nvmm_cpuid_80000008.ebx;
		cpudata->gprs[NVMM_X64_GPR_RCX] &= nvmm_cpuid_80000008.ecx;
		cpudata->gprs[NVMM_X64_GPR_RDX] &= nvmm_cpuid_80000008.edx;
		break;

	default:
		break;
	}
}

static void
vmx_exit_cpuid(struct nvmm_machine *mach, struct nvmm_cpu *vcpu,
    struct nvmm_vcpu_exit *exit)
{
	struct vmx_cpudata *cpudata = vcpu->cpudata;
	struct nvmm_vcpu_conf_cpuid *cpuid;
	uint64_t eax, ecx;
	size_t i;

	eax = cpudata->gprs[NVMM_X64_GPR_RAX];
	ecx = cpudata->gprs[NVMM_X64_GPR_RCX];
	vmx_inkernel_exec_cpuid(cpudata, eax, ecx);
	vmx_inkernel_handle_cpuid(mach, vcpu, eax, ecx);

	for (i = 0; i < VMX_NCPUIDS; i++) {
		if (!cpudata->cpuidpresent[i]) {
			continue;
		}
		cpuid = &cpudata->cpuid[i];
		if (cpuid->leaf != eax) {
			continue;
		}

		if (cpuid->exit) {
			vmx_exit_insn(exit, NVMM_VCPU_EXIT_CPUID);
			return;
		}
		OS_ASSERT(cpuid->mask);

		/* del */
		cpudata->gprs[NVMM_X64_GPR_RAX] &= ~cpuid->u.mask.del.eax;
		cpudata->gprs[NVMM_X64_GPR_RBX] &= ~cpuid->u.mask.del.ebx;
		cpudata->gprs[NVMM_X64_GPR_RCX] &= ~cpuid->u.mask.del.ecx;
		cpudata->gprs[NVMM_X64_GPR_RDX] &= ~cpuid->u.mask.del.edx;

		/* set */
		cpudata->gprs[NVMM_X64_GPR_RAX] |= cpuid->u.mask.set.eax;
		cpudata->gprs[NVMM_X64_GPR_RBX] |= cpuid->u.mask.set.ebx;
		cpudata->gprs[NVMM_X64_GPR_RCX] |= cpuid->u.mask.set.ecx;
		cpudata->gprs[NVMM_X64_GPR_RDX] |= cpuid->u.mask.set.edx;

		break;
	}

	vmx_inkernel_advance();
	exit->reason = NVMM_VCPU_EXIT_NONE;
}

#define VMX_QUAL_CR_NUM		__BITS(3,0)
#define VMX_QUAL_CR_TYPE	__BITS(5,4)
#define		CR_TYPE_WRITE	0
#define		CR_TYPE_READ	1
#define		CR_TYPE_CLTS	2
#define		CR_TYPE_LMSW	3
#define VMX_QUAL_CR_LMSW_OPMEM	__BIT(6)
#define VMX_QUAL_CR_GPR		__BITS(11,8)
#define VMX_QUAL_CR_LMSW_SRC	__BIT(31,16)

static int
vmx_inkernel_handle_cr0(struct nvmm_machine *mach, struct nvmm_cpu *vcpu,
    uint64_t qual)
{
	struct vmx_cpudata *cpudata = vcpu->cpudata;
	uint64_t type, gpr, oldcr0, realcr0, fakecr0;
	uint64_t efer, ctls1;

	type = __SHIFTOUT(qual, VMX_QUAL_CR_TYPE);
	if (type != CR_TYPE_WRITE) {
		return -1;
	}

	gpr = __SHIFTOUT(qual, VMX_QUAL_CR_GPR);
	OS_ASSERT(gpr < 16);

	if (gpr == NVMM_X64_GPR_RSP) {
		fakecr0 = vmx_vmread(VMCS_GUEST_RSP);
	} else {
		fakecr0 = cpudata->gprs[gpr];
	}

	/*
	 * fakecr0 is the value the guest believes is in %cr0. realcr0 is the
	 * actual value in %cr0.
	 *
	 * In fakecr0 we must force CR0_ET to 1.
	 *
	 * In realcr0 we must force CR0_NW and CR0_CD to 0, and CR0_ET and
	 * CR0_NE to 1.
	 */
	fakecr0 |= CR0_ET;
	realcr0 = (fakecr0 & ~CR0_STATIC_MASK) | CR0_ET | CR0_NE;

	if (vmx_check_cr(realcr0, vmx_cr0_fixed0, vmx_cr0_fixed1) == -1) {
		return -1;
	}

	/*
	 * XXX Handle 32bit PAE paging, need to set PDPTEs, fetched manually
	 * from CR3.
	 */

	if (realcr0 & CR0_PG) {
		ctls1 = vmx_vmread(VMCS_ENTRY_CTLS);
		efer = vmx_vmread(VMCS_GUEST_IA32_EFER);
		if (efer & EFER_LME) {
			ctls1 |= ENTRY_CTLS_LONG_MODE;
			efer |= EFER_LMA;
		} else {
			ctls1 &= ~ENTRY_CTLS_LONG_MODE;
			efer &= ~EFER_LMA;
		}
		vmx_vmwrite(VMCS_GUEST_IA32_EFER, efer);
		vmx_vmwrite(VMCS_ENTRY_CTLS, ctls1);
	}

	oldcr0 = (vmx_vmread(VMCS_CR0_SHADOW) & CR0_STATIC_MASK) |
	    (vmx_vmread(VMCS_GUEST_CR0) & ~CR0_STATIC_MASK);
	if ((oldcr0 ^ fakecr0) & CR0_TLB_FLUSH) {
		cpudata->gtlb_want_flush = true;
	}

	vmx_vmwrite(VMCS_CR0_SHADOW, fakecr0);
	vmx_vmwrite(VMCS_GUEST_CR0, realcr0);
	vmx_inkernel_advance();
	return 0;
}

static int
vmx_inkernel_handle_cr4(struct nvmm_machine *mach, struct nvmm_cpu *vcpu,
    uint64_t qual)
{
	struct vmx_cpudata *cpudata = vcpu->cpudata;
	uint64_t type, gpr, oldcr4, cr4;

	type = __SHIFTOUT(qual, VMX_QUAL_CR_TYPE);
	if (type != CR_TYPE_WRITE) {
		return -1;
	}

	gpr = __SHIFTOUT(qual, VMX_QUAL_CR_GPR);
	OS_ASSERT(gpr < 16);

	if (gpr == NVMM_X64_GPR_RSP) {
		gpr = vmx_vmread(VMCS_GUEST_RSP);
	} else {
		gpr = cpudata->gprs[gpr];
	}

	if (gpr & CR4_INVALID) {
		return -1;
	}
	cr4 = gpr | CR4_VMXE;
	if (vmx_check_cr(cr4, vmx_cr4_fixed0, vmx_cr4_fixed1) == -1) {
		return -1;
	}

	oldcr4 = vmx_vmread(VMCS_GUEST_CR4);
	if ((oldcr4 ^ gpr) & CR4_TLB_FLUSH) {
		cpudata->gtlb_want_flush = true;
	}

	vmx_vmwrite(VMCS_GUEST_CR4, cr4);
	vmx_inkernel_advance();
	return 0;
}

static int
vmx_inkernel_handle_cr8(struct nvmm_machine *mach, struct nvmm_cpu *vcpu,
    uint64_t qual, struct nvmm_vcpu_exit *exit)
{
	struct vmx_cpudata *cpudata = vcpu->cpudata;
	uint64_t type, gpr;
	bool write;

	type = __SHIFTOUT(qual, VMX_QUAL_CR_TYPE);
	if (type == CR_TYPE_WRITE) {
		write = true;
	} else if (type == CR_TYPE_READ) {
		write = false;
	} else {
		return -1;
	}

	gpr = __SHIFTOUT(qual, VMX_QUAL_CR_GPR);
	OS_ASSERT(gpr < 16);

	if (write) {
		if (gpr == NVMM_X64_GPR_RSP) {
			cpudata->gcr8 = vmx_vmread(VMCS_GUEST_RSP);
		} else {
			cpudata->gcr8 = cpudata->gprs[gpr];
		}
		if (cpudata->tpr.exit_changed) {
			exit->reason = NVMM_VCPU_EXIT_TPR_CHANGED;
		}
	} else {
		if (gpr == NVMM_X64_GPR_RSP) {
			vmx_vmwrite(VMCS_GUEST_RSP, cpudata->gcr8);
		} else {
			cpudata->gprs[gpr] = cpudata->gcr8;
		}
	}

	vmx_inkernel_advance();
	return 0;
}

static void
vmx_exit_cr(struct nvmm_machine *mach, struct nvmm_cpu *vcpu,
    struct nvmm_vcpu_exit *exit)
{
	uint64_t qual;
	int ret;

	exit->reason = NVMM_VCPU_EXIT_NONE;

	qual = vmx_vmread(VMCS_EXIT_QUALIFICATION);

	switch (__SHIFTOUT(qual, VMX_QUAL_CR_NUM)) {
	case 0:
		ret = vmx_inkernel_handle_cr0(mach, vcpu, qual);
		break;
	case 4:
		ret = vmx_inkernel_handle_cr4(mach, vcpu, qual);
		break;
	case 8:
		ret = vmx_inkernel_handle_cr8(mach, vcpu, qual, exit);
		break;
	default:
		ret = -1;
		break;
	}

	if (ret == -1) {
		vmx_inject_gp(vcpu);
	}
}

static const uint64_t msr_ignore_list[] = {
	MSR_BIOS_SIGN,
	MSR_IA32_PLATFORM_ID
};

static bool
vmx_inkernel_handle_msr(struct nvmm_machine *mach __unused, struct nvmm_cpu *vcpu,
    struct nvmm_vcpu_exit *exit)
{
	struct vmx_cpudata *cpudata = vcpu->cpudata;
	uint64_t val;
	size_t i;

	if (exit->reason == NVMM_VCPU_EXIT_RDMSR) {
		if (exit->u.rdmsr.msr == MSR_PAT) {
			val = vmx_vmread(VMCS_GUEST_IA32_PAT);
			cpudata->gprs[NVMM_X64_GPR_RAX] = (val & 0xFFFFFFFF);
			cpudata->gprs[NVMM_X64_GPR_RDX] = (val >> 32);
			goto handled;
		}
		if (exit->u.rdmsr.msr == MSR_IA32_MISC_ENABLE) {
			val = cpudata->gmsr_misc_enable;
			cpudata->gprs[NVMM_X64_GPR_RAX] = (val & 0xFFFFFFFF);
			cpudata->gprs[NVMM_X64_GPR_RDX] = (val >> 32);
			goto handled;
		}
		if (exit->u.rdmsr.msr == MSR_IA32_ARCH_CAPABILITIES) {
			u_int descs[4];
			if (cpu_high < 7) {
				goto error;
			}
			x86_cpuid2(7, 0, descs);
			if (!(descs[3] & CPUID_SEF_ARCH_CAP)) {
				goto error;
			}
			val = rdmsr(MSR_IA32_ARCH_CAPABILITIES);
			val &= (IA32_ARCH_RDCL_NO |
			    IA32_ARCH_SSB_NO |
			    IA32_ARCH_MDS_NO |
			    IA32_ARCH_TAA_NO);
			cpudata->gprs[NVMM_X64_GPR_RAX] = (val & 0xFFFFFFFF);
			cpudata->gprs[NVMM_X64_GPR_RDX] = (val >> 32);
			goto handled;
		}
		for (i = 0; i < nitems(msr_ignore_list); i++) {
			if (msr_ignore_list[i] != exit->u.rdmsr.msr)
				continue;
			val = 0;
			cpudata->gprs[NVMM_X64_GPR_RAX] = (val & 0xFFFFFFFF);
			cpudata->gprs[NVMM_X64_GPR_RDX] = (val >> 32);
			goto handled;
		}
	} else {
		if (exit->u.wrmsr.msr == MSR_TSC) {
			cpudata->gtsc = exit->u.wrmsr.val;
			cpudata->gtsc_want_update = true;
			goto handled;
		}
		if (exit->u.wrmsr.msr == MSR_PAT) {
			val = exit->u.wrmsr.val;
			if (__predict_false(!nvmm_x86_pat_validate(val))) {
				goto error;
			}
			vmx_vmwrite(VMCS_GUEST_IA32_PAT, val);
			goto handled;
		}
		if (exit->u.wrmsr.msr == MSR_IA32_MISC_ENABLE) {
			/* Don't care. */
			goto handled;
		}
		for (i = 0; i < nitems(msr_ignore_list); i++) {
			if (msr_ignore_list[i] != exit->u.wrmsr.msr)
				continue;
			goto handled;
		}
	}

	return false;

handled:
	vmx_inkernel_advance();
	return true;

error:
	vmx_inject_gp(vcpu);
	return true;
}

static void
vmx_exit_rdmsr(struct nvmm_machine *mach __unused, struct nvmm_cpu *vcpu,
    struct nvmm_vcpu_exit *exit)
{
	struct vmx_cpudata *cpudata = vcpu->cpudata;
	uint64_t inslen, rip;

	exit->reason = NVMM_VCPU_EXIT_RDMSR;
	exit->u.rdmsr.msr = (cpudata->gprs[NVMM_X64_GPR_RCX] & 0xFFFFFFFF);

	if (vmx_inkernel_handle_msr(mach, vcpu, exit)) {
		exit->reason = NVMM_VCPU_EXIT_NONE;
		return;
	}

	inslen = vmx_vmread(VMCS_EXIT_INSTRUCTION_LENGTH);
	rip = vmx_vmread(VMCS_GUEST_RIP);
	exit->u.rdmsr.npc = rip + inslen;

	vmx_vcpu_state_provide(vcpu, NVMM_X64_STATE_GPRS);
}

static void
vmx_exit_wrmsr(struct nvmm_machine *mach __unused, struct nvmm_cpu *vcpu,
    struct nvmm_vcpu_exit *exit)
{
	struct vmx_cpudata *cpudata = vcpu->cpudata;
	uint64_t rdx, rax, inslen, rip;

	rdx = cpudata->gprs[NVMM_X64_GPR_RDX];
	rax = cpudata->gprs[NVMM_X64_GPR_RAX];

	exit->reason = NVMM_VCPU_EXIT_WRMSR;
	exit->u.wrmsr.msr = (cpudata->gprs[NVMM_X64_GPR_RCX] & 0xFFFFFFFF);
	exit->u.wrmsr.val = (rdx << 32) | (rax & 0xFFFFFFFF);

	if (vmx_inkernel_handle_msr(mach, vcpu, exit)) {
		exit->reason = NVMM_VCPU_EXIT_NONE;
		return;
	}

	inslen = vmx_vmread(VMCS_EXIT_INSTRUCTION_LENGTH);
	rip = vmx_vmread(VMCS_GUEST_RIP);
	exit->u.wrmsr.npc = rip + inslen;

	vmx_vcpu_state_provide(vcpu, NVMM_X64_STATE_GPRS);
}

static void
vmx_exit_xsetbv(struct nvmm_machine *mach __unused, struct nvmm_cpu *vcpu,
    struct nvmm_vcpu_exit *exit)
{
	struct vmx_cpudata *cpudata = vcpu->cpudata;
	uint64_t val;

	exit->reason = NVMM_VCPU_EXIT_NONE;

	val = (cpudata->gprs[NVMM_X64_GPR_RDX] << 32) |
	    (cpudata->gprs[NVMM_X64_GPR_RAX] & 0xFFFFFFFF);

	if (__predict_false(cpudata->gprs[NVMM_X64_GPR_RCX] != 0)) {
		goto error;
	} else if (__predict_false(cpudata->xcr0_mask == 0)) {
		goto error;
	} else if (__predict_false(!nvmm_x86_xcr0_valid(val,
		    cpudata->xcr0_mask))) {
		goto error;
	}

	KASSERTMSG(nvmm_x86_xcr0_valid(val, cpudata->xcr0_mask),
	    "val=0x%"PRIx64" xcr0_mask=0x%"PRIx64" (gxcr0=0x%"PRIx64")",
	    val, cpudata->xcr0_mask, cpudata->gxcr0);
	cpudata->gxcr0 = val;

	vmx_inkernel_advance();
	return;

error:
	vmx_inject_gp(vcpu);
}



/* -------------------------------------------------------------------------- */

/* NetBSD's vmx_memalloc/vmx_memfree: physically contiguous, zeroed. */
static int
vmx_memalloc(paddr_t *pa, vaddr_t *va, size_t npages)
{
	return os_contigpa_zalloc(pa, va, npages);
}

static void
vmx_memfree(paddr_t pa, vaddr_t va, size_t npages)
{
	os_contigpa_free(pa, va, npages);
}

static void
vmx_vcpu_msr_allow(uint8_t *bitmap, uint64_t msr, bool read, bool write)
{
	uint64_t byte;
	uint8_t bitoff;

	if (msr < 0x00002000) {
		/* Range 1 */
		byte = ((msr - 0x00000000) / 8) + 0;
	} else if (msr >= 0xC0000000 && msr < 0xC0002000) {
		/* Range 2 */
		byte = ((msr - 0xC0000000) / 8) + 1024;
	} else {
		panic("%s: wrong range", __func__);
	}

	bitoff = (msr & 0x7);

	if (read) {
		bitmap[byte] &= ~__BIT(bitoff);
	}
	if (write) {
		bitmap[2048 + byte] &= ~__BIT(bitoff);
	}
}

#define VMX_SEG_ATTRIB_TYPE		__BITS(3,0)
#define VMX_SEG_ATTRIB_S		__BIT(4)
#define VMX_SEG_ATTRIB_DPL		__BITS(6,5)
#define VMX_SEG_ATTRIB_P		__BIT(7)
#define VMX_SEG_ATTRIB_AVL		__BIT(12)
#define VMX_SEG_ATTRIB_L		__BIT(13)
#define VMX_SEG_ATTRIB_DEF		__BIT(14)
#define VMX_SEG_ATTRIB_G		__BIT(15)
#define VMX_SEG_ATTRIB_UNUSABLE		__BIT(16)

static void
vmx_vcpu_setstate_seg(const struct nvmm_x64_state_seg *segs, int idx)
{
	uint64_t attrib;

	attrib =
	    __SHIFTIN(segs[idx].attrib.type, VMX_SEG_ATTRIB_TYPE) |
	    __SHIFTIN(segs[idx].attrib.s, VMX_SEG_ATTRIB_S) |
	    __SHIFTIN(segs[idx].attrib.dpl, VMX_SEG_ATTRIB_DPL) |
	    __SHIFTIN(segs[idx].attrib.p, VMX_SEG_ATTRIB_P) |
	    __SHIFTIN(segs[idx].attrib.avl, VMX_SEG_ATTRIB_AVL) |
	    __SHIFTIN(segs[idx].attrib.l, VMX_SEG_ATTRIB_L) |
	    __SHIFTIN(segs[idx].attrib.def, VMX_SEG_ATTRIB_DEF) |
	    __SHIFTIN(segs[idx].attrib.g, VMX_SEG_ATTRIB_G) |
	    (!segs[idx].attrib.p ? VMX_SEG_ATTRIB_UNUSABLE : 0);

	if (idx != NVMM_X64_SEG_GDT && idx != NVMM_X64_SEG_IDT) {
		vmx_vmwrite(vmx_guest_segs[idx].selector, segs[idx].selector);
		vmx_vmwrite(vmx_guest_segs[idx].attrib, attrib);
	}
	vmx_vmwrite(vmx_guest_segs[idx].limit, segs[idx].limit);
	vmx_vmwrite(vmx_guest_segs[idx].base, segs[idx].base);
}

static void
vmx_vcpu_getstate_seg(struct nvmm_x64_state_seg *segs, int idx)
{
	uint64_t selector = 0, attrib = 0, base, limit;

	if (idx != NVMM_X64_SEG_GDT && idx != NVMM_X64_SEG_IDT) {
		selector = vmx_vmread(vmx_guest_segs[idx].selector);
		attrib = vmx_vmread(vmx_guest_segs[idx].attrib);
	}
	limit = vmx_vmread(vmx_guest_segs[idx].limit);
	base = vmx_vmread(vmx_guest_segs[idx].base);

	segs[idx].selector = selector;
	segs[idx].limit = limit;
	segs[idx].base = base;
	segs[idx].attrib.type = __SHIFTOUT(attrib, VMX_SEG_ATTRIB_TYPE);
	segs[idx].attrib.s = __SHIFTOUT(attrib, VMX_SEG_ATTRIB_S);
	segs[idx].attrib.dpl = __SHIFTOUT(attrib, VMX_SEG_ATTRIB_DPL);
	segs[idx].attrib.p = __SHIFTOUT(attrib, VMX_SEG_ATTRIB_P);
	segs[idx].attrib.avl = __SHIFTOUT(attrib, VMX_SEG_ATTRIB_AVL);
	segs[idx].attrib.l = __SHIFTOUT(attrib, VMX_SEG_ATTRIB_L);
	segs[idx].attrib.def = __SHIFTOUT(attrib, VMX_SEG_ATTRIB_DEF);
	segs[idx].attrib.g = __SHIFTOUT(attrib, VMX_SEG_ATTRIB_G);
	if (attrib & VMX_SEG_ATTRIB_UNUSABLE) {
		segs[idx].attrib.p = 0;
	}
}

static inline bool
vmx_state_tlb_flush(const struct nvmm_x64_state *state, uint64_t flags)
{
	uint64_t cr0, cr3, cr4, efer;

	if (flags & NVMM_X64_STATE_CRS) {
		cr0 = vmx_vmread(VMCS_GUEST_CR0);
		if ((cr0 ^ state->crs[NVMM_X64_CR_CR0]) & CR0_TLB_FLUSH) {
			return true;
		}
		cr3 = vmx_vmread(VMCS_GUEST_CR3);
		if (cr3 != state->crs[NVMM_X64_CR_CR3]) {
			return true;
		}
		cr4 = vmx_vmread(VMCS_GUEST_CR4);
		if ((cr4 ^ state->crs[NVMM_X64_CR_CR4]) & CR4_TLB_FLUSH) {
			return true;
		}
	}

	if (flags & NVMM_X64_STATE_MSRS) {
		efer = vmx_vmread(VMCS_GUEST_IA32_EFER);
		if ((efer ^
		     state->msrs[NVMM_X64_MSR_EFER]) & EFER_TLB_FLUSH) {
			return true;
		}
	}

	return false;
}

static void
vmx_vcpu_setstate(struct nvmm_cpu *vcpu)
{
	struct nvmm_comm_page *comm = vcpu->comm;
	const struct nvmm_x64_state *state = &comm->state;
	struct vmx_cpudata *cpudata = vcpu->cpudata;
	struct nvmm_x64_state_fpu *fpustate;
	uint64_t ctls1, intstate;
	uint64_t flags;

	flags = comm->state_wanted;

	vmx_vmcs_enter(vcpu);

	if (vmx_state_tlb_flush(state, flags)) {
		cpudata->gtlb_want_flush = true;
	}

	if (flags & NVMM_X64_STATE_SEGS) {
		vmx_vcpu_setstate_seg(state->segs, NVMM_X64_SEG_CS);
		vmx_vcpu_setstate_seg(state->segs, NVMM_X64_SEG_DS);
		vmx_vcpu_setstate_seg(state->segs, NVMM_X64_SEG_ES);
		vmx_vcpu_setstate_seg(state->segs, NVMM_X64_SEG_FS);
		vmx_vcpu_setstate_seg(state->segs, NVMM_X64_SEG_GS);
		vmx_vcpu_setstate_seg(state->segs, NVMM_X64_SEG_SS);
		vmx_vcpu_setstate_seg(state->segs, NVMM_X64_SEG_GDT);
		vmx_vcpu_setstate_seg(state->segs, NVMM_X64_SEG_IDT);
		vmx_vcpu_setstate_seg(state->segs, NVMM_X64_SEG_LDT);
		vmx_vcpu_setstate_seg(state->segs, NVMM_X64_SEG_TR);
	}

	CTASSERT(sizeof(cpudata->gprs) == sizeof(state->gprs));
	if (flags & NVMM_X64_STATE_GPRS) {
		memcpy(cpudata->gprs, state->gprs, sizeof(state->gprs));

		vmx_vmwrite(VMCS_GUEST_RIP, state->gprs[NVMM_X64_GPR_RIP]);
		vmx_vmwrite(VMCS_GUEST_RSP, state->gprs[NVMM_X64_GPR_RSP]);
		vmx_vmwrite(VMCS_GUEST_RFLAGS, state->gprs[NVMM_X64_GPR_RFLAGS]);
	}

	if (flags & NVMM_X64_STATE_CRS) {
		/*
		 * CR0_ET must be 1 both in the shadow and the real register.
		 * CR0_NE must be 1 in the real register.
		 * CR0_NW and CR0_CD must be 0 in the real register.
		 */
		vmx_vmwrite(VMCS_CR0_SHADOW,
		    (state->crs[NVMM_X64_CR_CR0] & CR0_STATIC_MASK) |
		    CR0_ET);
		vmx_vmwrite(VMCS_GUEST_CR0,
		    (state->crs[NVMM_X64_CR_CR0] & ~CR0_STATIC_MASK) |
		    CR0_ET | CR0_NE);

		cpudata->gcr2 = state->crs[NVMM_X64_CR_CR2];

		/* XXX We are not handling PDPTE here. */
		vmx_vmwrite(VMCS_GUEST_CR3, state->crs[NVMM_X64_CR_CR3]);

		/* CR4_VMXE is mandatory. */
		vmx_vmwrite(VMCS_GUEST_CR4,
		    (state->crs[NVMM_X64_CR_CR4] & CR4_VALID) | CR4_VMXE);

		cpudata->gcr8 = state->crs[NVMM_X64_CR_CR8];

		if (cpudata->xcr0_mask != 0) {
			const uint64_t xcr0 = state->crs[NVMM_X64_CR_XCR0];

			cpudata->gxcr0 = nvmm_x86_munge_xcr0(xcr0,
			    cpudata->xcr0_mask);
		}
	}

	CTASSERT(sizeof(cpudata->drs) == sizeof(state->drs));
	if (flags & NVMM_X64_STATE_DRS) {
		memcpy(cpudata->drs, state->drs, sizeof(state->drs));

		cpudata->drs[NVMM_X64_DR_DR6] &= 0xFFFFFFFF;
		vmx_vmwrite(VMCS_GUEST_DR7, cpudata->drs[NVMM_X64_DR_DR7]);
	}

	if (flags & NVMM_X64_STATE_MSRS) {
		cpudata->gmsr[VMX_MSRLIST_STAR].val =
		    state->msrs[NVMM_X64_MSR_STAR];
		cpudata->gmsr[VMX_MSRLIST_LSTAR].val =
		    state->msrs[NVMM_X64_MSR_LSTAR];
		cpudata->gmsr[VMX_MSRLIST_CSTAR].val =
		    state->msrs[NVMM_X64_MSR_CSTAR];
		cpudata->gmsr[VMX_MSRLIST_SFMASK].val =
		    state->msrs[NVMM_X64_MSR_SFMASK];
		cpudata->gmsr[VMX_MSRLIST_KERNELGSBASE].val =
		    state->msrs[NVMM_X64_MSR_KERNELGSBASE];

		vmx_vmwrite(VMCS_GUEST_IA32_EFER,
		    state->msrs[NVMM_X64_MSR_EFER]);
		vmx_vmwrite(VMCS_GUEST_IA32_PAT,
		    state->msrs[NVMM_X64_MSR_PAT]);
		vmx_vmwrite(VMCS_GUEST_IA32_SYSENTER_CS,
		    state->msrs[NVMM_X64_MSR_SYSENTER_CS]);
		vmx_vmwrite(VMCS_GUEST_IA32_SYSENTER_ESP,
		    state->msrs[NVMM_X64_MSR_SYSENTER_ESP]);
		vmx_vmwrite(VMCS_GUEST_IA32_SYSENTER_EIP,
		    state->msrs[NVMM_X64_MSR_SYSENTER_EIP]);

		cpudata->gtsc = state->msrs[NVMM_X64_MSR_TSC];
		cpudata->gtsc_want_update = true;

		/* ENTRY_CTLS_LONG_MODE must match EFER_LMA. */
		ctls1 = vmx_vmread(VMCS_ENTRY_CTLS);
		if (state->msrs[NVMM_X64_MSR_EFER] & EFER_LMA) {
			ctls1 |= ENTRY_CTLS_LONG_MODE;
		} else {
			ctls1 &= ~ENTRY_CTLS_LONG_MODE;
		}
		vmx_vmwrite(VMCS_ENTRY_CTLS, ctls1);
	}

	if (flags & NVMM_X64_STATE_INTR) {
		intstate = vmx_vmread(VMCS_GUEST_INTERRUPTIBILITY);
		intstate &= ~(INT_STATE_STI|INT_STATE_MOVSS);
		if (state->intr.int_shadow) {
			intstate |= INT_STATE_MOVSS;
		}
		vmx_vmwrite(VMCS_GUEST_INTERRUPTIBILITY, intstate);

		if (state->intr.int_window_exiting) {
			vmx_event_waitexit_enable(vcpu, false);
		} else {
			vmx_event_waitexit_disable(vcpu, false);
		}

		if (state->intr.nmi_window_exiting) {
			vmx_event_waitexit_enable(vcpu, true);
		} else {
			vmx_event_waitexit_disable(vcpu, true);
		}
	}

	CTASSERT(sizeof(cpudata->gfpu.xsh_fxsave) == sizeof(state->fpu));
	if (flags & NVMM_X64_STATE_FPU) {
		memcpy(cpudata->gfpu.xsh_fxsave, &state->fpu,
		    sizeof(state->fpu));

		fpustate = (struct nvmm_x64_state_fpu *)cpudata->gfpu.xsh_fxsave;
		fpustate->fx_mxcsr_mask &= cpu_mxcsr_mask;
		fpustate->fx_mxcsr &= fpustate->fx_mxcsr_mask;

		if (cpudata->xcr0_mask != 0) {
			/* Reset XSTATE_BV, to force a reload. */
			cpudata->gfpu.xsh_xstate_bv = cpudata->xcr0_mask;
		}
	}
	/*
	 * XXX XSAVE area -- need to allocate and map it separately
	 * since it may exceed the comm page size
	 */

	vmx_vmcs_leave(vcpu);

	comm->state_wanted = 0;
	comm->state_cached |= flags;
}

static void
vmx_vcpu_getstate(struct nvmm_cpu *vcpu)
{
	struct nvmm_comm_page *comm = vcpu->comm;
	struct nvmm_x64_state *state = &comm->state;
	struct vmx_cpudata *cpudata = vcpu->cpudata;
	uint64_t intstate, flags;

	flags = comm->state_wanted;

	vmx_vmcs_enter(vcpu);

	if (flags & NVMM_X64_STATE_SEGS) {
		vmx_vcpu_getstate_seg(state->segs, NVMM_X64_SEG_CS);
		vmx_vcpu_getstate_seg(state->segs, NVMM_X64_SEG_DS);
		vmx_vcpu_getstate_seg(state->segs, NVMM_X64_SEG_ES);
		vmx_vcpu_getstate_seg(state->segs, NVMM_X64_SEG_FS);
		vmx_vcpu_getstate_seg(state->segs, NVMM_X64_SEG_GS);
		vmx_vcpu_getstate_seg(state->segs, NVMM_X64_SEG_SS);
		vmx_vcpu_getstate_seg(state->segs, NVMM_X64_SEG_GDT);
		vmx_vcpu_getstate_seg(state->segs, NVMM_X64_SEG_IDT);
		vmx_vcpu_getstate_seg(state->segs, NVMM_X64_SEG_LDT);
		vmx_vcpu_getstate_seg(state->segs, NVMM_X64_SEG_TR);
	}

	CTASSERT(sizeof(cpudata->gprs) == sizeof(state->gprs));
	if (flags & NVMM_X64_STATE_GPRS) {
		memcpy(state->gprs, cpudata->gprs, sizeof(state->gprs));

		state->gprs[NVMM_X64_GPR_RIP] = vmx_vmread(VMCS_GUEST_RIP);
		state->gprs[NVMM_X64_GPR_RSP] = vmx_vmread(VMCS_GUEST_RSP);
		state->gprs[NVMM_X64_GPR_RFLAGS] = vmx_vmread(VMCS_GUEST_RFLAGS);
	}

	if (flags & NVMM_X64_STATE_CRS) {
		state->crs[NVMM_X64_CR_CR0] =
		    (vmx_vmread(VMCS_CR0_SHADOW) & CR0_STATIC_MASK) |
		    (vmx_vmread(VMCS_GUEST_CR0) & ~CR0_STATIC_MASK);
		state->crs[NVMM_X64_CR_CR2] = cpudata->gcr2;
		state->crs[NVMM_X64_CR_CR3] = vmx_vmread(VMCS_GUEST_CR3);
		state->crs[NVMM_X64_CR_CR4] = vmx_vmread(VMCS_GUEST_CR4);
		state->crs[NVMM_X64_CR_CR8] = cpudata->gcr8;
		state->crs[NVMM_X64_CR_XCR0] = cpudata->gxcr0;

		/* Hide VMXE. */
		state->crs[NVMM_X64_CR_CR4] &= ~CR4_VMXE;
	}

	CTASSERT(sizeof(cpudata->drs) == sizeof(state->drs));
	if (flags & NVMM_X64_STATE_DRS) {
		memcpy(state->drs, cpudata->drs, sizeof(state->drs));

		state->drs[NVMM_X64_DR_DR7] = vmx_vmread(VMCS_GUEST_DR7);
	}

	if (flags & NVMM_X64_STATE_MSRS) {
		state->msrs[NVMM_X64_MSR_STAR] =
		    cpudata->gmsr[VMX_MSRLIST_STAR].val;
		state->msrs[NVMM_X64_MSR_LSTAR] =
		    cpudata->gmsr[VMX_MSRLIST_LSTAR].val;
		state->msrs[NVMM_X64_MSR_CSTAR] =
		    cpudata->gmsr[VMX_MSRLIST_CSTAR].val;
		state->msrs[NVMM_X64_MSR_SFMASK] =
		    cpudata->gmsr[VMX_MSRLIST_SFMASK].val;
		state->msrs[NVMM_X64_MSR_KERNELGSBASE] =
		    cpudata->gmsr[VMX_MSRLIST_KERNELGSBASE].val;
		state->msrs[NVMM_X64_MSR_EFER] =
		    vmx_vmread(VMCS_GUEST_IA32_EFER);
		state->msrs[NVMM_X64_MSR_PAT] =
		    vmx_vmread(VMCS_GUEST_IA32_PAT);
		state->msrs[NVMM_X64_MSR_SYSENTER_CS] =
		    vmx_vmread(VMCS_GUEST_IA32_SYSENTER_CS);
		state->msrs[NVMM_X64_MSR_SYSENTER_ESP] =
		    vmx_vmread(VMCS_GUEST_IA32_SYSENTER_ESP);
		state->msrs[NVMM_X64_MSR_SYSENTER_EIP] =
		    vmx_vmread(VMCS_GUEST_IA32_SYSENTER_EIP);
		state->msrs[NVMM_X64_MSR_TSC] = cpudata->gtsc;
	}

	if (flags & NVMM_X64_STATE_INTR) {
		intstate = vmx_vmread(VMCS_GUEST_INTERRUPTIBILITY);
		state->intr.int_shadow =
		    (intstate & (INT_STATE_STI|INT_STATE_MOVSS)) != 0;
		state->intr.int_window_exiting = cpudata->int_window_exit;
		state->intr.nmi_window_exiting = cpudata->nmi_window_exit;
		state->intr.evt_pending = cpudata->evt_pending;
	}

	CTASSERT(sizeof(cpudata->gfpu.xsh_fxsave) == sizeof(state->fpu));
	if (flags & NVMM_X64_STATE_FPU) {
		memcpy(&state->fpu, cpudata->gfpu.xsh_fxsave,
		    sizeof(state->fpu));
	}
	/*
	 * XXX XSAVE area -- need to allocate and map it separately
	 * since it may exceed the comm page size
	 */

	vmx_vmcs_leave(vcpu);

	comm->state_wanted = 0;
	comm->state_cached |= flags;
}

static void
vmx_vcpu_state_provide(struct nvmm_cpu *vcpu, uint64_t flags)
{
	vcpu->comm->state_wanted = flags;
	vmx_vcpu_getstate(vcpu);
}

static void
vmx_vcpu_state_commit(struct nvmm_cpu *vcpu)
{
	vcpu->comm->state_wanted = vcpu->comm->state_commit;
	vcpu->comm->state_commit = 0;
	vmx_vcpu_setstate(vcpu);
}

/* -------------------------------------------------------------------------- */

static void
vmx_asid_alloc(struct nvmm_cpu *vcpu)
{
	struct vmx_cpudata *cpudata = vcpu->cpudata;
	size_t i, oct, bit;

	mtx_lock_spin(&vmx_asidlock);

	for (i = 0; i < vmx_maxasid; i++) {
		oct = i / 8;
		bit = i % 8;

		if (vmx_asidmap[oct] & __BIT(bit)) {
			continue;
		}

		cpudata->asid = i;

		vmx_asidmap[oct] |= __BIT(bit);
		vmx_vmwrite(VMCS_VPID, i);
		mtx_unlock_spin(&vmx_asidlock);
		return;
	}

	mtx_unlock_spin(&vmx_asidlock);

	panic("%s: impossible", __func__);
}

static void
vmx_asid_free(struct nvmm_cpu *vcpu)
{
	size_t oct, bit;
	uint64_t asid;

	asid = vmx_vmread(VMCS_VPID);

	oct = asid / 8;
	bit = asid % 8;

	mtx_lock_spin(&vmx_asidlock);
	vmx_asidmap[oct] &= ~__BIT(bit);
	mtx_unlock_spin(&vmx_asidlock);
}

static void
vmx_init_asid(uint32_t maxasid)
{
	size_t allocsz;

	mtx_init(&vmx_asidlock, "nvmmasid", NULL, MTX_SPIN);

	vmx_maxasid = maxasid;
	allocsz = roundup(maxasid, 8) / 8;
	vmx_asidmap = os_mem_zalloc(allocsz);

	/* ASID 0 is reserved for the host. */
	vmx_asidmap[0] |= __BIT(0);
}

static void
vmx_fini_asid(void)
{
	size_t allocsz;

	allocsz = roundup(vmx_maxasid, 8) / 8;
	os_mem_free(vmx_asidmap, allocsz);
	(void)allocsz;	/* FreeBSD's free(9) does not need it */

	mtx_destroy(&vmx_asidlock);
}

static void
vmx_vcpu_init(struct nvmm_machine *mach, struct nvmm_cpu *vcpu)
{
	struct vmx_cpudata *cpudata = vcpu->cpudata;
	struct vmcs *vmcs = cpudata->vmcs;
	struct msr_entry *gmsr = cpudata->gmsr;
	uint64_t eptp;

	memset(vmcs, 0, VMCS_SIZE);
	vmcs->ident = __SHIFTIN(vmx_revision, VMCS_IDENT_REVISION);
	vmcs->abort = 0;

	vmx_vmcs_enter(vcpu);

	/* No link pointer. */
	vmx_vmwrite(VMCS_LINK_POINTER, 0xFFFFFFFFFFFFFFFF);

	/* Install the CTLSs. */
	vmx_vmwrite(VMCS_PINBASED_CTLS, vmx_pinbased_ctls);
	vmx_vmwrite(VMCS_PROCBASED_CTLS, vmx_procbased_ctls);
	vmx_vmwrite(VMCS_PROCBASED_CTLS2, vmx_procbased_ctls2);
	vmx_vmwrite(VMCS_ENTRY_CTLS, vmx_entry_ctls);
	vmx_vmwrite(VMCS_EXIT_CTLS, vmx_exit_ctls);

	/* Allow direct access to certain MSRs. */
	memset(cpudata->msrbm, 0xFF, MSRBM_SIZE);
	vmx_vcpu_msr_allow(cpudata->msrbm, MSR_EFER, true, true);
	vmx_vcpu_msr_allow(cpudata->msrbm, MSR_STAR, true, true);
	vmx_vcpu_msr_allow(cpudata->msrbm, MSR_LSTAR, true, true);
	vmx_vcpu_msr_allow(cpudata->msrbm, MSR_CSTAR, true, true);
	vmx_vcpu_msr_allow(cpudata->msrbm, MSR_SF_MASK, true, true);
	vmx_vcpu_msr_allow(cpudata->msrbm, MSR_KGSBASE, true, true);
	vmx_vcpu_msr_allow(cpudata->msrbm, MSR_SYSENTER_CS_MSR, true, true);
	vmx_vcpu_msr_allow(cpudata->msrbm, MSR_SYSENTER_ESP_MSR, true, true);
	vmx_vcpu_msr_allow(cpudata->msrbm, MSR_SYSENTER_EIP_MSR, true, true);
	vmx_vcpu_msr_allow(cpudata->msrbm, MSR_FSBASE, true, true);
	vmx_vcpu_msr_allow(cpudata->msrbm, MSR_GSBASE, true, true);
	vmx_vcpu_msr_allow(cpudata->msrbm, MSR_TSC, true, false);
	vmx_vmwrite(VMCS_MSR_BITMAP, (uint64_t)cpudata->msrbm_pa);

	/*
	 * List of Guest MSRs loaded on VMENTRY, saved on VMEXIT. This
	 * includes the L1D_FLUSH MSR, to mitigate L1TF.
	 */
	gmsr[VMX_MSRLIST_STAR].msr = MSR_STAR;
	gmsr[VMX_MSRLIST_STAR].val = 0;
	gmsr[VMX_MSRLIST_LSTAR].msr = MSR_LSTAR;
	gmsr[VMX_MSRLIST_LSTAR].val = 0;
	gmsr[VMX_MSRLIST_CSTAR].msr = MSR_CSTAR;
	gmsr[VMX_MSRLIST_CSTAR].val = 0;
	gmsr[VMX_MSRLIST_SFMASK].msr = MSR_SF_MASK;
	gmsr[VMX_MSRLIST_SFMASK].val = 0;
	gmsr[VMX_MSRLIST_KERNELGSBASE].msr = MSR_KGSBASE;
	gmsr[VMX_MSRLIST_KERNELGSBASE].val = 0;
	gmsr[VMX_MSRLIST_L1DFLUSH].msr = MSR_IA32_FLUSH_CMD;
	gmsr[VMX_MSRLIST_L1DFLUSH].val = IA32_FLUSH_CMD_L1D;
	vmx_vmwrite(VMCS_ENTRY_MSR_LOAD_ADDRESS, cpudata->gmsr_pa);
	vmx_vmwrite(VMCS_EXIT_MSR_STORE_ADDRESS, cpudata->gmsr_pa);
	vmx_vmwrite(VMCS_ENTRY_MSR_LOAD_COUNT, vmx_msrlist_entry_nmsr);
	vmx_vmwrite(VMCS_EXIT_MSR_STORE_COUNT, VMX_MSRLIST_EXIT_NMSR);

	/* Set the CR0 mask. Any change of these bits causes a VMEXIT. */
	vmx_vmwrite(VMCS_CR0_MASK, CR0_STATIC_MASK);

	/* Force unsupported CR4 fields to zero. */
	vmx_vmwrite(VMCS_CR4_MASK, CR4_INVALID);
	vmx_vmwrite(VMCS_CR4_SHADOW, 0);

	/*
	 * Set the Host state for resuming. The per-CPU host fields are
	 * written by vcpu_run.
	 */
	vmx_vmwrite(VMCS_HOST_RIP, (uint64_t)vmx_resume_rip);
	vmx_vmwrite(VMCS_HOST_CS_SELECTOR, GSEL(GCODE_SEL, SEL_KPL));
	vmx_vmwrite(VMCS_HOST_SS_SELECTOR, GSEL(GDATA_SEL, SEL_KPL));
	vmx_vmwrite(VMCS_HOST_DS_SELECTOR, GSEL(GDATA_SEL, SEL_KPL));
	vmx_vmwrite(VMCS_HOST_ES_SELECTOR, GSEL(GDATA_SEL, SEL_KPL));
	vmx_vmwrite(VMCS_HOST_FS_SELECTOR, 0);
	vmx_vmwrite(VMCS_HOST_GS_SELECTOR, 0);
	vmx_vmwrite(VMCS_HOST_IA32_SYSENTER_CS, 0);
	vmx_vmwrite(VMCS_HOST_IA32_SYSENTER_ESP, 0);
	vmx_vmwrite(VMCS_HOST_IA32_SYSENTER_EIP, 0);
	vmx_vmwrite(VMCS_HOST_IA32_PAT, rdmsr(MSR_PAT));
	vmx_vmwrite(VMCS_HOST_IA32_EFER, rdmsr(MSR_EFER));
	vmx_vmwrite(VMCS_HOST_CR0, rcr0() & ~CR0_TS);

	/* Generate ASID. */
	vmx_asid_alloc(vcpu);

	/* Enable Extended Paging, 4-Level. */
	eptp =
	    __SHIFTIN(vmx_eptp_type, EPTP_TYPE) |
	    __SHIFTIN(4-1, EPTP_WALKLEN) |
	    (vmx_ept_has_ad ? EPTP_FLAGS_AD : 0) |
	    os_vmspace_pdirpa(mach->vm);
	vmx_vmwrite(VMCS_EPTP, eptp);

	/* Init IA32_MISC_ENABLE. */
	cpudata->gmsr_misc_enable = rdmsr(MSR_IA32_MISC_ENABLE);
	cpudata->gmsr_misc_enable &=
	    ~(IA32_MISC_PERFMON_EN|IA32_MISC_EISST_EN|IA32_MISC_MWAIT_EN);
	cpudata->gmsr_misc_enable |=
	    (IA32_MISC_BTS_UNAVAIL|IA32_MISC_PEBS_UNAVAIL);

	/* Init XSAVE header. */
	cpudata->xcr0_mask = vmx_xcr0_mask;
	KASSERTMSG(cpudata->xcr0_mask == 0 ||
	    nvmm_x86_xcr0_valid(cpudata->xcr0_mask, vmx_xcr0_mask),
	    "cpudata->xcr0_mask=0x%"PRIx64" vmx_xcr0_mask=0x%"PRIx64,
	    cpudata->xcr0_mask, vmx_xcr0_mask);
	cpudata->gfpu.xsh_xstate_bv = cpudata->xcr0_mask;
	cpudata->gfpu.xsh_xcomp_bv = 0;

	/* These MSRs are static. */
	cpudata->star = rdmsr(MSR_STAR);
	cpudata->lstar = rdmsr(MSR_LSTAR);
	cpudata->cstar = rdmsr(MSR_CSTAR);
	cpudata->sfmask = rdmsr(MSR_SF_MASK);

	/* Install the RESET state. */
	memcpy(&vcpu->comm->state, &nvmm_x86_reset_state,
	    sizeof(nvmm_x86_reset_state));
	vcpu->comm->state_wanted = NVMM_X64_STATE_ALL;
	vcpu->comm->state_cached = 0;
	vmx_vcpu_setstate(vcpu);

	vmx_vmcs_leave(vcpu);
}

static void
vmx_vcpu_free(struct vmx_cpudata *cpudata)
{
	if (cpudata->vmcs_pa) {
		vmx_memfree(cpudata->vmcs_pa, (vaddr_t)cpudata->vmcs,
		    VMCS_NPAGES);
	}
	if (cpudata->msrbm_pa) {
		vmx_memfree(cpudata->msrbm_pa, (vaddr_t)cpudata->msrbm,
		    MSRBM_NPAGES);
	}
	if (cpudata->gmsr_pa) {
		vmx_memfree(cpudata->gmsr_pa, (vaddr_t)cpudata->gmsr, 1);
	}
	/*
	 * NetBSD frees sizeof(*cpudata) here and in vcpu_destroy, which
	 * does not match the XSAVE-sized allocation; use the real size.
	 */
	os_pagemem_free(cpudata, cpudata->cpudata_size);
}

static int
vmx_vcpu_create(struct nvmm_machine *mach, struct nvmm_cpu *vcpu)
{
	size_t xsave_size, cpudata_size;
	struct vmx_cpudata *cpudata;
	int error;

	/*
	 * Compute the size of the VMX cpudata.  We put the
	 * variable-length XSAVE area at the end so if it's small
	 * enough, it stays within a single page.  We size the XSAVE
	 * area for the maximum set of features a guest can enable.
	 */
	xsave_size = nvmm_x86_xsave_size(vmx_xcr0_mask);
	OS_ASSERT(xsave_size < SIZE_MAX - offsetof(struct vmx_cpudata, gfpu));
	cpudata_size = MAX(sizeof(*cpudata),
	    offsetof(struct vmx_cpudata, gfpu) + xsave_size);
	cpudata_size = roundup(cpudata_size, PAGE_SIZE);

	/* Allocate the VMX cpudata. */
	cpudata = os_pagemem_zalloc(cpudata_size);
	cpudata->cpudata_size = cpudata_size;
	vcpu->cpudata = cpudata;

	/* VMCS */
	error = vmx_memalloc(&cpudata->vmcs_pa, (vaddr_t *)&cpudata->vmcs,
	    VMCS_NPAGES);
	if (error)
		goto error;

	/* MSR Bitmap */
	error = vmx_memalloc(&cpudata->msrbm_pa, (vaddr_t *)&cpudata->msrbm,
	    MSRBM_NPAGES);
	if (error)
		goto error;

	/* Guest MSR List */
	error = vmx_memalloc(&cpudata->gmsr_pa, (vaddr_t *)&cpudata->gmsr, 1);
	if (error)
		goto error;

	/* Init the VCPU info. */
	vmx_vcpu_init(mach, vcpu);

	return 0;

error:
	vmx_vcpu_free(cpudata);
	vcpu->cpudata = NULL;
	return error;
}

static void
vmx_vcpu_destroy(struct nvmm_machine *mach __unused, struct nvmm_cpu *vcpu)
{
	struct vmx_cpudata *cpudata = vcpu->cpudata;

	vmx_vmcs_enter(vcpu);
	vmx_asid_free(vcpu);
	vmx_vmcs_destroy(vcpu);

	vmx_vcpu_free(cpudata);
	vcpu->cpudata = NULL;
}

/* -------------------------------------------------------------------------- */

static int
vmx_vcpu_configure_cpuid(struct vmx_cpudata *cpudata, void *data)
{
	struct nvmm_vcpu_conf_cpuid *cpuid = data;
	size_t i;

	if (__predict_false(cpuid->mask && cpuid->exit)) {
		return EINVAL;
	}
	if (__predict_false(cpuid->mask &&
	    ((cpuid->u.mask.set.eax & cpuid->u.mask.del.eax) ||
	     (cpuid->u.mask.set.ebx & cpuid->u.mask.del.ebx) ||
	     (cpuid->u.mask.set.ecx & cpuid->u.mask.del.ecx) ||
	     (cpuid->u.mask.set.edx & cpuid->u.mask.del.edx)))) {
		return EINVAL;
	}

	/* If unset, delete, to restore the default behavior. */
	if (!cpuid->mask && !cpuid->exit) {
		for (i = 0; i < VMX_NCPUIDS; i++) {
			if (!cpudata->cpuidpresent[i]) {
				continue;
			}
			if (cpudata->cpuid[i].leaf == cpuid->leaf) {
				cpudata->cpuidpresent[i] = false;
			}
		}
		return 0;
	}

	/* If already here, replace. */
	for (i = 0; i < VMX_NCPUIDS; i++) {
		if (!cpudata->cpuidpresent[i]) {
			continue;
		}
		if (cpudata->cpuid[i].leaf == cpuid->leaf) {
			memcpy(&cpudata->cpuid[i], cpuid,
			    sizeof(struct nvmm_vcpu_conf_cpuid));
			return 0;
		}
	}

	/* Not here, insert. */
	for (i = 0; i < VMX_NCPUIDS; i++) {
		if (!cpudata->cpuidpresent[i]) {
			cpudata->cpuidpresent[i] = true;
			memcpy(&cpudata->cpuid[i], cpuid,
			    sizeof(struct nvmm_vcpu_conf_cpuid));
			return 0;
		}
	}

	return ENOBUFS;
}

static int
vmx_vcpu_configure_tpr(struct vmx_cpudata *cpudata, void *data)
{
	struct nvmm_vcpu_conf_tpr *tpr = data;

	memcpy(&cpudata->tpr, tpr, sizeof(*tpr));
	return 0;
}

static int
vmx_vcpu_configure_xcr0_mask(struct vmx_cpudata *cpudata, void *data)
{
	const uint64_t *xcr0_maskp = data;

	/*
	 * Refuse to enable XCR0 bits (extended CPU state components)
	 * not supported by this system, or to set up otherwise
	 * nonsensical masks like AVX (YMM_Hi128) but not SSE (XMM)
	 * registers.  Exception: The mask can be all-zero to disable
	 * all XSAVE state components.
	 */
	if (*xcr0_maskp != 0 &&
	    !nvmm_x86_xcr0_valid(*xcr0_maskp, vmx_xcr0_mask))
		return EINVAL;

	/*
	 * Out of paranoia, clear any existing extended CPU state.
	 * This operation is unlikely to be used before the guest has
	 * begun execution at all, so the extended CPU state is
	 * probably all zero.  But in case some weird hypervisor
	 * software tries to change the XCR0 mask dynamically, let's
	 * avoid accidentally leaking things through any extended CPU
	 * state.
	 *
	 * We could zero only the components that are getting disabled.
	 * But if the saved state is compated (XSAVEC), we wouldd also
	 * have to move the remaining components around in order to
	 * avoid zeroing them.  Since no software is likely to try this
	 * anyway, we'll just zero everything to keep it simple and
	 * avoid having to test the difficult-and-unused paths.
	 */
	memset(&cpudata->gfpu, 0, nvmm_x86_xsave_size(vmx_xcr0_mask));

	/*
	 * Set the XCR0 mask, and limit the guest's XCR0 to this mask.
	 * Any extended CPU state the guest had previously been using
	 * will be wiped out.
	 */
	cpudata->xcr0_mask = *xcr0_maskp;
	cpudata->gxcr0 &= cpudata->xcr0_mask;
	KASSERTMSG((cpudata->xcr0_mask == 0 ||
		nvmm_x86_xcr0_valid(cpudata->gxcr0, cpudata->xcr0_mask)),
	    "gxcr0=0x%"PRIx64" xcr0_mask=0x%"PRIx64,
	    cpudata->gxcr0, cpudata->xcr0_mask);
	return 0;
}

static int
vmx_vcpu_configure(struct nvmm_cpu *vcpu, uint64_t op, void *data)
{
	struct vmx_cpudata *cpudata = vcpu->cpudata;

	switch (op) {
	case NVMM_VCPU_CONF_MD(NVMM_VCPU_CONF_CPUID):
		return vmx_vcpu_configure_cpuid(cpudata, data);
	case NVMM_VCPU_CONF_MD(NVMM_VCPU_CONF_TPR):
		return vmx_vcpu_configure_tpr(cpudata, data);
	case NVMM_VCPU_CONF_MD(NVMM_VCPU_CONF_XCR0_MASK):
		return vmx_vcpu_configure_xcr0_mask(cpudata, data);
	default:
		return EINVAL;
	}
}

static void
vmx_vcpu_suspend(struct nvmm_machine *mach __unused, struct nvmm_cpu *vcpu)
{
	struct vmx_cpudata *cpudata = vcpu->cpudata;
	os_cpu_t *vmcs_ci;

	OS_ASSERT(cpudata->vmcs_refcnt == 0);

	vmcs_ci = cpudata->vmcs_ci;
	cpudata->vmcs_ci = (void *)0x00FFFFFFFFFFFFFF; /* clobber */

	os_preempt_disable();
	if (vmcs_ci == NULL) {
		/* VMCS is inactive, nothing to do.  */
	} else if (vmcs_ci != os_curcpu()) {
		/* VMCS is active on a remote CPU; clear it there.  */
		vmx_vmclear_remote(vmcs_ci, cpudata->vmcs_pa);
	} else {
		/* VMCS is active on this CPU; clear it here.  */
		vmx_vmclear(&cpudata->vmcs_pa);
	}
	os_preempt_enable();
}

static void
vmx_vcpu_resume(struct nvmm_machine *mach __unused, struct nvmm_cpu *vcpu)
{
	struct vmx_cpudata *cpudata = vcpu->cpudata;

	OS_ASSERT(cpudata->vmcs_refcnt == 0);

	/* Mark VMCS as inactive.  */
	cpudata->vmcs_ci = NULL;
}

/* -------------------------------------------------------------------------- */

/* FreeBSD: the vmspace already has an EPT pmap (os_vmspace_ept_setup). */
static void
vmx_machine_create(struct nvmm_machine *mach)
{
	struct vmx_machdata *machdata;
	size_t i;

	OS_ASSERT(vmspace_pmap(mach->vm)->pm_type == PT_EPT);

	machdata = os_mem_zalloc(sizeof(struct vmx_machdata));
	for (i = 0; i < MAXCPU; i++)
		machdata->eptgen[i] = -1;
	mach->machdata = machdata;
}

static void
vmx_machine_destroy(struct nvmm_machine *mach)
{
	os_mem_free(mach->machdata, sizeof(struct vmx_machdata));
	mach->machdata = NULL;
}

static int
vmx_machine_configure(struct nvmm_machine *mach __unused, uint64_t op __unused,
    void *data __unused)
{
	panic("%s: impossible", __func__);
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


static void
vmx_init_l1tf(void)
{
	u_int descs[4];
	uint64_t msr;

	if (cpu_high < 7) {
		return;
	}

	x86_cpuid2(7, 0, descs);

	if (descs[3] & CPUID_SEF_ARCH_CAP) {
		msr = rdmsr(MSR_IA32_ARCH_CAPABILITIES);
		if (msr & IA32_ARCH_SKIP_L1DFL_VMENTRY) {
			/* No mitigation needed. */
			return;
		}
	}

	if (descs[3] & CPUID_SEF_L1D_FLUSH) {
		/* Enable hardware mitigation. */
		vmx_msrlist_entry_nmsr += 1;
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

	/* Init the max basic and extended CPUID leaves. */
	vmx_cpuid_max_basic = uimin(cpu_high, VMX_CPUID_MAX_BASIC);
	vmx_cpuid_max_extended = uimin(cpu_exthigh, VMX_CPUID_MAX_EXTENDED);

	/* Init the TLB flush op, the EPT flush op and the EPTP type. */
	if (!vmx_rdmsr(MSR_IA32_VMX_EPT_VPID_CAP, &msr))
		return ENXIO;
	if ((msr & IA32_VMX_EPT_VPID_INVVPID_CONTEXT) != 0) {
		vmx_tlb_flush_op = VMX_INVVPID_CONTEXT;
	} else {
		vmx_tlb_flush_op = VMX_INVVPID_ALL;
	}
	if ((msr & IA32_VMX_EPT_VPID_INVEPT_CONTEXT) != 0) {
		vmx_ept_flush_op = VMX_INVEPT_CONTEXT;
	} else {
		vmx_ept_flush_op = VMX_INVEPT_ALL;
	}
	if ((msr & IA32_VMX_EPT_VPID_WB) != 0) {
		vmx_eptp_type = EPTP_TYPE_WB;
	} else {
		vmx_eptp_type = EPTP_TYPE_UC;
	}

	/* Init the L1TF mitigation. */
	vmx_init_l1tf();

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

	/* Init the ASID bitmap (VPID), and the EPT pmap of the guests. */
	vmx_init_asid(VPID_MAX);
	os_vmspace_ept_setup(vmx_ept_has_ad);

	if (bootverbose)
		os_printf("nvmm: VMXON on %d CPUs\n", mp_ncpus);
	return 0;
}

static void
vmx_fini(void)
{
	vmx_disable_all();
	vmx_free_regions();
	vmx_fini_asid();
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


/* -------------------------------------------------------------------------- */

/*
 * FPU. NetBSD's fpu_area_restore/fpu_area_save, with FreeBSD's
 * fpu_kern_enter(FPU_KERN_NOCTX) around them (decision P5-2): it saves the
 * thread's FPU state if live, enters a critical section and lets us use
 * the FPU registers; fpu_kern_leave() lets the thread reload its state
 * lazily.
 */
static inline void
vmx_fpu_restore(struct xsave_header *area, uint64_t mask, bool xsave)
{
	if (xsave) {
		__asm volatile("xrstor64 %0"
		    : : "m" (*area), "a" ((uint32_t)mask),
		    "d" ((uint32_t)(mask >> 32)) : "memory");
	} else {
		__asm volatile("fxrstor64 %0" : : "m" (*area) : "memory");
	}
}

static inline void
vmx_fpu_save(struct xsave_header *area, uint64_t mask, bool xsave)
{
	if (xsave) {
		__asm volatile("xsave64 %0"
		    : "=m" (*area) : "a" ((uint32_t)mask),
		    "d" ((uint32_t)(mask >> 32)) : "memory");
	} else {
		__asm volatile("fxsave64 %0" : "=m" (*area) : : "memory");
	}
}

static void
vmx_vcpu_guest_fpu_enter(struct nvmm_cpu *vcpu)
{
	struct vmx_cpudata *cpudata = vcpu->cpudata;

	/*
	 * The guest's XCR0 had better not have any bits that aren't
	 * allowed in the vCPU configuration, and the current XSAVE
	 * area had better not store any either according to
	 * cpudata->gfpu.xsh_xstate_bv.
	 *
	 * Note that XRSTOR will trap if XSTATE_BV has any bits that
	 * are not set in XCR0.
	 */
	KASSERTMSG((cpudata->gxcr0 & ~cpudata->xcr0_mask) == 0,
	    "gxcr0=0x%"PRIx64" xcr0_mask=0x%"PRIx64,
	    cpudata->gxcr0, cpudata->xcr0_mask);
	KASSERTMSG((cpudata->gfpu.xsh_xstate_bv & ~cpudata->xcr0_mask) == 0,
	    "XSTATE_BV=0x%"PRIx64" xcr0_mask=0x%"PRIx64,
	    cpudata->gfpu.xsh_xstate_bv, cpudata->xcr0_mask);

	/*
	 * Save anything in the FPU registers that this thread might
	 * have been using to memory, and enter a critical section so
	 * that nothing else uses the FPU on this CPU meanwhile.
	 *
	 * After this point, we are free to use the FPU registers.
	 */
	fpu_kern_enter(curthread, NULL, FPU_KERN_NOCTX);

	/*
	 * If the host CPU doesn't support XSAVE or we're simulating a
	 * vCPU without it, just restore the x87 and SSE state.  The
	 * host should already have both x87 and SSE enabled in XCR0,
	 * if the host uses XSAVE.
	 */
	if (cpudata->xcr0_mask == 0) {
		/* TODO: should we use *XSAVE64 here? */
		vmx_fpu_restore(&cpudata->gfpu, XCR0_X87|XCR0_SSE, false);
		return;
	}

	/*
	 * Set XCR0 to allow access to anything the guest has
	 * previously used and is saved to memory, _and_ to anything
	 * the guest has asked to use in cpudata->gxcr0.
	 *
	 * The guest may have used some extended CPU state like the
	 * zmmN registers, and then later disabled them in XCR0; in
	 * that case, the state must be preserved in case the guest
	 * later enables it in XCR0, but we can only load while all
	 * bits in cpudata->gfpu.xsh_xstate_bv are set in XCR0.
	 *
	 * Similarly, the guest may _not_ have used some extended CPU
	 * state since reset, but may have since enabled it in XCR0.
	 * Such state will be clear in cpudata->gfpu.xsh_xstate_bv and
	 * must be initialized afresh by the CPU, which requires the
	 * bits be set in XCR0 to allow that.
	 */
	cpudata->hxcr0 = rxcr(0);
	load_xcr(0, cpudata->xcr0_mask &
	    (cpudata->gfpu.xsh_xstate_bv | cpudata->gxcr0));

	/*
	 * Load the guest's saved extended CPU state from memory into
	 * the CPU.
	 */
	/* TODO: should we use *XSAVE64 here? */
	vmx_fpu_restore(&cpudata->gfpu, cpudata->xcr0_mask, true);

	/*
	 * If we temporarily set XCR0 beyond what the guest asked for
	 * in order to restore state that is currently disabled, reduce
	 * it down to what the guest asked for.
	 */
	if (__predict_false(cpudata->gxcr0 != (cpudata->xcr0_mask &
		    (cpudata->gfpu.xsh_xstate_bv | cpudata->gxcr0))))
		load_xcr(0, cpudata->xcr0_mask & cpudata->gxcr0);
}

static void
vmx_vcpu_guest_fpu_leave(struct nvmm_cpu *vcpu)
{
	struct vmx_cpudata *cpudata = vcpu->cpudata;

	/*
	 * If the host CPU doesn't support XSAVE or we're simulating a
	 * vCPU without it, just save the x87 and SSE state.  If the
	 * host uses XSAVE, it should still have both x87 and SSE
	 * enabled in XCR0; if the host doesn't use XSAVE, doesn't
	 * matter.
	 */
	if (cpudata->xcr0_mask == 0) {
		/* TODO: should we use *XSAVE64 here? */
		vmx_fpu_save(&cpudata->gfpu, XCR0_X87|XCR0_SSE, false);
		goto leave;
	}

	/*
	 * In case the guest has cleared some XCR0 bits but used the
	 * corresponding extended CPU state, increase XCR0 to the
	 * maximum supported for this guest before we XSAVE.
	 *
	 * Note that XRSTOR will trap if XSTATE_BV has any bits that
	 * are not set in XCR0.
	 */
	cpudata->gxcr0 = rxcr(0);
	load_xcr(0, cpudata->xcr0_mask);

	/*
	 * Paranoia: Ensure the guest's XCR0 has no forbidden bits.
	 * Should not be possible because we filter them on XSETBV
	 * exits.
	 */
	KASSERTMSG((cpudata->gxcr0 & ~cpudata->xcr0_mask) == 0,
	    "gxcr0=0x%"PRIx64" xcr0_mask=0x%"PRIx64,
	    cpudata->gxcr0, cpudata->xcr0_mask);
	cpudata->gxcr0 &= cpudata->xcr0_mask;

	/*
	 * Save any extended CPU state that could be in use by the
	 * guest.
	 */
	/* TODO: should we use *XSAVE64 here? */
	vmx_fpu_save(&cpudata->gfpu, cpudata->xcr0_mask, false);

	/*
	 * If the host XCR0 is different from the maximum guest XCR0,
	 * switch back to the host XCR0 so we can restore NetBSD's FPU
	 * state.
	 */
	if (cpudata->xcr0_mask != cpudata->hxcr0)
		load_xcr(0, cpudata->hxcr0);

leave:	/*
	 * Leave the FPU to this thread again: FreeBSD reloads its saved
	 * state on its next FPU use.
	 *
	 * After this point, we must not touch the FPU registers.
	 */
	fpu_kern_leave(curthread, NULL);

	/*
	 * The guest's XCR0 had better not have any bits that aren't
	 * allowed in the vCPU configuration, and the current XSAVE
	 * area had better not store any either according to
	 * cpudata->gfpu.xsh_xstate_bv.
	 */
	KASSERTMSG((cpudata->gxcr0 & ~cpudata->xcr0_mask) == 0,
	    "gxcr0=0x%"PRIx64" xcr0_mask=0x%"PRIx64,
	    cpudata->gxcr0, cpudata->xcr0_mask);
	KASSERTMSG((cpudata->gfpu.xsh_xstate_bv & ~cpudata->xcr0_mask) == 0,
	    "XSTATE_BV=0x%"PRIx64" xcr0_mask=0x%"PRIx64,
	    cpudata->gfpu.xsh_xstate_bv, cpudata->xcr0_mask);
}

/*
 * Debug registers. NetBSD saves the lwp's through x86_dbregs_save(); here,
 * as bhyve's vmx_dr_enter_guest(), the host values are saved around the
 * guest. A VM exit leaves DR7 = 0x400 and DEBUGCTL = 0, so both are
 * restored too.
 */
static void
vmx_vcpu_guest_dbregs_enter(struct nvmm_cpu *vcpu)
{
	struct vmx_cpudata *cpudata = vcpu->cpudata;

	cpudata->hdr7 = rdr7();
	cpudata->hdebugctl = rdmsr(MSR_DEBUGCTLMSR);

	/* No host breakpoint may fire while guest values are loaded. */
	load_dr7(0);

	cpudata->hdr[0] = rdr0();
	cpudata->hdr[1] = rdr1();
	cpudata->hdr[2] = rdr2();
	cpudata->hdr[3] = rdr3();
	cpudata->hdr6 = rdr6();

	load_dr0(cpudata->drs[NVMM_X64_DR_DR0]);
	load_dr1(cpudata->drs[NVMM_X64_DR_DR1]);
	load_dr2(cpudata->drs[NVMM_X64_DR_DR2]);
	load_dr3(cpudata->drs[NVMM_X64_DR_DR3]);
	load_dr6(cpudata->drs[NVMM_X64_DR_DR6]);
}

static void
vmx_vcpu_guest_dbregs_leave(struct nvmm_cpu *vcpu)
{
	struct vmx_cpudata *cpudata = vcpu->cpudata;

	cpudata->drs[NVMM_X64_DR_DR0] = rdr0();
	cpudata->drs[NVMM_X64_DR_DR1] = rdr1();
	cpudata->drs[NVMM_X64_DR_DR2] = rdr2();
	cpudata->drs[NVMM_X64_DR_DR3] = rdr3();
	cpudata->drs[NVMM_X64_DR_DR6] = rdr6();

	load_dr0(cpudata->hdr[0]);
	load_dr1(cpudata->hdr[1]);
	load_dr2(cpudata->hdr[2]);
	load_dr3(cpudata->hdr[3]);
	load_dr6(cpudata->hdr6);
	load_dr7(cpudata->hdr7);
	wrmsr(MSR_DEBUGCTLMSR, cpudata->hdebugctl);
}

static void
vmx_vcpu_guest_misc_enter(struct nvmm_cpu *vcpu)
{
	struct vmx_cpudata *cpudata = vcpu->cpudata;

	/* This gets restored automatically by the CPU. */
	vmx_vmwrite(VMCS_HOST_IDTR_BASE, (uint64_t)os_curcpu_idt());
	vmx_vmwrite(VMCS_HOST_FS_BASE, rdmsr(MSR_FSBASE));
	vmx_vmwrite(VMCS_HOST_CR3, rcr3());
	vmx_vmwrite(VMCS_HOST_CR4, rcr4());

	cpudata->kernelgsbase = rdmsr(MSR_KGSBASE);
}

static void
vmx_vcpu_guest_misc_leave(struct nvmm_cpu *vcpu)
{
	struct vmx_cpudata *cpudata = vcpu->cpudata;

	wrmsr(MSR_STAR, cpudata->star);
	wrmsr(MSR_LSTAR, cpudata->lstar);
	wrmsr(MSR_CSTAR, cpudata->cstar);
	wrmsr(MSR_SF_MASK, cpudata->sfmask);
	wrmsr(MSR_KGSBASE, cpudata->kernelgsbase);
}

/* -------------------------------------------------------------------------- */

static inline void
vmx_gtlb_catchup(struct nvmm_cpu *vcpu, int hcpu)
{
	struct vmx_cpudata *cpudata = vcpu->cpudata;

	if (vcpu->hcpu_last != hcpu) {
		cpudata->gtlb_want_flush = true;
	}
}

/*
 * EPT invalidations, FreeBSD's protocol (bhyve's vmx_pmap_activate): with
 * interrupts off, mark this CPU active on the pmap, enter its SMR section,
 * then compare the pmap's generation with the one this CPU last flushed
 * at. pmap_invalidate_ept() bumps the generation, IPIs the CPUs in
 * pm_active (a VM exit) and waits on the SMR for every CPU that may have
 * read the old generation, so no guest runs on stale translations.
 */
static inline void
vmx_pmap_activate(struct nvmm_machine *mach, pmap_t pmap, int hcpu)
{
	struct vmx_machdata *machdata = mach->machdata;
	struct ept_desc ept_desc;
	long eptgen;

	CPU_SET_ATOMIC(hcpu, &pmap->pm_active);
	smr_enter(pmap->pm_eptsmr);
	eptgen = atomic_load_long(&pmap->pm_eptgen);
	if (eptgen != machdata->eptgen[hcpu]) {
		machdata->eptgen[hcpu] = eptgen;
		ept_desc.eptp = vmx_vmread(VMCS_EPTP);
		ept_desc.mbz = 0;
		vmx_invept(vmx_ept_flush_op, &ept_desc);
	}
}

static inline void
vmx_pmap_deactivate(pmap_t pmap, int hcpu)
{
	smr_exit(pmap->pm_eptsmr);
	CPU_CLR_ATOMIC(hcpu, &pmap->pm_active);
}

static inline void
vmx_exit_evt(struct vmx_cpudata *cpudata)
{
	uint64_t info, err, inslen;

	cpudata->evt_pending = false;

	info = vmx_vmread(VMCS_IDT_VECTORING_INFO);
	if (__predict_true((info & INTR_INFO_VALID) == 0)) {
		return;
	}
	err = vmx_vmread(VMCS_IDT_VECTORING_ERROR);

	vmx_vmwrite(VMCS_ENTRY_INTR_INFO, info);
	vmx_vmwrite(VMCS_ENTRY_EXCEPTION_ERROR, err);

	switch (__SHIFTOUT(info, INTR_INFO_TYPE)) {
	case INTR_TYPE_SW_INT:
	case INTR_TYPE_PRIV_SW_EXC:
	case INTR_TYPE_SW_EXC:
		inslen = vmx_vmread(VMCS_EXIT_INSTRUCTION_LENGTH);
		vmx_vmwrite(VMCS_ENTRY_INSTRUCTION_LENGTH, inslen);
	}

	cpudata->evt_pending = true;
}

static int
vmx_vcpu_run(struct nvmm_machine *mach, struct nvmm_cpu *vcpu,
    struct nvmm_vcpu_exit *exit)
{
	struct nvmm_comm_page *comm = vcpu->comm;
	struct vmx_cpudata *cpudata = vcpu->cpudata;
	pmap_t pmap = vmspace_pmap(mach->vm);
	struct region_descriptor gdtr, idtr;
	struct vpid_desc vpid_desc;
	uint64_t exitcode = 0;
	uint64_t intstate;
	int hcpu, ret;
	bool launched;

	vmx_vmcs_enter(vcpu);

	vmx_vcpu_state_commit(vcpu);
	comm->state_cached = 0;

	if (__predict_false(vmx_vcpu_event_commit(vcpu) != 0)) {
		vmx_vmcs_leave(vcpu);
		return EINVAL;
	}

	hcpu = curcpu;
	launched = cpudata->vmcs_launched;

	vmx_gtlb_catchup(vcpu, hcpu);

	if (vcpu->hcpu_last != hcpu) {
		vmx_vmwrite(VMCS_HOST_TR_SELECTOR, os_curcpu_tss_sel());
		vmx_vmwrite(VMCS_HOST_TR_BASE, (uint64_t)os_curcpu_tss());
		vmx_vmwrite(VMCS_HOST_GDTR_BASE, (uint64_t)os_curcpu_gdt());
		vmx_vmwrite(VMCS_HOST_GS_BASE, rdmsr(MSR_GSBASE));
		cpudata->gtsc_want_update = true;
		vcpu->hcpu_last = hcpu;
	}

	vmx_vcpu_guest_dbregs_enter(vcpu);
	vmx_vcpu_guest_misc_enter(vcpu);

	while (1) {
		if (cpudata->gtlb_want_flush) {
			vpid_desc.vpid = cpudata->asid;
			vpid_desc.addr = 0;
			vmx_invvpid(vmx_tlb_flush_op, &vpid_desc);
			cpudata->gtlb_want_flush = false;
		}

		if (__predict_false(cpudata->gtsc_want_update)) {
			vmx_vmwrite(VMCS_TSC_OFFSET, cpudata->gtsc - rdtsc());
			cpudata->gtsc_want_update = false;
		}

		vmx_vcpu_guest_fpu_enter(vcpu);
		vmx_cli();
		/*
		 * FreeBSD, as bhyve: a VM exit sets the GDTR and IDTR
		 * limits to 0xFFFF (only the bases are in the VMCS); keep
		 * the real ones to put back. The LDT is saved by the
		 * assembly entry code.
		 */
		sgdt(&gdtr);
		sidt(&idtr);
		vmx_pmap_activate(mach, pmap, hcpu);
		lcr2(cpudata->gcr2);
		if (launched) {
			ret = vmx_vmresume(cpudata->gprs);
		} else {
			ret = vmx_vmlaunch(cpudata->gprs);
		}
		cpudata->gcr2 = rcr2();
		vmx_pmap_deactivate(pmap, hcpu);
		bare_lgdt(&gdtr);
		lidt(&idtr);
		if (__predict_true(ret == 0)) {
			exitcode = vmx_vmread(VMCS_EXIT_REASON);
			exitcode &= __BITS(15,0);
			if (exitcode == VMCS_EXITCODE_EXC_NMI) {
				/* handle nmi before vmx_sti() */
				vmx_exit_exc_nmi(mach, vcpu, exit);
			}
		}
		vmx_sti();
		vmx_vcpu_guest_fpu_leave(vcpu);

		if (__predict_false(ret != 0)) {
			vmx_exit_invalid(exit, -1);
			break;
		}
		vmx_exit_evt(cpudata);

		launched = true;

		switch (exitcode) {
		case VMCS_EXITCODE_EXC_NMI:
			/* handled earlier */
			break;
		case VMCS_EXITCODE_EXT_INT:
			exit->reason = NVMM_VCPU_EXIT_NONE;
			break;
		case VMCS_EXITCODE_HLT:
			vmx_exit_hlt(mach, vcpu, exit);
			break;
		case VMCS_EXITCODE_IO:
			vmx_exit_io(mach, vcpu, exit);
			break;
		case VMCS_EXITCODE_EPT_VIOLATION:
			vmx_exit_epf(mach, vcpu, exit);
			break;
		case VMCS_EXITCODE_CPUID:
			vmx_exit_cpuid(mach, vcpu, exit);
			break;
		case VMCS_EXITCODE_CR:
			vmx_exit_cr(mach, vcpu, exit);
			break;
		case VMCS_EXITCODE_RDMSR:
			vmx_exit_rdmsr(mach, vcpu, exit);
			break;
		case VMCS_EXITCODE_WRMSR:
			vmx_exit_wrmsr(mach, vcpu, exit);
			break;
		case VMCS_EXITCODE_SHUTDOWN:
			exit->reason = NVMM_VCPU_EXIT_SHUTDOWN;
			break;
		case VMCS_EXITCODE_MONITOR:
			vmx_exit_insn(exit, NVMM_VCPU_EXIT_MONITOR);
			break;
		case VMCS_EXITCODE_MWAIT:
			vmx_exit_insn(exit, NVMM_VCPU_EXIT_MWAIT);
			break;
		case VMCS_EXITCODE_XSETBV:
			vmx_exit_xsetbv(mach, vcpu, exit);
			break;
		case VMCS_EXITCODE_RDPMC:
		case VMCS_EXITCODE_RDTSCP:
		case VMCS_EXITCODE_INVVPID:
		case VMCS_EXITCODE_INVEPT:
		case VMCS_EXITCODE_VMCALL:
		case VMCS_EXITCODE_VMCLEAR:
		case VMCS_EXITCODE_VMLAUNCH:
		case VMCS_EXITCODE_VMPTRLD:
		case VMCS_EXITCODE_VMPTRST:
		case VMCS_EXITCODE_VMREAD:
		case VMCS_EXITCODE_VMRESUME:
		case VMCS_EXITCODE_VMWRITE:
		case VMCS_EXITCODE_VMXOFF:
		case VMCS_EXITCODE_VMXON:
			vmx_inject_ud(vcpu);
			exit->reason = NVMM_VCPU_EXIT_NONE;
			break;
		case VMCS_EXITCODE_INT_WINDOW:
			vmx_event_waitexit_disable(vcpu, false);
			exit->reason = NVMM_VCPU_EXIT_INT_READY;
			break;
		case VMCS_EXITCODE_NMI_WINDOW:
			vmx_event_waitexit_disable(vcpu, true);
			exit->reason = NVMM_VCPU_EXIT_NMI_READY;
			break;
		default:
			vmx_exit_invalid(exit, exitcode);
			break;
		}

		/* If no reason to return to userland, keep rolling. */
		if (nvmm_return_needed(vcpu, exit)) {
			break;
		}
		if (exit->reason != NVMM_VCPU_EXIT_NONE) {
			break;
		}
	}

	cpudata->vmcs_launched = launched;

	cpudata->gtsc = vmx_vmread(VMCS_TSC_OFFSET) + rdtsc();

	vmx_vcpu_guest_misc_leave(vcpu);
	vmx_vcpu_guest_dbregs_leave(vcpu);

	exit->exitstate.rflags = vmx_vmread(VMCS_GUEST_RFLAGS);
	exit->exitstate.cr8 = cpudata->gcr8;
	intstate = vmx_vmread(VMCS_GUEST_INTERRUPTIBILITY);
	exit->exitstate.int_shadow =
	    (intstate & (INT_STATE_STI|INT_STATE_MOVSS)) != 0;
	exit->exitstate.int_window_exiting = cpudata->int_window_exit;
	exit->exitstate.nmi_window_exiting = cpudata->nmi_window_exit;
	exit->exitstate.evt_pending = cpudata->evt_pending;

	vmx_vmcs_leave(vcpu);

	return 0;
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
	.vcpu_suspend = vmx_vcpu_suspend,
	.vcpu_resume = vmx_vcpu_resume,
};
