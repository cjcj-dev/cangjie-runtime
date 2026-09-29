#!/usr/bin/env bash
# Single testable configuration per controlled arm. The release dual-config
# build remains kkk2_build_two.sh; all arm inputs are immutable copies.
set -u
ulimit -c 0
r=/root/sym_cangjie_runtime_1305_implement_r5892797534
head=a1543759f5df04db74f441fefb47cf0e8bfef49a
export CCACHE_DIR=/root/.ccache CCACHE_NOHASHDIR=1 GC_UNIT_GATE_SKIP=1
export PATH=/usr/lib/ccache:$PATH
ccache -M 50G >/dev/null
mkdir -p "$r/arms" "$r/keep"
uptime > "$r/arms/uptime-before.txt"
for arm in candidate restored termination objects arrays bitmap entry; do
  mkdir -p "$r/arms/$arm"
  tar -xzf "$r/cuts-source.tar.gz" -C "$r/arms/$arm"
  if [[ $arm != candidate && $arm != restored ]]; then
    (cd "$r/arms/$arm" && patch -p1 < "$r/cuts/$arm.diff") > "$r/arms/$arm/patch.log" 2>&1 || exit 2
  fi
  (
    start=$SECONDS
    root="$r/arms/$arm"
    export CCACHE_BASEDIR="$root"
    maps="-ffile-prefix-map=$root=/usr/src/cangjie-runtime -fdebug-prefix-map=$root=/usr/src/cangjie-runtime -fmacro-prefix-map=$root=/usr/src/cangjie-runtime"
    export CFLAGS="$maps" CXXFLAGS="$maps" ASMFLAGS="$maps"
    cmake -S "$root/runtime" -B "$root/build" -DCJ_RUNTIME_COMMIT="$head" -DCMAKE_BUILD_TYPE=Release -DCOPYGC_FLAG=1 -DDOPRA_FLAG=1 -DRUNTIME_TRACE_FLAG=1 -DCJ_SDK_VERSION=0.0.1 -DDISABLE_VERSION_CHECK=1 -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_AR_PATH=ar -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_ASM_COMPILER_LAUNCHER=ccache -DMRT_TESTABLE_INTERNALS=ON -DCMAKE_INSTALL_PREFIX="$root/install" > "$root/configure.log" 2>&1
    rc=$?; echo "$rc" > "$root/configure.rc"
    if [[ $rc == 0 ]]; then
      cmake --build "$root/build" -j"$(nproc)" > "$root/build.log" 2>&1
      rc=$?
    fi
    echo "$rc" > "$root/build.rc"
    echo "$((SECONDS-start))" > "$root/wall.txt"
    if [[ $rc == 0 ]]; then
      mkdir -p "$r/keep/$arm"
      cp -a "$root/build/runtime-staging/lib/x86_64_Release/"*.so "$r/keep/$arm/"
      sha256sum "$r/keep/$arm/"*.so > "$root/so.sha256"
      strings "$r/keep/$arm/libcangjie-runtime.so" | /usr/bin/grep -E '^CJRT-(COMMIT|DECLARED):' > "$root/lineage.txt"
      nm --defined-only "$r/keep/$arm/libcangjie-runtime.so" | c++filt | /usr/bin/grep -E 'HeapIterator::(mark_object|object_bitmap|steal|drain_and_steal|object_and_field_iterate|object_iterate)|TaskTerminator::offer_termination' > "$root/product-symbols.txt"
    fi
  ) &
done
wait
uptime > "$r/arms/uptime-after.txt"
for arm in candidate restored termination objects arrays bitmap entry; do
  d="$r/arms/$arm"
  echo "ARM=$arm configure_rc=$(cat "$d/configure.rc") build_rc=$(cat "$d/build.rc") wall=$(cat "$d/wall.txt")"
  if [[ $(cat "$d/build.rc") != 0 ]]; then
    /usr/bin/grep -m 5 -E 'error:|Error [0-9]|undefined reference' "$d/build.log" "$d/configure.log"
  else
    /usr/bin/grep libcangjie-runtime "$d/so.sha256"
  fi
done
