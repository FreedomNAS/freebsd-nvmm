# libnvmm for FreeBSD, from NetBSD's lib/libnvmm. Installs in the base
# system paths (PORTING.md, decision P6-1): /usr/lib/libnvmm.so.0,
# /usr/include/nvmm.h and the <dev/nvmm/...> headers it includes.

LIB=		nvmm
SHLIB_MAJOR=	0
SRCS=		libnvmm.c
MAN=		libnvmm.3

INCSGROUPS=	INCS DEVNVMM DEVNVMMX86
INCS=		nvmm.h
INCSDIR=	/usr/include
DEVNVMM=	${SYSNVMM}/nvmm.h ${SYSNVMM}/nvmm_ioctl.h ${SYSNVMM}/nvmm_bitops.h
DEVNVMMDIR=	/usr/include/dev/nvmm
DEVNVMMX86=	${SYSNVMM}/x86/nvmm_x86.h
DEVNVMMX86DIR=	/usr/include/dev/nvmm/x86

SYSNVMM=	${.CURDIR}/../../sys/dev/nvmm

# <dev/nvmm/...> from this tree, not from /usr/include.
CFLAGS+=	-I${.CURDIR}/../../sys
# NetBSD's WARNS=5 has neither -Wunused-parameter nor -Wcast-align; on
# FreeBSD those come with WARNS 4 and 6. The emulator has callbacks that
# ignore arguments and reads operands through byte pointers on x86.
WARNS?=		3

# Not in the base mtree: create the header directories.
beforeinstall:
	${INSTALL} -d ${DESTDIR}${DEVNVMMX86DIR}

.include <bsd.lib.mk>
