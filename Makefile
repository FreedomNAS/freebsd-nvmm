# Userland of the FreeBSD NVMM port: libnvmm, nvmmctl(8) and the libnvmm
# tests, in the base system paths (PORTING.md, P6-1). The kernel module
# builds on its own in sys/modules/nvmm.
#
#   make && make install
#   kyua test -k /usr/tests/lib/libnvmm/Kyuafile

SUBDIR=		lib/libnvmm usr.sbin/nvmmctl tests/lib/libnvmm
SUBDIR_DEPEND_usr.sbin/nvmmctl=		lib/libnvmm
SUBDIR_DEPEND_tests/lib/libnvmm=	lib/libnvmm

.include <bsd.subdir.mk>
