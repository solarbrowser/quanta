#!/bin/bash
# usage: battery.sh /path/to/quanta   -- every test must give the same md5 in the three modes
Q=${1:-/home/ata/Documents/GitHub/quanta/build/bin/quanta}; T=$(dirname $0)/tests
for t in nat_sem env_store edge leak deopt1 deopt2 nat_new range fcmp icmp nullish throwguard globalstore rtfast ctor_leaf plain_elems gctor_store bl_calls osr_vdom strict_swap getter_frames; do
  for e in "QUANTA_OPT=0 QUANTA_JIT_MODE=0 QUANTA_JIT_THRESHOLD=2" "QUANTA_NATIVE=0" "QUANTA_NATIVE_AT=2 QUANTA_OPT_ARM=1 QUANTA_OPT_PROFILE=1 QUANTA_OPT_DEOPT_STRESS=5" "QUANTA_NATIVE_AT=2 QUANTA_GC_STRESS=2"; do
    env $e timeout 300 $Q $T/$t.js 2>&1 | md5sum | cut -c1-8
  done | sort | uniq -c | awk -v t=$t '{printf "%s %s x%s%s", t, $2, $1, (NR==1?"":" DIFFERENT")} END{print ""}'
done
