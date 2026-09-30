#!/usr/bin/env bash
# Observe the selected test and its exec'd other-VM descendants before the
# kernel delivers SIGSEGV. Keep the normal test status and completion contract.
set -euo pipefail
ulimit -c 0
if [[ $# -eq 0 ]]; then
  echo 'usage: capture_segv.sh ELF [ARGS...]' >&2
  exit 2
fi
exec gdb -q -nx -batch \
  -ex 'set pagination off' \
  -ex 'set confirm off' \
  -ex 'set detach-on-fork off' \
  -ex 'set follow-fork-mode parent' \
  -ex 'set schedule-multiple on' \
  -ex 'handle SIGSEGV stop print pass' \
  -ex 'handle SIGPIPE nostop noprint pass' \
  -ex "source $(cd "$(dirname "$0")" && pwd)/capture_segv.gdb.py" \
  --args "$@"
