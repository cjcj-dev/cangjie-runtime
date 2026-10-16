#!/usr/bin/env bash
# Build both supported shapes via separate recipe kinds and independent dirs.
set -euo pipefail
ulimit -c 0
cd "$SB_SOURCE"
[[ "$SB_KIND" = runtime-default || "$SB_KIND" = runtime-testable ]]
[[ "$SB_OPTIMIZATION" = Release ]]
export CCACHE_DIR=/root/.ccache CCACHE_BASEDIR="$SB_SOURCE" CCACHE_NOHASHDIR=1
maps="-ffile-prefix-map=$SB_SOURCE=/usr/src/cangjie-runtime -fdebug-prefix-map=$SB_SOURCE=/usr/src/cangjie-runtime -fmacro-prefix-map=$SB_SOURCE=/usr/src/cangjie-runtime"
# The out-of-tree build directory also appears in DWARF compilation paths.
maps+=" -ffile-prefix-map=$TMPDIR=/usr/src/cangjie-runtime-build -fdebug-prefix-map=$TMPDIR=/usr/src/cangjie-runtime-build -fmacro-prefix-map=$TMPDIR=/usr/src/cangjie-runtime-build"
export CFLAGS="$maps" CXXFLAGS="$maps" ASMFLAGS="$maps" GC_UNIT_GATE_SKIP=1
ccache -M 50G
extra=()
python3 - > "$TMPDIR/cmake-args.nul" <<'PY'
import json,os,sys
from pathlib import Path
p=json.loads(Path(os.environ['SB_PARAMETERS']).read_text())
if set(p)-{'cmake_args'}: raise ValueError('unknown runtime parameters')
for arg in p.get('cmake_args',[]):
 if not isinstance(arg,str) or '\0' in arg: raise ValueError('invalid cmake argument')
 sys.stdout.buffer.write(arg.encode()+b'\0')
PY
while IFS= read -r -d '' arg; do extra+=("$arg"); done < "$TMPDIR/cmake-args.nul"
testable=OFF
[[ "$SB_KIND" != runtime-testable ]] || testable=ON
cmake -S runtime -B "$TMPDIR/build" "${extra[@]}" -DCMAKE_BUILD_TYPE=Release \
 -DCOPYGC_FLAG=1 -DDOPRA_FLAG=1 -DRUNTIME_TRACE_FLAG=1 -DCJ_SDK_VERSION=0.0.1 -DDISABLE_VERSION_CHECK=1 \
 -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_AR_PATH=ar \
 -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_ASM_COMPILER_LAUNCHER=ccache \
 -DMRT_GC_UNIT_TESTS=OFF -DMRT_TESTABLE_INTERNALS="$testable" -DCMAKE_INSTALL_PREFIX="$TMPDIR/install"
cmake --build "$TMPDIR/build"
lib=$(find "$TMPDIR/build" -type f -name libcangjie-runtime.so -print)
[[ "$lib" != *$'\n'* && -f "$lib" ]]
lib=$(cd "$(dirname "$lib")" && pwd)
mkdir -p "$SB_OUTPUT/lib"
cp "$lib/libcangjie-runtime.so" "$lib/libboundscheck.so" "$SB_OUTPUT/lib/"
sha256sum "$SB_OUTPUT/lib/"*.so > "$SB_OUTPUT/linked.sha256"
