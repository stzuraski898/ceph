// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "mgr/DaemonKey.h"

TEST(DaemonKey, Parse)
{
  auto [key, ok] = DaemonKey::parse("osd.1");
  ASSERT_TRUE(ok);
  EXPECT_EQ(key.type, "osd");
  EXPECT_EQ(key.name, "1");
}

TEST(DaemonKey, ParseMonDaemon)
{
  auto [key, ok] = DaemonKey::parse("mon.a");
  ASSERT_TRUE(ok);
  EXPECT_EQ(key.type, "mon");
  EXPECT_EQ(key.name, "a");
}

// The name portion is everything after the FIRST dot; extra dots stay in name.
TEST(DaemonKey, ParseExtraDot)
{
  auto [key, ok] = DaemonKey::parse("osd.1.extra");
  ASSERT_TRUE(ok);
  EXPECT_EQ(key.type, "osd");
  EXPECT_EQ(key.name, "1.extra");
}

// A leading dot gives an empty type and a non-empty name.
TEST(DaemonKey, ParseStartDot)
{
  auto [key, ok] = DaemonKey::parse(".osd");
  ASSERT_TRUE(ok);
  EXPECT_EQ(key.type, "");
  EXPECT_EQ(key.name, "osd");
}

// A trailing dot gives a valid key with an empty name.
TEST(DaemonKey, ParseEndDot)
{
  auto [key, ok] = DaemonKey::parse("osd.");
  ASSERT_TRUE(ok);
  EXPECT_EQ(key.type, "osd");
  EXPECT_EQ(key.name, "");
}

// A bare "." separates empty type from empty name — still a valid parse.
TEST(DaemonKey, ParseDot)
{
  auto [key, ok] = DaemonKey::parse(".");
  ASSERT_TRUE(ok);
  EXPECT_EQ(key.type, "");
  EXPECT_EQ(key.name, "");
}

// Three dots: type = "", name = "..".
TEST(DaemonKey, ParseThreeDots)
{
  auto [key, ok] = DaemonKey::parse("...");
  ASSERT_TRUE(ok);
  EXPECT_EQ(key.type, "");
  EXPECT_EQ(key.name, "..");
}

// No dot at all: parse must fail and return empty fields.
TEST(DaemonKey, ParseWithEmptyString)
{
  auto [key, ok] = DaemonKey::parse("");
  ASSERT_FALSE(ok);
  EXPECT_EQ(key.type, "");
  EXPECT_EQ(key.name, "");
}

TEST(DaemonKey, ParseWithoutDot)
{
  auto [key, ok] = DaemonKey::parse("osd1");
  ASSERT_FALSE(ok);
  EXPECT_EQ(key.type, "");
  EXPECT_EQ(key.name, "");
}

// Spaces are not special; a string with spaces but no dot must fail.
TEST(DaemonKey, ParseSpacesNoDot)
{
  auto [key, ok] = DaemonKey::parse("osd 1");
  ASSERT_FALSE(ok);
  EXPECT_EQ(key.type, "");
  EXPECT_EQ(key.name, "");
}

TEST(DaemonKey, LessThan)
{
  DaemonKey a{"mon", "a"};
  DaemonKey b{"osd", "1"};
  DaemonKey c{"osd", "2"};

  EXPECT_TRUE(a < b);   // mon < osd (type comparison)
  EXPECT_TRUE(b < c);   // osd.1 < osd.2 (name comparison, same type)
  EXPECT_FALSE(c < b);  // not 2 < 1
  EXPECT_FALSE(b < a);  // not osd < mon
}

// A key is never less than itself (reflexive irreflexivity).
TEST(DaemonKey, LessThanIrreflexive)
{
  DaemonKey a{"osd", "1"};
  DaemonKey b{"mon", "a"};
  DaemonKey c{"", ""};

  EXPECT_FALSE(a < a);
  EXPECT_FALSE(b < b);
  EXPECT_FALSE(c < c);
}

// Asymmetry: if a < b then !(b < a).
TEST(DaemonKey, LessThanAsymmetric)
{
  DaemonKey a{"mon", "a"};
  DaemonKey b{"osd", "1"};

  ASSERT_TRUE(a < b);
  EXPECT_FALSE(b < a);
}

// Transitivity: a < b && b < c => a < c.
TEST(DaemonKey, LessThanTransitive)
{
  DaemonKey a{"mds", "0"};
  DaemonKey b{"mgr", "x"};
  DaemonKey c{"osd", "1"};

  ASSERT_TRUE(a < b);
  ASSERT_TRUE(b < c);
  EXPECT_TRUE(a < c);
}

// Two keys with the same type and name are not ordered relative to each other.
TEST(DaemonKey, LessThanEqualKeysNotOrdered)
{
  DaemonKey a{"osd", "1"};
  DaemonKey b{"osd", "1"};

  EXPECT_FALSE(a < b);
  EXPECT_FALSE(b < a);
}

