PinRawPointerObject selects the GC phase from the installed forwarding carrier’s source generation and resolves through relocate_or_remap_object before pinning and returning the object. Young forwarding therefore remains usable after in-place promotion changes the page to old. The pin CHECKs remain enforced.

Adds ten product tests, including six that exercise the Heap-owned collector through the real MCC_AcquireRawData/PinArray and release entry. Product CompactRegion performs promotion and moves primitive arrays; three deterministic sizes place the old address at a product-generated filler header. Tests cover both relocation phases, the reverse source-IDLE/other-FORWARD case, absent forwarding, absent carrier, and unmovable pages. This complete call-chain evidence is limited to that filler-header layout; general pre-pin header reads are tracked separately.

Validation at 4ae08d2033af7d35d0466264b5b601313527dc48:
- Merged main fca1c8ce77f139b14cef9117c1246fe01b0aa084 (#579), retaining both test sets.
- Default/testable builds passed; default/filler each passed 536 tests; OHOS-host passed 3 tests.
- Same test ELF with five independent product cuts failed 3/5/5/10/2 tests respectively, at the intended copy-result, promotion, phase-result, pin-count, and PinArray-result assertions. Restored suite passed 536 tests; restored product SO matches green byte-for-byte.
- Entry-cut check passed; delivery form check passed. Independent review remains required.

ZGC anchors: zGeneration.inline.hpp:131–140; zRelocate.cpp:382–415 and 862–896.

Refs #581. Report: /root/cj_build/reports/REPORT-sym_cangjie_runtime_581_implement_r5668277053.md
Evidence: /root/cj_build/reports/EVIDENCE-sym_cangjie_runtime_581_implement_r5668277053/final3
