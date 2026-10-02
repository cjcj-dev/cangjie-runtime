# Frozen Apple strip product batch (#1481)

The product workflow is prepared, not activated by the implementation lane.
Only control may activate the unique `p1481-<full head>` label on PR1482 after
checking the full frozen head, inputs, and recipe. The label is 46 characters.
Never activate the old `pac1481-` compile probe, dispatch, or rerun this batch.
The product workflow checks the PR head rather than a merge ref, and attempt 1.
The pending activation HEAD must not contain GitHub commit skip directives or
a `skip-checks` trailer: they can suppress this `pull_request` label workflow.
Freeze the complete candidate HEAD after this prerequisite, then bind its full
40-digit hash to the control-only product label. A successful label API response
is not evidence that a product run started. A new control-bound absolute deadline and
first-error stop rules must precede activation; a suppressed event grants no extra batch or
time allowance. Preserve the suppressed HEAD and its label as historical evidence.

## Fixed producer → consumer plan

| Source | Value / boundary | Consumer |
| --- | --- | --- |
| consumer.cpp: linker relocation of `unwindPCForN2CStub` | independent owned raw code address | producer.S: `pac1481_produce` |
| consumer.cpp: linker relocation of `pac1481_outside` | independent non-owned fixture code address | same producer, once |
| producer.S: `hint #0x8` (PACIA1716) | IA key, modifier 0; raw recorded before signing; x30/SP untouched | separate consumer.cpp TU |
| consumer.cpp: `MachineFrame(nullptr, pc)` | real PC field, no mocked lookup | complete runtime `MachineFrame::IsN2CStubFrame` |
| MachineFrame.cpp:41 | strip before the two real marker comparisons | MemUtils.cpp Apple XPACLRI |

The signed symbol address is an instruction-address observation input. It is not
an executed N2C frame. The producer never branches to a signed PC, never invokes
authentication, and does not alter runtime state. PACIA1716 is encoded as a hint
for ordinary arm64, as XPACLRI is; CPU/OS effectiveness still needs runtime evidence.

## Fixed recipe awaiting control activation

Use macos-15 ordinary arm64, Xcode16.4 and its recorded actual macOS SDK, and the complete
backward-PAC runtime, with the exact runtime compile definitions and generated
include paths for this C++ TU. Compile producer.S separately, without LTO. Link
consumer.cpp and producer.o against the complete runtime/bounds dylibs; no
product source copies or symbol interposition. Capture linker commands, object
hashes/disassembly, runtime consumer and marker exports, fixture relocations,
loader mappings and both dylib/dependency hashes. Missing symbols or product EH
build failures are setup failures; do not patch those paths or disable PAC.

Preselected inputs: the single N2C marker and the single outside fixture symbol,
IA/zero for each, one production per input per run. No random modifiers or retries.
`ASSERT_EXECUTED` prints both product results and all input integers. rc=20 is
setup failure; rc=1 is target/control failure; rc=21 means the owned signed PC is
indistinguishable from raw and cut sensitivity is NOT_RUN. rc=0 alone is not PAC
qualification: disassembly, executed instruction and image identity evidence
remain required. A raw/control failure invalidates attribution of a cut run.

`run.py` configures two independent full product trees concurrently, using the
native `runtime/build.py` CMake platform recipe with explicit PAC-off/PAC-on,
`BUILD_CJTHREAD=ON`, product C++ sccache, and real ncpu parallel builds. CJThread
uses the existing platform configure entry and its own ncpu build; its compiler
cache recipe is recorded rather than assumed to use the parent launcher.
The default layout is only a build control. PAC-on consumer definitions, includes
and ABI flags come from the actual MachineFrame compile command. No product
implementation is compiled into the fixture. A producer object and consumer TU
are separately built without LTO, then linked once against both full dylibs.
The two cast prerequisites remain in all trees; a failed build is attributed to
the complete candidate, never strip alone. Any first error stops later stages.
Both already-started independent builds finish and retain their first errors.

The batch records compile/cache/link commands, rc and wall, source/object/dylib/
ELF hashes, real exports, relocations, product and producer disassembly, mappings
and readable dependency hashes. Libraries resident only in dyld shared cache are
explicitly recorded as such, with OS/SDK identity, never given invented hashes.
No missing export is repaired. Before behavior, the product opcode and caller LR
save/restore evidence must pass the fixed instruction checks. Each execution is
bounded by 120 seconds or the remaining batch deadline, whichever is less. Normal, cut and restored each use the same executable
and producer object; the only changed loaded artifact is the runtime dylib.

