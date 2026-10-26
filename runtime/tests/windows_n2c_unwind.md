# Windows N2C unwind consumers

The production object is `src/CMakeFiles/cangjie-runtime.dir/arch/x86_64_windows/N2CStub.S.obj` from the normal MINGW64 CMake target. Preserve the actual compiler command, response file, object and tool versions. Do not substitute the static-library recipe for an executed dynamic-library build.

Run the bounded record consumer against that object:

```text
llvm-readobj --unwind N2CStub.S.obj > unwind.txt
llvm-objdump -d --no-show-raw-insn N2CStub.S.obj > instructions.txt
node runtime/tests/windows_n2c_unwind.mjs N2CStub.S.obj unwind.txt instructions.txt
```

It decodes the actual COFF symbol positions and unwind records, follows the disassembled prologue instruction effects, and checks caller registers/PC/SP at each prologue boundary and with four dynamic stack depths. It also checks the actual return instruction sequence, private N2C 16-byte cleanup, preserved result registers, and ExecuteCangjieStub's argument slots against native home-space writes. The `control` mode consumes the separately built production Cjstart object. This model is deliberately limited: it does not execute Windows instructions or the Windows OS unwinder.

The independent native OS consumer uses the existing public N2C and C2N exports; it adds no runtime export. After a real Windows runtime DLL and dependencies exist, build it with the normal MINGW64 compiler, then pass the absolute installed DLL path:

```text
clang++ -std=c++14 runtime/tests/windows_n2c_os_unwind.cpp -lntdll -o windows_n2c_os_unwind.exe
windows_n2c_os_unwind.exe C:/absolute/install/runtime/lib/windows_x86_64_cjnative/libcangjie-runtime.dll
```

Use the actual install manifest to resolve the DLL spelling/location. Do not replace a failed LoadLibrary result with a source/model pass. This consumer calls RtlVirtualUnwind on the loaded production entry for four body stack depths; it does not invoke a managed method or qualify language exceptions. ApplyCangjieMethodStub/ExecuteCangjieStub are internal symbols and are not added to the export contract for testing.

The managed RBP anchor, saved-register slots and N2C slot data are unchanged. RSI supplies the Windows fixed frame base and is saved by an actual PUSH before allocation or SET_FPREG. Three redundant RBP pushes reserve the unchanged context words at RBP-8/-16/-24; their values may be overwritten by the managed context, because the first RBP push restores the real caller RBP last. RSI's push slot is the previously reserved RBP-32 word, and its original managed save slot at RBP-112 is also populated. The consumer overwrites all three context words to test this distinction.

All PUSH_NONVOL codes precede other unwind operations, and all offset-based SAVE codes follow SET_FPREG. These public encoding requirements are checked separately from the model's register recovery. The rejected fabd candidate violated the second rule even though its partial-prologue RSP fallback recovered the values in the model; that behavior is not a rule exemption.

The epilogue is one frame-relative LEA followed only by POPs and RET. The three context words are discarded into volatile RCX, preserving the managed RBP anchor until its final pop. Reverse unwind order restores RSP from RSI before restoring the original RSI. Review must check partial prologues and the epilogue, not just successful assembly. The normal private callee cleanup remains 16 bytes; body virtual unwind and normal RET cleanup are separate assertions. The OS consumer still covers only body states; partial-prologue/epilogue OS execution and actual language transitions remain separate obligations.

The fixed allocation also reserves 32 bytes below all saves for native helper home space. ExecuteCangjieStub has its own fixed home/argument area. Neither loop iterations nor outgoing shadow-space pushes are recorded as one-time prologue register saves.

