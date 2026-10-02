# A2 observer revision: offline scope and future qualification

待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。
坐标基于 `9ad8262b74edbf8cd212916518d0431c963e5307`。
Only Python observer, shared decisions and offline qualification are revised.
No product/fixture/linker-script change. Original collector and five target
assertions remain authoritative. This document grants no real execution.

## Frozen offline batch

Entry: `python3 a2_observer_offline.py <recipe> <private-new-recipe-directory>`.
One invocation each, in listed order; stop on first unexpected failure/exception.
Each recipe records complete synthetic maps, file bytes, manifest, CLI table,
argv, target markers and mutated inputs. Dynamic device/inodes identify the
small synthetic files created by that invocation, never the preserved ELF/SO.
The table is explicitly synthetic and does not prove GDB API support.
20 recipes, each once, no automatic retry:

1. maps-positive: required ELF/runtime/bounds plus PC fixture bound; anonymous,
   arbitrary-named deleted memfd, bracket rows preserved, heap address readable.
2. required-deleted: selected runtime deleted rejected.
3. required-missing: selected file missing rejected.
4. required-hash: manifest hash mismatch rejected.
5. required-inode: selected mapping inode mismatch rejected.
6. maps-boundary: last legal 4-byte read accepted; cross-boundary/heap execute rejected.
7. maps-malformed: empty range and malformed map rejected.
8. location-multiple: both inline/body locations selected by structured table.
9. location-empty: MULTIPLE without sublocations rejected.
10. location-external: one location in other SO rejected as a whole.
11. location-stale: symbol generation mismatch rejected.
12. location-ambiguous: extra row/duplicate address rejected; single location accepted.
13. callback-mutation: controlled install/disable/continue/delete forbidden in callback;
    same outer install action accepted (separate visible target markers).
14. outer-lifecycle: input/install, read/build/resume, return/disable, exit accepted;
    second input rejected.
15. exception: armed exception/cleanup then even exit0 invalid.
16. out-of-scope: armed out_of_scope/cleanup then even exit0 invalid.
17. identity-loss: changed thread identity rejected, cleanup and invalid exit.
18. collector: real unchanged collector accepts synthetic positive set then rejects INVALID.
19. watch-install: structured hardware read installation accepted, write watch rejected.
20. source-contract: parse actual source; callback bodies have no BP constructors,
    execution commands, deletion or enabled writes; no `.locations` usage.

Python only on kkk2 via box and `ulimit -c 0`; start availability >=26GiB,
stop below24GiB, new artifacts <=256MiB. Copy just these Python files and this
plan; no old source tree/SDK copy. Single small process per recipe intentionally
sequential so the first unexpected failure prevents later recipes. No debugger,
inferior, build, preprocessing, GHA or push. Positive synthetic collector data
is never labeled a real qualification record. No runtime acceptance inference.

## Shared implementation and consumer order

Raw maps -> parse_maps (all address/perms/dev/inode/type/offset rows retained)
-> required_inputs (manifest-selected executable/runtime/bounds only)
-> Input contains/current-PC owner bind (real file/hash/device/inode boundaries)
-> callback capture -> outer dispatch/freeze current symbol objects/install Build, read watch and Finish
-> Build/read capture (same thread/consumer frame, product instruction ownership)
-> return capture -> outer dispatch/disable
-> exception/out_of_scope/unknown stop/symbol change -> INVALID/outer cleanup
-> exited capture -> outer finalize -> unchanged collector.

`info breakpoints <number>` is used instead of Python Breakpoint.locations.
Public (internal=False) breakpoints make the selected table visible. Parser
requires a typed parent, enabled locations, complete numeric addresses, unique
subrows, all addresses belonging to the bound product SO. Unknown table formats,
empty/pending/disabled/mixed-SO tables reject; What text is not address evidence.
Source locates Build only; actual Build stack and instruction still must be
captured. Symbol load/clear events and tracked Objfile.is_valid()/objfiles list checks invalidate the entire armed snapshot; no refresh while armed. Symbol text/addresses cannot substitute for consumer execution.

`stop` decorators always stop and only capture/decide. Outer blocking continue
returns to Python before dispatch; every BP mutation/continue is gated there.
A Finish out_of_scope event records invalidity; cleanup occurs outside callback.
Return validation uses the stored caller frame and same thread; read/Build uses
stored consumer frame. These actual frame/API semantics remain uncalibrated.
ReadWatch/Finish constructors also invoke the same mutation gate as offline tests.
No inferior calls, writes, return override, PC/register/memory changes.

## Future fixed GDB12.1 calibration (NOT AUTHORIZED, NOT RUN)