// Type comparison drives ordering when names differ.
TEST(DaemonKey, LessThanTypeDominates)
{
  DaemonKey a{"mon", "z"};  // type mon < osd even though name z > 1
  DaemonKey b{"osd", "1"};

  EXPECT_TRUE(a < b);
  EXPECT_FALSE(b < a);
}

// Same type: ordering falls through to name.
TEST(DaemonKey, LessThanSameTypeDifferentName)
{
  DaemonKey a{"osd", "0"};
  DaemonKey b{"osd", "9"};

  EXPECT_TRUE(a < b);
  EXPECT_FALSE(b < a);
}

TEST(DaemonKey, LessThanEmptyStr)
{
  DaemonKey a{"", ""};
  DaemonKey b{"", ""};
  DaemonKey c{"osd", "1"};

  EXPECT_FALSE(a < b);
  EXPECT_TRUE(a < c);
  EXPECT_FALSE(c < a);
}

TEST(DaemonKey, LessThanEmptyType)
{
  DaemonKey a{"", "1"};
  DaemonKey b{"", "2"};
  DaemonKey c{"osd", "1"};

  EXPECT_TRUE(a < b);
  EXPECT_TRUE(a < c);
}

TEST(DaemonKey, LessThanEmptyName)
{
  DaemonKey a{"mon", ""};
  DaemonKey b{"mon", "a"};
  DaemonKey c{"osd", ""};

  EXPECT_TRUE(a < b);
  EXPECT_TRUE(a < c);
}

TEST(DaemonKey, Ostream)
{
  DaemonKey key{"osd", "1"};
  std::ostringstream oss;
  oss << key;

  EXPECT_EQ(oss.str(), "osd.1");
}

TEST(DaemonKey, OstreamEmpty)
{
  DaemonKey key{"", ""};
  std::ostringstream oss;
  oss << key;

  EXPECT_EQ(oss.str(), ".");
}

TEST(DaemonKey, OstreamEmptyType)
{
  DaemonKey key{"", "mgr0"};
  std::ostringstream oss;
  oss << key;

  EXPECT_EQ(oss.str(), ".mgr0");
}

TEST(DaemonKey, OstreamEmptyName)
{
  DaemonKey key{"mds", ""};
  std::ostringstream oss;
  oss << key;

  EXPECT_EQ(oss.str(), "mds.");
}

// Streaming two keys in sequence produces the correct concatenated output.
TEST(DaemonKey, OstreamChained)
{
  DaemonKey a{"mon", "a"};
  DaemonKey b{"osd", "2"};
  std::ostringstream oss;
  oss << a << " " << b;

  EXPECT_EQ(oss.str(), "mon.a osd.2");
}

TEST(DaemonKey, ToString)
{
  DaemonKey key{"osd", "1"};

  EXPECT_EQ(ceph::to_string(key), "osd.1");
}

TEST(DaemonKey, ToStringEmptyType)
{
  DaemonKey key{"", "1"};

  EXPECT_EQ(ceph::to_string(key), ".1");
}

TEST(DaemonKey, ToStringEmptyName)
{
  DaemonKey key{"osd", ""};

  EXPECT_EQ(ceph::to_string(key), "osd.");
}

TEST(DaemonKey, ToStringEmpty)
{
  DaemonKey key{"", ""};

  EXPECT_EQ(ceph::to_string(key), ".");
}

// to_string and operator<< must always agree.
TEST(DaemonKey, ToStringMatchesOstream)
{
  const std::vector<DaemonKey> keys = {
      {"osd", "0"},
      {"mon", "alpha"},
      {"mds", ""},
      {"", "1"},
      {"", ""},
  };
  for (const auto& key : keys) {
    std::ostringstream oss;
    oss << key;
    EXPECT_EQ(ceph::to_string(key), oss.str())
        << "mismatch for type='" << key.type << "' name='" << key.name << "'";
  }
}

TEST(DaemonKey, DefaultConstructedIsEmpty)
{
  DaemonKey key;

  EXPECT_EQ(key.type, "");
  EXPECT_EQ(key.name, "");
}

TEST(DaemonKey, ParseRoundTrip)
{
  const std::vector<std::string> inputs = {
      "osd.1",
      "mon.a",
      "mds.myfs:0",
      "osd.1.extra",
  };
  for (const auto& s : inputs) {
    auto [key, ok] = DaemonKey::parse(s);
    ASSERT_TRUE(ok) << "failed to parse: " << s;
    const std::string rt = ceph::to_string(key);
    auto [key2, ok2] = DaemonKey::parse(rt);
    ASSERT_TRUE(ok2) << "round-trip parse failed for: " << rt;
    EXPECT_EQ(key2.type, key.type);
    EXPECT_EQ(key2.name, key.name);
  }
}

TEST(DaemonKey, StdMapKey)
{
  std::map<DaemonKey, int> m;

  DaemonKey osd0{"osd", "0"};
  DaemonKey osd1{"osd", "1"};
  DaemonKey mon_a{"mon", "a"};

  m[osd0] = 10;
  m[osd1] = 20;
  m[mon_a] = 30;

  ASSERT_EQ(m.size(), 3u);
  EXPECT_EQ(m.at(osd0), 10);
  EXPECT_EQ(m.at(osd1), 20);
  EXPECT_EQ(m.at(mon_a), 30);

  // Inserting the same key again updates the value, not the size.
  m[osd0] = 99;
  EXPECT_EQ(m.size(), 3u);
  EXPECT_EQ(m.at(osd0), 99);
}

