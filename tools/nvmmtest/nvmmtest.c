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
 * Acceptance test of phases 3 and 4, with raw ioctls (libnvmm is phase
 * 6): machines and vCPUs up to the limits, the reset state, a
 * setstate/getstate round trip, vCPU configuration, event injection, guest
 * memory (host and guest mappings, their errors), micro-guests run with
 * vcpu_run (phase 5), and processes that exit without destroying anything. After each step the
 * hw.nvmm counters must be back to where they started.
 *
 * With a module built with -DNVMM_DEBUG, the memory test also goes through
 * the EPT: what the host writes the guest reads, and the other way round.
 */

#include <sys/types.h>
#include <sys/cpuset.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/wait.h>

#include <err.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <dev/nvmm/nvmm_ioctl.h>
#include <dev/nvmm/nvmm_debug.h>

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
static bool have_debug;
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

	printf("configure, inject\n");
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


	munmap(comm, getpagesize());
out:
	error = machine_destroy(fd, machid);
	check(error == 0, "destroy machine: %s", strerror(error));
	check_counters("configure/inject");
}

/* -------------------------------------------------------------------------- */

static int
hva_map(int fd, nvmm_machid_t machid, void *hva, size_t size)
{
	struct nvmm_ioc_hva_map args = {
		.machid = machid, .hva = (uintptr_t)hva, .size = size
	};

	return ioctl(fd, NVMM_IOC_HVA_MAP, &args) == -1 ? errno : 0;
}

static int
hva_unmap(int fd, nvmm_machid_t machid, void *hva, size_t size)
{
	struct nvmm_ioc_hva_unmap args = {
		.machid = machid, .hva = (uintptr_t)hva, .size = size
	};

	return ioctl(fd, NVMM_IOC_HVA_UNMAP, &args) == -1 ? errno : 0;
}

static int
gpa_map(int fd, nvmm_machid_t machid, void *hva, gpaddr_t gpa, size_t size,
    int prot)
{
	struct nvmm_ioc_gpa_map args = {
		.machid = machid, .hva = (uintptr_t)hva, .gpa = gpa,
		.size = size, .prot = prot
	};

	return ioctl(fd, NVMM_IOC_GPA_MAP, &args) == -1 ? errno : 0;
}

static int
gpa_unmap(int fd, nvmm_machid_t machid, gpaddr_t gpa, size_t size)
{
	struct nvmm_ioc_gpa_unmap args = {
		.machid = machid, .gpa = gpa, .size = size
	};

	return ioctl(fd, NVMM_IOC_GPA_UNMAP, &args) == -1 ? errno : 0;
}

static int
debug_gpa(int fd, nvmm_machid_t machid, gpaddr_t gpa, int write, void *buf)
{
	struct nvmm_ioc_debug_gpa args = {
		.machid = machid, .gpa = gpa, .write = write, .buf = buf
	};

	return ioctl(fd, NVMM_IOC_DEBUG_GPA, &args) == -1 ? errno : 0;
}

#define MEM_NPAGES	8
#define GPA_RW		0x100000ULL
#define GPA_RO		0x200000ULL
#define GPA_NONE	0x300000ULL

static void
fill(void *buf, size_t len, uint8_t seed)
{
	uint8_t *p = buf;
	size_t i;

	for (i = 0; i < len; i++)
		p[i] = (uint8_t)(seed + i * 7);
}

static void
test_memory(int fd)
{
	size_t ps = (size_t)getpagesize(), size = MEM_NPAGES * ps;
	nvmm_machid_t machid, machid2;
	uint8_t *hva, *other, *buf, *want;
	int error;

	printf("memory: host and guest mappings%s\n", have_debug ?
	    ", data through the EPT" : " (no debug module: no EPT data check)");
	buf = malloc(ps);
	want = malloc(ps);
	if (buf == NULL || want == NULL)
		err(1, "malloc");
	error = machine_create(fd, &machid);
	check(error == 0, "machine: %s", strerror(error));
	if (error)
		return;

	hva = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE,
	    -1, 0);
	other = mmap(NULL, ps, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE,
	    -1, 0);
	if (hva == MAP_FAILED || other == MAP_FAILED)
		err(1, "mmap");

	/* Host mappings. */
	error = hva_map(fd, machid, hva, size);
	check(error == 0, "hva_map: %s", strerror(error));
	error = hva_map(fd, machid, hva + 4 * ps, size);
	check(error == EEXIST, "hva_map overlapping: expected EEXIST, got %s",
	    error ? strerror(error) : "success");
	error = hva_map(fd, machid, hva + 1, ps);
	check(error == EINVAL, "hva_map unaligned: expected EINVAL, got %s",
	    error ? strerror(error) : "success");

	/* Guest mappings: one read-write, one read-only alias. */
	error = gpa_map(fd, machid, hva, GPA_RW, size, PROT_READ | PROT_WRITE);
	check(error == 0, "gpa_map rw: %s", strerror(error));
	error = gpa_map(fd, machid, hva, GPA_RO, size, PROT_READ);
	check(error == 0, "gpa_map ro: %s", strerror(error));
	error = gpa_map(fd, machid, other, GPA_NONE, ps, PROT_READ);
	check(error == EINVAL, "gpa_map of a non-hva_map'd address: expected "
	    "EINVAL, got %s", error ? strerror(error) : "success");
	error = gpa_map(fd, machid, hva, cap.max_ram, ps, PROT_READ);
	check(error == EINVAL, "gpa_map beyond max_ram: expected EINVAL, "
	    "got %s", error ? strerror(error) : "success");
	error = gpa_map(fd, machid, hva, GPA_NONE + 1, ps, PROT_READ);
	check(error == EINVAL, "gpa_map unaligned: expected EINVAL, got %s",
	    error ? strerror(error) : "success");
	error = gpa_map(fd, machid, hva, GPA_NONE, ps, 0x8);
	check(error == EINVAL, "gpa_map bad prot: expected EINVAL, got %s",
	    error ? strerror(error) : "success");

	if (have_debug) {
		/* Host writes, guest reads (both mappings). */
		fill(hva + 2 * ps, ps, 0x11);
		error = debug_gpa(fd, machid, GPA_RW + 2 * ps, 0, buf);
		check(error == 0 && memcmp(buf, hva + 2 * ps, ps) == 0,
		    "guest read of a host write (rw): %s",
		    error ? strerror(error) : "data differs");
		error = debug_gpa(fd, machid, GPA_RO + 2 * ps, 0, buf);
		check(error == 0 && memcmp(buf, hva + 2 * ps, ps) == 0,
		    "guest read of a host write (ro alias): %s",
		    error ? strerror(error) : "data differs");

		/* Guest writes, host reads. */
		fill(want, ps, 0x5A);
		error = debug_gpa(fd, machid, GPA_RW + 3 * ps, 1, want);
		check(error == 0 && memcmp(hva + 3 * ps, want, ps) == 0,
		    "host read of a guest write: %s",
		    error ? strerror(error) : "data differs");

		/* A page never touched by the host reads as zeros. */
		memset(want, 0, ps);
		error = debug_gpa(fd, machid, GPA_RW + 7 * ps, 0, buf);
		check(error == 0 && memcmp(buf, want, ps) == 0,
		    "untouched page: %s",
		    error ? strerror(error) : "not zero");

		/* Read-only alias and unmapped space must fault. */
		error = debug_gpa(fd, machid, GPA_RO + 3 * ps, 1, want);
		check(error == EFAULT, "guest write to read-only: expected "
		    "EFAULT, got %s", error ? strerror(error) : "success");
		error = debug_gpa(fd, machid, GPA_NONE, 0, buf);
		check(error == EFAULT, "guest read of unmapped: expected "
		    "EFAULT, got %s", error ? strerror(error) : "success");
	}

	/* Unmap the rw range: it faults, the alias still works. */
	error = gpa_unmap(fd, machid, GPA_RW, size);
	check(error == 0, "gpa_unmap: %s", strerror(error));
	if (have_debug) {
		error = debug_gpa(fd, machid, GPA_RW + 2 * ps, 0, buf);
		check(error == EFAULT, "read after gpa_unmap: expected EFAULT, "
		    "got %s", error ? strerror(error) : "success");
		error = debug_gpa(fd, machid, GPA_RO + 3 * ps, 0, buf);
		check(error == 0 && memcmp(buf, hva + 3 * ps, ps) == 0,
		    "alias after unmapping the other range: %s",
		    error ? strerror(error) : "data differs");
	}
	error = gpa_unmap(fd, machid, GPA_RO, size);
	check(error == 0, "gpa_unmap alias: %s", strerror(error));
	error = hva_unmap(fd, machid, hva, size);
	check(error == 0, "hva_unmap: %s", strerror(error));
	error = hva_unmap(fd, machid, hva, size);
	check(error == ENOENT, "hva_unmap twice: expected ENOENT, got %s",
	    error ? strerror(error) : "success");
	error = machine_destroy(fd, machid);
	check(error == 0, "destroy machine: %s", strerror(error));
	check_counters("memory");

	/* A machine destroyed with its memory still mapped. */
	error = machine_create(fd, &machid2);
	check(error == 0, "machine: %s", strerror(error));
	if (error == 0) {
		munmap(hva, size);
		hva = mmap(NULL, size, PROT_READ | PROT_WRITE,
		    MAP_ANON | MAP_PRIVATE, -1, 0);
		if (hva == MAP_FAILED)
			err(1, "mmap");
		check(hva_map(fd, machid2, hva, size) == 0 &&
		    gpa_map(fd, machid2, hva, GPA_RW, size,
		    PROT_READ | PROT_WRITE) == 0, "maps for destroy test");
		memset(hva, 0x77, size);
		if (have_debug)
			(void)debug_gpa(fd, machid2, GPA_RW, 0, buf);
		error = machine_destroy(fd, machid2);
		check(error == 0, "destroy with memory mapped: %s",
		    strerror(error));
		/* The host mapping stays usable until munmap. */
		check(hva[size - 1] == 0x77, "host memory after destroy");
		check_counters("destroy with memory mapped");
	}

	munmap(hva, size);
	munmap(other, ps);
	free(buf);
	free(want);
}

