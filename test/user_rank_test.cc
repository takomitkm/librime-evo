// Copyright RIME Developers
// Distributed under the BSD License
//
// user_rank::Apply / FilterPinnedStatic / PreferUser 契约（登记见
// doc/user-rank-failmodes.md J1-J6）。系统侧用真实编译词典，用户侧手工构造。
#include <gtest/gtest.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <rime/algo/syllabifier.h>
#include <rime/config.h>
#include <rime/dict/dictionary.h>
#include <rime/dict/dict_compiler.h>
#include <rime/dict/user_dictionary.h>
#include <rime/dict/vocabulary.h>
#include <rime/gear/user_rank.h>
#include <rime/schema.h>
#include <rime/ticket.h>

using namespace rime;

namespace {

an<DictEntry> MakeUser(const string& text,
                       double weight,
                       int position,
                       uint64_t version,
                       Code code,
                       int commits = 1) {
  auto e = New<DictEntry>();
  e->text = text;
  e->weight = weight;
  e->manual_position = position;
  e->manual_version = version;
  e->code = code;
  e->commit_count = commits;
  e->remaining_code_length = 0;
  return e;
}

Code SoloCode(int syllable) {
  Code code;
  code.push_back(static_cast<SyllableId>(syllable));
  return code;
}

}  // namespace

class RimeUserRankTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::ofstream("rank_test.dict.yaml")
        << "---\nname: rank_test\nversion: '1.0'\nsort: by_weight\n...\n"
        << "好\thao3\t100\n郝\thao3\t50\n的\tde1\t100\n好的\thao3 de1\t100\n";
    std::ofstream("rank_test.schema.yaml")
        << "build:\n  dictionaries:\n    rank_test:\n      algebra:\n"
           "        pinyin: ['xform/[0-9]$//']\n"
           "translator:\n  dictionary: rank_test\n  prism: pinyin\n"
           "  user_dict_mode: off\n";
    ASSERT_TRUE(
        DictCompiler::CompileDictionaries(path{"rank_test.schema.yaml"}));
  }

  void TearDown() override {
    for (const char* suffix :
         {".dict.yaml", ".schema.yaml", ".table.bin", ".reverse.bin",
          ".pinyin.prism.bin", ".pack.dict.yaml"})
      std::remove((string("rank_test") + suffix).c_str());
  }

  an<Dictionary> Open() {
    Schema schema("rank_test", new Config);
    schema.config()->SetString("translator/dictionary", "rank_test");
    schema.config()->SetString("translator/prism", "pinyin");
    an<Dictionary> result(
        Dictionary::Require("dictionary")->Create({&schema, "translator"}));
    if (result) {
      EXPECT_TRUE(result->Load());
    }
    return result;
  }

  // 取系统侧迭代器（未耗尽），input 形如 "hao"。
  DictEntryIterator SystemEntries(Dictionary* dictionary, const string& input) {
    SyllableGraph graph;
    Syllabifier syllabifier(" '", false);
    EXPECT_EQ(input.size(), syllabifier.BuildSyllableGraph(
                                input, *dictionary->prism(), &graph));
    auto collector = dictionary->Lookup(graph, 0, nullptr, false);
    if (!collector) {
      return {};
    }
    auto full = collector->find(input.size());
    if (full == collector->end()) {
      return {};
    }
    return full->second;
  }

  set<string> DrainTexts(DictEntryIterator* it) {
    set<string> texts;
    while (!it->exhausted()) {
      texts.insert(it->Peek()->text);
      it->Next();
    }
    return texts;
  }
};

