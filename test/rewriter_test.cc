// Copyright RIME Developers
// Distributed under the BSD License
//
// Rewriter 运行时端到端契约（登记见 doc/rewrite-pack-failmodes.md R1-R11）。
// 与 rewrite_store_test.cc 的区别：这里走 Filter::Apply 全链路——配置解析、
// option 门控、type
// 过滤、模式（derive/replace/comment）、多级串联、部署后热重载。
#include <gtest/gtest.h>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include <rime/candidate.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/dict/rewrite_store.h>
#include <rime/engine.h>
#include <rime/filter.h>
#include <rime/gear/rewriter.h>
#include <rime/schema.h>
#include <rime/segmentation.h>
#include <rime/service.h>
#include <rime/ticket.h>
#include <rime/translation.h>

using namespace rime;

namespace {

RewriteStageId StageId(uint64_t n) {
  return RewriteStageId{7, n};
}

RewriteStageData Stage(const std::vector<std::pair<string, string>>& pairs) {
  RewriteStageData stage;
  for (const auto& [key, value] : pairs) {
    stage.entries.push_back({key, value, ""});
  }
  return stage;
}

using ResultList = std::vector<std::pair<string, string>>;  // text, comment

ResultList Collect(const an<Translation>& translation) {
  ResultList out;
  if (!translation) {
    return out;
  }
  size_t guard = 0;
  while (!translation->exhausted() && guard++ < 64) {
    auto cand = translation->Peek();
    out.push_back(
        {cand ? cand->text() : string(), cand ? cand->comment() : string()});
    translation->Next();
  }
  return out;
}

}  // namespace

class RimeRewriterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    staging_ = path{"rewriter_test_staging"};
    std::filesystem::remove_all(staging_);
    std::filesystem::create_directories(staging_);
    original_staging_ = Service::instance().deployer().staging_dir;
    Service::instance().deployer().staging_dir = staging_;
  }

  void TearDown() override {
    Service::instance().deployer().staging_dir = original_staging_;
    std::filesystem::remove_all(staging_);
  }

  // 部署一个 store：schemaX 的 <name_space> 绑定 stages（顺序即语义）。
  bool Deploy(const vector<RewriteStageData>& stages,
              const string& name_space = "rewriter") {
    const path file = staging_ / "rewriter.rwp";
    vector<RewriteStageId> ids;
    {
      RewriteStoreWriter starter(file);
      if (!starter.BeginWorkspace()) {
        return false;
      }
    }
    RewriteStoreWriter writer(file);
    if (!writer.Open()) {
      return false;
    }
    for (size_t i = 0; i < stages.size(); ++i) {
      // stage id 必须每次部署都新鲜：AppendStage 对同 id 是幂等 no-op。
      const RewriteStageId id = StageId(++next_stage_id_);
      if (!writer.AppendStage(id, stages[i])) {
        return false;
      }
      ids.push_back(id);
    }
    vector<RewriteStageBinding> bindings;
    bindings.push_back({name_space, ids});
    return writer.CommitSchema("schemaX", bindings) && writer.CommitWorkspace();
  }

  the<Engine> MakeEngine(const std::function<void(Config*)>& configure) {
    the<Config> config(new Config);
    configure(config.get());
    the<Engine> engine(Engine::Create());
    if (!engine) {
      return nullptr;
    }
    engine->ApplySchema(new Schema("schemaX", config.release()));
    return engine;
  }

  static an<Filter> MakeRewriter(Engine* engine) {
    auto* component = Filter::Require("rewriter");
    if (!engine || !component) {
      return nullptr;
    }
    return an<Filter>(component->Create(Ticket(engine, "rewriter")));
  }

  an<Translation> MakeInput(const vector<string>& texts,
                            const string& type = "pinyin") {
    an<FifoTranslation> input(new FifoTranslation);
    for (const auto& text : texts) {
      input->Append(New<SimpleCandidate>(type, 0, text.size(), text));
    }
    return input;
  }

  // 便捷：配置 + 引擎 + rewriter，跑一批候选，返回结果。
  ResultList Run(const std::function<void(Config*)>& configure,
                 const vector<string>& texts,
                 const string& type = "pinyin",
                 Engine** engine_out = nullptr) {
    the<Engine> engine = MakeEngine(configure);
    if (!engine) {
      return {};
    }
    an<Filter> rewriter = MakeRewriter(engine.get());
    if (!rewriter) {
      return {};
    }
    auto out = Collect(rewriter->Apply(MakeInput(texts, type), nullptr));
    if (engine_out) {
      *engine_out = engine.release();
    }
    return out;
  }

  path staging_;
  path original_staging_;
  uint64_t next_stage_id_ = 0;
};

