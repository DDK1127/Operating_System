#!/bin/sh

DEMO="$1"
TEST="$2"

if [ -z "$DEMO" ] || [ -z "$TEST" ]; then
  echo "Usage: $0 <demo_bin> <test_bin>"
  exit 1
fi

ORIG=$(cat /proc/sys/kernel/sched_rt_runtime_us 2>/dev/null || echo "")
echo -1 > /proc/sys/kernel/sched_rt_runtime_us 2>/dev/null

i=1
for CASE in \
"-n 1 -t 0.5 -s NORMAL -p -1" \
"-n 2 -t 0.5 -s FIFO,FIFO -p 10,20" \
"-n 3 -t 1.0 -s NORMAL,FIFO,FIFO -p -1,10,30"
do
  echo "Running testcase $i: $DEMO $CASE"
  OUT1="$(mktemp)"; OUT2="$(mktemp)"

  sh -c "$DEMO $CASE" >"$OUT1" 2>&1
  sh -c "$TEST $CASE" >"$OUT2" 2>&1

  if diff -u "$OUT1" "$OUT2" >/dev/null 2>&1; then
    echo "Result: Success!"
  else
    echo "===== diff (demo vs test) ====="
    diff -u "$OUT1" "$OUT2" || true
    echo "Result: Failed..."
    rm -f "$OUT1" "$OUT2"
    [ -n "$ORIG" ] && echo "$ORIG" > /proc/sys/kernel/sched_rt_runtime_us 2>/dev/null
    exit 1
  fi

  rm -f "$OUT1" "$OUT2"
  i=$((i+1))
done

[ -n "$ORIG" ] && echo "$ORIG" > /proc/sys/kernel/sched_rt_runtime_us 2>/dev/null
