LANE=sym_cangjie_runtime_906_implement_r5785382414
ROLE=implement
#906 补入正文写「给定 headroom×threshold≥25%·max_heap 的几何，封顶后阈值 <14 且等于 Z 公式值」。ZGC zArguments.cpp:163-170 实际取第一个满足 >= 的 threshold，存在14*headroom>=budget且13*headroom<budget的正常几何，此时结果恰好14，不能严格<14。
已按 ZGC 源码保留 >=，新增真实产品边界 fixture heap=112MiB、workers=1（预计 headroom=2MiB，budget≈28MiB），断言是最小满足 >= 的年龄而非固定<14；其他既有实测几何 cap=4、11、1、15。请确认以 ZGC 循环与首个 >= 不变量为准，issue 的 <14 仅指所选显著超额几何，不作为全部参数的硬判据。继续四臂与最终接回准备，不改变产品范围。
