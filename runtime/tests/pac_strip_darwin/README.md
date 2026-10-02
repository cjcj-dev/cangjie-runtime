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
is not evidence that a product run started. The original absolute deadline and
first-error stop rules still apply; a suppressed event grants no extra batch or
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

The independent manifest-registration batch deadline is 2026-10-02T10:37:17Z, including preparation,
control wait, queue time and archive. PLAN and workflow environment bind it;
the driver reserves 120 seconds for archive and never opens a new time allowance.
Each stage uses its original timeout capped by remaining time. Zero/negative
remaining records NOT_RUN. The workflow's timeout is an additional ceiling,
not permission to cross that absolute deadline.

## Holds

No claim of managed/EH PAC support, full ABI pairing, iOS/OHOS/Android/Linux
qualification, #1478 A2, whole/A1/#135 or merge admission follows from this fixture.
Product auth/sign and EH/stub return schemes are outside this change.
