// Copyright RIME Developers
// Distributed under the BSD License
//
// 编辑-学习编排层：user_rank_actions.ApplyEditAction 与 memory OnCommit
// （登记见 doc/editor-learning-failmodes.md EL1-EL6）。
// harness 沿用 partial_commit_test 的合成 composition 模式：手工 Menu +
// Phrase 候选（language 指针指向被测 Memory 的 language，保证
// OwnsPhrase/FindManualSource 命中）。用户库经组件预置。
// 上屏用显式 ctx->Commit()：ConfirmCurrentSelection 对 kGuess 段只确认
// 不上屏，不触发 commit 通知（实测）。
// 断言一律走池内句柄（storage_identity 裸指针）：第二个句柄会撞
// LevelDb LOCK（实测）。池化句柄上 Close→OpenReadOnly 也会崩（教训 15）。
#include <gtest/gtest.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>
#include <rime/candidate.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/dict/db.h>
#include <rime/dict/dict_compiler.h>
#include <rime/dict/user_db.h>
#include <rime/dict/user_dictionary.h>
#include <rime/engine.h>
#include <rime/gear/memory.h>
#include <rime/gear/translator_commons.h>
#include <rime/gear/user_rank_actions.h>
#include <rime/menu.h>
#include <rime/schema.h>
#include <rime/service.h>
#include <rime/ticket.h>
#include <rime/translation.h>

using namespace rime;

namespace {

const char* kDictName = "el_dict";

}  // namespace

class RimeEditorLearningTest : public ::testing::Test {
 protected:
  struct TestMemoryLike : Memory {
    explicit TestMemoryLike(const Ticket& ticket) : Memory(ticket) {}
    vector<string> memorized;
    bool Memorize(const CommitEntry& entry) override {
      memorized.push_back(entry.text);
      return true;
    }
  };

  void SetUp() override {
    // 崩溃/中断的上一轮可能残留半写库，先清干净。
    std::filesystem::remove_all("el_dict.userdb");
    std::ofstream("el_dict.dict.yaml")
        << "---\nname: " << kDictName
        << "\nversion: '1.0'\nsort: "
           "by_weight\n...\n好\thao3\t100\n郝\thao3\t50\n";
    std::ofstream("el_test.schema.yaml")
        << "build:\n  dictionaries:\n    " << kDictName
        << ":\n      algebra:\n        pinyin: ['xform/[0-9]$//']\n"
           "translator:\n  dictionary: "
        << kDictName << "\n  prism: pinyin\n  user_dict: " << kDictName
        << "\n  user_dict_mode: auto\n"
           "translator_m:\n  dictionary: "
        << kDictName << "\n  prism: pinyin\n  user_dict: " << kDictName
        << "\n  user_dict_mode: manual\n";
    ASSERT_TRUE(DictCompiler::CompileDictionaries(path{"el_test.schema.yaml"}));

    auto& deployer = Service::instance().deployer();
    saved_staging_ = deployer.staging_dir;
    deployer.staging_dir = std::filesystem::current_path();

    component_ = UserDb::Require("userdb");
    ASSERT_TRUE(component_);
    ResetDb();

    the<Config> config(new Config);
    ASSERT_TRUE(config->LoadFromFile(path{"el_test.schema.yaml"}));
    engine_.reset(Engine::Create());
    ASSERT_TRUE(engine_);
    engine_->ApplySchema(new Schema("el_test", config.release()));
  }

  void TearDown() override {
    memories_.clear();
    engine_.reset();
    the<Db> db(component_->Create(kDictName));
    if (db && db->Exists()) {
      db->Remove();
    }
    auto& deployer = Service::instance().deployer();
    deployer.staging_dir = saved_staging_;
    for (const char* f :
         {"el_dict.dict.yaml", "el_test.schema.yaml", "el_dict.table.bin",
          "el_dict.reverse.bin", "el_dict.pinyin.prism.bin"}) {
      std::remove(f);
    }
  }

