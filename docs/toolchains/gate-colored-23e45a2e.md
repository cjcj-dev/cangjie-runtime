# Rebuilt colored toolchain for #571

待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。

Coordinates: LLVM `23e45a2e9dfbfc4a708d99bc7895fbc1d8ffcbaf`, runtime/stdlib
`fca1c8ce77f139b14cef9117c1246fe01b0aa084`, clean frontend
`52cb48f5524f67f4df79761bac863441d2b72c79`.

The kkk2 link `gate-colored-23e45a2e` resolves to
`/root/sym_cangjie_runtime_585_implement_r5669360435/gate-sdk`.
The adjacent identity JSON records compiler, backend, all twelve std packages
(`.a`, `.so`, `.cjo`), both runtime configurations, boundscheck, and compiler host
libraries. Runtime provenance is
`src-56c9cf4dc969e82d472d8698fd74879ee83afd6511339503532c38857faa5b34`.

Use the SDK's `bin/cjc` with compiler-host libraries from the same lane's
`host/runtime/lib/linux_x86_64_cjnative`, `host/third_party/llvm/lib`, and
`host/tools/lib`. Managed execution uses the lane's
`testable/build/runtime-staging/lib/x86_64_Release` (both SOs), or `default/`
for the default configuration. `tools/run_colored_585.py` records the exact
environment and accepts three reserved CPU ranges.

Validation receipts remain on kkk2 under that lane:

- `evidence/identity.json`, `source-inputs.sha256`, `loading.json`:
  build identity and actual loader/ldd/maps records for managed executables.
- `green/replay/{O0,O2}` and `baseline/replay/{O0,O2}`:
  same CJ input through complete new and old SDKs; generated assembly and ELF
  retain static write runtime calls only on the new chain without phase guards.
- `entry-checks/`: merged LLVM native static ref/struct and atomic contracts.
  New static checks return 0, old static checks return 1; atomic checks return 0.
- `control/evidence/{green,cut,restored}-final-check.{log,rc}`:
  isolated SDK backend replacement gives 0/2/0 at the static-write assertions.
  All compilations succeed; both restored ELF hashes equal their original hashes.
- `evidence/managed-results.json`: each of three managed runners ran N=3.
  Runner rc is 1 for every sample. LOADFC counts are finalizer 0/0/0,
  segmented array 1/1/1, phase entry 1/1/1. These are observations for #571,
  not managed acceptance. The finalizer executable exits 0 but its runner fails.

Runtime builds use `kkk2_build_two.sh`, `-j192`, two parallel configurations.
LLVM/std build returns 0 (389s; std static dependency graph 252s).
Standalone default/testable return 0/0 (526/699 cases); OHOS attempt returns 21
because its required product receipt symbol is absent. No runtime or std source
or existing test expectation is changed by this delivery.

The CJ struct is lowered into reference writes; native static-struct intrinsic
coverage comes from the merged LLVM test. Atomic checks establish ABI/routing,
not a separate runtime atomic semantics proof. No universal frontend/reflection
coverage or #571 success is inferred from these finite samples.
