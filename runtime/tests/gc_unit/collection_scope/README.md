# Collection scope statistics

ZGC anchors: zGeneration.cpp:69-76,379-391,514-540,938,993-1019,1225,1405.
`runtime_scope.cpp` initializes the real runtime, seeds a preceding accounting
interval through `increase_freed`, then issues synchronous USER, BACKUP and
YOUNG requests through `Heap::RequestGC`. It does not reconstruct collector phases.

The read-only GDB observer stops at product generation timer publication and
reads the registered sampler's per-CPU sample count before and after sampling.
It compares its address with the type-indexed generation sampler, including both
old collections. A hardware watchpoint reads the actual old reset store and its
VM-operation stack. Cycle end checks read the actual cycle start/end record.
All target assertions print their inputs on both pass and failure.

Compile the fixture once on kkk2, with C++17, `-g -O0 -fno-rtti
-fvisibility-inlines-hidden`, the same include roots as run_standalone.sh, and
link only product libcangjie-runtime/libboundscheck. Pass that ELF unchanged to
`run.sh` with SCOPE_ELF, SCOPE_OUT, SCOPE_CPUSET and GCV2_RUNTIME_LIB_DIR.
The runner executes three fresh processes concurrently, records full nm and
ELF/SO hashes, exit codes, CPU affinity and before/after uptime.

Independent product cuts:

- Route young scope to timer index 0: only type-to-sample assertions fail for
  full-preclean, full-roots and partial-roots; minor and old are controls.
- Remove generation RegisterEnd sampling: only sample-delta assertions fail.
- Move old reset_statistics from mark_start to old at_collection_start: only
  the reset-in-mark-pause assertion fails; all timer and cycle assertions run.
- Remove collection-start AtStart or collection-end AtEnd separately: cycle
  lifecycle assertions fail; timer and reset assertions remain observable.

A missing symbol, premature exit, debugger error or timeout is an observer
failure, never a product regression result. The observer requires the product's
existing debug info and Linux hardware watchpoints. No product result is written.