References: [Microsoft x64 unwind encoding](https://learn.microsoft.com/en-us/cpp/build/exception-handling-x64), and [fixed Microsoft .NET unwind implementation](https://github.com/dotnet/runtime/blob/421be955e4b70cddf583b10f5ad99814b713fb87/src/coreclr/unwinder/amd64/unwinder.cpp) (SET_FPREG and SAVE_NONVOL/XMM processing). The bounded test uses these operations; it is not a substitute for the native OS consumer.

The original 517a evidence covers N2C only. The successor closure below extends source and bounded consumers to the other selected assembly; it does not retroactively extend that earlier evidence. Official runtime link/export collection/install/package and managed exception consumers remain separate gates.

## Selected Windows assembly closure

The historical compilation database from run 40242564639 selected nine runtime
assembly sources, Cjstart, and the Exception archive's RestoreContextForEH
(11 source files; 20 dynamic/static compile commands). That is the old run's
closure, not the current source selection. The Windows Exception target now
selects `WindowsExceptionTransfer.cpp`; its `RestoreContextForEH.S` was retired.
Derive the current closure from CMake target sources and the next actual
compilation database, including the CalleeSavedStub macro expansions. Do not
replace it with a hand-picked list of transition names or claim that the new
official compilation database has already been produced.
The `closure` mode consumes each resulting actual COFF object and checks
encoding order, completed fixed-frame bias, every prologue boundary, and
nonvolatile saves under native home-space writes. `c2n` additionally checks
normal/guard shared restore, RSI frame recovery, refreshed managed r15,
return registers, and the private 16-byte cleanup. This remains a model.
The `callee` mode also executes every emitted CalleeSaved macro body's decoded
instructions around three modeled native calls (the wrapped function, TLS
refresh and Complete bridge), including the five-argument variant, native home
writes, preserved result and refreshed TLS register.
The `exclusive` mode follows ExecuteExclusiveCangjieStub's actual argument,
N2CSlotData and result slots through its three call sites. It separately checks
the Complete bridge and register restoration in the outer
`CJ_MCC_ExclusiveScope`; the inner Execute helper has no such bridge obligation.
The modeled calls exercise their ABI effects; they do not execute the helpers
or managed closure.

C2N now uses real GPR pushes before allocation, RSI as its fixed Windows FP,
and offset-based XMM records after FP establishment. The original RBP anchor,
GPR/XMM slots, safe-state slot and incoming argument locations remain. The
additional fixed 32-byte home area is distinct from outgoing copied arguments.
Loop/shadow allocations are body state recovered through that FP, not repeated
prologue push records. Its epilogue preserves the caller frame and uses RET16.

The fixed-frame helpers keep their managed RBP slot layouts. Safepoint and
StackGrow include native home space in the recorded fixed allocation and do
not publish an unrepresentable Windows RBP offset. CalleeSaved macro results
no longer overwrite the saved r15 slot; five-argument wrappers retain RSP+32
argument placement. Exclusive N2C helpers reserve the actual 24-byte N2CSlotData
before saved registers, and keep result slots outside native home space.

## Windows exception return and transfer consumers

The retired Windows RestoreContextForEH carrier and its empty SEH record do
not describe the current production mechanism. The Windows branch of
`ExceptionHandling::ChangeEHStackInfoLR` records the actual boundary frame
without rewriting its return address. Native callees return normally, allowing
C++ RAII cleanup to finish. Before the boundary epilogue, the production stub
calls `MRT_CompleteWindowsExceptionReturn` with that frame and its private
cleanup size (16 for C2N, zero for the other boundaries).

`WindowsExceptionTransfer.cpp` publishes the live unwind context, starts stack
watermark processing and captures the native context. The return helper uses
the real Windows ControlPc and unwind records, accounting separately for C2N
RET16. Boundary classification uses actual CALL-return PCs within established
save-area intervals; GC root traversal consumes the corresponding real saved
register slots. The existing `MRT_RestoreContext` applies managed stackmap and
watermark processing before `MRT_TransferWindowsException` transfers to the
landing pad through `RtlRestoreContext`. Finite COFF decoding does not execute
this complete production chain or qualify managed cleanup.

`windows_n2c_os_unwind.cpp` enumerates both public N2C and C2N exports in the
actual installed DLL for four body depths each. It still requires the real
Windows producer and does not qualify partial prologues, epilogues, managed
exceptions, stack migration, or exclusive stack switching.

Two further native consumer groups cover distinct parts of the new mechanism:

- `windows_exception_return_os.cpp` consumes the actual runtime return/transfer
  exports for C2N RET16, C2R, overflow and exclusive boundaries, using the product
  `CalleeSavedRegisterContext.h` include closure.
- `windows_exception_frames.cpp` with `gc_unit/test_unwind_regressions.cpp`
  runs `WindowsExceptionBoundaryRealReturns`, `WindowsExceptionSaveSlots` and
  `WindowsC2NPrivateCallerSP`. Build with the actual runtime/bounds import
  libraries and headers, `-std=gnu++17 -fexceptions -fno-rtti`. Its imported
  `windowsC2NBodyEnd` must match GetProcAddress on the explicitly selected DLL.

Run all three native groups against the normal independent install and again
against the same-install package unpack, with dependencies from the matching
location. Preserve each compile, link and consumer exit status. These installed
and unpacked executions remain NOT_RUN; existing finite compilation/model
evidence does not qualify them. First legal DLL production is required before
these executions; an export postgate rejection must preserve the link products
without claiming successful install or packaging.

Managed cleanup and reflection remain separate NOT_RUN obligations:
`windows_exception_native.cpp`, `windows_exception_transfer.cj` and
`reflection_constructor_product.cj` require qualified fixed Windows C/N/std
and the actual runtime DLL. Neither the native consumers nor an available
Linux compiler/old std substitutes for that tuple. Full raw/caller export
collection, five Reference services, ordinal/DATA compatibility and retired
ordinal 277 remaining unused also remain required production gates.
