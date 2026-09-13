# AGENTS.md —— 棒契约（⭐ 开工先读本文）

> ⭐ 本文只放**跨棒通用、且不随代码变化**的东西。
> ⛔ 凡带具体行号/计数/路径的事实，去下表拿，⛔ 别从本文抄。
> ⭐ 模板真值 = `ops/coord/AGENTS_runtime_template.md`（⭐ 派发时同步到各树）。

## ⭐⭐⭐⭐⭐ 零 · 先确认**你是哪种角色**（0823 用户令重定）

⭐ 你的任务书开头会写明。⛔ **五种角色的边界是硬的，⛔ 不许越界。**

| 角色 | 你干什么 | ⛔ 你**不许**干什么 |
|---|---|---|
| ⭐⭐ **探索**（explore） | ⭐ 读源码 / 读 ZGC / 对齐 / 定位 ⇒ ⭐ 交**一张表** | ⛔ **不改产品码**（⭐ 一改就没人能独立复核你的结论） |
| ⭐⭐⭐ **归并**（synthesize） | ⭐ 把若干探索结果合成**一份可执行结论** + 实现任务书 | ⛔ 不实现 |
| ⭐⭐ **实现**（implement） | ⭐ 写产品码 + 测试 + **故意破坏转红**证据 | ⛔ **不自审**、⛔ 不碰主分支 |
| ⭐⭐⭐ **审查**（review） | ⭐⭐ **试图证伪**这份实现 ⇒ ⭐ 放行 / 打回 | ⛔ 不替它改码 |
| ⭐⭐⭐ **合并**（merge，⭐ 每仓常驻一个） | ⭐ 把交付合进主分支 | ⛔ 别的角色**一律不碰主分支** |

```
⭐ 探索×N ──▶ ⭐⭐ 归并×1 ──▶ ⭐ 实现×M ──▶ ⭐⭐⭐ 审查×1（对抗） ──▶ ⭐⭐ 合并×1
```

## ⭐⭐ 一 · 当下真值从哪读

| 你要什么 | 读哪里 |
|---|---|
| **现役文档完整清单** | `ops/CURRENT_DOCS.manifest`（⛔ 未登记文档不作当前真值） |
| **ZGC 机制表与对齐状态** | `ops/design/ZGC_MECHANISM_LIST.md` |
| **当下真值 / 已被改判的结论** | `ops/coord/RELEASE_0_0_2_BLOCKERS.md` |
| **当前在飞状态／工作项** | 看板（GitHub Projects，`tools/cjops sym status`；索引 issue cjcj#8）；`ops/coord/TODO.md` 只放主线 sha／债务／现行裁决（⛔ 不从历史段反推当前） |
| **GC 诊断开关** | `tools/diag_registry.py` ⛔ 别自己搭临时探针 |
| **ZGC 源码** | `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/` |

⚠⚠ ⭐⭐⭐ **主线历史已被 squash 重写（0823）**：
```
⛔ 别 rebase / merge / pull  ⇒ ⭐ 旧的几百笔会被当成"新提交"
⭐ 读主线用：git show <主线sha>:<路径>
⭐⭐ 判「我的东西在不在主线」⇒ **按内容核**：git grep -c "<特征符号>" <主线sha> -- runtime/src
   ⛔ `merge-base --is-ancestor` 在这里**毫无意义**
```

## ⭐⭐⭐⭐⭐ 二 · 卡住了怎么办：**advisor 通道**（⛔ 别空转、⛔ 别自己改范围）

```bash
cjops advise ask --lane <你的棒名> --file <问题文件>
```
⭐ 答复出现在 `ops/advisor/outbox/<同名文件>`（⭐⭐ **文件名逐字符相同**）。
⚠ ⭐⭐ **别空转等** —— ⭐ 先把**不依赖该答案**的部分做完，⭐ 每轮回来看一次 outbox。

### ⭐⭐ 什么时候该问（⛔ 不问才是问题）
- ⭐ 任务书的**前提被你证伪** ⇒ ⭐⭐⭐ **停下来问**，⛔ 别硬按错前提做完
- ⭐ 要**绕路 / 改范围** ⇒ ⭐ 先问
- ⭐ 与另一条棒**撞车** ⇒ ⭐⭐ **报给主控**，⛔ 别自己让路
- ⭐ 验收判据**你认为是错的** ⇒ ⭐⭐ 问