TEST(DaemonKey, StdSetKey)
{
  std::set<DaemonKey> s;

  DaemonKey a{"mds", "0"};
  DaemonKey b{"mgr", "x"};
  DaemonKey c{"osd", "1"};

  s.insert(c);
  s.insert(a);
  s.insert(b);

  ASSERT_EQ(s.size(), 3u);

  // Verify ascending order: mds.0 < mgr.x < osd.1
  auto it = s.begin();
  EXPECT_EQ(it->type, "mds");
  EXPECT_EQ(it->name, "0");
  ++it;
  EXPECT_EQ(it->type, "mgr");
  EXPECT_EQ(it->name, "x");
  ++it;
  EXPECT_EQ(it->type, "osd");
  EXPECT_EQ(it->name, "1");

  // Inserting a duplicate does not grow the set.
  s.insert(a);
  EXPECT_EQ(s.size(), 3u);
}

TEST(DaemonKey, CopyConstruct)
{
  DaemonKey orig{"osd", "7"};
  DaemonKey copy{orig};

  EXPECT_EQ(copy.type, "osd");
  EXPECT_EQ(copy.name, "7");
  // Modifying copy does not affect original.
  copy.type = "mon";
  EXPECT_EQ(orig.type, "osd");
}

TEST(DaemonKey, CopyAssign)
{
  DaemonKey orig{"mds", "myfs:0"};
  DaemonKey assigned;
  assigned = orig;

  EXPECT_EQ(assigned.type, "mds");
  EXPECT_EQ(assigned.name, "myfs:0");
  // Modifying assigned does not affect original.
  assigned.name = "other";
  EXPECT_EQ(orig.name, "myfs:0");
}

// operator< uses string comparison; "9" > "10" lexicographically because
// '9' > '1'.  This is an important semantic difference from numeric ordering.
TEST(DaemonKey, LessThanNumericLookingNamesAreStringOrdered)
{
  DaemonKey osd10{"osd", "10"};
  DaemonKey osd9{"osd", "9"};

  // "10" < "9" lexicographically — '1' < '9'
  EXPECT_TRUE(osd10 < osd9);
  EXPECT_FALSE(osd9 < osd10);
}

// Two-digit numbers compared digit by digit.
TEST(DaemonKey, LessThanMultiDigitStringOrder)
{
  DaemonKey a{"osd", "20"};
  DaemonKey b{"osd", "100"};

  // "100" < "20" because '1' < '2'
  EXPECT_TRUE(b < a);
  EXPECT_FALSE(a < b);
}

// When parse fails it returns a default-constructed DaemonKey.
// to_string on that key must return ".".
TEST(DaemonKey, ToStringOnFailedParseResult)
{
  auto [key, ok] = DaemonKey::parse("nodot");
  ASSERT_FALSE(ok);
  EXPECT_EQ(ceph::to_string(key), ".");
}

// A space before the dot ends up in the type field.
TEST(DaemonKey, ParseSpaceInType)
{
  auto [key, ok] = DaemonKey::parse("osd .1");
  ASSERT_TRUE(ok);
  EXPECT_EQ(key.type, "osd ");
  EXPECT_EQ(key.name, "1");
}

// A space after the dot ends up in the name field.
TEST(DaemonKey, ParseSpaceInName)
{
  auto [key, ok] = DaemonKey::parse("osd. 1");
  ASSERT_TRUE(ok);
  EXPECT_EQ(key.type, "osd");
  EXPECT_EQ(key.name, " 1");
}

TEST(DaemonKey, ParseLongStrings)
{
  const std::string long_type(1024, 'a');
  const std::string long_name(1024, 'b');
  const std::string s = long_type + '.' + long_name;

  auto [key, ok] = DaemonKey::parse(s);
  ASSERT_TRUE(ok);
  EXPECT_EQ(key.type, long_type);
  EXPECT_EQ(key.name, long_name);
  EXPECT_EQ(ceph::to_string(key), s);
}

// Exactly one of (a<b), (b<a), or (a==b via !(a<b) && !(b<a)) holds.
TEST(DaemonKey, LessThanTotality)
{
  const std::vector<DaemonKey> keys = {
      {"", ""},
      {"", "1"},
      {"mds", ""},
      {"mds", "a"},
      {"osd", "0"},
      {"osd", "1"},
  };
  for (size_t i = 0; i < keys.size(); ++i) {
    for (size_t j = 0; j < keys.size(); ++j) {
      bool lt = keys[i] < keys[j];
      bool gt = keys[j] < keys[i];
      if (i == j) {
        EXPECT_FALSE(lt) << "i=" << i;
        EXPECT_FALSE(gt) << "i=" << i;
      } else if (i < j) {
        // Keys are in ascending order in the vector; exactly one of lt/gt.
        EXPECT_TRUE(lt || gt)
            << "keys[" << i << "] and keys[" << j << "] are incomparable";
        EXPECT_FALSE(lt && gt)
            << "keys[" << i << "] and keys[" << j << "] compare both ways";
      }
    }
  }
}

