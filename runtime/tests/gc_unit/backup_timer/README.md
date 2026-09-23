# Default-disabled backup timers

ZGC `z_globals.hpp:62-66` defaults both collection timers to disabled;
`zDirector.cpp:84-100,385-400` checks nonpositive intervals before expiry.
The public unsigned `backupGCInterval` uses zero for the disabled default.

On kkk2, compile `runtime_timer.cpp` with `-g -std=c++17 -I runtime/src`, linking
only the product `libcangjie-runtime.so`, as in `worker_config/README.md`.
Reuse this same ELF across candidate, producer-cut, consumer-cut and restored SOs.
Run `bash run.sh` with `TIMER_ELF`, `TIMER_SOURCE` (matching zDirector.cpp),
`GCV2_RUNTIME_LIB_DIR` (runtime and boundscheck SOs), `TIMER_OUT`, and the leased
`TIMER_CPUSET`. Sixteen cases run concurrently: env/API × default/1s × major/minor × unchanged/changed flag.
After sampling, the changed cases flip only the selected generation timer flag
between disabled (-1) and 1s; the public parameter and statistics remain unchanged.
Initialization must map the public parameter to both flags, and the real rule
result and dispatched request must follow the current flag.

The debugger stops the real director before sampling, holds other threads,
waits 241 seconds, then lets the real sampling, decision and port send complete.
It first observes the actual boolean result of the selected timer rule, then
reads the resulting product port message. Release inlines both static rules:
the x86-64 observer reads EFLAGS immediately after each source-mapped `ucomisd`
and executes its `jae`, checking that the resulting PC agrees. The first taken
branch is the rule's disabled/false exit; otherwise the second comparison is
the expiry return value. This reads the compiled rule result, without recomputing
the rule from statistics or inferring it from a request. Unexpected instruction
or source mappings are harness errors (rc=2), not behavioral failures.
Explicit minor cases also let the real minor driver receive the BACKUP request
and reach `RunYoungCollection` with `ZYoungType::minor`. The minor cases establish a busy
major through product methods, like the existing director busy matrix. No clock,
statistics, decision, or message output is overwritten. Product identity, input
statistics, elapsed time and the rule-return, dispatch and minor-entry assertions are printed.

Positive control: explicit 1s must dispatch BACKUP. Default must not. Restoring
both old 240s producers must fail exactly the four default cases. Adding 1000 seconds to the
sampled interval at the real director entry must fail exactly the four explicit
cases. The consumer cut preserves the runtime-dependent comparisons (assigning
a constant zero lets the compiler remove the rules and cannot validate this
observer). Both rule and dispatch assertions execute even when the first fails.
`TIMER_WAIT_SECONDS` may shorten a harness development run; only runs
exceeding 240 seconds qualify the producer-cut assertion.

The retired legacy queue timeout path is outside this test: controller ruling
`sym_cangjie_runtime_910_implement_r5785397551-20260922T224228Z.md` assigns its
removal to the #898 driver package and approves observing current director ports.

The finite run checks a real elapsed period greater than 240 seconds; the claim
for arbitrary elapsed times also relies on the product's nonpositive-interval
early return, before either elapsed-time value is read. It is not inferred from
an absence of messages. The observer supports the checked x86-64 Release
instruction shape only and does not claim portability to another code generator.

Q12 changes sample-boundary stops to the first decision entry: Release emits no
instruction for the aggregate return or assignment. The stall observer
`../test_director_stall_gdb.py` schedules the existing real blocked-allocation
fixture after that boundary, then asserts the worker count in the product minor
port request. `STALL_AFTER_SAMPLE=0` is the non-stall control. Required environment:
`DIRECTOR_SOURCE`, `STALL_FIXTURE_SOURCE`, `GCV2_RUNTIME_LIB_DIR`; run via GDB on
the matching gc_unit ELF with LD_LIBRARY_PATH pointing at the product SO.

Q12 headroom coverage is in `../test_director_headroom_gdb.py`. Use the same
`DIRECTOR_SOURCE`, `GCV2_RUNTIME_LIB_DIR`, and gc_unit ELF; select
`HEADROOM_SITE=dynamic|static|high` and `HEADROOM_CHANGE=0|1`. An optional
`HEADROOM_REPORT` chooses the existing runtime report path. The 64 MiB fixture
has no medium-page tier: changing ConcGCThreads after sampling must change the
free-space result emitted by the actual rule. Static coverage creates a real
young page before sampling, since the first old cycle promoted the retained
object. High-usage coverage stops the first old mark while cycle timing is not
yet trustworthy, so the allocation-rate rule cannot mask high usage.

Producer cut: use the fixed young-thread flag in `ZHeuristics::relocation_headroom`
instead of ConcGCThreads. Consumer cut: restore the sampled headroom member.
Both must fail changed-input cases in all three rules and preserve unchanged
controls. The actual rule result is read from existing MRT_REPORT output; the
debugger does not write heap statistics, free-space results, or decision outputs.