## ⭐⭐⭐ 三 · 证伪权

⭐⭐ **若主控的陈述与你的实测不符 ⇒ 以实测为准，⭐ 并在报告里写明主控哪里错了。**
⭐ 这不是客套 —— ⭐⭐ 0823 主控的「疑似缺」被棒实核 **12 次全错**，⭐ 且方向一致：
⭐⭐⭐ **一律低估我方已有的**。⇒ ⭐ **推翻前提【加分】**，⭐ 只要给得出证据。

## ⭐⭐⭐⭐⭐ 四 · 交付协议（⭐ **五种角色统一**，⭐ 可机器判定）

⭐⭐⭐ **一句话：⭐⭐⭐⭐ 每一条主张都必须携带【它是怎么被验证的】。**
⭐ 全文 `ops/coord/DELIVERY_PROTOCOL.md`；⭐⭐ **交付前自检**：`cjops deliver check --lane <你的棒名>`
⇒ ⭐⭐⭐ **⛔ 不合协议的交付不许收割**（⭐ 校验器非零退出）。

### ⭐⭐⭐⭐ 报告写到哪：⭐⭐ **`/root/cj_build/reports/REPORT-<你的棒名>.md`**

```
⭐⭐⭐ 这是**默认值**，⛔ 除非任务书明确另指一个路径。
⭐⭐ 棒名 = 报告头第1行 `LANE=` 后面那个串，⭐ 逐字符相同（⛔ 不许简写、⛔ 不许改大小写）。
⛔⛔ **⛔ 别写进任务书里提到的【别人的】报告** —— ⭐ 那些是给你读的**输入**，⛔ 不是你的输出。
⇒ ⭐⭐⭐ 0823 实账：⭐ 7 份审查任务书全指向同一个报告路径
  ⇒ ⭐⭐⭐⭐ **一份原始打回报告被后来的棒直接抹掉**，⛔ 再也没找回来。
⭐ 收割器按这个路径扫，⭐⭐ 写别处 = ⭐⭐⭐ **你交了但没人看得见**。
⛔⛔⛔ **⛔ 更别【删掉】它** —— ⭐⭐ 0901 实账：⭐ 一条棒中途把自己的 `REPORT-<lane>_r2.md` 删了，
  ⭐⭐ 主控只能靠它**上一轮**的报告才知道它做了什么 ⇒ ⭐⭐⭐ 差点当它一行没干。
⭐⭐⭐⭐ 而且派发器的 `--until-report` **按这个文件名判终态** ⇒ ⭐⭐ 文件不在 / 名字不对
  ⇒ ⭐⭐⭐ 它会**一直续跑**，⛔ 永远看不到你交付了。
⚠ ⭐ `reports/` ⛔ 不在任何 git 仓 ⇒ ⭐⭐ 覆盖就是**永久**的 ⇒ ⭐ 写之前先 `ls` 一下那个文件在不在。
```

### ⭐⭐⭐ 报告头：⭐ **前五行位置固定**

```
第1行  PROGRESS=<WIP|TRIAGED|DONE> · verdict=<一句话判词> · LANE=<棒名>
第2行  DELIVERY_REF=<仓>|<分支>|<40位sha>      ⭐ 或 DELIVERY_REF=none|no-code|<原因>
第3行  SIDE_EFFECT: <一句话>                    ⭐ 无副作用就写「无」
第4行  ROLE=<explore|synthesize|implement|review|merge>
第5行  EVIDENCE=<绝对路径,逗号分隔>              ⭐ 或 EVIDENCE=none
```
| 首行 | 含义 | 派发器 |
|---|---|---|
| ⭐ `WIP` | ⭐⭐ **我还在做** | ⭐ **续跑** |
| `TRIAGED` | ⭐ 停下等裁决 | ⛔ 停 |
| `DONE` | ⭐ 做完了 | ⛔ 停 |
⇒ ⭐⭐⭐ **还在做就写 `WIP`**（⭐ 写 `TRIAGED` 曾害一条棒空停 6 小时）
⛔ `DELIVERY_REF` **只认两种形态** —— ⭐ 自由文本（「那个分支上」）一律判违规。
⛔ `EVIDENCE` **必须绝对路径**（⭐ `cd` 后读错文件本战役已犯 5 次）。