// After std::move the new key holds the original values.
// The moved-from key is left in a valid (unspecified) state; std::string
// guarantees the moved-from string is empty, so we verify that too.
TEST(DaemonKey, MoveConstruct)
{
  DaemonKey src{"osd", "3"};
  DaemonKey dst{std::move(src)};

  EXPECT_EQ(dst.type, "osd");
  EXPECT_EQ(dst.name, "3");
  // std::string move leaves the source empty
  EXPECT_EQ(src.type, "");
  EXPECT_EQ(src.name, "");
}

TEST(DaemonKey, MoveAssign)
{
  DaemonKey src{"mon", "beta"};
  DaemonKey dst;
  dst = std::move(src);

  EXPECT_EQ(dst.type, "mon");
  EXPECT_EQ(dst.name, "beta");
  EXPECT_EQ(src.type, "");
  EXPECT_EQ(src.name, "");
}

// std::string can contain '\0'; find() respects the full string length.
// A string "os\0d.1" (length 7) has a dot at position 4.
// parse treats '\0' as an ordinary character: type = "os\0d", name = "1".
TEST(DaemonKey, ParseEmbeddedNull)
{
  // Build "os\0d.1" explicitly via std::string to avoid C-string termination.
  std::string s;
  s += "os";
  s += '\0';
  s += "d.1";

  auto [key, ok] = DaemonKey::parse(s);
  ASSERT_TRUE(ok);

  std::string expected_type;
  expected_type += "os";
  expected_type += '\0';
  expected_type += "d";
  EXPECT_EQ(key.type, expected_type);
  EXPECT_EQ(key.name, "1");
}

// A whitespace-only string with no dot must fail.
TEST(DaemonKey, ParseWhitespaceOnlyNoDot)
{
  auto [key, ok] = DaemonKey::parse("   ");
  ASSERT_FALSE(ok);
  EXPECT_EQ(key.type, "");
  EXPECT_EQ(key.name, "");
}

// A single character that is not a dot must fail.
TEST(DaemonKey, ParseSingleCharNoDot)
{
  auto [key, ok] = DaemonKey::parse("x");
  ASSERT_FALSE(ok);
  EXPECT_EQ(key.type, "");
  EXPECT_EQ(key.name, "");
}

// A tab character with no dot must fail.
TEST(DaemonKey, ParseTabNoDot)
{
  auto [key, ok] = DaemonKey::parse("\t");
  ASSERT_FALSE(ok);
  EXPECT_EQ(key.type, "");
  EXPECT_EQ(key.name, "");
}

// A newline with no dot must fail.
TEST(DaemonKey, ParseNewlineNoDot)
{
  auto [key, ok] = DaemonKey::parse("\n");
  ASSERT_FALSE(ok);
  EXPECT_EQ(key.type, "");
  EXPECT_EQ(key.name, "");
}

// A tab before the dot ends up in the type field.
TEST(DaemonKey, ParseTabWithDot)
{
  auto [key, ok] = DaemonKey::parse("osd\t.1");
  ASSERT_TRUE(ok);
  EXPECT_EQ(key.type, "osd\t");
  EXPECT_EQ(key.name, "1");
}

// A string of four dots: type = "", name = "...".
TEST(DaemonKey, ParseAllDots)
{
  auto [key, ok] = DaemonKey::parse("....");
  ASSERT_TRUE(ok);
  EXPECT_EQ(key.type, "");
  EXPECT_EQ(key.name, "...");
}

// Whitespace on both sides of a single dot yields whitespace in both fields.
TEST(DaemonKey, ParseWhitespaceAroundDot)
{
  auto [key, ok] = DaemonKey::parse("  .  ");
  ASSERT_TRUE(ok);
  EXPECT_EQ(key.type, "  ");
  EXPECT_EQ(key.name, "  ");
}

// Every failure case must produce an empty-field key.
TEST(DaemonKey, ParseFailureAlwaysEmptyFields)
{
  const std::vector<std::string> bad = {
      "",
      "osd1",
      "osd 1",
      "   ",
      "x",
      "\t",
      "\n",
  };
  for (const auto& s : bad) {
    auto [key, ok] = DaemonKey::parse(s);
    EXPECT_FALSE(ok)  << "expected false for: '" << s << "'";
    EXPECT_EQ(key.type, "") << "type should be empty for: '" << s << "'";
    EXPECT_EQ(key.name, "") << "name should be empty for: '" << s << "'";
  }
}

// "" < "a" lexicographically, so {"","z"} < {"a",""}.
TEST(DaemonKey, LessThanEmptyTypeVsNonEmptyTypeEmptyName)
{
  DaemonKey a{"", "z"};
  DaemonKey b{"a", ""};

  EXPECT_TRUE(a < b);
  EXPECT_FALSE(b < a);
}

