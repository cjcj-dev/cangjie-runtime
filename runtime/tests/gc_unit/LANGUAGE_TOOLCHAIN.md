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

The qualification binds the previously measured direct-TLAB consumer to its
compiler, LLVM entry wrappers and actual executables, static std archive set,
compiler host runtime, provenance, and colour checker/reference. It is not a
claim that any future coloured ABI is compatible. New tuples need fresh
producer/consumer evidence and an updated qualification; missing or changed
qualifications are NOT_RUN, not PASS. The known stage1 entry is an existing
wrapper that executes `cjcj-stage1`; both contents are checked.

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