⚠⚠ ⭐⭐⭐⭐⭐ **任务书给你相对的 `ops/…` 路径而没说【哪个仓】⇒ ⭐⭐ 停下来 advise ask，⛔ 别猜。**
⭐ 原因：⭐⭐ **两个仓都有 `ops/`** ——
```
⭐ 战役的  /root/cj_build/ops        ⇐ ⭐ CURRENT_DOCS.manifest · coord/ · design/ 的**权威**位置
   ⚠ ⭐⭐ 它常停在**别的棒的分支**上、带着**未提交改动** ⇒ ⛔ 你往里写就是碰别人的范围
⭐ cjcj 仓自己的 /root/cj_build/cjcj/ops   ⇐ ⭐ 也有 coord/ design/，⭐⭐ 且已有一份**同名** manifest
```
⇒ ⭐⭐⭐ **默认做法**：⭐ 文档/产物**交在你自己的树里**，⭐⭐ 由主控负责落位与登记；
  ⭐ 并在文件开头写一句「⭐ 待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`」。
⇒ ⭐⭐ 0831 一天内**三条棒**各为此问过一次 ⇒ ⭐⭐⭐ 这不是你的问题，⭐ 是任务书该写清。

⭐⭐⭐ **任何 fail-closed 守卫上线前**，必须先在真实输入集上预演并逐项列出被拦对象；
若命中正常输入，先修判据边界再启用。放行口必须有理由并留痕，⛔ 不能拿放行口掩盖过度捕获。

### ⭐⭐⭐⭐⭐ CLAIM 块（⭐ 协议核心，⭐ 四行紧挨）

```
CLAIM: <你的断言，一句话>
  METHOD: <read|measure|test|control-arm>
  EVIDENCE: <file:line | 绝对路径 | 可复跑的命令>
  N: <样本数>          ⭐⭐ METHOD=measure / control-arm 时**必填**
```
| METHOD | 用于 | EVIDENCE 该是 |
|---|---|---|
| ⭐ `read` | ⭐ 读源码 | ⭐⭐ **`file:line`**（⛔ 不是"我看过了"） |
| ⭐⭐ `measure` | ⭐ 跑出来的数 | ⭐ 结果文件绝对路径 + `N:` |
| ⭐⭐ `test` | ⭐ 由测试证明 | ⭐ 测试名 + ⭐⭐ **转红证据** |
| ⭐⭐⭐ `control-arm` | ⭐ 有对照臂 | ⭐ 两臂路径 + `N:` |

⛔⛔ **三条硬规矩**：
```
⭐⭐⭐ ① measure / control-arm ⇒ **必须给 N** —— ⭐ 单发不许当结论
⭐⭐⭐ ② 任何「没有 / 为 0 / 全绿」的主张 ⇒ ⭐⭐ **必须配一条阳性对照 CLAIM**
      ⇒ ⭐ 且「恒非 0 且很大」同样要对照（⭐ 尺可能已腐烂）
      ⇒ ⭐⭐⭐ **读数前先确认那个程序【真的运行了】** —— ⭐ 看 rc，⛔ 别只看 stdout
⭐⭐  ③ 涉及负载的数 ⇒ ⭐ 必带 **ELF sha256 + SO 血缘 + 核域 + 两端 uptime**
```

### ⭐⭐⭐ `## FALSIFIED` 节 —— ⭐ **必须存在**（⭐ 没有就写「无」）

⇒ ⭐⭐ 任务书里被你证伪的前提，⭐ 逐条列出带证据。
⇒ ⭐⭐⭐ **推翻前提【加分】** —— ⭐ 主控的「疑似缺」已被棒实核 **12 次全错**。

### ⭐⭐ 角色专属

