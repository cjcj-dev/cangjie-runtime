#!/usr/bin/env bash
ulimit -c 0
root=/root/sym_cangjie_runtime_1319_implement_r5899659797
head=b645992f7cbf11e985529f722cff62e72444f3c0
set -o pipefail
mkdir -p "$root/keep/green" "$root/keep/default" "$root/keep/elf"
cp "$root/testable/build/runtime-staging/lib/x86_64_Release/"{libcangjie-runtime,libboundscheck}.so "$root/keep/green/"
cp "$root/default/build/runtime-staging/lib/x86_64_Release/"{libcangjie-runtime,libboundscheck}.so "$root/keep/default/"
sha256sum "$root/keep/green/"*.so > "$root/keep/green.sha256"
sha256sum "$root/keep/default/"*.so > "$root/keep/default.sha256"
tar -xzf "$root/cuts.tar.gz" -C "$root"
uptime > "$root/cuts-uptime-before.txt"
export CCACHE_DIR=/root/.ccache CCACHE_NOHASHDIR=1 GC_UNIT_GATE_SKIP=1
for arm in head clear ordinary restored; do
 (
  dir="$root/cut-$arm"
  mkdir -p "$dir" "$root/keep/$arm"
  tar -xzf "$root/source.tar.gz" -C "$dir"
  if [ "$arm" != restored ]; then (cd "$dir" && patch -p1 < "$root/cuts/$arm.diff") > "$dir/patch.log" 2>&1 || exit 2; fi
  export CCACHE_BASEDIR="$dir"
  maps="-ffile-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fdebug-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fmacro-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime"
  export CFLAGS="$maps" CXXFLAGS="$maps" ASMFLAGS="$maps"
  start=$SECONDS
  cmake -S "$dir/runtime" -B "$dir/build" -DCJ_RUNTIME_COMMIT="$head" -DCMAKE_BUILD_TYPE=Release -DCOPYGC_FLAG=1 -DDOPRA_FLAG=1 -DRUNTIME_TRACE_FLAG=1 -DCJ_SDK_VERSION=0.0.1 -DDISABLE_VERSION_CHECK=1 -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_AR_PATH=ar -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_ASM_COMPILER_LAUNCHER=ccache -DMRT_TESTABLE_INTERNALS=ON -DCMAKE_INSTALL_PREFIX="$root/testable/install" > "$dir/configure.log" 2>&1
  rc=$?; echo "$rc" > "$dir/configure.rc"
  if [ "$rc" = 0 ]; then cmake --build "$dir/build" -j"$(nproc)" > "$dir/build.log" 2>&1; rc=$?; fi
  echo "$rc" > "$dir/build.rc"
  if [ "$rc" = 0 ]; then
   cp "$dir/build/runtime-staging/lib/x86_64_Release/"{libcangjie-runtime,libboundscheck}.so "$root/keep/$arm/"
   sha256sum "$root/keep/$arm/"*.so > "$root/keep/$arm.sha256"
  fi
  echo "wall=$((SECONDS-start))" > "$dir/wall.txt"
  # Keep only build logs and retained products once captured.
  rm -rf "$dir/build" "$dir/runtime" "$dir/AGENTS.md"
 ) &
done
wait
uptime > "$root/cuts-uptime-after.txt"
for arm in head clear ordinary restored; do
 echo "$arm configure=$(cat "$root/cut-$arm/configure.rc") build=$(cat "$root/cut-$arm/build.rc") $(cat "$root/cut-$arm/wall.txt")"
 /usr/bin/grep -m 5 -E 'error:|undefined reference' "$root/cut-$arm/build.log" 2>/dev/null | cut -c1-200
done

exit 0