// R1：derive 模式 = 原候选在前、影子候选在后（顺序是契约）。
TEST_F(RimeRewriterTest, DeriveModeAppendsShadowAfterOriginal) {
  ASSERT_TRUE(Deploy({Stage({{"abc", "ABC"}})}));
  auto out =
      Run([](Config* c) { c->SetBool("rewriter/option", true); }, {"abc"});
  ASSERT_EQ(2u, out.size());
  EXPECT_EQ("abc", out[0].first);
  EXPECT_EQ("ABC", out[1].first);
  EXPECT_TRUE(out[1].second.empty());  // 默认不派生注释
}

// R2：replace 模式丢弃原候选；未命中时回退原候选。
TEST_F(RimeRewriterTest, ReplaceModeDropsOriginalAndFallsBack) {
  ASSERT_TRUE(Deploy({Stage({{"abc", "ABC"}})}));
  auto out = Run(
      [](Config* c) {
        c->SetBool("rewriter/option", true);
        c->SetString("rewriter/mode", "replace");
      },
      {"abc", "zzz"});
  ASSERT_EQ(2u, out.size());
  EXPECT_EQ("ABC", out[0].first);  // 命中：只有替换值
  EXPECT_EQ("zzz", out[1].first);  // 未命中：原样保留
}

// R3：comment 模式不改文本，只挂模板注释。
TEST_F(RimeRewriterTest, CommentModeUsesTemplate) {
  ASSERT_TRUE(Deploy({Stage({{"abc", "ABC"}})}));
  auto out = Run(
      [](Config* c) {
        c->SetBool("rewriter/option", true);
        c->SetString("rewriter/mode", "comment");
      },
      {"abc"});
  ASSERT_EQ(1u, out.size());
  EXPECT_EQ("abc", out[0].first);
  EXPECT_EQ("〔ABC〕", out[0].second);  // 默认模板
}

// R4：option 门控——未开启时整条翻译不动；开启后才重写。
TEST_F(RimeRewriterTest, OptionGateControlsActivation) {
  ASSERT_TRUE(Deploy({Stage({{"abc", "ABC"}})}));
  const auto configure = [](Config* c) {
    c->SetString("rewriter/option", "abc_opt");
  };
  the<Engine> engine = MakeEngine(configure);
  ASSERT_TRUE(engine);
  an<Filter> rewriter = MakeRewriter(engine.get());
  ASSERT_TRUE(rewriter);

  auto before = Collect(rewriter->Apply(MakeInput({"abc"}), nullptr));
  ASSERT_EQ(1u, before.size());
  EXPECT_EQ("abc", before[0].first);  // 未开启：原样

  engine->context()->set_option("abc_opt", true);
  auto after = Collect(rewriter->Apply(MakeInput({"abc"}), nullptr));
  ASSERT_EQ(2u, after.size());
  EXPECT_EQ("ABC", after[1].first);
}

// R5：excluded_types 命中的候选跳过重写，其它类型照常。
TEST_F(RimeRewriterTest, ExcludedTypesSkipRewrite) {
  ASSERT_TRUE(Deploy({Stage({{"abc", "ABC"}})}));
  const auto configure = [](Config* c) {
    c->SetBool("rewriter/option", true);
    auto list = New<ConfigList>();
    list->Append(New<ConfigValue>("pinyin"));
    c->SetItem("rewriter/excluded_types", list);
  };
  ASSERT_EQ(1u, Run(configure, {"abc"}, "pinyin").size());  // 被排除
  auto table = Run(configure, {"abc"}, "table");
  ASSERT_EQ(2u, table.size());  // 其它类型正常重写
  EXPECT_EQ("ABC", table[1].first);
}

// R6：store 缺失时优雅降级（不崩溃、不重写）。
TEST_F(RimeRewriterTest, MissingStoreDegradesGracefully) {
  // 不部署 store
  auto out =
      Run([](Config* c) { c->SetBool("rewriter/option", true); }, {"abc"});
  ASSERT_EQ(1u, out.size());
  EXPECT_EQ("abc", out[0].first);
}

