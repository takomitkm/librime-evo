# 手排与用户词状态持久化：失效模式登记表

范围：`manual_stamp_codec`（手排时钟编解码）、`user_manual_order`（钉选/清除/移动三入口）、
`UserDbState` 五字段与 `userdb_tsv` 快照的本轮补充覆盖。
测试位置：`test/manual_stamp_codec_test.cc`（C 组）与 `test/user_db_test.cc`（F 组）。

**注入有效性对拍前置**（Iron Law）：每个新用例必须绑定下表一行，并经历过一次红——
基线提交 CI 全绿 → 单个变异提交（M1–M5，见文末）使对应用例变红 → revert 复绿。
变异命中关系：M1→C2、M2→C3、M3→F3、M4→F1、M5→F8。

## 状态家族

| 家族 | 角色 |
|---|---|
| userdb 主记录（key=`code\ttext`，值=五字段 `commits,dynamic,last_tick,position,manual_stamp`） | 自动学习 + 手排位置的唯一持久态 |
| 元数据 `/manual_clock`、`/tick` | 手排版本钟、全局学习节拍；排序复活与合并的裁决依据 |
| 统一快照 `*.userdb.tsv` | 同步/检查通道（本轮不新增用例，见排除项） |

## 登记

| 编号 | 类别 | 机制（会怎么坏） | 结果类别 | 观察 | 用例 |
|---|---|---|---|---|---|
| C1 | 自身 bug | 编解码规则漂移：Format/Parse 往返不等、字形悄然改变 | 损坏（排序错乱） | 往返相等 + 典型字形锚定 | Codec.Roundtrip / CanonicalEncodings |
| C2 | 自身 bug / 环境非标 | 接受非规范串（前导零、大写、符号、空白、超长）→ 同值多形，合并裁决失真 | 损坏 | 全部拒绝，合法 "0" 仍接受 | Codec.ParseRejects |
| C3 | 自身 bug | 溢出防护缺失 → 36^13-1 回绕成合法小值，伪造旧时钟 | 损坏 | 13×'z' 拒绝；uint64 上限恰可解析 | Codec.ParseRejectsOverflow |
| C4 | 环境非标 | 系统时钟早于纪元 → 钳制语义（恒为 1）不成立则排序基线漂移 | 丢持久性（可接受） | 四个边界值 | Codec.FromUnixSecondsClampsBeforeEpoch |
| F1 | 失控写入 | SetManual 挤占他人固定位；占用冲突未整体回滚 | 损坏 | 拒绝 + 双记录与钟零改动；自己重钉合法 | SetManualRefusesOccupiedSlotAndLeavesNoTrace |
| F2 | 自身 bug | 正常钉选破坏学习字段 | 损坏 | commits/dynamic/last_tick 不变 | SetManualPreservesLearning |
| F3 | 失控写入 | 时钟不取 max(墙钟, 已存+1) → 同步后旧排序复活 | 损坏 | 存钟 1e12 时新钉恰为 1e12+1 | ManualClockBeatsStoredClockEvenBehindWallClock |
| F4 | 环境非标（元数据损坏） | /manual_clock 非法 base36 → 写半截或覆盖 | 损坏 | 整体失败 + 钟与记录零改动 | CorruptManualClockFailsWithoutWrites |
| F5 | 并发与锁 | 事务内重入手排 → 破坏外层事务 | 损坏 | 事务内拒绝、事务外同一操作成功 | ManualOpsRefuseInsideTransaction |
| F6 | 失控写入 / 环境非标 | ResetAuto：误删手排、不幂等、/tick 损坏时半写 | 损坏 | 删自动保手排、二次调用零写入、/tick 损坏拒绝 | ResetAutoSemantics / ResetAutoRejectsCorruptTick |
| F7 | 失控写入 | ResetManual：误删学习、时钟不前进（旧排序复活）、缺失创建 | 损坏 | 位清零保学习、钟前进、缺失拒绝不建记录 | ResetManualClearsPositionKeepsLearning |
| F8 | 失控写入 | MoveAdjacent 可见邻居模式下挤走第三词 | 损坏（候选序错乱） | 第三词钉住时拒绝且零改动；邻居=占用者时合法交换 | VisibleNeighborMustMatchOccupantExactly |
| F9 | 自身 bug | 参数护栏缺失 → 越界位置/非法方向落盘 | 损坏 | 非法参数拒绝、钟与记录零改动 | MoveAdjacentRejectsInvalidArguments |

## 排除项（明确不测，含理由）

- **OOM / 掉电 / I/O 错误注入三件套**：仓库无故障注入基建，本轮仅有 CI 执行通道；
  属后续工作（需编译开关圈死的注入层 + 子进程快照原语），本轮明确不做。
- **多进程并发部署/同步**：超出本切片；LevelDb 自身并发语义由上游与 `level_db_cache_test` 承担。
- **UserDictionary 高层包装（SetManualPosition/ResetManualPosition）**：
  `level_db_cache_test` 已有间接覆盖；本轮在 Db 层测直接契约，避免双层重复。
- **userdb_tsv 快照校验矩阵**：现有 `CanonicalSnapshotOnly` 已绑定十余种畸形（重复元数据、
  保留字段、钟与行版本矛盾等），不重复建设。
- **TextDb 通道的手排语义**：与 LevelDb 共享同一实现层，不按后端复制用例
  （模块×后端矩阵留待注入基建落地时展开）。

## 变异点（红绿对拍用）

| 变异 | 位置 | 期望变红 |
|---|---|---|
| M1 | manual_stamp_codec.h：去掉前导零拒绝 | C2 |
| M2 | manual_stamp_codec.h：去掉溢出防护 | C3 |
| M3 | user_manual_order.cc：NextClock 不取 max | F3 |
| M4 | user_manual_order.cc：SetManual 放开 occupant==key 条件 | F1 |
| M5 | user_manual_order.cc：MoveAdjacent 放开 occupant==neighbor 校验 | F8 |
