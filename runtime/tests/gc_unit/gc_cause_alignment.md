# GC cause shape (Q6 / runtime #950)

ZGC source root: `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/`.

| Cause | ZGC cause | Producer | Consumers |
|---|---|---|---|
| TIMER | _z_timer | minor/major timer decisions | minor/major collect async; major keeps soft refs, partial roots |
| ALLOCATION_RATE | _z_allocation_rate | dynamic rule request; minor decision; major escalation | minor/major collect async; major keeps soft refs, partial roots |
| HIGH_USAGE | _z_high_usage | minor decision | minor collect async |
| WARMUP | _z_warmup | major decision | major collect async; warmup accounting, keeps soft refs, partial roots |
| PROACTIVE | _z_proactive | major decision | major collect async; keeps soft refs, partial roots |
| DCMD_GC_RUN | _dcmd_gc_run | ProfilerAgentImpl collectGarbage | major collect sync; keeps soft refs, precleans young |
| USER | _java_lang_system_gc | runtime/compiler explicit GC API | major collect sync; keeps soft refs, precleans young |
| FORCE | _wb_full_gc | object-array allocation recovery | major collect sync; clears soft refs, precleans young |
| YOUNG | _wb_young_gc | object-array allocation recovery | minor collect sync |
| ALLOCATION_STALL | _z_allocation_stall | page allocator stalled allocation | minor/major async; major clears soft refs and precleans young |
| WB_BREAKPOINT | _wb_breakpoint | concurrent GC controller | major StartGC + async; keeps soft refs, precleans young |

Anchors: zDirector.cpp:215,612-646,795-832; zDriver.cpp:133-151,232-317,334-367.
Profiler command semantics: `_dcmd_gc_run` enters the synchronous explicit request
case (zDriver.cpp:340) and does not imply clearing all soft references (:245).

Deleted: collapsed HEU and BACKUP; unused HEU_SYNC, NATIVE, NATIVE_SYNC, OOM
causes and request-table rows; cause-specific throttle cases and unused initial
timestamps; the uninstantiated legacy timeout executor branch. Existing throttle
methods remain owned by Q17. The external collected-heap async parameter and
worker routing are owned by Q8, per advisor decision
`sym_cangjie_runtime_950_implement_r5789443301-20260923T051458Z.md`.

## Product tests

`run_gc_cause_gdb.sh` runs eight cases on the same standalone ELF and product SO.
Allocations and RuntimeParam feed the actual director sampling entry. Minor-only
cases hold a major request in the product port and mark its product worker set
active; the director obtains that state through its ordinary sampling path.
The major allocation-rate case pauses actual warmup work and performs a real
young collection to establish measured cost and lookahead inputs. No sampled
statistics, rule return values, or dispatch instructions are written.

The debugger reads the request entering collect and the message actually stored
by send_async. The dynamic allocation-rate case also reads requests after the
product constructor returns. Missing dispatch is asserted from the actual port
state at the director's adjustment boundary.

`DriverCause.*` calls the running product drivers and checks completed generation
sequence deltas (one young pass vs preclean plus roots) and the product reference
processor's selected policy. `ProfilerDiagnosticCommand` enters through the
actual protocol handler and observes the completed old-generation cause.
No product test hooks, callbacks, or counters are added.
