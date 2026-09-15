#!/bin/bash
# Rebuild the post-static-barrier LLVM backend and std in an isolated kkk2 tree.
# Reference: zBarrierSet.inline.hpp store_barrier, all reference stores use barriers.
set -euo pipefail
ulimit -c 0
root=${1:?isolated root}
old=/root/sym_cangjie_runtime_560b_sym_cangjie_runtime_564_implement_r5664061027
cd "$root"
mkdir -p evidence llvm-src
start=$SECONDS
trap 'rc=$?; echo "$rc" > evidence/toolchain-build.rc; echo "wall=$((SECONDS-start))" > evidence/toolchain-build.wall; uptime > evidence/build-after.txt' EXIT
uptime > evidence/build-before.txt
tar -xzf llvm-source.tar.gz -C llvm-src
export CCACHE_DIR=/root/.ccache CCACHE_BASEDIR="$root/llvm-src" CCACHE_NOHASHDIR=1
export PATH=/usr/lib/ccache:$PATH
maps="-ffile-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-llvm -fdebug-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-llvm -fmacro-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-llvm"
cmake -S llvm-src/llvm -B llvm-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_C_FLAGS="$maps" -DCMAKE_CXX_FLAGS="$maps" -DLLVM_TARGETS_TO_BUILD=X86 -DLLVM_ENABLE_ASSERTIONS=ON -DLLVM_INCLUDE_TESTS=ON -DLLVM_BUILD_LLVM_DYLIB=ON -DLLVM_LINK_LLVM_DYLIB=OFF > evidence/llvm-configure.log 2>&1
ninja -C llvm-build -j"$(nproc)" -l150 llc opt FileCheck llvm-dis > evidence/llvm-build.log 2>&1
sha256sum llvm-build/bin/{llc,opt} > evidence/llvm.sha256
cp -a --reflink=auto "$old/target" target
cp -a --reflink=auto "$old/host" host
rm target/third_party/llvm/bin/llc target/third_party/llvm/bin/opt
cp llvm-build/bin/{llc,opt} target/third_party/llvm/bin/
mkdir -p stdlib
tar -xzf "$old/stdlib-source.tar.gz" -C stdlib --strip-components=1
# Reuse the verified build recipe, with only its isolated root substituted.
for script in build-closure.sh build-shared.sh install-closure.py; do
  sed "s|$old|$root|g" "$old/$script" > "$script"
done
# build-shared.sh has no `set -e`: its own exit status is that of the final `echo`, so
# a failed shared build returns 0 here. Its receipt evidence/std-shared.rc is the build's
# real exit code; stop on it before install-closure.py, which would otherwise be the first
# thing to notice a missing SO.
step_rc() { # step_rc <what> <receipt> <log>
  local rc
  rc=$(cat "$2" 2>/dev/null || true)
  if [[ ! $rc =~ ^[0-9]+$ ]]; then
    echo "$1: receipt $2 missing or not an integer ('$rc'); see $3" >&2
    return 70
  fi
  if [[ $rc != 0 ]]; then
    echo "$1 failed rc=$rc; see $3" >&2
    return "$rc"
  fi
}
bash build-closure.sh
bash build-shared.sh
step_rc "std shared build" evidence/std-shared.rc evidence/std-shared.log
python3 install-closure.py
cp -a --reflink=auto target gate-sdk
cp -a std-install/. gate-sdk/
echo 23e45a2e9dfbfc4a708d99bc7895fbc1d8ffcbaf > evidence/llvm-main.sha
