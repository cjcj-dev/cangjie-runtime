#!/usr/bin/env bash
set -u
ulimit -c 0
root=/root/sym_cangjie_runtime_1322_implement_r5896137675
cd "$root" || exit 2
sdk=/root/sdkdepot/b99430a618af-1ecb811801ca
hrt=/root/sharedbuild/h48-host-runtime/35da7be2434ad72348ed27e8a0bf599ec4e91524/linux_x86_64_cjnative
start=$SECONDS
uptime > keep/sdk-uptime-before.txt
python3 std_runtime_colour.py --colour-runtime "$sdk/runtime/lib/linux_x86_64_cjnative/libcangjie-runtime.so" --host-runtime "$hrt/libcangjie-runtime.so" --runtime "$sdk/runtime/lib/linux_x86_64_cjnative/libcangjie-runtime.so" --std "$sdk/lib/linux_x86_64_cjnative/libcangjie-std-core.a" --source "$sdk" > keep/std-colour.log 2>&1
rc=$?
printf 'rc=%s wall=%s\n' "$rc" "$((SECONDS-start))" > keep/std-colour.rc
uptime > keep/sdk-uptime-after.txt
cat keep/std-colour.rc
# Avoid printing the potentially large export list.
tail -c 1500 keep/std-colour.log
exit "$rc"