TEST(DaemonKey, MutateFieldsThenCompare)
{
  DaemonKey a{"mon", "a"};
  DaemonKey b{"osd", "1"};

  // Initially mon < osd
  ASSERT_TRUE(a < b);

  // Mutate a so its type becomes "zz" — now a > b
  a.type = "zz";
  EXPECT_FALSE(a < b);
  EXPECT_TRUE(b < a);
}

TEST(DaemonKey, MutateFieldsThenToString)
{
  DaemonKey key{"osd", "1"};
  EXPECT_EQ(ceph::to_string(key), "osd.1");

  key.type = "mon";
  key.name = "alpha";
  EXPECT_EQ(ceph::to_string(key), "mon.alpha");
}

TEST(DaemonKey, MutateFieldsThenOstream)
{
  DaemonKey key{"mds", "0"};
  {
    std::ostringstream oss;
    oss << key;
    EXPECT_EQ(oss.str(), "mds.0");
  }

  key.type = "mgr";
  key.name = "standby";
  {
    std::ostringstream oss;
    oss << key;
    EXPECT_EQ(oss.str(), "mgr.standby");
  }
}

TEST(DaemonKey, SelfAssign)
{
  DaemonKey key{"osd", "5"};
  // Suppress the self-assignment warning intentionally.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-assign"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
#endif
  key = key;  // NOLINT
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

  EXPECT_EQ(key.type, "osd");
  EXPECT_EQ(key.name, "5");
}

TEST(DaemonKey, SetInsertEraseReinsert)
{
  std::set<DaemonKey> s;
  DaemonKey key{"osd", "0"};

  EXPECT_EQ(s.size(), 0u);

  s.insert(key);
  EXPECT_EQ(s.size(), 1u);
  EXPECT_EQ(s.count(key), 1u);

  s.erase(key);
  EXPECT_EQ(s.size(), 0u);
  EXPECT_EQ(s.count(key), 0u);

  s.insert(key);
  EXPECT_EQ(s.size(), 1u);
  EXPECT_EQ(s.count(key), 1u);
}

TEST(DaemonKey, SortVector)
{
  std::vector<DaemonKey> v = {
      {"osd", "2"},
      {"mds", "0"},
      {"osd", "1"},
      {"mon", "b"},
      {"mon", "a"},
  };

  std::sort(v.begin(), v.end());

  // Expected order: mds.0, mon.a, mon.b, osd.1, osd.2
  ASSERT_EQ(v.size(), 5u);
  EXPECT_EQ(v[0].type, "mds"); EXPECT_EQ(v[0].name, "0");
  EXPECT_EQ(v[1].type, "mon"); EXPECT_EQ(v[1].name, "a");
  EXPECT_EQ(v[2].type, "mon"); EXPECT_EQ(v[2].name, "b");
  EXPECT_EQ(v[3].type, "osd"); EXPECT_EQ(v[3].name, "1");
  EXPECT_EQ(v[4].type, "osd"); EXPECT_EQ(v[4].name, "2");
}

// "\r\n" has no dot, so it must fail.
TEST(DaemonKey, ParseCRLFNoDot)
{
  auto [key, ok] = DaemonKey::parse("\r\n");
  ASSERT_FALSE(ok);
  EXPECT_EQ(key.type, "");
  EXPECT_EQ(key.name, "");
}

// A carriage return before the dot becomes part of the type field.
TEST(DaemonKey, ParseCRInType)
{
  auto [key, ok] = DaemonKey::parse("osd\r.1");
  ASSERT_TRUE(ok);
  EXPECT_EQ(key.type, "osd\r");
  EXPECT_EQ(key.name, "1");
}

// The shortest possible valid key: one-char type, dot, one-char name.
TEST(DaemonKey, ParseSingleCharTypeAndName)
{
  auto [key, ok] = DaemonKey::parse("a.b");
  ASSERT_TRUE(ok);
  EXPECT_EQ(key.type, "a");
  EXPECT_EQ(key.name, "b");
}

TEST(DaemonKey, LowerBound)
{
  std::vector<DaemonKey> v = {
      {"mds", "0"},
      {"mon", "a"},
      {"mon", "b"},
      {"osd", "1"},
      {"osd", "2"},
  };

  DaemonKey target{"mon", "a"};
  auto it = std::lower_bound(v.begin(), v.end(), target);
  ASSERT_NE(it, v.end());
  EXPECT_EQ(it->type, "mon");
  EXPECT_EQ(it->name, "a");
}

TEST(DaemonKey, EqualRange)
{
  std::vector<DaemonKey> v = {
      {"mds", "0"},
      {"mon", "a"},
      {"mon", "a"},  // duplicate
      {"osd", "1"},
  };

  DaemonKey target{"mon", "a"};
  auto [first, last] = std::equal_range(v.begin(), v.end(), target);
  // Both copies of {"mon","a"} must be in the range.
  EXPECT_EQ(std::distance(first, last), 2);
  for (auto it = first; it != last; ++it) {
    EXPECT_EQ(it->type, "mon");
    EXPECT_EQ(it->name, "a");
  }
}

