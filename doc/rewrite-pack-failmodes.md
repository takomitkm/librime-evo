# rewrite store/pack 持久层：失效模式登记表

范围：`RewriteStore/Writer`（多段存储 + workspace 部署协议）、`RewritePackBuilder/RewritePack`
（pack 二进制格式）。这是候选重写功能的落盘层：运行时 `gear/rewriter.cc` 读 `.rwp`，
部署侧 `lever/rewrite_compiler.cc` 写；本轮之前**四个类零直接测试**（packs_test 只测部署管线）。
测试位置：`test/rewrite_store_test.cc`。

**注入有效性对拍前置**（Iron Law）：基线全绿 → 单个变异提交（V1–V6，文末）→ 对应用例红 →
revert 复绿。命中关系：V1→S8、V2→S2、V3→S4、V4→S3、V5→S5、V6→P2。

## 状态家族

| 家族 | 角色 |
|---|---|
| store 文件（128B 双超块 + 8B 对齐对象区：对象头 64B + pack 载荷 + 索引） | 唯一持久态；超块=提交点，committed_size 之内才是已提交数据 |
| 进程级 workspace 注册表（全局互斥 + 按 path 单例） | 部署事务的内存侧；abort 靠它回滚物理文件 |
| pack 载荷（RIMERWP1 格式：dispatch 页 + darts trie + 值池） | 不可变对象；mmap 读 |

关键机制事实（读码确认）：**AppendStage 直接写物理文件**（不经内存缓冲），
提交点 = 超块翻块（generation%2 槽位 + fsync）；崩溃残留 = 文件长于 committed_size 的尾巴，
由 Writer::Open 与 BeginWorkspace 截断回收；abort = 恢复超块头 + resize 截断
（RestoreRewriteWorkspace）；空快照提交 = 删除整个 store 文件。

**Writer 调用序契约**（rewrite_compiler.cc:628-634 定）：一次性 writer 只调
`BeginWorkspace`（注册进程级 workspace），真正干活的 writer 以 `Open()` 加入后才进入
事务。反序调用（Open 先于 BeginWorkspace）事务不会挂上——CommitSchema 会静默直提、
CommitWorkspace 恒 false 且注册表条目滞留（按协议以 AbortWorkspace 收尾可清）。
本表全部用例按生产协议书写；反序行为的进一步收紧（显式报错）列为对上游的建议，不在测试内钉死。

## 登记

| 编号 | 类别 | 机制（会怎么坏） | 结果类别 | 观察 | 用例 |
|---|---|---|---|---|---|
| S1 | 自身 bug | 全链路往返失真：binding 顺序/内容、generation、stage 定位错 | 损坏 | 重开读回逐项对拍 | WorkspaceCommitRoundTripAndBindings |
| S2 | 失控写入 | MarkWorkspaceFailed 后仍部分提交；abort 残留 .tmp | 损坏 | 拒交 + 旧态完好 + 无临时文件 | WorkspaceFailureRefusesPartialCommitThenAbortRestores |
| S3 | 自身 bug | AppendStage 同 id 重复写入（不幂等）/空 stage 收下 | 损坏 | 幂等（字节数不变）+ 空 entries 拒绝 | AppendStageIdempotentAndEmptyRejected |
| S4 | 丢持久性/失控写入 | abort 不截断物理文件 → 垃圾尾巴永久化 | 假性损坏 | abort 后 file_size==committed_size | AbortWorkspaceTruncatesPhysicalFile |
| S5 | 崩溃残留 | 上次进程崩溃留下的未提交尾巴不回收 → 后续解析踩界 | 损坏 | Writer::Open 后 file_size==committed_size | UncommittedTailDiscardedOnWriterOpen |
| S6 | 失控写入（GC） | RetainSchemas 错删/漏删；空 store 删除契约 | 丢数据 | 死 schema 不可见、活 schema 完好；空提交删文件 | RetainSchemasDropsDeadSchemas |
| S7 | 失控写入 | 代际不推进/干净提交仍有写入（指纹漂移） | 假性损坏 | generation==2 且 file_size 不变 | SecondCommitAdvancesGenerationAndCleanCommitWritesNothing |
| S8 | 环境非标（损坏） | 索引字节损坏被静默接受 → 错误 stage 定位 | 损坏 | Open/OpenCached 干净失败 | CorruptIndexRejectedAndOpenFailsCleanly |
| S9 | 自身 bug | CommitSchema 收下未暂存/畸形绑定 → 索引指向不存在对象 | 损坏 | 四种畸形全拒绝 | CommitSchemaValidatesBindings |
| S10 | 并发与锁 | workspace 重入/并发 Writer 抢占 | 损坏 | 重入拒绝；第二 Writer 加入现 workspace | WorkspaceReentryGuardAndSecondWriterJoins |
| P1 | 自身 bug | pack 往返失真：多值顺序、preedit、miss 语义 | 损坏 | 逐 (value,preedit) 对拍 | BuildThroughStoreRoundTripWithPreedit |
| P2 | 自身 bug | Build 输出混入非内容因素（时间/指针序）→ 部署指纹漂移 | 假性损坏 | 同输入两次 Build 字节相同 | BuildOutputIsDeterministic |
| P3 | 环境非标（损坏） | 载荷字节损坏被 mmap 静默接受 → 错误候选 | 损坏 | 翻转载荷首字节 → OpenStage 拒绝 | CorruptedStagePayloadRejectedOnOpenStage |

