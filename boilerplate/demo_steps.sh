#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

usage() {
    cat <<'EOF'
Usage:
  ./demo_steps.sh soft
  ./demo_steps.sh hard
  ./demo_steps.sh sched
  ./demo_steps.sh teardown

Notes:
- Run supervisor in another terminal before soft/hard/sched.
- Commands print the same evidence output used in ss.md sections 5-8.
EOF
}

require_supervisor() {
    if [[ ! -S /tmp/mini_runtime.sock ]]; then
        echo "Supervisor socket missing: /tmp/mini_runtime.sock"
        echo "Start supervisor first: sudo ./engine supervisor ./rootfs-base"
        exit 1
    fi
}

run_soft_limit() {
    require_supervisor

    local ts cid rootfs
    ts="$(date +%s)"
    cid="softcap${ts}"
    rootfs="./rootfs-soft-${ts}"

    cp -a ./rootfs-base "$rootfs"
    cp -f ./memory_hog "$rootfs/"
    chmod +x "$rootfs/memory_hog"

    echo "CID=$cid ROOTFS=$rootfs"
    sudo ./engine start "$cid" "$rootfs" "/memory_hog 8 500" --soft-mib 32 --hard-mib 256
    sleep 4
    sudo dmesg -T | grep -E "container_monitor|SOFT LIMIT|$cid" | tail -n 40 || true
    sudo ./engine ps | grep "$cid" || true
}

run_hard_limit() {
    require_supervisor

    local ts cid rootfs
    ts="$(date +%s)"
    cid="hardcap${ts}"
    rootfs="./rootfs-hard-${ts}"

    cp -a ./rootfs-base "$rootfs"
    cp -f ./memory_hog "$rootfs/"
    chmod +x "$rootfs/memory_hog"

    echo "CID=$cid ROOTFS=$rootfs"
    sudo ./engine start "$cid" "$rootfs" "/memory_hog 8 500" --soft-mib 32 --hard-mib 48
    sleep 5
    sudo dmesg -T | grep -E "container_monitor|HARD LIMIT|$cid" | tail -n 40 || true
    sudo ./engine ps | grep "$cid" || true
}

run_sched() {
    require_supervisor

    if ! command -v musl-gcc >/dev/null 2>&1; then
        echo "musl-gcc not found. Install with: sudo apt-get install -y musl-tools"
        exit 1
    fi

    musl-gcc -O2 -static -o cpu_hog cpu_hog.c
    cp -f cpu_hog rootfs-alpha/cpu_hog
    cp -f cpu_hog rootfs-beta/cpu_hog
    chmod +x rootfs-alpha/cpu_hog rootfs-beta/cpu_hog

    local ts
    ts="$(date +%s)"
    time sudo ./engine run "cpu-low-${ts}" ./rootfs-alpha "/cpu_hog 10" --nice 10
    time sudo ./engine run "cpu-high-${ts}" ./rootfs-beta "/cpu_hog 10" --nice -10
}

run_teardown() {
    if [[ -S /tmp/mini_runtime.sock ]]; then
        local ps_snapshot entry cid pid state

        ps_snapshot="$(sudo ./engine ps || true)"
        echo "$ps_snapshot"

        while IFS= read -r entry; do
            entry="$(echo "$entry" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')"
            [[ -z "$entry" || "$entry" == "no containers" ]] && continue

            cid="$(echo "$entry" | sed -nE 's/^([^ (]+)\(.*/\1/p')"
            pid="$(echo "$entry" | sed -nE 's/.*pid=([0-9]+).*/\1/p')"
            state="$(echo "$entry" | sed -nE 's/.*state=([^,)]*).*/\1/p')"

            if [[ "$state" == "running" || "$state" == "starting" ]]; then
                [[ -n "$cid" ]] && sudo ./engine stop "$cid" || true

                if [[ -n "$pid" ]] && ps -p "$pid" >/dev/null 2>&1; then
                    sudo kill -9 "$pid" || true
                fi
            fi
        done < <(echo "$ps_snapshot" | tr ';' '\n')

        sudo ./engine ps || true
    else
        echo "Supervisor socket not found. Skipping engine stop/ps."
    fi

    ps -ef | grep '[d]efunct' || true
    lsmod | grep -q '^monitor' && sudo rmmod monitor || true
}

if [[ $# -ne 1 ]]; then
    usage
    exit 1
fi

case "$1" in
    soft)
        run_soft_limit
        ;;
    hard)
        run_hard_limit
        ;;
    sched)
        run_sched
        ;;
    teardown)
        run_teardown
        ;;
    *)
        usage
        exit 1
        ;;
esac
