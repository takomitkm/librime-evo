// Copyright RIME Developers
// Distributed under the BSD License
//
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <vector>
#include <rime/dict/rewrite_pack.h>
#include <rime/dict/rewrite_store.h>

using namespace rime;

namespace {

path TestPath(const char* name) {
  return path{std::string("rewrite_store_") + name + ".rwp"};
}

std::uint64_t FileSize(const path& file) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(file, ec);
  return ec ? 0 : size;
}

RewriteStageData MakeStage(
    const std::vector<std::tuple<string, string, string>>& entries) {
  RewriteStageData stage;
  for (const auto& [key, value, preedit] : entries) {
    stage.entries.push_back({key, value, preedit});
  }
  return stage;
}

RewriteStageId TestId(std::uint64_t n) {
  return RewriteStageId{1, n};
}

int CountTemporaryFiles() {
  int count = 0;
  std::error_code ec;
  for (const auto& item : std::filesystem::directory_iterator(".", ec)) {
    const string name = item.path().filename().string();
    if (name.find(".tmp.") != string::npos) {
      ++count;
    }
  }
  return count;
}

std::string ReadFileBytes(const path& file) {
  std::ifstream in(static_cast<const std::filesystem::path&>(file),
                   std::ios::binary);
  return string((std::istreambuf_iterator<char>(in)),
                std::istreambuf_iterator<char>());
}