// J1：手排候选压过系统次名（锚点相对"过滤后"的系统侧计算），系统侧
// 同名候选被过滤。词典权重是对数域，断言用实测值锚定而非字面量。
TEST_F(RimeUserRankTest, PinBeatsSystemTopAndFiltersSystem) {
  auto dictionary = Open();
  ASSERT_TRUE(dictionary && dictionary->loaded());
  DictEntryIterator system = SystemEntries(dictionary.get(), "hao");
  ASSERT_EQ(2u, system.entry_count());  // 好 郝

  // 采集系统权重（对数域、降序）作锚点参照。
  DictEntryIterator probe = SystemEntries(dictionary.get(), "hao");
  vector<double> weights;
  while (!probe.exhausted()) {
    weights.push_back(probe.Peek()->weight);
    probe.Next();
  }
  std::sort(weights.begin(), weights.end(), std::greater<double>());
  const double system_top = weights[0];
  const double system_second = weights[1];

  UserDictEntryIterator users;
  users.Add(MakeUser("好", 1.0, 1, 7, SoloCode(1)));

  user_rank::Apply(&system, &users, false);
  EXPECT_EQ((set<string>{"郝"}), DrainTexts(&system));  // 好 被过滤
  users.SortAll();
  ASSERT_FALSE(users.exhausted());
  auto pinned = users.Peek();
  EXPECT_EQ("好", pinned->text);
  EXPECT_GT(pinned->weight, system_second);  // 压过过滤后的系统首位
  EXPECT_LT(pinned->weight, system_top);     // 且在原系统首位之下
}

// J2：多个手排位保持相对次序，锚点插进系统权重缝隙。
TEST_F(RimeUserRankTest, MultiplePinsKeepRelativeOrder) {
  auto dictionary = Open();
  ASSERT_TRUE(dictionary && dictionary->loaded());
  DictEntryIterator system = SystemEntries(dictionary.get(), "hao");
  ASSERT_EQ(2u, system.entry_count());

  UserDictEntryIterator users;
  users.Add(MakeUser("好", 1.0, 1, 7, SoloCode(1)));
  users.Add(MakeUser("郝", 1.0, 2, 7, SoloCode(1)));

  user_rank::Apply(&system, &users, false);
  EXPECT_TRUE(system.exhausted());  // 两个系统候选都被过滤
  users.SortAll();
  ASSERT_FALSE(users.exhausted());
  const auto first = users.Peek();
  EXPECT_EQ("好", first->text);
  users.Next();
  ASSERT_FALSE(users.exhausted());
  const auto second = users.Peek();
  EXPECT_EQ("郝", second->text);
  EXPECT_LT(second->weight, first->weight);  // 位次不倒挂
}

// J3：无手排时，已学习词获得"系统榜首 + 小偏移"的加成（对数域）。
TEST_F(RimeUserRankTest, LearnedEntryBoostsAboveSystemTop) {
  auto dictionary = Open();
  ASSERT_TRUE(dictionary && dictionary->loaded());
  DictEntryIterator system = SystemEntries(dictionary.get(), "hao");

  DictEntryIterator probe = SystemEntries(dictionary.get(), "hao");
  double system_top = probe.Peek()->weight;  // 首位即该码榜首

  UserDictEntryIterator users;
  // 加成是 max(自身, 榜首+偏移)：自身权重必须压到榜首之下，加成才可见。
  users.Add(MakeUser("好U", system_top - 1.0, 0, 0, SoloCode(1), 1));

  user_rank::Apply(&system, &users, false);
  users.SortAll();
  ASSERT_FALSE(users.exhausted());
  auto boosted = users.Peek();
  EXPECT_EQ("好U", boosted->text);
  EXPECT_GT(boosted->weight, system_top);                     // 越过系统榜首
  EXPECT_LT(boosted->weight, system_top + 0.05);              // 且只是小偏移
  EXPECT_EQ((set<string>{"好", "郝"}), DrainTexts(&system));  // 系统不动
}