  void ResetDb() {
    the<Db> db(component_->Create(kDictName));
    ASSERT_TRUE(db);
    if (db->Exists()) {
      ASSERT_TRUE(db->Remove());
    }
    ASSERT_TRUE(db->Open());
    ASSERT_TRUE(db->CreateMetadata());
    ASSERT_TRUE(db->MetaUpdate("/tick", "10"));
    ASSERT_TRUE(db->Close());
  }

  // 创建挂到本引擎的 Memory（注册进 registry），并预置用户库条目。
  // 返回池内 Db 裸指针（教训 13：不包 owning shared_ptr）。
  TestMemoryLike* MakeMemory(const string& name_space,
                             const vector<std::pair<string, string>>& entries,
                             Db** db_out) {
    memories_.emplace_back(
        std::make_unique<TestMemoryLike>(Ticket(engine_.get(), name_space)));
    TestMemoryLike* mem = memories_.back().get();
    EXPECT_TRUE(mem->user_dict() && mem->user_dict()->loaded());
    *db_out = const_cast<Db*>(mem->user_dict()->storage_identity());
    EXPECT_TRUE(*db_out && (*db_out)->loaded());
    for (const auto& [key, value] : entries) {
      EXPECT_TRUE((*db_out)->Update(key, value));
    }
    return mem;
  }

  // 合成 composition：单 segment + 手工菜单，段标记已确认
  // （OnCommit 的立即保存分支依赖 status >= kConfirmed）。
  void SetComposition(Context* ctx,
                      const vector<an<Phrase>>& phrases,
                      size_t selected) {
    auto menu = New<Menu>();
    for (const auto& phrase : phrases) {
      menu->AddTranslation(New<UniqueTranslation>(phrase));
    }
    ASSERT_EQ(phrases.size(), menu->Prepare(phrases.size()));
    ctx->set_input("hao");
    Composition& composition = ctx->composition();
    composition.clear();
    composition.emplace_back(0, 3);
    composition[0].status = Segment::kConfirmed;
    composition[0].menu = menu;
    composition[0].selected_index = selected;
  }

  an<Phrase> MakePhrase(Memory* mem, const string& text) {
    auto entry = New<DictEntry>();
    entry->text = text;
    entry->custom_code = "hao";
    entry->weight = 1.0;
    auto phrase = New<Phrase>(mem->language(), "table", 0, 3, entry);
    return phrase;
  }

  string FetchVia(Db* db, const string& key) {
    string value;
    db->Fetch(key, &value);
    return value;
  }

  UserDbState StateVia(Db* db, const string& key) {
    UserDbState state;
    string raw = FetchVia(db, key);
    EXPECT_TRUE(state.Unpack(raw)) << "key=" << key << " raw=" << raw;
    return state;
  }

  static constexpr const char* kHaoKey = "hao \t好";
  static constexpr const char* kHaoPinKey = "hao \t郝";

  vector<std::unique_ptr<TestMemoryLike>> memories_;
  the<Engine> engine_;
  UserDb::Component* component_ = nullptr;
  path saved_staging_;
};

// EL1：auto 模式提交 → OnCommit 编排把自有短语交给 Memorize。
TEST_F(RimeEditorLearningTest, AutoModeCommitLearnsViaMemorize) {
  Db* db = nullptr;
  auto* mem = MakeMemory("translator", {}, &db);
  Context* ctx = engine_->context();
  SetComposition(ctx, {MakePhrase(mem, "好")}, 0);

  ctx->Commit();

  ASSERT_FALSE(mem->memorized.empty());
  EXPECT_EQ("好", mem->memorized.front());
}

