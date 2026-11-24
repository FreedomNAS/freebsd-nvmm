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
 * Phase 1 acceptance test: query the NVMM capability through the raw
 * ioctl (libnvmm is not ported yet) and check that the requests not
 * implemented yet answer ENOSYS.
 */

#include <sys/types.h>
#include <sys/ioctl.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <dev/nvmm/nvmm_ioctl.h>

int
main(void)
{
	struct nvmm_ioc_capability args;
	struct nvmm_ioc_machine_create mc;
	const struct nvmm_capability *cap = &args.cap;
	int fd, rv = 0;

	fd = open("/dev/nvmm", O_RDONLY | O_CLOEXEC);
	if (fd == -1)
		err(1, "open /dev/nvmm");

	memset(&args, 0, sizeof(args));
	if (ioctl(fd, NVMM_IOC_CAPABILITY, &args) == -1)
		err(1, "NVMM_IOC_CAPABILITY");

	printf("version            %u\n", cap->version);
	printf("state_size         %u\n", cap->state_size);
	printf("max_machines       %u\n", cap->max_machines);
	printf("max_vcpus          %u\n", cap->max_vcpus);
	printf("max_ram            %ju GiB\n", (uintmax_t)(cap->max_ram >> 30));
	printf("mach_conf_support  0x%jx\n",
	    (uintmax_t)cap->arch.mach_conf_support);
	printf("vcpu_conf_support  0x%jx\n",
	    (uintmax_t)cap->arch.vcpu_conf_support);
	printf("xcr0_mask          0x%jx\n", (uintmax_t)cap->arch.xcr0_mask);
	printf("mxcsr_mask         0x%x\n", cap->arch.mxcsr_mask);
	printf("conf_cpuid_maxops  %u\n", cap->arch.conf_cpuid_maxops);

	if (cap->version != NVMM_KERN_VERSION) {
		warnx("kernel version %u, headers %u", cap->version,
		    NVMM_KERN_VERSION);
		rv = 1;
	}
	if (cap->state_size != sizeof(struct nvmm_x64_state)) {
		warnx("state_size %u, headers %zu", cap->state_size,
		    sizeof(struct nvmm_x64_state));
		rv = 1;
	}

	memset(&mc, 0, sizeof(mc));
	if (ioctl(fd, NVMM_IOC_MACHINE_CREATE, &mc) != -1 || errno != ENOSYS) {
		warnx("NVMM_IOC_MACHINE_CREATE: expected ENOSYS in phase 1");
		rv = 1;
	} else {
		printf("machine_create     ENOSYS (expected in phase 1)\n");
	}

	close(fd);
	printf("%s\n", rv == 0 ? "OK" : "FAIL");
	return rv;
}
