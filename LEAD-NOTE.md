# Lane continuation — 1356

Report: `/root/cj_build/reports/REPORT-sym_cangjie_runtime_1356_implement_r5901475010.md`.
Status WIP, not ready for Review. Both draft PRs exist: runtime#1393, cjcj#755.
Compiler tree: `/root/cj_build/agent_scratch/sym_cangjie_runtime_1356_implement_r5901475010/cjcj`.
Remote all artifacts: `/root/sym_cangjie_runtime_1356_implement_r5901475010/` via box.sh only.

Synth authorization and advisor decisions are in report. Core carriers and sync
bridge are authorized. Finalizer registration stays at allocation for this lane;
cjcj#754 separately owns constructor-completion timing (do not silently change).

Current blockers are unfinished validation/migration, not an external block:
1. Complete same-ABI std build and sdk_build/sdk_verify; run managed product test.
2. Migrate old NativeSlot/Node finalizer fixtures; do not restore deleted APIs or
   weaken valid lifetime assertions. Existing default unit compile rc123, 0 ran.
3. Real producer and consumer mutation arms, runtime + compiler; header scanner
   already 12 pass / 2 precise failures / 12 pass, but only proves header face.
4. Final default/filler/testable/OHOS/DIFF after all fixes, report/checks/delivery.

Two repositories merged latest main/master without conflicts and pushed same
candidate branch. Preserve recent main mechanisms; main-recent-preservation.json
contains per-file git grep counts. No main/master branch was modified.

Scratch scripts/logs and remote keep directories are the continuation recipes.
Full std currently uses compiler3 (aca4a28722) because it started before merge;
compiler4 (62550e9e) was subsequently rebuilt successfully. Do not claim those
are the same compiler ELF. Report records hashes and source boundaries.

Latest runtime head cce2ebc9b1944aa46505aa529d189bb61bb5da21. Full std succeeded, SDK target verified. Real managed entry still SIGSEGV in finalizer worker: see report 11:56 and remote keep/managed-candidate/diagnostic.log. Thread masks old versus global loadShift; do not guess a barrier fix. Compiler kind and strength cuts each exactly 2/15 red, candidate/restored 15/15. New infra issue runtime#1396. Current SDK is retained and runnable for diagnosis; core std built by compiler3, compiler4 independently compiled.

Cleanup: default/build, testable/build, ohos/build, stdlib/build/build removed after retaining staging in remote keep/retained-{default-cce2,testable-cce2,ohos-8f6b}. sdk-target remains verified and contains current cce2 SO. Minimal startup and Mutex control exit normally (gdb wrapper rc1 is post-exit diagnostics); full managed test faults, diagnostic masks in report. No active builds intended at handoff. lane_selfcheck rc1 missing DIFF; deliver check rc0 only form.
