P01最新联合std-v7已完成（前端r6已修普通参数/泛型读取），托管首次到std.time:getTimeZoneName→String.indexOfFast时读到colored myData：0x80000008241510被当普通地址使用，pc finalizer_trigger+0xb4e5e。gdb完整栈/maps在kkk2:/root/sym_cangjie_runtime_608_implement_r5676392826-nested-tests/evidence/finalizer-r6-gdb.log。
生产链事实：cjcj值类型构造函数的this是AS1 pointer，同时传hidden $BP；栈上临时构造call给$BP=null（std.time.r6.input.ll getTimeZoneName内Range构造可见）。CreateTypedStoreForAddress→TryCreateManagedMemberStore以$BP发gcwrite_ref/struct。
两侧现有分路：
- LLVM CJBarrierLowering.cpp:storeFastPath只编译期跳过ConstantNull base，对构造函数里动态$BP不会跳过，prev=0也进入带色store。
- runtime CompilerCalls.cpp:MCC_WriteRefField先按field是否heap路由，IsGlobalStruct识别base==1全局，最后把所有其它非heap目的也WriteStaticRef带色（:339），没有plain根路径。MCC_WriteStructField非heap obj也WriteStaticStruct；CJ_MCC_ReadRefField/ReadStructField亦缺stack域读取。原高位布局被普遍AND解码隐藏，本包删除后暴露。
请求P01最小族修裁决（不恢复AND/SHR猜测）：值类型构造的$BP协议应在cjcj codegen处动态分三类（0=stack plain，1=global colored，其余heap colored），普通load/store在$BP=0臂，真实heap/global才发GC intrinsic；runtime MCC接口按既有IsGlobalStruct及实际slot归属将残余非heap非global的值字段走RootSlot/普通copy。LLVM快路需仅对真实heap域走带色store（例如动态$BP=0/1拒绝inline），不能把匿名null holder的实际heap slot误当stack；现runtime heap-slot优先规则须保留。
这涉及#607已有分路点，请确认纳入本包或给应保留的精确ABI域协议。继续WIP，其他root-table/嵌套根对照已完成，不改目标断言。
