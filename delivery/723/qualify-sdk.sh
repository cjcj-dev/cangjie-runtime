#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
LANE=/root/sym_cangjie_runtime_723_implement_r5741705087
OLD=/root/sdkdepot/945fe3e8f023-fa13e8d5c17b
CJC=$LANE/source/target/release/bin/cjcj::cjc
CJC_SHA=$(sha256sum "$CJC" | cut -d' ' -f1)
NEW=/root/sdkdepot/${CJC_SHA:0:12}-fa13e8d5c17b
HRT=/root/sym_cjcj_48_implement_r5685150408/host/runtime/lib/linux_x86_64_cjnative
STDLIB=/root/sym_cangjie_runtime_704_implement_r5737968063/stdlib
[[ ! -e "$NEW" ]]
cp -a "$OLD" "$NEW"
cp "$CJC" "$NEW/bin/cjcj-stage1"
for tool in llc opt; do
  cp /root/llvmdepot/fa13e8d5/$tool "$NEW/bin/$tool"
  cp /root/llvmdepot/fa13e8d5/$tool "$NEW/third_party/llvm/bin/$tool"
done
{
  echo stage1_sha12=${CJC_SHA:0:12}
  echo cjc_sha256=$CJC_SHA
  echo cjcj_source_sha=$(cat "$LANE/source.sha")
  echo llvm_sha=fa13e8d5c17ba1d65c2bc94d71ec801b49209d8f
  echo llc_sha256=$(sha256sum "$NEW/bin/llc" | cut -d' ' -f1)
  echo opt_sha256=$(sha256sum "$NEW/bin/opt" | cut -d' ' -f1)
  echo stage1_build_host=/root/cj_build/gate_hosts/nightly-1.3.0-alpha.20260904010027
  echo compiler_run_host=$HRT
  echo base_sdk=$OLD
  echo stdlib_origin=unchanged-from-base-sdk
  echo qualification=$LANE/evidence/random-new
} > "$NEW/MANIFEST"
echo "$NEW" > "$LANE/sdk.path"
sha256sum "$HRT/libcangjie-runtime.so" "$HRT/libboundscheck.so" > "$LANE/evidence/host-so.sha256"
for arm in new old; do
  home=$NEW; [[ $arm == old ]] && home=$OLD
  for i in $(seq 1 10); do
    (
      out=$LANE/evidence/random-$arm/run$i
      mkdir -p "$out"
      export CANGJIE_HOME=$home
      export PATH=$home/bin:/usr/bin:/bin
      export LD_LIBRARY_PATH=$HRT:$home/third_party/llvm/lib:$home/tools/lib
      export CANGJIE_PATH=/root/sym_cjcj_54_implement_r5738848751/stdverify/build-fresh/modules/linux_x86_64_cjnative
      export LIBRARY_PATH=/root/sym_cjcj_54_implement_r5738848751/stdverify/build-fresh/lib
      uptime > "$out/uptime.before"
      sha256sum "$home/bin/cjc" "$HRT/libcangjie-runtime.so" "$HRT/libboundscheck.so" > "$out/inputs.sha256"
      echo '0-63' > "$out/cores"
      start=$SECONDS
      set +e
      taskset -c 0-63 "$home/bin/cjc" --no-sub-pkg -g --apc=1 --output-type=staticlib -p "$STDLIB/libs/std/random" --lto=full --output "$out/libstd.random.bc" -O2 > "$out/cjc.log" 2>&1
      rc=$?
      set -e
      echo "$rc" > "$out/cjc.rc"
      echo "wall=$((SECONDS-start))" > "$out/wall"
      uptime > "$out/uptime.after"
      [[ ! -f "$out/libstd.random.bc" ]] || sha256sum "$out/libstd.random.bc" > "$out/output.sha256"
    ) &
  done
done
wait
python3 - "$LANE" <<'PY'
import json, pathlib, sys
lane=pathlib.Path(sys.argv[1])
summary={arm:[int((lane/f'evidence/random-{arm}/run{i}/cjc.rc').read_text()) for i in range(1,11)] for arm in ('new','old')}
(lane/'evidence/random-summary.json').write_text(json.dumps(summary,indent=2)+'\n')
print(summary)
assert summary['new']==[0]*10
assert all((lane/f'evidence/random-new/run{i}/libstd.random.bc').stat().st_size for i in range(1,11))
PY
