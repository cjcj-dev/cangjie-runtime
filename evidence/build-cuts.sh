#!/bin/bash
set -u
lane=sym_cangjie_runtime_607_implement_r5738304930
for arm in producer consumer promotion final strong nonmajor partial; do
 (bash /root/cj_build/ops/bin/wf_kkk2.sh build /root/cj_build/agent_scratch/$lane/finalcuts/$arm $lane-finalcut-$arm > /root/cj_build/cangjie_runtime_wt/$lane/evidence/final-build-$arm.log 2>&1
  echo "$arm wrapper_rc=$?"
  grep '^== ' /root/cj_build/cangjie_runtime_wt/$lane/evidence/final-build-$arm.log
 ) &
done
wait
