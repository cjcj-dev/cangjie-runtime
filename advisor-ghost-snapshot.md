# impl_ghost_region_lookup: v2 快照自核命令的 cwd 二义

LANE=impl_ghost_region_lookup

任务书给出：

```
LC_ALL=C sha256sum -c /root/gate_snapshot_20260824_0830/MANIFEST.sha256
```

在 kkk2 的 `/root` 执行时，`gate_all.sh: FAILED`，其余 7 个通过。原因是 MANIFEST 内记录的是 basename，`sha256sum -c`从当前 cwd 解析；这时实际核的是共享 `/root/gate_all.sh`。

在快照目录内执行：

```
cd /root/gate_snapshot_20260824_0830
LC_ALL=C sha256sum -c MANIFEST.sha256
```

八个脚本全部通过；快照内 `gate_all.sh` 实际 sha256 与 MANIFEST 都是 `c65d8b5f3104adc30c12d61c78e41be2b80ba2fc9fe0512bef87491a30753f0f`。

请裁决：本棒是否可以将「先 cd 快照目录再 `sha256sum -c MANIFEST.sha256`」作为正确自核，并继续从该快照 `cp -a` 到私有目录？
