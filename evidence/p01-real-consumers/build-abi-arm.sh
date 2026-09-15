#!/bin/bash
set -euo pipefail
ulimit -c 0
arm=${1:?}; lane=/root/sym_cangjie_runtime_608_implement_r5683164869
old=/root/sym_cangjie_runtime_608_implement_r5676392826/llvm
root=${ABI_ROOT:-$lane/abi-$arm}
mkdir -p "$root/evidence"
start=$SECONDS
trap 'rc=$?; echo "$rc" > "$root/evidence/build.rc"; echo "wall=$((SECONDS-start))" > "$root/evidence/build.wall"; uptime > "$root/evidence/uptime-after.txt"' EXIT
uptime > "$root/evidence/uptime-before.txt"
# Reuse the preserved source, configuration, objects and SDK by copying them.
# Never modify the old tree or its evidence.
for d in llvm-src llvm-build target host; do cp -a --reflink=auto "$old/$d" "$root/$d"; done
mkdir "$root/stdlib"
tar -C "$old/stdlib" --exclude=./build -cf - . | tar -C "$root/stdlib" -xf -
python3 - "$root" "$old" <<'PY'
import pathlib,sys
root=pathlib.Path(sys.argv[1]); old=sys.argv[2]
# CMake/Ninja relocation is limited to generated text. Compiler objects are reused.
for p in (root/'llvm-build').rglob('*'):
 if p.is_file() and (p.name in ('CMakeCache.txt','build.ninja','rules.ninja') or p.suffix=='.cmake'):
  t=p.read_text(); p.write_text(t.replace(old,str(root)))
PY
tar -xzf "$lane/llvm-candidate-inputs.tar.gz" -C "$root/llvm-src"
python3 "$lane/check-llvm-inputs.py" "$root/llvm-src" "$lane/llvm-input-manifest.json" > "$root/evidence/source-check.json"
if [ "$arm" = cut ]; then (cd "$root/llvm-src" && patch -p1 < "$lane/cut.diff"); fi
cd "$root"
export CCACHE_DIR=/root/.ccache CCACHE_BASEDIR="$root/llvm-src" CCACHE_NOHASHDIR=1 PATH=/usr/lib/ccache:$PATH
maps="-ffile-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-llvm -fdebug-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-llvm -fmacro-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-llvm"
cmake -S llvm-src/llvm -B llvm-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_C_FLAGS="$maps" -DCMAKE_CXX_FLAGS="$maps" -DLLVM_TARGETS_TO_BUILD=X86 -DLLVM_ENABLE_ASSERTIONS=ON -DLLVM_INCLUDE_TESTS=ON -DLLVM_BUILD_LLVM_DYLIB=ON -DLLVM_LINK_LLVM_DYLIB=OFF > evidence/llvm-configure.log 2>&1
ninja -C llvm-build -j"$(nproc)" -l150 llc opt > evidence/llvm-build.log 2>&1
sha256sum llvm-build/bin/llc llvm-build/bin/opt > evidence/llvm.sha256
cp llvm-build/bin/llc llvm-build/bin/opt target/third_party/llvm/bin/
cp "$lane-finalrt/default/build/runtime-staging/lib/x86_64_Release/"{libcangjie-runtime.so,libboundscheck.so} target/runtime/lib/linux_x86_64_cjnative/
for script in build-closure.sh build-shared.sh install-closure.py; do
 sed "s|$old|$root|g" "$old/$script" > "$script"
done
sha256sum target/bin/cjcj-stage1 target/third_party/llvm/bin/llc target/runtime/lib/linux_x86_64_cjnative/{libcangjie-runtime.so,libboundscheck.so} > evidence/std-inputs.sha256
bash build-closure.sh
bash build-shared.sh
[ "$(cat evidence/std-shared.rc)" = 0 ]
python3 install-closure.py
find std-install -type f -print0 | sort -z | xargs -0 sha256sum > evidence/std.sha256
printf 'ABI_ARM=%s BUILD_OK\n' "$arm"
