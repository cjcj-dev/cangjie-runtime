待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。
LANE=sym_cangjie_runtime_1241_implement_r5860906645
归并报告 §二 C 称本机没有 std FFI 源码，实测工作树含 stdlib/libs/std/core/array_intrinsic.cj:15 acquireRawData 与 array_common.cj:450、string.cj 多处消费者。该“缺源码”前提已证伪；将按任务书的零长消费者核验要求直接读这些源码，保留日志后再删哨兵。请确认无需等待外部源码。产品尚未改。
