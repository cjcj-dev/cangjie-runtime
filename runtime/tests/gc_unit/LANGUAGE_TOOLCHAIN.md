# Managed gate toolchain inputs

The build SDK and the SDK that compiles managed fixtures are independent inputs.
`CANGJIE_HOME` remains the caller's build environment; it is not a managed SDK
fallback. `CJC`, when supplied, must resolve to the selected managed SDK entry.

Set all of the following for managed tests, including direct runner invocations:

- `GC_UNIT_BUILD_SDK`: complete SDK used to build the runtime (identity only).
- `GC_UNIT_LANGUAGE_SDK`: complete qualified SDK used to compile the fixtures.
- `GC_UNIT_CJC_RUNTIME_LIB_DIR`: runtime used to execute that SDK's compiler.
- `GC_UNIT_LANGUAGE_QUALIFICATION`: the checked-in
  `language_toolchain_qualification.json`, copied without modification.
- `GC_UNIT_COLOUR_CHECKER`: an entity copy of
  `cjcj/ci/bootstrap/std_runtime_colour.py` with the qualified content hash.
- `GC_UNIT_COLOUR_HOST_RUNTIME`: the declared official reference runtime SO.
- `GCV2_RUNTIME_LIB_DIR`: the runtime and boundscheck SOs under test.

The qualification binds the measured direct-TLAB consumer to its
compiler, LLVM executables and their public LLVM library, static std archive set,
compiler host runtime, provenance, and colour checker/reference. It is not a
claim that any future coloured ABI is compatible. New tuples need fresh
producer/consumer evidence and an updated qualification; missing or changed
qualifications are NOT_RUN, not PASS. The current stage1 entry is the SDK's
`bin/cjc` alias to `cjcj-stage1`; admission checks the contents of both.

The qualified tuple is the 853e native-producer SDK package (archive `fe17f688…`):
the cjcj native launcher `bin/cjc` → `bin/cjcj-stage1` whose `.entry` names the
compiler entity `3be32b48…`, static std from runtime/stdlib `54497690…`
(`std_core=6484711b…`), and the LLVM entities named by the llc/opt entries.
Both public LLVM entries search `third_party/llvm/lib` first; admission binds
`third_party/llvm/lib/libLLVM-15.so` separately from the compiler host's copy.
The compiler host is the package's own `.compiler-exec` directories listed in the
launcher entry; pass `.compiler-exec/runtime/lib/linux_x86_64_cjnative` as
`GC_UNIT_CJC_RUNTIME_LIB_DIR`. The qualification binds the complete file set of
those directories. Legacy SDKs with a `host/compiler` directory are still measured
as a single directory, but no legacy tuple is currently qualified.

`language.runtime` binds the **fixed SDK runtime**, not the runtime SO being
tested. The SDK's packaged runtime keeps its measured identity (`4d6e503d…` for
this package) when a later runtime commit includes this qualification. Admission
separately measures `GCV2_RUNTIME_LIB_DIR` as `target_runtime` and
`target_boundscheck`, and checks its colour exports against the qualified
reference and std. A later target SO's commit stamp therefore does not create
a circular hash requirement. The qualification is not a byte-identity claim
about that later target SO.

Admission happens before cache lookup and compilation and is shared by the gate
and all three managed runners. Identity changes invalidate the language cache.
The C++ suite remains independent; `defer` records LANGUAGE_DEFERRED, not language
success. The tools composition gate must also transmit these inputs and include
their identity in its cache; that cross-repository work is tracked separately.

`test_language_toolchain_integration.py` runs the actual gate and direct runners
against real SDKs. It requires the same environment and `--official-sdk` and
`--out` arguments. It verifies rejection before compilation, including stale
status replacement. Positive fixture execution is run separately with the
qualified SDK and the same product SOs.

`test_language_toolchain_qualification.py --out <new-directory>
--old-qualification <previous-record> --old-sdk <previous-sdk>` runs the
current-tuple acceptance assertion and rejects both the previous record and
the previous real SDK. Run it again with `--group mutations` and a new output
directory to check each bound SDK file, each static std archive, each compiler
host file, and the checker/reference in private copies. The test follows the
launchers' entry and entity paths and the compiler host directories used by
admission. It flips one byte per file, or adds a slash to one entry search
directory to keep the recipe valid. It checks the exact first rejection
reason and restores the file. Use an ordinary temporary fixture when checks
on the real installation are limited to read-only acceptance. The old
record is a test input supplied by the caller, not an alternative admitted
qualification. Acceptance/control runs also provide the red/restore check
when a checked-in component hash is temporarily replaced with its old value.