// R7：candidate_type 覆盖影子类型；preserve 保留原类型。
TEST_F(RimeRewriterTest, CandidateTypeOverrideAndPreserve) {
  ASSERT_TRUE(Deploy({Stage({{"abc", "ABC"}})}));
  {
    the<Engine> engine = MakeEngine([](Config* c) {
      c->SetBool("rewriter/option", true);
      c->SetString("rewriter/candidate_type", "rewrite");
    });
    ASSERT_TRUE(engine);
    an<Filter> rewriter = MakeRewriter(engine.get());
    ASSERT_TRUE(rewriter);
    auto translation = rewriter->Apply(MakeInput({"abc"}), nullptr);
    ASSERT_TRUE(translation);
    while (!translation->exhausted() && translation->Peek()->text() != "ABC") {
      translation->Next();
    }
    ASSERT_FALSE(translation->exhausted());
    EXPECT_EQ("rewrite", translation->Peek()->type());
  }
  {
    the<Engine> engine = MakeEngine([](Config* c) {
      c->SetBool("rewriter/option", true);
      c->SetString("rewriter/candidate_type", "preserve");
    });
    ASSERT_TRUE(engine);
    an<Filter> rewriter = MakeRewriter(engine.get());
    ASSERT_TRUE(rewriter);
    auto translation = rewriter->Apply(MakeInput({"abc"}), nullptr);
    ASSERT_TRUE(translation);
    while (!translation->exhausted() && translation->Peek()->text() != "ABC") {
      translation->Next();
    }
    ASSERT_FALSE(translation->exhausted());
    EXPECT_EQ("pinyin", translation->Peek()->type());
  }
}

// R8：多级串联按绑定顺序逐级作用。
TEST_F(RimeRewriterTest, MultiStageChainAppliesInOrder) {
  ASSERT_TRUE(Deploy({Stage({{"abc", "xyz"}}), Stage({{"xyz", "XYZ"}})}));
  auto out =
      Run([](Config* c) { c->SetBool("rewriter/option", true); }, {"abc"});
  ASSERT_EQ(2u, out.size());
  EXPECT_EQ("abc", out[0].first);
  EXPECT_EQ("XYZ", out[1].first);  // 两级都生效
}

// R9：部署后热重载——ReleaseStagesForDeployment 之后拿到新代际数据。
TEST_F(RimeRewriterTest, ReleaseStagesPicksUpFreshDeployment) {
  ASSERT_TRUE(Deploy({Stage({{"abc", "AAA"}})}));
  the<Engine> engine =
      MakeEngine([](Config* c) { c->SetBool("rewriter/option", true); });
  ASSERT_TRUE(engine);
  an<Filter> rewriter = MakeRewriter(engine.get());
  ASSERT_TRUE(rewriter);

  auto first = Collect(rewriter->Apply(MakeInput({"abc"}), nullptr));
  ASSERT_EQ(2u, first.size());
  EXPECT_EQ("AAA", first[1].first);

  ASSERT_TRUE(Deploy({Stage({{"abc", "BBB"}})}));  // 新代际
  auto cached = Collect(rewriter->Apply(MakeInput({"abc"}), nullptr));
  ASSERT_EQ(2u, cached.size());
  EXPECT_EQ("AAA", cached[1].first);  // 未释放前仍用旧状态

  Rewriter::ReleaseStagesForDeployment();
  auto reloaded = Collect(rewriter->Apply(MakeInput({"abc"}), nullptr));
  ASSERT_EQ(2u, reloaded.size());
  EXPECT_EQ("BBB", reloaded[1].first);  // 释放后重新打开
}

// R10：enable_sentence 走句子级增量路径，结果与 lookup 一致。
TEST_F(RimeRewriterTest, SentenceModeUsesIncrementalPath) {
  ASSERT_TRUE(Deploy({Stage({{"abc", "ABC"}})}));
  const auto configure = [](Config* c) {
    c->SetBool("rewriter/option", true);
    c->SetBool("rewriter/enable_sentence", true);
  };
  auto hit = Run(configure, {"abc"});
  ASSERT_EQ(2u, hit.size());
  EXPECT_EQ("ABC", hit[1].first);
  auto miss = Run(configure, {"zzz"});
  ASSERT_EQ(1u, miss.size());  // 未命中不产生影子候选
}

// R11：tags 门控——不匹配的 segment 不处理。
TEST_F(RimeRewriterTest, TagGateRejectsUnmatchedSegment) {
  ASSERT_TRUE(Deploy({Stage({{"abc", "ABC"}})}));
  the<Engine> engine = MakeEngine([](Config* c) {
    c->SetBool("rewriter/option", true);
    auto tags = New<ConfigList>();
    tags->Append(New<ConfigValue>("abc"));
    c->SetItem("rewriter/tags", tags);
  });
  ASSERT_TRUE(engine);
  an<Filter> rewriter = MakeRewriter(engine.get());
  ASSERT_TRUE(rewriter);
  Segment plain(0, 3);
  EXPECT_FALSE(rewriter->AppliesToSegment(&plain));
  Segment tagged(0, 3);
  tagged.tags.insert("abc");
  EXPECT_TRUE(rewriter->AppliesToSegment(&tagged));
}
