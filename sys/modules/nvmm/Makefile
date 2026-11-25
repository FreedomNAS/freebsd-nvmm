# Out-of-tree build of nvmm.ko against the kernel sources in SYSDIR
# (default /usr/src/sys, which must match the running kernel).

.PATH: ${.CURDIR}/../../dev/nvmm ${.CURDIR}/../../dev/nvmm/x86

KMOD=	nvmm
SRCS=	nvmm.c nvmm_freebsd.c
SRCS+=	nvmm_x86.c nvmm_x86_vmx.c
# <x86/apicvar.h> pulls in <sys/bus.h>, which wants the generated headers.
SRCS+=	bus_if.h device_if.h

# <dev/nvmm/...> must resolve to this tree, not to SYSDIR.
CFLAGS+=	-I${.CURDIR}/../..

.include <bsd.kmod.mk>