## 排除项（明确不测，含理由）

- **掉电/崩溃注入三件套（真·故障注入）**：无注入基建；S4/S5 以"手工构造崩溃残留
  （超出 committed_size 的物理尾巴）+ 断言回收"作轻量替代，真注入留待基建。
- **ConvertSentence/Incremental 句子级语义**：运行时热路径、依赖 darts 遍历细节，
  语义面大；packs 管线测试已间接触达。独立覆盖列为后续。
- **OpenCached 进程级缓存的并发失效**：多线程部署竞争无法在单线程套件里稳定复现；
  缓存按 generation 失效的语义已由 S7 的代际推进间接覆盖。
- **ShouldCompact/CompactStoreFile 压缩路径**：触发阈值语义未读透，避免拍脑袋断言；
  durable-rename 安装原语属 rewrite_internal，若展开应单独登记。
- **FindStageSequence 对不存在 schema/section 的负路径**：S1 已含一例（schemaB miss），
  不逐组合展开。

## 句子级转换（T 组，运行时热路径）

契约（读码定）：贪心最长短语匹配压制单字回退（同首字时短语优先）；短语探针失败回退单字；
多值键句中只取首值；value==源文本 的恒等替换不计 changed；不可匹配段按 UTF-8 标量
整段原样保留；探针停在输入末尾（可能被后续追加延长）为 append-sensitive——决策不缓存、
由 frontier 记录续探点，纯追加时从断点续探；**增量转换必须与一次性转换逐字节等价**
（对拍 oracle，归一化规则见 T4/T6）。**惰性拷贝契约：一次性 ConvertSentence 在
changed=false 时 result 保持为空**（调用方只在 changed=true 时读结果），而增量路径的
state.output 始终持有全文——等价对拍须按此归一化。测试挂 `test/rewrite_store_test.cc`
的 RimeRewritePackTest。

| 编号 | 类别 | 机制（会怎么坏） | 观察 | 用例 |
|---|---|---|---|---|
| T1 | 自身 bug | 贪心失效：短语键被单字回退拆碎 | "abc"→"ABC" 而非 "甲b"+"c" 类碎片 | GreedyLongestPhraseBeatsSingleChar |
| T2 | 自身 bug | 短语探针失败后不回退单字/不原样续扫 | "axb"→"甲xb" | PhraseMissFallsBackToSingleChar |
| T3 | 自身 bug | 句中转换不取首值 | 多值键 "a" 句中取甲 | SentenceUsesFirstValue |
| T4 | 自身 bug | 恒等替换误报 changed（假变化→多余重绘） | "x"→"x" 返回 false | IdentityReplacementNotChanged |
| T5 | 自身 bug | 不可匹配段丢失/UTF-8 多字节截断 | "q界r"→"q界界r"；"qwr" 原样且 false | UnmatchableRunsVerbatimUtf8 |
| T6 | 自身 bug | 增量与一次性不等价（前缀复用错位/frontier 状态错） | 固定种子 200 步随机序列对拍 | IncrementalMatchesOneShot |
| T7 | 自身 bug | append-sensitive 续探丢匹配/重复消费 | "a"→"ab" 续探得 AB | FrontierContinuationResolvesAppend |
| T8 | 自身 bug | 相同输入快速路径失效 | identical_input_hits 递增且结果稳定 | IdenticalInputUsesCache |

