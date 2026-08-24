#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
test_dir=$(mktemp -d /tmp/perf-observer-symmetry.XXXXXX)
trap 'rm -rf -- "$test_dir"' EXIT

install -d "$test_dir/rt" "$test_dir/campaign/runs/SD/256MB" "$test_dir/campaign/ledger/runs/SD/256MB"
cc -shared -fPIC "$script_dir/observer_stub.c" -o "$test_dir/rt/libcangjie-runtime.so"
cc -shared -fPIC "$script_dir/observer_stub.c" -o "$test_dir/rt/libboundscheck.so"
cc "$script_dir/observer_env_probe.c" -L"$test_dir/rt" \
    -Wl,--no-as-needed -lcangjie-runtime -lboundscheck -o "$test_dir/probe"
printf 'TEST_BUILD built_at=%s probe_sha256=%s runtime_stub_sha256=%s bounds_stub_sha256=%s\n' \
    "$(date --iso-8601=ns)" \
    "$(sha256sum "$test_dir/probe" | awk '{print $1}')" \
    "$(sha256sum "$test_dir/rt/libcangjie-runtime.so" | awk '{print $1}')" \
    "$(sha256sum "$test_dir/rt/libboundscheck.so" | awk '{print $1}')"

fair="$test_dir/campaign/runs/SD/256MB/r01-subject"
official="$test_dir/campaign/runs/SD/256MB/r01-official"
ledger="$test_dir/campaign/ledger/runs/SD/256MB/r01-subject"
"$script_dir/run_one.sh" "$fair" subject 256MB "$test_dir/probe" OBSERVER_ENV_OK \
    1 probe "$test_dir/rt" 0 10 fair >/dev/null
"$script_dir/run_one.sh" "$official" official 256MB "$test_dir/probe" OBSERVER_ENV_OK \
    1 probe "$test_dir/rt" 0 10 fair >/dev/null
"$script_dir/run_one.sh" "$ledger" subject 256MB "$test_dir/probe" OBSERVER_ENV_OK \
    1 probe "$test_dir/rt" 0 10 ledger >/dev/null
printf '%s\n' $'workload\tmarker\twork_units' $'SD\tOBSERVER_ENV_OK\t1' >"$test_dir/campaign/manifest.tsv"

grep -Fx 'MRT_GC_LOG=UNSET' "$fair/stdout"
grep -Fx 'MRT_LOG_LEVEL=UNSET' "$fair/stdout"
grep -Fx 'MRT_LOG_PATH=UNSET' "$fair/stdout"
grep -Fx 'MRT_REPORT=UNSET' "$fair/stdout"
grep -Fx 'MRT_GC_LOG=1' "$ledger/stdout" || {
    echo 'TEST_FAIL test_harness_env_cut_and_restore_contract ledger MRT_GC_LOG did not reach probe'
    exit 1
}
grep -Fx 'env.MRT_GC_LOG=UNSET' "$fair/meta.txt"
grep -Fx 'env.MRT_GC_LOG=1' "$ledger/meta.txt"
for key in MRT_LOG_LEVEL MRT_LOG_PATH MRT_REPORT; do
    grep -E "^${key}=" "$fair/stdout" >"$test_dir/fair-$key"
    grep -E "^${key}=" "$ledger/stdout" | sed "s#${ledger}#ATTEMPT_DIR#" >"$test_dir/ledger-$key"
    cmp "$test_dir/fair-$key" "$test_dir/ledger-$key"
done
echo 'TEST_OK test_harness_env_cut_and_restore_contract'

set +e
"$script_dir/run_one.sh" "$test_dir/forbidden" official 256MB "$test_dir/probe" OBSERVER_ENV_OK \
    1 probe "$test_dir/rt" 0 10 ledger >"$test_dir/forbidden.stdout" 2>"$test_dir/forbidden.stderr"
forbidden_rc=$?
set -e
[[ $forbidden_rc -eq 2 ]]
grep -F 'ledger profile is subject-only' "$test_dir/forbidden.stderr"
echo 'TEST_OK test_official_ledger_rejected'

python3 "$script_dir/analyze.py" "$test_dir/campaign" "$test_dir/matrix-analysis" \
    --min-pairs 1 --bootstrap-samples 100 --seed 20260821
grep -F $'SD\t256MB\twall\t' "$test_dir/matrix-analysis/matrix.tsv"
echo 'TEST_OK test_main_ratio_accepts_clean_fair'

ledger_ratio_root="$test_dir/ledger-in-ratio"
install -d "$ledger_ratio_root/runs/SD/256MB"
cp "$test_dir/campaign/manifest.tsv" "$ledger_ratio_root/manifest.tsv"
"$script_dir/run_one.sh" "$ledger_ratio_root/runs/SD/256MB/r01-subject" \
    subject 256MB "$test_dir/probe" OBSERVER_ENV_OK 1 probe "$test_dir/rt" 0 10 ledger >/dev/null
"$script_dir/run_one.sh" "$ledger_ratio_root/runs/SD/256MB/r01-official" \
    official 256MB "$test_dir/probe" OBSERVER_ENV_OK 1 probe "$test_dir/rt" 0 10 fair >/dev/null
