#!/usr/bin/env bash
# Build and run every reconstructed-simulation native test. These check the
# port/sim layers against behavioural / mathematical identities and need NO game
# data, so they run anywhere (locally and in CI) and continuously verify the
# reconstruction. Each <layer>_test.c is linked with the non-test sim sources
# (axis_sim.c is excluded: it is the faithful dispatcher skeleton whose
# unreconstructed callees are intentionally extern).
#
#   cd port/sim && ./run_tests.sh      # CC=gcc ./run_tests.sh to pick a compiler
set -u
cd "$(dirname "$0")"
CC=${CC:-cc}
SRC=$(ls *.c | grep -v '_test\.c$' | grep -v '^axis_sim\.c$')
fail=0 pass=0
for t in *_test.c; do
  if ! $CC -O2 -Wall $SRC "$t" -lm -o /tmp/axis_sim_test 2>/tmp/axis_sim_err; then
    echo "FAIL(build) $t"; sed 's/^/    /' /tmp/axis_sim_err; fail=$((fail+1)); continue
  fi
  if out=$(/tmp/axis_sim_test 2>&1); then
    echo "PASS  $t  —  $(printf '%s' "$out" | tr '\n' ' ' | sed 's/  */ /g' | tail -c 80)"
    pass=$((pass+1))
  else
    echo "FAIL(run) $t"; printf '%s\n' "$out" | sed 's/^/    /'; fail=$((fail+1))
  fi
done
echo "----"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
