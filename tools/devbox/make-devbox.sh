#!/bin/sh
# Writes a plain FreeBSD 15.1 build machine for nvmm onto a whole disk,
# using the image builder's QEMU driver and FreeBSD's official VM image
# as the build environment.
#
# Usage: make-devbox.sh /dev/disk/by-id/<disk> <serial>
# The serial must match the disk: it is the confirmation that this disk
# may be erased. Needs root (raw disk access), qemu, xorriso, python3.
set -eu

DISK="$1"
SERIAL="$2"
HERE="$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)"
BUILDER="${FNAS_IMAGE_BUILDER:-$HERE/../../../freedomnas-image-builder}"
VM_IMAGE="${FREEBSD_VM_IMAGE:-/var/cache/freedomnas-ci/freebsd/FreeBSD-15.1-RELEASE-amd64-ufs.qcow2}"
PUBKEY="${DEVBOX_PUBKEY:-$HOME/.ssh/id_ed25519.pub}"

case "$DISK" in
    /dev/disk/by-id/*) ;;
    *) echo "make-devbox: give the disk as /dev/disk/by-id/..." >&2; exit 1 ;;
esac
[ -b "$DISK" ] || { echo "make-devbox: $DISK is not a block device" >&2; exit 1; }
DEV="$(readlink -f "$DISK")"
real="$(lsblk -dno SERIAL "$DEV")"
[ "$real" = "$SERIAL" ] || { echo "make-devbox: serial of $DEV is '$real', not '$SERIAL'" >&2; exit 1; }
if lsblk -no MOUNTPOINTS "$DEV" | grep -q .; then
    echo "make-devbox: $DEV has mounted partitions" >&2; exit 1
fi

SEED="$(mktemp -d)"
trap 'rm -rf "$SEED"' EXIT
cp "$HERE/devbox-in-guest.sh" "$SEED/"
cp "$PUBKEY" "$SEED/authorized_keys"
cat > "$SEED/env.sh" <<EOF
RELEASE="15.1-RELEASE"
SRC_BRANCH="releng/15.1"
SRC_URL="https://github.com/freebsd/freebsd-src.git"
PASSWORD="freedomnas"
EOF

QEMU_MEM="${QEMU_MEM:-4096}" QEMU_SMP="${QEMU_SMP:-4}" \
    QEMU_SCREENDUMP="${QEMU_SCREENDUMP:-/dev/shm/nvmm-devbox-screen.ppm}" \
    sh "$BUILDER/freebsd/qemu-driver.sh" "$DEV" "$SEED" "$VM_IMAGE" devbox-in-guest.sh

partprobe "$DEV" 2>/dev/null || blockdev --rereadpt "$DEV" 2>/dev/null || true
sleep 2
SWAP="$(lsblk -lnpo NAME,PARTLABEL "$DEV" | awk '$2 == "nvmmswap" { print $1 }')"
[ -n "$SWAP" ] || { echo "make-devbox: no nvmmswap partition: the guest did not get far" >&2; exit 1; }
dd if="$SWAP" bs=1M count=1 2>/dev/null | tr -d '\000'
dd if="$SWAP" bs=1M count=1 2>/dev/null | tr -d '\000' | grep -q '^RESULT ok'
