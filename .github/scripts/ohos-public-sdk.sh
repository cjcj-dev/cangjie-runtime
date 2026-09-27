#!/usr/bin/env bash
# Real public-SDK product build, on an official GitHub runner.
set -euo pipefail
ulimit -c 0
arch=$1
entry=$2
repo=$(pwd)
out="$repo/ohos-evidence"
mkdir -p "$out"
exec > >(tee "$out/run.log") 2>&1
started=$SECONDS
uptime
trap 'rc=$?; echo "RC=$rc wall=$((SECONDS-started)) jobs=$(nproc)"; uptime; exit "$rc"' EXIT
unset OHOS_ROOT OHOS_PUBLIC_SDK
export CMAKE_EXPORT_COMPILE_COMMANDS=ON
export CMAKE_C_COMPILER_LAUNCHER=ccache CMAKE_CXX_COMPILER_LAUNCHER=ccache CMAKE_ASM_COMPILER_LAUNCHER=ccache
export CCACHE_DIR="$RUNNER_TEMP/ohos-ccache" CCACHE_BASEDIR="$repo" CCACHE_NOHASHDIR=1
export CANGJIE_BUILD_JOBS=$(nproc) CMAKE_BUILD_PARALLEL_LEVEL=$(nproc)
ccache -M 50G
cp "$RUNNER_TEMP/ohos-sdk/archive.sha256" "$out/sdk.sha256"
cp "$PUBLIC_NATIVE/oh-uni-package.json" "$out/sdk.json"
git rev-parse HEAD > "$out/source.sha"
if [[ $entry == controls ]]; then
    python3 .github/scripts/ohos-routing-controls.py "$arch"
    exit 0
fi
flag=2
extra=()
if [[ $arch == aarch64 ]]; then
    flag=1
    extra=(-DRUNTIME_FORWARD_PTRAUTH_CFI=1 -DRUNTIME_BACKWARD_PTRAUTH_CFI=1)
fi
if [[ $entry == build.py ]]; then
    (cd runtime && python3 build.py build -t release --target "ohos-$arch" --ohos-public-sdk "$PUBLIC_NATIVE" -v 0.0.1 && python3 build.py install)
    build="$repo/runtime/CMakebuild"
else
    build="$RUNNER_TEMP/runtime-build"
    cmake -S runtime -B "$build" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$RUNNER_TEMP/runtime-install" \
        -DOHOS_FLAG="$flag" -DOHOS_PUBLIC_SDK="$PUBLIC_NATIVE" \
        -DCOPYGC_FLAG=1 -DDOPRA_FLAG=1 -DRUNTIME_TRACE_FLAG=0 \
        -DCJ_SDK_VERSION=0.0.1 -DDISABLE_VERSION_CHECK=1 "${extra[@]}"
    cmake --build "$build" -j"$(nproc)"
    cmake --install "$build"
fi
python3 runtime/tests/test_ohos_public_sdk.py --build "$build" --sdk "$PUBLIC_NATIVE" --arch "$arch" --output "$out/observations.json"
cp "$build/compile_commands.json" "$out/runtime-compile-commands.json"
cp "$build/cjthread-build/compile_commands.json" "$out/cjthread-compile-commands.json"
find "$build" -name link.txt -exec sh -c 'printf "\n%s\n" "$1"; cat "$1"' sh {} \; > "$out/link-commands.txt"
