#!/bin/sh
#
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Sarah Chiandotto
#
# Phase 7 acceptance: boot a guest with QEMU, headless, until the login
# prompt on its serial console, and report how long it took.
#
#   guestboot.sh <accel> <name> <timeout-s> -- <qemu disk/cdrom args...>
#   accel: nvmm or tcg
#
# QEMU is ${QEMU} (default: qemu-system-x86_64 in PATH), with ${SMP:-2}
# vCPUs. The serial
# console goes to /tmp/guestboot-<name>-<accel>.serial, QEMU's own output
# to .qemu. Exit 0 when the prompt appeared.

set -u

[ $# -ge 4 ] && [ "$4" = "--" ] || {
	echo "usage: $0 <nvmm|tcg> <name> <timeout-s> -- <qemu args>" >&2
	exit 2
}
accel=$1 name=$2 tmo=$3
shift 4
QEMU=${QEMU:-qemu-system-x86_64}
base=/tmp/guestboot-$name-$accel
rm -f "$base.serial" "$base.qemu"

start=$(date +%s)
"$QEMU" -accel "$accel" -cpu max -m 1024 -smp "${SMP:-2}" \
    -display none -monitor none -parallel none \
    -serial "file:$base.serial" \
    -nic user,model=virtio-net-pci \
    "$@" >"$base.qemu" 2>&1 &
pid=$!

result=timeout
while :; do
	now=$(date +%s)
	if grep -q 'login:' "$base.serial" 2>/dev/null; then
		result=ok
		break
	fi
	if ! kill -0 "$pid" 2>/dev/null; then
		result=exited
		break
	fi
	[ $((now - start)) -ge "$tmo" ] && break
	sleep 1
done
elapsed=$(( $(date +%s) - start ))

kill "$pid" 2>/dev/null
wait "$pid" 2>/dev/null

printf '%-8s %-6s %-8s %4ss\n' "$name" "$accel" "$result" "$elapsed"
if [ "$result" != ok ]; then
	echo "--- last serial lines ($base.serial)"
	tail -n 15 "$base.serial" 2>/dev/null
	echo "--- qemu output ($base.qemu)"
	tail -n 15 "$base.qemu" 2>/dev/null
	exit 1
fi
grep -i 'operational\|nvmm' "$base.qemu" 2>/dev/null | head -n 3
exit 0
