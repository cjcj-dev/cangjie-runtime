# impl_zgc_minorconc_align WCollector.h overlap

Direct task requires deleting the young-concurrent rollback names/branches and integrating wave8 y2y handoff while `impl_zgc_loadheal_struct` owns `WCollector.h`.

After the scoped Generation/RegionSpace/MutatorManager edits, `runtime/src/Heap/WCollector/WCollector.h` still contains only two stale test-instrumentation remnants:

1. line ~85 comment names deleted `MRT_GCV2_YOUNG_CONC_MARK` and describes an unset closed arm;
2. `NoteY2yAfterStw2TestReceipt` declaration (its Generation definition is now unused because latepub09 removed pause-local STW2 discovery).

May this lane make the minimal comment/declaration cleanup in `WCollector.h`, or should it leave those two remnants for the loadheal/merge lane? No load/relocate API or implementation would be changed.
