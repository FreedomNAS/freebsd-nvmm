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
