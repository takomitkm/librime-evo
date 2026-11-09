// Copyright RIME Developers
// Distributed under the BSD License
//
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
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