/* -------------------------------------------------------------------------- */

/*
 * Micro-guest in real mode (phase 5a). Guest physical memory: 4 pages at 0.
 *
 *   0x0080  IVT entry of vector 0x20 -> 0000:2000
 *   0x1000  mov ax,0x1234 ; out 0x80,ax ; inc ax ; out 0x80,ax
 *   0x1008  sti ; hlt ; jmp hlt
 *   0x2000  mov al,0x55 ; out 0x81,al ; iret      (vector 0x20 handler)
 */
static const uint8_t guest_main[] = {
	0xB8, 0x34, 0x12,	/* 1000: mov ax,0x1234 */
	0xE7, 0x80,		/* 1003: out 0x80,ax */
	0x40,			/* 1005: inc ax */
	0xE7, 0x80,		/* 1006: out 0x80,ax */
	0xFB,			/* 1008: sti */
	0xF4,			/* 1009: hlt */
	0xEB, 0xFD,		/* 100A: jmp 1009 */
};
static const uint8_t guest_int20[] = {
	0xB0, 0x55,		/* 2000: mov al,0x55 */
	0xE6, 0x81,		/* 2002: out 0x81,al */
	0xCF,			/* 2004: iret */
};

static int
vcpu_run(int fd, nvmm_machid_t machid, nvmm_cpuid_t cpuid,
    struct nvmm_vcpu_exit *exit)
{
	struct nvmm_ioc_vcpu_run args = { .machid = machid, .cpuid = cpuid };
	int error;

	error = ioctl(fd, NVMM_IOC_VCPU_RUN, &args) == -1 ? errno : 0;
	*exit = args.exit;
	return error;
}

static const char *
exit_name(uint64_t reason)
{
	switch (reason) {
	case NVMM_VCPU_EXIT_NONE:	return "NONE";
	case NVMM_VCPU_EXIT_STOPPED:	return "STOPPED";
	case NVMM_VCPU_EXIT_INVALID:	return "INVALID";
	case NVMM_VCPU_EXIT_MEMORY:	return "MEMORY";
	case NVMM_VCPU_EXIT_IO:		return "IO";
	case NVMM_VCPU_EXIT_SHUTDOWN:	return "SHUTDOWN";
	case NVMM_VCPU_EXIT_HALTED:	return "HALTED";
	default:			return "other";
	}
}

/* Runs until an exit other than NONE (interrupted by the host). */
static int
run_until(int fd, nvmm_machid_t machid, struct nvmm_vcpu_exit *exit)
{
	int error, i;

	for (i = 0; i < 1000; i++) {
		error = vcpu_run(fd, machid, 0, exit);
		if (error != 0 || exit->reason != NVMM_VCPU_EXIT_NONE)
			return error;
	}
	return ETIMEDOUT;
}

static bool
expect_exit(const char *what, int error, const struct nvmm_vcpu_exit *exit,
    uint64_t reason)
{
	check(error == 0, "%s: vcpu_run: %s", what, strerror(error));
	if (error != 0)
		return false;
	check(exit->reason == reason, "%s: exit %s (0x%jx, hwcode %ju), "
	    "expected %s", what, exit_name(exit->reason),
	    (uintmax_t)exit->reason, exit->reason == NVMM_VCPU_EXIT_INVALID ?
	    (uintmax_t)exit->u.inv.hwcode : 0, exit_name(reason));
	return exit->reason == reason;
}

/* After an I/O exit: continue after the instruction. */
static void
skip_io(struct nvmm_comm_page *comm, const struct nvmm_vcpu_exit *exit)
{
	comm->state.gprs[NVMM_X64_GPR_RIP] = exit->u.io.npc;
	comm->state_commit |= NVMM_X64_STATE_GPRS;
}

static bool
pin_to_cpu(int cpu)
{
	cpuset_t set;

	CPU_ZERO(&set);
	if (cpu < 0) {
		if (cpuset_getaffinity(CPU_LEVEL_ROOT, CPU_WHICH_PID, -1,
		    sizeof(set), &set) != 0)
			return false;
	} else {
		CPU_SET(cpu, &set);
	}
	return cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1,
	    sizeof(set), &set) == 0;
}

