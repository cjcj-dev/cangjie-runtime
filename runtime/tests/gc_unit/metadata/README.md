# Platform managed-metadata checks

This target links `test_empty_stackmap.cpp` to the product shared runtime. It
uses the existing test registry without the full GC heap fixture. No product
translation unit is compiled into the test executable.

`.github/workflows/metadata-platform.yml` uses public standard runners:
`ubuntu-24.04-arm`, `windows-2025`, and `macos-15`. Pushes are limited to
`sym/1284-implement-*`; dispatch runs the selected ref. Each C++ job configures
sccache and records its compiler, image, CPU count, commands, exit codes, wall
time, and linked hashes. Core dumps are disabled.

The prepare jobs build default and testable products separately. Testable
builds produce a single executable and a bundle of its original libraries.
Every behavioral arm downloads that bundle. The baseline builds the frozen
product (`575e37b5f271065e08eb401a38fe60976f71a925`) and runs the same new tests;
this does not claim the old suite contained those tests. Restored and CHECK
arms build in the same path and with the same recipe as prepare. Restored
runtime bytes must equal candidate bytes; a CHECK-cut library must differ.
Only the runtime library is replaced. All other bundle hashes, including
boundscheck and the executable, must remain identical. The executable reports
the actual loaded runtime path and the supervisor checks it against the bundle.

| Platform | Cases | Single CHECK cuts |
| --- | --- | --- |
| Linux AArch64 | CallerSpAbsentStackMap, CallerSpNative, CallerSpPresent | `FrameInfo::CallerSP` map check |
| Windows x64 | WinCurrentAbsentDescriptor, WinCurrentAbsentStackMap, WinCurrentPresent, WinCallerAbsentDescriptor, WinCallerAbsentStackMap, WinCallerPresent | Each descriptor/map check in `GetCurFrameInfo` / `GetCallerFrameInfo` |

All names belong to `ManagedMetadata`. Each product cut must fail exactly its
one named negative case; the two positive cases must still pass. The positive
cases compare product-returned addresses. Windows inputs are real PE functions
with `.seh_proc` unwind records and compiler-format descriptor offsets; the
product discovers their module through `WinModuleManager::Init` and
`ElfUnloadQuiescence::LinkImage`. They are never executed as managed code.
Imports, unwind records, and disassembly are retained for fixture inspection.

A negative case requires its start marker, exact diagnostic PC/IP values, and
abort status. Windows abort status is calibrated using the same executable and
CRT. An access exception, a loader failure, a timeout, or a downstream check
cannot substitute for the targeted check. The preflight records missing
executables, unknown filters, timeouts and non-target exceptions. The device
cut redirects the actual spawn arguments to an unknown filter, while leaving
the abort calibration intact. Its expected failures are all selected cases;
this proves the supervisor wiring only, not product behavior.

PAC OFF/ON jobs are capability probes. Their logs and build return codes are
kept even on failure; neither ordinary macOS build success nor workflow success
qualifies PAC metadata behavior. PAC ON currently encounters the product
`EhFrameInfo.h:58` integer cast error (#1287), before the remaining Darwin PAC
metadata/ABI prerequisites can be verified. PAC death cases remain NOT_RUN.

The frame-processing reference is HotSpot `zStackWatermark.cpp:209–214` and
platform `compiled_frame_details` in `frame_aarch64.cpp:814–825` /
`frame_x86.cpp:669–672`. Cangjie compiler descriptors and Windows PE unwind
records are platform inputs, not copies of the HotSpot frame layout. This
package changes tests and CI only, and makes no GC phase or performance claim.
