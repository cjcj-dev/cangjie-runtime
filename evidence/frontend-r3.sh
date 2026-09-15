#!/bin/bash
set -euo pipefail
ulimit -c 0
root=/root/sym_cangjie_runtime_608_implement_r5676392826/frontend-r2
source_sha=7bc39bbff1a2b7fc9b58049cd63fef7f633ebfc4
cd "$root"
started=$SECONDS
trap 'rc=$?; echo "$rc" > evidence/frontend-r3.rc; echo "$((SECONDS-started))" > evidence/frontend-r3.wall; uptime > evidence/frontend-r3-after.txt' EXIT
uptime > evidence/frontend-r3-before.txt
test "$(cat evidence/frontend-build.rc)" = 0
git -C source fetch "$root/cjcj-r3.bundle" sym/608-implement-r5676392826-cjcj
git -C source checkout --detach "$source_sha"
test -z "$(git -C source status --porcelain)"
git -C source diff --exit-code 4c155ca4d80e76d843a89953ba4139f14f9cc20c "$source_sha" -- runtime_shim/
cc -std=c11 -O2 -fPIC -D_POSIX_C_SOURCE=200809L "-DCJCJ_COMMIT=\"$source_sha\"" -c source/runtime_shim/cjc_runtime_config.c -o source/runtime_shim/cjc_runtime_config.o
rm host/bin/cjc
cat > host/bin/cjc <<'PY'
#!/usr/bin/env python3
import json, os, pathlib, sys
here = pathlib.Path(__file__).resolve().parent
before = sys.argv[1:]
after = ['-O1' if arg == '-O2' else arg for arg in before]
(here.parent.parent/'evidence'/('r3-seed-command-'+str(os.getpid())+'.json')).write_text(json.dumps({'before':before,'after':after})+'\n')
os.execv(str(here/'cjc-official'),[str(here/'cjc-official'),*after])
PY
chmod +x host/bin/cjc
export CANGJIE_HOME="$root/host"
export LD_LIBRARY_PATH="$CANGJIE_HOME/runtime/lib/linux_x86_64_cjnative:$CANGJIE_HOME/third_party/llvm/lib:$CANGJIE_HOME/tools/lib"
export PATH="$CANGJIE_HOME/bin:$CANGJIE_HOME/tools/bin:$CANGJIE_HOME/third_party/llvm/bin:$PATH"
export cjHeapSize=24GB
(cd source && cjpm build -j "$(nproc)")
git -C source status --porcelain > evidence/frontend-r3-source-status.txt
test ! -s evidence/frontend-r3-source-status.txt
for name in 'cjcj::cjc' 'cjc@cjcj'; do
  if [[ -f source/target/release/bin/$name ]]; then install -m755 "source/target/release/bin/$name" cjcj-stage1-r3; fi
done
sha256sum cjcj-stage1-r3 cjcj-r3.bundle source/runtime_shim/cjc_runtime_config.o host/bin/cjc-official > evidence/frontend-r3.sha256
strings cjcj-stage1-r3 | /usr/bin/grep 'CJCJ-COMMIT:' > evidence/frontend-r3.stamp
test "$(cat evidence/frontend-r3.stamp)" = "CJCJ-COMMIT:$source_sha"
install -m755 cjcj-stage1-r3 host/bin/cjcj-stage1
rm host/bin/cjc
ln -s cjcj-stage1 host/bin/cjc
