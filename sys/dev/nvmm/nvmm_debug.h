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
 * Test-only request, not part of the NVMM ABI: it exists only in a module
 * built with -DNVMM_DEBUG (decision P4-1 in PORTING.md). It reaches one
 * page of guest physical memory the way the guest would, through a fault
 * on the EPT vmspace and the physical page its pmap then maps, and copies
 * it to or from userland. hw.nvmm.debug says whether the module has it.
 */

#ifndef _NVMM_DEBUG_H_
#define _NVMM_DEBUG_H_

#include <sys/ioccom.h>
#include <dev/nvmm/nvmm.h>

struct nvmm_ioc_debug_gpa {
	nvmm_machid_t machid;
	gpaddr_t gpa;		/* page-aligned guest physical address */
	int write;		/* 0: page -> buf, 1: buf -> page */
	void *buf;		/* one page */
};

#define NVMM_IOC_DEBUG_GPA	_IOW('N', 100, struct nvmm_ioc_debug_gpa)

#endif /* _NVMM_DEBUG_H_ */
