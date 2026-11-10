# Rewriter 运行时（Filter 层）：失效模式登记表

范围：`src/rime/gear/rewriter.cc`（783 行，fork 全新文件，此前零直接测试）。
它是候选重写功能的**运行时入口**：schema 配置解析 → option/type/tag 门控 →
按绑定顺序串联 stage → 产出影子候选。测试在 `test/rewriter_test.cc`（R1-R11）。

**注入有效性对拍前置**（Iron Law）：基线绿 → 变异（Y1-Y7）→ 对应用例红 → 还原复绿。
归因表见文末。

## 契约（读码定）

- **模式**：`derive`（默认）= 原候选在前 + 影子候选在后；`replace` = 只留替换值，
  未命中回退原候选；`comment` = 不改文本、只挂模板注释（默认 `〔%s〕`）；
  `abbrev` = 走另一条 translation（需 segment 范围）。
- **门控**：`option: true` = always_on；`option: <name>`/列表 = 任一 option 开启才生效；
  `tags` 非空时只处理带匹配 tag 的 segment；`excluded_types` 命中的候选类型跳过。
- **类型**：`candidate_type` 覆盖影子类型，`preserve` 表示沿用原类型（内部清空）。
- **多级串联**：binding 的 stage 顺序即语义，逐级作用；某级未命中是 no-op，
  文本继续传给后续级。
- **降级**：store 缺失 / 无绑定 / stage 打不开 → 整体禁用并原样返回翻译（不崩）。
- **热重载**：`ReleaseStagesForDeployment()` 清空所有 rewriter 的缓存态，
  下次调用按新代际重新打开；未释放前继续用旧状态。
- **句子模式**：`enable_sentence` 走 `ConvertSentenceIncremental` 增量路径，
  未命中不产生影子候选。

## 登记

| 编号 | 类别 | 机制（会怎么坏） | 观察 | 用例 |
|---|---|---|---|---|
| R1 | 自身 bug | derive 模式顺序错（影子在前/漏原候选） | 结果序列 = [原, 影子] | DeriveModeAppendsShadowAfterOriginal |
| R2 | 失控写入 | replace 模式漏原候选或未命中不回退 → 候选凭空消失 | 命中=替换值、未命中=原文 | ReplaceModeDropsOriginalAndFallsBack |
| R3 | 自身 bug | comment 模式模板未套用/文本被改 | 文本不变、注释=`〔ABC〕` | CommentModeUsesTemplate |
| R4 | 环境非标（配置） | option 门控失效 → 未开启也重写 | 未开启=原样、开启后才变 | OptionGateControlsActivation |
| R5 | 环境非标（配置） | excluded_types 失效 → 该排除的类型被改写 | pinyin 跳过、table 重写 | ExcludedTypesSkipRewrite |
| R6 | 环境非标（缺失） | store 缺失时崩溃或返回空翻译 | 原样返回且不崩 | MissingStoreDegradesGracefully |
| R7 | 自身 bug | candidate_type 覆盖/preserve 语义错 → 下游 filter 按错类型处理 | 影子类型 = rewrite / pinyin | CandidateTypeOverrideAndPreserve |
| R8 | 自身 bug | 多级串联只跑第一级 / 顺序错 | [原, XYZ] | MultiStageChainAppliesInOrder |
| R9 | 环境非标（部署） | 部署后热重载失效 → 永远用旧数据，或未释放就换 | 释放前 AAA、释放后 BBB | ReleaseStagesPicksUpFreshDeployment |
| R10 | 自身 bug | 句子模式结果与 lookup 不一致 / 未命中仍产影子 | 命中 [原, ABC]、未命中仅 [原] | SentenceModeUsesIncrementalPath |
| R11 | 环境非标（配置） | tags 门控失效 → 处理了不该处理的 segment | 无 tag 拒绝、有 tag 通过 | TagGateRejectsUnmatchedSegment |

## 排除项（明确不测，含理由）

- **abbrev 模式**：需要 `Segment` 范围 + Context 组合输入上下文，属 UI 交互路径；
  其核心仍是同一套 stage 查找，R1/R8 已覆盖查找语义。
- **并发**：`RuntimeState` 的原子发布与 mutex 双检属并发语义，单线程套件无法证伪，
  需要多线程 harness（与多进程部署并发一并列为后续）。
- **`insert_position`/`insert_count`/`promote_on_types`**：读码显示这三个选项在
  当前 Transform 路径不参与产出位置决策（由翻译层消费），断言它们会钉死实现细节；
  等实现明确后再补。
- **sentence 缓存 lane 的容量上限（64KB）**：需要构造超限语料，收益低于成本，
  且 `RetainedBytes` 只影响缓存回收不影响输出；列为后续。

## 变异点（红绿对拍用）

| 变异 | 位置 | 期望变红 |
|---|---|---|
| Y1 | rewriter.cc Transform：derive 模式不再先推原候选 | R1（以及 R7/R9/R10 的顺序断言） |
| Y2 | rewriter.cc Transform：replace 模式空结果不回退原候选 | R2 |
| Y3 | rewriter.cc ApplyCommentTemplate：不套用模板 | R3 |
| Y4 | rewriter.cc Active：恒返回 true | R4 |
| Y5 | rewriter.cc Transform：去掉 excluded_types 检查 | R5 |
| Y6 | rewriter.cc Rewrite：多级串联只跑第一级 | R8 |
| Y7 | rewriter.cc ReleaseStagesForDeployment：不清 runtime_state_ | R9 |
