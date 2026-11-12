# Memory 注册表与会话生命周期：失效模式登记表

范围：`src/rime/gear/memory.cc`（425 行，fork 改 +317/-27）。fork 新增
`MemoryRegistry`：多个 translator 的 Memory 挂同一用户库（UserDictionaryComponent
的 `db_pool_` 按名共享同一 Db 实例）时的**单一写者**协调（writer_owner）与
**学习会话**生命周期（session_owner：Start/Finish/Discard/Abort、
写者析构转移、FinishSharedSession 手排前提交）。测试在
`test/memory_registry_test.cc`（MR1-MR6）。

**注入有效性对拍前置**（Iron Law）：基线绿 → 变异（M12-M15）→ 对应用例红 →
还原复绿。

## 契约（读码定）

- **单一写者**：同一 storage（Db*）上第一个非 readonly 的 Memory 成为唯一
  writer（`IsUserDbWriter()`），其余 StartSession 必须失败；writer 析构时
  转移给存活的非 readonly 候选。
- **会话提交落库**：StartSession 开事务并登记 session_owner；事务内写入在
  FinishSession（CommitPendingTransaction）前对外不可见，提交后可见。
- **弃稿回滚**：DiscardSession 走 RevertRecentTransaction，事务内写入不得残留。
- **三模式**：`off` → UserDictionary 不创建（Memory 无 user_dict，一切写路径
  no-op）；`manual` → 手排会话通路正常（Start/Finish 可用），自动学习由
  `automatic_learning_enabled()` 门控（已由 user_db_test 覆盖）。

## 登记

| 编号 | 类别 | 机制（会怎么坏） | 观察 | 用例 |
|---|---|---|---|---|
| MR1 | 并发与锁 | 双写者 → 同库并发写冲突/词频互踩 | 仅 #1 是 writer；#2 StartSession 失败 | SoleWriterPerStorage |
| MR2 | 丢持久性 | writer 析构不转移 → 幸存者永远无法学习 | 转移后 StartSession 成功 | WriterOwnershipTransfersOnDestruction |
| MR3 | 丢持久性 | 会话不提交 → 学习数据无声丢失 | 提交后 Fetch 可见 | SessionCommitPersistsWrites |
| MR4 | 失控写入 | 弃稿不回滚 → 撤销的候选仍进词频 | Discard 后无残留 | DiscardSessionRollsBack |
| MR5 | 环境非标（配置） | manual 模式手排通路被断 | Start/Finish 可用 | ManualModeKeepsManualSessionPath |
| MR6 | 环境非标（配置） | off 模式仍建学习层/崩溃 | 无 user_dict、写路径 no-op | OffModeCreatesNoLearningLayer |

## 排除项（明确不测，含理由）

- **OnCommit 学习主流程**：需要构造 composition + 选中 Phrase 候选（Engine/Context
  深度交互）；其门控（automatic_learning_enabled、readonly、writer）与写路径
  （UpdateEntry）均已分别覆盖，编排层列为后续。
- **OnUnhandledKey / OnDeleteEntry**：同上，键事件与菜单交互层。
- **FinishSharedSession**：与 MR3/MR5 共用 CommitPendingTransaction 路径，
  手排入口（user_rank_actions）整体排除时一并排除。
- **FindManualSource**：by_engine 分组的线性查找，MR1 的注册已覆盖数据结构；
  语义断言需多语言 schema，成本高于收益。
- **多线程并发注册**：registry 有 mutex，并发竞争需多线程 harness（与多进程
  并发一并列为后续）。

## 夹具要点（实测踩到）

- **`an<Db>(db_.get())` 是双重所有权**：storage_identity 返回裸指针，包成
  owning shared_ptr 会造出第二个控制块——两个控制块各删一次，析构期
  SIGSEGV（gdb 栈：~Memory → ~UserDictionary → _M_release_last_use_cold →
  野指针调用）。共享句柄一律用裸指针，不接管所有权。
- 元数据键约定：库内键 = `code + 空格 + \t + text`（code 尾带空格）；TSV 快照
  行是规范化键（码尾空格剥除）。Backup 快照断言按规范化键查。
- schema 必须有 `build:` + `dictionaries:` 段，CompileDictionaries 才会产出
  table.bin。
- memory.h 的签名用了 Ticket 但不自含声明，包含顺序必须先 rime/ticket.h。

## 变异点（红绿对拍用）

| 变异 | 位置 | 期望变红 |
|---|---|---|
| M12 | memory.cc 构造：writer_owner.emplace 去掉（无写者） | MR1 |
| M13 | memory.cc 析构：写者转移块删除 | MR2 |
| M14 | memory.cc FinishSession：跳过 CommitPendingTransaction | MR3 |
| M15 | memory.cc DiscardSession：跳过 RevertRecentTransaction | MR4 |
| M13b | memory.cc 析构：转移块整体禁用（`if (false && ...)`） | MR2 |

## 红绿闭环记录（校机 WIN11-HV1，mingw64）

基线 374/374 绿（367 + 本组 6 + 1 个强化）→
M12→MR1/MR2/MR3/MR4/MR5 全链红（+RimeBuildDictionary 2 例波及：无写者时
编码器学习路径同样失效——佐证 writer 语义是全局契约）；
M13 首版 patch 破坏函数结构编译失败（教训：多行块变异用 `if (false && 原条件)`
形态，不删块）；M13b→MR2 确定性红（强化后的 MR2）；M14→MR3；M15→MR4。
还原后 374/374 复绿。6 个用例各自至少红过一次。

**MR2 强化记录**：原版 first 先析构、second 后创建——db_pool_ 会重建新库，
storage 指针不同，转移代码路径根本没被走到（M13 首轮未红的原因）。
强化后 second 在 first 存活期间创建（共享同一 storage），转移契约才真正
被测试。教训：**测"转移/交接"类契约必须重叠生命周期**，先后创建测的是
另一条路径。
