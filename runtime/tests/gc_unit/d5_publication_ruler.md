# D-5 publication prerequisite ruler

Current coordinate: `b3f7f136c1712cc48b30dadb3baf461f496ed0be`.
The prerequisite correction was first investigated at
`f65c773dea5e198592d1bce4624525e560fc918e`; remeasure on this coordinate
after the independent MarkStack cleanup fix (#200).
This independent ruler investigates
`YoungConc.TraceRefFieldRemapsLoadGoodFromBeforeStoreGood`. It does not
reinstate the historical six-test composite ruler or inherit D-4's results.

Build the product-linked CMake `cj_gc_unit` on kkk2, starting in the runtime
source directory (the cjthread configure step uses relative paths):

```sh
ulimit -c 0
cmake -S . -B "$BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCOPYGC_FLAG=1 -DDOPRA_FLAG=1 -DRUNTIME_TRACE_FLAG=1 \
  -DDISABLE_VERSION_CHECK=1 -DCJ_SDK_VERSION=1.0.0 \
  -DMRT_GC_UNIT_TESTS=ON -DMRT_TESTABLE_INTERNALS=ON
GC_UNIT_GATE_SKIP=1 cmake --build "$BUILD" -j8
```

The skip only defers the post-build test gate. Save configure/build exit
statuses. Preserve the executable, runtime SO and boundscheck SO before
running; record SHA-256, runtime provenance, configuration, build time,
leased CPU set and both hosts' uptime. Point `LD_LIBRARY_PATH` at the
preserved artifacts. Enumerate `--gtest_list_tests`, then select the exact
target with `--gtest_filter=YoungConc.TraceRefFieldRemapsLoadGoodFromBeforeStoreGood`.
A nonexistent exact filter must fail, while a registered control must run.

Run the target three times per arm with the same recipe. Save RUN records,
the first failed assertion, completion tally and exit status separately.
Distinguish fixture publication failure from the later field-remap result.
Static call-chain evidence alone does not establish either runtime outcome.

If a fixture correction is justified, preserve the original test name and
assertions. Record the baseline/candidate failure-name set difference.
For product sensitivity, keep the executable and boundscheck identical
while changing only the product SO. Cut the baseline product producer and
consumer paths separately, verify the intended assertion is reached, then
restore byte-identical artifacts. A compiler, loader or earlier fixture
failure is not evidence for the remap assertion. Manual receipt insertion
does not prove the real copying entry was traversed.

Default-product builds use both testability options OFF and the same build
entry. Compare defined SO symbol sets before/after a fixture change; do not
infer product behavior from the test-only configuration. Any product defect
found after the prerequisite is restored requires a separate scope decision.
