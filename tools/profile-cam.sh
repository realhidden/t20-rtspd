#!/bin/sh
# profile-cam.sh — per-thread CPU + context-switch profile for t20-rtspd on a camera.
#
# Usage: profile-cam.sh <pid> [seconds]
#   defaults to the running t20-rtspd pid and a 60s window.
#
# Reads /proc/<pid>/task/*/{stat,status} twice and reports per-thread CPU
# deltas, so we can attribute cost to the capture loop, the uploader, the
# autonight thread, or IMP SDK worker threads.
#
# Clock ticks are assumed to be 100 Hz (USER_HZ on the MIPS/uclibc camera),
# so 100 ticks == 1s of CPU. Verify with `getconf CLK_TCK` if unsure.

PID="${1:-}"
SECS="${2:-60}"

if [ -z "$PID" ]; then
    PID=$(pidof t20rtspd 2>/dev/null | awk '{print $1}')
fi
if [ -z "$PID" ] || [ ! -d "/proc/$PID/task" ]; then
    echo "no such pid: '$PID'" >&2
    exit 1
fi

HZ=$(getconf CLK_TCK 2>/dev/null || echo 100)
TICKS=$((SECS * HZ))
echo "=== t20-rtspd profile: pid $PID, ${SECS}s window, HZ=$HZ ==="

sample() {
    for t in /proc/"$PID"/task/*; do
        tid=$(basename "$t")
        [ -r "$t/stat" ] || continue
        # comm can contain spaces/parens: split on the LAST ')'
        l=$(cat "$t/stat" 2>/dev/null)
        name=$(echo "$l" | sed 's/^[0-9]* (//; s/)[^)]*$//')
        # everything after the last ')' : state is field 1, so utime=12 stime=13
        rest=$(echo "$l" | sed 's/.*) //')
        ut=$(echo "$rest" | awk '{print $12}')
        st=$(echo "$rest" | awk '{print $13}')
        # voluntary/involuntary ctxt switches and page faults from status
        vcs=$(awk '/^voluntary_ctxt_switches/{print $2}' "$t/status" 2>/dev/null)
        ivcs=$(awk '/^nonvoluntary_ctxt_switches/{print $2}' "$t/status" 2>/dev/null)
        minf=$(awk '/^minflt/{print $2}' "$t/status" 2>/dev/null)
        echo "$tid $name $ut $st ${vcs:-0} ${ivcs:-0} ${minf:-0}"
    done
}

sample > /tmp/.prof_a
sleep "$SECS"
sample > /tmp/.prof_b

# Print the second sample keyed by tid so deltas are computed reliably.
# utime/stime are kept separate on purpose: stime is work the thread spent in
# the kernel (syscalls, ioctls, DMA/cache maintenance) and utime is work it
# spent in userspace. That split decides whether a kernel-driver rewrite
# could ever help -- if a thread's cost is utime, it is inside the vendor
# userspace library and no driver change will touch it.
awk -v TICKS="$TICKS" '
NR==FNR { u[$1]=$3; s[$1]=$4; vcs[$1]=$5; ivcs[$1]=$6; mf[$1]=$7; name[$1]=$2; next }
{
    du = $3 - u[$1]
    ds = $4 - s[$1]
    dt = du + ds
    pc = (dt*100.0)/TICKS
    pu = (du*100.0)/TICKS
    ps = (ds*100.0)/TICKS
    printf "%-8s %-16s cpu=%6.2f%%  usr=%6.2f%%  sys=%6.2f%%  (%s)  ctxsw=%-6d minflt=%d\n",
           $1, name[$1], pc, pu, ps, (ps > pu ? "KERNEL" : (pu > ps*2 ? "userspace" : "mixed")), \
           ($5 - vcs[$1]) + ($6 - ivcs[$1]), $7 - mf[$1]
}
' /tmp/.prof_a /tmp/.prof_b

echo
echo "=== system ==="
cat /proc/loadavg
grep -E '^(cpu|Mem)' /proc/stat 2>/dev/null | head -2
echo
echo "=== threads (comm) ==="
for t in /proc/"$PID"/task/*; do
    echo "  $(basename "$t") $(cat "$t/comm" 2>/dev/null)"
done
rm -f /tmp/.prof_a /tmp/.prof_b
