/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Sarah Chiandotto
 * All rights reserved.
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
 * Kernel-only NetBSD x86 names that the NVMM x86 code uses and FreeBSD
 * spells differently or does not have. Only what the code built so far
 * needs; the CPUID feature names come with the CPUID exit handler.
 */

#ifndef _NVMM_X86_FREEBSD_H_
#define _NVMM_X86_FREEBSD_H_

#ifndef _KERNEL
#error "kernel only"
#endif

#include <sys/types.h>
#include <machine/cpufunc.h>
#include <machine/md_var.h>
#include <machine/specialreg.h>

#include <dev/nvmm/nvmm_bitops.h>
#include <machine/_inttypes.h>

/* NetBSD's KASSERTMSG(e, fmt, args...); KASSERT(e) is OS_ASSERT(e). */
#define KASSERTMSG(e, ...)	KASSERT(e, (__VA_ARGS__))

/* <x86/specialreg.h>: XCR0 state components. */
#define XCR0_X87		XFEATURE_ENABLED_X87
#define XCR0_SSE		XFEATURE_ENABLED_SSE
#define XCR0_YMM_Hi128		XFEATURE_ENABLED_AVX

/* <x86/specialreg.h>: control register bits. */
#ifndef CR0_ET
#define CR0_ET			0x00000010
#endif
#define CR4_OSFXSR		CR4_FXSR
#define CR4_OSXMMEXCPT		CR4_XMM
#define CR4_OSXSAVE		CR4_XSAVE

/* <x86/specialreg.h>: IA32_MISC_ENABLE bits. */
#define IA32_MISC_PERFMON_EN	__BIT(7)
#define IA32_MISC_BTS_UNAVAIL	__BIT(11)
#define IA32_MISC_PEBS_UNAVAIL	__BIT(12)
#define IA32_MISC_EISST_EN	__BIT(16)
#define IA32_MISC_MWAIT_EN	__BIT(18)

/* <x86/specialreg.h>: L1TF mitigation. */
#define CPUID_SEF_L1D_FLUSH		CPUID_STDEXT3_L1D_FLUSH
#define CPUID_SEF_ARCH_CAP		CPUID_STDEXT3_ARCH_CAP
#define MSR_IA32_ARCH_CAPABILITIES	MSR_IA32_ARCH_CAP
#define IA32_ARCH_SKIP_L1DFL_VMENTRY	IA32_ARCH_CAP_SKIP_L1DFL_VMENTRY

/*
 * <x86/specialreg.h>: the CPUID feature bits, MSRs and fields named by the
 * CPUID masks (nvmm_x86.c) and the CPUID/MSR exit handlers, copied from
 * NetBSD's header with their values. Names FreeBSD already defines were
 * checked to have the same values.
 */
