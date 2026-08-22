# Canonical GC workloads

This file is the identity ledger for the current `sd256` and `nw256` gates.
A result without the full ELF SHA-256 below is not attributable to either
gate.  The two entries deliberately use the same compiler, optimization
level, static standard library, and default paint-on lowering.

## Current set (2026-08-23)

| Gate | Canonical path on kkk2 | ELF SHA-256 | Size | Built by / at | Source and flags |
|---|---|---|---:|---|---|
| `sd256` | `/root/sdprov-run/canonical/survival_dense` | `12587d56300be61bbb14db1eb6ec68b2590b8014e9650365d2b6bab82f025dc5` | 1,086,984 | lane `sdprov` (Zxilly), 2026-08-23 03:22:39 +0800 | `survival_dense.cj` `e0e045bd8acac455cb1d95938cd21c62e2a9febcf0efa9b432866863b52a838d`; `cjc -O2 --static-std`, no llc wrapper or paint override |
| `nw256` | `/root/sdprov-run/canonical/natural_wave_notime` | `2f2acb78baed487499d26ef57b6a231cb7aeca1b0ad6c9d31c4ab8bf067c2402` | 1,096,424 | lane `sdprov` (Zxilly), 2026-08-23 03:41:39 +0800 | `natural_wave_notime.cj` `7adedc59e071c786b7ca74faddf5be5606617d1d3242d9ce2b2ad006e609159a`; `cjc -O2 --static-std`, no llc wrapper or paint override |

Both ELFs were linked against the same static core archive and produced by
the same backend identity:

| Input | SHA-256 / stamp |
|---|---|
| `cjc` and `cjc-frontend` | `ed806687b1fa0228b84d18b72e01cdc174d75d140cf5f7dd6267598fb80cb509` |
| `opt` | `c2f6b4f50c951b7316aa6619b3960b025780eb76b55098194346ca8a6443dad2` |
| `llc` | `6b826fafc550e410a58554bee5ca4f6f77c840ea0493db28ea36a43f7ddd8090` |
| LLVM provenance | `CJLLVM-COMMIT:6e5da66db63d6b1c90dba8b2263949dafbb9acaf` |
| `libcangjie-std-core.a` | `9f4b82140e34825b81ce82cd214e53d624c86593417bae34c612911526c1da8a` |

Exact build shape (substituting the source and output from the table):

```bash
env -i HOME=/root PATH=<paintflip-sdk-paths> CANGJIE_HOME=/root/paintflip-run/sdk \
  LD_LIBRARY_PATH=<host-and-sdk-libraries> cjHeapSize=24GB \
  taskset -c 0-15 /root/paintflip-run/sdk/bin/cjc \
  -O2 --static-std -V -o <output> <source>
```

The trust fast-arm count is `0` for both ELFs.  This count means a
`shr $0x30` followed by a conditional branch in the next three instructions;
the one raw `shr $0x30` present in each ELF does not satisfy that detector.
Thus these are trust-off controls as intended, not silent trust fast-road
products.

## Why these are selected

They are detector artifacts, not trophies selected for passing.  They keep
the intended default store-good paint path enabled and use production-like
`-O2`; that is the code-generation state whose scalar non-heap write defect
this investigation exposed.

- `survival_dense` is a fail-closed positive detector: against runtime
  `8165ce23f9c824ba7f8ec83987c13228fac7183a` at 256 MB it was GOLD `0/12`.
  The single-variable `-O0` pair is independently causal: true paint-on ELF
  `b3d0c7ad81072878cdf68e676f31283170b5331dbaa164750d547615863238ef`
  was GOLD `2/12`, while disabling paint alone produced ELF
  `920de4d371dcb86287342a729db0717c4469feb22a2c8575b824ad6956eddc05`
  byte-for-byte and was GOLD `12/12` in the same alternating window.  The
  strict nine-column result table is
  `89755a872eb189b8369180c28b91ed749e902c22b7ed0e2dfefe15507ed8ea9c`.
  A green result from that paint-off control must not be counted as release
  acceptance.
- `natural_wave_notime` is built from the same pinned chain so `nw256` does
  not silently test another compiler mode.  Its first 256 MB smoke run
  completed with rc=0 but checksum `635925220717200`, not the established
  gold `635925223159200`; it therefore also retains detection power.

These ELFs are intentionally RED diagnostic canonicals while LLVM commit
`6e5da66d` is unfixed.  After the lowering defect is corrected, rebuild both
from the same sources and flags, re-run the negative-control mutation that
makes the guard fail, and replace the two SHA rows atomically.  Do not bless
the paint-off controls merely because they are green.

## Explicitly non-canonical survival_dense artifacts

| SHA-256 | Disposition |
|---|---|
| `2603386ff5ae8da94ec12281a67ced07163205677597a645cf06d67878f30489` | Historical A. It is an older weakmask LLVM / `-O2` product and drifts `12/12`; it is useful evidence, but not the pinned current toolchain. |
| `920de4d371dcb86287342a729db0717c4469feb22a2c8575b824ad6956eddc05` | Historical B and exact paint-off control. The producing harness passed the literal `default` through a non-empty-string test and actually injected `-cj-store-good-paint=0`; its old “default paint-on” label is false. |
| `b3d0c7ad81072878cdf68e676f31283170b5331dbaa164750d547615863238ef` | Same compiler/source at true paint-on `-O0`; useful causal arm, not the canonical production optimization level. |

## Reporting rule

Every `sd256`/`nw256` result must include the full workload ELF SHA-256, the
runtime SO SHA-256, heap size, optimization/paint mode, and the last
`checksum=` line (`tail -1`).  A basename such as `survival_dense` is not an
identity.
