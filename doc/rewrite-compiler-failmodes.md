# 部署侧编译（RewriteCompiler / RewritePresetCatalog）：失效模式登记表

范围：`src/rime/lever/rewrite_compiler.cc`（657 行）与 `rewrite_preset.cc`（148 行），
均为 fork 全新文件、此前零直接测试。职责：把 schema 的 rewriter 配置编译成
共享 store 里的 stage 并提交绑定；preset 是共享目录里的可复用 stage 目录。
测试在 `test/rewrite_compiler_test.cc`（D1-D15）。

**注入有效性对拍前置**（Iron Law）：基线绿 → 变异（Z1-Z8）→ 对应用例红 → 还原复绿。

## 契约（读码定）

- **stage 身份 = 内容指纹**：`FNV(契约版本 + key_xlit + 文件数 + 每个文件的大小与全部字节)`。
  内容相同 ⇒ id 相同 ⇒ 复用已有对象、零写入；内容变化 ⇒ 新 id（**等长不同内容也必须区分**）。
- **files/preset 二选一**：两者都给或都不给都算配置错误；`/preset` 不得与 `/key_xlit` 组合。
- **源文件解析**：先 user_data_dir 再 shared_data_dir；`../` 等越权名一律拒绝。
- **行解析**：≤16MiB/行；剥 BOM、剥行尾 `\r`、跳过空行与 `#` 注释；
  必须 `key<TAB>value` 且两侧非空，否则整体失败（不产出半份数据）。
- **key_xlit**：`from>to` 必须恰好一个 `>` 且两侧非空；当作 `xlit/from/to/` 投影应用；
  键被改写时原键记入 preedit；改写后为空 → 失败。
- **section 发现**：只认 `engine/filters` 里的 `rewriter` 与 `rewriter@<section>`；
  某 schema 不再声明任何 rewriter section ⇒ 提交空绑定（清掉旧绑定）。
- **preset 目录**：`shared_data_dir/rewriter/presets.yaml`，`version` 必须为 1；
  stage 文件名必须同目录内（拒绝逃逸与重复）。
- **失败传播**：任何一步失败都 `MarkWorkspaceFailed`，使整次部署拒交部分数据；
  workspace abort ⇒ 绑定与数据整体回滚。

## 登记

| 编号 | 类别 | 机制（会怎么坏） | 观察 | 用例 |
|---|---|---|---|---|
| D1 | 自身 bug | files→stage→绑定的基本链路断 | 条目可按 key 查到 | CompileFilesIntoStore |
| D2 | 失控写入 | 内容未变仍重写（部署抖动） | id 相同、文件零增长、代际不变 | RecompileSameContentReusesStage |
| D3 | 自身 bug | 指纹漏内容 → 改动后仍复用旧 stage（候选不更新） | 等长不同内容产生新 id 且查到新值 | ContentChangeProducesNewStage |
| D4 | 环境非标（配置） | files/preset 互斥未校验 → 歧义配置被接受 | 两者同设 → 失败 | FilesAndPresetMutuallyExclusive |
| D5 | 环境非标（配置） | preset 与 key_xlit 组合被接受 → key_xlit 被静默忽略 | 组合 → 失败 | PresetWithKeyXlitRejected |
| D6 | 环境非标（缺失） | 缺源文件时产出空 stage 或成功 | 编译失败 | MissingSourceFileFails |
| D7 | 硬件与底层（安全） | 路径逃逸读取任意文件 | `../outside.txt` → 失败 | PathTraversalRejected |
| D8 | 自身 bug | key_xlit 未应用/丢原键 | 新键可查、preedit=原键 | KeyXlitRewritesKeyAndKeepsPreedit |
| D9 | 自身 bug | 畸形行被静默跳过 → 半份数据上线 | 编译失败 | MalformedLineFails |
| D10 | 环境非标（编码） | BOM/CRLF/注释污染键值 | 条目干净、无 BOM 键 | BomCommentsAndCrlfHandled |
| D11 | 自身 bug | 文件列表未去重 → 指纹漂移（重复部署） | 重复列出与单次列出 id 相同 | DuplicateFileListingDeduped |
| D12 | 失控写入（GC） | section 移除后旧绑定残留 → 幽灵重写 | 绑定被清空 | RemovingSectionClearsBinding |
| D13 | 自身 bug | preset 解析/落库失败 | 条目按 preset 生效 | PresetResolvesAndCompiles |
| D14 | 环境非标（配置） | preset 版本/名称未校验 → 静默空 stage | 版本≠1、未定义名 → 失败 | BadPresetCatalogFails |
| D15 | 失控写入 | workspace abort 不生效 → 部署失败却已生效 | 绑定与数据都回滚 | WorkspaceAbortKeepsPreviousDeployment |

