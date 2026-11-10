// Copyright RIME Developers
// Distributed under the BSD License
//
// UserDictManager 契约：发现、快照、导入导出与合并。登记见
// doc/user-dict-manager-failmodes.md U1-U7。
// 注意：全局 userdb 组件的 resolver 根在注册时钉死为 CWD（rime_test_main 的
// traits），因此本夹具把 deployer.user_data_dir 也指向 CWD，两边对齐；
// 库名一律加 udm_ 前缀做隔离，断言用相对性质防外来残留。
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>
#include <rime/deployer.h>
#include <rime/dict/level_db.h>
#include <rime/dict/user_db.h>
#include <rime/lever/user_dict_manager.h>
#include <rime/service.h>

using namespace rime;

namespace {

bool Contains(const UserDictList& names, const string& name) {
  return std::find(names.begin(), names.end(), name) != names.end();
}

}  // namespace

class RimeUserDictManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    sync_ = path{"udm_sync_test"};
    std::filesystem::remove_all(sync_);
    std::filesystem::create_directories(sync_);

    auto& deployer = Service::instance().deployer();
    saved_user_ = deployer.user_data_dir;
    saved_sync_ = deployer.sync_dir;
    saved_id_ = deployer.user_id;
    deployer.user_data_dir = std::filesystem::current_path();
    deployer.sync_dir = sync_;
    deployer.user_id = "testdevice";

    manager_ = new UserDictManager(&deployer);
    component_ = UserDb::Require("userdb");
    ASSERT_TRUE(component_);
  }

  void TearDown() override {
    delete manager_;
    for (const char* name :
         {"udm_alpha", "udm_beta", "udm_valid", "udm_test", "udm_junk"}) {
      the<Db> db(component_->Create(name));
      if (db && db->Exists()) {
        db->Remove();
      }
    }
    auto& deployer = Service::instance().deployer();
    deployer.user_data_dir = saved_user_;
    deployer.sync_dir = saved_sync_;
    deployer.user_id = saved_id_;
    std::filesystem::remove_all(sync_);
  }

  // 经组件创建一个合法 userdb（含元数据），写入条目与 tick。
  void CreateDb(const string& name,
                const vector<std::pair<string, string>>& entries,
                uint64_t tick,
                const string& user_id = "testdevice") {
    the<Db> db(component_->Create(name));
    ASSERT_TRUE(db);
    if (db->Exists()) {
      ASSERT_TRUE(db->Remove());
    }
    // CreateMetadata 只写键，必须先 Open；/db_type 由特化给 "userdb"。
    ASSERT_TRUE(db->Open());
    ASSERT_TRUE(db->CreateMetadata());
    if (!user_id.empty()) {
      ASSERT_TRUE(db->MetaUpdate("/user_id", user_id));
    }
    ASSERT_TRUE(db->MetaUpdate("/tick", std::to_string(tick)));
    for (const auto& [key, value] : entries) {
      ASSERT_TRUE(db->Update(key, value));
    }
    ASSERT_TRUE(db->Close());
  }

  string Fetch(const string& name, const string& key) {
    the<Db> db(component_->Create(name));
    string value;
    if (!db || !db->OpenReadOnly() || !db->Fetch(key, &value)) {
      return string();
    }
    db->Close();
    return value;
  }

  UserDictList List() {
    UserDictList names;
    manager_->GetUserDictList(&names);
    return names;
  }

  path sync_;
  path saved_user_;
  path saved_sync_;
  string saved_id_;
  UserDictManager* manager_ = nullptr;
  UserDb::Component* component_ = nullptr;
};

// U1：发现合法 userdb 并按名排序（相对断言，防外来残留）。
TEST_F(RimeUserDictManagerTest, DiscoveryListsAndSorts) {
  CreateDb("udm_beta", {{"ni \t你", "1,1,1,0,0"}}, 5);
  CreateDb("udm_alpha", {{"ni \t你", "1,1,1,0,0"}}, 5);
  UserDictList names;
  EXPECT_TRUE(manager_->GetUserDictList(&names));
  ASSERT_TRUE(Contains(names, "udm_alpha"));
  ASSERT_TRUE(Contains(names, "udm_beta"));
  const auto alpha = std::find(names.begin(), names.end(), "udm_alpha");
  const auto beta = std::find(names.begin(), names.end(), "udm_beta");
  EXPECT_LT(alpha, beta);  // 排序契约
}

// U2：发现跳过非法名（点前缀），空库目录触发 incomplete 但不吞合法项。
// 点前缀库用"内容合法的库"构造，让断言只落在点前缀过滤这一层。
TEST_F(RimeUserDictManagerTest, DiscoverySkipsInvalidAndReportsIncomplete) {
  CreateDb("udm_valid", {{"ni \t你", "1,1,1,0,0"}}, 5);
  CreateDb(".udm_hidden", {{"ni \t你", "1,1,1,0,0"}}, 5);
  std::filesystem::create_directories("udm_junk.userdb");
  UserDictList names;
  EXPECT_FALSE(manager_->GetUserDictList(&names));  // 空库 → incomplete
  EXPECT_TRUE(Contains(names, "udm_valid"));
  EXPECT_FALSE(Contains(names, "udm_junk"));     // 空库不列入
  EXPECT_FALSE(Contains(names, ".udm_hidden"));  // 点前缀忽略
  {
    the<Db> db(component_->Create(".udm_hidden"));
    if (db && db->Exists()) {
      db->Remove();
    }
  }
}