void WriteFileBytes(const path& file, const string& bytes) {
  std::ofstream out(static_cast<const std::filesystem::path&>(file),
                    std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// 生产协议（rewrite_compiler.cc）：一次性 writer 只调 BeginWorkspace 注册
// 全局 workspace；真正干活的 writer 经 Open() 加入后才进入事务。
void CommitMinimalStore(const path& file, const RewriteStageId& stage_id) {
  {
    RewriteStoreWriter starter(file);
    ASSERT_TRUE(starter.BeginWorkspace());
  }
  RewriteStoreWriter writer(file);
  ASSERT_TRUE(writer.Open());
  ASSERT_TRUE(writer.AppendStage(stage_id, MakeStage({{"a", "甲", ""}})));
  vector<RewriteStageBinding> bindings;
  bindings.push_back({"sect", {stage_id}});
  ASSERT_TRUE(writer.CommitSchema("schemaA", bindings));
  ASSERT_TRUE(writer.CommitWorkspace());
}

}  // namespace

// S1：全链路往返——binding 顺序、generation、stage 内容逐项对拍。
TEST(RimeRewriteStoreTest, WorkspaceCommitRoundTripAndBindings) {
  const path file = TestPath("roundtrip");
  std::filesystem::remove(file);
  {
    {
      RewriteStoreWriter starter(file);
      ASSERT_TRUE(starter.BeginWorkspace());
    }
    RewriteStoreWriter writer(file);
    ASSERT_TRUE(writer.Open());
    const RewriteStageId s1 = TestId(1);
    const RewriteStageId s2 = TestId(2);
    ASSERT_TRUE(writer.AppendStage(s1, MakeStage({{"a", "甲", ""}})));
    ASSERT_TRUE(writer.AppendStage(s2, MakeStage({{"b", "乙", ""}})));
    EXPECT_TRUE(writer.HasStage(s1));
    EXPECT_TRUE(writer.HasStage(s2));
    vector<RewriteStageBinding> bindings;
    bindings.push_back({"sect", {s1, s2}});
    ASSERT_TRUE(writer.CommitSchema("schemaA", bindings));
    ASSERT_TRUE(writer.CommitWorkspace());
  }
  auto store = RewriteStore::OpenCached(file);
  ASSERT_TRUE(store);
  EXPECT_EQ(1u, store->generation());
  vector<RewriteStageId> ids;
  ASSERT_TRUE(store->FindStageSequence("schemaA", "sect", &ids));
  ASSERT_EQ(2u, ids.size());
  EXPECT_EQ(TestId(1), ids[0]);  // 绑定顺序是语义，不得重排
  EXPECT_EQ(TestId(2), ids[1]);
  auto pack = store->OpenStage(ids[0]);
  ASSERT_TRUE(pack);
  vector<string> values;
  ASSERT_TRUE(pack->Lookup("a", &values));
  ASSERT_EQ(1u, values.size());
  EXPECT_EQ("甲", values[0]);
  EXPECT_FALSE(pack->Lookup("missing", &values));  // miss -> false + 空
  EXPECT_TRUE(values.empty());
  ASSERT_TRUE(store->OpenStage(ids[1]));
  vector<RewriteStageId> missing;
  EXPECT_FALSE(store->FindStageSequence("schemaB", "sect", &missing));
  EXPECT_TRUE(missing.empty());
  std::filesystem::remove(file);
}

// S2：失败标记后必须拒交；abort 后旧态完好、无临时文件残留。
TEST(RimeRewriteStoreTest,
     WorkspaceFailureRefusesPartialCommitThenAbortRestores) {
  const path file = TestPath("failrefuse");
  std::filesystem::remove(file);
  const RewriteStageId s1 = TestId(1);
  const RewriteStageId s2 = TestId(2);
  CommitMinimalStore(file, s1);
  const auto committed = FileSize(file);
  {
    {
      RewriteStoreWriter starter(file);
      ASSERT_TRUE(starter.BeginWorkspace());
    }
    RewriteStoreWriter writer(file);
    ASSERT_TRUE(writer.Open());
    ASSERT_TRUE(writer.AppendStage(s2, MakeStage({{"b", "乙", ""}})));
    vector<RewriteStageBinding> bindings;
    bindings.push_back({"sect", {s1, s2}});
    ASSERT_TRUE(writer.CommitSchema("schemaA", bindings));
    writer.MarkWorkspaceFailed();
    EXPECT_TRUE(writer.WorkspaceFailed());
    EXPECT_FALSE(writer.CommitWorkspace());
    EXPECT_EQ(0, CountTemporaryFiles());
    ASSERT_TRUE(writer.AbortWorkspace());
  }
  EXPECT_EQ(committed, FileSize(file));
  auto store = RewriteStore::OpenCached(file);
  ASSERT_TRUE(store);
  EXPECT_EQ(1u, store->generation());
  vector<RewriteStageId> ids;
  ASSERT_TRUE(store->FindStageSequence("schemaA", "sect", &ids));
  ASSERT_EQ(1u, ids.size());
  EXPECT_EQ(s1, ids[0]);
  EXPECT_FALSE(store->OpenStage(s2));
  std::filesystem::remove(file);
}

// S3：同 id 幂等（字节数不变、首写内容生效）；空 entries 拒绝。
TEST(RimeRewriteStoreTest, AppendStageIdempotentAndEmptyRejected) {
  const path file = TestPath("idempotent");
  std::filesystem::remove(file);
  {
    RewriteStoreWriter starter(file);
    ASSERT_TRUE(starter.BeginWorkspace());
  }
  RewriteStoreWriter writer(file);
  ASSERT_TRUE(writer.Open());
  const RewriteStageId s1 = TestId(1);
  ASSERT_TRUE(writer.AppendStage(s1, MakeStage({{"a", "甲", ""}})));
  const auto after_first = FileSize(file);
  // 同 id 重复追加：no-op，不得再写一份载荷。
  ASSERT_TRUE(writer.AppendStage(s1, MakeStage({{"a", "乙", ""}})));
  EXPECT_EQ(after_first, FileSize(file));
  EXPECT_FALSE(writer.AppendStage(TestId(2), RewriteStageData{}));
  vector<RewriteStageBinding> bindings;
  bindings.push_back({"sect", {s1}});
  ASSERT_TRUE(writer.CommitSchema("schemaA", bindings));
  ASSERT_TRUE(writer.CommitWorkspace());
  auto store = RewriteStore::OpenCached(file);
  ASSERT_TRUE(store);
  auto pack = store->OpenStage(s1);
  ASSERT_TRUE(pack);
  vector<string> values;
  ASSERT_TRUE(pack->Lookup("a", &values));
  ASSERT_EQ(1u, values.size());
  EXPECT_EQ("甲", values[0]);  // 首写内容生效，不被重复追加覆盖
  std::filesystem::remove(file);
}

// S4：abort 必须把物理文件截回提交点（AppendStage 是直写物理文件的）。
TEST(RimeRewriteStoreTest, AbortWorkspaceTruncatesPhysicalFile) {
  const path file = TestPath("abort");
  std::filesystem::remove(file);
  const RewriteStageId s1 = TestId(1);
  const RewriteStageId s2 = TestId(2);
  CommitMinimalStore(file, s1);
  const auto committed = FileSize(file);
  {
    {
      RewriteStoreWriter starter(file);
      ASSERT_TRUE(starter.BeginWorkspace());
    }
    RewriteStoreWriter writer(file);
    ASSERT_TRUE(writer.Open());
    ASSERT_TRUE(writer.AppendStage(s2, MakeStage({{"b", "乙", ""}})));
    EXPECT_GT(FileSize(file), committed);  // 直写发生了
    EXPECT_TRUE(writer.AbortWorkspace());
  }
  EXPECT_EQ(committed, FileSize(file));  // 截回提交点
  auto store = RewriteStore::OpenCached(file);
  ASSERT_TRUE(store);
  EXPECT_FALSE(store->OpenStage(s2));
  std::filesystem::remove(file);
}

// S5：上次进程崩溃留下的未提交尾巴，Writer::Open 时必须回收。
TEST(RimeRewriteStoreTest, UncommittedTailDiscardedOnWriterOpen) {
  const path file = TestPath("crashtail");
  std::filesystem::remove(file);
  const RewriteStageId s1 = TestId(1);
  CommitMinimalStore(file, s1);
  const auto committed = FileSize(file);
  {
    std::ofstream out(static_cast<const std::filesystem::path&>(file),
                      std::ios::binary | std::ios::app);
    out << "GARBAGE-TAIL-FROM-CRASH";
  }
  EXPECT_GT(FileSize(file), committed);
  {
    RewriteStoreWriter writer(file);
    ASSERT_TRUE(writer.Open());  // 打开即截断尾巴
  }
  EXPECT_EQ(committed, FileSize(file));
  auto store = RewriteStore::OpenCached(file);
  ASSERT_TRUE(store);
  EXPECT_EQ(1u, store->generation());
  std::filesystem::remove(file);
}

// S6：RetainSchemas 只留名单内 schema；全空提交删除 store 文件。
TEST(RimeRewriteStoreTest, RetainSchemasDropsDeadSchemas) {
  const path file = TestPath("retain");
  std::filesystem::remove(file);
  const RewriteStageId s1 = TestId(1);
  const RewriteStageId s2 = TestId(2);
  {
    {
      RewriteStoreWriter starter(file);
      ASSERT_TRUE(starter.BeginWorkspace());
    }
    RewriteStoreWriter writer(file);
    ASSERT_TRUE(writer.Open());
    ASSERT_TRUE(writer.AppendStage(s1, MakeStage({{"a", "甲", ""}})));
    ASSERT_TRUE(writer.AppendStage(s2, MakeStage({{"b", "乙", ""}})));
    vector<RewriteStageBinding> bindings;
    bindings.push_back({"sect", {s1}});
    ASSERT_TRUE(writer.CommitSchema("schemaA", bindings));
    vector<RewriteStageBinding> bindings_b;
    bindings_b.push_back({"sect", {s2}});
    ASSERT_TRUE(writer.CommitSchema("schemaB", bindings_b));
    ASSERT_TRUE(writer.CommitWorkspace());
  }
  {
    {
      RewriteStoreWriter starter(file);
      ASSERT_TRUE(starter.BeginWorkspace());
    }
    RewriteStoreWriter writer(file);
    ASSERT_TRUE(writer.Open());
    EXPECT_TRUE(writer.RetainSchemas({"schemaA"}));
    ASSERT_TRUE(writer.CommitWorkspace());
  }
  auto store = RewriteStore::OpenCached(file);
  ASSERT_TRUE(store);
  vector<RewriteStageId> ids;
  EXPECT_TRUE(store->FindStageSequence("schemaA", "sect", &ids));
  EXPECT_FALSE(store->FindStageSequence("schemaB", "sect", &ids));
  EXPECT_TRUE(store->OpenStage(s1));
  // GC 边界 = schema 引用：死 stage 仅从绑定解绑，运行时经 schema 已不可达；
  // 数据本体保留到压缩阈值（dead_stages>=16 或垃圾>=4MB）才物理回收，
  // 因此 OpenStage 仍可打开——这是刻意的惰性回收，不是泄漏。
  EXPECT_TRUE(store->OpenStage(s2));
  {
    // 清空全部 schema -> 提交时删除整个文件
    {
      RewriteStoreWriter starter(file);
      ASSERT_TRUE(starter.BeginWorkspace());
    }
    RewriteStoreWriter writer(file);
    ASSERT_TRUE(writer.Open());
    EXPECT_TRUE(writer.RetainSchemas({}));
    ASSERT_TRUE(writer.CommitWorkspace());
  }
  EXPECT_FALSE(std::filesystem::exists(file));
  EXPECT_FALSE(RewriteStore::OpenCached(file));
}

// S7：第二次提交推进代际且旧 stage 仍可读；干净提交零写入。
TEST(RimeRewriteStoreTest,
     SecondCommitAdvancesGenerationAndCleanCommitWritesNothing) {
  const path file = TestPath("generation");
  std::filesystem::remove(file);
  const RewriteStageId s1 = TestId(1);
  const RewriteStageId s2 = TestId(2);
  CommitMinimalStore(file, s1);
  vector<RewriteStageBinding> same_bindings;
  same_bindings.push_back({"sect", {s1, s2}});
  {
    {
      RewriteStoreWriter starter(file);
      ASSERT_TRUE(starter.BeginWorkspace());
    }
    RewriteStoreWriter writer(file);
    ASSERT_TRUE(writer.Open());
    ASSERT_TRUE(writer.AppendStage(s2, MakeStage({{"b", "乙", ""}})));
    ASSERT_TRUE(writer.CommitSchema("schemaA", same_bindings));
    ASSERT_TRUE(writer.CommitWorkspace());
  }
  auto store = RewriteStore::OpenCached(file);
  ASSERT_TRUE(store);
  EXPECT_EQ(2u, store->generation());
  vector<RewriteStageId> ids;
  ASSERT_TRUE(store->FindStageSequence("schemaA", "sect", &ids));
  ASSERT_EQ(2u, ids.size());
  ASSERT_TRUE(store->OpenStage(s1));  // 旧 stage 不可变仍可读
  const auto size_before = FileSize(file);
  {
    // 内容未变的干净提交：不得产生任何写入
    {
      RewriteStoreWriter starter(file);
      ASSERT_TRUE(starter.BeginWorkspace());
    }
    RewriteStoreWriter writer(file);
    ASSERT_TRUE(writer.Open());
    ASSERT_TRUE(writer.CommitSchema("schemaA", same_bindings));
    ASSERT_TRUE(writer.CommitWorkspace());
  }
  EXPECT_EQ(size_before, FileSize(file));
  store = RewriteStore::OpenCached(file);
  ASSERT_TRUE(store);
  EXPECT_EQ(2u, store->generation());
  std::filesystem::remove(file);
}

// S8：索引损坏必须被拒收（干净失败，不崩溃）。导出面只有 OpenCached。
// 两个腐蚀面：结构位（stage id 字段，解析层即拒）与 schema 名字节
// （解析层容忍的垃圾名，只有 index_hash 抓得住）。
TEST(RimeRewriteStoreTest, CorruptIndexRejectedAndOpenFailsCleanly) {
  const path file = TestPath("corrupt");
  std::filesystem::remove(file);
  const RewriteStageId s1 = TestId(1);
  CommitMinimalStore(file, s1);
  const string pristine = ReadFileBytes(file);  // 两个案例各自从同一完好态派生
  {
    // 结构位：文件末尾 = 末绑定 stage id 的低 64 位，翻转后引用失配。
    string bytes = pristine;
    ASSERT_GT(bytes.size(), 128u);
    bytes.back() = static_cast<char>(bytes.back() ^ 0xFF);
    WriteFileBytes(file, bytes);
    EXPECT_FALSE(RewriteStore::OpenCached(file));
  }
  {
    // 仅哈希可检测：schema 名字节翻转，索引结构完全合法。
    string bytes = pristine;
    const auto name_pos = bytes.find("schemaA");
    ASSERT_NE(string::npos, name_pos);
    bytes[name_pos + 2] = static_cast<char>(bytes[name_pos + 2] ^ 0xFF);
    WriteFileBytes(file, bytes);
    EXPECT_FALSE(RewriteStore::OpenCached(file));
  }
  std::filesystem::remove(file);
}

// S9：CommitSchema 拒绝未暂存/畸形绑定。
TEST(RimeRewriteStoreTest, CommitSchemaValidatesBindings) {
  const path file = TestPath("validate");
  std::filesystem::remove(file);
  {
    RewriteStoreWriter starter(file);
    ASSERT_TRUE(starter.BeginWorkspace());
  }
  RewriteStoreWriter writer(file);
  ASSERT_TRUE(writer.Open());
  const RewriteStageId s1 = TestId(1);
  ASSERT_TRUE(writer.AppendStage(s1, MakeStage({{"a", "甲", ""}})));
  EXPECT_FALSE(writer.CommitSchema("", {}));
  EXPECT_FALSE(writer.CommitSchema("A", {{"", {s1}}}));
  EXPECT_FALSE(writer.CommitSchema("A", {{"sect", {TestId(99)}}}));
  EXPECT_FALSE(writer.CommitSchema("A", {{"sect", {}}}));
  EXPECT_FALSE(writer.CommitSchema("A", {{"sect", {s1}}, {"sect", {s1}}}));
  EXPECT_TRUE(writer.CommitSchema("A", {{"sect", {s1}}}));
  ASSERT_TRUE(writer.AbortWorkspace());
  std::filesystem::remove(file);
}

// S10：同 path 的 workspace 重入必须拒绝；第二个 Writer 加入现有 workspace。
TEST(RimeRewriteStoreTest, WorkspaceReentryGuardAndSecondWriterJoins) {
  const path file = TestPath("reentry");
  std::filesystem::remove(file);
  const RewriteStageId s1 = TestId(1);
  CommitMinimalStore(file, s1);
  {
    {
      RewriteStoreWriter starter(file);
      ASSERT_TRUE(starter.BeginWorkspace());
    }
    RewriteStoreWriter writer(file);
    ASSERT_TRUE(writer.Open());
    EXPECT_FALSE(writer.BeginWorkspace());  // 重入拒绝
    RewriteStoreWriter second(file);
    ASSERT_TRUE(second.Open());  // 加入现 workspace（并回收尾巴）
    ASSERT_TRUE(writer.AppendStage(s1, MakeStage({{"a", "甲", ""}})));
    ASSERT_TRUE(writer.AbortWorkspace());
  }
  auto store = RewriteStore::OpenCached(file);
  ASSERT_TRUE(store);
  EXPECT_EQ(1u, store->generation());
  std::filesystem::remove(file);
}

// P1：pack 往返——多值顺序、preedit 对、miss 语义。
TEST(RimeRewritePackTest, BuildThroughStoreRoundTripWithPreedit) {
  const path file = TestPath("packrt");
  std::filesystem::remove(file);
  const RewriteStageId s1 = TestId(1);
  {
    {
      RewriteStoreWriter starter(file);
      ASSERT_TRUE(starter.BeginWorkspace());
    }
    RewriteStoreWriter writer(file);
    ASSERT_TRUE(writer.Open());
    ASSERT_TRUE(writer.AppendStage(s1, MakeStage({
                                           {"a", "甲", ""},
                                           {"a", "乙", "preA"},
                                           {"bb", "丙丁", "preB"},
                                       })));
    vector<RewriteStageBinding> bindings;
    bindings.push_back({"sect", {s1}});
    ASSERT_TRUE(writer.CommitSchema("schemaA", bindings));
    ASSERT_TRUE(writer.CommitWorkspace());
  }
  auto store = RewriteStore::OpenCached(file);
  ASSERT_TRUE(store);
  auto pack = store->OpenStage(s1);
  ASSERT_TRUE(pack);
  vector<string> values;
  ASSERT_TRUE(pack->Lookup("a", &values));
  ASSERT_EQ(2u, values.size());  // 同 key 多值按写入顺序
  EXPECT_EQ("甲", values[0]);
  EXPECT_EQ("乙", values[1]);
  vector<RewriteResult> results;
  ASSERT_TRUE(pack->LookupWithPreedit("a", &results));
  ASSERT_EQ(2u, results.size());
  EXPECT_EQ("甲", results[0].value);
  EXPECT_EQ("", results[0].preedit);
  EXPECT_EQ("乙", results[1].value);
  EXPECT_EQ("preA", results[1].preedit);
  vector<RewriteResult> single;
  ASSERT_TRUE(pack->LookupWithPreedit("bb", &single));
  ASSERT_EQ(1u, single.size());
  EXPECT_EQ("丙丁", single[0].value);
  EXPECT_EQ("preB", single[0].preedit);
  std::filesystem::remove(file);
}

// P2：同一数据两次 Build 输出必须逐字节相同（部署指纹正确性的前提）。
TEST(RimeRewritePackTest, BuildOutputIsDeterministic) {
  const path first = TestPath("det_a");
  const path second = TestPath("det_b");
  std::filesystem::remove(first);
  std::filesystem::remove(second);
  const RewriteStageData stage = MakeStage({
      {"a", "甲", "pA"},
      {"bb", "乙丙", ""},
      {"ccc", "丁", "pC"},
      {"dddd", "戊", ""},
  });
  RewritePackBuilder builder;
  ASSERT_TRUE(builder.Build(first, stage));
  ASSERT_TRUE(builder.Build(second, stage));
  EXPECT_EQ(ReadFileBytes(first), ReadFileBytes(second));
  std::filesystem::remove(first);
  std::filesystem::remove(second);
}

// P3：载荷字节损坏必须让 OpenStage 拒绝（mmap 面上的畸形拒收）。
TEST(RimeRewritePackTest, CorruptedStagePayloadRejectedOnOpenStage) {
  const path file = TestPath("corruptpack");
  std::filesystem::remove(file);
  const RewriteStageId s1 = TestId(1);
  CommitMinimalStore(file, s1);
  auto store = RewriteStore::OpenCached(file);
  ASSERT_TRUE(store);
  ASSERT_TRUE(store->OpenStage(s1));  // 对照：先证明完好时可读
  string bytes = ReadFileBytes(file);
  // 布局：128B 超块 + 64B 对象头 + 载荷；载荷首字节在 192。
  ASSERT_GT(bytes.size(), 192u);
  bytes[192] = static_cast<char>(bytes[192] ^ 0xFF);  // 破坏载荷 magic
  WriteFileBytes(file, bytes);
  EXPECT_FALSE(store->OpenStage(s1));
  std::filesystem::remove(file);
}

// ===== 句子级转换（T1-T8，登记见 doc/rewrite-pack-failmodes.md）=====

namespace {

// T 组固定 stage："a" 多值（句中取首值甲）、"ab"/"abc" 短语（贪心最长）、
// "x" 恒等替换、"界" 多字节键。
an<const RewritePack> OpenSentencePack(const path& file) {
  std::error_code ec;
  std::filesystem::remove(file, ec);
  const RewriteStageId s1 = TestId(7);
  {
    RewriteStoreWriter starter(file);
    if (!starter.BeginWorkspace()) {
      return nullptr;
    }
  }
  RewriteStoreWriter writer(file);
  if (!writer.Open()) {
    return nullptr;
  }
  RewriteStageData stage;
  const char* rows[][3] = {{"a", "甲", ""},  {"a", "乙", ""},
                           {"ab", "AB", ""}, {"abc", "ABC", ""},
                           {"x", "x", ""},   {"界", "界界", ""}};
  for (const auto& row : rows) {
    stage.entries.push_back({row[0], row[1], row[2]});
  }
  if (!writer.AppendStage(s1, stage)) {
    return nullptr;
  }
  vector<RewriteStageBinding> bindings;
  bindings.push_back({"sect", {s1}});
  if (!writer.CommitSchema("schemaA", bindings) || !writer.CommitWorkspace()) {
    return nullptr;
  }
  auto store = RewriteStore::OpenCached(file);
  if (!store) {
    return nullptr;
  }
  vector<RewriteStageId> ids;
  if (!store->FindStageSequence("schemaA", "sect", &ids) || ids.size() != 1) {
    return nullptr;
  }
  return store->OpenStage(ids[0]);
}

}  // namespace

// T1：贪心最长短语压制单字回退。
TEST(RimeRewritePackTest, GreedyLongestPhraseBeatsSingleChar) {
  const path file = TestPath("sent_greedy");
  auto pack = OpenSentencePack(file);
  ASSERT_TRUE(pack);
  string result;
  ASSERT_TRUE(pack->ConvertSentence("abc", &result));
  EXPECT_EQ("ABC", result);
  ASSERT_TRUE(pack->ConvertSentence("abcd", &result));
  EXPECT_EQ("ABCd", result);
  pack.reset();
  std::filesystem::remove(file);
}

// T2：短语探针失败回退单字；探针命中后未匹配尾原样保留。
TEST(RimeRewritePackTest, PhraseMissFallsBackToSingleChar) {
  const path file = TestPath("sent_fallback");
  auto pack = OpenSentencePack(file);
  ASSERT_TRUE(pack);
  string result;
  ASSERT_TRUE(pack->ConvertSentence("axb", &result));
  EXPECT_EQ("甲xb", result);
  // 恒等 + 未匹配：changed=false，惰性拷贝设计下 result 保持为空
  EXPECT_FALSE(pack->ConvertSentence("xb", &result));
  EXPECT_TRUE(result.empty());
  pack.reset();
  std::filesystem::remove(file);
}

// T3：多值键句中转换取首值。
TEST(RimeRewritePackTest, SentenceUsesFirstValue) {
  const path file = TestPath("sent_first");
  auto pack = OpenSentencePack(file);
  ASSERT_TRUE(pack);
  string result;
  ASSERT_TRUE(pack->ConvertSentence("a", &result));
  EXPECT_EQ("甲", result);
  vector<string> values;
  ASSERT_TRUE(pack->Lookup("a", &values));
  ASSERT_EQ(2u, values.size());  // Lookup 仍给全部候选，句子只取第一个
  pack.reset();
  std::filesystem::remove(file);
}

// T4：恒等替换不计 changed。
TEST(RimeRewritePackTest, IdentityReplacementNotChanged) {
  const path file = TestPath("sent_identity");
  auto pack = OpenSentencePack(file);
  ASSERT_TRUE(pack);
  string result;
  EXPECT_FALSE(pack->ConvertSentence("x", &result));
  EXPECT_TRUE(result.empty());  // 惰性拷贝：无变化时不构造结果串
  EXPECT_FALSE(pack->ConvertSentence("qxq", &result));
  EXPECT_TRUE(result.empty());
  ASSERT_TRUE(pack->ConvertSentence("ax", &result));  // a 变化 -> 整句 true
  EXPECT_EQ("甲x", result);
  pack.reset();
  std::filesystem::remove(file);
}

// T5：不可匹配段原样保留（含多字节键替换、纯未匹配段）。
TEST(RimeRewritePackTest, UnmatchableRunsVerbatimUtf8) {
  const path file = TestPath("sent_verbatim");
  auto pack = OpenSentencePack(file);
  ASSERT_TRUE(pack);
  string result;
  ASSERT_TRUE(pack->ConvertSentence("q界r", &result));
  EXPECT_EQ("q界界r", result);
  EXPECT_FALSE(pack->ConvertSentence("qwr", &result));
  EXPECT_TRUE(result.empty());  // 惰性拷贝
  // 变化发生之后的不可匹配段仍须原样保留（skip 分支的 changed 追加路径）
  ASSERT_TRUE(pack->ConvertSentence("aq", &result));
  EXPECT_EQ("甲q", result);
  pack.reset();
  std::filesystem::remove(file);
}

// T7：append-sensitive 续探——"a" 停在输入末尾，追加后从断点续探得最长匹配。
TEST(RimeRewritePackTest, FrontierContinuationResolvesAppend) {
  const path file = TestPath("sent_frontier");
  auto pack = OpenSentencePack(file);
  ASSERT_TRUE(pack);
  RewriteSentenceState state;
  string one_shot;
  ASSERT_TRUE(pack->ConvertSentenceIncremental("a", &state));
  EXPECT_EQ("甲", state.output);  // 单字回退（短语未决）
  ASSERT_TRUE(pack->ConvertSentenceIncremental("ab", &state));
  EXPECT_EQ("AB", state.output);  // 续探命中最长
  ASSERT_TRUE(pack->ConvertSentenceIncremental("abc", &state));
  EXPECT_EQ("ABC", state.output);
  ASSERT_TRUE(pack->ConvertSentence("abc", &one_shot));
  EXPECT_EQ(one_shot, state.output);
  pack.reset();
  std::filesystem::remove(file);
}

// T8：相同输入快速路径命中且结果稳定。
TEST(RimeRewritePackTest, IdenticalInputUsesCache) {
  const path file = TestPath("sent_identical");
  auto pack = OpenSentencePack(file);
  ASSERT_TRUE(pack);
  RewriteSentenceState state;
  RewriteSentenceMetrics metrics;
  ASSERT_TRUE(pack->ConvertSentenceIncremental("abc", &state, &metrics));
  EXPECT_EQ("ABC", state.output);
  const bool again = pack->ConvertSentenceIncremental("abc", &state, &metrics);
  EXPECT_EQ(2u, metrics.calls);
  EXPECT_EQ(1u, metrics.identical_input_hits);
  EXPECT_TRUE(again);
  EXPECT_EQ("ABC", state.output);
  pack.reset();
  std::filesystem::remove(file);
}

// T6：增量 == 一次性（固定种子随机序列对拍；追加与截断编辑混合）。
TEST(RimeRewritePackTest, IncrementalMatchesOneShot) {
  const path file = TestPath("sent_equiv");
  auto pack = OpenSentencePack(file);
  ASSERT_TRUE(pack);
  std::mt19937 rng(20261110);
  const char* units[] = {"a", "b", "c", "x", "界", "q"};
  RewriteSentenceState state;
  RewriteSentenceMetrics metrics;
  vector<string> text_units;
  size_t prefix_reuse_seen = 0;
  for (int step = 0; step < 200; ++step) {
    if (!text_units.empty() && (rng() % 3) == 0) {
      // 编辑：随机截断到某个字符边界
      const size_t keep = rng() % (text_units.size() + 1);
      text_units.resize(keep);
    } else {
      const size_t append = 1 + rng() % 3;
      for (size_t i = 0; i < append; ++i) {
        text_units.push_back(units[rng() % 6]);
      }
    }
    string text;
    for (const auto& unit : text_units) {
      text += unit;
    }
    SCOPED_TRACE(::testing::Message() << "step=" << step << " text=" << text);
    string one_shot;
    const bool one_changed = pack->ConvertSentence(text, &one_shot);
    const bool inc_changed =
        pack->ConvertSentenceIncremental(text, &state, &metrics);
    EXPECT_EQ(one_changed, inc_changed);
    // 惰性拷贝契约：changed=false 时一次性结果为空，期望输出=原文本
    const string& expected = one_changed ? one_shot : text;
    EXPECT_EQ(expected, state.output);
  }
  EXPECT_GT(metrics.prefix_reuse_calls, 0u);  // 复用路径确实被踩过
  pack.reset();
  std::filesystem::remove(file);
}

// ===== 压缩阈值（G1-G3，登记见 doc/rewrite-pack-failmodes.md）=====

namespace {

// 建 count 个 stage 全绑在 schemaA/sect 下，返回 id 列表。
bool BuildStages(const path& file,
                 int count,
                 size_t value_bytes,
                 vector<RewriteStageId>* ids) {
  RewriteStoreWriter starter(file);
  if (!starter.BeginWorkspace()) {
    return false;
  }
  RewriteStoreWriter writer(file);
  if (!writer.Open()) {
    return false;
  }
  for (int i = 0; i < count; ++i) {
    const RewriteStageId id = TestId(static_cast<uint64_t>(100 + i));
    RewriteStageData stage;
    stage.entries.push_back(
        {"key" + std::to_string(i),
         string(value_bytes, static_cast<char>('a' + i % 26)), ""});
    if (!writer.AppendStage(id, stage)) {
      return false;
    }
    ids->push_back(id);
  }
  vector<RewriteStageBinding> all;
  all.push_back({"sect", *ids});
  if (!writer.CommitSchema("schemaA", all)) {
    return false;
  }
  return writer.CommitWorkspace();
}

// 重绑只留前 keep 个 stage，其余成为死 stage。
bool DropStages(const path& file,
                size_t keep,
                const vector<RewriteStageId>& ids) {
  RewriteStoreWriter starter(file);
  if (!starter.BeginWorkspace()) {
    return false;
  }
  RewriteStoreWriter writer(file);
  if (!writer.Open()) {
    return false;
  }
  vector<RewriteStageId> live(ids.begin(), ids.begin() + keep);
  vector<RewriteStageBinding> fewer;
  fewer.push_back({"sect", live});
  if (!writer.CommitSchema("schemaA", fewer)) {
    return false;
  }
  return writer.CommitWorkspace();
}

}  // namespace

// G1：未达任何阈值时不得压缩——只追加索引，死数据保留。
TEST(RimeRewriteStoreCompactionTest, CompactionStaysLazyBelowThreshold) {
  const path file = TestPath("compact_lazy");
  std::filesystem::remove(file);
  vector<RewriteStageId> ids;
  ASSERT_TRUE(BuildStages(file, 3, 64, &ids));
  const auto before = FileSize(file);
  ASSERT_TRUE(DropStages(file, 1, ids));  // 2 死 stage，垃圾仅百字节
  const auto after = FileSize(file);
  EXPECT_GE(after, before);  // 不收缩（只追加了一份索引）
  auto store = RewriteStore::OpenCached(file);
  ASSERT_TRUE(store);
  EXPECT_EQ(2u, store->generation());
  EXPECT_TRUE(store->OpenStage(ids[0]));
  EXPECT_TRUE(store->OpenStage(ids[2]));  // 死 stage 仍在（惰性回收）
  std::filesystem::remove(file);
}

// G2：死 stage 数达 16 → 物理压缩；死数据从索引消失、live 内容不变。
TEST(RimeRewriteStoreCompactionTest, DeadStageThresholdTriggersCompaction) {
  const path file = TestPath("compact_dead16");
  std::filesystem::remove(file);
  vector<RewriteStageId> ids;
  ASSERT_TRUE(BuildStages(file, 17, 64, &ids));
  const auto before = FileSize(file);
  ASSERT_TRUE(DropStages(file, 1, ids));  // 16 死 stage
  const auto after = FileSize(file);
  EXPECT_LT(after, before);  // 物理回收
  auto store = RewriteStore::OpenCached(file);
  ASSERT_TRUE(store);
  EXPECT_EQ(2u, store->generation());
  auto live = store->OpenStage(ids[0]);
  ASSERT_TRUE(live);
  EXPECT_FALSE(store->OpenStage(ids[16]));  // 死 stage 已从索引消失
  vector<string> values;
  ASSERT_TRUE(live->Lookup("key0", &values));
  ASSERT_EQ(1u, values.size());
  EXPECT_EQ(string(64, 'a'), values[0]);  // live 内容逐字节保留
  std::filesystem::remove(file);
}

// G3：死 stage 不足 16，但垃圾 ≥4MB 且 ≥已提交大小的 1/4 → 同样压缩。
TEST(RimeRewriteStoreCompactionTest, GarbageRatioTriggersCompaction) {
  const path file = TestPath("compact_garbage");
  std::filesystem::remove(file);
  constexpr size_t kBigValue = 320 * 1024;  // 16 个 stage ≈ 5MB
  vector<RewriteStageId> ids;
  ASSERT_TRUE(BuildStages(file, 16, kBigValue, &ids));
  const auto before = FileSize(file);
  ASSERT_TRUE(DropStages(file, 1, ids));  // 15 死 stage（不足 16）
  const auto after = FileSize(file);
  EXPECT_LT(after, before / 2);  // 垃圾占比触发压缩
  auto store = RewriteStore::OpenCached(file);
  ASSERT_TRUE(store);
  EXPECT_TRUE(store->OpenStage(ids[0]));
  EXPECT_FALSE(store->OpenStage(ids[15]));
  vector<string> values;
  auto live = store->OpenStage(ids[0]);
  ASSERT_TRUE(live);
  ASSERT_TRUE(live->Lookup("key0", &values));
  ASSERT_EQ(1u, values.size());
  EXPECT_EQ(string(kBigValue, 'a'), values[0]);
  std::filesystem::remove(file);
}
