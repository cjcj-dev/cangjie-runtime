# Signal/terminal integration fixtures — NOT_RUN

Build through `build_signal_shutdown.mjs` with the admitted candidate runtime library
and a complete matching compiler/LLVM/std install. The native executable exports
only test observers/callbacks; product services stay in the actual runtime SO.
Run each mode in its own process and empty lane-owned working directory. Preserve
command, process rc, compiler/ELF/SO identities and target lines. A timeout is a
failure/preserved debugger scene, never a passed late-return barrier.

- `managed-dump LIB`: R1 real compiled language task -> C2N signal sender -> SIGUSR1
  -> unchanged product SignalStack dispatcher/default handler -> typed VM_HeapDumper
  -> completed `item_data.dat` with product header -> Fini. The language task must
  continue to stage 41. Existing output is rejected, not reused or removed.
  Grok additionally uses the product writer observer to establish execution on the
  VMThread and not on the calling dispatcher; file output alone does not prove it.
- `managed-gc LIB`: R2 real managed handler allocates a reachable ArrayList and
  stays runnable; host calls public CJ_MRT_ForceFullGC (actual attach/service/exit),
  then language checks all contents. Require actual collection count advancement,
  bad=0, successful shutdown, and Grok's actual target-frame/pause observation.
- `managed LIB`: real compiled @C handler -> C2N ShutdownNativeBlock. After logical
  Fini the released native call must reach the terminal return barrier before
  managed stage 2. The fixture intentionally stays alive for Grok observation;
  only Grok's explicit assertions/termination protocol can qualify this arm.
- `blocked-vm` (legacy non-managed modes): called-side ScopedEnterSaferegion is
  SAFE only. It proves physical retention plus the saferegion return barrier,
  not `_thread_in_native`; do not relabel the old fixture as native qualification.
- `single-p`: historical hold/dispatch arm remains separate; its old timeout is
  retained. No replay is authorized merely by adding this source fixture.
- `reentry`: old extra signal delivery arm remains for explicit Grok adaptation;
  handler body counts must not be confused with true N2C admission.

`shutdown-owner-identity` separately exercises native OS identity reuse. It must
show the reuse precondition and actual VM exit barrier; its intentional waiting
is not rc0 evidence without the admitted observer. The permanent final pause
keeps backing code/metadata and needed thread closures, while Runtime::CurrentRef
must be null. Old scripts expecting the entire Runtime to survive are obsolete.

Required product cuts are owned by the admitted dynamic writer, never the live
source tree: sever the real default signal handler's DumpHeap dispatch (R1 must
fail its dump target while the independent managed allocation/GC control stays
reachable); sever the actual managed-frame safepoint/VM-service edge only under
the frozen contract, preserving the target before interpreting a red. Publication,
late-return, owner identity and lifetime cuts remain in the 1473 gate contract.
No debugger ran when this document was written.
