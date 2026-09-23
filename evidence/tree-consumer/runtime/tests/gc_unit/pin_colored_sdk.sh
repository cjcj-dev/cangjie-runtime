#!/usr/bin/env bash
# Pin 945fe3e8 stage1 + fa13e8d5 llc/opt SDK onto kkk2:/root/sdkdepot/<s12>-<l12>/.
# Does not write /root/sdks or /root/.cjv.
set -euo pipefail
ulimit -c 0

STAGE1_SHA12=${STAGE1_SHA12:-945fe3e8f023}
LLVM_SHA12=${LLVM_SHA12:-fa13e8d5c17b}
DEST=${DEST:-/root/sdkdepot/${STAGE1_SHA12}-${LLVM_SHA12}}
SRC=${SRC:-/root/sym_cjcj_54_implement_r5738848751/stdverify/home-good}
LLC_SRC=${LLC_SRC:-/root/llvmdepot/fa13e8d5/llc}
OPT_SRC=${OPT_SRC:-/root/llvmdepot/fa13e8d5/opt}

if [[ ! -x "$SRC/bin/cjc" ]]; then
  echo "pin_colored_sdk FAIL: missing $SRC/bin/cjc" >&2
  exit 2
fi
mkdir -p /root/sdkdepot
if [[ ! -d "$DEST/bin" ]]; then
  cp -a "$SRC" "$DEST"
fi
mkdir -p "$DEST/bin" "$DEST/third_party/llvm/bin"
if [[ -x "$LLC_SRC" ]]; then
  cp -a "$LLC_SRC" "$DEST/bin/llc"
  cp -a "$LLC_SRC" "$DEST/third_party/llvm/bin/llc"
fi
if [[ -x "$OPT_SRC" ]]; then
  cp -a "$OPT_SRC" "$DEST/bin/opt"
  cp -a "$OPT_SRC" "$DEST/third_party/llvm/bin/opt"
fi
ln -sfn cjcj-stage1 "$DEST/bin/cjc"
# cjc already sha-identical to cjcj-stage1 on home-good; keep both names.

CJC_SHA=$(sha256sum "$DEST/bin/cjc" | awk '{print $1}')
LLC_SHA=$(sha256sum "$DEST/bin/llc" | awk '{print $1}')
OPT_SHA=$(sha256sum "$DEST/bin/opt" | awk '{print $1}')
{
  echo "stage1_sha12=$STAGE1_SHA12"
  echo "llvm_sha12=$LLVM_SHA12"
  echo "cjc_sha256=$CJC_SHA"
  echo "llc_sha256=$LLC_SHA"
  echo "opt_sha256=$OPT_SHA"
  echo "src=$SRC"
  echo "missing_packages=std.random"
  echo "missing_reason=cjcj#56 cjc --lto=full -p std/random signal 11 in frontend before opt; pin unrelated"
  echo "std_packages_p01_zero=11 (cjcj#54 stdverify)"
} > "$DEST/MANIFEST"
echo "pin_colored_sdk dest=$DEST"
cat "$DEST/MANIFEST"