```
⭐ explore    ⇒ ⭐ DELIVERY_REF 应为 `none|no-code|…`；⭐ 交付含**统一对齐表**（⭐ ✅/⚠/⛔ 三档）
⭐ synthesize ⇒ ⭐ 交付含**可直接派的实现任务书** + ⭐ 矛盾消解记录
⭐⭐ implement ⇒ ⭐⭐⭐ **DONE 时必须至少一条 `METHOD: test`**（⭐ 精确转红，⛔ 一破全红=过度捕获）
⭐⭐⭐ review  ⇒ ⭐ 必须明写「**放行**」或「**打回**」+ ⭐⭐ 两栏「新的放进来什么 / 旧的挡住了什么」
⭐⭐ merge     ⇒ ⭐ 必须给**逐文件内容核**结果；⭐⭐⭐ `merge-tree` 报干净**对静默回退是盲的**
```

⭐ commit 作者 `Zxilly <zxilly@outlook.com>`，semantic 前缀 + **ZGC file:line 锚**，⛔ **无任何 AI 署名**。
⚠ ⭐ 已有 5 轮因超时颗粒无收 ⇒ ⭐⭐ **边做边写**，⭐ 只是首行用 `WIP`。

## ⭐ 四·半 · 实现棒交付前必跑（0909）

```
tools/entry_cut_check.py --repo <工作树> --base <基线sha> --head <候选sha> --cut cut.diff --phase-entries /root/cj_build/ops/coord/PHASE_ENTRIES.txt --json <证据目录>/entry_cut_check.json
```
断开产品调用点的补丁必须断在基线里已存在的产品行上（不是你新加的行），且至少一处在真实相位入口函数；rc≠0 别送审。细则见 DELIVERY_PROTOCOL.md §2.3.1–2.3.2。


### 审查尺度分档（0909，用户裁）
- 产品代码（runtime / cjcj / llvm）：对抗式，尺度不变。
- 基础设施与临时工具（tools 仓、门脚本、编排器）：审查范围＝任务书写明的不变量＋真实历史故障类。超出运行包络的边界情形（例如 2^53 轮次号、毫秒级时钟差）**记录为已知限制，不作为打回理由**；打回必须指向会在日常使用里出现的行为。

## ⛔ 五 · 硬边界

- ⭐ **可以、也应该把自己的候选分支 `sym/<n>-<stage>-r<run>` 推到 cjcjdev 并开/复用 PR**（0908 用户令；0913 起返工轮推同一分支）；⛔ **不 push 主分支、不动 main**（⭐ 那是合并 agent 的事）。⭐ 沙箱若拒推，交 DONE 并写明，主控代推。
  ⇒ ⭐⭐⭐ **`ROLE=merge` 的棒就是那个合并 agent**：组合门绿（⛔⛔ ⭐⭐⭐ **0907 起：`NWDET_BASELINE_DEVIATIONS` 差分档的 `GATE_ALL_OK` 【不再独立构成准入证据】，见下方 D-6 注**）之后，push 与 `pin_delivery` 是它任务书里的职责项，⭐ 本条硬边界对它不适用；⭐ 0902 用户令「推送总是批准」：门绿+过审后推送不再逐次问用户

⛔⛔ ⭐⭐⭐⭐ **D-6（gpt-6 Q65-B，0907 15:1x 起生效）**
> **旧差分 nwdet 的整数比较及 `GATE_ALL_OK` 不独立构成准入证据。必判执行无资格或资格不可证时，不得据此合并放行；只有两臂资格闭合，才允许原合同内差分比较。任何带缺验集成必须另获绑定具体提交的明确例外，且不得记为验收通过。**
⭐ 债项全文见 `/root/cj_build/ops/coord/INTEGRATION_DEBT.md` 的 **D-6**；
⭐ 引用该段结果一律用新记法：「旧尺输出 NOT_WORSE（仅偏差计数比较）；负载有效完成条件不满足，nwdet 验收不可判，不构成合并通过证据。」

- ⛔ **不改共享 SDK**：`/root/sdks/**`、`/root/.cjv/**`（⭐ 定制走 `cp -a` + `cjv toolchain link`）
- ⛔ **不碰其它棒的范围**（⭐ 撞车报给主控）
- ⛔ **别对 `/root` 做 `grep -R`**（⭐ 有跑 22.6 小时占满一核的前科）
- ⛔ **禁 `pkill`/`pgrep` 宽模式**（⭐ 观察者会匹配到自己）