// U3：快照写入 sync/<device>/，设备号变化时更新归属。
TEST_F(RimeUserDictManagerTest, BackupWritesSnapshotAndUpdatesOwner) {
  CreateDb("udm_test", {{"ni \t你", "5,1,10,0,0"}}, 10, "otherdevice");
  EXPECT_TRUE(manager_->Backup("udm_test"));
  const path snapshot = sync_ / "testdevice" / "udm_test.userdb.tsv";
  ASSERT_TRUE(std::filesystem::exists(snapshot));
  {
    the<Db> db(component_->Create("udm_test"));
    string user_id;
    ASSERT_TRUE(db && db->OpenReadOnly());
    ASSERT_TRUE(db->MetaFetch("/user_id", &user_id));
    ASSERT_TRUE(db->Close());
    EXPECT_EQ("testdevice", user_id);  // 归属已更新为本机设备
  }
}

// U4：恢复走合并语义——按最大提交数收敛，不回退本机较新的数据。
TEST_F(RimeUserDictManagerTest, RestoreMergesByMaxCommits) {
  CreateDb("udm_test", {{"ni \t你", "5,1,10,0,0"}}, 10);
  const path snapshot = sync_ / "testdevice" / "udm_test.userdb.tsv";
  ASSERT_TRUE(manager_->Backup("udm_test"));

  // 本机数据退化到 1 次，恢复后应回到 5（合并取最大）。
  {
    the<Db> db(component_->Create("udm_test"));
    ASSERT_TRUE(db->Open());
    ASSERT_TRUE(db->Update("ni \t你", "1,1,1,0,0"));
    ASSERT_TRUE(db->Close());
  }
  EXPECT_TRUE(manager_->Restore(snapshot));
  const string merged = Fetch("udm_test", "ni \t你");
  ASSERT_FALSE(merged.empty());
  UserDbState state;
  ASSERT_TRUE(state.Unpack(merged));
  EXPECT_EQ(5, state.commits);
  EXPECT_EQ(10u, state.last_tick);
}

// U5：不属于本地任何库的快照被拒绝。
// 用"另一台设备的真快照"构造：内容合法、命名规范，唯一拦它的是
// 归属检查（名字不在本地库清单）→ 内层文件名匹配 → staging 库名三层。
TEST_F(RimeUserDictManagerTest, RestoreRejectsForeignSnapshot) {
  CreateDb("udm_test", {{"ni \t你", "1,1,1,0,0"}}, 5);
  CreateDb("udm_other", {{"ni \t你", "2,1,2,0,0"}}, 6);
  ASSERT_TRUE(manager_->Backup("udm_other"));
  const path foreign = sync_ / "testdevice" / "udm_other.userdb.tsv";
  // 删掉 udm_other 本体：此刻该快照不归属任何本地库。
  {
    the<Db> db(component_->Create("udm_other"));
    ASSERT_TRUE(db && db->Exists());
    ASSERT_TRUE(db->Remove());
  }
  EXPECT_FALSE(manager_->Restore(foreign));
}

// U6：sync 根之外的快照（路径逃逸）被拒绝。
// 用真快照放到 sync 根外（文件名与库匹配），排除内容/命名因素，
// 让断言只落在 sync 根 containment 这一层守卫上。
TEST_F(RimeUserDictManagerTest, RestoreRejectsTraversal) {
  CreateDb("udm_test", {{"ni \t你", "1,1,1,0,0"}}, 5);
  const path snapshot = sync_ / "testdevice" / "udm_test.userdb.tsv";
  ASSERT_TRUE(manager_->Backup("udm_test"));
  const path outside = path{"udm_test.userdb.tsv"};  // CWD，sync 根之外
  std::filesystem::copy_file(
      snapshot, outside, std::filesystem::copy_options::overwrite_existing);
  EXPECT_FALSE(manager_->Restore(outside));
  std::filesystem::remove(outside);
}

// U7：导入返回合并条数；对不存在的库返回 -1。
TEST_F(RimeUserDictManagerTest, ImportReturnsMergedCount) {
  CreateDb("udm_test", {{"ni \t你", "5,1,10,0,0"}}, 10);
  const path snapshot = sync_ / "testdevice" / "udm_test.userdb.tsv";
  ASSERT_TRUE(manager_->Backup("udm_test"));
  {
    the<Db> db(component_->Create("udm_test"));
    ASSERT_TRUE(db->Open());
    ASSERT_TRUE(db->Update("ni \t你", "1,1,1,0,0"));
    ASSERT_TRUE(db->Close());
  }
  EXPECT_EQ(1, manager_->Import("udm_test", snapshot));
  UserDbState state;
  ASSERT_TRUE(state.Unpack(Fetch("udm_test", "ni \t你")));
  EXPECT_EQ(5, state.commits);
  EXPECT_EQ(-1, manager_->Import("udm_nosuch", snapshot));
}
