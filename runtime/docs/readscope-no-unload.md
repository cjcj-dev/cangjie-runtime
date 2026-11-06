# Process-lifetime ELF retention

On native Linux (excluding Android and OHOS), set `CJ_READSCOPE_FAST=1`
before starting the process to opt into process-lifetime retention of Cangjie
images. The first metadata-reader or loader query, including a pre-init native
image registration callback, freezes the choice for that
runtime image. Other values leave normal unload enabled. Changing the environment
later cannot enable or disable the mode. No public function signature, runtime
parameter layout, calling convention, or TLS model changes.

In this mode, nested ReadScopes still maintain their thread-local depth, while
outermost scopes bypass slot/counter admission and release. `AssertReaderActive`
still applies. No GC barriers or assertions are disabled.

`LoadCJLibrary` keeps its native library reference. Repeated loads are resolved by the native loader using the complete
requested path, and their native references remain retained. A basename match
does not bypass native path errors or select a different existing image. `UnloadCJLibrary` logically closes a known handle successfully
without unmapping its code, unregistering metadata, or releasing its native
reference. Null and unknown names retain their normal errors. Reopening finds
the retained image. Image replacement/reload is therefore unavailable until
process exit, and memory usage can grow with distinct loaded images.

Runtime finalization leaves the loader's image metadata resident. It does not
clear the mode or re-enable unloading. Native fini callbacks after completed
runtime finalization may return; callbacks before initialization or during a
live runtime fail the no-unload CHECK. The writer itself also retains a CHECK.
Direct `dlclose`/`FreeLibrary` of a Cangjie image during this mode violates the
contract; native libraries without Cangjie metadata are unaffected.

Compiler 1260 uses two existing macro paths: namespace handles have `retain`
set, and runtime-managed handles call `UnloadCJLibrary`. Both retain their images
under this contract; keeping only the runtime SO mapped would not suffice.
Each separately loaded runtime image selects its own immutable mode from the
startup environment. This is opt-in, not an automatic compiler/default policy.

The normal unload path continues to follow ZGC's unlink/rendezvous/purge order
(`zUnload.cpp:126-173`, `zGeneration.cpp:1330-1388`). ZGC's `ClassUnloading` option
skips unlink/purge when class unloading is disabled. Here ELF loading is owned by
the native loader: its handles must additionally remain referenced, and the
runtime's explicit close API must implement the retention contract.

`readscope_no_unload_harness.cpp` exercises public library load, symbol lookup,
logical close/reopen, runtime shutdown, immutable activation, and direct-close
rejection against the actual installed SO. Compiler macro and fixed workload
results must be recorded separately; a harness or `--help` result is not compiler
qualification or a performance comparison.
