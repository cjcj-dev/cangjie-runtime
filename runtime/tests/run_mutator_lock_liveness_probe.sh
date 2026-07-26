#!/usr/bin/env bash
# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(git -C "$script_dir" rev-parse --show-toplevel)
pre_ref=${1:-f56e60bfb05121f138f39dec46d7e0b38eb3165a}
post_ref=${2:-HEAD}
rounds=${3:-20}
cpuset=${MUTATOR_LOCK_CPUSET:-0-3}
harness="$repo/runtime/tests/mutator_lock_liveness_harness.cpp"
probe_tmp=$(mktemp -d /tmp/armlivelock-probe.XXXXXX)

cleanup()
{
    unlink "$probe_tmp/pre" "$probe_tmp/post" 2>/dev/null || true
    rmdir "$probe_tmp"
}
trap cleanup EXIT

postcheck_count()
{
    git -C "$repo" show "$1:runtime/src/Mutator/MutatorManager.h" |
        awk '
            /bool TryAcquireMutatorManagementRLock\(\)/ { active = 1 }
            active && /mgmtWritersWaiting\.load/ { count++ }
            active && /void AnnounceMgmtWriterPending\(\)/ { print count + 0; exit }
        '
}

runtime_mutator_scope()
{
    git -C "$repo" show "$1:runtime/src/Mutator/MutatorManager.cpp" |
        awk '
            /Mutator\* MutatorManager::CreateRuntimeMutator\(/ { active = 1 }
            active && /MutatorManagementRUnlock\(\)/ { unlock = NR }
            active && /MRT_PreRunManagedCode\(/ { prerun = NR }
            active && /return mutator;/ { print unlock, prerun; exit }
        '
}

finalizer_mutator_scope()
{
    git -C "$repo" show "$1:runtime/src/CjScheduler.cpp" |
        awk '
            /CJThreadHandle NewFinalizerCJThread\(/ { active = 1 }
            active && /MutatorManagementRUnlock\(\)/ { unlock = NR }
            active && /MRT_PreRunManagedCode\(/ { prerun = NR }
            active && /return cjthread;/ { print unlock, prerun; exit }
        '
}

git -C "$repo" cat-file -e "$pre_ref^{commit}"
git -C "$repo" cat-file -e "$post_ref^{commit}"
[[ "$rounds" =~ ^[1-9][0-9]*$ ]] || { echo "invalid rounds: $rounds"; exit 2; }
[[ "$cpuset" =~ ^[0-9]+(-[0-9]+)?$ ]] || { echo "invalid MUTATOR_LOCK_CPUSET: $cpuset"; exit 2; }

pre_count=$(postcheck_count "$pre_ref")
post_count=$(postcheck_count "$post_ref")
read -r pre_unlock pre_prerun <<< "$(runtime_mutator_scope "$pre_ref")"
read -r post_unlock post_prerun <<< "$(runtime_mutator_scope "$post_ref")"
read -r pre_finalizer_unlock pre_finalizer_prerun <<< "$(finalizer_mutator_scope "$pre_ref")"
read -r post_finalizer_unlock post_finalizer_prerun <<< "$(finalizer_mutator_scope "$post_ref")"

if [[ "$pre_count" != 1 || "$post_count" != 2 ]]; then
    echo "REF_STRUCTURE result=FAIL pre_ref=$pre_ref pre_checks=$pre_count post_ref=$post_ref post_checks=$post_count"
    exit 1
fi
if ((pre_unlock <= pre_prerun || post_unlock >= post_prerun)); then
    echo "RUNTIME_PRERUN_SCOPE result=FAIL pre_unlock=$pre_unlock pre_prerun=$pre_prerun post_unlock=$post_unlock post_prerun=$post_prerun"
    exit 1
fi
if ((pre_finalizer_unlock <= pre_finalizer_prerun || post_finalizer_unlock >= post_finalizer_prerun)); then
    echo "FINALIZER_PRERUN_SCOPE result=FAIL pre_unlock=$pre_finalizer_unlock pre_prerun=$pre_finalizer_prerun post_unlock=$post_finalizer_unlock post_prerun=$post_finalizer_prerun"
    exit 1
fi

ulimit -v 1048576
export cjHeapSize=512MB
g++ -std=c++17 -O2 -pthread -DMUTATOR_LOCK_POSTCHECK=0 "$harness" -o "$probe_tmp/pre"
g++ -std=c++17 -O2 -pthread -DMUTATOR_LOCK_POSTCHECK=1 "$harness" -o "$probe_tmp/post"

echo "REF_STRUCTURE result=PASS pre_ref=$(git -C "$repo" rev-parse "$pre_ref") pre_checks=$pre_count post_ref=$(git -C "$repo" rev-parse "$post_ref") post_checks=$post_count"
echo "RUNTIME_PRERUN_SCOPE result=PASS pre_unlock=$pre_unlock pre_prerun=$pre_prerun post_unlock=$post_unlock post_prerun=$post_prerun"
echo "FINALIZER_PRERUN_SCOPE result=PASS pre_unlock=$pre_finalizer_unlock pre_prerun=$pre_finalizer_prerun post_unlock=$post_finalizer_unlock post_prerun=$post_finalizer_prerun"
for ((round = 1; round <= rounds; ++round)); do
    echo "PROBE_ROUND state=pre round=$round/$rounds cpuset=$cpuset"
    timeout 20s taskset -c "$cpuset" "$probe_tmp/pre"
done
for ((round = 1; round <= rounds; ++round)); do
    echo "PROBE_ROUND state=post round=$round/$rounds cpuset=$cpuset"
    timeout 20s taskset -c "$cpuset" "$probe_tmp/post"
done
echo "MUTATOR_LOCK_PROBE pre_ref=$(git -C "$repo" rev-parse "$pre_ref") post_ref=$(git -C "$repo" rev-parse "$post_ref") rounds=$rounds cpuset=$cpuset result=PASS"