static void
test_run(int fd)
{
	size_t ps = (size_t)getpagesize(), size = 4 * ps;
	struct nvmm_vcpu_exit exit;
	struct nvmm_comm_page *comm;
	struct nvmm_x64_state *st;
	struct timespec t0, t1;
	nvmm_machid_t machid;
	uint8_t *mem;
	long ncpu;
	int error, i, cpu, nhalt;
	double us;

	printf("run: real-mode micro-guest (out, hlt, injected interrupt)\n");
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

	mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE,
	    -1, 0);
	if (mem == MAP_FAILED)
		err(1, "mmap");
	if (hva_map(fd, machid, mem, size) != 0 ||
	    gpa_map(fd, machid, mem, 0, size,
	    PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
		check(false, "guest memory");
		goto out;
	}
	memset(mem, 0, size);
	mem[0x80] = 0x00; mem[0x81] = 0x20;	/* IVT[0x20] = 0000:2000 */
	mem[0x82] = 0x00; mem[0x83] = 0x00;
	memcpy(mem + 0x1000, guest_main, sizeof(guest_main));
	memcpy(mem + 0x2000, guest_int20, sizeof(guest_int20));

	/* CS:IP = 0000:1000, flat real-mode segments, stack at 0x3FF0. */
	comm->state_wanted = NVMM_X64_STATE_ALL;
	comm->state_cached = 0;
	error = vcpu_ioctl(fd, NVMM_IOC_VCPU_GETSTATE, machid, 0);
	check(error == 0, "getstate: %s", strerror(error));
	st->segs[NVMM_X64_SEG_CS].selector = 0;
	st->segs[NVMM_X64_SEG_CS].base = 0;
	st->gprs[NVMM_X64_GPR_RIP] = 0x1000;
	st->gprs[NVMM_X64_GPR_RSP] = 0x3FF0;
	comm->state_commit = NVMM_X64_STATE_SEGS | NVMM_X64_STATE_GPRS;

	/* out 0x80,ax twice. */
	error = run_until(fd, machid, &exit);
	if (expect_exit("first out", error, &exit, NVMM_VCPU_EXIT_IO)) {
		check(!exit.u.io.in && exit.u.io.port == 0x80 &&
		    exit.u.io.operand_size == 2, "first out: in %d port 0x%x "
		    "size %u", exit.u.io.in, exit.u.io.port,
		    exit.u.io.operand_size);
		check((st->gprs[NVMM_X64_GPR_RAX] & 0xFFFF) == 0x1234,
		    "first out: ax 0x%jx", (uintmax_t)st->gprs[NVMM_X64_GPR_RAX]);
		check(exit.u.io.npc == 0x1005, "first out: npc 0x%jx",
		    (uintmax_t)exit.u.io.npc);
		skip_io(comm, &exit);
	}
	error = run_until(fd, machid, &exit);
	if (expect_exit("second out", error, &exit, NVMM_VCPU_EXIT_IO)) {
		check((st->gprs[NVMM_X64_GPR_RAX] & 0xFFFF) == 0x1235,
		    "second out: ax 0x%jx (inc ax not run?)",
		    (uintmax_t)st->gprs[NVMM_X64_GPR_RAX]);
		skip_io(comm, &exit);
	}

	/* sti; hlt. */
	error = run_until(fd, machid, &exit);
	expect_exit("hlt", error, &exit, NVMM_VCPU_EXIT_HALTED);

	/* Interrupt 0x20 injected: the handler does out 0x81,al. */
	memset(&comm->event, 0, sizeof(comm->event));
	comm->event.type = NVMM_VCPU_EVENT_INTR;
	comm->event.vector = 0x20;
	comm->event_commit = true;
	error = run_until(fd, machid, &exit);
	if (expect_exit("injected int 0x20", error, &exit, NVMM_VCPU_EXIT_IO)) {
		check(exit.u.io.port == 0x81 &&
		    (st->gprs[NVMM_X64_GPR_RAX] & 0xFF) == 0x55,
		    "int 0x20 handler: port 0x%x al 0x%jx", exit.u.io.port,
		    (uintmax_t)(st->gprs[NVMM_X64_GPR_RAX] & 0xFF));
		skip_io(comm, &exit);
	}
	/* iret back after hlt, jmp, hlt again. */
	error = run_until(fd, machid, &exit);
	expect_exit("hlt after iret", error, &exit, NVMM_VCPU_EXIT_HALTED);

	/* Many entries and exits: hlt, jmp, hlt... */
	nhalt = 0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (i = 0; i < 100000; i++) {
		error = run_until(fd, machid, &exit);
		if (error != 0 || exit.reason != NVMM_VCPU_EXIT_HALTED)
			break;
		nhalt++;
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	us = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) /
	    1e3 / (nhalt ? nhalt : 1);
	check(nhalt == 100000, "hlt loop stopped at %d: %s, exit %s", nhalt,
	    strerror(error), exit_name(exit.reason));
	printf("  %d hlt exits to userland, %.2f us each\n", nhalt, us);

	/* The vCPU moved across every host CPU: remote VMCLEAR, host fields. */
	ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	for (cpu = 0; cpu < ncpu; cpu++) {
		if (!pin_to_cpu(cpu)) {
			check(false, "pin to CPU %d", cpu);
			continue;
		}
		for (i = 0; i < 100; i++) {
			error = run_until(fd, machid, &exit);
			if (error != 0 || exit.reason != NVMM_VCPU_EXIT_HALTED)
				break;
		}
		check(i == 100, "on CPU %d: run %d: %s, exit %s", cpu, i,
		    strerror(error), exit_name(exit.reason));
	}
	(void)pin_to_cpu(-1);
	printf("  ran on %ld host CPUs in turn\n", ncpu);

	munmap(comm, ps);
out:
	error = machine_destroy(fd, machid);
	check(error == 0, "destroy machine: %s", strerror(error));
	check_counters("run");
}

/* -------------------------------------------------------------------------- */

/*
 * Step 5b. A guest that spins reading gpa 0x3000 and reports each new
 * value with "out 0x83,al", while another thread remaps that page onto
 * host pages with different contents. Each remap goes through
 * pmap_invalidate_ept() with the vCPU possibly in the guest; if a stale
 * translation survived, the guest would keep reading the old page.
 *
 *   1000: mov al,[0x3000] ; mov ah,al
 *   1005: mov al,[0x3000] ; cmp al,ah ; je 1005
 *   100C: out 0x83,al ; mov ah,al ; jmp 1005
 *   1100: mov byte [0x5000],0x42	(MMIO: 0x5000 is not mapped)
 */
static const uint8_t guest_spin[] = {
	0xA0, 0x00, 0x30,	/* 1000: mov al,[0x3000] */
	0x88, 0xC4,		/* 1003: mov ah,al */
	0xA0, 0x00, 0x30,	/* 1005: mov al,[0x3000] */
	0x38, 0xE0,		/* 1008: cmp al,ah */
	0x74, 0xF9,		/* 100A: je 1005 */
	0xE6, 0x83,		/* 100C: out 0x83,al */
	0x88, 0xC4,		/* 100E: mov ah,al */
	0xEB, 0xF3,		/* 1010: jmp 1005 */
};
static const uint8_t guest_mmio[] = {
	0xC6, 0x06, 0x00, 0x50, 0x42,	/* 1100: mov byte [0x5000],0x42 */
	0xF4,				/* 1105: hlt */
};

#define REMAP_NPAGES	16
#define REMAP_ROUNDS	1000
#define REMAP_LAST	0xFF

struct spin_ctx {
	int fd;
	nvmm_machid_t machid;
	struct nvmm_comm_page *comm;
	_Atomic int seen;	/* last value the guest reported, -1: none */
	_Atomic int nexits;
	int error;
	uint64_t bad_reason;
};

static void *
spin_runner(void *arg)
{
	struct spin_ctx *ctx = arg;
	struct nvmm_vcpu_exit exit;
	int error, value;

	for (;;) {
		error = vcpu_run(ctx->fd, ctx->machid, 0, &exit);
		if (error != 0) {
			ctx->error = error;
			break;
		}
		atomic_fetch_add(&ctx->nexits, 1);
		if (exit.reason == NVMM_VCPU_EXIT_NONE)
			continue;
		if (exit.reason != NVMM_VCPU_EXIT_IO || exit.u.io.port != 0x83) {
			ctx->bad_reason = exit.reason;
			break;
		}
		value = (int)(ctx->comm->state.gprs[NVMM_X64_GPR_RAX] & 0xFF);
		skip_io(ctx->comm, &exit);
		atomic_store(&ctx->seen, value);
		if (value == REMAP_LAST)
			break;
	}
	return NULL;
}

static bool
set_real_mode_rip(int fd, nvmm_machid_t machid, struct nvmm_comm_page *comm,
    uint64_t rip)
{
	comm->state_wanted = NVMM_X64_STATE_ALL;
	comm->state_cached = 0;
	if (vcpu_ioctl(fd, NVMM_IOC_VCPU_GETSTATE, machid, 0) != 0)
		return false;
	comm->state.segs[NVMM_X64_SEG_CS].selector = 0;
	comm->state.segs[NVMM_X64_SEG_CS].base = 0;
	comm->state.gprs[NVMM_X64_GPR_RIP] = rip;
	comm->state.gprs[NVMM_X64_GPR_RSP] = 0x2FF0;
	comm->state_commit = NVMM_X64_STATE_SEGS | NVMM_X64_STATE_GPRS;
	return true;
}

