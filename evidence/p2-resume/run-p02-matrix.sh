#!/bin/bash
set -u
L=sym_cangjie_runtime_607_implement_r5684610492
run_group() {
 local cores=$1; shift
 for spec in "$@"; do
  read -r scenario arm <<< "$spec"
  bash /root/cj_build/ops/bin/wf_kkk2.sh sh "$L" "P2_CORES=$cores bash /root/$L-build9/run-case-arm.sh $scenario $arm"
 done
}
run_group 16-23 'slow green' 'slow slow9-strong' 'slow slow9-final' 'slow slow9-publish' 'slow restored' 'array green' 'array array9-partial' 'array restored' 'struct array9-partial' &
p1=$!
run_group 24-31 'full green' 'full young9-major' 'full restored' 'minor green' 'minor remset9-producer' 'minor remset9-consumer' 'minor rearm9' 'minor restored' &
p2=$!
run_group 32-39 'closure green' 'closure final9-field' 'closure restored' 'array-final green' 'array-final array9-partial' 'array-final restored' 'struct green' 'struct-final green' 'registration green' &
p3=$!
wait "$p1"; wait "$p2"; wait "$p3"