句子组变异（W1–W6，校机实测归因）：W1→T1；W2→T4（波及 T2：恒等恒真使 "xb" 报 changed）；
W3→T7（波及 T6：去掉 output 截断使旧输出累积）；W4→T8（波及 T6）；W5→T5（波及 T6）；
W6→T3（把句中取值从首值改成末值；波及 T2/T4/T7，三者都依赖 "a" 取首值）。
T6 是等价对拍，任何语义漂移都会打红它——它没有"专属"变异，属设计使然。

## 压缩阈值（G 组）

契约（读码定）：`ShouldCompact` 双条件——死 stage 数 ≥16，或垃圾字节 ≥4MB **且** ≥
已提交大小的 1/4；两者皆不满足时只追加索引、死数据保留（惰性，见 S6）。
`CompactStoreFile` 只复制 live stage（逐个复算 payload 哈希校验）、写临时文件 +
fsync + 原子改名，然后在新文件上补写一个镜像超块。压缩后死 stage 从索引消失
（OpenStage 返回空），live stage 内容逐字节保留，代际 +1。

| 编号 | 类别 | 机制（会怎么坏） | 观察 | 用例 |
|---|---|---|---|---|
| G1 | 失控写入 | 未达阈值就压缩（抖动/白耗 I/O），或该压缩不压缩（文件无限膨胀） | 2 死 stage 小型 store 只增长不收缩，死 stage 仍可开 | CompactionStaysLazyBelowThreshold |
| G2 | 失控写入（GC） | 死 stage 数达阈值不回收 → 死数据永久滞留 | 16 死 stage 后文件显著缩小、死 stage 从索引消失、live 内容不变、代际 +1 | DeadStageThresholdTriggersCompaction |
| G3 | 失控写入（GC） | 只看死 stage 数、忽略垃圾比例条件 → 大 store 反复重写或永不回收 | 15 死 stage 但垃圾 ≥4MB 且 ≥1/4 → 收缩到不足一半 | GarbageRatioTriggersCompaction |

## 变异点（红绿对拍用）

| 变异 | 位置 | 期望变红 |
|---|---|---|
| V1 | rewrite_store.cc ParseStoreIndex：去掉 index_hash 校验 | S8（检测面=schema 名字节：解析层容忍的垃圾名，结构校验全放行，只有哈希拦得住；结构位翻转由解析层兜底，不构成 V1 的红） |
| V2 | rewrite_store.cc CommitWorkspace：去掉 WorkspaceFailed 拒交 | S2 |
| V3 | rewrite_store.cc RestoreRewriteWorkspace：去掉 resize 截断 | S4 |
| V4 | rewrite_store.cc AppendStage：去掉 HasStage 幂等短路 | S3 |
| V5 | rewrite_store.cc Writer::Open：去掉未提交尾巴截断 | S5 |
| V6 | rewrite_pack.cc BuildRewritePack：注入递增计数器字节 | P2 |
| W1 | rewrite_pack.cc ConvertSentence：去掉短语优先分支 | T1 |
| W2 | rewrite_pack.cc ConvertSentence：replacement_changed 恒置 true | T4 |
| W3 | rewrite_pack.cc ConvertSentenceIncremental：去掉 output.resize(output_end) | T6 |
| W4 | rewrite_pack.cc ConvertSentenceIncremental：去掉相同输入快速路径 | T8 |
| W5 | rewrite_pack.cc ConvertSentence：changed 后丢弃不可匹配段 | T5 |
| W6 | rewrite_pack.cc GetFirstValue：句中改取末值 | T3 |
| X1 | rewrite_store.cc ShouldCompact：去掉 dead_stages>=16 条件 | G2 |
| X2 | rewrite_store.cc ShouldCompact：去掉垃圾量/比例条件 | G3 |
| X3 | rewrite_store.cc CompactStoreFile：跳过 InstallAtomically | G2、G3 |

红绿闭环记录（校机 WIN11-HV1，msvc-free mingw64）：基线 324/324 →
W1–W5 单轮 7 红（5 绑定 + 2 波及）→ W6 单轮 4 红（1 绑定 + 3 波及）→
还原 324/324 复绿。八个 T 用例各自至少红过一次（T2/T6/T7 由波及提供红）。
