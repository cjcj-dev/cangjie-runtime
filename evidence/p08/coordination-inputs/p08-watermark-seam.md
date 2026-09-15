LANE=sym_cangjie_runtime_614_implement_r5687433872
ROLE=implement
PROGRESS=WIP

P08 TLS/buffer producer→consumer实际读证补充（准备迁移，未改相关产品）：
1. 当前zStackWatermark.cpp仅构造/IsDone；phase head入口在Mutator.cpp:956,979 DrainStackWatermark→TryBegin。P08要求compiled fast path改TLS，buffer Add删颜色变更Flush，二者一旦落地都依赖每次phase转换更新TLS mask和on_new_phase。
2. PLAN将ZStackWatermark::start_processing更新TLS/on_new_phase接线分给P10（#617为本包下游）。仅加TLS字段并改编译器读取、把更新接线等下游，会让本包联合验收读旧掩码并遗留buffer。
建议函数级会合：本包提供并接入最小start_processing调用点（当前DrainStackWatermark开始成功后、任何root/frame消费前，TLS mask更新+buffer on_new_phase），P10迁完整process_head/frames时消费该接口并保留顺序。请明确允许的最小hunk和线程身份：现有mark stacks/store buffer为OS TLS，watermark为Mutator/协程；不能把GC扫描线程的TLS误当被扫描协程所属TLS。
3. invisible_root同理：当前PublishInvisibleRoot/WithdrawInvisibleRoot在Mutator.h:508–529、VisitRawObjects在Mutator.cpp:416；P08迁入TLS要保留协程切换/迁移与枚举身份。P10收拢process_head归属不变，先固定共同接口。
仍遵守容量暂停，无新复制/建测。独立primitive/plain-root包装删除已保存2f1bb137974c6848ba10fe6180176e65d09a054c，draft PR#647；不宣称本包已完成。
