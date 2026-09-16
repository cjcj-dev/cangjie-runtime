# 未完成项（不能作为已通过交付）

1. 等20260915T202002Z和202204Z两份advisor：P01真实预留段发布/编译器8段限额，原包围区间不能沿用为真实段。
2. 新容量恢复回执到达后经wf_kkk2.sh build在独立代次目录建default/testable；20:03Z之前的容量回执不可用。初次现读kkk2可用1.3G。
3. gc_unit default/filler/testable同候选N>=3；filler就是default SO/ELF以CJRT_HEAP_FILLER=0再跑。OHOS实际尝试；最终三行UNIT_*_RC不可用历史rc填。
4. 最终5刀已生成cut1至cut5，cut5在Uncommitter::Uncommit断现有UncommitFlushed消费行，entry_cut_check位置预演rc0。每刀仍需构建/运行/恢复、单项filter、真实ELF/SO哈希与核域/uptime。工具绿只说明位置，不代表测试红。
5. finalizer_trigger/segmented_array_managed各N>=3，私有现成SDK/P01工具链；不复制完整SDK，不修改共享SDK。
6. 对应表/删除清单/测试集合差最终归档。旧MemMap中仍有物理失败/分段不变量需逐条判定替换，不因删除旧API就声明全部覆盖。
7. 当前来源predecessor的四刀日志保存在historical/；历史cut4目标index(first)=12而期望0，修后用例在8791；不升级为当前候选结果。
8. 同wf/P04/PR641，完成后重新fetch核main；发生新合成需按内容核并重验相关范围。形式自检与sym_deliver登记后才送首次独审。
