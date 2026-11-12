// Copyright RIME Developers
// Distributed under the BSD License
//
// MemoryRegistry 单一写者 + 学习会话生命周期（登记见
// doc/memory-registry-failmodes.md MR1-MR6）。
// 关键前提：UserDictionaryComponent 的 db_pool_ 按名共享同一 Db 实例——
// 两个 namespace 的 user_dict 同名即共享 storage，registry 分组生效。
// 全局组件 resolver 根钉死 CWD（见 user_dict_manager_test 夹具说明）。
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <rime/config.h>
#include <rime/deployer.h>
#include <rime/dict/db.h>
#include <rime/dict/dict_compiler.h>
#include <rime/dict/user_db.h>
#include <rime/dict/user_dictionary.h>
#include <rime/engine.h>
#include <rime/schema.h>
#include <rime/service.h>
// memory.h 的签名用了 Ticket 但不自含其声明，必须先于它包含。
#include <rime/ticket.h>
#include <rime/gear/memory.h>

using namespace rime;

namespace {

// Memory 是抽象类（Memorize 纯虚）；测试派生只记录调用。
class TestMemory : public Memory {
 public:
  explicit TestMemory(const Ticket& ticket) : Memory(ticket) {}
  bool Memorize(const CommitEntry& entry) override {
    memorized.push_back(entry.text);
    return true;
  }
  vector<string> memorized;
};

}  // namespace

class RimeMemoryRegistryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::ofstream("mr_dict.dict.yaml")
        << "---\nname: mr_dict\nversion: '1.0'\nsort: by_weight\n...\n"
        << "好\thao3\t100\n";
    std::ofstream("mr_test.schema.yaml")
        << "schema:\n  schema_id: mr_test\n"
           "translator:\n  dictionary: mr_dict\n  user_dict: mr_dict\n"
           "  user_dict_mode: auto\n"
           "translator_b:\n  dictionary: mr_dict\n  user_dict: mr_dict\n"
           "  user_dict_mode: auto\n"
           "translator_m:\n  dictionary: mr_dict\n  user_dict: mr_manual\n"
           "  user_dict_mode: manual\n"
           "translator_o:\n  dictionary: mr_dict\n  user_dict: mr_off\n"
           "  user_dict_mode: off\n";
    ASSERT_TRUE(DictCompiler::CompileDictionaries(path{"mr_test.schema.yaml"}));

    the<Config> config(new Config);
    ASSERT_TRUE(config->LoadFromFile(path{"mr_test.schema.yaml"}));
    engine_.reset(Engine::Create());
    ASSERT_TRUE(engine_);
    engine_->ApplySchema(new Schema("mr_test", config.release()));

    component_ = UserDb::Require("userdb");
    ASSERT_TRUE(component_);
  }

  void TearDown() override {
    engine_.reset();
    for (const char* name : {"mr_dict", "mr_manual"}) {
      the<Db> db(component_->Create(name));
      if (db && db->Exists()) {
        db->Remove();
      }
    }
    for (const char* f :
         {"mr_dict.dict.yaml", "mr_test.schema.yaml", "mr_dict.table.bin",
          "mr_dict.reverse.bin", "mr_dict.pinyin.prism.bin"}) {
      std::remove(f);
    }
  }

  // 取 Memory 持有用户库的池内句柄（storage_identity 即 db_pool_ 里那个，
  // 已被 UserDictionary::Load 打开）。UserDb::Require 直接 Create 会得到
  // 同文件的另一个未加载句柄，不是同一实例。
  // 返回裸指针：storage_identity 是 .get() 出来的裸指针，包成 an<Db> 会
  // 造出第二个 owning shared_ptr（独立控制块）→ 双重释放（实测 SIGSEGV）。
  Db* SessionDb(Memory& memory) {
    return const_cast<Db*>(memory.user_dict()->storage_identity());
  }

  Engine* engine() { return engine_.get(); }

  the<Engine> engine_;
  UserDb::Component* component_ = nullptr;
};

// MR1：同一用户库上只允许一个写者；非写者开不了会话。
TEST_F(RimeMemoryRegistryTest, SoleWriterPerStorage) {
  TestMemory first(Ticket(engine(), "translator"));
  TestMemory second(Ticket(engine(), "translator_b"));
  ASSERT_TRUE(first.user_dict() && first.user_dict()->loaded());
  ASSERT_TRUE(second.user_dict() && second.user_dict()->loaded());
  EXPECT_TRUE(first.IsUserDbWriter());
  EXPECT_FALSE(second.IsUserDbWriter());
  EXPECT_TRUE(first.StartSession());
  EXPECT_FALSE(second.StartSession());
  EXPECT_TRUE(first.FinishSession());
}

