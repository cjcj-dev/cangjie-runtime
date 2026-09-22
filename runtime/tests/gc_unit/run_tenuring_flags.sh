#!/usr/bin/env bash
# Run the same compiled product tests against a retained product SO directory.
set -uo pipefail
ulimit -c 0
: "${TENURING_TEST_ELF:?}" "${TENURING_LIB_DIR:?}" "${TENURING_OUT:?}" "${TENURING_CPUSET:?}"
mkdir -p "$TENURING_OUT"
uptime > "$TENURING_OUT/uptime-before.txt"
sha256sum "$TENURING_TEST_ELF" "$TENURING_LIB_DIR/libcangjie-runtime.so" \
    "$TENURING_LIB_DIR/libboundscheck.so" > "$TENURING_OUT/identity.sha256"
nm --defined-only "$TENURING_LIB_DIR/libcangjie-runtime.so" > "$TENURING_OUT/product.full-nm.txt"
printf 'cpuset=%s\n' "$TENURING_CPUSET" > "$TENURING_OUT/recipe.txt"
cases=(DefaultSmallHeap DefaultLargeHeap DefaultCeiling DefaultManyWorkers ExplicitAutomatic
       ExplicitMaximum MaximumZero OverrideZero OverridePositive MaximumAndOverride
       EnvironmentOverride EnvironmentMaximum EnvironmentAutomatic EnvironmentZero MaximumOne PrecleanOverridesFlag MaximumBoundary OverrideBoundary DefaultBoundaryFourteen)
start=$SECONDS
run_case() {
    local name=$1
    env LD_LIBRARY_PATH="$TENURING_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
      LD_DEBUG=libs GC_UNIT_FILTER="$name" timeout 45 taskset -c "$TENURING_CPUSET" \
      "$TENURING_TEST_ELF" > "$TENURING_OUT/$name.log" 2>&1
    echo "$?" > "$TENURING_OUT/$name.rc"
}
pids=()
for name in "${cases[@]}"; do run_case "TenuringFlags.$name" & pids+=("$!"); done
run_case PageAge.UntypeRoundTrip & pids+=("$!")
for pid in "${pids[@]}"; do wait "$pid"; done
failed=0
for result in "$TENURING_OUT"/*.rc; do
    [[ $(basename "$result") == run.rc ]] && continue
    rc=$(cat "$result")
    printf '%s rc=%s\n' "$(basename "$result" .rc)" "$rc"
    [[ "$rc" == 0 ]] || failed=$((failed + 1))
done
printf 'cases=%s failed=%s parallel=%s wall=%s\n' "${#pids[@]}" "$failed" "${#pids[@]}" "$((SECONDS-start))" > "$TENURING_OUT/summary.txt"
uptime > "$TENURING_OUT/uptime-after.txt"
if [[ "$failed" == 0 ]]; then echo 0 > "$TENURING_OUT/run.rc"; exit 0; fi
echo 1 > "$TENURING_OUT/run.rc"
exit 1
