# Constructor registration fixture (NOT_RUN)

Build only with a complete matching compiler/std/runtime/LLVM install, through the
admitted box + wf slot. `build.mjs SDK OUTPUT` records each compiler's own status;
it does not run the executable or qualify the tuple. Run the ELF with the exact
candidate runtime and observer directories on its library path and preserve both
SO identities. This is a real language consumer, not a hand call to RegisterFinalizer.

The normal object and the object whose derived constructor fails after Object.init
must each finalize once. The object whose super-argument fails before Object.init
must never be registered. Native observer output and process rc are both required.

The existing Grok qualification owner additionally owns young-mark observation and
native debugger gates. Checkpoints 0–3 bracket real source constructors; use actual
collector phase control and production registration, preserving the independent
ordinary-construction/finalization control. Observe the target young object's mark
state and registration's mark-stack effects at the actual bridge, accounting
separately for the allocated FinalReference carrier and exception objects. Merely
holding all constructor allocations across two checkpoints is not an isolated
registration measurement. No young-mark result or debugger execution is claimed.

Required product cuts: suppress the real root-return registration call (normal and
after-root targets turn red, before-root control remains zero); restore; separately
reintroduce the removed registration-time MarkObjectIfActive call at the real
OnFinalizerCreated bridge only in the admitted runtime copy and prove the exact
young-registration invariant. Do not add a test-only registration export.
Keep prior 1468 missing-library and 1472 timeout evidence separate from this input.
