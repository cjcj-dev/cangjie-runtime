待主控登记进 /root/cj_build/ops/CURRENT_DOCS.manifest

基线 779924ea4ca8d91e2f1fd049410f17e419d6eb8b

producer → consumer 顺序（修改前）
ZRemembered::scan_and_follow zRemembered.cpp:362 → ZRememberedScanTask 页消费 zRemembered.cpp:334 → scan_page_and_clear_remset :171 → oops_do_remembered / oops_do_remembered_in_live :177/179 → scan_field :274 → clear_remset_previous :182 → zPage.inline.hpp:313 → zRememberedSet.cpp:47 → BitMap.h:243 clear_range。
排序必须落在上述消费完成之后、清 previous 之前；ZGC zRemembered.cpp:153-157。
整面清位对应 ZGC zRememberedSet.cpp:67/77 → bitMap.cpp:636 clear_large → clear_large_range_of_words。
删除族检索见 deletion-before.txt；定义与声明之外仅有族内两条调用，外部调用者不存在。
