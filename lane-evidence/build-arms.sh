#!/bin/bash
ulimit -c 0
set -u
root=/root/sym_cangjie_runtime_581_implement_r5668277053-final3
cd "$root" || exit 2
uptime > arms-uptime-before.txt
for arm in producer promotion phase consumer entry restored ohos; do
  child="$root-$arm"
  mkdir -p "$child/source"
  tar -xzf "$root/source.tar.gz" -C "$child/source"
  if [ "$arm" != restored ] && [ "$arm" != ohos ]; then
    (cd "$child/source" && patch -p1 < "$root/cut-$arm.diff") || exit 3
  fi
  tar -czf "$child/source.tar.gz" -C "$child/source" runtime AGENTS.md
  rm -rf "$child/source"
  sed "s#${root}#${child}#g" "$root/remote_build.sh" > "$child/remote_build.sh"
  if [ "$arm" = ohos ]; then
    sed -i 's/for arm in default testable/for arm in default/g; s/-DCOPYGC_FLAG=1/-DMRT_GC_UNIT_OHOS_HOST=ON -DCOPYGC_FLAG=1/' "$child/remote_build.sh"
  fi
  (bash "$child/remote_build.sh" > "$root/build-$arm.summary" 2>&1; echo $? > "$root/build-$arm.rc") &
done
wait
uptime > arms-uptime-after.txt
