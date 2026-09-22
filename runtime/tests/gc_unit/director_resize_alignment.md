待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。

# #895: minor-start resize policy

Reference: `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zDirector.cpp:801-818`.
Baseline: `1d4c491e6424991d770eb7a66fa4ff053693fdd0`.

`start_minor_gc` selects workers, then checks dynamic worker policy and the
current major busy state. Inside that branch it copies the old resize statistics
and compares the current worker count before requesting resize. This preserves
the ZGC branch location, nesting and intermediate data. Worker selection and
the comparison algorithm are unchanged; the old combined condition is replaced.

The existing product debugger fixture accepts a static/dynamic axis for resize:

```sh
export BUSY_SITES=resize BUSY_DYNAMICS="0 1"
bash runtime/tests/gc_unit/run_director_busy_gdb.sh
```

Use the ELF, product library, matching source, output directory and leased CPU
settings documented by `run_director_busy_gdb.sh`. The 16 processes cover both
sample-time busy states, both decision-time busy states, equal/unequal counts,
and static/dynamic policies. `RuntimeParam.gcParam.staticGCThreads` sets the
policy before real runtime initialization; the test reads the resulting product
flag. Product worker methods establish the worker state before sampling.

The real director thread reaches `start_minor_gc` through `run_thread/start_gc`.
`ASSERT_RESIZE` reads the product worker's `_requested_nworkers` at the minor
send boundary. No copied director algorithm or product test hook is used.
Only dynamic + busy + unequal may produce a resize request. Reinstating the
baseline condition must fail only static + busy + unequal; removing the product
request store must fail only dynamic + busy + unequal. Equal-count and idle-major
cases remain controls. The separate director-entry cut checks the runtime entry.

Keep candidate, cut and restored SOs with the same test ELF and hash each before
running. Diagnostic locals can be optimized out; the mandatory output assertion
reads the actual product atomic state. Source-level single stepping must not
skip the send boundary when `start_minor_gc` is inlined.
