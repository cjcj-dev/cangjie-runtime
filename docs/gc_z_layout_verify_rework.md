# D09 #484：校验定义与停顿观测归位

待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。

搬前基线 `3439312e41310da5c961ffa2025642e19081142f`；本轮起点 `5f406cdce5f288a51dcc4a391777034e067da194`。下表只声明定义位置对应，保留已有函数体，不声明机制完全等价。

| 函数 | 本轮起点 | 当前定义 | ZGC 对应 |
|---|---|---|---|
| `ColoredRoot` | `runtime/src/Heap/Verify/VerifyRoots.cpp:13` | `runtime/src/Heap/z/zVerify.cpp:71` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zVerify.cpp:214` |
| `PlainRoot` | `runtime/src/Heap/Verify/VerifyRoots.cpp:25` | `runtime/src/Heap/z/zVerify.cpp:83` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zVerify.cpp:171` |
| `VerifyAccessedOop` | `runtime/src/Heap/Verify/VerifyHeap.cpp:18` | `runtime/src/Heap/z/zVerify.cpp:114` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zVerify.cpp:121` |
| `Object` | `runtime/src/Heap/Verify/VerifyHeap.cpp:26` | `runtime/src/Heap/z/zVerify.cpp:122` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zVerify.cpp:121` |
| `Oop` | `runtime/src/Heap/Verify/VerifyHeap.cpp:44` | `runtime/src/Heap/z/zVerify.cpp:140` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zVerify.cpp:131` |
| `Objects` | `runtime/src/Heap/Verify/VerifyHeap.cpp:95` | `runtime/src/Heap/z/zVerify.cpp:191` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zVerify.cpp:467` |
| `RootsStrong` | `runtime/src/Heap/Verify/VerifyRoots.cpp:34` | `runtime/src/Heap/z/zVerify.cpp:92` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zVerify.cpp:363` |
| `RootsWeak` | `runtime/src/Heap/Verify/VerifyRoots.cpp:45` | `runtime/src/Heap/z/zVerify.cpp:103` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zVerify.cpp:386` |
| `IntentionallyUnremembered` | `runtime/src/Heap/Verify/VerifyRememberedSet.cpp:48` | `runtime/src/Heap/z/zVerify.cpp:230` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zVerify.cpp:549` |
| `OnColorFlip` | `runtime/src/Heap/Verify/VerifyRememberedSet.cpp:54` | `runtime/src/Heap/z/zVerify.cpp:236` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zVerify.cpp:587` |
| `BeforeRelocation` | `runtime/src/Heap/Verify/VerifyRememberedSet.cpp:62` | `runtime/src/Heap/z/zVerify.cpp:244` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zVerify.cpp:610` |
| `AfterRelocationInternal` | `runtime/src/Heap/Verify/VerifyRememberedSet.cpp:87` | `runtime/src/Heap/z/zVerify.cpp:269` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zVerify.cpp:724` |
| `AfterRelocation` | `runtime/src/Heap/Verify/VerifyRememberedSet.cpp:114` | `runtime/src/Heap/z/zVerify.cpp:296` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zVerify.cpp:741` |
| `AfterScan` | `runtime/src/Heap/Verify/VerifyRememberedSet.cpp:120` | `runtime/src/Heap/z/zVerify.cpp:302` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zVerify.cpp:763` |
| `verify` | `runtime/src/Heap/Verify/VerifyRememberedSet.cpp:18` | `runtime/src/Heap/z/zForwarding.cpp:276` | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/zForwarding.cpp:369` |

`VerifyAccessedOop` 是调用 ZVerify::Object 的本地检查叶；参考 zVerify.cpp:121 的对象有效性检查职责，不声称有同名独立 ZGC 函数。调用入口在我方 zAddress.inline.hpp:98（ZGC 地址入口 zAddress.inline.hpp:505）。本包保留宏上下文、外部链接及实现；Oop 合并承载强/弱检查，参考 zVerify.cpp:131 与 :179。辅助状态 brokenObject 与 bufferedStores 随使用它们的定义搬移。

## 独立测试观测（ZGC 无对应定义）

| 定义 | 当前位置 | 后续包 |
|---|---|---|
| `SetAllocationStallTestHooks` | `runtime/src/Heap/Allocator/zPageAllocator.cpp:105` | #228 / A06；分配机制包负责其测试观测 |
| `PendingStalledAllocations` | `runtime/src/Heap/Allocator/zPageAllocator.cpp:114` | #228 / A06；分配机制包负责其测试观测 |
| `EnqueuedStalledAllocations` | `runtime/src/Heap/Allocator/zPageAllocator.cpp:115` | #228 / A06；分配机制包负责其测试观测 |
| `DequeuedStalledAllocations` | `runtime/src/Heap/Allocator/zPageAllocator.cpp:116` | #228 / A06；分配机制包负责其测试观测 |
| `SatisfiedStalledAllocations` | `runtime/src/Heap/Allocator/zPageAllocator.cpp:117` | #228 / A06；分配机制包负责其测试观测 |
| `FailedStalledAllocations` | `runtime/src/Heap/Allocator/zPageAllocator.cpp:118` | #228 / A06；分配机制包负责其测试观测 |

六个声明及条件编译保持，产品 StallAllocation 内部回调保持；没有添加开关、适配器或测试导出。

## 删除清单

- 删除旧物理源文件 `Heap/Verify/VerifyRoots.cpp`、`VerifyHeap.cpp`、`VerifyRememberedSet.cpp`，定义全量归位；Verify/CMakeLists.txt 移除三项。
- 从 Heap/z/zPageAllocator.cpp 移出六个独立测试定义，保留于 Heap/Allocator/zPageAllocator.cpp，原 CMake 已编译该文件。
- 本轮未替换任何机制或开关，既定 D06b / A06 / #228 原地保留规则继续适用。

## 测试同批清单

沿用 gc_z_layout.md 的七个 test_z*.cpp 路径搬移；本轮测试源码、名称及 CMake 未改。ZGC 没有独立 test_zVerify.cpp；现有 test_verify_phase.cpp 随原机制保留，不为了布局包增删测试。