Retained debugger record: GNU GDB12.1 Ubuntu12.1-0ubuntu1~22.04.2,
/usr/bin/gdb SHA256 a6222ff0f450ca040e3479c04fdb7d0f1e0cabb099688270e720b7115d933c3d.
No new entity/version/API probe this batch. Versioned source documentation can
establish interface availability, not actual installation behavior.

Proposed finite batch: at most seven launches total, each once, timeout120s,
stop immediately on first unexpected failure. Requires explicit new authorization
bound to this local candidate SHA, retained artifact manifests and fixed debugger.
The first five use the preserved ELF, same product/bounds/fixture manifests and
original a2_qualify_case.sh/collector. No rebuild to test Python.

- prefix-ordinary/continuous/invalid (3): real raw maps and required identities;
  watch CLI dialect, hardware resource, Finish return_value/thread/caller frame;
  stop returns before outer install/disable; actual preceding instruction read
  boundary/bytes; expected acceptance1/1/0 and existing read assertions unchanged.
- roots-missing/zero (2): actual `info breakpoints N` single/multiple/inline
  addresses, every DSO owner, offset normalization, Build/read same consumer
  thread/frame; missing negative and zero-root Build/map positive unchanged.
  Real table, frame chain and instruction captures are required, not grep.
- abnormal scope calibration (1 separate retained-ELF launch): stopping the
  debugger without a consumer return must finalize INVALID and collector reject.
  A natural Finish.out_of_scope path is not known in the retained cases; do not
  induce product exceptions, change PC/returns, or claim it covered by exit.
- symbol lifecycle calibration (1 separate retained-ELF launch): capture actual
  new_objfile/clear_objfiles event availability and load/unload snapshot invalidity
  if the original case naturally exercises it. No injected dlopen/call/state.
  If the preserved ELF cannot naturally exercise scope/unload, mark NOT_OBSERVED
  and stop; a separately scoped calibration fixture requires new approval.

These two separate calibrations have no promised positive natural trigger.
No mock can close them. They are explicit remaining gaps, not waived gates.
Existing ELF 6e08807968ab3036fa67ad817a8103d54b024843e8aa55e4de23544cfa4b6b6b,
continuous DSO596c105348b18a5f9339969288750d4d87abd87bd1a0602aa0578336fca6b8dd,
default runtime df06a238df0072969fa70f267486dfe0517d93af88033b0a0fd0a82590afefba,
bounds c1205ffc1878a3d3663bdf78089ff3a07746ee6f2562d96e5fe28ed34ed5272a
must still match full preserved dependency/source/configuration/fixture manifests
and selected mapped paths/device/inodes at execution. Hash lines alone grant no
reuse permission. Keep historical five INVALID/collector2 untouched.
FDE causality, A2 product cuts/recovery, platform ABI, #1454A1/#135, release hold.

## Version-specific source anchors (read only; no debugger probe)

GNU upstream gdb-12.1.tar.xz, SHA256
0e1793bf8f2b54d53f46dea84ccfd446f48f81b297b28c4f7fc017b818d69fed:
- gdb/doc/python.texi:5790 internal breakpoints hidden from info breakpoints;
  use public breakpoints for this selected structured CLI table.
- :5871-5888 stop True/False and callback mutation prohibition;
  outer synchronous continue dispatch is outside that callback.
- gdb/breakpoint.c:5967 bp_read_watchpoint table type is `read watchpoint`,
  not the historical observer's `hw read watchpoint` substring.
- :6508-6569 selected-number filter and Num/Type/Disp/Enb/Address/What columns.
- gdb/doc/python.texi:3391-3407 new_objfile and clear_objfiles available.
  GDB12 lacks free_objfile event; retain old Objfile handles and test is_valid
  plus complete current filename set before resume and each capture, fail on
  invalid handle even if another SO reuses the same filename. No silent refresh.
  An unload with no further stop/normal return still fails lifecycle completion;
  transient unload/reload of tracked SO leaves its old handle invalid.
These are upstream version-source facts. Ubuntu entity behavior, output wrapping,
Finish frame recovery and events still require the proposed real calibration.

The snapshot begins at the real consumer Input stop, after the original fixture
loader (test_package_init.cpp:515) has completed. Before Input, normal loader
events only advance generation; there is no Build snapshot to reuse. After
normal return, probes are disabled before resume and fixture dlclose at :584
is normal teardown. Subsequent snapshot queries still reject generation/object
changes. Armed load/clear/unload/identity loss always makes the run INVALID.
This ordering avoids treating the expected fixture loader as an abnormal stop.
