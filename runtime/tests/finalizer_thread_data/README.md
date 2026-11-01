# Finalizer registration thread data

Linux x86-64 consumer. Compile `native.c` as a shared library and `main.cj` with a
Cangjie SDK, linking that library. Run `main <threads> <rounds>`.

Creating a finalizable object calls `CJ_MCC_OnFinalizerCreated`, which runs the
managed registration through `ExecuteCangjieStub`. Under contention that managed
code parks in the registry mutex and can resume on another OS thread. The managed
caller must then see the current thread data in r15. `RequireCurrentThreadData`
reads r15 before any native call can change it and compares it with
`MRT_GetThreadLocalData`.

Exit 3 with `FINALIZER_THREAD_DATA_TARGET` is the target failure. Exit 100 or
`FINALIZER_THREAD_DATA_INPUT_MISSING` is a missing input, not a target red. A
SIGABRT from a spurious stack grow is the same defect reached first, and a clean
single-thread run is the control. A green multi-thread run on a build without the
stub does not show that the defect is absent, because migration is timing dependent.
