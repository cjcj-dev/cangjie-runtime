# Windows export contract

The native link still uses `--export-all-symbols`. The checked contract covers
exports referenced by compiler emitters and the standard library, captured in
`src/windows_export_references.json`. Each entry has caller file/line evidence;
each input tree has a revision and a content digest. There is no prefix allowlist.

`generate_windows_exports.py collect` captures source references, excluding
comments. Emitter inputs contribute symbol strings and macro tables, plus
constant string concatenations resolved from their definitions. Source inputs
contribute identifiers, including foreign declarations. The intersection with a
complete, known-good Windows export definition assigns ownership to this DLL;
references to other libraries do not become runtime requirements.

Collect from pinned compiler CodeGen/CHIR, LLVM lib/include, cjcj CodeGen and
stdlib/libs trees, using one `--consumer` per subtree:

```sh
python3 runtime/build/generate_windows_exports.py collect complete.raw.def \
  runtime/src/windows_export_references.json \
  --consumer emitter:compiler-codegen@COMMIT=COMPILER/src/CodeGen \
  --consumer emitter:compiler-reflection@COMMIT=COMPILER/include/cangjie/CHIR \
  --consumer emitter:llvm-lib@COMMIT=LLVM/llvm/lib \
  --consumer emitter:llvm-include@COMMIT=LLVM/llvm/include \
  --consumer emitter:cjcj-codegen@COMMIT=CJCJ/packages/codegen/src \
  --consumer source:stdlib@COMMIT=stdlib/libs
python3 runtime/build/generate_windows_exports.py write complete.raw.def \
  runtime/src/windows_x86_64_exports.def --source-ref RUNTIME_COMMIT
```

Replace each `COMMIT` with the source tree's 40-digit revision. The initial
capture uses the complete registered definition from runtime commit
`d2840f7621dfe6d31e260e9bedd1dc33c8b08a5f`; this is a migration input, not the
candidate DLL under test. Capture is an explicit contract update: do not recapture
from a failing candidate to make a missing symbol disappear. Review source
changes that construct names dynamically when updating compiler consumers.

Generation and verification use the same reference selector. Generation fails
if any captured symbol is absent. Verification also requires the registered
`.def` to contain exactly the captured contract. Then it checks every contract
symbol's presence and DATA attribute in the candidate. Extra implementation
symbols and linker-assigned ordinals are not caller ABI requirements. The
`.def` remains usable by existing name-based readers and is not a linker input.

Run `runtime/tests/windows_exports/test_contract.py --raw complete.raw.def` to
exercise the actual CLI, both libc++ ABI tags, and removal of each captured
symbol. The Windows consumer export contract workflow also runs the complete
CLANG64 product build, saving toolchain identity, raw/registered definitions,
reference provenance, DLL hashes and results.
