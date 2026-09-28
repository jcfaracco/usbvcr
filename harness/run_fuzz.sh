#!/usr/bin/env bash
#
# usbredir guest-USB-stack fuzzing harness.
#
# Boots a QEMU guest (ideally a KASAN/UBSAN kernel) exposing a usbredir
# chardev socket, then repeatedly connects the fuzzer with incrementing
# seeds. After each run it scans the guest serial log for a crash signature;
# on a hit it saves the seed + recorded frame log as a reproducer and
# restarts the VM.
#
# Prerequisites (edit the CONFIG block):
#   - A guest disk image that auto-boots and logs to serial (ttyS0).
#   - A kernel built with CONFIG_KASAN=y, CONFIG_UBSAN=y, panic_on_warn=1.
#   - qemu-system-x86_64, and the built ./build/src/client/usb_redir_hello.
#
set -uo pipefail

# ----- CONFIG (edit me) --------------------------------------------------
QEMU=${QEMU:-qemu-system-x86_64}
IMAGE=${IMAGE:?set IMAGE=/path/to/guest.qcow2}
KERNEL=${KERNEL:-}                      # optional: -kernel path (else disk boots)
APPEND=${APPEND:-"console=ttyS0 kasan.fault=panic panic_on_warn=1 oops=panic"}
FUZZER=${FUZZER:-./build/src/client/usb_redir_hello}
HOST=127.0.0.1
PORT=${PORT:-4000}
SEED_START=${SEED_START:-1}
SEED_END=${SEED_END:-100000}
BOOT_WAIT=${BOOT_WAIT:-25}              # seconds to let the guest come up
OUTDIR=${OUTDIR:-./findings}
CRASH_RE='KASAN|UBSAN|BUG:|kernel panic|general protection|Unable to handle'
# ------------------------------------------------------------------------

mkdir -p "$OUTDIR"
SERIAL_LOG=$(mktemp)
QEMU_PID=""

start_vm() {
    : > "$SERIAL_LOG"
    local kargs=()
    [[ -n "$KERNEL" ]] && kargs+=(-kernel "$KERNEL" -append "$APPEND")
    "$QEMU" -m 1G -smp 2 -enable-kvm \
        -drive file="$IMAGE",format=qcow2,if=virtio \
        "${kargs[@]}" \
        -device qemu-xhci,id=xhci \
        -chardev socket,id=u0,host="$HOST",port="$PORT",server=on,wait=off \
        -device usb-redir,chardev=u0,bus=xhci.0 \
        -nographic -serial file:"$SERIAL_LOG" &
    QEMU_PID=$!
    sleep "$BOOT_WAIT"
}

stop_vm() {
    [[ -n "$QEMU_PID" ]] && kill "$QEMU_PID" 2>/dev/null
    wait "$QEMU_PID" 2>/dev/null
    QEMU_PID=""
}

crashed() { grep -Eq "$CRASH_RE" "$SERIAL_LOG"; }

save_repro() {
    local seed=$1 rec=$2
    local dir="$OUTDIR/crash_seed_${seed}"
    mkdir -p "$dir"
    cp "$rec" "$dir/frames.bin"
    cp "$SERIAL_LOG" "$dir/serial.log"
    echo "$seed" > "$dir/seed.txt"
    echo "[!] CRASH seed=$seed -> $dir"
    echo "    replay: $FUZZER --replay $dir/frames.bin $HOST $PORT"
}

trap stop_vm EXIT

start_vm
for ((seed = SEED_START; seed <= SEED_END; seed++)); do
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        echo "[*] QEMU died (likely a crash); inspecting..."
        crashed && save_repro "$((seed - 1))" "$OUTDIR/last.bin"
        start_vm
    fi

    "$FUZZER" --seed "$seed" --record "$OUTDIR/last.bin" "$HOST" "$PORT" \
        >/dev/null 2>&1

    if crashed; then
        save_repro "$seed" "$OUTDIR/last.bin"
        stop_vm
        start_vm
    fi

    (( seed % 500 == 0 )) && echo "[*] seed=$seed"
done
