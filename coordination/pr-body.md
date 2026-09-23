Fixes #906.

`MaxTenuringThreshold` was a fixed 14. Initialize its default using relocation headroom after worker selection and medium-page sizing, preserve explicit maximum priority, and consume `ZTenuringThreshold` after promote-all selection. Both native `GCParam` and managed environment configuration preserve whether a value was explicitly supplied.

ZGC anchors: zArguments.cpp:151-175 and zGeneration.cpp:704-715,812. Remove the late medium-page initialization and fixed maximum constant. Per advisor, HotSpot-only AlwaysTenure/NeverTenure constraint infrastructure is outside this change; real zero-threshold promotion is tested against a maximum-one control.

Validation in progress: default/testable product builds passed; real initialization, allocation, young GC, managed init/start and survivor-page observations are covered. Deliberate product cuts, final four-arm differential and final lineage checks are pending. This draft must not advance to review until main contains #900 and this branch merges that main and reruns validation.