## ⭐⭐ 六 · 编译与测量一律去 kkk2

```bash
bash /root/cj_build/tools/box.sh kkk2 '<命令>'
bash /root/cj_build/tools/box.sh kkk2 --put <本地> <远端>   # --get 反向
```
⭐⭐⭐ runtime 冷建固定复用 kkk2 的内容缓存，CMake 配方必须含：
```bash
-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_ASM_COMPILER_LAUNCHER=ccache
export CCACHE_DIR=/root/.ccache
export CCACHE_BASEDIR=<runtime仓根>
export CCACHE_NOHASHDIR=1
export CFLAGS="$CFLAGS -ffile-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fdebug-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fmacro-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime"
export CXXFLAGS="$CXXFLAGS -ffile-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fdebug-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fmacro-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime"
export ASMFLAGS="$ASMFLAGS -ffile-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fdebug-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fmacro-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime"
ccache -M 50G
```
⭐ `CCACHE_DISABLE=1` 是冷建阳性对照；缓存身份只认编译内容，⛔ 不拿树路径或 mtime 当 key。
⭐⭐ 组合门调用 `/root/gate_all.py` 时，主线臂显式传 `GATE_ARM=main` 才可读写
`/root/gate_cache/<main_sha>/<构型>/result.json`；候选臂传 `GATE_ARM=candidate`，⛔ 永不复用。
命中日志必须含 `GATE_MAIN_ARM=cached sha=… age=…`；result 或产物哈希不符即重跑。
⚠ ⭐ 长任务 `setsid nohup … &` 立即返回，⛔ 别内嵌 while-sleep 等待循环。
⚠ ⭐⭐ **别绕过 `box.sh` 直接连**（⭐ 有棒连到已 RETIRED 的机器，⭐ 整轮 BLOCKED）。
⭐ SO 走 sodepot `/root/sodepot/<sha>/` ⇒ ⭐⭐ **runtime 与 boundscheck 两个都要**（⛔ 只拿一个 rc=127）。
⭐ 核域用 `cjops windows` 领；⭐ 测量前后各记 `uptime`（⭐⭐ **load 是一等判据**）。

### ⭐⭐⭐ 六·半 · kkk2 192 核：并发是默认，串行要说理由（0910 用户令）

```
⭐ `box.sh` 已给每条 kkk2 命令注入 `CANGJIE_BUILD_JOBS=192`、`GC_UNIT_JOBS=192`、`CMAKE_BUILD_PARALLEL_LEVEL=192`，并把 `/usr/lib/ccache` 放到 PATH 前（0913 09:4x 起；clang/clang++ 经 ccache，base_dir=/root ⇒ 不同棒目录也命中）
   ⇒ `python3 build.py build -t release` 自动 -j192；⛔ 别再手写 `-j4`/`-j8`/`-j64`；cmake 直建不写 -j（走 192）或写 `-j$(nproc)`；⛔ 自写的 build 脚本同样不许写死小于 nproc 的 -j
⭐⭐ 绿臂 / 切刀臂 / 恢复臂是**互相独立**的构建 ⇒ 用**不同构建目录并发跑**（`setsid nohup … &` 各起一份，`wait` 收），
   ⛔ 不许一个建完再建下一个；产物身份仍按各自目录的 sha256 记
⭐⭐ gc_unit 测试：`run_standalone.sh` 并发化（#116）落地后按用例并发；落地前也要把**两枚 ELF**并发跑、
   多臂的测试并发跑；单臂内部想顺序跑必须在报告里写理由（共享状态/端口）
⭐ 量尺：每个构建/测试步骤记 `wall=`；报告里若有任何 wall>10min 的顺序步骤，写明为什么不能并发
⭐⭐ load 仍是一等判据：并发前后各记 `uptime`；192 核上 load>150 再往上加就是自伤
```

### ⛔⛔ ⭐⭐⭐ 本机 `/tmp` 是 **24GB 的 tmpfs（内存盘）** —— ⛔ 别把 GB 级产物留在那

