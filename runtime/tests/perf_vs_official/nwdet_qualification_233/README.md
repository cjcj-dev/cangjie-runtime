# NW qualification attempt for #233

The rebuilt input failed qualification on frozen runtime `dea244c0918ae4b874d4490e12dc97268b78371a`. This proposal keeps `qualified=false` and replaces the historical incompatibility explanation with the observed first fatal check. It does not restore the merge gate or approve this ELF.

`qualification.patch` targets the ops repository's `state/nwdet_qualification.json`; it is a review artifact, not an automatically applied runtime setting. The original file digest and proposed digest are in `manifest.json`. Shared canonical, SDK and qualification files were not deployed by this lane. The original `since` and `workloads` fields are preserved.

The controller selected official nightly `1.3.0-alpha.20260831010012` cjc and the frozen runtime pair after the prior r3 SDK paths were found missing. The private SDK copied the official target stdlib; compiling the workload does not establish a coloured closure for that archive. `-O2 --static-std --save-temps <dir> -V --link-option -Map=<map> --link-option -t` produced the ELF; its exact command, link map and input hashes are in the evidence archive. The compiler host used the official runtime, while target linking used the freshly built frozen runtime pair.

The unchanged `gate_nwdet.py` was run with `N=3`, `CORES=16-31`, `TMO=300`, the new content-addressed ELF, a private execution permit and the private `build/deferred-sodepot`. The first 256MB sample returned SIGABRT (`-6` as reported by subprocess); the gate returned 1. `ColourCensus.cpp:26` rejected heap slots and identified `std.core:OutOfMemoryError`. A's remaining samples and all of B were stopped per the predeclared failure rule. This is one failed sample, not a measured failure rate or a root-cause proof. The controller assigned the follow-up to cangjie-runtime#190; no runtime repair is included.

`gate-controls.json` records independent fixture execution through the unchanged gate: correct → wave8 checksum error → restored returned 0 → 1 → 0. Missing/duplicate records, missing completion, abnormal exit and missing heap count were also rejected. These fixtures prove the gate only, not NW product qualification. No gate source or runtime source was changed, so no new product disconnection claim is made.

The raw archive contains the runtime/bounds pair, build log, original workload stderr, gate result, compiler link map, identities, recipes and both-host measurement context (local context is adjacent to the archive). Review must inspect those originals; this directory is an index and proposed qualification update.

Reference obligation: ZGC `test/hotspot/jtreg/gc/z/TestSmallHeap.java:43-66` requires successful process completion. NW did not satisfy the corresponding existing gate requirement. Product check: `runtime/src/Heap/Verify/ColourCensus.cpp:24-34`, reached through `VerifyHeap.cpp:389` and `ColourCensus.cpp:99`.
