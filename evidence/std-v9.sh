#!/bin/bash
set -euo pipefail
ulimit -c 0
lane=/root/sym_cangjie_runtime_608_implement_r5676392826
root="$lane/llvm"
cd "$root"
start=$SECONDS
trap 'rc=$?; echo "$rc" > evidence/std-v9.rc; echo "$((SECONDS-start))" > evidence/std-v9.wall; uptime > evidence/std-v9-after.txt' EXIT
uptime > evidence/std-v9-before.txt
cp "$lane/frontend-r2/cjcj-stage1-r7" target/bin/cjcj-stage1
ln -sfn cjcj-stage1 target/bin/cjc
ln -sfn cjcj-stage1 target/bin/cjc-frontend
cp llvm-build/bin/llc llvm-build/bin/opt target/third_party/llvm/bin/
cp "${lane}-bp-tests/default/build/runtime-staging/lib/x86_64_Release/libcangjie-runtime.so" "${lane}-bp-tests/default/build/runtime-staging/lib/x86_64_Release/libboundscheck.so" target/runtime/lib/linux_x86_64_cjnative/
sha256sum llvm-build/bin/llc llvm-build/bin/opt target/runtime/lib/linux_x86_64_cjnative/libcangjie-runtime.so target/runtime/lib/linux_x86_64_cjnative/libboundscheck.so > evidence/std-v9-inputs.sha256
# This is this lane's generated std build, containing objects made by the earlier backend.
rm -rf "$root/stdlib/build/build"
bash build-closure.sh
bash build-shared.sh
shared_rc=$(cat evidence/std-shared.rc)
if [[ "$shared_rc" != 0 ]]; then exit "$shared_rc"; fi
python3 install-closure.py
mkdir -p gate-sdk
cp -a --reflink=auto target/. gate-sdk/
cp -a std-install/. gate-sdk/
