#!/usr/bin/env bash
# Build an O1 seed without editing the source checkout. Run on kkk2 only.
set -euo pipefail
ulimit -c 0
root=${1:?work directory}
source_sha=${2:?cjcj commit}
host=${3:?official host SDK}
tuple=${4:?verified LLVM tuple}
sdk_builder=${5:?sdk_build.sh}
cd "$root"
mkdir -p evidence tmp
export TMPDIR="$root/tmp"
exec > >(tee evidence/frontend-build.log) 2>&1
started=$SECONDS
trap 'rc=$?; echo "$rc" > evidence/frontend-build.rc; echo "wall=$((SECONDS-started))"; uptime > evidence/uptime-after.txt' EXIT
uptime > evidence/uptime-before.txt
git clone cjcj.bundle source
git -C source checkout --detach "$source_sha"
test -z "$(git -C source status --porcelain)"
git -C source rev-parse HEAD > evidence/source.sha
git -C source ls-files -s > evidence/source-index.txt
(cd "$tuple" && sha256sum --strict -c SHA256SUMS) > evidence/tuple-check.txt
# The pinned tuple's shim source must match the clean frontend checkout.
recipe=$(sed -n 's/^RECIPE_CJCJ_SHA=//p' "$tuple/MANIFEST")
git -C source diff --exit-code "$recipe" "$source_sha" -- runtime_shim/cjselfhost_llvmshim.cpp
bash "$sdk_builder" --from "$host" --to "$root/host" --host
cp "$tuple/fixed-llc/cjselfhost_llvmshim.o" source/runtime_shim/
cc -std=c11 -O2 -fPIC -D_POSIX_C_SOURCE=200809L \
  "-DCJCJ_COMMIT=\"$source_sha\"" -c source/runtime_shim/cjc_runtime_config.c \
  -o source/runtime_shim/cjc_runtime_config.o
sha256sum source/runtime_shim/*.o > evidence/shim.sha256
mv host/bin/cjc host/bin/cjc-official
cat > host/bin/cjc <<'WRAPPER'
#!/usr/bin/env python3
import json, os, pathlib, sys
here = pathlib.Path(__file__).resolve().parent
before = sys.argv[1:]
after = ['-O1' if arg == '-O2' else arg for arg in before]
log = here.parent.parent / 'evidence' / ('seed-command-' + str(os.getpid()) + '.json')
log.write_text(json.dumps({'before': before, 'after': after}) + '\n')
os.execv(str(here / 'cjc-official'), [str(here / 'cjc-official'), *after])
WRAPPER
chmod +x host/bin/cjc
export CANGJIE_HOME="$root/host"
export LD_LIBRARY_PATH="$CANGJIE_HOME/runtime/lib/linux_x86_64_cjnative:$CANGJIE_HOME/third_party/llvm/lib:$CANGJIE_HOME/tools/lib"
export PATH="$CANGJIE_HOME/bin:$CANGJIE_HOME/tools/bin:$CANGJIE_HOME/third_party/llvm/bin:$PATH"
export cjHeapSize=24GB
sha256sum host/bin/cjc host/bin/cjc-official host/runtime/lib/linux_x86_64_cjnative/*.so host/third_party/llvm/lib/libLLVM-15.so > evidence/host.sha256
nm --defined-only host/third_party/llvm/lib/libLLVM-15.so > evidence/host-llvm.nm
nm --defined-only host/runtime/lib/linux_x86_64_cjnative/libcangjie-runtime.so > evidence/host-runtime.nm
echo "jobs=$(nproc)"
(cd source && cjpm build -j "$(nproc)")
git -C source status --porcelain > evidence/source-status-after.txt
test ! -s evidence/source-status-after.txt
for name in 'cjcj::cjc' 'cjc@cjcj'; do
  if [[ -f source/target/release/bin/$name ]]; then
    test ! -e cjcj-stage1
    install -m755 "source/target/release/bin/$name" cjcj-stage1
  fi
done
test -x cjcj-stage1
sha256sum cjcj-stage1 > evidence/frontend.sha256
strings cjcj-stage1 | /usr/bin/grep 'CJCJ-COMMIT:' > evidence/frontend.stamp
test "$(cat evidence/frontend.stamp)" = "CJCJ-COMMIT:$source_sha"
# Preserve the mapped basename required by the official managed runtime.
install -m755 cjcj-stage1 host/bin/cjcj-stage1
rm host/bin/cjc
ln -s cjcj-stage1 host/bin/cjc
mkdir -p minimal
printf 'main(): Int64 { return 0 }\n' > minimal/main.cj
set +e
LD_DEBUG=libs host/bin/cjc minimal/main.cj -o minimal/main > evidence/minimal-build.log 2>&1
rc=$?
set -e
echo "$rc" > evidence/minimal-build.rc
test "$rc" = 0
sha256sum minimal/main > evidence/minimal.sha256
set +e
LD_DEBUG=libs minimal/main > evidence/minimal-run.log 2>&1
rc=$?
set -e
echo "$rc" > evidence/minimal-run.rc
test "$rc" = 0
