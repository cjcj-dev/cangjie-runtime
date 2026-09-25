#!/bin/bash
set -euo pipefail

root="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$root"

if ! command -v xcrun >/dev/null 2>&1; then
  echo "BSD_BACKING_FAIL skip: xcrun missing (BSD backing is not in the Linux SO)" >&2
  exit 2
fi

sdk_path="$(xcrun --sdk macosx --show-sdk-path)"
cxx="$(xcrun --sdk macosx --find clang++)"
bc="runtime/third_party/third_party_bounds_checking_function"
if [[ ! -f "$bc/include/securec.h" ]]; then
  git clone --depth 1 --branch OpenHarmony-v6.0-Release \
    https://gitcode.com/openharmony/third_party_bounds_checking_function \
    "$bc"
fi
test -f "$bc/include/securec.h"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

cj="runtime/src/CJThread/src"
flags=(
  -std=c++14
  -fno-exceptions
  -fno-strict-aliasing
  -ffunction-sections
  -fdata-sections
  -isysroot "$sdk_path"
  -I runtime/src
  -I runtime/src/Heap
  -I runtime/src/Mutator
  -I "$bc/include"
  -I "$cj/runtime/schedule/include"
  -I "$cj/runtime/schedule/include/inner"
  -I "$cj/sync/sema/include"
  -I "$cj/runtime/waitqueue/include"
  -I "$cj/base/log/include"
  -I "$cj/base/mid/include"
  -I "$cj/base/external/include"
  -I "$cj/runtime/util/list/include"
  -I "$cj/runtime/util/basetime/include"
  -D_LARGEFILE_SOURCE
  -D_FILE_OFFSET_BITS=64
  -DCANGJIE
  -DMRT_MACOS
  -D_XOPEN_SOURCE=600
  -D_DARWIN_C_SOURCE
)

cxx_tus=(
  runtime/src/Heap/z/zPhysicalMemoryBacking_bsd.cpp
  runtime/src/Heap/z/zInitialize.cpp
  runtime/src/Heap/z/zLargePages.cpp
  runtime/src/Heap/z/zErrno.cpp
  runtime/src/Heap/z/zAddress.cpp
  runtime/src/Base/Log.cpp
  runtime/src/Base/CString.cpp
  runtime/src/Base/TimeUtils.cpp
  runtime/src/Base/SysCall.cpp
  runtime/src/os/Linux/Path.cpp
  runtime/src/CjScheduler.cpp
  runtime/tests/bsd/backing_fail_main.cpp
)

objs=()
for tu in "${cxx_tus[@]}"; do
  obj="$work/$(basename "$tu" .cpp).o"
  echo "compile $tu"
  "$cxx" "${flags[@]}" -c "$tu" -o "$obj"
  objs+=("$obj")
done

shopt -s nullglob
for tu in "$bc"/src/*.c; do
  base="$(basename "$tu")"
  obj="$work/${base%.c}.o"
  echo "compile $base"
  xcrun --sdk macosx clang "${flags[@]}" -c "$tu" -o "$obj"
  objs+=("$obj")
done

"$cxx" -isysroot "$sdk_path" -Wl,-dead_strip "${objs[@]}" -o "$work/backing_fail"

run_case() {
  local name="$1"
  local kind="$2"
  local needle="$3"
  local err="$work/${name}.err"
  set +e
  "$work/backing_fail" "$name" >"$work/${name}.out" 2>"$err"
  local rc=$?
  set -e
  echo "BSD_BACKING_FAIL case=$name rc=$rc"
  cat "$err" || true
  if [[ "$kind" == "ok" ]]; then
    [[ "$rc" -eq 0 ]]
    grep -F "$needle" "$err" >/dev/null
  else
    [[ "$rc" -eq 134 ]]
    grep -F "$needle" "$err" >/dev/null
  fi
}

run_case ctor_ok ok "CASE ctor_ok ok"
run_case ctor_fail ok "Failed to reserve address space for backing memory"
grep -F "CASE ctor_fail ok" "$work/ctor_fail.err" >/dev/null
run_case map_ok ok "CASE map_ok returned"
run_case map_fail abort "Failed to remap memory"
run_case unmap_ok ok "CASE unmap_ok returned"
run_case unmap_fail abort "Failed to map memory"
echo "BSD_BACKING_FAIL pass arch=$(uname -m)"
