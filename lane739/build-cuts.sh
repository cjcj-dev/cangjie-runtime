#!/bin/bash
set -u
ulimit -c 0
ROOT=/root/cj_build/cangjie_runtime_wt/sym_cangjie_runtime_739_implement_r5747765432
LANE=sym_cangjie_runtime_739_implement_r5747765432
# Initial concurrent suites measured load >300 on 192 CPUs; cap to two
# independent fault worktrees (four product configurations) per build wave.
for group in 'consumer notify' 'restart shutdown' 'young reset'; do
  for arm in $group; do
    (start=$SECONDS
     /root/cj_build/ops/bin/kkk2_build_two.sh "$ROOT/lane739/cut-$arm" "$LANE-cut-$arm" > "$ROOT/lane739/build-$arm.log" 2>&1
     echo $? > "$ROOT/lane739/build-$arm.rc"
     echo $((SECONDS-start)) > "$ROOT/lane739/build-$arm.wall") &
  done
  wait
done
