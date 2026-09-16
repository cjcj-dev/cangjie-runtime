LANE=sym_cangjie_runtime_614_implement_r5687433872
ROLE=implement
PROGRESS=WIP

任务书不变量②：“屏障决策只读颜色/TLS，不读GC相位、不读转发表以外的页态；GCPhase消费点在屏障/缓冲路径为0”。逐函数实读发现这个绝对判据与要求形态一致的ZGC相矛盾：
- /root/cj_build/reference/jdk/src/hotspot/share/gc/z/zBarrier.cpp:61-64 keep_alive_young实际读取young()->is_phase_mark()，不是ASSERT。
- 同树zStoreBarrierBuffer.cpp:190-192 is_old_mark读取old()->is_phase_mark()；:217 on_new_phase_mark实际调用它，保护SATB开始前的prev不能进入快照。
- 同树zBarrier.inline.hpp:729-731 remember使用ZHeap::is_old(p)；:746-749 mark按目标代路由，不是forwarding查询。
- 本轮要求保留#607 Finalizable与remset整链也依赖这些语义。
请求将判据按ZGC函数分解落实：fast path/是否需要remap只由颜色，remap路由查代forwarding；slow及buffer只保留ZGC上述对应相位/代判断，删除我方GCPhaseCheck与RemapPendingField相位路由。若“0”仍必须字面执行，将无法同时满足ZGC形态，请裁定。
另：不变量③称基址仅relocate start一次，但ZGC zStoreBarrierBuffer.cpp on_new_phase也调用install_base_pointers（锁内按last_installed_color防重），将按ZGC原分解保留两个调用入口，同一颜色只安装一次。
本轮尚未修改产品。候选已按授权fetch+merge快进到91f3dcc232201d4ad98ec3af6f10165edb950416，rc=0。
