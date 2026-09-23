# Y2y mark-end regression ruler

This ruler replaces no historical result. Its initial source coordinate is
`b9cf4ab357ee4dd442e67d56d1636ce2a1863cc3`. It investigates D-4 independently
of the old six-test composite filter and the separate publication debt D-5.

## Definition

Use the product-linked CMake `cj_gc_unit` executable, with both
`MRT_GC_UNIT_TESTS=ON` and `MRT_TESTABLE_INTERNALS=ON`. Select each name in a
separate invocation with `--gtest_filter=<exact name>`:

| Role | Test |
| --- | --- |
| Target | `YoungConc.Y2yAfterReleaseBatchForcesContinueAndReachesClosure` |
| Concurrent consumption control | `YoungConc.Y2yDirtyVisibleBeforePauseMarkEnd` |
| Other producer control | `YoungConc.SatbAfterWorkerTerminationUsesBoundedMarkEndContinue` |

The target's product dispatch is `WCollector::DoGarbageCollection()` →
`DoYoungGarbageCollection()`. The pre-release holder/slot batch must be
consumed. Two later publications must reach the pause after concurrent workers
have terminated, force continuation, and eventually mark the holder's child.
The holderless slot's child must also be marked. The existing receipt and mark
bit assertions, not log tokens alone, decide the result.

Publishing immediately after mark-start release is too early to require a
failed mark-end: `MarkYoungSatbBuffer()` can consume that publication first.
The test-only publication therefore occurs after worker termination and before
the mark-end pause, once per iteration. Its budget remains two. This follows
OpenJDK `zGeneration.cpp:897-904`: continue only when mark-end is incomplete.
The separate concurrent-consumption control retains coverage of work published
before concurrent consumers finish.

## Instantiation and identity

Configure from the runtime directory in an isolated checkout on kkk2:

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

The skip defers the post-build gate; it is not a test pass. Run the registered
tests separately. Preserve `cj_gc_unit`, `libcangjie-runtime.so`, and
`libboundscheck.so` together before running them. Record their SHA-256 values,
the runtime `CJRT-COMMIT` stamp, compiler/configuration, build rc and timestamps,
the leased CPU set, and local/kkk2 uptime before and after each batch.
Set `LD_LIBRARY_PATH` to that preserved directory.

First enumerate `--gtest_list_tests` and require every selected name to be
registered. A deliberately nonexistent exact name must return nonzero with
the runner's no-match diagnostic. Every real invocation must contain its
`[ RUN ]` record, a one-test completion tally, and its saved exit status.
The OTHER_VM child also requires its successful completion sentinel.

## Same-ruler attribution and sensitivity

Keep the test ELF and boundscheck SO byte-identical while exchanging only the
runtime SO between baseline and candidate. Run every selected test three times
per arm; report the exact first failing assertion and the failure-name set
difference. Never equate a changed test count with preserved coverage.

For this fixture timing change, also build baseline and candidate with both
testability macros OFF using the same recipe. Compare their defined symbol
sets and retain both artifacts. Default-product behavior is not inferred from
the test-hook observations.

On the candidate, exercise the product producer, pause consumer, and slot
entry independently with controlled source cuts. Rebuild the product SO,
keep the same ELF/boundscheck pair, and rerun the same selection. A valid cut
must reach a target invariant assertion; a compilation/loading failure or
an earlier fixture assertion is not sensitivity evidence. Preserve the cut
diff, build log, identities and result logs. Restore the source and rebuild;
the restored runtime must match the uncut candidate byte for byte.

Record which controls remain valid for each cut: cutting a producer shared by
the concurrent-consumption control may also invalidate that control. The SATB
control is independent of the y2y producer. No result here grants eligibility
to an incompatible historical composite ruler or closes D-5.