static void
test_run_ept(int fd)
{
	size_t ps = (size_t)getpagesize();
	struct nvmm_vcpu_exit exit;
	struct nvmm_comm_page *comm;
	struct spin_ctx ctx;
	struct timespec t0, t1;
	nvmm_machid_t machid;
	uint8_t *mem, *alt;
	pthread_t thr;
	int error, i, page, want, waited;
	double ms;

	printf("run: MMIO exit, EPT remapped under a running vCPU\n");
	error = machine_create(fd, &machid);
	check(error == 0, "machine: %s", strerror(error));
	if (error)
		return;
	error = vcpu_create(fd, machid, 0);
	check(error == 0, "vcpu: %s", strerror(error));
	if (error)
		goto out;
	comm = comm_map(fd, machid, 0);

	/* gpa 0-0x3FFF from mem; alt: the pages 0x3000 gets remapped to. */
	mem = mmap(NULL, 4 * ps, PROT_READ | PROT_WRITE, MAP_ANON |
	    MAP_PRIVATE, -1, 0);
	alt = mmap(NULL, REMAP_NPAGES * ps, PROT_READ | PROT_WRITE,
	    MAP_ANON | MAP_PRIVATE, -1, 0);
	if (mem == MAP_FAILED || alt == MAP_FAILED)
		err(1, "mmap");
	if (hva_map(fd, machid, mem, 4 * ps) != 0 ||
	    hva_map(fd, machid, alt, REMAP_NPAGES * ps) != 0 ||
	    gpa_map(fd, machid, mem, 0, 4 * ps,
	    PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
		check(false, "guest memory");
		goto out;
	}
	memset(mem, 0, 4 * ps);
	memcpy(mem + 0x1000, guest_spin, sizeof(guest_spin));
	memcpy(mem + 0x1100, guest_mmio, sizeof(guest_mmio));
	for (i = 0; i < REMAP_NPAGES; i++)
		memset(alt + i * ps, 0, ps);

	/* MMIO: a write to unmapped 0x5000 comes back as a memory exit. */
	if (!set_real_mode_rip(fd, machid, comm, 0x1100)) {
		check(false, "state");
		goto out;
	}
	error = run_until(fd, machid, &exit);
	if (expect_exit("mmio write", error, &exit, NVMM_VCPU_EXIT_MEMORY)) {
		check(exit.u.mem.gpa == 0x5000 &&
		    exit.u.mem.prot == PROT_WRITE, "mmio write: gpa 0x%jx "
		    "prot %d", (uintmax_t)exit.u.mem.gpa, exit.u.mem.prot);
		check(comm->state.gprs[NVMM_X64_GPR_RIP] == 0x1100,
		    "mmio write: rip 0x%jx, the faulting instruction",
		    (uintmax_t)comm->state.gprs[NVMM_X64_GPR_RIP]);
	}

	/* The spinning guest, from 0x1000, reading 0 at 0x3000. */
	if (!set_real_mode_rip(fd, machid, comm, 0x1000)) {
		check(false, "state");
		goto out;
	}
	memset(&ctx, 0, sizeof(ctx));
	ctx.fd = fd;
	ctx.machid = machid;
	ctx.comm = comm;
	atomic_store(&ctx.seen, -1);
	if (pthread_create(&thr, NULL, spin_runner, &ctx) != 0)
		err(1, "pthread_create");
	usleep(100000);	/* let the guest read the first value */

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (i = 1; i <= REMAP_ROUNDS; i++) {
		page = i % REMAP_NPAGES;
		want = (i == REMAP_ROUNDS) ? REMAP_LAST : (i % 200) + 1;
		memset(alt + page * ps, want, ps);
		error = gpa_unmap(fd, machid, 0x3000, ps);
		if (error == 0)
			error = gpa_map(fd, machid, alt + page * ps, 0x3000,
			    ps, PROT_READ | PROT_WRITE);
		if (error != 0) {
			check(false, "remap %d: %s", i, strerror(error));
			break;
		}
		/* The guest must see the new value: 1s at most. */
		for (waited = 0; waited < 1000; waited++) {
			if (atomic_load(&ctx.seen) == want ||
			    ctx.error != 0 || ctx.bad_reason != 0)
				break;
			usleep(1000);
		}
		if (atomic_load(&ctx.seen) != want) {
			check(false, "remap %d: guest still sees %d, want %d "
			    "(stale EPT translation?) runner error %d exit "
			    "0x%jx", i, atomic_load(&ctx.seen), want,
			    ctx.error, (uintmax_t)ctx.bad_reason);
			break;
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	if (i <= REMAP_ROUNDS) {
		/* Unblock the runner: it stops on the last value. */
		memset(alt, REMAP_LAST, ps);
		(void)gpa_unmap(fd, machid, 0x3000, ps);
		(void)gpa_map(fd, machid, alt, 0x3000, ps, PROT_READ);
	}
	pthread_join(thr, NULL);
	ms = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 1e6;
	if (i > REMAP_ROUNDS)
		printf("  %d remaps seen by the running guest, %.3f ms each, "
		    "%d exits\n", REMAP_ROUNDS, ms / REMAP_ROUNDS,
		    atomic_load(&ctx.nexits));

	munmap(comm, ps);
out:
	error = machine_destroy(fd, machid);
	check(error == 0, "destroy machine: %s", strerror(error));
	check_counters("run ept");
}

/* -------------------------------------------------------------------------- */

/*
 * Step 5c. CPUID, MSRs and CR0, each followed by "out 0x84,al" so that
 * userland sees the registers (an I/O exit provides the GPRs).
 */
static const uint8_t guest_cpuid_msr[] = {
	0x66, 0x31, 0xC0,			/* 1200: xor eax,eax */
	0x0F, 0xA2,				/* 1203: cpuid */
	0xE6, 0x84,				/* 1205: out  [vendor] */
	0x66, 0xB8, 0x00, 0x00, 0x00, 0x40,	/* 1207: mov eax,0x40000000 */
	0x0F, 0xA2,				/* 120D: cpuid */
	0xE6, 0x84,				/* 120F: out  [hypervisor] */
	0x66, 0xB8, 0x01, 0x00, 0x00, 0x00,	/* 1211: mov eax,1 */
	0x0F, 0xA2,				/* 1217: cpuid */
	0xE6, 0x84,				/* 1219: out  [leaf 1] */
	0x66, 0xB8, 0x06, 0x00, 0x00, 0x00,	/* 121B: mov eax,6 */
	0x0F, 0xA2,				/* 1221: cpuid (exit: configured) */
	0xE6, 0x84,				/* 1223: out  [userland value] */
	0x66, 0xB9, 0x1B, 0x00, 0x00, 0x00,	/* 1225: mov ecx,0x1B */
	0x0F, 0x32,				/* 122B: rdmsr (exit: APIC base) */
	0xE6, 0x84,				/* 122D: out  [userland value] */
	0x66, 0xB9, 0x77, 0x02, 0x00, 0x00,	/* 122F: mov ecx,0x277 */
	0x0F, 0x32,				/* 1235: rdmsr (in kernel: PAT) */
	0xE6, 0x84,				/* 1237: out  [PAT] */
	0x0F, 0x20, 0xC0,			/* 1239: mov eax,cr0 */
	0x66, 0x25, 0xFF, 0xFF, 0xFF, 0x9F,	/* 123C: and eax,~(CD|NW) */
	0x0F, 0x22, 0xC0,			/* 1242: mov cr0,eax (CR exit) */
	0x0F, 0x20, 0xC3,			/* 1245: mov ebx,cr0 */
	0xE6, 0x84,				/* 1248: out  [cr0] */
	0xF4,					/* 124A: hlt */
};

static bool
next_out84(int fd, nvmm_machid_t machid, struct nvmm_comm_page *comm,
    const char *what)
{
	struct nvmm_vcpu_exit exit;
	int error;

	error = run_until(fd, machid, &exit);
	if (!expect_exit(what, error, &exit, NVMM_VCPU_EXIT_IO))
		return false;
	if (exit.u.io.port != 0x84) {
		check(false, "%s: out to port 0x%x", what, exit.u.io.port);
		return false;
	}
	skip_io(comm, &exit);
	return true;
}

static void
test_run_cpuid_msr(int fd)
{
	size_t ps = (size_t)getpagesize();
	struct nvmm_vcpu_conf_cpuid conf;
	struct nvmm_vcpu_exit exit;
	struct nvmm_comm_page *comm;
	uint64_t *gpr;
	nvmm_machid_t machid;
	uint8_t *mem;
	char id[13];
	int error;

	printf("run: CPUID, MSRs, CR0\n");
	error = machine_create(fd, &machid);
	check(error == 0, "machine: %s", strerror(error));
	if (error)
		return;
	error = vcpu_create(fd, machid, 0);
	check(error == 0, "vcpu: %s", strerror(error));
	if (error)
		goto out;
	comm = comm_map(fd, machid, 0);
	gpr = comm->state.gprs;

	mem = mmap(NULL, 4 * ps, PROT_READ | PROT_WRITE, MAP_ANON |
	    MAP_PRIVATE, -1, 0);
	if (mem == MAP_FAILED)
		err(1, "mmap");
	if (hva_map(fd, machid, mem, 4 * ps) != 0 ||
	    gpa_map(fd, machid, mem, 0, 4 * ps,
	    PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
		check(false, "guest memory");
		goto out;
	}
	memset(mem, 0, 4 * ps);
	memcpy(mem + 0x1200, guest_cpuid_msr, sizeof(guest_cpuid_msr));

	memset(&conf, 0, sizeof(conf));
	conf.exit = 1;
	conf.leaf = 0x00000006;
	error = vcpu_configure(fd, machid, 0, NVMM_VCPU_CONF_CPUID, &conf);
	check(error == 0, "conf cpuid 6 exit: %s", strerror(error));

	if (!set_real_mode_rip(fd, machid, comm, 0x1200)) {
		check(false, "state");
		goto out;
	}

	/* Leaf 0: the host vendor, max basic leaf clipped. */
	if (!next_out84(fd, machid, comm, "cpuid 0"))
		goto out;
	memcpy(id, &gpr[NVMM_X64_GPR_RBX], 4);
	memcpy(id + 4, &gpr[NVMM_X64_GPR_RDX], 4);
	memcpy(id + 8, &gpr[NVMM_X64_GPR_RCX], 4);
	id[12] = '\0';
	check(strcmp(id, "GenuineIntel") == 0, "cpuid 0 vendor \"%s\"", id);
	check((uint32_t)gpr[NVMM_X64_GPR_RAX] <= 0x16,
	    "cpuid 0 max leaf 0x%jx", (uintmax_t)gpr[NVMM_X64_GPR_RAX]);
	printf("  vendor %s, max basic leaf 0x%jx\n", id,
	    (uintmax_t)(uint32_t)gpr[NVMM_X64_GPR_RAX]);

	/* Leaf 0x40000000: the NVMM signature. */
	if (!next_out84(fd, machid, comm, "cpuid 0x40000000"))
		goto out;
	memcpy(id, &gpr[NVMM_X64_GPR_RBX], 4);
	memcpy(id + 4, &gpr[NVMM_X64_GPR_RCX], 4);
	memcpy(id + 8, &gpr[NVMM_X64_GPR_RDX], 4);
	check(memcmp(id, "___ NVMM ___", 12) == 0,
	    "cpuid 0x40000000 signature \"%.12s\"", id);

	/* Leaf 1: VMX hidden, hypervisor bit set, APIC ID = vCPU 0. */
	if (!next_out84(fd, machid, comm, "cpuid 1"))
		goto out;
	check((gpr[NVMM_X64_GPR_RCX] & 0x20) == 0, "cpuid 1: VMX visible");
	check((gpr[NVMM_X64_GPR_RCX] & 0x80000000U) != 0,
	    "cpuid 1: hypervisor bit clear");
	check(((gpr[NVMM_X64_GPR_RBX] >> 24) & 0xFF) == 0,
	    "cpuid 1: APIC ID %ju", (uintmax_t)((gpr[NVMM_X64_GPR_RBX] >> 24) &
	    0xFF));

	/* Leaf 6, configured to exit: userland answers. */
	error = run_until(fd, machid, &exit);
	if (!expect_exit("cpuid 6", error, &exit, NVMM_VCPU_EXIT_CPUID))
		goto out;
	comm->state_wanted = NVMM_X64_STATE_GPRS;
	comm->state_cached = 0;
	(void)vcpu_ioctl(fd, NVMM_IOC_VCPU_GETSTATE, machid, 0);
	gpr[NVMM_X64_GPR_RAX] = 0xABCD;
	gpr[NVMM_X64_GPR_RIP] = exit.u.insn.npc;
	comm->state_commit |= NVMM_X64_STATE_GPRS;
	if (!next_out84(fd, machid, comm, "after cpuid 6"))
		goto out;
	check((gpr[NVMM_X64_GPR_RAX] & 0xFFFF) == 0xABCD,
	    "cpuid 6: eax 0x%jx", (uintmax_t)gpr[NVMM_X64_GPR_RAX]);

	/* RDMSR of the APIC base: not in the kernel, userland answers. */
	error = run_until(fd, machid, &exit);
	if (!expect_exit("rdmsr 0x1B", error, &exit, NVMM_VCPU_EXIT_RDMSR))
		goto out;
	check(exit.u.rdmsr.msr == 0x1B, "rdmsr: msr 0x%x", exit.u.rdmsr.msr);
	gpr[NVMM_X64_GPR_RAX] = 0xFEE00900;
	gpr[NVMM_X64_GPR_RDX] = 0;
	gpr[NVMM_X64_GPR_RIP] = exit.u.rdmsr.npc;
	comm->state_commit |= NVMM_X64_STATE_GPRS;
	if (!next_out84(fd, machid, comm, "after rdmsr 0x1B"))
		goto out;
	check((uint32_t)gpr[NVMM_X64_GPR_RAX] == 0xFEE00900,
	    "rdmsr 0x1B: eax 0x%jx", (uintmax_t)gpr[NVMM_X64_GPR_RAX]);

	/* RDMSR of the PAT: in the kernel, the guest's reset value. */
	if (!next_out84(fd, machid, comm, "rdmsr PAT"))
		goto out;
	check(((uint32_t)gpr[NVMM_X64_GPR_RDX] == 0x00070406) &&
	    ((uint32_t)gpr[NVMM_X64_GPR_RAX] == 0x00070406),
	    "rdmsr PAT: edx:eax 0x%08jx:%08jx",
	    (uintmax_t)(uint32_t)gpr[NVMM_X64_GPR_RDX],
	    (uintmax_t)(uint32_t)gpr[NVMM_X64_GPR_RAX]);

	/* CR0 write (CD/NW cleared): handled in the kernel. */
	if (!next_out84(fd, machid, comm, "cr0"))
		goto out;
	check((gpr[NVMM_X64_GPR_RBX] & 0x60000000) == 0 &&
	    (gpr[NVMM_X64_GPR_RBX] & 0x10) != 0,
	    "cr0 after write 0x%jx", (uintmax_t)gpr[NVMM_X64_GPR_RBX]);

	error = run_until(fd, machid, &exit);
	expect_exit("final hlt", error, &exit, NVMM_VCPU_EXIT_HALTED);

	munmap(comm, ps);
out:
	error = machine_destroy(fd, machid);
	check(error == 0, "destroy machine: %s", strerror(error));
	check_counters("run cpuid/msr");
}

/* -------------------------------------------------------------------------- */

/*
 * Step 5d. The guest puts a pattern in XMM0 and makes 1000 exits to
 * userland; around each run the host loads other values in XMM0 and YMM1
 * (AVX) and checks them after the ioctl, done with a raw syscall so that
 * no libc code touches vector registers in between. At the end the guest
 * stores XMM0: it must still hold its pattern. Then RDTSC with a guest TSC
 * set from userland.
 */
static const uint8_t guest_fpu[] = {
	0x0F, 0x20, 0xC0,			/* 1300: mov eax,cr0 */
	0x66, 0x83, 0xE0, 0xFB,			/* 1303: and eax,~CR0_EM */
	0x66, 0x83, 0xC8, 0x02,			/* 1307: or eax,CR0_MP */
	0x0F, 0x22, 0xC0,			/* 130B: mov cr0,eax */
	0x0F, 0x20, 0xE0,			/* 130E: mov eax,cr4 */
	0x66, 0x0D, 0x00, 0x06, 0x00, 0x00,	/* 1311: or eax,OSFXSR|OSXMMEXCPT */
	0x0F, 0x22, 0xE0,			/* 1317: mov cr4,eax */
	0x0F, 0x10, 0x06, 0x00, 0x30,		/* 131A: movups xmm0,[0x3000] */
	0xB9, 0xE8, 0x03,			/* 131F: mov cx,1000 */
	0xE6, 0x85,				/* 1322: out 0x85,al */
	0x49,					/* 1324: dec cx */
	0x75, 0xFB,				/* 1325: jnz 1322 */
	0x0F, 0x11, 0x06, 0x10, 0x30,		/* 1327: movups [0x3010],xmm0 */
	0x0F, 0x31,				/* 132C: rdtsc */
	0xE6, 0x86,				/* 132E: out 0x86,al */
	0xF4,					/* 1330: hlt */
};

/* NVMM_IOC_VCPU_RUN with host XMM0/YMM1 loaded before and saved after. */
static int
run_with_host_vectors(int fd, struct nvmm_ioc_vcpu_run *args,
    const uint8_t in[32], uint8_t outx[16], uint8_t outy[32])
{
	long ret, rdx = (long)args;
	uint8_t cf;

	/*
	 * FreeBSD's syscall returns a second value in RDX and may change
	 * RCX, R8-R11: the pointers must live in other registers.
	 */
	__asm volatile(
	    "movdqu (%[in]), %%xmm0\n\t"
	    "vmovdqu (%[in]), %%ymm1\n\t"
	    "syscall\n\t"
	    "setc %[cf]\n\t"
	    "movdqu %%xmm0, (%[ox])\n\t"
	    "vmovdqu %%ymm1, (%[oy])\n\t"
	    "vzeroupper\n\t"
	    : "=a" (ret), [cf] "=&b" (cf), "+d" (rdx)
	    : "a" ((long)SYS_ioctl), "D" ((long)fd),
	      "S" ((long)NVMM_IOC_VCPU_RUN),
	      [in] "r" (in), [ox] "r" (outx), [oy] "r" (outy)
	    : "rcx", "r8", "r9", "r10", "r11", "xmm0", "xmm1", "memory",
	      "cc");
	return cf ? (int)ret : 0;
}

static void
test_run_fpu(int fd)
{
	size_t ps = (size_t)getpagesize();
	struct nvmm_ioc_vcpu_run args;
	struct nvmm_comm_page *comm;
	uint8_t hostv[32], outx[16], outy[32];
	uint8_t *mem, gpat[16];
	nvmm_machid_t machid;
	uint64_t tsc;
	int error, i, nouts, nbad;

	printf("run: guest SSE state vs host SSE/AVX state, guest TSC\n");
	if (!__builtin_cpu_supports("avx")) {
		printf("  no AVX on this host: skipped\n");
		return;
	}
	error = machine_create(fd, &machid);
	check(error == 0, "machine: %s", strerror(error));
	if (error)
		return;
	error = vcpu_create(fd, machid, 0);
	check(error == 0, "vcpu: %s", strerror(error));
	if (error)
		goto out;
	comm = comm_map(fd, machid, 0);

	mem = mmap(NULL, 4 * ps, PROT_READ | PROT_WRITE, MAP_ANON |
	    MAP_PRIVATE, -1, 0);
	if (mem == MAP_FAILED)
		err(1, "mmap");
	if (hva_map(fd, machid, mem, 4 * ps) != 0 ||
	    gpa_map(fd, machid, mem, 0, 4 * ps,
	    PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
		check(false, "guest memory");
		goto out;
	}
	memset(mem, 0, 4 * ps);
	memcpy(mem + 0x1300, guest_fpu, sizeof(guest_fpu));
	fill(gpat, sizeof(gpat), 0xC3);
	memcpy(mem + 0x3000, gpat, sizeof(gpat));

	if (!set_real_mode_rip(fd, machid, comm, 0x1300)) {
		check(false, "state");
		goto out;
	}
	/* Guest TSC starts near 1000. */
	comm->state_wanted = NVMM_X64_STATE_MSRS;
	comm->state_cached = 0;
	(void)vcpu_ioctl(fd, NVMM_IOC_VCPU_GETSTATE, machid, 0);
	comm->state.msrs[NVMM_X64_MSR_TSC] = 1000;
	comm->state_commit |= NVMM_X64_STATE_MSRS;

	nouts = nbad = 0;
	for (i = 0; i < 100000 && nouts < 1000; i++) {
		fill(hostv, sizeof(hostv), (uint8_t)(0x10 + i));
		memset(&args, 0, sizeof(args));
		args.machid = machid;
		args.cpuid = 0;
		error = run_with_host_vectors(fd, &args, hostv, outx, outy);
		if (error != 0) {
			check(false, "run: %s", strerror(error));
			break;
		}
		if (memcmp(outx, hostv, 16) != 0 ||
		    memcmp(outy, hostv, 32) != 0)
			nbad++;
		if (args.exit.reason == NVMM_VCPU_EXIT_NONE)
			continue;
		if (args.exit.reason != NVMM_VCPU_EXIT_IO ||
		    args.exit.u.io.port != 0x85) {
			check(false, "fpu loop: exit %s port 0x%x",
			    exit_name(args.exit.reason), args.exit.u.io.port);
			break;
		}
		skip_io(comm, &args.exit);
		nouts++;
	}
	check(nbad == 0, "host XMM0/YMM1 changed across %d of %d runs", nbad,
	    i);
	check(nouts == 1000, "fpu loop: %d exits", nouts);

	/* movups [0x3010],xmm0 ; rdtsc ; out 0x86 */
	{
		struct nvmm_vcpu_exit exit;

		error = run_until(fd, machid, &exit);
		if (expect_exit("rdtsc", error, &exit, NVMM_VCPU_EXIT_IO)) {
			check(memcmp(mem + 0x3010, gpat, 16) == 0,
			    "guest XMM0 lost across host FPU use");
			tsc = (comm->state.gprs[NVMM_X64_GPR_RDX] << 32) |
			    (comm->state.gprs[NVMM_X64_GPR_RAX] & 0xFFFFFFFF);
			/* ~1000 runs of a few us: far below 10^10 cycles. */
			check(tsc > 1000 && tsc < 10000000000ULL,
			    "guest TSC %ju, set to 1000", (uintmax_t)tsc);
			printf("  %d runs, host vectors intact; guest TSC %ju\n",
			    i, (uintmax_t)tsc);
			skip_io(comm, &exit);
		}
		error = run_until(fd, machid, &exit);
		expect_exit("final hlt", error, &exit, NVMM_VCPU_EXIT_HALTED);
	}

	munmap(comm, ps);
out:
	error = machine_destroy(fd, machid);
	check(error == 0, "destroy machine: %s", strerror(error));
	check_counters("run fpu");
}

/* -------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------- */

/*
 * Guest AVX state (YMM, 256 bits) across exits. The guest runs in 32-bit
 * protected mode with CR4.OSXSAVE and XCR0 = x87|SSE|AVX, loads YMM0 from
 * 0x3000, makes 1000 exits, stores YMM0 at 0x3020. With "host_avx" the
 * host loads other values in YMM0/YMM1 around every run.
 */
static const uint8_t guest_avx[] = {
	0xC5, 0xFE, 0x6F, 0x05, 0x00, 0x30, 0x00, 0x00,	/* vmovdqu ymm0,[0x3000] */
	0xB9, 0xE8, 0x03, 0x00, 0x00,			/* mov ecx,1000 */
	0xE6, 0x87,					/* out 0x87,al */
	0x49,						/* dec ecx */
	0x75, 0xFB,					/* jnz out */
	0xC5, 0xFE, 0x7F, 0x05, 0x20, 0x30, 0x00, 0x00,	/* vmovdqu [0x3020],ymm0 */
	0xF4,						/* hlt */
};

static int
run_with_host_ymm(int fd, struct nvmm_ioc_vcpu_run *args, const uint8_t in[32])
{
	long ret, rdx = (long)args;
	uint8_t cf;

	__asm volatile(
	    "vmovdqu (%[in]), %%ymm0\n\t"
	    "vmovdqu (%[in]), %%ymm1\n\t"
	    "syscall\n\t"
	    "setc %[cf]\n\t"
	    "vzeroupper\n\t"
	    : "=a" (ret), [cf] "=&b" (cf), "+d" (rdx)
	    : "a" ((long)SYS_ioctl), "D" ((long)fd),
	      "S" ((long)NVMM_IOC_VCPU_RUN), [in] "r" (in)
	    : "rcx", "r8", "r9", "r10", "r11", "xmm0", "xmm1", "memory",
	      "cc");
	return cf ? (int)ret : 0;
}

static void
test_run_avx_one(int fd, bool host_avx)
{
	size_t ps = (size_t)getpagesize();
	struct nvmm_ioc_vcpu_run args;
	struct nvmm_comm_page *comm;
	struct nvmm_x64_state *st;
	uint8_t *mem, gpat[32], hostv[32];
	nvmm_machid_t machid;
	int error, i, nouts;

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

	mem = mmap(NULL, 4 * ps, PROT_READ | PROT_WRITE, MAP_ANON |
	    MAP_PRIVATE, -1, 0);
	if (mem == MAP_FAILED)
		err(1, "mmap");
	if (hva_map(fd, machid, mem, 4 * ps) != 0 ||
	    gpa_map(fd, machid, mem, 0, 4 * ps,
	    PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
		check(false, "guest memory");
		goto out;
	}
	memset(mem, 0, 4 * ps);
	memcpy(mem + 0x1400, guest_avx, sizeof(guest_avx));
	fill(gpat, sizeof(gpat), 0x3D);
	memcpy(mem + 0x3000, gpat, sizeof(gpat));

	/* Flat 32-bit protected mode, AVX enabled. */
	comm->state_wanted = NVMM_X64_STATE_ALL;
	comm->state_cached = 0;
	(void)vcpu_ioctl(fd, NVMM_IOC_VCPU_GETSTATE, machid, 0);
	for (i = 0; i < NVMM_X64_NSEG; i++) {
		struct nvmm_x64_state_seg *sg = &st->segs[i];

		if (i == NVMM_X64_SEG_GDT || i == NVMM_X64_SEG_IDT)
			continue;
		sg->base = 0;
		sg->limit = 0xFFFFFFFF;
		sg->attrib.s = (i != NVMM_X64_SEG_LDT && i != NVMM_X64_SEG_TR);
		sg->attrib.p = 1;
		sg->attrib.def = 1;
		sg->attrib.g = 1;
		sg->attrib.type = (i == NVMM_X64_SEG_CS) ? 11 :
		    (i == NVMM_X64_SEG_TR) ? 11 : (i == NVMM_X64_SEG_LDT) ? 2 : 3;
		sg->selector = (i == NVMM_X64_SEG_CS) ? 0x08 : 0x10;
	}
	st->crs[NVMM_X64_CR_CR0] = 0x33;		/* PE MP ET NE */
	st->crs[NVMM_X64_CR_CR4] = 0x40600;		/* OSFXSR XMMEXCPT OSXSAVE */
	st->crs[NVMM_X64_CR_XCR0] = 0x7;		/* x87 SSE AVX */
	st->gprs[NVMM_X64_GPR_RIP] = 0x1400;
	st->gprs[NVMM_X64_GPR_RFLAGS] = 0x2;
	comm->state_commit = NVMM_X64_STATE_SEGS | NVMM_X64_STATE_GPRS |
	    NVMM_X64_STATE_CRS;

	nouts = 0;
	for (i = 0; i < 100000 && nouts < 1000; i++) {
		fill(hostv, sizeof(hostv), (uint8_t)(0x90 + i));
		memset(&args, 0, sizeof(args));
		args.machid = machid;
		error = host_avx ? run_with_host_ymm(fd, &args, hostv) :
		    vcpu_run(fd, machid, 0, &args.exit);
		if (error != 0) {
			check(false, "run: %s", strerror(error));
			break;
		}
		if (args.exit.reason == NVMM_VCPU_EXIT_NONE)
			continue;
		if (args.exit.reason != NVMM_VCPU_EXIT_IO ||
		    args.exit.u.io.port != 0x87) {
			check(false, "avx loop: exit %s (0x%jx hw %ju) port 0x%x",
			    exit_name(args.exit.reason),
			    (uintmax_t)args.exit.reason,
			    (uintmax_t)args.exit.u.inv.hwcode,
			    args.exit.u.io.port);
			break;
		}
		skip_io(comm, &args.exit);
		nouts++;
	}
	check(nouts == 1000, "avx loop: %d exits", nouts);
	{
		struct nvmm_vcpu_exit exit;

		error = run_until(fd, machid, &exit);
		if (expect_exit("avx hlt", error, &exit,
		    NVMM_VCPU_EXIT_HALTED)) {
			check(memcmp(mem + 0x3020, gpat, 16) == 0,
			    "guest YMM0 low 128 bits lost (host_avx %d)",
			    host_avx);
			check(memcmp(mem + 0x3030, gpat + 16, 16) == 0,
			    "guest YMM0 HIGH 128 bits lost (host_avx %d): "
			    "%02x%02x.. want %02x%02x..", host_avx,
			    mem[0x3030], mem[0x3031], gpat[16], gpat[17]);
		}
	}
	printf("  host AVX %s: guest YMM0 after 1000 exits %s\n",
	    host_avx ? "on " : "off", memcmp(mem + 0x3020, gpat, 32) == 0 ?
	    "intact" : "CORRUPTED");

	munmap(comm, ps);
out:
	error = machine_destroy(fd, machid);
	check(error == 0, "destroy machine: %s", strerror(error));
	check_counters("run avx");
}

static void
test_run_avx(int fd)
{
	printf("run: guest AVX (YMM) state across exits\n");
	if (!__builtin_cpu_supports("avx")) {
		printf("  no AVX on this host: skipped\n");
		return;
	}
	test_run_avx_one(fd, false);
	test_run_avx_one(fd, true);
}

/*
 * Step 5e. A guest that never exits by itself ("jmp $") must still give
 * the CPU back: on a signal, on comm->stop, when the scheduler wants the
 * CPU, and when the process is killed.
 */
static const uint8_t guest_spin_forever[] = {
	0xEB, 0xFE,	/* 1400: jmp 1400 */
};

#define NSPIN	8

struct forever_ctx {
	int fd;
	nvmm_machid_t machid;
	nvmm_cpuid_t cpuid;
	_Atomic bool done;
	_Atomic int nreturns;	/* ioctls that came back */
	_Atomic uint64_t last_reason;
	int error;
};

static void
sigusr1(int sig __unused)
{
}

static void *
forever_runner(void *arg)
{
	struct forever_ctx *ctx = arg;
	struct nvmm_vcpu_exit exit;
	int error;

	while (!atomic_load(&ctx->done)) {
		error = vcpu_run(ctx->fd, ctx->machid, ctx->cpuid, &exit);
		if (error != 0 && error != EINTR) {
			ctx->error = error;
			break;
		}
		atomic_store(&ctx->last_reason, exit.reason);
		atomic_fetch_add(&ctx->nreturns, 1);
		if (exit.reason == NVMM_VCPU_EXIT_STOPPED)
			break;
		if (exit.reason != NVMM_VCPU_EXIT_NONE) {
			ctx->error = -1;
			break;
		}
	}
	return NULL;
}

static double
elapsed_ms(const struct timespec *t0)
{
	struct timespec t1;

	clock_gettime(CLOCK_MONOTONIC, &t1);
	return ((t1.tv_sec - t0->tv_sec) * 1e9 +
	    (t1.tv_nsec - t0->tv_nsec)) / 1e6;
}

/* Wait until ctx->nreturns > n, at most 1 s. */
static double
wait_return(struct forever_ctx *ctx, int n)
{
	struct timespec t0;
	double ms;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (atomic_load(&ctx->nreturns) <= n) {
		ms = elapsed_ms(&t0);
		if (ms > 1000)
			return -1;
		usleep(100);
	}
	return elapsed_ms(&t0);
}

static bool
spin_machine(int fd, nvmm_machid_t machid, u_int nvcpu, uint8_t **memp)
{
	size_t ps = (size_t)getpagesize();
	struct nvmm_comm_page *comm;
	uint8_t *mem;
	u_int c;

	mem = mmap(NULL, 4 * ps, PROT_READ | PROT_WRITE, MAP_ANON |
	    MAP_PRIVATE, -1, 0);
	if (mem == MAP_FAILED)
		return false;
	if (hva_map(fd, machid, mem, 4 * ps) != 0 ||
	    gpa_map(fd, machid, mem, 0, 4 * ps,
	    PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
		return false;
	memset(mem, 0, 4 * ps);
	memcpy(mem + 0x1400, guest_spin_forever, sizeof(guest_spin_forever));
	for (c = 0; c < nvcpu; c++) {
		if (vcpu_create(fd, machid, c) != 0)
			return false;
		comm = comm_map(fd, machid, c);
		comm->state_wanted = NVMM_X64_STATE_ALL;
		comm->state_cached = 0;
		{
			struct nvmm_ioc_vcpu_getstate a = {
				.machid = machid, .cpuid = c
			};
			if (ioctl(fd, NVMM_IOC_VCPU_GETSTATE, &a) == -1)
				return false;
		}
		comm->state.segs[NVMM_X64_SEG_CS].selector = 0;
		comm->state.segs[NVMM_X64_SEG_CS].base = 0;
		comm->state.gprs[NVMM_X64_GPR_RIP] = 0x1400;
		comm->state_commit = NVMM_X64_STATE_SEGS | NVMM_X64_STATE_GPRS;
		munmap(comm, ps);
	}
	*memp = mem;
	return true;
}

static void
test_run_signals(int fd)
{
	struct forever_ctx ctx[NSPIN];
	struct nvmm_comm_page *comm;
	struct sigaction sa;
	struct timespec t0;
	pthread_t thr[NSPIN];
	nvmm_machid_t machid;
	uint8_t *mem;
	double ms, worst;
	int error, i, n, minret;
	pid_t pid;
	int status;

	printf("run: guest that never exits: signal, stop, preemption, kill\n");
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigusr1;	/* no SA_RESTART */
	sigaction(SIGUSR1, &sa, NULL);

	error = machine_create(fd, &machid);
	check(error == 0, "machine: %s", strerror(error));
	if (error)
		return;
	if (!spin_machine(fd, machid, NSPIN, &mem)) {
		check(false, "spin machine setup");
		goto out;
	}

	/* 1. A signal to the thread running the vCPU. */
	memset(&ctx[0], 0, sizeof(ctx[0]));
	ctx[0].fd = fd;
	ctx[0].machid = machid;
	ctx[0].cpuid = 0;
	pthread_create(&thr[0], NULL, forever_runner, &ctx[0]);
	usleep(200000);
	/* Informational: preemption may bring it back now and then. */
	printf("  spinning alone for 200 ms: back in userland %d times\n",
	    atomic_load(&ctx[0].nreturns));
	n = atomic_load(&ctx[0].nreturns);
	pthread_kill(thr[0], SIGUSR1);
	ms = wait_return(&ctx[0], n);
	check(ms >= 0, "signal: vcpu_run did not return within 1 s");
	printf("  signal: back in userland after %.2f ms\n", ms);

	/* 2. comm->stop. */
	comm = comm_map(fd, machid, 0);
	usleep(50000);
	n = atomic_load(&ctx[0].nreturns);
	comm->stop = 1;
	ms = wait_return(&ctx[0], n);
	check(ms >= 0, "stop: vcpu_run did not return within 1 s");
	pthread_join(thr[0], NULL);
	check(atomic_load(&ctx[0].last_reason) == NVMM_VCPU_EXIT_STOPPED,
	    "stop: last exit 0x%jx, expected STOPPED",
	    (uintmax_t)atomic_load(&ctx[0].last_reason));
	check(comm->stop == 0, "stop flag not cleared by the kernel");
	printf("  stop: STOPPED after %.2f ms\n", ms);
	munmap(comm, getpagesize());

	/* 3. NSPIN spinning vCPUs on fewer host CPUs for 2 s. */
	for (i = 0; i < NSPIN; i++) {
		memset(&ctx[i], 0, sizeof(ctx[i]));
		ctx[i].fd = fd;
		ctx[i].machid = machid;
		ctx[i].cpuid = i;
		pthread_create(&thr[i], NULL, forever_runner, &ctx[i]);
	}
	worst = 0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < 2000) {
		struct timespec s0;

		clock_gettime(CLOCK_MONOTONIC, &s0);
		usleep(10000);
		ms = elapsed_ms(&s0) - 10;
		if (ms > worst)
			worst = ms;
	}
	for (i = 0; i < NSPIN; i++) {
		struct nvmm_comm_page *c = comm_map(fd, machid, i);

		atomic_store(&ctx[i].done, true);
		c->stop = 1;
		munmap(c, getpagesize());
	}
	minret = INT32_MAX;
	for (i = 0; i < NSPIN; i++) {
		pthread_join(thr[i], NULL);
		check(ctx[i].error == 0, "vcpu %d: error %d", i, ctx[i].error);
		if (atomic_load(&ctx[i].nreturns) < minret)
			minret = atomic_load(&ctx[i].nreturns);
	}
	check(minret > 1, "a spinning vCPU never gave the CPU back");
	check(worst < 500, "host thread delayed %.1f ms by spinning guests",
	    worst);
	printf("  %d spinning vCPUs for 2 s: each back in userland >= %d "
	    "times, worst extra host sleep delay %.2f ms\n", NSPIN, minret,
	    worst);

	/* 4. A child with a spinning vCPU, killed. */
	pid = fork();
	if (pid == -1)
		err(1, "fork");
	if (pid == 0) {
		struct nvmm_vcpu_exit exit;
		nvmm_machid_t m;
		uint8_t *cm;
		int cfd = open_nvmm();

		if (machine_create(cfd, &m) != 0 ||
		    !spin_machine(cfd, m, 1, &cm))
			_exit(2);
		for (;;)
			(void)vcpu_run(cfd, m, 0, &exit);
	}
	usleep(300000);
	kill(pid, SIGKILL);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	if (waitpid(pid, &status, 0) == -1)
		err(1, "waitpid");
	ms = elapsed_ms(&t0);
	check(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
	    "child status 0x%x", status);
	printf("  child with a spinning vCPU killed in %.2f ms\n", ms);

	munmap(mem, 4 * getpagesize());
out:
	error = machine_destroy(fd, machid);
	check(error == 0, "destroy machine: %s", strerror(error));
	check_counters("run signals");
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

	printf("exit without destroy: 4 machines x 4 vcpus + memory in a child\n");
	fflush(stdout);	/* or the child prints the parent's buffer again */
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
			/* And 16 pages of guest memory, touched. */
			{
				size_t sz = 16 * (size_t)getpagesize();
				uint8_t *h = mmap(NULL, sz,
				    PROT_READ | PROT_WRITE,
				    MAP_ANON | MAP_PRIVATE, -1, 0);

				if (h == MAP_FAILED ||
				    hva_map(fd, machid, h, sz) != 0 ||
				    gpa_map(fd, machid, h, 0, sz,
				    PROT_READ | PROT_WRITE) != 0)
					_exit(4);
				memset(h, (int)m, sz);
			}
		}
		get_counters(&mid);
		print_counters("in the child:", &mid);
		fflush(stdout);
		_exit(0);
	}
	if (waitpid(pid, &status, 0) == -1)
		err(1, "waitpid");
	check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	    "child failed (status 0x%x)", status);
	check_counters("exit without destroy");
}

int
main(int argc, char **argv)
{
	struct counters end;
	bool only_run;
	int fd;

	only_run = argc > 1 && strcmp(argv[1], "run") == 0;
	if (argc > 1 && strcmp(argv[1], "avx") == 0) {
		setvbuf(stdout, NULL, _IOLBF, 0);
		fd = open_nvmm();
		get_counters(&base);
		test_run_avx(fd);
		close(fd);
		printf("%s\n", nfail ? "FAILED" : "OK");
		return nfail ? 1 : 0;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);

	fd = open_nvmm();
	{
		int dbg = 0;
		size_t len = sizeof(dbg);

		(void)sysctlbyname("hw.nvmm.debug", &dbg, &len, NULL, 0);
		have_debug = dbg != 0;
	}
	get_counters(&base);
	print_counters("at start:", &base);

	if (!only_run) {
		test_machines(fd);
		test_vcpus(fd);
		test_state(fd);
		test_configure_inject(fd);
		test_memory(fd);
	}
	test_run(fd);
	test_run_ept(fd);
	test_run_cpuid_msr(fd);
	test_run_fpu(fd);
	test_run_avx(fd);
	test_run_signals(fd);
	close(fd);
	if (!only_run)
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
