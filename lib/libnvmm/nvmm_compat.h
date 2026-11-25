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
 * Private to libnvmm and its tests, not installed: the NetBSD x86 headers
 * the emulator uses (<machine/vmparam.h>, <machine/pte.h>,
 * <machine/psl.h>, <x86/specialreg.h>) mapped to FreeBSD's, plus the few
 * names FreeBSD spells differently. The page-table bits are the guest's
 * architectural ones, so they are spelled out as in DragonFly's libnvmm
 * rather than taken from FreeBSD's pmap.
 */

#ifndef _NVMM_COMPAT_H_
#define _NVMM_COMPAT_H_

#include <inttypes.h>			/* PRIx64, as NetBSD gets it */
#include <machine/param.h>		/* PAGE_SIZE, PAGE_MASK */
#include <machine/psl.h>		/* PSL_C ... PSL_D */
#include <machine/specialreg.h>		/* CR0_*, CR4_*, EFER_*, MSR_EFER */

#include <dev/nvmm/nvmm_bitops.h>	/* __BIT, __BITS, __SHIFTIN/OUT */

/* RFLAGS bit 1, always set. */
#define PSL_MBO		PSL_RESERVED_DEFAULT

/* x86 page-table entry bits. */
#define PTE_P		0x0000000000000001ULL	/* Present */
#define PTE_W		0x0000000000000002ULL	/* Write */
#define PTE_U		0x0000000000000004ULL	/* User */
#define PTE_PS		0x0000000000000080ULL	/* Large Page Size */
#define PTE_NX		0x8000000000000000ULL	/* No Execute */
#define PTE_FRAME	0x000ffffffffff000ULL	/* 4KB frame */

/* A long-mode page-table entry, as the tests build guest page tables. */
typedef uint64_t pt_entry_t;

#endif /* _NVMM_COMPAT_H_ */
