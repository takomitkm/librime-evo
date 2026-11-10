// Copyright RIME Developers
// Distributed under the BSD License
//
// 部署侧契约：RewriteCompiler（配置→stage→store）与 RewritePresetCatalog
// （共享 preset 目录）。登记见 doc/rewrite-compiler-failmodes.md D1-D15。
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <rime/config.h>
#include <rime/deployer.h>
#include <rime/dict/rewrite_pack.h>
#include <rime/dict/rewrite_store.h>
#include <rime/lever/rewrite_compiler.h>
#include <rime/lever/rewrite_preset.h>
#include <rime/service.h>

using namespace rime;

namespace {

const char* kStoreName = "rewriter.rwp";

std::uint64_t FileSize(const path& file) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(file, ec);
  return ec ? 0 : size;
}

void WriteTextFile(const path& file, const string& content) {
  std::filesystem::create_directories(file.parent_path());
  std::ofstream out(static_cast<const std::filesystem::path&>(file),
                    std::ios::binary | std::ios::trunc);
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

an<ConfigList> MakeList(const std::vector<string>& values) {
  auto list = New<ConfigList>();
  for (const auto& value : values) {
    list->Append(New<ConfigValue>(value));
  }
  return list;
}

}  // namespace

class RimeRewriteCompilerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = path{"rewrite_compiler_test"};
    std::filesystem::remove_all(root_);
    user_data_ = root_ / "user";
    shared_data_ = root_ / "shared";
    staging_ = root_ / "staging";
    std::filesystem::create_directories(user_data_);
    std::filesystem::create_directories(shared_data_);
    std::filesystem::create_directories(staging_);

    auto& deployer = Service::instance().deployer();
    saved_user_ = deployer.user_data_dir;
    saved_shared_ = deployer.shared_data_dir;
    saved_staging_ = deployer.staging_dir;
    deployer.user_data_dir = user_data_;
    deployer.shared_data_dir = shared_data_;
    deployer.staging_dir = staging_;
  }

  void TearDown() override {
    auto& deployer = Service::instance().deployer();
    deployer.user_data_dir = saved_user_;
    deployer.shared_data_dir = saved_shared_;
    deployer.staging_dir = saved_staging_;
    std::filesystem::remove_all(root_);
  }

  path store_path() const { return staging_ / kStoreName; }

  // filters 决定能找到哪些 rewriter section；extra 配置按 section 写入。
  the<Config> MakeConfig(const std::vector<string>& filters,
                         const std::vector<string>& section_files = {"rw.txt"},
                         const string& key_xlit = "",
                         const string& preset = "") {
    the<Config> config(new Config);
    config->SetItem("engine/filters", MakeList(filters));
    if (!preset.empty()) {
      config->SetString("rewriter/preset", preset);
    } else if (!section_files.empty()) {
      config->SetItem("rewriter/files", MakeList(section_files));
    }
    if (!key_xlit.empty()) {
      config->SetString("rewriter/key_xlit", key_xlit);
    }
    return config;
  }

  bool Compile(Config* config, const string& schema_id = "schemaX") {
    RewriteCompiler compiler(schema_id, config,
                             &Service::instance().deployer());
    return compiler.Compile();
  }

  vector<RewriteStageId> Binding(an<const RewriteStore> store,
                                 const string& schema_id = "schemaX",
                                 const string& section = "rewriter") {
    vector<RewriteStageId> ids;
    if (store) {
      store->FindStageSequence(schema_id, section, &ids);
    }
    return ids;
  }

  path root_;
  path user_data_;
  path shared_data_;
  path staging_;
  path saved_user_;
  path saved_shared_;
  path saved_staging_;
};

// D1：files → stage 落库、绑定可读、条目可查。
TEST_F(RimeRewriteCompilerTest, CompileFilesIntoStore) {
  WriteTextFile(user_data_ / "rw.txt", "abc\tABC\n");
  auto config = MakeConfig({"rewriter"});
  ASSERT_TRUE(Compile(config.get()));
  auto store = RewriteStore::OpenCached(store_path());
  ASSERT_TRUE(store);
  auto ids = Binding(store);
  ASSERT_EQ(1u, ids.size());
  auto pack = store->OpenStage(ids[0]);
  ASSERT_TRUE(pack);
  vector<string> values;
  ASSERT_TRUE(pack->Lookup("abc", &values));
  ASSERT_EQ(1u, values.size());
  EXPECT_EQ("ABC", values[0]);
}

