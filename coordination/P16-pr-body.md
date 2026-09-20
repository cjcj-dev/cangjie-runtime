P16 aligns verification helpers, root/object/remset closures, heap iteration, marking-stack checks, and debug safepoint checks with ZGC. It removes obsolete product diagnostics and test callbacks, migrates active tests, and consolidates Heap libraries.

This is an implementation draft for #627. Allocator/page lifecycle work owned by #727/#730, the segmented initialization controls explicitly retained for #730, and OHOS interop qualification owned by #733 are recorded in `coordination/P16-alignment-and-exceptions.md`. Windows canonical exports remain dependent on a successful full Windows link; no manually renumbered table is presented as validated.

Validation in progress: both release configurations build on kkk2 with 192 jobs. The e0d865927fe7 differential ran default/filler/testable and managed arms; default/filler had no candidate-only failures, testable had one assertion/setup issue now being retested. Root verification has retained N=3 cut/restored product evidence. Debug builds after a separate C++14 ODR fix; managed Debug input stops at a pre-existing TLAB role assertion, while three focused Debug units each passed N=3. Remaining control-arm work and final-SHA validation are tracked in the lane report; this draft is not ready for Review.

Refs #627.