// lower_bound for a key not present returns the first key greater than target.
TEST(DaemonKey, LowerBoundMiss)
{
  std::vector<DaemonKey> v = {
      {"mds", "0"},
      {"osd", "1"},
  };

  DaemonKey target{"mon", "a"};  // between mds and osd
  auto it = std::lower_bound(v.begin(), v.end(), target);
  ASSERT_NE(it, v.end());
  EXPECT_EQ(it->type, "osd");
}

// A std::string of length 1 containing only '\0' has no dot.
TEST(DaemonKey, ParseSingleNullByte)
{
  std::string s("\0", 1);  // length=1, value=0x00, not '.'
  auto [key, ok] = DaemonKey::parse(s);
  ASSERT_FALSE(ok);
  EXPECT_EQ(key.type, "");
  EXPECT_EQ(key.name, "");
}

// 'O' (0x4F) < 'o' (0x6F), so "OSD" < "osd".
// A key with an uppercase type sorts before the same type in lowercase.
TEST(DaemonKey, LessThanCaseSensitiveType)
{
  DaemonKey upper{"OSD", "1"};
  DaemonKey lower{"osd", "1"};

  EXPECT_TRUE(upper < lower);  // 'O' < 'o'
  EXPECT_FALSE(lower < upper);
}

// Same type, but uppercase name vs lowercase name.
TEST(DaemonKey, LessThanCaseSensitiveName)
{
  DaemonKey a{"osd", "A"};  // 'A' (0x41) < 'a' (0x61)
  DaemonKey b{"osd", "a"};

  EXPECT_TRUE(a < b);
  EXPECT_FALSE(b < a);
}

// When name itself contains a dot, to_string must preserve it verbatim.
// The result is type + '.' + name, so "osd" + "." + "1.0" = "osd.1.0".
TEST(DaemonKey, ToStringNameWithDot)
{
  DaemonKey key{"osd", "1.0"};
  EXPECT_EQ(ceph::to_string(key), "osd.1.0");
}

// Verify that parsing the to_string result picks up the FIRST dot,
// giving type="osd" and name="1.0" (round-trip).
TEST(DaemonKey, ToStringNameWithDotRoundTrip)
{
  DaemonKey key{"osd", "1.0"};
  const std::string s = ceph::to_string(key);
  ASSERT_EQ(s, "osd.1.0");

  auto [parsed, ok] = DaemonKey::parse(s);
  ASSERT_TRUE(ok);
  EXPECT_EQ(parsed.type, "osd");
  EXPECT_EQ(parsed.name, "1.0");
}

TEST(DaemonKey, SwapKeys)
{
  DaemonKey a{"mon", "alpha"};
  DaemonKey b{"osd", "7"};

  std::swap(a, b);

  EXPECT_EQ(a.type, "osd");
  EXPECT_EQ(a.name, "7");
  EXPECT_EQ(b.type, "mon");
  EXPECT_EQ(b.name, "alpha");
}

// Swapping a key with itself must leave fields unchanged.
TEST(DaemonKey, SwapSelf)
{
  DaemonKey key{"mds", "myfs:0"};
  std::swap(key, key);  // NOLINT(bugprone-use-after-move)
  EXPECT_EQ(key.type, "mds");
  EXPECT_EQ(key.name, "myfs:0");
}

TEST(DaemonKey, OstreamReturnRef)
{
  DaemonKey key{"osd", "1"};
  std::ostringstream oss;
  std::ostream& ret = (oss << key);
  // The returned reference must be the same stream object.
  EXPECT_EQ(&ret, &oss);
  EXPECT_EQ(oss.str(), "osd.1");
}

TEST(DaemonKey, OstreamAppendsToExistingContent)
{
  DaemonKey key{"mgr", "active"};
  std::ostringstream oss;
  oss << "prefix:";
  oss << key;
  EXPECT_EQ(oss.str(), "prefix:mgr.active");
}

// operator<< on a std::stringstream (read+write), not just ostringstream.
TEST(DaemonKey, OstreamStringstream)
{
  DaemonKey key{"mon", "b"};
  std::stringstream ss;
  ss << "leader=";
  ss << key;
  EXPECT_EQ(ss.str(), "leader=mon.b");
}

TEST(DaemonKey, ToStringRepeatedCalls)
{
  DaemonKey key{"osd", "42"};
  const std::string first  = ceph::to_string(key);
  const std::string second = ceph::to_string(key);
  EXPECT_EQ(first, "osd.42");
  EXPECT_EQ(second, "osd.42");
  EXPECT_EQ(first, second);
}

