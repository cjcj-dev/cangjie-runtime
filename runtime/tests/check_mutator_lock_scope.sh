#!/usr/bin/env bash
set -euo pipefail
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$script_dir/../.." && pwd)

check_scope() {
    local file=$1 function=$2 label=$3 unlock prerun
    read -r unlock prerun < <(awk -v function_name="$function" '
        index($0, function_name "(") { active = 1 }
        active && /MutatorManagementRUnlock\(\)/ { unlock = NR }
        active && /MRT_PreRunManagedCode\(/ { prerun = NR }
        active && /^}/ { print unlock + 0, prerun + 0; exit }
    ' "$repo/$file")
    if ((unlock == 0 || prerun == 0 || unlock >= prerun)); then
        echo "$label result=FAIL unlock=$unlock prerun=$prerun"
        return 1
    fi
    echo "$label result=PASS unlock=$unlock prerun=$prerun"
}

check_scope runtime/src/Mutator/MutatorManager.cpp MutatorManager::CreateRuntimeMutator RUNTIME_PRERUN_SCOPE
check_scope runtime/src/CjScheduler.cpp NewFinalizerCJThread FINALIZER_PRERUN_SCOPE
