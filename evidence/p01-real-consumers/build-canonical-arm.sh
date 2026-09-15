#!/bin/bash
set -euo pipefail
ulimit -c 0
arm=${1:?}
R=/root/sym_cangjie_runtime_608_implement_r5683164869
physical=$R/abi-final-$arm
canonical=$R/abi-canonical
mkdir -p "$physical" "$canonical"
# Private to this build process: parallel arms see the same path, backed by
# separate directories. Exiting the namespace releases the bind mount.
unshare -m --propagation private bash -c '
  set -euo pipefail
  mount --bind "$1" "$2"
  export ABI_ROOT="$2"
  bash "$3" "$4"
' _ "$physical" "$canonical" "$R/build-abi-arm-final.sh" "$arm"
