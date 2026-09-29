#!/usr/bin/env bash
set -eu
ulimit -c 0
r=/root/sym_cangjie_runtime_1305_implement_r5892797534
mkdir -p "$r/keep/default-tests" "$r/keep/testable-tests" "$r/keep/ohos-tests" "$r/keep/default"
cp "$r/unit-final-default/cj_gc_unit" "$r/unit-final-default/cj_gc_forwarding_publication_unit" "$r/keep/default-tests/"
cp "$r/unit-final-testable/cj_gc_unit" "$r/unit-final-testable/cj_gc_forwarding_publication_unit" "$r/keep/testable-tests/"
cp "$r/ohos/unit/cj_gc_ohos_host_unit" "$r/keep/ohos-tests/"
cp -a "$r/default/build/runtime-staging/lib/x86_64_Release/"*.so "$r/keep/default/"
cp "$r/cuts-source.tar.gz" "$r/keep/source-a1543759f5df.tar.gz"
for arm in candidate restored termination objects arrays bitmap entry; do
 cp "$r/arms/$arm/build/CMakeCache.txt" "$r/arms/$arm/CMakeCache.txt"
 stat -c '%n %y' "$r/arms/$arm/runtime/src/Heap/z/zHeapIterator.cpp" "$r/keep/$arm/libcangjie-runtime.so" > "$r/arms/$arm/timestamps.txt"
done
for arm in default testable; do cp "$r/$arm/build/CMakeCache.txt" "$r/$arm-CMakeCache.txt"; done
cp "$r/ohos/build/CMakeCache.txt" "$r/ohos/CMakeCache.txt"
{
 for file in runtime/src/Base/BitMap.h runtime/src/Heap/z/zRootsIterator.cpp runtime/src/Heap/z/zHeapIterator.cpp; do
  cmp "$r/arms/candidate/$file" "$r/arms/restored/$file"
  echo "RESTORE_COMPARE_RC=0 file=$file"
  sha256sum "$r/arms/candidate/$file" "$r/arms/restored/$file"
 done
} > "$r/matrix/source-restore.txt"
{
 for arm in termination objects arrays bitmap entry; do
  /usr/bin/grep -E '\[  FAIL|expect|EXPECT|P82_.*ASSERT|P82_TERMINATION' "$r/matrix/$arm/"*.log || true
 done
} > "$r/matrix/failure-assertions.txt"
# Build and test intermediate artifacts are no longer needed. Retained SO/ELF
# inputs are explicit keep/ copies, with run-time hashes in matrix/ and units/.
rm -rf "$r/default/build" "$r/testable/build" "$r/ohos/build"
for arm in candidate restored termination objects arrays bitmap entry; do rm -rf "$r/arms/$arm/build" "$r/arms/$arm/runtime"; done
rm -rf "$r/default/runtime" "$r/testable/runtime" "$r/ohos/runtime"
for name in unit-initial unit-updated unit-candidate unit-final-default unit-final-filler unit-final-testable; do
 rm -rf "$r/$name/objects" "$r/$name/ohos_host_runroot"
 rm -f "$r/$name/cj_gc_unit" "$r/$name/cj_gc_forwarding_publication_unit" "$r/$name/cj_gc_other_vm_exit_unit" "$r/$name/"*.so
 done
rm -rf "$r/ohos/unit/ohos_host_runroot" "$r/ohos/shim"
rm -f "$r/ohos/unit/cj_gc_ohos_host_unit" "$r/source.tar.gz" "$r/cuts-source.tar.gz" "$r/cuts.tar.gz"
du -sh "$r" > "$r/retained-size.txt"
# Small evidence bundle only. Large SO/ELF carriers stay in remote keep/.
tar -czf "$r/evidence.tar.gz" --exclude='matrix/elf.defined.txt' \
 -C "$r" matrix cuts arms \
 default-configure.log default-build.log default-configure.rc default-build.rc default-wall.txt default-so.sha256 default-CMakeCache.txt \
 testable-configure.log testable-build.log testable-configure.rc testable-build.rc testable-wall.txt testable-so.sha256 testable-CMakeCache.txt \
 uptime-before.txt uptime-after.txt units-uptime-before.txt units-uptime-after.txt \
 unit-final-default/run.log unit-final-default/run.rc unit-final-default/elf.sha256 unit-final-default/so.sha256 unit-final-default/wall.txt \
 unit-final-filler/run.log unit-final-filler/run.rc unit-final-filler/elf.sha256 unit-final-filler/so.sha256 unit-final-filler/wall.txt \
 unit-final-testable/run.log unit-final-testable/run.rc unit-final-testable/elf.sha256 unit-final-testable/so.sha256 unit-final-testable/wall.txt \
 ohos/configure-first.log ohos/configure.log ohos/configure.rc ohos/build.log ohos/build.rc ohos/run.log ohos/run.rc ohos/so.sha256 ohos/wall.txt ohos/CMakeCache.txt ohos/shim.sha256 ohos/unit/ohos_host.receipt \
 build-cut-arms.sh run-cut-matrix.sh run-unit-arms.sh run-ohos.sh retained-size.txt
cat "$r/retained-size.txt"
