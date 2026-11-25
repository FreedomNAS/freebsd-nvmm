# Out-of-tree build of nvmm.ko against the kernel sources in SYSDIR
# (default /usr/src/sys, which must match the running kernel).

.PATH: ${.CURDIR}/../../dev/nvmm ${.CURDIR}/../../dev/nvmm/x86

KMOD=	nvmm
SRCS=	nvmm.c nvmm_freebsd.c
SRCS+=	nvmm_x86.c nvmm_x86_vmx.c
# <x86/apicvar.h> pulls in <sys/bus.h>, which wants the generated headers.
SRCS+=	bus_if.h device_if.h

# Test build only (PORTING.md, decision P4-1): make -DNVMM_DEBUG adds the
# NVMM_IOC_DEBUG_GPA request used by tools/nvmmtest.
.if defined(NVMM_DEBUG)
CFLAGS+=	-DNVMM_DEBUG
.endif

# <dev/nvmm/...> must resolve to this tree, not to SYSDIR.
CFLAGS+=	-I${.CURDIR}/../..

.include <bsd.kmod.mk>
