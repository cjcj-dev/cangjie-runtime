# Source-only Apple strip fixture (#1481)

No execution admission is attached to these files. Do not connect this fixture to
CI, activate the old probe label, or build/run it until the controller binds a
fixed candidate/toolchain/recipe and a separate execution budget.

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

## Pending qualified recipe

Use the approved ordinary arm64-apple-macos12 compiler/SDK and the complete
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

Single proposed cut (not applied or built): in the baseline-existing
`runtime/src/UnwindStack/MachineFrame.cpp:41`, replace only
`PtrauthStripInstPointer(reinterpret_cast<Uptr>(ip))` with
`reinterpret_cast<Uptr>(ip)`. Keep both comparisons and every other consumer.
For a distinguishable owned sample, only its signed-result assertion should fail;
owned raw and outside raw/signed must retain their results. Keep the fixture ELF
and producer object byte-identical for green/cut/restored. Restore the original
green dylibs by copying retained files, then execute the same fixed input once.
All of this, including entry_cut_check and disassembly validation of the compiler's
x30 save/restore, is NOT_RUN in the source stage.

## Holds

No claim of managed/EH PAC support, full ABI pairing, iOS/OHOS/Android/Linux
qualification, #1478 A2, whole/A1/#135 or merge admission follows from this fixture.
Product auth/sign and EH/stub return schemes are outside this change.
