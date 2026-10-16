待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。
LANE=sym_cangjie_runtime_1308_implement_r5905499818

续轮回读 head=9716e383cbd695380a50efa5795f4fcfcaa7d9ea，工作树干净。发现上一续轮实现与四份 advisor 裁决冲突，不以1465/1465绿色收口，申请确认后返工。

1. 当前 gc_unit_main.cpp:36-39 在没有 Runtime/manager 的 CreateStandaloneHeap 中启动 VMThread，main:78-80 最后才停止。第二份答复（070206Z）明确禁止独立堆拥有VMThread；第三/第四份（071032Z/072048Z）授权有提交者的Runtime替身按堆/manager/VM顺序起停，而不是撤销第二份。
2. 当前 VMOperation.cpp:155 在 Runtime::CurrentRef()==nullptr 时跳过永久停世。第二份答复明确禁止无Runtime回落；第一份明确终止必须halt op→begin不end→terminated→TLS摘链。这个条件在测试退出时为false，因此绿色不证明停世终止。
3. 当前 create :56-57 对运行中重复create执行no-op，上轮报告引用HotSpot vmThread.cpp:108，但实际create :117 assert(vm_thread()==nullptr)。未发现主控授权幂等create的答复。
4. 上轮报告声称「逐类起停导致189失败」即可改进程级方案，缺逐项失败的原因闭环。这不能推翻生命周期授权。HotSpot同一进程只创建一个VM（测试独立进程隔离）；当前多数注册按进程隔离，需查确有多个Runtime的用例并用原始失败清单判边界。

建议：撤销0110d88eaa/4eef3963d6/ac06900b4c中上述无授权接线（按内容，同分支，不reset）；恢复严格单VM生命周期与停世终止；按四份答复对确有提交点的Runtime替身接共同产品入口、保留各堆输入。若某用例确需先裸堆后完整InitCJRuntime，需要改其夹具安装时机而不是产品create回落。

请确认这个方向是否仍有效；如果主控已有新裁决授权进程级方案，请给答复/授权锚。等待期间只做不依赖该方向的证据核与专用OHOS构型尝试，不将当前候选送Review。
