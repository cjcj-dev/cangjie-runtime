#!/bin/bash
# Resume completed static-std arms after their launcher was rewritten while bash
# was reading it. Both llc and static closure already have successful receipts.
set -euo pipefail
ulimit -c 0
R=/root/sym_cangjie_runtime_608_implement_r5683164869
arm=${1:?}
physical=$R/abi-final-$arm
canonical=$R/abi-canonical
cp "$physical/evidence/build.rc" "$physical/evidence/launcher-interrupted.rc"
unshare -m --propagation private bash -c '
  set -euo pipefail
  mount --bind "$1" "$2"
  cd "$2"
  start=$SECONDS
  trap '\''rc=$?; echo "$rc" > evidence/shared-completion.rc; echo "wall=$((SECONDS-start))" > evidence/shared-completion.wall'\'' EXIT
  [ "$(cat evidence/std-build.rc)" = 0 ]
  bash build-shared.sh
  [ "$(cat evidence/std-shared.rc)" = 0 ]
  python3 install-closure.py
  find std-install -type f -print0 | sort -z | xargs -0 sha256sum > evidence/std.sha256
  echo 0 > evidence/build.rc
' _ "$physical" "$canonical"