set +e
python3 "$script_dir/analyze.py" "$ledger_ratio_root" "$test_dir/ledger-ratio-analysis" \
    --min-pairs 1 --bootstrap-samples 100 --seed 20260821 \
    >"$test_dir/ledger-ratio.stdout" 2>"$test_dir/ledger-ratio.stderr"
ledger_ratio_rc=$?
set -e
ledger_ratio_matrix=absent
if [[ -e "$test_dir/ledger-ratio-analysis/matrix.tsv" ]]; then
    ledger_ratio_matrix=present
fi
if [[ $ledger_ratio_rc -eq 0 || "$ledger_ratio_matrix" == present ]]; then
    printf 'TEST_FAIL test_main_ratio_rejects_ledger_metadata analyzer_rc=%s matrix=%s\n' \
        "$ledger_ratio_rc" "$ledger_ratio_matrix"
fi
[[ $ledger_ratio_rc -ne 0 ]]
[[ "$ledger_ratio_matrix" == absent ]]
grep -F 'non-fair attempt cannot enter subject/official ratio' "$test_dir/ledger-ratio.stderr"
echo 'TEST_OK test_main_ratio_rejects_ledger_metadata'

dirty_ratio_root="$test_dir/dirty-fair-ratio"
cp -a "$test_dir/campaign" "$dirty_ratio_root"
sed -i 's#env.MRT_REPORT=UNSET#env.MRT_REPORT=DRIFT#' \
    "$dirty_ratio_root/runs/SD/256MB/r01-subject/meta.txt"
set +e
python3 "$script_dir/analyze.py" "$dirty_ratio_root" "$test_dir/dirty-ratio-analysis" \
    --min-pairs 1 --bootstrap-samples 100 --seed 20260821 \
    >"$test_dir/dirty-ratio.stdout" 2>"$test_dir/dirty-ratio.stderr"
dirty_ratio_rc=$?
set -e
[[ $dirty_ratio_rc -ne 0 ]]
[[ ! -e "$test_dir/dirty-ratio-analysis/matrix.tsv" ]]
grep -F 'unclean fair observer recipe for subject/official ratio' "$test_dir/dirty-ratio.stderr"
echo 'TEST_OK test_main_ratio_rejects_dirty_fair_metadata'

python3 "$script_dir/analyze_observer.py" "$test_dir/campaign" "$test_dir/analysis" \
    --min-pairs 1 --bootstrap-samples 100 --seed 20260824
grep -F $'SD\t256MB\t1\t1\t1\t' "$test_dir/analysis/summary.tsv"
echo 'TEST_OK test_observer_accepts_exact_recipe'

sed -i 's#env.MRT_REPORT=UNSET#env.MRT_REPORT=DRIFT#' "$ledger/meta.txt"
set +e
python3 "$script_dir/analyze_observer.py" "$test_dir/campaign" "$test_dir/drift-analysis" \
    --min-pairs 1 --bootstrap-samples 100 --seed 20260824 \
    >"$test_dir/drift.stdout" 2>"$test_dir/drift.stderr"
drift_rc=$?
set -e
[[ $drift_rc -ne 0 ]]
grep -E 'ledger observer recipe|expected only env.MRT_GC_LOG' "$test_dir/drift.stderr"
echo 'TEST_OK test_observer_rejects_single_arm_drift'

sed -i 's#env.MRT_REPORT=DRIFT#env.MRT_REPORT=UNSET#' "$ledger/meta.txt"
sed -i 's#env.MRT_REPORT=UNSET#env.MRT_REPORT=DRIFT#' "$fair/meta.txt" "$ledger/meta.txt"
set +e
python3 "$script_dir/analyze_observer.py" "$test_dir/campaign" "$test_dir/common-drift-analysis" \
    --min-pairs 1 --bootstrap-samples 100 --seed 20260824 \
    >"$test_dir/common-drift.stdout" 2>"$test_dir/common-drift.stderr"
common_drift_rc=$?
set -e
common_drift_pairs=absent
if [[ -e "$test_dir/common-drift-analysis/pairs.tsv" ]]; then
    common_drift_pairs=present
fi
if [[ $common_drift_rc -eq 0 || "$common_drift_pairs" == present ]]; then
    printf 'TEST_FAIL test_observer_rejects_common_mrt_report_drift analyzer_rc=%s pairs=%s\n' \
        "$common_drift_rc" "$common_drift_pairs"
fi
[[ $common_drift_rc -ne 0 ]]
[[ "$common_drift_pairs" == absent ]]
grep -F 'fair observer recipe' "$test_dir/common-drift.stderr"
echo 'TEST_OK test_observer_rejects_common_mrt_report_drift'

grep -F 'order=(subject:fair official:fair subject:ledger)' "$script_dir/run_matrix.sh"
grep -F 'order=(subject:ledger official:fair subject:fair)' "$script_dir/run_matrix.sh"
echo 'TEST_OK test_odd_even_order_is_full_reverse'

echo "OBSERVER_SYMMETRY_TEST_OK"
