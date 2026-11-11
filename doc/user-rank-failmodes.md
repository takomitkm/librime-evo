# 用户词调序（user_rank）：失效模式登记表

范围：`src/rime/gear/user_rank.cc`（209 行）+ `user_rank_actions.cc`（167 行），
均为 fork 全新文件、此前零直接测试（`user_rank_math.h` 已有独立测试）。
职责：把手排锚点和学习加成落到候选权重上——`Apply`（锚点插缝 + 学习加成 +
系统侧过滤）、`PreferUser`（平权裁决）。测试在 `test/user_rank_test.cc`（J1-J7）。

**注入有效性对拍前置**（Iron Law）：基线绿 → 变异（M6-M10，见文末）→ 对应用例红 →
还原复绿。

## 契约（读码定）

- **锚点插缝**：手排位 N 的锚 = 系统侧"去重后第 N-1 与第 N 名"权重的中点；
  系统侧空缺时用 ±∞ 边界（nextafter）；两键平权用观察位次裁决。
- **系统侧过滤**：被手排的文字（按读音隔离与否）从系统迭代器剔除，
  不靠权重压制——同一文字不会出现两份。
- **学习加成**：无手排的码里，commit_count>0 的精确匹配词条获得
  `max(自身, 系统该码榜首 + 0.02 + 0.001*atan(w))`；同码有手排时该码整体不参与加成；
  从未学习（commits≤0）不参与。
- **读音隔离**（isolate_code）：锚点与加成均按读音分桶，互不影响。
- **精确匹配门槛**：手排/加成/HasManualPin 均要求 `remaining_code_length==0`，
  预测匹配不参与。

## 登记

| 编号 | 类别 | 机制（会怎么坏） | 观察 | 用例 |
|---|---|---|---|---|
| J1 | 自身 bug | 锚点未压过系统首位/系统侧未过滤（同文字双份） | 锚点 ∈ (50,100)，系统只剩 郝 | PinBeatsSystemTopAndFiltersSystem |
| J2 | 自身 bug | 多锚点相对次序倒挂/挤占序号 | 好>郝 且都从系统消失 | MultiplePinsKeepRelativeOrder |
| J3 | 自身 bug | 学习加成越不过系统榜首，或误伤系统侧 | 好U>100，系统不动 | LearnedEntryBoostsAboveSystemTop |
| J4 | 自身 bug | 手排码未抑制自动加成（双重置顶打架） | 好U 保持原权重 | PinnedCodeSuppressesAutoBoost |
| J5 | 失控写入 | 读音未隔离——甲读音手排牵连乙读音加成/过滤 | 的U 照常加成 | ReadingsAreIsolated |
| J6 | 自身 bug | 平权裁决不看观察位次 | 2→true、1→false、权重高→true | PreferUserTieBreaksByObservedPosition |
| J7 | 自身 bug | 预测匹配/未钉选被误认为手排 | 三态断言 | HasManualPinRequiresExactMatch |

## 排除项（明确不测，含理由）

- **user_rank_actions.cc（编辑动作）**：依赖 Engine/Context 交互（选词、置顶、
  删除），属 UI 编排层；其持久化后果（SetManual/ResetManual）已由
  user_db_test 的手排契约覆盖。等 editor 层可注入时再补。
- **RankReading 的排序稳定性细节**（同权重同版本的次序）：实现用 text 兜底
  排序，属确定性细节，契约层面由 J2 的"不倒挂"覆盖。
- **WeightBetween 数学**：`user_rank_math_test.cc` 已覆盖（中点/边界/邻浮点）。
- **与 script_translator 的集成**：`script_translator.cc` 本身是后续切片；
  Apply 的产出语义在此独立验证。

## 变异点（红绿对拍用）

| 变异 | 位置 | 期望变红 |
|---|---|---|
| M6 | user_rank.cc FilterPinnedStatic：过滤条件恒 false（不剔系统侧） | J1 |
| M7 | user_rank.cc RankReading：锚点直接用 pin.position 对应原权重（不插缝） | J1/J2 |
| M8 | user_rank.cc ApplyEqualLengthPriority：加成偏移置 0 | J3 |
| M9 | user_rank.cc ApplyEqualLengthPriority：manually_ranked 判断去掉 | J4 |
| M10 | user_rank_math.h PreferPinnedOnTie：恒 true | J6（波及 user_rank_math_test.EqualWeightUsesPosition） |
| M11 | user_rank.cc HasManualPin：去掉精确匹配门槛 | J7 |

## 红绿闭环记录（校机 WIN11-HV1，mingw64）

基线 367/367 绿（360 + 本组 7）→ M6→J1/J2/J5（过滤契约三处同红）；
M7→J1/J2（不插缝即越位）；M8→J3；M9→J4；M10→J6；M11→J7。
还原后 367/367 复绿。7 个用例各自至少红过一次。

**测试设计要点（实测踩到）**：
- 词典编译后权重是**对数域**（yaml 里 100 的频率 → 约 -13.8），
  断言不能用线性字面量，要用 probe 迭代器实测值锚定。
- 学习加成是 **max(自身, 榜首+偏移)** 不是累加：自身权重已高于榜首时
  加成不可见（J3/J4 都踩过），测试输入必须压到榜首之下。
- 锚点相对"**过滤后**"的系统侧计算（FilterPinnedStatic 先于 RankReading）：
  J1 断言"压过过滤后首位、低于原首位"。