Single frozen cut in `consumer-strip.diff` (not applied to this candidate): in the baseline-existing
`runtime/src/UnwindStack/MachineFrame.cpp:41`, replace only
`PtrauthStripInstPointer(reinterpret_cast<Uptr>(ip))` with
`reinterpret_cast<Uptr>(ip)`. Keep both comparisons and every other consumer.
For a distinguishable owned sample, only its signed-result assertion should fail;
owned raw and outside raw/signed must retain their results. Keep the fixture ELF
and producer object byte-identical for green/cut/restored. Restore the original
green dylibs by copying retained files, then execute the same fixed input once.
`run.py` builds that third full PAC-on product only after the green identity,
controls and distinguishable owned signed value pass. It keeps the green bounds
dependency, checks only the signed owned result is red, copies retained green
dylibs for restoration, and never resamples a nondistinguishable input. Independent
processes may change raw addresses due to ASLR; the invariant columns, IA/0 scheme
and relocated symbol inputs remain fixed. Numeric addresses are not equalized.
Execution and generated-code validation remain NOT_RUN until activation.

## Preparation only: runner inputs still missing

The old manifest-registration deadline 2026-10-02T10:37:17Z and failed run
36992795432 remain historical evidence. PLAN.deadline_utc is null: this recipe
cannot execute a new batch until control binds a candidate, absolute deadline
and unique event. The existing workflow's old deadline is not a new authorization.
No push, label, dispatch, GHA, compiler/cache, configure or native execution is
part of this preparation. The preparation allowance is 45 minutes from its run
created_at; it grants no product allowance.

Before future execution, the runner must supply SCCACHE_PATH from that run's
sccache-action output and PAC1481_LLVM_BIN from the actual installed LLVM
collection (e.g. that runner's brew --prefix llvm, never a frozen Homebrew path).
recipe.bind_tools resolves, stats, checks executability and hashes each actual
binary separately. Archive digests are not binary digests. It binds llvm-nm,
llvm-objdump and llvm-readobj individually, rejects a missing member, and does
not search alternative versions. Compiler entities retain current PATH origins;
Xcode16.4, SDK and ordinary arm64 checks remain required.

Top configure passes the same resolved sccache to CANGJIE_COMPILER_CACHE:FILEPATH
and both C/CXX launchers. Before either fresh configure, its inherited process
environment sets CMAKE_C_COMPILER_LAUNCHER and CMAKE_CXX_COMPILER_LAUNCHER.
CMake >=3.17 initializes these variables in the fresh CJThread child. The actual
Apple call runtime/CMakeLists.txt:280 and build/build_cjthread.sh:66 do not
replace these variables; the child directory is freshly recreated. No ASM
launcher requirement is added. Input argv/environment is saved before configure.
After configure, top and child Cache plus generated commands must consume the
frozen entity; cache statistics bracket configure (which already compiles the
child). Setting environment alone proves no real consumption. Missing or
mismatching evidence stops before top build without repair/reconfigure.

Finally, each arm separately copies formed compile/link/response/flags inputs,
child generated commands/cache, key actual TUs, formed objects and logs to an
arm-owned retained directory, verifies hashes and records absent outputs as
MISSING before deleting its private source tree. Copy/hash failure retains the
original tree and stops. Existing keep directories are never removed. The 120s
archive reserve remains; no full SDK/source/build copy is required.

LR validation is a bounded straight-line text/dataflow check of the actual
caller and bound callee, not a complete CFG safety proof. Unsupported branch,
return or instruction syntax is INVALID and behavior stays NOT_RUN. Complete
function blocks are retained before checking. An unsupported first actual shape
must go to independent assessment; do not extend formats and rerun to obtain green.
Saved probe assembly is historical input, not full-product caller qualification.

## Holds

No claim of managed/EH PAC support, full ABI pairing, iOS/OHOS/Android/Linux
qualification, #1478 A2, whole/A1/#135 or merge admission follows from this fixture.
Product auth/sign and EH/stub return schemes are outside this change.
