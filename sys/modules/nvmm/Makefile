# Out-of-tree build of nvmm.ko against the kernel sources in SYSDIR
# (default /usr/src/sys, which must match the running kernel).

.PATH: ${.CURDIR}/../../dev/nvmm ${.CURDIR}/../../dev/nvmm/x86

KMOD=	nvmm
SRCS=	nvmm.c nvmm_freebsd.c
SRCS+=	nvmm_x86_vmx.c

# <dev/nvmm/...> must resolve to this tree, not to SYSDIR.
CFLAGS+=	-I${.CURDIR}/../..

.include <bsd.kmod.mk>
