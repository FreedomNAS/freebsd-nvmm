#!/bin/sh
# Runs inside FreeBSD's official 15.1 VM image (booted by the image
# builder's qemu-driver.sh) and turns FNAS_TARGET_DISK into a plain
# FreeBSD 15.1 build machine for the nvmm module: whole-disk GPT that
# boots on BIOS and UEFI, UFS root, base system from pkgbase, git, sudo,
# kernel sources of releng/15.1, sshd, user admin.
#
# Completion: the log and a RESULT line are written to the start of the
# swap partition (label nvmmswap), which the host reads back after the
# guest powers off.

SEED=/fnas-seed
LOG=/tmp/devbox.log
# shellcheck source=/dev/null
. "$SEED/env.sh"

main() {
    set -eu
    D="${FNAS_TARGET_DISK:?}"
    N="${D#/dev/}"
    R=/mnt

    echo "== partitioning $D"
    gpart destroy -F "$N" 2>/dev/null || true
    gpart create -s gpt "$N"
    gpart add -t freebsd-boot -s 512k -l nvmmboot "$N"
    gpart add -t efi -a 1m -s 260m -l nvmmefi "$N"
    gpart add -t freebsd-swap -a 1m -s 8g -l nvmmswap "$N"
    gpart add -t freebsd-ufs -a 1m -l nvmmroot "$N"
    gpart show "$N"

    newfs -U -L nvmmroot "/dev/${N}p4"
    mount "/dev/${N}p4" "$R"

    echo "== base system from pkgbase ($RELEASE)"
    export ASSUME_ALWAYS_YES=yes
    pkg bootstrap -y >/dev/null
    v="${RELEASE%%-*}"
    mkdir -p "$R/usr/share/keys" /tmp/repos
    cp -R /usr/share/keys/. "$R/usr/share/keys/"
    cat > /tmp/repos/FreeBSD-base.conf <<EOF
FreeBSD-base: {
  url: "pkg+https://pkg.FreeBSD.org/FreeBSD:${v%%.*}:amd64/base_release_${v#*.}",
  mirror_type: "srv",
  signature_type: "fingerprints",
  fingerprints: "/usr/share/keys/pkgbase-${v%%.*}",
  enabled: yes
}
EOF
    P="pkg --rootdir $R --repo-conf-dir /tmp/repos -o IGNORE_OSVERSION=yes"
    $P update
    $P install -U -y -r FreeBSD-base FreeBSD-set-minimal FreeBSD-set-base FreeBSD-set-kernels
    $P clean -ay
    chroot "$R" certctl rehash
    mkdir -p "$R/usr/local/etc/pkg/repos"
    echo 'FreeBSD-base: { enabled: yes }' > "$R/usr/local/etc/pkg/repos/FreeBSD-base.conf"

    echo "== boot code"
    gpart bootcode -b "$R/boot/pmbr" -p "$R/boot/gptboot" -i 1 "$N"
    newfs_msdos -F 32 -c 1 -L NVMMEFI "/dev/${N}p2"
    mkdir -p /tmp/esp
    mount -t msdosfs "/dev/${N}p2" /tmp/esp
    mkdir -p /tmp/esp/EFI/BOOT /tmp/esp/EFI/freebsd
    cp "$R/boot/loader.efi" /tmp/esp/EFI/BOOT/BOOTX64.efi
    cp "$R/boot/loader.efi" /tmp/esp/EFI/freebsd/loader.efi
    umount /tmp/esp

    echo "== configuration"
    cat > "$R/etc/fstab" <<'EOF'
/dev/gpt/nvmmroot	/		ufs	rw	1	1
/dev/gpt/nvmmswap	none		swap	sw	0	0
/dev/gpt/nvmmefi	/boot/efi	msdosfs	rw	2	2
EOF
    mkdir -p "$R/boot/efi"
    cat > "$R/etc/rc.conf" <<'EOF'
hostname="nvmm-dev"
ifconfig_DEFAULT="DHCP SYNCDHCP"
sshd_enable="YES"
ntpd_enable="YES"
ntpd_sync_on_start="YES"
dumpdev="AUTO"
clear_tmp_enable="YES"
EOF
    cat > "$R/boot/loader.conf" <<'EOF'
autoboot_delay="3"
boot_multicons="YES"
console="vidconsole,comconsole"
EOF
    cp "$R/usr/share/zoneinfo/Europe/Rome" "$R/etc/localtime"
    cp /etc/resolv.conf "$R/etc/resolv.conf"

    mount -t devfs devfs "$R/dev"
    chroot "$R" env ASSUME_ALWAYS_YES=yes pkg bootstrap -y
    chroot "$R" env ASSUME_ALWAYS_YES=yes pkg install -y git-lite sudo rsync

    echo "== users"
    echo "$PASSWORD" | chroot "$R" pw usermod root -h 0
    chroot "$R" pw useradd admin -m -G wheel -s /bin/sh -c "admin"
    echo "$PASSWORD" | chroot "$R" pw usermod admin -h 0
    for h in root/.ssh home/admin/.ssh; do
        mkdir -p "$R/$h"
        cp "$SEED/authorized_keys" "$R/$h/authorized_keys"
        chmod 700 "$R/$h"
        chmod 600 "$R/$h/authorized_keys"
    done
    chroot "$R" chown -R admin:admin /home/admin/.ssh
    echo '%wheel ALL=(ALL:ALL) NOPASSWD: ALL' > "$R/usr/local/etc/sudoers.d/wheel"
    chmod 440 "$R/usr/local/etc/sudoers.d/wheel"

    echo "== kernel sources ($SRC_BRANCH)"
    chroot "$R" git clone -q --depth 1 -b "$SRC_BRANCH" "$SRC_URL" /usr/src
    chroot "$R" git -C /usr/src log -1 --format='%h %cs %s'
    grep -E '^(REVISION|BRANCH)=' "$R/usr/src/sys/conf/newvers.sh"
    chroot "$R" freebsd-version -ku

    umount "$R/dev"
    df -h "$R"
    sync
    umount "$R"
    echo "RESULT ok"
}

main 2>&1 | tee "$LOG"
grep -q '^RESULT ok' "$LOG" || echo "RESULT fail" >> "$LOG"
dd if="$LOG" of=/dev/gpt/nvmmswap bs=1m conv=sync 2>/dev/null || true
sync
poweroff
