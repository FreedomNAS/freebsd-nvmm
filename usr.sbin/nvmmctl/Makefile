# nvmmctl(8) for FreeBSD, from NetBSD's usr.sbin/nvmmctl, in /usr/sbin
# (PORTING.md, decision P6-1). Not setgid: /dev/nvmm is root:wheel 0600
# until the group is decided with the FreedomNAS VM module.

PROG=		nvmmctl
MAN=		nvmmctl.8
BINDIR=		/usr/sbin

CFLAGS+=	-I${.CURDIR}/../../lib/libnvmm -I${.CURDIR}/../../sys
LDFLAGS+=	-L${.OBJDIR}/../../lib/libnvmm
LDADD+=		-lutil -lnvmm
WARNS?=		3

.include <bsd.prog.mk>
