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

On supported glibc systems, each native metadata registration pins the actual
image with `RTLD_NODELETE` in its own namespace before publishing its address
map. The runtime verifies the native handle's object and namespace identity and
checks pin failures. Direct `dlclose` can return successfully, but cannot unmap
the pinned image or invoke its finalizers. Native libraries without Cangjie
metadata are unaffected. An unavailable native pin API fails closed in this mode.

Runtime finalization leaves image metadata resident and does not re-enable
unloading. Process exit still invokes native finalizers. The fini callback
accepts completed runtime finalization or the registered image's verified pin,
so never-initialized and initialized normal exit preserve metadata as well.
An unknown/unretained image before completed finalization fails the CHECK.
The unload writer retains its separate CHECK.

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
image persistence against the actual installed SO. Compiler macro and fixed workload
results must be recorded separately; a harness or `--help` result is not compiler
qualification or a performance comparison.

`readscope_pinned_image_harness.cpp` checks actual handle/map/namespace identity,
metadata access and managed allocation after closing all observation handles,
including a secondary namespace with a base-namespace decoy. Pin failure is
injected at the native API. The first-load harness also checks initialized exit,
metadata reads under ReadScope, and two-image native finalization in both load
orders. Results and native fini traces must be recorded against the installed SO.
