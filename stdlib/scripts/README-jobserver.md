# Ninja CPU budget (Linux host)

`python3 build.py build -t release --jobs N` uses one GNU jobserver budget for the
Ninja invocation and the existing cjc `ConcurrencyBudget`. The old 8-job clamp
does not apply on Linux. With `--jobs 0` (the default), the CPU budget is the
process's affinity width. `--package-jobs` separately limits simultaneous cjc
package commands; its default is 2. Increasing CPU capacity does not start N
memory-heavy packages. Runtime heap limits and the backend IR concurrency guard
are unchanged.

`build.py` configures `CANGJIE_CPU_JOBS`, `CANGJIE_PACKAGE_JOBS` and
`CANGJIE_SHARED_JOBSERVER`, including when resuming an old build tree. The CMake
producer wraps ordinary and LTO cjc commands, native compile/link commands and
archive extraction. Ordinary and LTO outputs are both retained; the existing
same-package dependency still prevents simultaneous writes to their cache.
The real top-level `CANGJIE_ENABLE_CCACHE=ON` configuration composes native
commands as budget admission, then ccache, then compiler/linker; it does not
replace the budget launcher. `--build-target NAME` selects an existing native
Ninja target and its dependencies (repeatable); omitting it still builds all.
Target values must be nonempty names, not options beginning with `-` or variable
assignments containing `=` (also applies to the Windows Make fallback).
Both `--build-target name` and `--build-target=name` select the same target;
these forms never expose Ninja's dry-run or tool-mode options.

Configuration, build and install execute under a stable Python guardian. The
Python frontend's unique liveness writer lets that guardian detect its loss
even when the intermediate Node bridge stays alive. Guardians use Linux
subreaper/pidfd APIs, tested before borrowing or launching work; Python 3.9+
and `/proc` are required. Direct command exit is observed independently of
stdout EOF, so inherited output handles cannot hide a dead adapter. The
existing `CMAKE_BUILD_PARALLEL_LEVEL=1` still limits native configure probes.

The adapter works with Ninja 1.10. It passes Ninja an explicit `-j N` even on
versions with native jobserver support, so Ninja does not independently acquire
another implicit token (see the [Ninja jobserver specification](https://ninja-build.org/manual.html#_gnu_jobserver_support)). Generated shared-budget graphs should be launched using
`build.py`; a raw `ninja` without the owning budget fails with
`JOBSERVER_MISSING_BUILD_ENTRY` instead of silently starting independent pools.

The waiting supervisor lends its implicit slot to its FIFO. Every command takes
one byte before starting a compiler, and that held byte represents the child's
implicit slot. Extra cjc workers borrow from the same FIFO. Nonblocking numeric
descriptors 3 and 4 are passed to cjc and its child tools; this avoids the current
consumer reopening a FIFO with a blocking read after `select`. A command
returns its held byte after successful completion. On failure the supervisor
terminates the exact owned process group, preserves the first command's exit
status and diagnostic, and drains the group before releasing resources.

When called inside a GNU make or parent jobserver task, the invocation already
owns one implicit slot. It borrows up to N-1 *currently available* parent bytes,
never waits while holding them, and delegates only that reserved capacity to its
Ninja/cjc FIFO. This also enforces a smaller explicit local `--jobs` value.
Borrowed bytes are returned unchanged only after the entire subtree has ended;
the parent FIFO is never deleted or replenished. This conservative allocation
does not grow later when other parent tasks finish. Invalid authentication is an
error. Linux FIFO and inherited numeric pipe authentication are supported;
inherited pipe descriptors are reopened through `/proc/self/fd` without changing
the parent's open-file-description flags.
GNU make authentication containing a negative descriptor means the pool is
disabled. That state is preserved through SDK command hops; a budget build uses
only one local slot, even if its requested jobs value is larger. It is distinct
from malformed authentication or a closed positive descriptor, which still fail.

Normal completion requires all local bytes back. Failure/cancellation records a
deficit rather than fabricating successful returns; the failed local pool is
discarded after its descendants are gone. SIGINT/SIGTERM are forwarded through
the build entry. Loss of an intermediate guardian is recovered by a surviving ancestor. Loss
of the outermost original-byte guardian is explicitly INCOMPLETE; it cannot
fabricate returned bytes. A live guardian with a blocked return retains its
ledger and cleanup responsibility rather than timing out and abandoning it.

For bounded diagnostics, `CANGJIE_JOBSERVER_LOG=/absolute/file.jsonl` records pool
capacity, command intervals, return counts and original exit statuses.
`CJCJ_JOBSERVER_TRACE=1` additionally enables the compiler's existing counters.
Compiler lease peaks are not measurements of CPU utilization or speedup.

## Finite qualification

`tests/ninja-jobserver.integration.mjs protocol` checks token conservation,
single-slot behavior, make pipe/FIFO import, nested local caps, invalid
authentication, errors and cancellation. `real` exercises a verbatim build.py
entry and the actual CMake package command producer on two small real packages,
their ordinary/LTO outputs, an installed executable and cache resume. Set
`JOBSERVER_TEST_ROOT` to a fresh private output directory and
`JOBSERVER_TEST_SDK` to the fixed installed SDK. `real-only` selects the independent
compile/budget portion while leaving cache qualification separate.
`tests/jobserver-rework.integration.mjs` adds public-SDK regression gates for the
real stdlib top-level ccache configuration, first/reconfigure cancellation and
controlled launch-boundary signals, plus disabled/valid/invalid jobserver
authentication. Its finite native target gate retains all top-level configuration
dependencies and explicitly copies the built native library for consumption;
it does not run the complete std install or replace the top-level CMake graph.

These finite gates do not establish complete std throughput, whole-SDK release
qualification, every compiler phase's internal thread safety, or support for a
different C2/ABI. They do not enable `CJCJ_BACKEND_CONCURRENCY`.
# Layered memory budget

The SDK's native std stage must pass `--jobs`, `--package-jobs`,
`--memory-budget-gib`, `--compiler-heap-gib`, and `--native-reserve-gib`
through the real `build.py build` entry. The three memory options form one
optional group for standalone callers. Partial, nonpositive, or overcommitted
groups fail before configuration/build. The admitted configuration satisfies
`min(jobs, packageJobs) * compilerHeapGiB + nativeReserveGiB <= memoryBudgetGiB`.
The external memory scope remains required: managed heap limits do not bound
all native allocations. This arithmetic is not a measured RSS guarantee.

For example, a 64-core/64-GiB scope can reserve 16 GiB outside managed heaps
and select two 24-GiB compiler heaps. The package pool is shared by static and
BC rules; native C/C++/ASM retain the CPU jobserver rather than using that
package limit. APC1 and the compiler's IR protection remain unchanged.

`CANGJIE_COMPILER_HEAP_GIB` is persisted in CMake and consumed as `cjHeapSize`
on both actual compiler commands. Re-entry changes reconfigure the graph;
ambient `cjHeapSize` cannot override an explicitly selected heap. Removing
the memory options clears this configured override. Changing only total or
reserve still repeats admission before invoking the existing graph.

`tests/layered-budget.integration.mjs` checks this production entry and CMake
command producer with an isolated finite graph and instrumented compiler
stand-in. It is a budget/target-selection fixture, not a language, ABI, SDK,
or full-stdlib qualification. Native commands in that fixture use real CC.

