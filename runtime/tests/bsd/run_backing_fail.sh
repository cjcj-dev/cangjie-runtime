#!/bin/bash
set -euo pipefail

root="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$root"

if ! command -v xcrun >/dev/null 2>&1; then
  echo "BSD_BACKING_FAIL skip: xcrun missing (BSD backing is not in the Linux SO)" >&2
  exit 2
fi

sdk_name="${BSD_BACKING_SDK:-macosx}"
sdk_path="$(xcrun --sdk "$sdk_name" --show-sdk-path)"
cxx="$(xcrun --sdk "$sdk_name" --find clang++)"
target_arch="${BSD_BACKING_ARCH:-$(uname -m)}"
# When BSD_BACKING_SIM_UDID is set, cases run inside that booted iOS
# simulator via simctl spawn instead of directly on the host.
run_prefix=()
if [[ -n "${BSD_BACKING_SIM_UDID:-}" ]]; then
  run_prefix=(xcrun simctl spawn "$BSD_BACKING_SIM_UDID")
fi
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
case "$(uname -m)" in
  arm64) gas="arm/arm64" ;;
  x86_64) gas="x86/x86_64" ;;
  *) echo "BSD_BACKING_FAIL unknown arch $(uname -m)" >&2; exit 2 ;;
esac
flags=(
  -std=c++14
  -fno-exceptions
  -fno-strict-aliasing
  -ffunction-sections
  -fdata-sections
  -arch "$target_arch"
  -isysroot "$sdk_path"
  -I runtime/src
  -I runtime/include
  -I runtime/src/Heap
  -I runtime/src/Heap/z/os/bsd
  -I runtime/src/Mutator
  -I "$bc/include"
  -I "$cj/runtime/schedule/include"
  -I "$cj/runtime/schedule/include/inner"
  -I "$cj/runtime/schedule/include/inner/gas/$gas"
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
  # LocalDeque.h calls MRT_PRCTL, which SysCall.h defines only for Linux.
  # The driver never calls DequeMapping; this lets the real zInitialize.cpp
  # and CjScheduler.cpp compile. It is not a product definition.
  '-DMRT_PRCTL(base,size,tag)=((void)0)'
)

cxx_tus=(
  runtime/src/Heap/z/zPhysicalMemoryBacking_bsd.cpp
  runtime/src/Heap/z/zInitialize.cpp
  runtime/src/Heap/z/zLargePages.cpp
  runtime/src/Heap/z/zErrno.cpp
  runtime/src/Heap/z/zAddress.cpp
  runtime/src/Base/Log.cpp
  runtime/src/Base/LogFile.cpp
  runtime/src/Base/CString.cpp
  runtime/src/Base/TimeUtils.cpp
  runtime/src/Base/SysCall.cpp
  runtime/src/os/Linux/Path.cpp
  runtime/src/CjScheduler.cpp
  runtime/tests/bsd/backing_fail_main.cpp
)

objs=()
compile_fail=0
for tu in "${cxx_tus[@]}"; do
  obj="$work/$(basename "$tu" .cpp).o"
  echo "compile $tu"
  if "$cxx" "${flags[@]}" -c "$tu" -o "$obj"; then
    objs+=("$obj")
  else
    compile_fail=1
  fi
done
if [[ "$compile_fail" -ne 0 ]]; then
  echo "BSD_BACKING_FAIL compile failed" >&2
  exit 1
fi

cflags=()
for flag in "${flags[@]}"; do
  case "$flag" in
    -std=c++14|-fno-exceptions) continue ;;
  esac
  cflags+=("$flag")
done
shopt -s nullglob
for tu in "$bc"/src/*.c; do
  base="$(basename "$tu")"
  obj="$work/${base%.c}.o"
  echo "compile $base"
  if xcrun --sdk "$sdk_name" clang "${cflags[@]}" -x c -std=c11 -c "$tu" -o "$obj"; then
    objs+=("$obj")
  else
    compile_fail=1
  fi
done
if [[ "$compile_fail" -ne 0 ]]; then
  echo "BSD_BACKING_FAIL compile failed" >&2
  exit 1
fi

"$cxx" -arch "$target_arch" -isysroot "$sdk_path" -Wl,-dead_strip "${objs[@]}" -o "$work/backing_fail"

run_case() {
  local name="$1"
  local kind="$2"
  local needle="$3"
  local err="$work/${name}.err"
  set +e
  "${run_prefix[@]}" "$work/backing_fail" "$name" >"$work/${name}.out" 2>"$err"
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

cases="${BSD_BACKING_CASES:-ctor_ok ctor_fail map_ok map_fail unmap_ok unmap_fail}"
for name in $cases; do
  case "$name" in
    ctor_ok) run_case ctor_ok ok "CASE ctor_ok ok" ;;
    ctor_fail)
      run_case ctor_fail ok "Failed to reserve address space for backing memory"
      grep -F "CASE ctor_fail ok" "$work/ctor_fail.err" >/dev/null
      ;;
    map_ok) run_case map_ok ok "CASE map_ok returned" ;;
    map_fail) run_case map_fail abort "Failed to remap memory" ;;
    unmap_ok) run_case unmap_ok ok "CASE unmap_ok returned" ;;
    unmap_fail) run_case unmap_fail abort "Failed to map memory" ;;
    *) echo "BSD_BACKING_FAIL unknown case $name" >&2; exit 2 ;;
  esac
done
echo "BSD_BACKING_FAIL pass sdk=$sdk_name arch=$target_arch cases=[$cases]"
