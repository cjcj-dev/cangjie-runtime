# R1/R2 producer → consumer（改码前）
坐标：候选 c9e912409aaa77a31eb88c9b90ccb9f9f208d2c3；主线回读 c3973505c171aa7e72095c3357ffd57f56156707。
| producer | consumer | 修改点 / ZGC |
|---|---|---|
| zMark.inline.hpp:27 typed GCThread claim | zPage.inline.hpp:557 → :478/507/541/614 | zLiveMap.inline.hpp:41/71 纯原语先统一 true=new，再消除页层反转；Z zBitMap.inline.hpp:55-83 |
| zPage.inline.hpp:625 EnqueueObject | zLiveMap.inline.hpp:59 自动 live 结算 | EnqueueObject 是尚存 P3 旧接口，边界明确适配 !new；LARGE/无效页分支合同不变 |
| zRelocate.cpp:508/562 FROM route 消费 | MarkBits 三参 | 返回值丢弃，保留 route 职责 |
| DoYoungGarbageCollection :108 | old mark-start :126-137 | color → retire → seq → phase → domain；Z zGeneration.cpp:1212-1237 |
| DoYoungGarbageCollection :108 | young mark-start :119-167 | color → retire/prepare → seq → phase → domain → remset；Z zGeneration.cpp:855-880 |
位图测试原 false=first 期望按 ZGC true=first 修改，保留位图内容、计数和重复 claim 断言。全体调用清单见 producer-consumers.txt。未修改根/字段生产接口。
