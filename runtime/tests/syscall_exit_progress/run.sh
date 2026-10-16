#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
root=$(realpath "$1")
library=$(realpath "$2")
fixture=$(realpath "$3")
script_dir=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$root"
uptime > "$root/uptime-before.txt"
for mode in asleep gap fast slow-p ordinary; do
    mkdir -p "$root/$mode/lib"
    cp "$fixture" "$root/$mode/blocking_task"
    cp "$library"/*.so "$root/$mode/lib/"
    sha256sum "$root/$mode/blocking_task" "$root/$mode/lib/"*.so > "$root/$mode/identity.sha256"
    (
        set +e
        python3 "$script_dir/run.py" "$root/$mode" "$mode" > "$root/$mode/run.log" 2>&1
        echo "$?" > "$root/$mode/run.rc"
    ) &
done
wait
uptime > "$root/uptime-after.txt"
python3 - "$root" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
results = [json.loads(path.read_text()) for path in sorted(root.glob("*/result.json"))]
(root / "results.json").write_text(json.dumps(results, indent=2) + "\n")
for result in results:
    print(result["mode"], result["status"], "rc=" + str(result["rc"]))
sys.exit(0 if len(results) == 5 and all(result["rc"] == 0 for result in results) else 1)
PY