#ifndef CPUID2_DEADLINE
#define CPUID2_DEADLINE	__BIT(24)
#endif
#ifndef CPUID2_MONITOR
#define CPUID2_MONITOR	__BIT(3)
#endif
#ifndef CPUID2_RAZ
#define CPUID2_RAZ	__BIT(31)
#endif
#ifndef CPUID_3DNOW
#define CPUID_3DNOW	0x80000000
#endif
#ifndef CPUID_3DNOW2
#define CPUID_3DNOW2	0x40000000
#endif
#ifndef CPUID_3DNOWPF
#define CPUID_3DNOWPF	__BIT(8)
#endif
#ifndef CPUID_ABM
#define CPUID_ABM	__BIT(5)
#endif
#ifndef CPUID_ALTMOVCR0
#define CPUID_ALTMOVCR0	__BIT(4)
#endif
#ifndef CPUID_APM_ITSC
#define CPUID_APM_ITSC	__BIT(8)
#endif
#ifndef CPUID_CAPEX_CLZERO
#define CPUID_CAPEX_CLZERO	__BIT(0)
#endif
#ifndef CPUID_CAPEX_IRPERF
#define CPUID_CAPEX_IRPERF	__BIT(1)
#endif
#ifndef CPUID_CAPEX_MCOMMIT
#define CPUID_CAPEX_MCOMMIT	__BIT(8)
#endif
#ifndef CPUID_CAPEX_RDPRU
#define CPUID_CAPEX_RDPRU	__BIT(4)
#endif
#ifndef CPUID_CAPEX_WBNOINVD
#define CPUID_CAPEX_WBNOINVD	__BIT(9)
#endif
#ifndef CPUID_CAPEX_XSAVEERPTR
#define CPUID_CAPEX_XSAVEERPTR	__BIT(2)
#endif
#ifndef CPUID_CMPLEGACY
#define CPUID_CMPLEGACY	__BIT(1)
#endif
#ifndef CPUID_DBE
#define CPUID_DBE	__BIT(26)
#endif
#ifndef CPUID_EAPIC
#define CPUID_EAPIC	__BIT(3)
#endif
#ifndef CPUID_EM64T
#define CPUID_EM64T	__BIT(29)
#endif
#ifndef CPUID_FFXSR
#define CPUID_FFXSR	0x02000000
#endif
#ifndef CPUID_FMA4
#define CPUID_FMA4	__BIT(16)
#endif
#ifndef CPUID_IBS
#define CPUID_IBS	__BIT(10)
#endif
#ifndef CPUID_L2IPERFC
#define CPUID_L2IPERFC	__BIT(28)
#endif
#ifndef CPUID_LAHF
#define CPUID_LAHF	__BIT(0)
#endif
#ifndef CPUID_LWP
#define CPUID_LWP	__BIT(15)
#endif
#ifndef CPUID_MISALIGNSSE
#define CPUID_MISALIGNSSE	__BIT(7)
#endif
#ifndef CPUID_MMXX
#define CPUID_MMXX	0x00400000
#endif
#ifndef CPUID_MWAITX
#define CPUID_MWAITX	__BIT(29)
#endif
#ifndef CPUID_NODEID
#define CPUID_NODEID	__BIT(19)
#endif
#ifndef CPUID_OSVW
#define CPUID_OSVW	__BIT(9)
#endif
#ifndef CPUID_PAGE1GB
#define CPUID_PAGE1GB	__BIT(26)
#endif
#ifndef CPUID_PCEC
#define CPUID_PCEC	__BIT(23)
#endif
#ifndef CPUID_PCENB
#define CPUID_PCENB	__BIT(24)
#endif
#ifndef CPUID_PES1_XGETBV
#define CPUID_PES1_XGETBV	__BIT(2)
#endif
#ifndef CPUID_PES1_XSAVEC
#define CPUID_PES1_XSAVEC	__BIT(1)
#endif
#ifndef CPUID_PES1_XSAVEOPT
#define CPUID_PES1_XSAVEOPT	__BIT(0)
#endif
#ifndef CPUID_PTSC
#define CPUID_PTSC	__BIT(27)
#endif
#ifndef CPUID_RDTSCP
#define CPUID_RDTSCP	__BIT(27)
#endif
#ifndef CPUID_SEF_ADX
#define CPUID_SEF_ADX	__BIT(19)
#endif
#ifndef CPUID_SEF_AVX2
#define CPUID_SEF_AVX2	__BIT(5)
#endif
#ifndef CPUID_SEF_AVX512_4FMAPS
#define CPUID_SEF_AVX512_4FMAPS	__BIT(3)
#endif
#ifndef CPUID_SEF_AVX512_4VNNIW
#define CPUID_SEF_AVX512_4VNNIW	__BIT(2)
#endif
#ifndef CPUID_SEF_AVX512_BITALG
#define CPUID_SEF_AVX512_BITALG	__BIT(12)
#endif
#ifndef CPUID_SEF_AVX512BW
#define CPUID_SEF_AVX512BW	__BIT(30)
#endif
#ifndef CPUID_SEF_AVX512CD
#define CPUID_SEF_AVX512CD	__BIT(28)
#endif
#ifndef CPUID_SEF_AVX512DQ
#define CPUID_SEF_AVX512DQ	__BIT(17)
#endif
#ifndef CPUID_SEF_AVX512ER
#define CPUID_SEF_AVX512ER	__BIT(27)
#endif
#ifndef CPUID_SEF_AVX512F
#define CPUID_SEF_AVX512F	__BIT(16)
#endif
#ifndef CPUID_SEF_AVX512_IFMA
#define CPUID_SEF_AVX512_IFMA	__BIT(21)
#endif
#ifndef CPUID_SEF_AVX512PF
#define CPUID_SEF_AVX512PF	__BIT(26)
#endif
#ifndef CPUID_SEF_AVX512_VBMI
#define CPUID_SEF_AVX512_VBMI	__BIT(1)
#endif
#ifndef CPUID_SEF_AVX512_VBMI2
#define CPUID_SEF_AVX512_VBMI2	__BIT(6)
#endif
#ifndef CPUID_SEF_AVX512VL
#define CPUID_SEF_AVX512VL	__BIT(31)
#endif
#ifndef CPUID_SEF_AVX512_VNNI
#define CPUID_SEF_AVX512_VNNI	__BIT(11)
#endif
#ifndef CPUID_SEF_AVX512_VP2INTERSECT
#define CPUID_SEF_AVX512_VP2INTERSECT	__BIT(8)
#endif
#ifndef CPUID_SEF_AVX512_VPOPCNTDQ
#define CPUID_SEF_AVX512_VPOPCNTDQ	__BIT(14)
#endif
#ifndef CPUID_SEF_BMI1
#define CPUID_SEF_BMI1	__BIT(3)
#endif
#ifndef CPUID_SEF_BMI2
#define CPUID_SEF_BMI2	__BIT(8)
#endif
#ifndef CPUID_SEF_CET_IBT
#define CPUID_SEF_CET_IBT	__BIT(20)
#endif
#ifndef CPUID_SEF_CET_SS
#define CPUID_SEF_CET_SS	__BIT(7)
#endif
#ifndef CPUID_SEF_CLDEMOTE
#define CPUID_SEF_CLDEMOTE	__BIT(25)
#endif
#ifndef CPUID_SEF_CLFLUSHOPT
#define CPUID_SEF_CLFLUSHOPT	__BIT(23)
#endif
#ifndef CPUID_SEF_CLWB
#define CPUID_SEF_CLWB	__BIT(24)
#endif
#ifndef CPUID_SEF_CORE_CAP
#define CPUID_SEF_CORE_CAP	__BIT(30)
#endif
#ifndef CPUID_SEF_ERMS
#define CPUID_SEF_ERMS	__BIT(9)
#endif
#ifndef CPUID_SEF_FDPEXONLY
#define CPUID_SEF_FDPEXONLY	__BIT(6)
#endif
#ifndef CPUID_SEF_FPUCSDS
#define CPUID_SEF_FPUCSDS	__BIT(13)
#endif
#ifndef CPUID_SEF_FSGSBASE
#define CPUID_SEF_FSGSBASE	__BIT(0)
#endif
#ifndef CPUID_SEF_FSRM
#define CPUID_SEF_FSRM	__BIT(4)
#endif
#ifndef CPUID_SEF_GFNI
#define CPUID_SEF_GFNI	__BIT(8)
#endif
#ifndef CPUID_SEF_HLE
#define CPUID_SEF_HLE	__BIT(4)
#endif
#ifndef CPUID_SEF_HYBRID
#define CPUID_SEF_HYBRID	__BIT(15)
#endif
#ifndef CPUID_SEF_IBRS
#define CPUID_SEF_IBRS	__BIT(26)
#endif
#ifndef CPUID_SEF_INVPCID
#define CPUID_SEF_INVPCID	__BIT(10)
#endif
#ifndef CPUID_SEF_MAWAU
#define CPUID_SEF_MAWAU	__BITS(21, 17)
#endif
#ifndef CPUID_SEF_MD_CLEAR
#define CPUID_SEF_MD_CLEAR	__BIT(10)
#endif
#ifndef CPUID_SEF_MOVDIR64B
#define CPUID_SEF_MOVDIR64B	__BIT(28)
#endif
#ifndef CPUID_SEF_MOVDIRI
#define CPUID_SEF_MOVDIRI	__BIT(27)
#endif
#ifndef CPUID_SEF_MPX
#define CPUID_SEF_MPX	__BIT(14)
#endif
#ifndef CPUID_SEF_OSPKE
#define CPUID_SEF_OSPKE	__BIT(4)
#endif
#ifndef CPUID_SEF_PKS
#define CPUID_SEF_PKS	__BIT(31)
#endif
#ifndef CPUID_SEF_PKU
#define CPUID_SEF_PKU	__BIT(3)
#endif
#ifndef CPUID_SEF_PQE
#define CPUID_SEF_PQE	__BIT(15)
#endif
#ifndef CPUID_SEF_PREFETCHWT1
#define CPUID_SEF_PREFETCHWT1	__BIT(0)
#endif
#ifndef CPUID_SEF_PT
#define CPUID_SEF_PT	__BIT(25)
#endif
#ifndef CPUID_SEF_QM
#define CPUID_SEF_QM	__BIT(12)
#endif
#ifndef CPUID_SEF_RDPID
#define CPUID_SEF_RDPID	__BIT(22)
#endif
#ifndef CPUID_SEF_RDSEED
#define CPUID_SEF_RDSEED	__BIT(18)
#endif
#ifndef CPUID_SEF_RTM
#define CPUID_SEF_RTM	__BIT(11)
#endif
#ifndef CPUID_SEF_SERIALIZE
#define CPUID_SEF_SERIALIZE	__BIT(14)
#endif
#ifndef CPUID_SEF_SGX
#define CPUID_SEF_SGX	__BIT(2)
#endif
#ifndef CPUID_SEF_SGXLC
#define CPUID_SEF_SGXLC	__BIT(30)
#endif
#ifndef CPUID_SEF_SHA
#define CPUID_SEF_SHA	__BIT(29)
#endif
#ifndef CPUID_SEF_SMAP
#define CPUID_SEF_SMAP	__BIT(20)
#endif
#ifndef CPUID_SEF_SMEP
#define CPUID_SEF_SMEP	__BIT(7)
#endif
#ifndef CPUID_SEF_SRBDS_CTRL
#define CPUID_SEF_SRBDS_CTRL	__BIT(9)
#endif
#ifndef CPUID_SEF_SSBD
#define CPUID_SEF_SSBD	__BIT(31)
#endif
#ifndef CPUID_SEF_STIBP
#define CPUID_SEF_STIBP	__BIT(27)
#endif
#ifndef CPUID_SEF_TSC_ADJUST
#define CPUID_SEF_TSC_ADJUST	__BIT(1)
#endif
#ifndef CPUID_SEF_TSXLDTRK
#define CPUID_SEF_TSXLDTRK	__BIT(16)
#endif
#ifndef CPUID_SEF_UMIP
#define CPUID_SEF_UMIP	__BIT(2)
#endif
#ifndef CPUID_SEF_VAES
#define CPUID_SEF_VAES	__BIT(9)
#endif
#ifndef CPUID_SEF_VPCLMULQDQ
#define CPUID_SEF_VPCLMULQDQ	__BIT(10)
#endif
#ifndef CPUID_SEF_WAITPKG
#define CPUID_SEF_WAITPKG	__BIT(5)
#endif
#ifndef CPUID_SKINIT
#define CPUID_SKINIT	__BIT(12)
#endif
#ifndef CPUID_SPM
#define CPUID_SPM	__BIT(25)
#endif
#ifndef CPUID_SSE4A
#define CPUID_SSE4A	__BIT(6)
#endif
#ifndef CPUID_SVM
#define CPUID_SVM	__BIT(2)
#endif
#ifndef CPUID_SYSCALL
#define CPUID_SYSCALL	__BIT(11)
#endif
#ifndef CPUID_TBM
#define CPUID_TBM	__BIT(21)
#endif
#ifndef CPUID_TCE
#define CPUID_TCE	__BIT(17)
#endif
#ifndef CPUID_TOP_LVLNUM
#define CPUID_TOP_LVLNUM	__BITS(7, 0)
#endif
#ifndef CPUID_TOP_LVLTYPE
#define CPUID_TOP_LVLTYPE	__BITS(15, 8)
#endif
#ifndef CPUID_TOP_LVLTYPE_CORE
#define CPUID_TOP_LVLTYPE_CORE	2
#endif
#ifndef CPUID_TOP_LVLTYPE_SMT
#define CPUID_TOP_LVLTYPE_SMT	1
#endif
#ifndef CPUID_TOPOEXT
#define CPUID_TOPOEXT	__BIT(22)
#endif
#ifndef CPUID_WDT
#define CPUID_WDT	__BIT(13)
#endif
#ifndef CPUID_XD
#define CPUID_XD	__BIT(20)
#endif
#ifndef CPUID_XOP
#define CPUID_XOP	__BIT(11)
#endif
#ifndef IA32_ARCH_MDS_NO
#define IA32_ARCH_MDS_NO	0x20
#endif
#ifndef IA32_ARCH_RDCL_NO
#define IA32_ARCH_RDCL_NO	0x01
#endif
#ifndef IA32_ARCH_SSB_NO
#define IA32_ARCH_SSB_NO	0x10
#endif
#ifndef IA32_ARCH_TAA_NO
#define IA32_ARCH_TAA_NO	0x100
#endif
#ifndef MSR_CR_PAT
#define MSR_CR_PAT	0x277
#endif
#ifndef MSR_MISC_ENABLE
#define MSR_MISC_ENABLE	0x1a0
#endif

