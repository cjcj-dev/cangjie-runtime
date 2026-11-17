# Buffered struct-array edge across young flip

`BufferFlip1508.StructArraySoleEdgeSurvivesYoungCollection` uses a real
`RawArray` allocation with a flattened three-word element (primitive,
strong reference, primitive). A full product collection promotes the holder.
The holder is an export root; a separate noinline setter allocates the young
referent and publishes it through `MCC_WriteStructField`. The native task
sets its managed context to false and does not register the referent as an
export, native-frame, or invisible root. A separate unrooted large page is
expected to disappear, and a separately rooted page must survive.

The test reads the actual mutator store buffer before collection, including
its paired slot/previous value. `DriverLocker` excludes automatic cycles
through setup, product `young.collect(minor)`, and observation. The product
collection owns VM mark-start, root processing, buffer phase handling,
remembered scanning, selection, and relocation. The test does not set mark,
remembered, forwarding, or color bits, and adds no product hooks.

Large pages provide a stable address across selection. The final page may
have an empty livemap after age reset, so the output distinguishes bitmap
marking from the product's allocating-page implicit liveness. The test also
checks the surviving page, current remembered slot, exact one-flip sequence,
consumed buffer, and adjacent primitive values.

ZGC anchors: `zStoreBarrierBuffer.cpp:162-184,216-241`,
`zBarrier.inline.hpp:695-705`, `zRemembered.cpp:578-588`,
`zGeneration.cpp:538-576,855-889` at
`5b2d6991a1279d375f9a3c00c7bcd0bbcc7081d6`.

The finite qualification is incomplete. The first normal run allowed an
extra automatic young cycle; the sole authorized premise correction added
the driver lock across the complete observation interval. The corrected
normal run passed. Removing the product buffer phase-consumer call in
`ZStackWatermark::start_processing_impl` reclaimed the referent while both
controls passed. However, subsequent runtime activity crashed before the
native task result reached the outer assertion. The printed
`TARGET_ASSERT_EXECUTED` marker is only an observation marker in that case;
it is not proof of an executed target assertion. The cut is **not accepted
as precise red evidence**. Qualification stopped under the original contract,
and no restored execution was performed. The transient product source was
restored; this candidate changes tests only.

This construction does not cover the original
`RawArray<HashMapEntry<String, ASTContext>>` metadata, String value layout,
AST graph, historical producer/colors, or either old-holder relocation
branch. It neither resolves nor narrows parent issue #1506.
