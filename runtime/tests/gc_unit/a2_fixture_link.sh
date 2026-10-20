#!/usr/bin/env bash
# Record the one contiguous-fixture link, including failed-link inputs.
set -euo pipefail
ulimit -c 0
evidence=$(realpath -m "$1")
shift
mkdir -p "$evidence"
script_dir=$(cd "$(dirname "$0")" && pwd)
cp "$script_dir/package_init_image.cpp" "$script_dir/a2_contiguous.ld" "$evidence/"
printf '%q ' "$@" -v "-Wl,-Map,$evidence/link.map,--cref,--trace" > "$evidence/link.argv"
printf '\n' >> "$evidence/link.argv"
pwd > "$evidence/link.cwd"
output=
previous=
index=0
inspect() {
  local file=$1 stem=$2 rc
  set +e
  readelf -W -h -l -S -r -s --debug-dump=frames "$file" > "$stem.readelf" 2> "$stem.readelf.stderr"
  rc=$?
  printf '%s\n' "$rc" > "$stem.readelf.rc"
  nm --defined-only "$file" > "$stem.nm" 2> "$stem.nm.stderr"
  rc=$?
  printf '%s\n' "$rc" > "$stem.nm.rc"
  set -e
  sha256sum "$file" > "$stem.sha256"
}
for argument in "$@"; do
  if [[ $previous == -o ]]; then output=$argument; fi
  if [[ $argument == *.o && -f $argument ]]; then
    stem="$evidence/input-$index"
    # Keep originals as well as a stable copy; CMake and standalone both pass a TU.o.
    cp "$argument" "$stem.o"
    for extension in ii s bc; do
      if [[ -f ${argument%.o}.$extension ]]; then
        cp "${argument%.o}.$extension" "$stem.$extension"
      fi
    done
    inspect "$stem.o" "$stem"
    index=$((index + 1))
  fi
  previous=$argument
done
set +e
"$@" -v "-Wl,-Map,$evidence/link.map,--cref,--trace" > "$evidence/link.log" 2>&1
rc=$?
set -e
printf '%s\n' "$rc" > "$evidence/link.rc"
# GNU ld may remove its failed output; record that fact without creating a substitute.
if [[ -n $output && -f $output ]]; then
  cp "$output" "$evidence/output.so"
  inspect "$evidence/output.so" "$evidence/output"
else
  printf 'linker left no output file: %s\n' "$output" > "$evidence/output.absent"
fi
cat "$evidence/link.log"
exit "$rc"
