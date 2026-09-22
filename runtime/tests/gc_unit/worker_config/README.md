# Worker configuration regression

Build `runtime_workers.cpp` with the candidate `runtime/src/Cangjie.h` and link the
product runtime SO. On Linux, run under a 16-CPU affinity:

```sh
clang++ -std=c++17 -I runtime/src runtime/tests/gc_unit/worker_config/runtime_workers.cpp -L "$SO" -Wl,-rpath-link,"$SO" -lcangjie-runtime -o "$ELF"
taskset -c "$CPUS" python3 runtime/tests/gc_unit/worker_config/check_workers.py "$ELF" "$SO" "$OUT"
```

The 22 cases initialize the actual runtime through both `InitCJRuntime` and
`CJ_MRT_CjRuntimeInit`, then enumerate `/proc/self/task/*/comm` and compare against
product REPORT output. The fixture contains no worker-selection implementation.
Each process initializes once, without a GC workload; eight independent cases run
concurrently. A fixed one-second startup window precedes the single thread-name
snapshot because pthread creation precedes worker-side OS naming. Optional fourth argument: comma-separated case names.

The public GCParam fields `concGCThreads`, `youngGCThreads`, `oldGCThreads` use
zero for ergonomics, following the existing runtime parameter convention.
`staticGCThreads=false` is the dynamic default. Environment equivalents are
`cjConcGCThreads`, `cjYoungGCThreads`, `cjOldGCThreads` (positive decimal integers)
and `cjUseDynamicNumberOfGCThreads` (0 or 1, default 1).

ZGC anchors: zArguments.cpp:67-118 and zWorkers.cpp:45-65. Configuration must be
resolved before heap construction, including the shared relocation headroom.