/* <x86/pat.h>: memory types of the PAT entries. */
#define PAT_UC			0x0ULL
#define PAT_WC			0x1ULL
#define PAT_WT			0x4ULL
#define PAT_WP			0x5ULL
#define PAT_WB			0x6ULL
#define PAT_UCMINUS		0x7ULL
#define PATENTRY(n, type)	((uint64_t)(type) << ((n) * 8))

/* <x86/cpu_extended_state.h>: the 512-byte legacy area + XSAVE header. */
struct xsave_header {
	uint8_t xsh_fxsave[512];
	uint64_t xsh_xstate_bv;
	uint64_t xsh_xcomp_bv;
	uint8_t xsh_rsrvd[48];
} __packed;
_Static_assert(sizeof(struct xsave_header) == 512 + 64, "xsave_header");

/* <x86/cpufunc.h>: FreeBSD reads CR2 (rcr2) but has no writer. */
static inline void
lcr2(u_long val)
{
	__asm volatile("movq %0,%%cr2" : : "r" (val));
}

#define x86_cpuid2(leaf, subleaf, descs)	\
	cpuid_count((leaf), (subleaf), (u_int *)(descs))

/*
 * NetBSD's x86_xsave_features: the XCR0 bits the CPU supports,
 * CPUID[EAX=0x0d,ECX=0].EDX:EAX. FreeBSD only keeps xsave_mask, the
 * subset it enabled for itself.
 */
static inline uint64_t
nvmm_x86_cpu_xsave_features(void)
{
	u_int descs[4];

	if ((cpu_feature2 & CPUID2_XSAVE) == 0)
		return 0;
	cpuid_count(0x0d, 0, descs);
	return ((uint64_t)descs[3] << 32) | descs[0];
}
#define x86_xsave_features	nvmm_x86_cpu_xsave_features()

#endif /* _NVMM_X86_FREEBSD_H_ */
