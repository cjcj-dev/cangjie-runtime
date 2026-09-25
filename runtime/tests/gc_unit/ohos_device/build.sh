#!/usr/bin/env bash
# Build the actual OHOS product, without MRT_GC_UNIT_OHOS_HOST or host stubs.
set -euo pipefail
ulimit -c 0
src=$(cd "$(dirname "$0")/../../.." && pwd)
: "${OHOS_PUBLIC_SDK:?native SDK directory}"
: "${OHOS_DEVICE_BUILD:?private build directory}"
mkdir -p "$OHOS_DEVICE_BUILD"
build=$(cd "$OHOS_DEVICE_BUILD" && pwd)
export CCACHE_DIR=${CCACHE_DIR:-/root/.ccache}
export CCACHE_BASEDIR=${CCACHE_BASEDIR:-$(dirname "$src")}
export CCACHE_NOHASHDIR=1
maps="-ffile-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fdebug-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fmacro-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime"
export CFLAGS="${CFLAGS:-} $maps" CXXFLAGS="${CXXFLAGS:-} $maps" ASMFLAGS="${ASMFLAGS:-} $maps"
ccache -M 50G
cmake -S "$src" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DOHOS_FLAG=1 -DOHOS_PUBLIC_SDK="$OHOS_PUBLIC_SDK" \
    -DRUNTIME_FORWARD_PTRAUTH_CFI=1 -DRUNTIME_BACKWARD_PTRAUTH_CFI=1 \
    -DCOPYGC_FLAG=1 -DDOPRA_FLAG=1 -DRUNTIME_TRACE_FLAG=0 \
    -DCJ_SDK_VERSION=0.0.1 -DDISABLE_VERSION_CHECK=1 \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DCMAKE_ASM_COMPILER_LAUNCHER=ccache \
    -DMRT_TESTABLE_INTERNALS=OFF -DMRT_GC_UNIT_OHOS_HOST=OFF
cmake --build "$build" --target cangjie-runtime
sha256sum "$build/runtime-staging/lib/aarch64_Release/"*.so > "$build/linked.sha256"
