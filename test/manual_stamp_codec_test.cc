// Copyright RIME Developers
// Distributed under the BSD License
#include <gtest/gtest.h>
#include <rime/dict/manual_stamp_codec.h>

using namespace rime;

namespace {
using manual_stamp_codec::Format;
using manual_stamp_codec::FromUnixSeconds;
using manual_stamp_codec::kEpochUnixSeconds;
using manual_stamp_codec::Parse;
}  // namespace

// 失效模式登记：doc/manual-order-failmodes.md C1。
TEST(RimeManualStampCodecTest, RoundtripAcrossMagnitudes) {
  const uint64_t values[] = {0,
                             1,
                             2,
                             35,
                             36,
                             1295,
                             1296,
                             46655,
                             46656,
                             kEpochUnixSeconds,
                             kEpochUnixSeconds + 1,
                             1000000000000ULL,
                             1ULL << 62,
                             ~0ULL};
  for (uint64_t value : values) {
    SCOPED_TRACE(::testing::Message() << "value=" << value);
    const std::string raw = Format(value);
    uint64_t decoded = 0;
    ASSERT_TRUE(Parse(raw, &decoded));
    EXPECT_EQ(value, decoded);
  }
}

// C1 辅：锚定典型编码字形，防止编码规则悄然漂移。
TEST(RimeManualStampCodecTest, CanonicalEncodings) {
  EXPECT_EQ("0", Format(0));
  EXPECT_EQ("1", Format(1));
  EXPECT_EQ("z", Format(35));
  EXPECT_EQ("10", Format(36));
  EXPECT_EQ("zz", Format(1295));
  EXPECT_EQ("100", Format(1296));
  EXPECT_EQ("zzz", Format(46655));
  EXPECT_EQ("1000", Format(46656));
  EXPECT_EQ("cre66i9s", Format(1000000000000ULL));
  EXPECT_EQ("3w5e11264sgsf", Format(~0ULL));
}

// C2：前导零、大写、符号、空白、超长一律拒绝。
TEST(RimeManualStampCodecTest, ParseRejectsMalformed) {
  const char* malformed[] = {
      "",   "01", "00", "0a", "A",  "AB",
      "zA", "+1", "-1", " 1", "1 ", "10000000000000",  // 14 位超长
  };
  for (const char* raw : malformed) {
    SCOPED_TRACE(::testing::Message() << "raw=\"" << raw << "\"");
    uint64_t value = 12345;
    EXPECT_FALSE(Parse(raw, &value));
  }
  uint64_t value = 0;
  EXPECT_TRUE(Parse("0", &value));  // 单个 0 合法，表示未手排
  EXPECT_EQ(0u, value);
}

// C3：36^13-1 超出 uint64，溢出防护必须拒绝而不是回绕。
TEST(RimeManualStampCodecTest, ParseRejectsOverflow) {
  uint64_t value = 0;
  EXPECT_FALSE(Parse("zzzzzzzzzzzzz", &value));   // 36^13-1
  EXPECT_FALSE(Parse("3w5e11264sgsfz", &value));  // 14 位
  EXPECT_TRUE(Parse("3w5e11264sgsf", &value));    // 恰为 uint64 上限
  EXPECT_EQ(~0ULL, value);
}

// C4：纪元前的墙钟一律钳制为 1，纪元当天为 1，次日为 2。
TEST(RimeManualStampCodecTest, FromUnixSecondsClampsBeforeEpoch) {
  EXPECT_EQ(1u, FromUnixSeconds(0));
  EXPECT_EQ(1u, FromUnixSeconds(kEpochUnixSeconds - 1));
  EXPECT_EQ(1u, FromUnixSeconds(kEpochUnixSeconds));
  EXPECT_EQ(2u, FromUnixSeconds(kEpochUnixSeconds + 1));
}
