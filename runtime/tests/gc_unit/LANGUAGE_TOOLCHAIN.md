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
compiler, LLVM executables, static std archive set,
compiler host runtime, provenance, and colour checker/reference. It is not a
claim that any future coloured ABI is compatible. New tuples need fresh
producer/consumer evidence and an updated qualification; missing or changed
qualifications are NOT_RUN, not PASS. The current stage1 entry is the SDK's
`bin/cjc` alias to `cjcj-stage1`; admission checks the contents of both.

The qualified tuple is the supplied stage1 compiler `a4ae3418…`, same-source
std built from runtime `f2283db793…` (`std_core=47a91409…`), and the LLVM
tuple recorded in its `SDK.lock.json`. The compiler host is independent of
the target runtime: its runtime, boundscheck, and host LLVM must match the
current `cjcj/ci/bootstrap/stage1_host_identities.txt`. Put entity copies in
the private SDK's `host/compiler` directory and pass that directory as
`GC_UNIT_CJC_RUNTIME_LIB_DIR`. The qualification binds the complete directory,
including the host LLVM; compiler invocations search it before the target LLVM.
Adding this directory does not rebuild or replace the SDK's static std.

`language.runtime` binds the **fixed SDK runtime**, not the runtime SO being
tested. The SDK's `SDK.lock.json` and its runtime retain the original `f2283db793…`
identity when a later runtime commit includes this qualification. Admission
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
directory to change one byte of each bound SDK component, each static std
archive, each compiler-host file, and the checker/reference in private copies.
It checks the exact first rejection reason and restores each byte. The old
record is a test input supplied by the caller, not an alternative admitted
qualification. Acceptance/control runs also provide the red/restore check
when a checked-in component hash is temporarily replaced with its old value.
