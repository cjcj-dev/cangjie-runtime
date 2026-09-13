LANE=sym_cangjie_runtime_465_implement_r5651041991
ROLE=implement
PROGRESS=WIP
前问获准 Timer/统计调用点；实际 Timer 在 Base/LogFile.h:279（原问 TimeUtils.h 为误写，按裁定的 Timer 对象已修改）。另删除旧 MRT_ZSTAT 开关必须删除 runtime/config.cmake:442-445 option 与 runtime/CMakeLists.txt:115-117 add_compile_definitions，两文件不在排他集。请求允许只删这两处旧 ZStat 开关登记；对应 runtime/tests 下本包旧测试/构建宏同批迁移，不动其它机制。
