/*	$NetBSD: nvmmctl.c,v 1.2 2020/09/05 07:22:26 maxv Exp $	*/

/*
 * Copyright (c) 2019-2020 Maxime Villard, m00nbsd.net
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

#include <sys/cdefs.h>
#ifndef lint
__RCSID("$NetBSD: nvmmctl.c,v 1.2 2020/09/05 07:22:26 maxv Exp $");
#endif /* not lint */

#include <sys/param.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#if defined(__FreeBSD__)
#include <libutil.h>	/* humanize_number() */
#else
#include <util.h>
#endif
#include <nvmm.h>

#if defined(__FreeBSD__)
#define __dead	__dead2

/* NetBSD's <x86/specialreg.h> */
#define XCR0_FLAGS1	"\177\020"					  \
	"b\000"		"x87\0"						  \
	"b\001"		"SSE\0"						  \
	"b\002"		"AVX\0"						  \
	"b\003"		"BNDREGS\0"					  \
	"b\004"		"BNDCSR\0"					  \
	"b\005"		"Opmask\0"					  \
	"b\006"		"ZMM_Hi256\0"					  \
	"b\007"		"Hi16_ZMM\0"					  \
	"b\010"		"PT\0"						  \
	"b\011"		"PKRU\0"					  \
	"b\013"		"CET_U\0"					  \
	"b\014"		"CET_S\0"					  \
	"b\015"		"HDC\0"						  \
	"b\017"		"LBR\0"						  \
	"b\020"		"HWP\0"						  \
	"b\021"		"TILECFG\0"					  \
	"b\022"		"TILEDATA\0"					  \
	"b\076"		"LWP\0"						  \
	"b\077"		"X\0"

/*
 * FreeBSD has no snprintb(3). The subset used here: the old format
 * ("\20" then <bit number, 1-based><name>...) and the new one ("\177\020"
 * then "b<bit, 0-based><name>\0"...), printed as NetBSD does:
 * 0x<value><NAME,NAME>.
 */
static int
snprintb(char *buf, size_t size, const char *fmt, uint64_t val)
{
	const char *p;
	size_t len;
	bool any = false;

	len = (size_t)snprintf(buf, size, "0x%jx", (uintmax_t)val);
#define APPEND(str) do {						\
		if (len < size)						\
			len += strlcpy(buf + len, (str), size - len);	\
	} while (0)
	if (fmt[0] == '\177') {
		for (p = fmt + 2; *p == 'b'; p += strlen(p) + 1) {
			unsigned bit = (unsigned char)p[1];

			if (bit < 64 && (val & (1ULL << bit))) {
				APPEND(any ? "," : "<");
				APPEND(p + 2);
				any = true;
			}
		}
	} else {
		for (p = fmt + 1; *p != '\0';) {
			unsigned bit = (unsigned char)*p++ - 1;
			char name[32];
			size_t n = 0;

			while (*p > ' ' && n < sizeof(name) - 1)
				name[n++] = *p++;
			name[n] = '\0';
			if (bit < 64 && (val & (1ULL << bit))) {
				APPEND(any ? "," : "<");
				APPEND(name);
				any = true;
			}
		}
	}
	if (any)
		APPEND(">");
#undef APPEND
	return (int)len;
}
#else
#include <x86/specialreg.h>
#endif

__dead static void usage(void);
static void nvmm_identify(char **);
static void nvmm_list(char **);

static struct cmdtab {
	const char *label;
	bool takesargs;
	bool argsoptional;
	void (*func)(char **);
} const nvmm_cmdtab[] = {
	{ "identify",	false, false, nvmm_identify },
	{ "list",	false, false, nvmm_list },
	{ NULL,		false, false, NULL },
};

static struct nvmm_capability cap;

int
main(int argc, char **argv)
{
	const struct cmdtab *ct;

	argc -= 1;
	argv += 1;
	if (argc < 1)
		usage();

	for (ct = nvmm_cmdtab; ct->label != NULL; ct++) {
		if (strcmp(argv[0], ct->label) == 0) {
			if (!ct->argsoptional &&
			    ((ct->takesargs == 0) ^ (argv[1] == NULL)))
			{
				usage();
			}
			(*ct->func)(argv + 1);
			break;
		}
	}

	if (ct->label == NULL)
		errx(EXIT_FAILURE, "unknown command ``%s''", argv[0]);

	exit(EXIT_SUCCESS);
	/* NOTREACHED */
}

static void
usage(void)
{
	const char *progname = getprogname();

	fprintf(stderr, "usage: %s identify\n", progname);
	fprintf(stderr, "       %s list\n", progname);
	exit(EXIT_FAILURE);
	/* NOTREACHED */
}

#define MACH_CONF_FLAGS		"\20"
#define VCPU_CONF_FLAGS		"\20" "\1" "CPUID" "\2" "TPR"

static void
nvmm_identify(char **argv)
{
	char buf[256], ram[4+1];

	if (nvmm_init() == -1)
		err(EXIT_FAILURE, "nvmm_init failed");
	if (nvmm_capability(&cap) == -1)
		err(EXIT_FAILURE, "nvmm_capability failed");

	printf("nvmm: Kernel API version %u\n", cap.version);
	printf("nvmm: State size %u\n", cap.state_size);
	printf("nvmm: Max machines %u\n", cap.max_machines);
	printf("nvmm: Max VCPUs per machine %u\n", cap.max_vcpus);

	if (humanize_number(ram, sizeof(ram), cap.max_ram, "", HN_AUTOSCALE,
	    (HN_DECIMAL | HN_B | HN_NOSPACE)) == -1)
		err(EXIT_FAILURE, "humanize_number");
	printf("nvmm: Max RAM per machine %s\n", ram);

	snprintb(buf, sizeof(buf), MACH_CONF_FLAGS, cap.arch.mach_conf_support);
	printf("nvmm: Arch Mach conf %s\n", buf);

	snprintb(buf, sizeof(buf), VCPU_CONF_FLAGS, cap.arch.vcpu_conf_support);
	printf("nvmm: Arch VCPU conf %s\n", buf);

	snprintb(buf, sizeof(buf), XCR0_FLAGS1, cap.arch.xcr0_mask);
	printf("nvmm: Guest FPU states %s\n", buf);
}

static void
nvmm_list(char **argv)
{
	struct nvmm_ctl_mach_info machinfo;
	char ram[4+1], *ts;
	size_t i;
	int ret;

	if (nvmm_root_init() == -1)
		err(EXIT_FAILURE, "nvmm_root_init failed");
	if (nvmm_capability(&cap) == -1)
		err(EXIT_FAILURE, "nvmm_capability failed");

	printf(
	    "Machine ID VCPUs RAM  Owner PID Creation Time           \n"
	    "---------- ----- ---- --------- ------------------------\n");

	for (i = 0; i < cap.max_machines; i++) {
		machinfo.machid = i;
		ret = nvmm_ctl(NVMM_CTL_MACH_INFO, &machinfo, sizeof(machinfo));
		if (ret == -1) {
			if (errno == ENOENT)
				continue;
			err(EXIT_FAILURE, "nvmm_ctl failed");
		}

		ts = asctime(localtime(&machinfo.time));
		ts[strlen(ts) - 1] = '\0';

		if (humanize_number(ram, sizeof(ram), machinfo.nram, "",
		    HN_AUTOSCALE, (HN_DECIMAL | HN_B | HN_NOSPACE)) == -1)
			err(EXIT_FAILURE, "humanize_number");

		printf("%-10zu %-5u %-4s %-9d %s\n", i, machinfo.nvcpus, ram,
		    machinfo.pid, ts);
	}
}
