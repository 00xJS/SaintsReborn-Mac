#!/bin/bash
# Sums up the last play session from the game's log: frame rate and memory.
# Needs a file named "perf_log" next to saintsrow.exe (the Mac setup creates it).
#   scripts/mac/perf_summary.sh            last session
#   scripts/mac/perf_summary.sh all        every session in the current log
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LOG="$ROOT/dist/saintsrow_sdk.log"
[ -f "$LOG" ] || { echo "No log at $LOG"; exit 1; }
if [ "${1:-}" = "all" ]; then DATA="$(cat "$LOG")"; else
  START="$(grep -n "Saints Row starting" "$LOG" | tail -1 | cut -d: -f1)"; DATA="$(tail -n +"${START:-1}" "$LOG")"; fi
echo "$DATA" | grep -h "Draw resolution\|Anti-aliasing\|Shadows:\|Texture cache limits\|FPS cap" | sed 's/.*\] //' | sort -u
echo "$DATA" | grep -o "PERF fps [0-9.]*" | awk '$3>0{a[++n]=$3; s+=$3} END{ if(!n){print "No frame rate samples (is perf_log present?)"; exit}
  asort(a); printf "Frame rate: %d samples of 2 s | average %.1f | median %.1f | slowest 10%% under %.1f | fastest %.1f\n", n, s/n, a[int(n/2)+1], a[int(n/10)+1], a[n] }'
echo "$DATA" | grep -o "working set [0-9]* MiB.*system available [0-9]* MiB" | awk '{ws=$3; av=$(NF-1); if(ws>mw)mw=ws; if(n==0||av<ma)ma=av; n++} END{ if(n) printf "Memory: game up to %d MiB | system free dropped to %d MiB\n", mw, ma }'