// J4：同码有手排时，学习词不再自动加成（手排优先让位）。
// 学习词权重压到榜首之下：若无抑制（M9），会被加成到榜首+偏移，断言即红。
TEST_F(RimeUserRankTest, PinnedCodeSuppressesAutoBoost) {
  auto dictionary = Open();
  ASSERT_TRUE(dictionary && dictionary->loaded());
  DictEntryIterator system = SystemEntries(dictionary.get(), "hao");
  DictEntryIterator probe = SystemEntries(dictionary.get(), "hao");
  const double system_top = probe.Peek()->weight;

  UserDictEntryIterator users;
  users.Add(MakeUser("好", 1.0, 1, 7, SoloCode(1)));  // 手排
  users.Add(MakeUser("好U", system_top - 1.0, 0, 0, SoloCode(1),
                     1));  // 学习过，低于榜首

  user_rank::Apply(&system, &users, false);
  users.SortAll();
  ASSERT_FALSE(users.exhausted());
  EXPECT_EQ("好", users.Peek()->text);  // 手排仍第一
  users.Next();
  ASSERT_FALSE(users.exhausted());
  EXPECT_NEAR(users.Peek()->weight, system_top - 1.0,
              1e-9);  // 学习词保持原权重，未被加成
}

// J5：读音隔离——FilterPinnedStatic 在 isolate 模式下按读音分桶：
// 钉在 de 读音的"好"不得牵连 hao 读音的系统侧；正例（hao 自钉）则过滤。
TEST_F(RimeUserRankTest, ReadingsAreIsolated) {
  auto dictionary = Open();
  ASSERT_TRUE(dictionary && dictionary->loaded());

  {
    // 反例：钉在别的读音，hao 系统侧不受影响。
    DictEntryIterator system = SystemEntries(dictionary.get(), "hao");
    UserDictEntryIterator users;
    users.Add(MakeUser("好", 1.0, 1, 7, SoloCode(2)));  // 钉在 de 读音
    user_rank::FilterPinnedStatic(&system, users, true);
    EXPECT_EQ((set<string>{"好", "郝"}), DrainTexts(&system));
  }
  {
    // 正例：钉在本读音，系统侧同名候选被剔除。
    DictEntryIterator system = SystemEntries(dictionary.get(), "hao");
    UserDictEntryIterator users;
    users.Add(MakeUser("好", 1.0, 1, 7, SoloCode(1)));  // 钉在 hao 读音
    user_rank::FilterPinnedStatic(&system, users, true);
    EXPECT_EQ((set<string>{"郝"}), DrainTexts(&system));
  }
}

// J6：PreferUser 平权裁决——位次到位才偏向用户，未到位偏系统。
TEST_F(RimeUserRankTest, PreferUserTieBreaksByObservedPosition) {
  DictEntry user;
  user.text = "好";
  user.weight = 100.0;
  user.manual_position = 2;
  DictEntry system;
  system.text = "郝";
  system.weight = 100.0;
  EXPECT_TRUE(user_rank::PreferUser(user, system, 2));   // 观察位到位
  EXPECT_FALSE(user_rank::PreferUser(user, system, 1));  // 未到位
  user.weight = 101.0;
  EXPECT_TRUE(user_rank::PreferUser(user, system, 1));  // 权重更高直接赢
}

// J7：HasManualPin 只认精确匹配的钉选项。
TEST_F(RimeUserRankTest, HasManualPinRequiresExactMatch) {
  UserDictEntryIterator users;
  users.Add(MakeUser("好", 1.0, 1, 7, SoloCode(1)));
  EXPECT_TRUE(user_rank::HasManualPin(users));

  UserDictEntryIterator predictive;
  auto e = MakeUser("好", 1.0, 1, 7, SoloCode(1));
  e->remaining_code_length = 1;  // 预测匹配不参与手排
  predictive.Add(std::move(e));
  EXPECT_FALSE(user_rank::HasManualPin(predictive));

  UserDictEntryIterator unpinned;
  unpinned.Add(MakeUser("好", 1.0, 0, 0, SoloCode(1)));
  EXPECT_FALSE(user_rank::HasManualPin(unpinned));
}
