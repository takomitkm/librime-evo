# librime-evo 测试问题清单

上游：[rime/librime](https://github.com/rime/librime)（正主）·
[amzxyz/librime-evo](https://github.com/amzxyz/librime-evo)（本仓库的来源）。
本分支 `test/manual-order-contracts` 是测试分支；测试代码在 `test/`，
每条失效模式的完整登记与变异归因在 `doc/*-failmodes.md`。

## 导读（先看这里）

**查出了什么**：67 个契约点、约 30 次变异攻击，**没有发现源码 bug**——
所有"红"最终归因到测试自身的假设错误。本文记录的是锐边、契约和教训，不是 bug 单。

**问题在哪**：

| 类别 | 位置 | 现状 |
|---|---|---|
| API 锐边 2 则（改进建议） | 本文第一节 1、2 | **暂不提上游**（决策见下） |
| 有意不兼容 1 则 | 本文第一节 3 | 设计决策，不做兼容 |
| 设计契约 4 则 | 本文第二节 4-7 | 调用方须知，非 bug |
| 测试基建教训 4 则 | 本文第三节 8-11 | 写测试的人看 |

**测了什么**：四节覆盖表列出的 7 个切片 67 个用例（`test/*.cc`），
全部经校机红绿闭环、每个用例亲眼见过红；每条失效模式绑定登记表
（`doc/*-failmodes.md`，含变异归因）。

**没测什么（排除项及理由）**：各登记表的"排除项"小节；汇总——
故障注入三件套（OOM/掉电/IO，需注入基建）、多进程部署并发、
Synchronize 多设备编排、16MiB 单行上限、user_rank_actions 编辑动作
（需 Engine/Context 交互）、RankReading 同权稳定序细节、
TopWeightsByCode 与 script_translator 的集成、symlink 逃逸
（SafePath 系列已覆盖）、大改的既有文件内部
（gear/memory.cc、script_translator.cc、deployment_tasks.cc——有间接覆盖，
单独切片待续）。

**决策记录**：

- 上游 1.x 用户库不兼容（第一节 3）是**有意设计**（新五槽格式是新架构的前提），
  不做 legacy reader、不发 issue。
- API 锐边 1（writer 调用序静默旁路）**暂不提上游**；锐边 2（失败后滞留）
  跟随 1 一并搁置。两条已完整记录在案，将来要提时直接引用。

---

## 一、API 锐边（改进建议，暂不提上游）

### 1. RewriteStoreWriter 调用序陷阱：反序不报错，静默旁路事务

- 正确序：一次性 writer 只调 `BeginWorkspace()` 注册进程级事务 →
  干活的 writer 用 `Open()` 加入后才进事务（`rewrite_compiler.cc` 的两步式）。
- 反序（`Open()` 先于 `BeginWorkspace()`）：事务根本不挂上——
  `CommitSchema` **静默直提**、`CommitWorkspace` **恒 false**、
  注册表条目滞留并毒化同路径的后续操作（再 BeginWorkspace 报 already active）。
- 生产代码用对了；任何新调用方都可能踩。收紧方向：反序时显式报错，
  或 `Open()` 顺带注册。

### 2. CommitWorkspace 失败后必须 AbortWorkspace 解锁

- 拒交（含 `MarkWorkspaceFailed` 之后）不清理注册表，同路径被锁死，
  直到显式 `AbortWorkspace()`。生产侧失败分支是否每处都 Abort 未逐一核实。

### 3. 上游老用户库静默失效（有意不兼容）

- evo 的五槽 CSV 值格式没有 legacy reader：上游 1.x 的三字段用户库
  升级到 evo 后，学习数据（词频/时间戳）静默作废，无迁移提示、无告警。
- **定性：有意设计**（amzxyz），不做兼容层。

## 二、设计契约（调用方必须知道，不是 bug）

### 4. 一次性 ConvertSentence 是惰性拷贝

- `changed == false` 时 `result` 保持为空串；增量路径 `state.output`
  始终持有全文。拿两者对拍或做断言时必须按此归一化。

### 5. 句子级转换只取首值

- 同 key 多值（Lookup 给全部候选）在句中转换只取第一个值。

### 6. 惰性 GC：删方案后文件不会立即缩小

- `RetainSchemas` 只把死 schema 从索引解绑；stage 数据本体保留到
  压缩阈值（死 stage ≥16，或垃圾 ≥4MB 且 ≥已提交大小 1/4）。
  属设计（避免小部署反复重写），但用户观察"删了方案文件还那么大"时要知道这是刻意的。

### 7. replace 模式的空结果回退分支不可达

- `if (mode == replace && result->empty())` 的回退在当前 pack 构建约束下
  永远走不到：空 value 编译期就被 `BuildBuckets` 丢弃，未命中时
  `Rewrite` 直接 false。防御性代码，动它前先看 `rewrite-pack-failmodes.md` 的 Y2 记录。

## 三、测试基建教训（写测试的人看）

### 8. clang-format LINT_OK ≠ 能编译

- 它只查排版，字符串字面量断行也能过（本文第 8 条自己就断行过一次）。
  含 `\t`、`\n`、反引号转义的补丁一律落 `.py` 文件或用编辑工具，
  不要内联 `python -c`。

### 9. 产物类断言必须按目标文件名取域

- 扫全工作目录数 `*.tmp.*` 会被其它用例或变异残留污染出假红（实测踩过）。
  统计"本用例目标文件名 + 后缀"前缀；红绿脚本每轮前后清残留。

### 10. 全局组件的 resolver 根在注册时钉死

- userdb 组件的路径根 = 注册时的 `user_data_dir`（测试环境是 CWD）。
  夹具要把 `deployer.user_data_dir` 指向同一处，否则 manager 扫描目录
  与组件读写路径分家（Backup 直接失败）。

### 11. 安全契约有多层守卫，测前先"净化输入"

- 路径逃逸 2 层、缺失文件 4 层、外来快照 3 层独立守卫。
  单点变异必被相邻层兜住——先把测试输入绕开无关层（真快照、合法名），
  让断言单层化，再按层数做组合变异。"没见过红"先数守卫层数，再说测试无效。

### 12. 断言用实测值锚定，不用想象的字面量

- 词典权重是对数域（yaml 里 100 → 约 -13.8）；学习加成是 `max(自身, 榜首+偏移)`
  不是累加。三处断言因想象值域返工。写断言前先跑一次探针打印实际值，
  确认值域与语义（max 还是累加、恒等时输出什么），再固化断言。

## 四、覆盖现状（全部经校机红绿闭环，每个用例见过红）

| 切片 | 登记 | 用例 |
|---|---|---|
| 手排 + 编解码 | doc/manual-order-failmodes.md（C/F） | 15 |
| rewrite store/pack 持久层 | doc/rewrite-pack-failmodes.md（S/P） | 10 |
| 句子级转换 | doc/rewrite-pack-failmodes.md（T） | 8 |
| 压缩阈值 | doc/rewrite-pack-failmodes.md（G） | 3 |
| rewriter 运行时 | doc/rewriter-runtime-failmodes.md（R） | 11 |
| 部署编译 | doc/rewrite-compiler-failmodes.md（D） | 15 |
| 词典管理 | doc/user-dict-manager-failmodes.md（U） | 7 |
| 用户词调序 | doc/user-rank-failmodes.md（J） | 7 |
| 数学原语 | （上游既有 user_rank_math_test.cc） | 3 |

明确排除（理由见各登记表与上文导读"没测什么"）：故障注入三件套、
多进程部署并发、Synchronize 多设备编排、16MiB 单行上限。
