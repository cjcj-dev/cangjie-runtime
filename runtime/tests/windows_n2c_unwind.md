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

The normal CMake compilation database selects nine runtime assembly sources,
Cjstart, and the Exception archive's RestoreContextForEH (11 source files;
20 dynamic/static compile commands in run 40242564639). Derive this set from
that database and CMake target sources, including the CalleeSavedStub macro
expansions; do not replace it with a hand-picked list of transition names.
The `closure` mode consumes each resulting actual COFF object and checks
encoding order, completed fixed-frame bias, every prologue boundary, and
nonvolatile saves under native home-space writes. `c2n` additionally checks
normal/guard shared restore, RSI frame recovery, refreshed managed r15,
return registers, and the private 16-byte cleanup. This remains a model.
The `callee` mode also executes every emitted CalleeSaved macro body's decoded
instructions around two modeled native calls, including the five-argument
variant, native home writes, preserved result and refreshed TLS register.
The `exclusive` mode follows ExecuteExclusiveCangjieStub's actual argument,
N2CSlotData and result slots through its three call sites. The modeled calls
exercise their ABI effects; they do not execute the helpers or managed closure.

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

RestoreContextForEH is entered by a rewritten managed return, without an
ordinary native caller PC on entry RSP (Exception.cpp redirects the last
runtime frame). Its empty existing SEH encoding is enumerated separately.
The normal-call model explicitly does not qualify that transfer trampoline;
its landing-pad/OS semantics are NOT_RUN and must not be counted as a
normal-frame pass. No native exception handler or synthetic return convention
is invented by this consumer.

The OS C++ consumer now enumerates both public N2C and C2N exports in the
actual installed DLL for four body depths each. It still requires the real
Windows producer and does not qualify partial prologues, epilogues, managed
exceptions, stack migration, or exclusive stack switching.
