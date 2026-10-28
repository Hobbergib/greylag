#!/bin/sh
# qemu_test.sh IMAGE MARKER [MARKER2]
# Boots IMAGE headless and waits for the console to show MARKER (and
# MARKER2, if given, on the same line). Fails on a kernel panic, a failed
# check, or a 60 second timeout. Prints the console either way.

image=$1
marker=$2
marker2=${3:-}
qemu=${QEMU:-qemu-system-x86_64}
log=$(mktemp)

"$qemu" -m 128M -serial stdio -display none -no-reboot -kernel "$image" </dev/null >"$log" 2>&1 &
pid=$!

status=1
i=0
while [ $i -lt 600 ]; do
    if grep -q -e "kernel panic" -e "FAILED" -e "FAIL:" "$log"; then
        break
    fi
    if grep -F "$marker" "$log" | grep -q -F -- "$marker2"; then
        status=0
        break
    fi
    if ! kill -0 $pid 2>/dev/null; then
        break
    fi
    sleep 0.1
    i=$((i + 1))
done
sleep 0.2
kill $pid 2>/dev/null
wait $pid 2>/dev/null

tr -d '\r' <"$log"
rm -f "$log"
if [ $status -eq 0 ]; then echo "== PASS"; else echo "== FAIL"; fi
exit $status