// D2：内容未变时重编译复用同一 stage id，且不追加任何字节。
TEST_F(RimeRewriteCompilerTest, RecompileSameContentReusesStage) {
  WriteTextFile(user_data_ / "rw.txt", "abc\tABC\n");
  auto config = MakeConfig({"rewriter"});
  ASSERT_TRUE(Compile(config.get()));
  auto store = RewriteStore::OpenCached(store_path());
  ASSERT_TRUE(store);
  auto first = Binding(store);
  ASSERT_EQ(1u, first.size());
  const auto size_before = FileSize(store_path());
  const auto generation_before = store->generation();

  ASSERT_TRUE(Compile(config.get()));
  auto again = RewriteStore::OpenCached(store_path());
  ASSERT_TRUE(again);
  auto second = Binding(again);
  ASSERT_EQ(1u, second.size());
  EXPECT_EQ(first[0], second[0]);                  // 同一指纹
  EXPECT_EQ(size_before, FileSize(store_path()));  // 干净提交零写入
  EXPECT_EQ(generation_before, again->generation());
}

// D3：内容变化 → 新 stage id（同长度不同内容也必须区分）。
TEST_F(RimeRewriteCompilerTest, ContentChangeProducesNewStage) {
  WriteTextFile(user_data_ / "rw.txt", "abc\tABC\n");
  auto config = MakeConfig({"rewriter"});
  ASSERT_TRUE(Compile(config.get()));
  auto store = RewriteStore::OpenCached(store_path());
  ASSERT_TRUE(store);
  auto first = Binding(store);
  ASSERT_EQ(1u, first.size());

  WriteTextFile(user_data_ / "rw.txt", "abd\tABD\n");  // 与上者等长
  ASSERT_TRUE(Compile(config.get()));
  auto after = RewriteStore::OpenCached(store_path());
  ASSERT_TRUE(after);
  auto second = Binding(after);
  ASSERT_EQ(1u, second.size());
  EXPECT_NE(first[0], second[0]);
  auto pack = after->OpenStage(second[0]);
  ASSERT_TRUE(pack);
  vector<string> values;
  ASSERT_TRUE(pack->Lookup("abd", &values));
  EXPECT_EQ("ABD", values[0]);
}

// D4：/files 与 /preset 必须二选一（两者同时设置时）；
// 注意 MakeConfig 是 else-if，这里必须显式同时写入两个键。
TEST_F(RimeRewriteCompilerTest, FilesAndPresetMutuallyExclusive) {
  WriteTextFile(user_data_ / "rw.txt", "abc\tABC\n");
  the<Config> config(new Config);
  config->SetItem("engine/filters", MakeList({"rewriter"}));
  config->SetItem("rewriter/files", MakeList({"rw.txt"}));
  config->SetString("rewriter/preset", "demo");
  EXPECT_FALSE(Compile(config.get()));
}

