待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。

# #1478 固定 ARM64 未执行阶段

坐标基于 `e8d8e9d4fea011b8b943ca4dc696ffc1f974f82f`。
此目录仅用于 run **37080734066 attempt 1** 的两个已成产物。
workflow `metadata-arm64-remaining.yml` 只有 workflow_dispatch；没有 push、PR、schedule 或 workflow_run 触发。
主控须先独立核接线，再绑定本次完整执行 SHA、唯一 dispatch 事件和绝对 UTC deadline；准备此代码本身不授权执行。
原 source_head 永远是 e8d8，执行 head 单列，不修改原 receipt。

| 构型 | artifact ID | 顺序 |
|---|---|---|
| default | 11259325217 | 四 fixture reader → 一次原 shape consumer |
| testable | 11259310233 | 四 fixture reader → 一次原 run.py supervisor → 一次原 shape consumer |

两官方 ubuntu-24.04-arm runner，fail-fast=false，独立输入/副本/结果；各目标原 120 秒看门狗，无重试，无切刀/恢复。
首错后下游 NOT_RUN。全文件清单必须无新增/缺失/哈希差异；原 receipt、run/attempt/head/config/artifact ID/API expiry 一并核验。
不使用旧 full/a2-resume，不编译、链接、签名或重新运行生成器。
仅实体复制固定四 fixture、shape、三产品库；testable 额外复制原 metadata。
运输后的 ELF 执行位恢复单列 source/copy mode，内容哈希必须保持。
LLVM18 reader 必须按显式绝对路径存在、可执行、实际 --version rc0、版本18和实体 SHA256 后用于四次 reader 命令。
包管理准备不替换产品编译器；权限/API失败直接保原文停，不使用 repository variables。

原 `build.py` 和 `run.py` 不改。固定 artifact 的源文件先通过完整清单；shape consumer 从原 build.py 的唯一 `a2_run_bundle` 函数取出并逐字执行（有界 AST 函数选择，既有顶层构建/CLI 不执行）。
保留 INPUT、DESCRIPTOR、TARGET 等原断言；ADDRESS/逐 byte 的完整检查由原 shape ELF 内部断言负责；不写第二份宽松判词。
原 supervisor 连同校准/拒绝控制全部运行一次。
新 `a2-candidate.json` 只描述副本供原 consumer 读；不是原 receipt，不冒充原 run 上传身份。

新 runner 独立记录 AArch64、4KB页、Image、GNU loader 实体及哈希、前后 uptime/load、每次命令/rc/wall、原/副本前后哈希。
静态 ldd、实际 LD_DEBUG loader init 记录、原始 /proc maps 三类分开保存；maps 是机会性观察，短进程未捕获明确 NOT_CAPTURED，禁止反推。
实际 loader init 的系统库实体哈希另记；supervisor 的 RUNTIME_MODULE 仍受原判词约束。
首次安装/下载失败的原始日志在 GHA，always 上传输入及结果（若下载前失败，可能无实体可上传，仍保留 workflow 失败）。
artifact 是否仍可下载只在获准的新事件核，不从原成功下载推当前可达。
所有未执行、原首错、缓存缺件、ABI/最终 tuple/Apple/A1/#135/stage33/release hold 保持。
