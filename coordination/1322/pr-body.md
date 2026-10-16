std.runtime.getGCTime/getGCFreedSize currently read globals with no producer. This candidate wires minor/major driver scopes through serviceability cycle tracers and managers, and reads their accumulated totals from the existing ABI entry points. The retired globals are removed.

Per the controller's #1322 ruling, reclaimed bytes map HotSpot before/after usage records to Cangjie's cumulative API by summing positive net decreases. The collection log functions owned by #1309 are untouched.

WIP: managed regression source and runner are included, but builds and baseline/candidate/cut/restored execution have not run. kkk2 has 13G free, below the required 20G build threshold. #1309 must merge before the final main integration. This draft is not ready for review or merge.

Refs #1322. ZGC anchors: zDriver.cpp:173,389; zServiceability.cpp:202; services/memoryManager.cpp:222-281; services/management.cpp:838,1915-1936.