// D5：/preset 不得与 /key_xlit 组合（preset 本身有效，失败只能来自互斥检查）。
TEST_F(RimeRewriteCompilerTest, PresetWithKeyXlitRejected) {
  const path preset_dir = shared_data_ / "rewriter";
  WriteTextFile(preset_dir / "p1.txt", "a	X
");
  WriteTextFile(preset_dir / "presets.yaml",
                "version: 1
presets:
  demo: [s1]
"
                "stages:
  s1:
    files: [p1.txt]
");
  auto config = MakeConfig({"rewriter"}, {}, "a>b", "demo");
  EXPECT_FALSE(Compile(config.get()));
}

// D6：声明的源文件缺失 → 编译失败（不能静默产出空 stage）。
TEST_F(RimeRewriteCompilerTest, MissingSourceFileFails) {
  auto config = MakeConfig({"rewriter"}, {"missing.txt"});
  EXPECT_FALSE(Compile(config.get()));
}

// D7：越权路径（../ 逃逸）必须拒绝。
TEST_F(RimeRewriteCompilerTest, PathTraversalRejected) {
  WriteTextFile(root_ / "outside.txt", "abc\tABC\n");
  auto config = MakeConfig({"rewriter"}, {"../outside.txt"});
  EXPECT_FALSE(Compile(config.get()));
}

// D8：key_xlit 改写查询键，原始键进 preedit。
TEST_F(RimeRewriteCompilerTest, KeyXlitRewritesKeyAndKeepsPreedit) {
  WriteTextFile(user_data_ / "rw.txt", "a\tX\n");
  auto config = MakeConfig({"rewriter"}, {"rw.txt"}, "a>b");
  ASSERT_TRUE(Compile(config.get()));
  auto store = RewriteStore::OpenCached(store_path());
  ASSERT_TRUE(store);
  auto ids = Binding(store);
  ASSERT_EQ(1u, ids.size());
  auto pack = store->OpenStage(ids[0]);
  ASSERT_TRUE(pack);
  vector<RewriteResult> results;
  ASSERT_TRUE(pack->LookupWithPreedit("b", &results));
  ASSERT_EQ(1u, results.size());
  EXPECT_EQ("X", results[0].value);
  EXPECT_EQ("a", results[0].preedit);  // 原键保留为预编辑
}

// D9：缺 TAB 的畸形行 → 整体失败（不吞半份数据）。
TEST_F(RimeRewriteCompilerTest, MalformedLineFails) {
  WriteTextFile(user_data_ / "rw.txt", "abc\tABC\nno-tab-here\n");
  auto config = MakeConfig({"rewriter"});
  EXPECT_FALSE(Compile(config.get()));
}

// D10：BOM、注释行、空行、CRLF 都不得污染条目。
TEST_F(RimeRewriteCompilerTest, BomCommentsAndCrlfHandled) {
  const string content =
      "\xef\xbb\xbf# comment line\r\n"
      "\r\n"
      "  # indented comment\r\n"
      "abc\tABC\r\n";
  WriteTextFile(user_data_ / "rw.txt", content);
  auto config = MakeConfig({"rewriter"});
  ASSERT_TRUE(Compile(config.get()));
  auto store = RewriteStore::OpenCached(store_path());
  ASSERT_TRUE(store);
  auto ids = Binding(store);
  ASSERT_EQ(1u, ids.size());
  auto pack = store->OpenStage(ids[0]);
  ASSERT_TRUE(pack);
  vector<string> values;
  ASSERT_TRUE(pack->Lookup("abc", &values));
  ASSERT_EQ(1u, values.size());
  EXPECT_EQ("ABC", values[0]);  // 值不带 \r
  EXPECT_FALSE(pack->Lookup("\xef\xbb\xbf# comment line", &values));
}

// D11：文件列表重复项去重（同一文件两次列出 = 一次）。
TEST_F(RimeRewriteCompilerTest, DuplicateFileListingDeduped) {
  WriteTextFile(user_data_ / "rw.txt", "abc\tABC\n");
  auto single = MakeConfig({"rewriter"}, {"rw.txt"});
  ASSERT_TRUE(Compile(single.get()));
  auto store = RewriteStore::OpenCached(store_path());
  ASSERT_TRUE(store);
  auto first = Binding(store);
  ASSERT_EQ(1u, first.size());

  auto dup = MakeConfig({"rewriter"}, {"rw.txt", "rw.txt"});
  ASSERT_TRUE(Compile(dup.get()));
  auto after = RewriteStore::OpenCached(store_path());
  ASSERT_TRUE(after);
  auto second = Binding(after);
  ASSERT_EQ(1u, second.size());
  EXPECT_EQ(first[0], second[0]);  // 去重后指纹相同
}

// D12：schema 不再声明 rewriter 时，旧绑定必须被清除。
TEST_F(RimeRewriteCompilerTest, RemovingSectionClearsBinding) {
  WriteTextFile(user_data_ / "rw.txt", "abc\tABC\n");
  auto config = MakeConfig({"rewriter"});
  ASSERT_TRUE(Compile(config.get()));
  ASSERT_EQ(1u, Binding(RewriteStore::OpenCached(store_path())).size());

  auto plain = MakeConfig({"abc_segmentor"}, {});
  ASSERT_TRUE(Compile(plain.get()));
  EXPECT_TRUE(Binding(RewriteStore::OpenCached(store_path())).empty());
}

// D13：preset 从 shared/rewriter/presets.yaml 解析并落库。
TEST_F(RimeRewriteCompilerTest, PresetResolvesAndCompiles) {
  const path preset_dir = shared_data_ / "rewriter";
  WriteTextFile(preset_dir / "p1.txt", "abc\tZZZ\n");
  WriteTextFile(preset_dir / "presets.yaml",
                "version: 1\n"
                "presets:\n"
                "  demo: [s1]\n"
                "stages:\n"
                "  s1:\n"
                "    files: [p1.txt]\n");
  auto config = MakeConfig({"rewriter"}, {}, "", "demo");
  ASSERT_TRUE(Compile(config.get()));
  auto store = RewriteStore::OpenCached(store_path());
  ASSERT_TRUE(store);
  auto ids = Binding(store);
  ASSERT_EQ(1u, ids.size());
  auto pack = store->OpenStage(ids[0]);
  ASSERT_TRUE(pack);
  vector<string> values;
  ASSERT_TRUE(pack->Lookup("abc", &values));
  EXPECT_EQ("ZZZ", values[0]);
}

// D14：preset 版本不匹配 / 未定义 → 编译失败。
TEST_F(RimeRewriteCompilerTest, BadPresetCatalogFails) {
  const path preset_dir = shared_data_ / "rewriter";
  WriteTextFile(preset_dir / "p1.txt", "abc\tZZZ\n");
  WriteTextFile(preset_dir / "presets.yaml",
                "version: 2\n"
                "presets:\n"
                "  demo: [s1]\n"
                "stages:\n"
                "  s1:\n"
                "    files: [p1.txt]\n");
  auto wrong_version = MakeConfig({"rewriter"}, {}, "", "demo");
  EXPECT_FALSE(Compile(wrong_version.get()));

  WriteTextFile(preset_dir / "presets.yaml",
                "version: 1\n"
                "presets:\n"
                "  demo: [s1]\n"
                "stages:\n"
                "  s1:\n"
                "    files: [p1.txt]\n");
  auto unknown = MakeConfig({"rewriter"}, {}, "", "not-defined");
  EXPECT_FALSE(Compile(unknown.get()));
}

// D15：workspace 中途 abort → 旧绑定与旧数据完好。
TEST_F(RimeRewriteCompilerTest, WorkspaceAbortKeepsPreviousDeployment) {
  WriteTextFile(user_data_ / "rw.txt", "abc\tAAA\n");
  auto config = MakeConfig({"rewriter"});
  ASSERT_TRUE(Compile(config.get()));
  auto store = RewriteStore::OpenCached(store_path());
  ASSERT_TRUE(store);
  auto first = Binding(store);
  ASSERT_EQ(1u, first.size());

  auto& deployer = Service::instance().deployer();
  ASSERT_TRUE(RewriteCompiler::BeginWorkspace(&deployer));
  WriteTextFile(user_data_ / "rw.txt", "abc\tBBB\n");
  ASSERT_TRUE(Compile(config.get()));
  ASSERT_TRUE(RewriteCompiler::AbortWorkspace(&deployer));

  auto after = RewriteStore::OpenCached(store_path());
  ASSERT_TRUE(after);
  auto second = Binding(after);
  ASSERT_EQ(1u, second.size());
  EXPECT_EQ(first[0], second[0]);  // 绑定回滚
  auto pack = after->OpenStage(second[0]);
  ASSERT_TRUE(pack);
  vector<string> values;
  ASSERT_TRUE(pack->Lookup("abc", &values));
  EXPECT_EQ("AAA", values[0]);  // 数据也是旧的
}