// 0x2E (ASCII '.') cannot appear inside a UTF-8 continuation byte (0x80–0xBF)
// or multi-byte lead byte (0xC0–0xFF), so find('.') locates the correct
// separator even when type/name contain multi-byte UTF-8 sequences.
TEST(DaemonKey, ParseUtf8InType)
{
  // "mér" encoded as UTF-8: 'm' 0xC3 0xA9 'r' — no 0x2E in the sequence.
  const std::string s = "m\xc3\xa9r.1";
  auto [key, ok] = DaemonKey::parse(s);
  ASSERT_TRUE(ok);
  EXPECT_EQ(key.type, "m\xc3\xa9r");
  EXPECT_EQ(key.name, "1");
}

TEST(DaemonKey, ParseUtf8InName)
{
  // name contains a multi-byte sequence after the dot.
  const std::string s = "osd.r\xc3\xa9plica";
  auto [key, ok] = DaemonKey::parse(s);
  ASSERT_TRUE(ok);
  EXPECT_EQ(key.type, "osd");
  EXPECT_EQ(key.name, "r\xc3\xa9plica");
}

// "mon" is a prefix of "monitor"; "mon" < "monitor" because 'r'[3] > end[3].
TEST(DaemonKey, LessThanTypePrefixOrdering)
{
  DaemonKey a{"mon", "0"};
  DaemonKey b{"monitor", "0"};

  // "mon" < "monitor" (shorter string is less when it is a prefix)
  EXPECT_TRUE(a < b);
  EXPECT_FALSE(b < a);
}

// "mgr" vs "mgr2" — same-prefix but different length names.
TEST(DaemonKey, LessThanNamePrefixOrdering)
{
  DaemonKey a{"osd", "1"};
  DaemonKey b{"osd", "10"};

  // "1" < "10" because at index 1: end-of-string < '0'
  EXPECT_TRUE(a < b);
  EXPECT_FALSE(b < a);
}

// If a DaemonKey is constructed directly with a dot in the type field,
// to_string emits type + '.' + name.  parse of that output will pick the
// FIRST dot, so the round-trip changes type/name — but to_string itself
// must still produce the exact concatenation.
TEST(DaemonKey, ToStringTypeWithDot)
{
  DaemonKey key{"mds.myfs", "0"};
  EXPECT_EQ(ceph::to_string(key), "mds.myfs.0");
}

// Parsing the output of ToStringTypeWithDot gives a different split
// (first dot is the separator), which is the documented parse semantic.
TEST(DaemonKey, ParseSplitsOnFirstDotOnly)
{
  // Direct construction with dot in type, then serialise.
  DaemonKey key{"mds.myfs", "0"};
  const std::string s = ceph::to_string(key);  // "mds.myfs.0"

  auto [parsed, ok] = DaemonKey::parse(s);
  ASSERT_TRUE(ok);
  // First dot is at index 3, so type = "mds", name = "myfs.0".
  EXPECT_EQ(parsed.type, "mds");
  EXPECT_EQ(parsed.name, "myfs.0");
}

TEST(DaemonKey, SwapWithEmpty)
{
  DaemonKey full{"osd", "5"};
  DaemonKey empty;

  std::swap(full, empty);

  // full is now empty; empty holds the original values.
  EXPECT_EQ(full.type, "");
  EXPECT_EQ(full.name, "");
  EXPECT_EQ(empty.type, "osd");
  EXPECT_EQ(empty.name, "5");
}

// Adjacent dots: first dot splits at index 1, name begins with a dot.
// "a..b" => type="a", name=".b"
TEST(DaemonKey, ParseAdjacentDots)
{
  auto [key, ok] = DaemonKey::parse("a..b");
  ASSERT_TRUE(ok);
  EXPECT_EQ(key.type, "a");
  EXPECT_EQ(key.name, ".b");
}

// High-byte (0xFF) in the type field: '.' is 0x2E so find() must not
// confuse the high byte with the separator.
TEST(DaemonKey, ParseHighByteInType)
{
  std::string s;
  s += '\xff';
  s += '\xff';
  s += '.';
  s += "1";
  auto [key, ok] = DaemonKey::parse(s);
  ASSERT_TRUE(ok);
  std::string expected_type;
  expected_type += '\xff';
  expected_type += '\xff';
  EXPECT_EQ(key.type, expected_type);
  EXPECT_EQ(key.name, "1");
}

// operator<< must return the same ostream& reference even when the stream
// has its failbit set.  The returned pointer must equal &os and the function
// must not throw.
TEST(DaemonKey, OstreamFailedStream)
{
  DaemonKey key{"osd", "1"};
  std::ostringstream oss;
  // Force the stream into a failed state by setting failbit explicitly.
  oss.setstate(std::ios::failbit);
  ASSERT_TRUE(oss.fail());

  std::ostream& ret = (oss << key);
  // The returned reference must be the same object.
  EXPECT_EQ(&ret, &oss);
  // Stream remains in failed state; no crash or exception.
  EXPECT_TRUE(oss.fail());
}

