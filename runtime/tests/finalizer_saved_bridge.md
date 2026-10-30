# Finalizer callback saved-register boundary

`Finalizer.runFinalizer` calls `CJ_MCC_ReferenceInvokeFinalize` as a managed
runtime call. `FinalizerProcessor::InvokeFinalize` then calls a managed finalizer
through `ExecuteCangjieStub`. The native implementation's register saves are not
managed stack maps. On Linux, the public entry must therefore be inside the
existing `unwindPCForC2RStubStart/End` range and use `CalleeSavedRegistersStub`.
The native implementation is `MCC_ReferenceInvokeFinalize`, as on Windows.

The walker may clear `RegSlotsMap` while traversing intervening runtime frames.
On reaching the public bridge it must classify `C2R_STUB`, then reconstruct
callee-saved slots with `RegRoot::RecordStubCalleeSaved` before consuming the
managed caller's stack-pointer map. On x86-64 the R14 slot is bridge FA - 16;
the normal bridge epilogue reloads that same slot. No native prologue decoding,
missing-root substitution, or change to Reference reachability is involved.

This is a Cangjie managed/native transition, not a new ZGC reference-processing
phase. `zReferenceProcessor.cpp` pending publication and finalization reachability
remain unchanged; HotSpot's equivalent reference enqueue protocol is in
`src/hotspot/share/gc/z/zReferenceProcessor.cpp:484-517`. The save-map responsibility
is the existing platform bridge/stack walker contract.

## Required consumer (owned by the compiler fixture lane)

Use the normal Finalizer service and a real managed class finalizer. While that
callback is active, use the existing native reader via a normal C2N call to walk
the current mutator stack. Require the actual `runFinalizer` caller descriptor's
R14 stack-pointer use, a preceding recognized C2R bridge, and a recorded saved
slot. The producer's method PC/map must be checked from its own emitted metadata;
no fixed address from an older core SO is an execution predicate.

Compare frozen ee395 with the candidate using the same legal consumer. An absent
finalizer callback or a walk that stops before the parent is NOT_OBSERVED. Check
normal completion and exception cleanup separately when the consumer supports
them. Do not substitute a synthetic stack, sleep's null `startProc`, arbitrary
native C++ register preservation, or a success marker for the parent map check.
The original stdx failure's caller remains unproven; this regression does not
identify it by analogy.

Linux x86-64 is the initial native build/installed consumer scope. Linux AArch64
and ARM select their existing CalleeSavedStub macros under the same native symbol
split; their source coverage does not grant cross-platform execution. Darwin's
existing direct entry and Windows's existing bridge are not changed here.
