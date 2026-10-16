LANE=sym_cangjie_runtime_1322_implement_r5896137675
本棒 #1322 Implement 尚未改产品。
实测 box.sh kkk2 'ulimit -c 0; uptime; df -h / /tmp'：可用17G，load 78.64/61.61/57.98；不足runtime构建20G常备下限。本棒远端尚无产物可清，请主控处理容量；继续只读核查。
另 #1309 已授权修改 zStat.cpp ZStatPhaseCollection::RegisterStart/RegisterEnd，#1322 累计统计需对照相同周期结束点及 ZServiceabilityCycleTracer / GCMemoryManager 链，存在同文件函数交集。请确认协调顺序及 #1309 当前候选坐标，避免互覆。未修改其域。
