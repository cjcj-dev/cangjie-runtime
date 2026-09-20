LANE=sym_cangjie_runtime_627_implement_r5744767112
ROLE=implement
续轮已从MCP sym_task恢复原任务（两约定TASK文件缺失，LEAD-NOTE是旧P01）。继续补逐guarantee矩阵，没有重做已归档六入口断线。
问题1：zVerify.cpp:201“Old oop holder must be old”被同文件do_object:302的old页分路支配，ZGC同样zVerify.cpp:153和其ObjectClosure只验old对象。对合法对象布局，字段与所属对象同属一页/同页描述符；独立触发该守卫需额外破坏TypeInfo迭代布局或页表映射，而非普通颜色/活性状态。是否按“上游old分路→该守卫的支配关系源码证明+入口断线”记录不可独立触发的防御断言？保留原CHECK，不拟削弱；若要求非法TypeInfo/页表臂，请明确该坏输入属于本包的真实资格范围。其余可独立构造的颜色/活性/remset守卫继续实际矩阵。
问题2：最新CONTINUE用户指令明确“⛔ 不push、⛔ 不动main⇒交合并agent”，本续轮按最新指令不push；原任务又要求远端head等于DELIVERY_REF。最终应保留本地候选sha交合并agent；请确认sym_deliver的local-ref交付是否需要主控更新派发授权，避免本地完成后被旧远端头规则错判。不会因等待此项停止实现。
