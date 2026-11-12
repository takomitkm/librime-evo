# 编辑-学习编排（ApplyEditAction / OnCommit）：失效模式登记表

范围：`src/rime/gear/user_rank_actions.cc`（167 行，fork 新文件）与
`memory.cc` 的 `OnCommit` 编排——此前零直接测试（登记表曾明确排除）。
这是用户可感的最后一层：**选词 → 自动学习 / 菜单内手排动作 → 用户库落盘**。
测试在 `test/editor_learning_test.cc`（EL1-EL6），harness 沿用
`partial_commit_test` 的合成 composition 模式（手工 Menu + Phrase，
language 指针指向被测 Memory 的 language）。

**注入有效性对拍前置**（Iron Law）：基线绿 → 变异（P1-P4）→ 对应用例红 →
还原复绿。

## 契约（读码定）

- **OnCommit 门控链**：`user_dict_ loaded && !readonly && IsUserDbWriter &&
  automatic_learning_enabled` 全过才学习；manual 模式提交不产生 Memorize。
- **ApplyEditAction 前置**：composing + 有菜单 + 选中候选是可 Phrase 化的 +
  FindManualSource 命中（同 language 且非 readonly）+ 是 writer +
  `FinishSharedSession` 先提交共享库上的学习事务。
- **MoveDown/MoveUp**：local_position 按菜单可见序（同 span 去重）计算；
  已手排词用存储的绝对位置；未手排词借邻词固定位置推导；
  邻词已手排时交换（好↔郝），未手排时仅钉自己。
- **ResetAuto**：自动学习标记删除（commits 变负），手排保留；
  **ResetManual**：手排清除（position 0、版本推进），自动学习保留。
- 动作成功后刷新未确认 composition 并移动高亮。

## 登记

| 编号 | 类别 | 机制（会怎么坏） | 观察 | 用例 |
|---|---|---|---|---|
| EL1 | 自身 bug | auto 提交不产生 Memorize（门控/编排断链） | memorized 含 "好" | AutoModeCommitLearnsViaMemorize |
| EL2 | 失控写入 | MoveDown 不钉位/钉错位 | 好 position=2 | MenuMoveDownPinsBelowNeighbor |
| EL3 | 自身 bug | 上移遇已手排邻词不交换（次序错乱） | 好=1、郝=2 | MenuMoveUpSwapsWithPinnedNeighbor |
| EL4 | 失控写入 | ResetAuto 连手排一起清 | 删除标记 + 手排保留 | ResetAutoKeepsManualPin |
| EL5 | 失控写入 | ResetManual 连学习一起清 | position=0、学习保留 | ResetManualKeepsLearning |
| EL6 | 环境非标（配置） | manual 模式仍自动学习 | memorized 为空 | ManualModeCommitSkipsAutoLearning |

## 排除项（明确不测，含理由）

- **OnUnhandledKey（BackSpace 弃稿）**：键事件层，DiscardSession 语义已由
  MR4 覆盖；键映射属 UI。
- **FinishSharedSession 的"未拥有事务"告警分支**：需多 Memory 竞争注入，
  MR3/MR5 已覆盖提交/回滚语义。
- **InsertPosition/InsertCount/promote_on_types**：翻译层消费，见
  rewriter-runtime-failmodes 排除项。
- **script_translator +300 的运行时 prism 切换**：build_dictionary_test 的
  SharedInputTypes* 四例已端到端覆盖（切换/enabled 门控/配置校验矩阵/patch），
  不重复建设。

## 变异点（红绿对拍用）

| 变异 | 位置 | 期望变红 |
|---|---|---|
| P1 | memory.cc OnCommit：去掉 automatic_learning_enabled 门控 | EL6 |
| P2 | user_rank_actions.cc：去掉 FinishSharedSession 调用 | EL3（会话冲突使手排失败） |
| P3 | user_rank_actions.cc：MoveManualOneStep 方向取反 | EL2 |
| P4 | user_rank_actions.cc：reset_auto/reset_manual 分支互换 | EL4、EL5 |