## 排除项（明确不测，含理由）

- **16MiB 单行上限**：构造 16MiB+ 单行只为触发一处边界，代价（内存/时长）高于收益；
  且失败路径与 D9 同类（返回 false）。列为后续。
- **多 schema 并发编译**：`FinalizeWorkspace` 的 RetainSchemas 已有 S6 覆盖语义；
  多 schema 同时编译属部署编排，需多进程/多线程 harness。
- **symlink 逃逸**：`safe_path::ExistingFile` 的 canonical 检查由 path_resolver_test 的
  SafePath 系列覆盖，避免重复建设。
- **共享 shared_data 的 preset 覆盖顺序**：单 root 场景已覆盖，双 root 优先级属
  ResolveDataFile 语义，与 D1/D6 同源。

## 变异点（红绿对拍用）

| 变异 | 位置 | 期望变红 |
|---|---|---|
| Z1 | rewrite_compiler.cc ComputeStageId：只哈文件大小，不哈字节 | D3 |
| Z3 | rewrite_compiler.cc ResolveRewriteSources：去掉 files/preset 互斥检查 | D4 |
| Z4 | rewrite_compiler.cc ResolveRewriteSources：去掉 preset+key_xlit 检查 | D5 |
| Z5 | rewrite_compiler.cc ResolveDataFile：直接用 user_data_dir/file_name（不校验逃逸） | D7 |
| Z6 | rewrite_compiler.cc CompileStage：不启用 key_xlit 投影 | D8 |
| Z7 | rewrite_compiler.cc PrepareSourceLine：不剥 BOM | D10 |
| Z8 | rewrite_compiler.cc ComputeStageId：不去重文件列表 | D11 |
| Z9 | rewrite_compiler.cc Compile：无 section 时不提交空绑定 | D12 |
| Z10 | rewrite_preset.cc Load：不校验 version | D14 |
| Z5b/Z5c | 解析守卫层卸载（见下） | D7 |
| Z11b/Z11c | 缺失文件守卫层卸载（见下） | D6 |
| Z12 | rewrite_preset.cc Resolve：恒失败 | D13 |
| Z13 | ParseFileLine：key/value 互换 | D1、D9？否——D10、D13、D15（正向链路共用条目写入） |
| Z14 | ComputeStageId：注入递增计数器（指纹不确定化） | D2、D11 |
| Z15 | LoadSourceFile：畸形行跳过而非失败 | D9 |

## 红绿闭环记录（校机 WIN11-HV1，mingw64）

基线 353/353 绿（338 + 本组 15）→ 变异轮次：Z1→D3；Z6→D8；Z7→D10；Z8→D11；
Z9→D12；Z10→D14；Z3→D4；Z4→D5；Z5c→D7；Z12→D13；Z13→D1/D10/D13/D15；
Z14→D2/D11；Z15→D9；Z11c→D6/D7。还原后 353/353 复绿。
15 个用例各自至少红过一次。

**两点纵深防御的实测发现**：

1. **D7（路径逃逸）有两层独立守卫**——入口 `safe_path::ValidRelativeName` 与
   解析 `safe_path::ExistingFile`（canonical 检查）。只卸任何一层，D7 仍绿；
   必须两层同卸（Z5c）才红。测试有效，但单点变异无法证伪。
2. **D6（缺失文件）有四层守卫**——`ValidRelativeName`、`ExistingFile`、
   `ComputeStageId` 的 `file_size`/open 失败、`LoadSourceFile` 的 open 失败。
   需要四点组合变异（Z11c）才打红。这类多层防护下，"未见过红"不等于测试无效，
   而是说明单点失效被相邻守卫兜住——记录层数本身就是安全评估结论。

**Z13 的归因修正**：swap key/value 并不会让 D9（畸形行）变红——无 TAB 的行
交换后依然解析失败。D9 由 Z15（跳过畸形行）打红。教训：变异前先推演它对
断言的实际影响，"期望变红"要写得出机理。
