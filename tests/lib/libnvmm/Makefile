# NetBSD's libnvmm tests (I/O and memory assists) as ATF tests for kyua
# (PORTING.md, decision P6-2), installed in /usr/tests/lib/libnvmm.
# Run: kyua test -k /usr/tests/lib/libnvmm/Kyuafile

TESTSDIR=	${TESTSBASE}/lib/libnvmm

ATF_TESTS_SH=	t_io_assist t_mem_assist

PROGS=			h_io_assist h_mem_assist
SRCS.h_io_assist=	h_io_assist.c h_io_assist_asm.S
SRCS.h_mem_assist=	h_mem_assist.c h_mem_assist_asm.S
BINDIR=			${TESTSDIR}
MAN=

# nvmm.h and the private nvmm_compat.h from this tree.
CFLAGS+=	-I${.CURDIR}/../../../lib/libnvmm -I${.CURDIR}/../../../sys
CFLAGS+=	-Wall
LDFLAGS+=	-L${.OBJDIR}/../../../lib/libnvmm
LDADD+=		-lnvmm

.include <bsd.test.mk>