// std::map::find() must locate an existing key and return end() for a miss.
TEST(DaemonKey, MapFind)
{
  std::map<DaemonKey, int> m;
  DaemonKey osd0{"osd", "0"};
  DaemonKey osd1{"osd", "1"};
  DaemonKey mon_a{"mon", "a"};

  m[osd0] = 10;
  m[osd1] = 20;
  m[mon_a] = 30;

  // Hit: existing key.
  auto it = m.find(osd1);
  ASSERT_NE(it, m.end());
  EXPECT_EQ(it->first.type, "osd");
  EXPECT_EQ(it->first.name, "1");
  EXPECT_EQ(it->second, 20);

  // Miss: key not in map.
  DaemonKey mgr_x{"mgr", "x"};
  EXPECT_EQ(m.find(mgr_x), m.end());
}

// std::map::lower_bound() must return an iterator to the first key not less
// than the given key, mirroring the std::vector lower_bound test but on
// the map's own tree-based overload.
TEST(DaemonKey, MapLowerBound)
{
  std::map<DaemonKey, int> m;
  m[{"mds", "0"}] = 1;
  m[{"mon", "a"}] = 2;
  m[{"osd", "1"}] = 3;

  // Exact-hit: lower_bound points at "mon.a".
  auto it = m.lower_bound({"mon", "a"});
  ASSERT_NE(it, m.end());
  EXPECT_EQ(it->first.type, "mon");
  EXPECT_EQ(it->first.name, "a");
  EXPECT_EQ(it->second, 2);

  // Miss between mds.0 and mon.a: lower_bound returns "mon.a".
  auto it2 = m.lower_bound({"mgr", "z"});
  ASSERT_NE(it2, m.end());
  EXPECT_EQ(it2->first.type, "mon");
  EXPECT_EQ(it2->first.name, "a");

  // Past the last element: lower_bound returns end().
  EXPECT_EQ(m.lower_bound({"zzz", "0"}), m.end());
}

// std::set::find() must return an iterator to the matching element when
// the key is present, and end() when it is absent.
TEST(DaemonKey, SetFind)
{
  std::set<DaemonKey> s;
  DaemonKey a{"mds", "0"};
  DaemonKey b{"mgr", "x"};
  DaemonKey c{"osd", "1"};

  s.insert(a);
  s.insert(b);
  s.insert(c);

  // Hit: each inserted key is found.
  auto it_a = s.find(a);
  ASSERT_NE(it_a, s.end());
  EXPECT_EQ(it_a->type, "mds");
  EXPECT_EQ(it_a->name, "0");

  auto it_c = s.find(c);
  ASSERT_NE(it_c, s.end());
  EXPECT_EQ(it_c->type, "osd");
  EXPECT_EQ(it_c->name, "1");

  // Miss: key not in set.
  DaemonKey absent{"mon", "z"};
  EXPECT_EQ(s.find(absent), s.end());
}

// parse("..") — first dot at index 0, second dot becomes part of name.
// Expected: ok=true, type="", name="."
TEST(DaemonKey, ParseTwoDots)
{
  auto [key, ok] = DaemonKey::parse("..");
  ASSERT_TRUE(ok);
  EXPECT_EQ(key.type, "");
  EXPECT_EQ(key.name, ".");
}

// to_string on a key whose type contains an embedded null byte.
// The result must equal type + '.' + name even when type has '\0' in it.
TEST(DaemonKey, ToStringEmbeddedNullInType)
{
  std::string t;
  t += "os";
  t += '\0';
  t += "d";
  DaemonKey key{t, "1"};

  std::string expected = t + '.' + "1";
  EXPECT_EQ(ceph::to_string(key), expected);
  EXPECT_EQ(ceph::to_string(key).size(), expected.size());
}

// std::string "\0.1" (3 bytes): find('.') returns 1.
// Expected: ok=true, type="\0", name="1".
TEST(DaemonKey, ParseNullAtStart)
{
  std::string s;
  s += '\0';
  s += '.';
  s += '1';

  auto [key, ok] = DaemonKey::parse(s);
  ASSERT_TRUE(ok);

  std::string expected_type;
  expected_type += '\0';
  EXPECT_EQ(key.type, expected_type);
  EXPECT_EQ(key.name, "1");
}

TEST(DaemonKey, ParseSingleCharTypeTrailingDot)
{
  auto [key, ok] = DaemonKey::parse("a.");
  ASSERT_TRUE(ok);
  EXPECT_EQ(key.type, "a");
  EXPECT_EQ(key.name, "");
}

TEST(DaemonKey, MapEraseAndFind)
{
  std::map<DaemonKey, int> m;
  DaemonKey osd0{"osd", "0"};
  DaemonKey osd1{"osd", "1"};

  m[osd0] = 10;
  m[osd1] = 20;
  ASSERT_EQ(m.size(), 2u);

  // Erase osd0 and verify it's gone.
  m.erase(osd0);
  EXPECT_EQ(m.size(), 1u);
  EXPECT_EQ(m.find(osd0), m.end());

  // osd1 is still present.
  auto it = m.find(osd1);
  ASSERT_NE(it, m.end());
  EXPECT_EQ(it->second, 20);

  // Erase a key that was never inserted — must not crash or alter size.
  DaemonKey absent{"mon", "z"};
  m.erase(absent);
  EXPECT_EQ(m.size(), 1u);
}