```
⚠ ⭐⭐⭐⭐ 0901 实付：⭐ 它被历轮棒的构建/证据目录填满到 **100%**
   ⇒ ⭐⭐ 后果不是「磁盘告警」，⭐⭐⭐ 而是**主控自己的工具静默失灵** ——
     `cjops lanes` 的输出写不下 ⇒ 返回空 ⇒ ⭐ 判活、判拦截全部读到「什么都没有」
   ⇒ ⭐ 当时是分类器哨兵的「判不了」档把它暴露出来的，⛔ 否则会被读成「一切正常」
```
⭐⭐⭐ **规矩**：
```
⭐ 中间产物 / 构建树 / 证据归档 ⇒ ⭐⭐ 放 kkk2，⭐ 或本机 `/root/<你的棒名>-*/`
⭐ `/tmp` 只放**小的、当轮就用完**的东西（⭐ 单个 < 100MB）
⭐⭐ **收尾时清掉自己在 /tmp 下建的目录** —— ⭐ 要留的证据先取出来
⭐ 开建前顺手 `df -h /tmp`；⭐⭐ 低于 3GB 就先清自己的，⛔ 别硬上
⛔ **别删别人的** —— ⭐ `/tmp/opencode`（agent 状态）与 `/tmp/claude-0`（主控会话）⛔ 一律不碰
```

## ⭐⭐⭐ 七 · 测量纪律（⭐ 本战役翻车最集中的一族）

```
⭐⭐⭐ **恒 0 / 恒 false / 恒绿 是高危信号，⛔ 不是好消息**
   ⇒ ⭐ 读任何计数前先问：**「我验过这个计数【会变】吗？」** ⇒ ⭐ 做阳性对照
   ⭐⭐⭐ **对偶同样成立**：⭐「恒非 0 且很大」也要先做对照（⭐ 尺可能已腐烂）
⭐⭐⭐ **读数之前先确认那个程序【真的运行了】** ⇒ ⭐ 看 rc，⛔ 别只看 stdout
   ⇒ ⭐ 实账：⭐ 一条棒 72 分钟耗在 `LD_LIBRARY_PATH` 缺失、⭐ 从未启动的程序上，⭐ 而它一直读到 `0`
⭐⭐⭐ **只在【被多发证明确定】的档上断言；⭐ 其余一律只观测**
⭐⭐ 新加的守卫/测试**必须能真的失败** ⇒ ⭐ 给**故意破坏后转红**的证据
   ⇒ ⭐⭐ 且要**精确转红**（⛔ 一破全红 = 过度捕获）
⭐⭐ 判据写**不变量**，⛔ 不写机制
⭐⭐ 对照臂要证明「特性不存在」⇒ ⭐⭐⭐ 最强证据 = **full `nm --defined-only` 符号 0 命中 + sha256**
   ⚠⚠ ⭐⭐⭐ **⛔ 别用 `nm -D`**：它只读动态符号表，对 `STB_LOCAL`（static/内部链接）**盲**。
     ⭐ 0831 实测阳性对照：static 函数 `nm -D`=**0** / `nm --defined-only`=**1**；公开符号两把尺都=1
⭐  `si_addr=0` 二义 ⇒ ⭐⭐ **必须同时读 `si_code`**（1=MAPERR · 2=ACCERR · 128=非规范地址）
⭐  下「颜色」结论前先对位布局：⭐ bits 48-49 首先是 `StateWord.stateCode`（FORWARDED=3）
⚠ ⭐⭐ **本机 `grep` 是 shell 函数（包 ugrep）** ⇒ ⭐ 在含 emoji 的管道流上 `-c` **什么都不输出**
   ⇒ ⭐⭐ 对文档/报告计数一律用 `/usr/bin/grep`
⚠ ⭐  `git grep -E` 配 `\|` 是**字面竖线**，⛔ 不是「或」
```

## ⭐⭐⭐⭐⭐ 七·半 · **立「应为 N / 应为空」这类判据之前**（⭐ 0901 一天栽两次，⭐⭐ 两次都是主控栽的）

```
⭐⭐⭐⭐ **绝对判据（⭐「命中数应为 0」「⭐ 必须 16/25 绿」）依赖两样你多半没核过的东西**：
   ⭐ ① **作用域里的既有内容** —— ⭐⭐ 换一棵树/一个目录，⭐ 同一条判据可能恒红或恒绿
   ⭐ ② **产出那个数的那把尺** —— ⭐⭐ 换一个 runner，⭐ 同一个套可能是 16 也可能是 22
```