// EL6：manual 模式提交不走自动学习（Memorize 不被调用）。
TEST_F(RimeEditorLearningTest, ManualModeCommitSkipsAutoLearning) {
  Db* db = nullptr;
  auto* mem = MakeMemory("translator_m", {}, &db);
  Context* ctx = engine_->context();
  SetComposition(ctx, {MakePhrase(mem, "好")}, 0);

  ctx->Commit();

  EXPECT_TRUE(mem->memorized.empty());
}

// EL2：菜单里 MoveDown 未手排词 → 钉到邻位之后（经 ApplyEditAction 编排）。
TEST_F(RimeEditorLearningTest, MenuMoveDownPinsBelowNeighbor) {
  Db* db = nullptr;
  auto* mem = MakeMemory(
      "translator", {{kHaoKey, "1,1,1,0,0"}, {kHaoPinKey, "1,1,1,0,0"}}, &db);
  Context* ctx = engine_->context();
  SetComposition(ctx, {MakePhrase(mem, "好"), MakePhrase(mem, "郝")}, 0);

  EXPECT_TRUE(user_rank::ApplyEditAction(engine_.get(), ctx,
                                         user_rank::EditAction::kMoveDown));
  auto state = StateVia(db, kHaoKey);
  EXPECT_EQ(2, state.position);  // 钉到原邻位之后
  auto neighbor = StateVia(db, kHaoPinKey);
  EXPECT_EQ(0, neighbor.position);  // 未手排邻词不被牵连
}

// EL3：MoveUp 遇已手排邻词 → 交换位置（好↔郝）。
TEST_F(RimeEditorLearningTest, MenuMoveUpSwapsWithPinnedNeighbor) {
  Db* db = nullptr;
  auto* mem = MakeMemory(
      "translator", {{kHaoKey, "1,1,1,0,0"}, {kHaoPinKey, "2,1,8,1,7"}}, &db);
  Context* ctx = engine_->context();
  // 菜单 [郝(已钉 1), 好(未钉)]，选中 idx1=好，上移 → 与郝交换。
  SetComposition(ctx, {MakePhrase(mem, "郝"), MakePhrase(mem, "好")}, 1);

  EXPECT_TRUE(user_rank::ApplyEditAction(engine_.get(), ctx,
                                         user_rank::EditAction::kMoveUp));
  EXPECT_EQ(1, StateVia(db, kHaoKey).position);     // 好 顶到 1
  EXPECT_EQ(2, StateVia(db, kHaoPinKey).position);  // 郝 换到 2
}

// EL4：ResetAuto 只清自动学习，手排保留。
TEST_F(RimeEditorLearningTest, ResetAutoKeepsManualPin) {
  Db* db = nullptr;
  auto* mem = MakeMemory("translator", {{kHaoKey, "3,1,10,2,7"}}, &db);
  Context* ctx = engine_->context();
  SetComposition(ctx, {MakePhrase(mem, "好")}, 0);

  EXPECT_TRUE(user_rank::ApplyEditAction(engine_.get(), ctx,
                                         user_rank::EditAction::kResetAuto));
  auto state = StateVia(db, kHaoKey);
  EXPECT_TRUE(state.is_auto_deleted());  // 自动学习标记删除
  EXPECT_EQ(2, state.position);          // 手排保留
}

// EL5：ResetManual 只清手排，自动学习保留。
TEST_F(RimeEditorLearningTest, ResetManualKeepsLearning) {
  Db* db = nullptr;
  auto* mem = MakeMemory("translator", {{kHaoKey, "3,1,10,2,7"}}, &db);
  Context* ctx = engine_->context();
  SetComposition(ctx, {MakePhrase(mem, "好")}, 0);

  EXPECT_TRUE(user_rank::ApplyEditAction(engine_.get(), ctx,
                                         user_rank::EditAction::kResetManual));
  auto state = StateVia(db, kHaoKey);
  EXPECT_EQ(0, state.position);       // 手排清除
  EXPECT_GT(state.manual_stamp, 0u);  // 版本推进
  EXPECT_EQ(3, state.commits);        // 自动学习保留
}