// MR2：写者析构后身份转移给幸存者。
// second 必须在 first 存活期间创建（经 db_pool_ 共享同一 storage）——
// 转移契约的本意就是"同一库上转移"；若先后创建，pool 会重建新库，
// 测的是另一条路径（且转移残条目会被地址复用掩盖）。
TEST_F(RimeMemoryRegistryTest, WriterOwnershipTransfersOnDestruction) {
  auto first = std::make_unique<TestMemory>(Ticket(engine(), "translator"));
  TestMemory second(Ticket(engine(), "translator_b"));
  EXPECT_TRUE(first->IsUserDbWriter());
  EXPECT_FALSE(second.IsUserDbWriter());
  first.reset();  // writer 析构 → 转移
  EXPECT_TRUE(second.IsUserDbWriter());
  EXPECT_TRUE(second.StartSession());
  EXPECT_TRUE(second.FinishSession());
}

// MR3：会话提交契约——Finish = CommitPendingTransaction 成功；
// 提交后重启会话，写入的行经句柄仍可读（LevelDb 持久化本身由
// level_db_cache_test 覆盖；Backup/重开句柄在池化句柄上会崩，见
// 登记表排除项）。
TEST_F(RimeMemoryRegistryTest, SessionCommitPersistsWrites) {
  std::cout << "[s0] body-enter" << std::endl;
  TestMemory writer(Ticket(engine(), "translator"));
  ASSERT_TRUE(writer.IsUserDbWriter());
  Db* db = SessionDb(writer);
  ASSERT_TRUE(db && db->loaded());

  EXPECT_TRUE(writer.StartSession());
  ASSERT_TRUE(db->Update("ni \t你", "3,1,10,0,0"));
  EXPECT_TRUE(writer.FinishSession());

  // 新会话可复用，且提交过的行仍在。
  EXPECT_TRUE(writer.StartSession());
  string after;
  ASSERT_TRUE(db->Fetch("ni \t你", &after));
  EXPECT_EQ("3,1,10,0,0", after);
  EXPECT_TRUE(writer.FinishSession());
}

// MR4：弃稿回滚——Discard 走 RevertRecentTransaction，撤销的行
// 在后续会话中不得再出现（若仍可见即为真 bug）。
TEST_F(RimeMemoryRegistryTest, DiscardSessionRollsBack) {
  TestMemory writer(Ticket(engine(), "translator"));
  ASSERT_TRUE(writer.IsUserDbWriter());
  Db* db = SessionDb(writer);
  ASSERT_TRUE(db && db->loaded());

  EXPECT_TRUE(writer.StartSession());
  ASSERT_TRUE(db->Update("er \t贰", "1,1,1,0,0"));
  EXPECT_TRUE(writer.DiscardSession());

  EXPECT_TRUE(writer.StartSession());
  string value;
  EXPECT_FALSE(db->Fetch("er \t贰", &value));
  EXPECT_TRUE(writer.FinishSession());
}

// MR5：manual 模式手排会话通路正常（自动学习门控已由 user_db_test 覆盖）。
TEST_F(RimeMemoryRegistryTest, ManualModeKeepsManualSessionPath) {
  TestMemory writer(Ticket(engine(), "translator_m"));
  ASSERT_TRUE(writer.user_dict() && writer.user_dict()->loaded());
  EXPECT_TRUE(writer.IsUserDbWriter());
  EXPECT_TRUE(writer.StartSession());
  Db* db = SessionDb(writer);
  ASSERT_TRUE(db && db->loaded());
  ASSERT_TRUE(db->Update("pin \t拼", "2,1,2,0,0"));
  EXPECT_TRUE(writer.FinishSession());
  string value;
  ASSERT_TRUE(db->Fetch("pin \t拼", &value));
  EXPECT_EQ("2,1,2,0,0", value);
}

// MR6：off 模式不建学习层——Memory 无 user_dict，写路径全部 no-op 不崩。
TEST_F(RimeMemoryRegistryTest, OffModeCreatesNoLearningLayer) {
  TestMemory off(Ticket(engine(), "translator_o"));
  EXPECT_EQ(nullptr, off.user_dict());
  EXPECT_FALSE(off.IsUserDbWriter());
  EXPECT_FALSE(off.StartSession());
  EXPECT_TRUE(off.FinishSession());  // 无会话时 Finish 恒 true
}