⭐⭐ 0901 两笔实账（⭐ 都由**下级棒**发现，⛔ 不是主控自己）：
```
⭐ ① 主控把 CLAUDE.md 里 `grep -rn '<<<<<<<' runtime/src/` 这条**原样换作用域**到 `llvm/lib`
   ⇒ ⭐⭐ 命中 5 条**上游 LLVM 自己的注释** ⇒ ⭐⭐⭐ 「应为空」当场被证伪
⭐ ② 主控把审查报告里「⭐ restored 臂 16 PASS / 9 预失败」写成**硬基线**
   ⇒ ⭐⭐ 合并棒用自己的 runner 实测 **22/25**（⭐ 差异全在 6 个 `RUN: not not opt` 的双重取反）
   ⇒ ⭐⭐⭐ 而**两把都不是权威 harness** ⇒ ⭐ 「16 对还是 22 对」问错了问题
```

### ⭐⭐⭐⭐ 解法：**写差分判据，⛔ 别写绝对判据**

```
⛔ 别问 ⇒ ⭐「⭐ 现在命中几条 / 现在几项绿」
⭐⭐⭐ 要问 ⇒ ⭐⭐⭐⭐ **「⭐ 改动前后的【差集】是不是空（或恰好是我预期的那一项）」**
   ⇒ ⭐⭐ 两臂**必须用同一把尺、同一棵树、同一份构型**
   ⇒ ⭐⭐⭐ 这样它对「⭐ 基线里有什么」和「⭐ 尺本身有多准」**都免疫**，
     ⭐ 而且正好回答真正要问的那个问题：⭐⭐⭐ **「这次改动有没有弄坏别的东西」**
```

### ⭐⭐ 自检一问（⭐ 写下任何数字之前）

```
⭐⭐⭐⭐⭐ **「这个数是【测出来的】还是【推出来的】？⭐⭐ 换一把尺 / 换一个目录，⭐ 它还成立吗？」**
   ⇒ ⭐ 答不出 ⇒ ⭐⭐⭐ 就该写成差分判据
   ⇒ ⭐⭐ 从别人报告里**抄数字或抄命令**时，⭐⭐⭐ **必答「它是哪把尺、在哪个作用域产出的」**
⭐⭐ 若你收到的任务书里就写着一条绝对判据，而你实测对不上 ⇒
   ⭐⭐⭐ **⛔ 别调整期望值去凑、⛔ 也别改 harness 口径** ⇒ ⭐ 停下来走 advisor 通道，⭐⭐ 并带上你的实测数字

## ⭐⭐ 八 · 跨系统对照（⭐ ZGC 移植期高频）

```
⭐⭐⭐ 参照实现出现「两个变体/两个分支」⇒ ⭐⭐ **必须去【调用层】查它实际用哪个**
   ⛔ 别从名字或注释推 ⇒ ⭐ 实账：⭐ 有一项**两个变体在 ZGC 自己那边都零实例化**
⭐⭐ 机制是**成对**的（⭐ 生产端 ⇄ 消费端）⇒ ⭐ 只比一端必错
⭐⭐⭐ **「grep 零命中」⛔ 不等于功能不存在** ⇒ ⭐ 按**功能**找，⛔ 别按名字找
⭐⭐ **读【为什么】，⛔ 不只读【是什么】**
   ⇒ ⭐ 实账：⭐ ZGC 自研锁是为绕开 HotSpot Mutex 的 rank+safepoint 协议，
     ⛔ 不是 `std::mutex` 缺功能 ⇒ ⭐⭐ 我方走握手 ⇒ **该动机不成立 ⇒ 别照抄**
