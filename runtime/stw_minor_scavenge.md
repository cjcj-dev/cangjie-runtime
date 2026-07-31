# Default-off STW minor scavenge

## Scope and switch

This change adds an optional survivor evacuation step to the sticky minor
collector.  It runs inside `WCollector::DoYoungGarbageCollection`, after the
young trace has completed and before `RegionManager::CollectYoungGarbage`
reclaims the selected source regions.

`MRT_STICKY_EVAC_THRESHOLD` is the maximum live percentage of a small young
region that may be selected.  Its default is zero, which skips every
evacuation operation.  Values above 100 are clamped to 100.
`MRT_STICKY_EVAC_MAX_REGIONS` bounds the number of source regions selected per
minor collection and defaults to 8.  Setting either value to zero selects no
source regions.

This facility must remain disabled until the remembered-set completeness work
has established that every old-to-young edge is represented.  It allocates no
`ForwardDataManager` storage and does not use the concurrent major collector's
route, ghost, or epoch protocol.

## STW ownership and forwarding-table lifetime

`DoYoungGarbageCollection` holds `ScopedStopTheWorld` across tracing,
evacuation, slot correction, and reclamation (`WCollector.cpp`).  The
from-object to to-object table is a local variable owned by
`EvacuateYoungRegions`; it is built, consumed, and destroyed before that
method returns.  The copied-object facts recorded by `CopyObject` are also
cleared before returning.

No mutator can publish a reference during this interval.  Before the table is
discarded, every slot class that can still name a source object is corrected.
Only then are source-region live bytes reset so that the existing all-dead
young-region path can reclaim those regions.

## Source and destination regions

A source candidate must be a small young region with age at least one,
non-zero live bytes no greater than the configured percentage, no raw-pointer
objects, and no pin.  Both `RegionInfo::IsPinnedRegion` and the dynamic set of
regions reached through by-value root channels are exclusion conditions.
Candidates are ordered by live bytes and then region address, and the maximum
region count is applied after ordering.

Each source receives a fresh thread-local region from
`RegionManager::AllocateThreadLocalRegion`.  The destination remains young,
inherits the source age, and stays outside the fixed source snapshot for the
current collection.  Objects are copied with `CopyCollector::CopyObject` and
retain their allocation size and normal mark state.

The destination is initialized by the existing
`RegionInfo::InitRegionInfo` path.  Its census boundary is therefore the
region start before the first allocation.  Every copied address is at or above
that boundary, so evacuation does not create a young object below the census
boundary.  Destination regions are removed from the temporary thread-local
list and enlisted as full young regions only after the current young
reclamation pass; they participate normally in the next minor collection.

## Exhaustive slot correction

The remaining references are partitioned by storage location:

1. True root slots are revisited through the same mutator, static,
   concurrency, finalizer, and export root visitors used by the young trace.
2. Old-region slots are revisited through the remembered set in correction
   mode.  This is why deployment remains blocked on remembered-set
   completeness.
3. Young-region slots are visited in every marked live object outside a source
   region and in every newly copied destination object.

By-value root channels have no writable slot.  Regions reached from allocation
buffer `stackRoots`, both resurrected-export sets, or `cycleRefWorkStack` are
therefore pinned as whole regions and cannot become sources.  Raw/interior
references are handled by excluding every region with a non-zero raw-pointer
object count.  Dead objects cannot retain a live reference.  Source objects
will be reclaimed and therefore require no correction.

These cases exhaust the root, old-heap, young-live, and young-dead partitions.
The field correction walk deliberately does not filter reference kind:
`BaseObject::ForEachStrongRefSlot` also reports `WEAK_REFERENT` slots in this
runtime, and those slots must be corrected as well.

## Failure behavior

Failure is explicit while the world is stopped:

- inability to allocate a destination region fails a `CHECK`;
- destination allocation or object copy failure fails the existing
  allocation/copy checks;
- a conflicting from-object table entry fails a `CHECK`;
- failure to replace a slot with the table's destination fails a `CHECK`.

There is no partial fallback that would reclaim a source region after an
incomplete copy or incomplete slot correction.  With the threshold at its
default zero, none of these operations is reached and the pre-existing sticky
minor path is unchanged.
