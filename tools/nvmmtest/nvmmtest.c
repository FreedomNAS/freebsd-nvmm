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
 * Phase 3 acceptance test, with raw ioctls (libnvmm is phase 6):
 * machines and vCPUs up to the limits, the reset state, a setstate/getstate
 * round trip, vCPU configuration, event injection, the requests that must
 * still answer ENOSYS, and a process that exits without destroying
 * anything. After each step the hw.nvmm counters must be back to where
 * they started.
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/wait.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <dev/nvmm/nvmm_ioctl.h>

#define CR0_NE		0x00000020
#define CR4_PAE		0x00000020
#define CR4_OSFXSR	0x00000200
#define EFER_LME	0x00000100
#define EFER_LMA	0x00000400

struct counters {
	u_int machines;
	u_int vcpus;
	u_long kpages;
	u_long vmobjs;
};

static struct nvmm_capability cap;
static struct counters base;
static int nfail;

static void
check(bool ok, const char *fmt, ...)
{
	va_list ap;

	if (ok)
		return;
	nfail++;
	printf("  FAIL: ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
}

static void
get_counters(struct counters *c)
{
	size_t len;

	len = sizeof(c->machines);
	if (sysctlbyname("hw.nvmm.machines", &c->machines, &len, NULL, 0))
		err(1, "hw.nvmm.machines");
	len = sizeof(c->vcpus);
	if (sysctlbyname("hw.nvmm.vcpus", &c->vcpus, &len, NULL, 0))
		err(1, "hw.nvmm.vcpus");
	len = sizeof(c->kpages);
	if (sysctlbyname("hw.nvmm.kpages", &c->kpages, &len, NULL, 0))
		err(1, "hw.nvmm.kpages");
	len = sizeof(c->vmobjs);
	if (sysctlbyname("hw.nvmm.vmobjs", &c->vmobjs, &len, NULL, 0))
		err(1, "hw.nvmm.vmobjs");
}

static void
print_counters(const char *when, const struct counters *c)
{
	printf("  %-22s machines %u, vcpus %u, kpages %lu, vmobjs %lu\n",
	    when, c->machines, c->vcpus, c->kpages, c->vmobjs);
}

static void
check_counters(const char *step)
{
	struct counters c;

	get_counters(&c);
	check(c.machines == base.machines && c.vcpus == base.vcpus &&
	    c.kpages == base.kpages && c.vmobjs == base.vmobjs,
	    "%s: counters not back (machines %u, vcpus %u, kpages %lu, "
	    "vmobjs %lu)", step, c.machines, c.vcpus, c.kpages, c.vmobjs);
}

static int
open_nvmm(void)
{
	struct nvmm_ioc_capability args;
	int fd;

	fd = open("/dev/nvmm", O_RDWR | O_CLOEXEC);
	if (fd == -1)
		err(1, "open /dev/nvmm");
	memset(&args, 0, sizeof(args));
	if (ioctl(fd, NVMM_IOC_CAPABILITY, &args) == -1)
		err(1, "NVMM_IOC_CAPABILITY");
	cap = args.cap;
	return fd;
}

static int
machine_create(int fd, nvmm_machid_t *machid)
{
	struct nvmm_ioc_machine_create args;

	memset(&args, 0, sizeof(args));
	if (ioctl(fd, NVMM_IOC_MACHINE_CREATE, &args) == -1)
		return errno;
	*machid = args.machid;
	return 0;
}

static int
machine_destroy(int fd, nvmm_machid_t machid)
{
	struct nvmm_ioc_machine_destroy args = { .machid = machid };

	return ioctl(fd, NVMM_IOC_MACHINE_DESTROY, &args) == -1 ? errno : 0;
}

static int
vcpu_create(int fd, nvmm_machid_t machid, nvmm_cpuid_t cpuid)
{
	struct nvmm_ioc_vcpu_create args = { .machid = machid, .cpuid = cpuid };

	return ioctl(fd, NVMM_IOC_VCPU_CREATE, &args) == -1 ? errno : 0;
}

static int
vcpu_destroy(int fd, nvmm_machid_t machid, nvmm_cpuid_t cpuid)
{
	struct nvmm_ioc_vcpu_destroy args = { .machid = machid, .cpuid = cpuid };

	return ioctl(fd, NVMM_IOC_VCPU_DESTROY, &args) == -1 ? errno : 0;
}

static struct nvmm_comm_page *
comm_map(int fd, nvmm_machid_t machid, nvmm_cpuid_t cpuid)
{
	void *p;

	p = mmap(NULL, getpagesize(), PROT_READ | PROT_WRITE, MAP_SHARED,
	    fd, NVMM_COMM_OFF(machid, cpuid));
	if (p == MAP_FAILED)
		err(1, "mmap comm page %u/%u", machid, cpuid);
	return p;
}

static int
vcpu_ioctl(int fd, unsigned long cmd, nvmm_machid_t machid,
    nvmm_cpuid_t cpuid)
{
	struct nvmm_ioc_vcpu_getstate args = { .machid = machid, .cpuid = cpuid };

	return ioctl(fd, cmd, &args) == -1 ? errno : 0;
}

static int
vcpu_configure(int fd, nvmm_machid_t machid, nvmm_cpuid_t cpuid,
    uint64_t op, void *conf)
{
	struct nvmm_ioc_vcpu_configure args = {
		.machid = machid, .cpuid = cpuid, .op = op, .conf = conf
	};

	return ioctl(fd, NVMM_IOC_VCPU_CONFIGURE, &args) == -1 ? errno : 0;
}

/* -------------------------------------------------------------------------- */

static void
test_machines(int fd)
{
	nvmm_machid_t *ids, extra;
	u_int i, n = 0;
	int error;

	printf("machines: create %u, one more, destroy all\n",
	    cap.max_machines);
	ids = calloc(cap.max_machines, sizeof(*ids));
	if (ids == NULL)
		err(1, "calloc");
	for (i = 0; i < cap.max_machines; i++) {
		error = machine_create(fd, &ids[i]);
		check(error == 0, "machine %u: %s", i, strerror(error));
		if (error)
			break;
		n++;
	}
	error = machine_create(fd, &extra);
	check(error == ENOBUFS, "machine %u: expected ENOBUFS, got %s",
	    cap.max_machines, error ? strerror(error) : "success");
	if (error == 0)
		(void)machine_destroy(fd, extra);
	for (i = 0; i < n; i++) {
		error = machine_destroy(fd, ids[i]);
		check(error == 0, "destroy machine %u: %s", ids[i],
		    strerror(error));
	}
	if (n > 0) {
		error = machine_destroy(fd, ids[0]);
		check(error != 0, "destroying machine %u twice succeeded",
		    ids[0]);
	}
	free(ids);
	check_counters("machines");
}

static void
test_vcpus(int fd)
{
	nvmm_machid_t machid;
	u_int i, n = 0;
	int error;

	printf("vcpus: create %u in one machine, one more, destroy all\n",
	    cap.max_vcpus);
	error = machine_create(fd, &machid);
	check(error == 0, "machine: %s", strerror(error));
	if (error)
		return;
	for (i = 0; i < cap.max_vcpus; i++) {
		error = vcpu_create(fd, machid, i);
		check(error == 0, "vcpu %u: %s", i, strerror(error));
		if (error)
			break;
		n++;
	}
	error = vcpu_create(fd, machid, cap.max_vcpus);
	check(error != 0, "vcpu %u: created beyond the limit",
	    cap.max_vcpus);
	error = vcpu_create(fd, machid, 0);
	check(error == EBUSY, "vcpu 0 twice: expected EBUSY, got %s",
	    error ? strerror(error) : "success");
	/* Half destroyed one by one, the rest with the machine. */
	for (i = 0; i < n / 2; i++) {
		error = vcpu_destroy(fd, machid, i);
		check(error == 0, "destroy vcpu %u: %s", i, strerror(error));
	}
	error = machine_destroy(fd, machid);
	check(error == 0, "destroy machine: %s", strerror(error));
	check_counters("vcpus");
}

static void
test_state(int fd)
{
	struct nvmm_comm_page *comm;
	struct nvmm_x64_state *st, want;
	nvmm_machid_t machid;
	int error, i;

	printf("state: reset values, setstate/getstate round trip\n");
	error = machine_create(fd, &machid);
	check(error == 0, "machine: %s", strerror(error));
	if (error)
		return;
	error = vcpu_create(fd, machid, 0);
	check(error == 0, "vcpu: %s", strerror(error));
	if (error)
		goto out;
	comm = comm_map(fd, machid, 0);
	st = &comm->state;

	/* The RESET state, as installed by vcpu_create. */
	memset(st, 0, sizeof(*st));
	comm->state_wanted = NVMM_X64_STATE_ALL;
	comm->state_cached = 0;
	error = vcpu_ioctl(fd, NVMM_IOC_VCPU_GETSTATE, machid, 0);
	check(error == 0, "getstate: %s", strerror(error));
	check(st->gprs[NVMM_X64_GPR_RIP] == 0xFFF0, "reset RIP 0x%jx",
	    (uintmax_t)st->gprs[NVMM_X64_GPR_RIP]);
	check(st->gprs[NVMM_X64_GPR_RDX] == 0x600, "reset RDX 0x%jx",
	    (uintmax_t)st->gprs[NVMM_X64_GPR_RDX]);
	check(st->segs[NVMM_X64_SEG_CS].selector == 0xF000 &&
	    st->segs[NVMM_X64_SEG_CS].base == 0xFFFF0000,
	    "reset CS %04x base 0x%jx", st->segs[NVMM_X64_SEG_CS].selector,
	    (uintmax_t)st->segs[NVMM_X64_SEG_CS].base);
	check((st->crs[NVMM_X64_CR_CR0] & ~(uint64_t)CR0_NE) == 0x60000010,
	    "reset CR0 0x%jx", (uintmax_t)st->crs[NVMM_X64_CR_CR0]);
	check(st->crs[NVMM_X64_CR_CR4] == 0, "reset CR4 0x%jx (VMXE hidden?)",
	    (uintmax_t)st->crs[NVMM_X64_CR_CR4]);
	check(st->drs[NVMM_X64_DR_DR7] == 0x400, "reset DR7 0x%jx",
	    (uintmax_t)st->drs[NVMM_X64_DR_DR7]);
	check(st->fpu.fx_cw == 0x40 && st->fpu.fx_mxcsr == 0x1F80,
	    "reset FPU cw 0x%x mxcsr 0x%x", st->fpu.fx_cw, st->fpu.fx_mxcsr);

	/* A 64-bit state, written then read back. */
	want = *st;
	for (i = 0; i < NVMM_X64_NGPR; i++)
		want.gprs[i] = 0x1111111100000000ULL + (uint64_t)i;
	want.gprs[NVMM_X64_GPR_RIP] = 0xFFFFFFFF80001000ULL;
	want.gprs[NVMM_X64_GPR_RSP] = 0xFFFFFFFF80200000ULL;
	want.gprs[NVMM_X64_GPR_RFLAGS] = 0x202;
	want.segs[NVMM_X64_SEG_CS].selector = 0x08;
	want.segs[NVMM_X64_SEG_CS].base = 0;
	want.segs[NVMM_X64_SEG_CS].limit = 0xFFFFFFFF;
	want.segs[NVMM_X64_SEG_CS].attrib.type = 11;
	want.segs[NVMM_X64_SEG_CS].attrib.l = 1;
	want.segs[NVMM_X64_SEG_CS].attrib.g = 1;
	want.segs[NVMM_X64_SEG_GDT].base = 0xFFFFFFFF80003000ULL;
	want.segs[NVMM_X64_SEG_GDT].limit = 0x37;
	want.crs[NVMM_X64_CR_CR0] = 0x80010031;	/* PG WP NE ET PE */
	want.crs[NVMM_X64_CR_CR2] = 0x123000;
	want.crs[NVMM_X64_CR_CR3] = 0x5000;
	want.crs[NVMM_X64_CR_CR4] = CR4_PAE | CR4_OSFXSR;
	want.crs[NVMM_X64_CR_CR8] = 3;
	want.drs[NVMM_X64_DR_DR0] = 0x1000;
	want.drs[NVMM_X64_DR_DR6] = 0xFFFF0FF0;
	want.drs[NVMM_X64_DR_DR7] = 0x401;
	want.msrs[NVMM_X64_MSR_EFER] = EFER_LME | EFER_LMA;
	want.msrs[NVMM_X64_MSR_STAR] = 0x0023001000000000ULL;
	want.msrs[NVMM_X64_MSR_LSTAR] = 0xFFFFFFFF80002000ULL;
	want.msrs[NVMM_X64_MSR_SFMASK] = 0x47700;
	want.msrs[NVMM_X64_MSR_KERNELGSBASE] = 0xFFFFFFFF80004000ULL;
	want.msrs[NVMM_X64_MSR_SYSENTER_CS] = 0x10;
	want.msrs[NVMM_X64_MSR_TSC] = 123456789;
	want.fpu.fx_cw = 0x37F;
	want.fpu.fx_mxcsr = 0x1F80;
	memset(&want.fpu.fx_xmm[3], 0xA5, sizeof(want.fpu.fx_xmm[3]));

	*st = want;
	comm->state_wanted = NVMM_X64_STATE_ALL;
	error = vcpu_ioctl(fd, NVMM_IOC_VCPU_SETSTATE, machid, 0);
	check(error == 0, "setstate: %s", strerror(error));

	memset(st, 0, sizeof(*st));
	comm->state_wanted = NVMM_X64_STATE_ALL;
	comm->state_cached = 0;
	error = vcpu_ioctl(fd, NVMM_IOC_VCPU_GETSTATE, machid, 0);
	check(error == 0, "getstate: %s", strerror(error));

	check(memcmp(st->gprs, want.gprs, sizeof(want.gprs)) == 0,
	    "GPRs differ (RIP 0x%jx RAX 0x%jx)",
	    (uintmax_t)st->gprs[NVMM_X64_GPR_RIP],
	    (uintmax_t)st->gprs[NVMM_X64_GPR_RAX]);
	check(st->segs[NVMM_X64_SEG_CS].selector == 0x08 &&
	    st->segs[NVMM_X64_SEG_CS].attrib.l == 1 &&
	    st->segs[NVMM_X64_SEG_CS].limit == 0xFFFFFFFF,
	    "CS differs");
	check(st->segs[NVMM_X64_SEG_GDT].base == 0xFFFFFFFF80003000ULL &&
	    st->segs[NVMM_X64_SEG_GDT].limit == 0x37, "GDTR differs");
	for (i = 0; i < NVMM_X64_NCR; i++) {
		if (i == NVMM_X64_CR_XCR0)
			continue;
		check(st->crs[i] == want.crs[i], "CR index %d: 0x%jx, want "
		    "0x%jx", i, (uintmax_t)st->crs[i],
		    (uintmax_t)want.crs[i]);
	}
	check(st->drs[NVMM_X64_DR_DR0] == 0x1000 &&
	    st->drs[NVMM_X64_DR_DR7] == 0x401, "DRs differ");
	for (i = 0; i < NVMM_X64_NMSR; i++) {
		check(st->msrs[i] == want.msrs[i], "MSR index %d: 0x%jx, want "
		    "0x%jx", i, (uintmax_t)st->msrs[i],
		    (uintmax_t)want.msrs[i]);
	}
	check(st->fpu.fx_cw == 0x37F && memcmp(&st->fpu.fx_xmm[3],
	    &want.fpu.fx_xmm[3], sizeof(want.fpu.fx_xmm[3])) == 0,
	    "FPU differs");

	munmap(comm, getpagesize());
out:
	error = machine_destroy(fd, machid);
	check(error == 0, "destroy machine: %s", strerror(error));
	check_counters("state");
}

static void
test_configure_inject(int fd)
{
	struct nvmm_vcpu_conf_cpuid cpuid;
	struct nvmm_vcpu_conf_tpr tpr;
	struct nvmm_comm_page *comm;
	nvmm_machid_t machid;
	uint64_t xcr0;
	int error;

	printf("configure, inject, requests still ENOSYS\n");
	error = machine_create(fd, &machid);
	check(error == 0, "machine: %s", strerror(error));
	if (error)
		return;
	error = vcpu_create(fd, machid, 0);
	check(error == 0, "vcpu: %s", strerror(error));
	if (error)
		goto out;
	comm = comm_map(fd, machid, 0);

	memset(&cpuid, 0, sizeof(cpuid));
	cpuid.mask = 1;
	cpuid.leaf = 0x00000001;
	cpuid.u.mask.del.ecx = 0x00000020;	/* hide VMX */
	error = vcpu_configure(fd, machid, 0, NVMM_VCPU_CONF_CPUID, &cpuid);
	check(error == 0, "conf cpuid: %s", strerror(error));
	cpuid.u.mask.set.ecx = 0x00000020;	/* set and del: invalid */
	error = vcpu_configure(fd, machid, 0, NVMM_VCPU_CONF_CPUID, &cpuid);
	check(error == EINVAL, "conf cpuid set&del: expected EINVAL, got %s",
	    error ? strerror(error) : "success");

	memset(&tpr, 0, sizeof(tpr));
	tpr.exit_changed = 1;
	error = vcpu_configure(fd, machid, 0, NVMM_VCPU_CONF_TPR, &tpr);
	check(error == 0, "conf tpr: %s", strerror(error));

	xcr0 = cap.arch.xcr0_mask;
	error = vcpu_configure(fd, machid, 0, NVMM_VCPU_CONF_XCR0_MASK,
	    &xcr0);
	check(error == 0, "conf xcr0 0x%jx: %s", (uintmax_t)xcr0,
	    strerror(error));
	xcr0 = 0x4;	/* AVX without x87/SSE: invalid */
	error = vcpu_configure(fd, machid, 0, NVMM_VCPU_CONF_XCR0_MASK,
	    &xcr0);
	check(error == EINVAL, "conf xcr0 0x4: expected EINVAL, got %s",
	    error ? strerror(error) : "success");

	memset(&comm->event, 0, sizeof(comm->event));
	comm->event.type = NVMM_VCPU_EVENT_EXCP;
	comm->event.vector = 13;	/* #GP */
	error = vcpu_ioctl(fd, NVMM_IOC_VCPU_INJECT, machid, 0);
	check(error == 0, "inject #GP: %s", strerror(error));
	comm->event.vector = 2;		/* NMI as an exception: invalid */
	error = vcpu_ioctl(fd, NVMM_IOC_VCPU_INJECT, machid, 0);
	check(error == EINVAL, "inject exception 2: expected EINVAL, got %s",
	    error ? strerror(error) : "success");
	comm->state_wanted = NVMM_X64_STATE_INTR;
	comm->state_cached = 0;
	error = vcpu_ioctl(fd, NVMM_IOC_VCPU_GETSTATE, machid, 0);
	check(error == 0 && comm->state.intr.evt_pending,
	    "event not pending after inject");

	error = vcpu_ioctl(fd, NVMM_IOC_VCPU_RUN, machid, 0);
	check(error == ENOSYS, "VCPU_RUN: expected ENOSYS, got %s",
	    error ? strerror(error) : "success");
	{
		struct nvmm_ioc_gpa_map gm = { .machid = machid };
		struct nvmm_ioc_hva_map hm = { .machid = machid };

		error = ioctl(fd, NVMM_IOC_GPA_MAP, &gm) == -1 ? errno : 0;
		check(error == ENOSYS, "GPA_MAP: expected ENOSYS, got %s",
		    error ? strerror(error) : "success");
		error = ioctl(fd, NVMM_IOC_HVA_MAP, &hm) == -1 ? errno : 0;
		check(error == ENOSYS, "HVA_MAP: expected ENOSYS, got %s",
		    error ? strerror(error) : "success");
	}

	munmap(comm, getpagesize());
out:
	error = machine_destroy(fd, machid);
	check(error == 0, "destroy machine: %s", strerror(error));
	check_counters("configure/inject");
}

/*
 * A child opens its own /dev/nvmm, creates machines and vCPUs, maps their
 * comm pages and exits without destroying anything: closing the
 * descriptor at exit must free everything.
 */
static void
test_exit_without_destroy(void)
{
	struct counters mid;
	pid_t pid;
	int status;

	printf("exit without destroy: 4 machines x 4 vcpus in a child\n");
	pid = fork();
	if (pid == -1)
		err(1, "fork");
	if (pid == 0) {
		nvmm_machid_t machid;
		u_int m, c;
		int fd;

		fd = open_nvmm();
		for (m = 0; m < 4; m++) {
			if (machine_create(fd, &machid) != 0)
				_exit(2);
			for (c = 0; c < 4; c++) {
				if (vcpu_create(fd, machid, c) != 0)
					_exit(3);
				(void)comm_map(fd, machid, c);
			}
		}
		get_counters(&mid);
		print_counters("in the child:", &mid);
		_exit(0);
	}
	if (waitpid(pid, &status, 0) == -1)
		err(1, "waitpid");
	check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	    "child failed (status 0x%x)", status);
	check_counters("exit without destroy");
}

int
main(void)
{
	struct counters end;
	int fd;

	fd = open_nvmm();
	get_counters(&base);
	print_counters("at start:", &base);

	test_machines(fd);
	test_vcpus(fd);
	test_state(fd);
	test_configure_inject(fd);
	close(fd);
	test_exit_without_destroy();

	get_counters(&end);
	print_counters("at end:", &end);
	if (nfail != 0) {
		printf("FAILED: %d check(s)\n", nfail);
		return 1;
	}
	printf("OK\n");
	return 0;
}
