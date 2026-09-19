#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
LANE=/root/sym_cangjie_runtime_723_implement_r5741705087
SDK=/root/cj_build/gate_hosts/nightly-1.3.0-alpha.20260904010027
export CANGJIE_HOME=$SDK
export PATH=$SDK/bin:$SDK/tools/bin:$SDK/third_party/llvm/bin:/usr/lib/ccache:/usr/bin:/bin
export LD_LIBRARY_PATH=$SDK/runtime/lib/linux_x86_64_cjnative:$SDK/third_party/llvm/lib:$SDK/tools/lib
export CANGJIE_BUILD_JOBS=$(nproc) CMAKE_BUILD_PARALLEL_LEVEL=$(nproc) cjHeapSize=32GB
export CCACHE_DIR=/root/.ccache CCACHE_NOHASHDIR=1
cd "$LANE/source"
tar -xf "$LANE/source.tar"
rm "$LANE/source.tar"
printf '%s\n' 35da7be2434ad72348ed27e8a0bf599ec4e91524 > "$LANE/source.sha"
sed -i 's/compile-option = "-O2"/compile-option = "-O1"/' cjpm.toml
mkdir -p runtime_shim
cp /root/sym_cjcj_56_implement_r5740526386/cjcj-src/runtime_shim/*.o runtime_shim/
sha256sum "$SDK/bin/cjc" "$SDK/runtime/lib/linux_x86_64_cjnative/"*.so runtime_shim/*.o > "$LANE/build-inputs.sha256"
uptime > "$LANE/build.uptime.before"
start=$SECONDS
set +e
"$SDK/tools/bin/cjpm" build > "$LANE/build.log" 2>&1
rc=$?
set -e
printf '%s\n' "$rc" > "$LANE/build.rc"
printf 'wall=%s jobs=%s\n' "$((SECONDS-start))" "$(nproc)" > "$LANE/build.wall"
uptime > "$LANE/build.uptime.after"
if [[ $rc == 0 ]]; then sha256sum target/release/bin/cjcj::cjc > "$LANE/stage1.sha256"; fi
exit "$rc"
