# UserDictManager（用户词典管理）：失效模式登记表

范围：`src/rime/lever/user_dict_manager.cc`（658 行，fork 大改 +568/-136），此前零
直接测试。职责：发现本地 userdb（GetUserDictList）、快照（Backup）、按 TSV 导入导出
（Export/Import）、快照合并回本地库（RestoreMerge/Restore）、按 sync 目录多设备合并
（Synchronize*）。测试在 `test/user_dict_manager_test.cc`（U1-U7）。

**注入有效性对拍前置**（Iron Law）：基线绿 → 变异（V 组见文末）→ 对应用例红 → 还原复绿。

## 契约（读码定）

- **发现**：只扫 `<user_data_dir>/*.userdb` 目录；点前缀/非法名计入 ignored 但不影响
  complete；空库目录、打不开的库 → complete=false（返回 false 但列表仍含合法项）；
  结果按名排序去重；不同时持有多个数据库（逐个打开检查元数据后关闭）。
- **快照**：写到 `sync_dir/<user_id>/<name>.userdb.tsv`；设备号不一致时先更新
  `/user_id`（只动归属，不重建元数据）；先写 pending 再 rename 发布。
- **合并**：`RestoreMerge` 要求快照文件名 == `<name>.userdb.tsv`；临时 staging 库
  逐项校验（/db_type、/userdb_format、库名、/manual_clock 合法 base36、/tick 纯数字）；
  目标库在事务内合并，失败整体回滚。合并语义按 max（commits/dynamic/last_tick/tick）
  收敛——本机较新数据不被远端旧值回退。
- **路径安全**：快照必须在 sync 根之内（拒绝逃逸与 symlink）。

## 登记

| 编号 | 类别 | 机制（会怎么坏） | 观察 | 用例 |
|---|---|---|---|---|
| U1 | 自身 bug | 发现漏库/乱序/重复 | [alpha, beta] 有序 | DiscoveryListsAndSorts |
| U2 | 环境非标（损坏） | 损坏库被静默列入或吞掉合法库 | incomplete=false 且合法项保留 | DiscoverySkipsInvalidAndReportsIncomplete |
| U3 | 失控写入 | 快照丢失/写错目录/归属未更新 | 文件在 sync/<device>/、/user_id 已更新 | BackupWritesSnapshotAndUpdatesOwner |
| U4 | 丢持久性 | 合并用远端旧值覆盖本机新值（回退用户数据） | 合并后 commits=5（max） | RestoreMergesByMaxCommits |
| U5 | 失控写入 | 外来快照污染本地库 | 拒绝 | RestoreRejectsForeignSnapshot |
| U6 | 硬件与底层（安全） | sync 根外快照被读取 | 拒绝 | RestoreRejectsTraversal |
| U7 | 自身 bug | 导入计数错误/对不存在库误成功 | 返回 1 / -1 | ImportReturnsMergedCount |

## 排除项（明确不测，含理由）

- **Synchronize/SynchronizeAll 多设备合并**：需要构造多设备目录与多份快照的编排，
  核心合并语义已由 U4 覆盖；编排层列为后续。
- **staging 库残留（.userdb_sync_staging）清理**：BOOST_SCOPE_EXIT 保证删除，
  异常路径的清理已由 S6/S15 类比；专门注入需故障基建。
- **快照内容校验矩阵**（坏 base36、坏 tick、record_count 撒谎）：
  `user_db_test.cc` 的 CanonicalSnapshotOnly 与 userdb_tsv::ValidateSnapshot 已覆盖，
  不重复建设。
- **并发同步**：多进程同时合并同一库需多进程 harness，与部署并发一并列为后续。

## 变异点（红绿对拍用）

| 变异 | 位置 | 期望变红 |
|---|---|---|
| W1（本组） | user_dict_manager.cc GetUserDictList：不排序 | U1 |
| W2（本组） | user_dict_manager.cc Backup：直接写目标（跳过 /user_id 更新） | U3 |
| W3（本组） | user_dict_manager.cc RestoreMerge：去掉文件名匹配检查 | U5 |
| W4（本组） | user_dict_manager.cc Restore：去掉 sync 根检查 | U6 |
| W5（本组） | user_dict_manager.cc RestoreMerge：ObserveClock/Tick 不调用（合并退化为覆盖） | U4、U7 |

注：为避免与 rewrite_compiler 的 Z 组混淆，本组变异按 W 组续号。
