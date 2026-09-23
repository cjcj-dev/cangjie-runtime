LANE=sym_cangjie_runtime_906_implement_r5785382414
ROLE=implement
最终接线核对发现一个直接影响 #906 的既有输入差异，请裁决范围/落点，当前暂停送审，继续保存已完成证据。
我方 runtime/src/Heap/z/zArguments.cpp:22-28 的 initialize_heap_flags_and_sizes 把 maxHeap 改成 maxHeap*90/100 + maxHeap*10/100。对于512MiB，整数截断变成512MiB-1；set_medium_page_size() 随后把预期16MiB tier向下取为8MiB。既有实测 DefaultLargeHeap: overhead=12582912,budget=134217727,cap=11；按未被减1的512MiB及2workers，medium=16MiB,headroom=20MiB,budget=128MiB，ZGC结果应为7。64MiB实测budget=16777215同样暴露减1。
ZGC zArguments.cpp:40-52 只在缺省输入时调整 SoftMaxHeapSize=MaxHeapSize*90/100，绝不改 MaxHeapSize；我方这段错误地改了最大堆，且无SoftMax更新。入口 HeapManager.cpp:28 已将param.heapSize*1024设入ZHeuristics；initialize()91随后错误覆盖，直接在本包输入链。
按常备裁决5，本条修法链经过候选修改函数，似应并入本包；但不应擅自扩到完整SoftMax旗标装置。建议删除这段错误MaxHeapSize重写及仅服务它的函数/调用（真正堆尺寸输入已由HeapManager::Init生产），本包保证configured max等于封顶/medium使用的max；SoftMax缺省90%另开Triage。请确认该删除形态或指定ZGC对应分解，不留空壳。需重跑最终构建/差分/全部切刀，已有最终59d1c122四臂CAND-ONLY=0与六臂产品闭环仅作修正前记录。
