#!/bin/sh
# Read-only process sampler: one line per graphics-stack process: comm, pid, ppid, start epoch,
# Chromium role (--type). Distinguishes browser / renderer / gpu / utility / zygote and who
# replaced whom. No signals, no writes.
now=$(date +%s)
boot=$(awk '/^btime/{print $2}' /proc/stat 2>/dev/null)
hz=$(getconf CLK_TCK 2>/dev/null); [ -n "$hz" ] || hz=100
for d in /proc/[0-9]*; do
  [ -r "$d/comm" ] || continue
  c=$(cat "$d/comm" 2>/dev/null)
  case "$c" in steam|steamwebhelper|steamsysinfo|gamescope|Xwayland) ;; *) continue ;; esac
  pid=${d#/proc/}
  stat=$(cat "$d/stat" 2>/dev/null) || continue
  rest=${stat##*) }
  set -- $rest
  ppid=$2; startj=$20
  start=$((boot + startj / hz))
  cl=$(tr '\0' ' ' < "$d/cmdline" 2>/dev/null)
  role=browser
  case "$cl" in
    *--type=gpu-process*) role=gpu ;;
    *--type=renderer*) role=renderer ;;
    *--type=utility*) role=utility ;;
    *--type=zygote*) role=zygote ;;
    *--type=broker*) role=broker ;;
  esac
  echo "P now=$now $c pid=$pid ppid=$ppid born=$start role=$role"
done