```

## ⭐ 九 · 用户令（⭐ 与你直接相关的）

⭐ bundle / tar / 源码副本一律放 kkk2 或 scratch，用完即删；`/tmp` 只放 <100M 短命文件。

```
⭐⭐⭐ **不要无意义的压力测试，⭐ 先读代码把逻辑对齐**
⛔⛔ 不许靠重复跑负载撞运气找 bug ⇒ ⭐⭐ **信号稀有就去【构造】它**
⭐⭐  完全按 ZGC 来 ⇒ ⭐ 遇 bug 先看 ZGC 怎么做（⭐ 给 file:line 锚）
⭐⭐  **不需要保持官方兼容** ⇒ ⭐ 别因"兼容"自我设限
⭐⭐  测试同批移植
⛔  不许用性能门否决类型/结构装置
⚠ ⭐⭐ **措辞用工程用语** ⇒ ⛔ 别写「读悬垂/崩/毒化/UAF」（⭐ 会触发外置分类器，⭐ 整轮被拦）
```

## ⛔⛔⛔ 提问之后**必须保持 `PROGRESS=WIP`**（⭐ 0831 第二次实付）

```
⭐⭐⭐ `cjops advise ask` 之后**不许转终态**（⛔ 不许 `TRIAGED`、⛔ 不许 `DONE`）
   ⇒ ⭐⭐ 派发器看到终态就**停你** ⇒ ⭐⭐⭐⭐ **那条裁决永远送不到**
⚠ ⭐⭐ 实付两次：
   ⭐ `merge_runtime5` 在答复落盘前 **4 分钟**转终态
   ⭐⭐ `impl_hostchain_official_llvm_c`（0831 18:21）在答复落盘前 **10 秒**转终态
     ⇒ ⭐⭐⭐ 它甚至在报告里写了「⭐ 当前报告保持 TRIAGED」⇒ ⭐⭐⭐⭐ **以为 TRIAGED 是「等裁决」的意思**
     ⇒ ⛔⛔ **不是。** ⭐ `TRIAGED` = 「⭐⭐ 我做完了，⭐ 停下等人看」⇒ ⭐⭐⭐ 派发器据此**结束**你
⇒ ⭐⭐⭐⭐⭐ **等裁决的正确写法是 `PROGRESS=WIP`** ——
   ⭐ 在 verdict 里写「⭐⭐ 已提问，等 outbox 答复」即可，⛔ 首行三态不许动
```

⭐ 主控侧的反制（⭐ 告诉你，⛔ 不用你做）：⭐⭐ 答完 advisor 后会验你还活着，⭐ 已终态就 `--resume` 复活。

## 找文件先看地图
先读 `/root/cj_build/ops/design/RUNTIME_ZGC_FILE_MAP.md`：我方 `runtime/src` 文件 ⇄ ZGC 文件的对应表，以及「ZGC 无对应物＝删除对象」清单。按表找到 ZGC 文件后逐函数对照，再动手。

## ⭐⭐⭐⭐⭐ kkk2 上的构建与测试必须把 CPU 跑满（0913 09:3x 用户令）

- kkk2 192 核，实测 load average 常年 <10 ⇒ ⭐ 浪费。
- 构建一律 `cmake --build … -j$(nproc)`（=192）或 `ninja -j$(nproc)`；⛔ 不写死 -j12 / -j64。
- 两构型（default / testable）**同时**构建，各用独立 build 目录，不串行。
- 跑测试时把互不共享目录/端口的臂并行起来（gate_unit 已两臂并发；自己起的 fixture 也要并行）；单个用例超时不因并行放宽。
- 交付报告写出实际用的 -j 值与并行臂数；load average 只作过载判据（>150 时才降并发）。

## ⭐⭐⭐⭐⭐ 打回后的返工轮在【同一条候选分支、同一个 PR】上继续（0913 11:3x 用户令，tools bcb3970 起生效）

- 返工轮的工作树由编排器直接检出条目的候选分支（`Delivery-ref` 里的 branch，HEAD＝上一轮候选 head），⛔ 不再从冻结主线另起分支。
- 你只需在其上继续提交并推同一分支；PR 复用（编排器/主控会关掉被替代的旧 PR）。⛔ 不要 cherry-pick「承接前轮」、⛔ 不要 merge/rebase 主线。
- 交付的 DELIVERY_REF 仍写 `repo|branch|head`，branch 就是这条候选分支。
- 主线前进导致合并冲突时，主控会给「接回」说明，那时仍在这条分支上按内容重落，不换分支。
