// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
//
// Ordering / invariant tests for MgrCap.
//
// These tests replay fixed and randomized sequences of MgrCap operations
// (parse, decode, set_allow_all, copy, is_capable) and assert the
// state-consistency INVARIANTS that must hold regardless of operation order.
// They assert the invariant, not the current behaviour: a test that fails here
// is exposing a real defect.
//
// Covers hypotheses K1–K7 from ORDERING-MODEL.md.

#include <atomic>
#include <cstdint>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "include/buffer.h"
#include "mgr/MgrCap.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const entity_addr_t kNoAddr{};
static const EntityName kNoName{};

// A small fixed corpus of cap strings used across all tests.
struct Corpus {
  // valid strings
  static constexpr const char* kAllowAll   = "allow *";
  static constexpr const char* kAllowR     = "allow r";
  static constexpr const char* kAllowRW    = "allow rw";
  static constexpr const char* kReadOnly   = "profile read-only";
  static constexpr const char* kReadWrite  = "profile read-write";
  static constexpr const char* kCrash      = "profile crash";
  static constexpr const char* kOsd        = "profile osd";
  static constexpr const char* kRbd        = "profile rbd";
  static constexpr const char* kRbdPool    = "profile rbd pool=mypool";
  static constexpr const char* kRbdRO      = "profile rbd-read-only";
  static constexpr const char* kModule     = "allow module rbd_support rw";
  static constexpr const char* kCommand    = "allow command \"crash post\"";
  static constexpr const char* kService    = "allow service mds r";
  static constexpr const char* kMulti      = "allow r, allow module rbd_support rw";
  // invalid strings
  static constexpr const char* kProfileUnknown  = "profile unknown";
  static constexpr const char* kProfileBadKey   = "profile rbd bad-key=val";
  static constexpr const char* kGrammarBad      = "not a valid cap string";
  static constexpr const char* kEmpty           = "";

  static const std::vector<std::string>& valid() {
    static std::vector<std::string> v = {
      kAllowAll, kAllowR, kAllowRW, kReadOnly, kReadWrite,
      kCrash, kOsd, kRbd, kRbdPool, kRbdRO,
      kModule, kCommand, kService, kMulti,
    };
    return v;
  }
  static const std::vector<std::string>& invalid() {
    static std::vector<std::string> v = {
      kProfileUnknown, kProfileBadKey, kGrammarBad, kEmpty,
    };
    return v;
  }
  static const std::vector<std::string>& all() {
    static std::vector<std::string> v = [](){
      auto& vv = valid();
      auto& iv = invalid();
      std::vector<std::string> out(vv.begin(), vv.end());
      out.insert(out.end(), iv.begin(), iv.end());
      return out;
    }();
    return v;
  }
};

// The fixed request set used to compare is_capable decisions.
struct Request {
  std::string service;
  std::string module;
  std::string command;
  std::map<std::string,std::string> args;
  bool may_read;
  bool may_write;
  bool may_exec;
};
static const std::vector<Request> kRequests = {
  {"", "",            "",           {}, true,  false, false},
  {"", "",            "",           {}, false, true,  false},
  {"", "",            "",           {}, true,  true,  false},
  {"", "",            "",           {}, false, false, true },
  {"", "rbd_support", "",           {}, true,  false, false},
  {"", "rbd_support", "",           {}, true,  true,  false},
  {"", "rbd_support", "",           {{"pool","mypool"}}, true, true, false},
  {"", "",            "crash post", {}, false, false, true },
  {"mds", "",         "",           {}, true,  false, false},
};

static bool capable(const MgrCap& cap, const Request& req) {
  return cap.is_capable(nullptr, kNoName,
                        req.service, req.module, req.command, req.args,
                        req.may_read, req.may_write, req.may_exec,
                        kNoAddr);
}

// Collect the full vector of is_capable decisions for the fixed request set.
static std::vector<bool> decisions(const MgrCap& cap) {
  std::vector<bool> out;
  out.reserve(kRequests.size());
  for (const auto& req : kRequests) {
    out.push_back(capable(cap, req));
  }
  return out;
}

static std::string format_decisions(const std::vector<bool>& d) {
  std::string s = "[";
  for (bool b : d) s += (b ? "1" : "0");
  s += "]";
  return s;
}

static ceph::buffer::list encode_cap(const MgrCap& cap) {
  ceph::buffer::list bl;
  cap.encode(bl);
  return bl;
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class MgrCapOrderingTest : public ::testing::Test {
protected:
  MgrCap cap;
};

// ---------------------------------------------------------------------------
// K1: parse twice — valid then valid
// INVARIANT (INV-G): grants equal exactly the grants of s2, text == s2, no residue from s1.
// ---------------------------------------------------------------------------

TEST_F(MgrCapOrderingTest, OrderingBug_K1_ParseTwice_ValidThenValid)
{
  ASSERT_TRUE(cap.parse(Corpus::kAllowAll))
      << "Sequence: parse('" << Corpus::kAllowAll << "')"
      << "\n  Expected: parse succeeds";

  auto dec1 = decisions(cap);
  const std::string text1 = cap.get_str();

  ASSERT_TRUE(cap.parse(Corpus::kAllowR))
      << "Sequence: parse('" << Corpus::kAllowAll << "') -> parse('" << Corpus::kAllowR << "')"
      << "\n  Expected: second parse succeeds";

  const std::string text2 = cap.get_str();
  auto dec2 = decisions(cap);

  MgrCap fresh;
  ASSERT_TRUE(fresh.parse(Corpus::kAllowR));
  auto dec_fresh = decisions(fresh);

  EXPECT_EQ(text2, std::string(Corpus::kAllowR))
      << "Sequence: parse('" << Corpus::kAllowAll << "') -> parse('" << Corpus::kAllowR << "')"
      << "\n  Current: text == '" << text2 << "'"
      << "\n  Expected: text == '" << Corpus::kAllowR << "'";

  EXPECT_EQ(dec2, dec_fresh)
      << "Sequence: parse('" << Corpus::kAllowAll << "') -> parse('" << Corpus::kAllowR << "')"
      << "\n  Current decisions: " << format_decisions(dec2)
      << "\n  Expected (fresh parse): " << format_decisions(dec_fresh)
      << "\n  Expected: second parse completely replaces first parse's grants";
}

TEST_F(MgrCapOrderingTest, OrderingBug_K1_ParseTwice_DifferentProfiles)
{
  ASSERT_TRUE(cap.parse(Corpus::kRbd))
      << "Sequence: parse('" << Corpus::kRbd << "')"
      << "\n  Expected: parse succeeds";

  ASSERT_TRUE(cap.parse(Corpus::kReadOnly))
      << "Sequence: parse('" << Corpus::kRbd << "') -> parse('" << Corpus::kReadOnly << "')"
      << "\n  Expected: second parse succeeds";

  MgrCap fresh;
  ASSERT_TRUE(fresh.parse(Corpus::kReadOnly));
  auto dec_fresh = decisions(fresh);
  auto dec2 = decisions(cap);

  EXPECT_EQ(cap.get_str(), std::string(Corpus::kReadOnly))
      << "Sequence: parse(rbd) -> parse(read-only)"
      << "\n  Current: text == '" << cap.get_str() << "'"
      << "\n  Expected: text == '" << Corpus::kReadOnly << "'";

  EXPECT_EQ(dec2, dec_fresh)
      << "Sequence: parse(rbd) -> parse(read-only)"
      << "\n  Current decisions: " << format_decisions(dec2)
      << "\n  Expected: " << format_decisions(dec_fresh)
      << "\n  Expected: second parse replaces profile_grants from first parse";
}

// ---------------------------------------------------------------------------
// K2a: grammar failure after successful parse
// INVARIANT (INV-B): grants must be empty after grammar failure.
// ---------------------------------------------------------------------------

TEST_F(MgrCapOrderingTest, OrderingBug_K2a_GrammarFailAfterSuccess)
{
  ASSERT_TRUE(cap.parse(Corpus::kAllowAll))
      << "Sequence: parse('" << Corpus::kAllowAll << "')"
      << "\n  Expected: succeeds";

  bool r = cap.parse(Corpus::kGrammarBad);
  EXPECT_FALSE(r)
      << "Sequence: parse('" << Corpus::kAllowAll << "') -> parse('" << Corpus::kGrammarBad << "')"
      << "\n  Expected: second parse fails";

  EXPECT_TRUE(cap.grants.empty())
      << "Sequence: parse('" << Corpus::kAllowAll << "') -> parse('" << Corpus::kGrammarBad << "')"
      << "\n  Current: grants.size() == " << cap.grants.size()
      << "\n  Expected: grants empty after grammar failure (INV-B)"
      << "\n  Function: MgrCap::parse(), src/mgr/MgrCap.cc:566";
}

// ---------------------------------------------------------------------------
// K2b: profile-validation failure after successful parse
// INVARIANT (INV-B): grants must be empty after any parse failure, including
// the profile-validation path (#81015).
// ---------------------------------------------------------------------------

TEST_F(MgrCapOrderingTest, OrderingBug_K2b_ProfileValidationFailAfterSuccess_UnknownProfile)
{
  ASSERT_TRUE(cap.parse(Corpus::kAllowAll))
      << "Sequence: parse('" << Corpus::kAllowAll << "')"
      << "\n  Expected: succeeds";

  bool r = cap.parse(Corpus::kProfileUnknown);
  EXPECT_FALSE(r)
      << "Sequence: parse('" << Corpus::kAllowAll << "') -> parse('" << Corpus::kProfileUnknown << "')"
      << "\n  Expected: second parse fails (profile unknown)";

  EXPECT_TRUE(cap.grants.empty())
      << "Sequence: parse('" << Corpus::kAllowAll << "') -> parse('" << Corpus::kProfileUnknown << "')"
      << "\n  Current: grants.size() == " << cap.grants.size()
      << "\n  Expected: grants empty after profile-validation failure (INV-B, #81015)"
      << "\n  Function: MgrCap::parse(), src/mgr/MgrCap.cc:555-563"
      << "\n  Bug: the profile-validation failure path does not call grants.clear()";
}

TEST_F(MgrCapOrderingTest, OrderingBug_K2b_ProfileValidationFailAfterSuccess_BadKey)
{
  ASSERT_TRUE(cap.parse(Corpus::kRbd))
      << "Sequence: parse('" << Corpus::kRbd << "')"
      << "\n  Expected: succeeds";

  bool r = cap.parse(Corpus::kProfileBadKey);
  EXPECT_FALSE(r)
      << "Sequence: parse('" << Corpus::kRbd << "') -> parse('" << Corpus::kProfileBadKey << "')"
      << "\n  Expected: second parse fails (rbd bad key)";

  EXPECT_TRUE(cap.grants.empty())
      << "Sequence: parse('" << Corpus::kRbd << "') -> parse('" << Corpus::kProfileBadKey << "')"
      << "\n  Current: grants.size() == " << cap.grants.size()
      << "\n  Expected: grants empty after profile-validation failure (INV-B, #81015)"
      << "\n  Function: MgrCap::parse(), src/mgr/MgrCap.cc:555-563";
}

TEST_F(MgrCapOrderingTest, OrderingBug_K2b_FreshObjectProfileValidationFail)
{
  bool r = cap.parse(Corpus::kProfileUnknown);
  EXPECT_FALSE(r)
      << "Sequence: parse('" << Corpus::kProfileUnknown << "') on fresh object"
      << "\n  Expected: parse fails";

  EXPECT_TRUE(cap.grants.empty())
      << "Sequence: fresh parse('" << Corpus::kProfileUnknown << "')"
      << "\n  Current: grants.size() == " << cap.grants.size()
      << "\n  Expected: grants empty after profile-validation failure (INV-B, #81015)";
}

// ---------------------------------------------------------------------------
// K3a: decode valid blob into a used MgrCap
// INVARIANT (INV-D): result equals a freshly decoded object.
// ---------------------------------------------------------------------------

TEST_F(MgrCapOrderingTest, OrderingBug_K3a_DecodeValidIntoUsed)
{
  ASSERT_TRUE(cap.parse(Corpus::kAllowAll))
      << "Sequence: parse('" << Corpus::kAllowAll << "')"
      << "\n  Expected: succeeds";

  MgrCap reference;
  ASSERT_TRUE(reference.parse(Corpus::kModule));
  auto ref_bl = encode_cap(reference);

  auto it = ref_bl.cbegin();
  cap.decode(it);

  EXPECT_EQ(cap.get_str(), reference.get_str())
      << "Sequence: parse('" << Corpus::kAllowAll << "') -> decode('" << Corpus::kModule << "')"
      << "\n  Current: text == '" << cap.get_str() << "'"
      << "\n  Expected: text == '" << reference.get_str() << "' (INV-D)";

  auto dec_cap = decisions(cap);
  auto dec_ref = decisions(reference);
  EXPECT_EQ(dec_cap, dec_ref)
      << "Sequence: parse('" << Corpus::kAllowAll << "') -> decode('" << Corpus::kModule << "')"
      << "\n  Current decisions: " << format_decisions(dec_cap)
      << "\n  Expected decisions: " << format_decisions(dec_ref)
      << "\n  Expected: decoded object makes identical is_capable decisions (INV-D)";
}

// ---------------------------------------------------------------------------
// K3b: decode invalid blob into a used MgrCap — text staleness bug
// INVARIANT (INV-D): result must equal a freshly decoded object.
// When decode receives an invalid cap string, parse() fails and does NOT set
// `text`. A fresh object has text=="" after the same failed decode; a reused
// object retains its prior `text`.
// ---------------------------------------------------------------------------

TEST_F(MgrCapOrderingTest, OrderingBug_K3b_DecodeInvalidIntoUsed_TextStaleness)
{
  ASSERT_TRUE(cap.parse(Corpus::kAllowAll))
      << "Sequence: parse('" << Corpus::kAllowAll << "')"
      << "\n  Expected: succeeds";

  const std::string old_text = cap.get_str();
  ASSERT_EQ(old_text, std::string(Corpus::kAllowAll));

  // Build a buffer encoding a cap that will fail grammar parse.
  // We encode it directly: version 4, then a bad string.
  // Easiest: encode a known-bad-grammar string as MgrCap text directly.
  // We can encode the text field manually using ENCODE_START macro pattern,
  // but it's simpler to encode a fresh MgrCap with a valid string then
  // reconstruct the buffer with a bad string:
  MgrCap bad_cap;
  // bad_cap.text is empty, so encoding it yields version=4 + text="".
  // We need the buffer to decode text == kGrammarBad.
  // Manually build a v4 buffer: encode(kGrammarBad).
  {
    ceph::buffer::list bl;
    // Emulate MgrCap::encode: ENCODE_START(4,4,bl) + encode(text, bl) + ENCODE_FINISH(bl)
    // Use a helper MgrCap whose text we set manually.
    MgrCap helper;
    helper.text = Corpus::kGrammarBad;  // set text without parsing
    helper.encode(bl);

    auto it = bl.cbegin();
    cap.decode(it);
  }

  // A fresh MgrCap decoded from the same buffer should have text=="" and
  // empty grants (because parse fails and text is only set on success).
  MgrCap fresh;
  {
    MgrCap helper;
    helper.text = Corpus::kGrammarBad;
    ceph::buffer::list bl;
    helper.encode(bl);
    auto it = bl.cbegin();
    fresh.decode(it);
  }

  EXPECT_TRUE(cap.grants.empty())
      << "Sequence: parse('" << Corpus::kAllowAll << "') -> decode(invalid grammar)"
      << "\n  Current: grants.size() == " << cap.grants.size()
      << "\n  Expected: grants empty after failed decode";

  EXPECT_EQ(cap.get_str(), fresh.get_str())
      << "Sequence: parse('" << Corpus::kAllowAll << "') -> decode(invalid grammar)"
      << "\n  Current: text == '" << cap.get_str() << "' (old value retained)"
      << "\n  Expected: text == '" << fresh.get_str() << "' (same as freshly decoded)"
      << "\n  INV-D violated: reused object retains stale text after failed decode"
      << "\n  Function: MgrCap::decode(), src/mgr/MgrCap.cc:370"
      << "\n  Root cause: decode() does not clear text before calling parse();"
         " parse() only sets text on success";
}

TEST_F(MgrCapOrderingTest, OrderingBug_K3b_DecodeProfileValidationFailIntoUsed_TextStaleness)
{
  ASSERT_TRUE(cap.parse(Corpus::kAllowAll))
      << "Sequence: parse('" << Corpus::kAllowAll << "')"
      << "\n  Expected: succeeds";

  MgrCap fresh;
  {
    // Encode the profile-unknown string.
    MgrCap helper;
    helper.text = Corpus::kProfileUnknown;
    ceph::buffer::list bl;
    helper.encode(bl);
    auto it = bl.cbegin();
    cap.decode(it);

    auto it2 = bl.cbegin();
    fresh.decode(it2);
  }

  // Both should have empty grants.
  EXPECT_TRUE(cap.grants.empty())
      << "Sequence: parse('" << Corpus::kAllowAll << "') -> decode('" << Corpus::kProfileUnknown << "')"
      << "\n  Current: grants.size() == " << cap.grants.size()
      << "\n  Expected: grants empty after decode of invalid profile (INV-B, #81015)";

  EXPECT_EQ(cap.get_str(), fresh.get_str())
      << "Sequence: parse('" << Corpus::kAllowAll << "') -> decode('" << Corpus::kProfileUnknown << "')"
      << "\n  Current: reused.text == '" << cap.get_str() << "'"
      << "\n  Expected: same as fresh.text == '" << fresh.get_str() << "' (INV-D)"
      << "\n  Function: MgrCap::decode(), src/mgr/MgrCap.cc:370"
      << "\n  Root cause: decode() does not clear text before calling parse();"
         " parse() only sets text on success";
}

// ---------------------------------------------------------------------------
// K4: encode/decode round trip
// INVARIANT (INV-D): is_capable decisions identical before and after round trip.
// ---------------------------------------------------------------------------

TEST_F(MgrCapOrderingTest, OrderingBug_K4_RoundTrip_ValidCaps)
{
  for (const auto& s : Corpus::valid()) {
    MgrCap original;
    ASSERT_TRUE(original.parse(s))
        << "Corpus item '" << s << "' failed to parse";

    auto before = decisions(original);
    auto bl = encode_cap(original);

    MgrCap decoded;
    auto it = bl.cbegin();
    decoded.decode(it);

    auto after = decisions(decoded);

    EXPECT_EQ(before, after)
        << "Sequence: parse('" << s << "') -> encode -> decode"
        << "\n  Decisions before encode: " << format_decisions(before)
        << "\n  Decisions after decode:  " << format_decisions(after)
        << "\n  Expected: identical is_capable decisions after round-trip (INV-D)";

    EXPECT_EQ(decoded.get_str(), original.get_str())
        << "Sequence: parse('" << s << "') -> encode -> decode"
        << "\n  Current: decoded.text == '" << decoded.get_str() << "'"
        << "\n  Expected: text == '" << original.get_str() << "' (INV-D)";
  }
}

// ---------------------------------------------------------------------------
// K5: copy and mutate original
// INVARIANT (INV-E): copy is fully independent; mutations to original do not
// affect the copy; profile_grants are separate.
// ---------------------------------------------------------------------------

TEST_F(MgrCapOrderingTest, OrderingBug_K5_CopyIndependence_FailedParseOnOriginal)
{
  ASSERT_TRUE(cap.parse(Corpus::kRbd))
      << "Sequence: parse('" << Corpus::kRbd << "')"
      << "\n  Expected: succeeds";

  MgrCap copy = cap;

  auto dec_copy_before = decisions(copy);
  auto text_copy = copy.get_str();

  // Mutate original with a failed parse.
  bool r = cap.parse(Corpus::kGrammarBad);
  EXPECT_FALSE(r);

  auto dec_copy_after = decisions(copy);
  EXPECT_EQ(dec_copy_before, dec_copy_after)
      << "Sequence: parse(rbd) -> copy -> parse(invalid) on original"
      << "\n  Before: copy decisions: " << format_decisions(dec_copy_before)
      << "\n  After:  copy decisions: " << format_decisions(dec_copy_after)
      << "\n  Expected: copy unaffected by mutation of original (INV-E)";

  EXPECT_EQ(copy.get_str(), text_copy)
      << "Sequence: parse(rbd) -> copy -> parse(invalid) on original"
      << "\n  Current: copy.text == '" << copy.get_str() << "'"
      << "\n  Expected: copy.text == '" << text_copy << "' (INV-E)";
}

TEST_F(MgrCapOrderingTest, OrderingBug_K5_CopyIndependence_SetAllowAllOnOriginal)
{
  ASSERT_TRUE(cap.parse(Corpus::kReadOnly))
      << "Sequence: parse('" << Corpus::kReadOnly << "')"
      << "\n  Expected: succeeds";

  MgrCap copy = cap;
  auto dec_copy_before = decisions(copy);

  cap.set_allow_all();

  auto dec_copy_after = decisions(copy);
  EXPECT_EQ(dec_copy_before, dec_copy_after)
      << "Sequence: parse(read-only) -> copy -> set_allow_all() on original"
      << "\n  Before copy decisions: " << format_decisions(dec_copy_before)
      << "\n  After  copy decisions: " << format_decisions(dec_copy_after)
      << "\n  Expected: copy unaffected by set_allow_all on original (INV-E)";

  EXPECT_TRUE(cap.is_allow_all())
      << "Sequence: parse(read-only) -> copy -> set_allow_all() on original"
      << "\n  Expected: original is allow-all after set_allow_all";

  EXPECT_FALSE(copy.is_allow_all())
      << "Sequence: parse(read-only) -> copy -> set_allow_all() on original"
      << "\n  Expected: copy is NOT allow-all (it was read-only) (INV-E)";
}

// ---------------------------------------------------------------------------
// K6: set_allow_all and parse ordering
// INVARIANT (INV-C, INV-B)
// ---------------------------------------------------------------------------

TEST_F(MgrCapOrderingTest, OrderingBug_K6a_SetAllowAllThenGrammarFailure)
{
  cap.set_allow_all();
  ASSERT_TRUE(cap.is_allow_all())
      << "Expected: is_allow_all after set_allow_all";

  bool r = cap.parse(Corpus::kGrammarBad);
  EXPECT_FALSE(r)
      << "Sequence: set_allow_all -> parse('" << Corpus::kGrammarBad << "')"
      << "\n  Expected: parse fails";

  EXPECT_FALSE(cap.is_allow_all())
      << "Sequence: set_allow_all -> parse(grammar failure)"
      << "\n  Current: is_allow_all() == true"
      << "\n  Expected: is_allow_all() == false (grammar failure clears grants)";

  EXPECT_TRUE(cap.grants.empty())
      << "Sequence: set_allow_all -> parse(grammar failure)"
      << "\n  Current: grants.size() == " << cap.grants.size()
      << "\n  Expected: grants empty after grammar failure (INV-B)";
}

TEST_F(MgrCapOrderingTest, OrderingBug_K6a_SetAllowAllThenProfileValidationFailure)
{
  cap.set_allow_all();
  ASSERT_TRUE(cap.is_allow_all())
      << "Expected: is_allow_all after set_allow_all";

  bool r = cap.parse(Corpus::kProfileUnknown);
  EXPECT_FALSE(r)
      << "Sequence: set_allow_all -> parse('" << Corpus::kProfileUnknown << "')"
      << "\n  Expected: parse fails (profile unknown)";

  EXPECT_TRUE(cap.grants.empty())
      << "Sequence: set_allow_all -> parse(profile unknown)"
      << "\n  Current: grants.size() == " << cap.grants.size()
      << "\n  Expected: grants empty after profile-validation failure (INV-B, #81015)"
      << "\n  Function: MgrCap::parse(), src/mgr/MgrCap.cc:555-563";
}

TEST_F(MgrCapOrderingTest, OrderingBug_K6b_ParseThenSetAllowAll)
{
  ASSERT_TRUE(cap.parse(Corpus::kAllowR));

  EXPECT_FALSE(cap.is_allow_all())
      << "Sequence: parse(allow r)"
      << "\n  Expected: is_allow_all == false after allow r";

  cap.set_allow_all();

  EXPECT_TRUE(cap.is_allow_all())
      << "Sequence: parse(allow r) -> set_allow_all"
      << "\n  Expected: is_allow_all == true after set_allow_all (INV-C)";

  EXPECT_EQ(cap.get_str(), "allow *")
      << "Sequence: parse(allow r) -> set_allow_all"
      << "\n  Current: text == '" << cap.get_str() << "'"
      << "\n  Expected: text == 'allow *' (INV-C)";

  EXPECT_EQ(cap.grants.size(), 1u)
      << "Sequence: parse(allow r) -> set_allow_all"
      << "\n  Expected: exactly one grant after set_allow_all (INV-C)";
}

// ---------------------------------------------------------------------------
// K6c: multiple successive failed parses — INV-B must hold after each one
// ---------------------------------------------------------------------------

TEST_F(MgrCapOrderingTest, OrderingBug_K6c_SuccessiveFailedParses)
{
  const std::vector<std::string> failures = {
    Corpus::kGrammarBad, Corpus::kProfileUnknown, Corpus::kProfileBadKey, Corpus::kEmpty,
  };

  ASSERT_TRUE(cap.parse(Corpus::kAllowAll))
      << "Setup: parse(allow *) succeeds";

  for (const auto& bad : failures) {
    bool r = cap.parse(bad);
    EXPECT_FALSE(r)
        << "Sequence: parse('" << bad << "') expected to fail";

    EXPECT_TRUE(cap.grants.empty())
        << "Sequence: parse(allow *) -> parse('" << bad << "')"
        << "\n  Current: grants.size() == " << cap.grants.size()
        << "\n  Expected: grants empty after any parse failure (INV-B)"
        << "\n  Note: profile-validation failures (#81015) may leave grants non-empty";
  }
}

// ---------------------------------------------------------------------------
// K7: concurrent is_capable on a shared parsed MgrCap
// NOT-TESTABLE: After parse(), profile_grants for substantive profiles are
// already populated (no write in expand_profile at is_capable time). For
// zero-grant profiles (osd/mds/rgw), expand_profile returns immediately
// without writing profile_grants. No write ever happens to profile_grants
// during concurrent is_capable calls on a fully parsed cap.
//
// This placeholder documents the analysis and verifies the claim that
// concurrent reads on a parsed cap are safe.
// ---------------------------------------------------------------------------

TEST_F(MgrCapOrderingTest, OrderingBug_K7_ConcurrentIsCapable_SubstantiveProfile)
{
  ASSERT_TRUE(cap.parse(Corpus::kRbd))
      << "Expected: parse(profile rbd) succeeds";

  // Verify profile_grants are already populated after parse (memoization fires
  // immediately in get_allowed, so no write happens during is_capable).
  ASSERT_FALSE(cap.grants.empty());
  EXPECT_FALSE(cap.grants.front().profile_grants.empty())
      << "Expected: profile_grants populated eagerly by parse() for 'rbd'"
      << "\n  This confirms no write happens to profile_grants during"
         " concurrent is_capable calls.";

  // Run concurrent is_capable on the shared parsed cap to verify no crash/race.
  std::atomic<int> mismatches{0};
  const int kIters = 10000;
  auto dec_serial = decisions(cap);

  auto reader = [&]() {
    for (int i = 0; i < kIters; ++i) {
      auto d = decisions(cap);
      if (d != dec_serial) {
        mismatches.fetch_add(1, std::memory_order_relaxed);
      }
    }
  };

  std::thread t1(reader);
  std::thread t2(reader);
  t1.join();
  t2.join();

  EXPECT_EQ(mismatches.load(), 0)
      << "Sequence: parse(profile rbd) -> concurrent is_capable x " << (2 * kIters)
      << "\n  Current: " << mismatches.load() << " decision mismatches"
      << "\n  Expected: 0 mismatches — concurrent reads of a fully parsed cap"
         " are safe because profile_grants is already populated and no write occurs";
}

TEST_F(MgrCapOrderingTest, OrderingBug_K7_ConcurrentIsCapable_ZeroGrantProfile)
{
  ASSERT_TRUE(cap.parse(Corpus::kOsd))
      << "Expected: parse(profile osd) succeeds";

  ASSERT_FALSE(cap.grants.empty());
  EXPECT_TRUE(cap.grants.front().profile_grants.empty())
      << "Expected: profile_grants empty for 'osd' (zero-grant profile)"
      << "\n  expand_profile returns without writing profile_grants for osd/mds/rgw";

  // Concurrent is_capable on zero-grant profile: expand_profile(nullptr) is
  // called on every get_allowed call, but it returns immediately without
  // writing profile_grants (the osd/mds/rgw branch just returns). Safe.
  std::atomic<int> mismatches{0};
  const int kIters = 10000;

  auto reader = [&]() {
    for (int i = 0; i < kIters; ++i) {
      bool d = capable(cap, kRequests[0]);
      if (d) {
        mismatches.fetch_add(1, std::memory_order_relaxed);
      }
    }
  };

  std::thread t1(reader);
  std::thread t2(reader);
  t1.join();
  t2.join();

  EXPECT_EQ(mismatches.load(), 0)
      << "Sequence: parse(profile osd) -> concurrent is_capable x " << (2 * kIters)
      << "\n  Expected: osd profile grants nothing; no data race (INV from K7 analysis)";
}

// ---------------------------------------------------------------------------
// Randomized walk test
// Exercises all invariants across 100000 random operation sequences.
// Fixed seed for reproducibility; prints seed and minimal failing sequence.
// ---------------------------------------------------------------------------

TEST_F(MgrCapOrderingTest, OrderingBug_RandomWalk_AllInvariants)
{
  const uint64_t seed = 0x5a7e3c91ULL;
  std::mt19937_64 rng(seed);
  const int kSteps = 100000;

  const auto& valid_strings = Corpus::valid();

  std::vector<std::string> trace;
  trace.reserve(32);

  auto log = [&](const std::string& msg) {
    if (trace.size() >= 32) trace.erase(trace.begin());
    trace.push_back(msg);
  };

  auto tail_trace = [&](size_t n) -> std::string {
    std::ostringstream oss;
    const size_t start = (trace.size() > n) ? trace.size() - n : 0;
    for (size_t i = start; i < trace.size(); ++i) {
      oss << "\n    [" << i << "] " << trace[i];
    }
    return oss.str();
  };

  // State: current cap and a "last known good" reference for copy tests.
  MgrCap current;
  MgrCap copy_ref;
  bool copy_valid = false;
  std::string copy_text;
  std::vector<bool> copy_decisions;

  // Operations: 0=parse(valid), 1=parse(invalid), 2=set_allow_all,
  //             3=copy, 4=is_capable, 5=decode(valid), 6=decode(invalid)
  for (int step = 0; step < kSteps; ++step) {
    const int op = rng() % 7;
    switch (op) {
      case 0: {
        const std::string& s = valid_strings[rng() % valid_strings.size()];
        bool r = current.parse(s);
        log("parse(valid:'" + s + "') -> " + (r ? "ok" : "FAIL"));
        if (r) {
          // INV-A: text == s, grants non-empty for non-empty grants
          ASSERT_EQ(current.get_str(), s)
              << "RandomWalk step " << step
              << ": parse(valid:'" << s << "') -> text mismatch"
              << "\n  Current: text == '" << current.get_str() << "'"
              << "\n  Expected: text == '" << s << "' (INV-A)"
              << "\n  Seed: 0x" << std::hex << seed
              << "\n  Trace:" << tail_trace(12);
        } else {
          // A valid string must parse successfully.
          FAIL() << "RandomWalk step " << step
                 << ": parse(valid:'" << s << "') returned false unexpectedly"
                 << "\n  Seed: 0x" << std::hex << seed
                 << "\n  Trace:" << tail_trace(12);
        }
        copy_valid = false;
        break;
      }
      case 1: {
        const std::string& s = Corpus::invalid()[rng() % Corpus::invalid().size()];
        MgrCap pre_state;
        // Capture pre-state.
        bool had_grants = !current.grants.empty();
        (void)had_grants;
        bool r = current.parse(s);
        log("parse(invalid:'" + s + "') -> " + (r ? "ok" : "FAIL"));
        ASSERT_FALSE(r)
            << "RandomWalk step " << step
            << ": parse(invalid:'" << s << "') returned true unexpectedly"
            << "\n  Seed: 0x" << std::hex << seed
            << "\n  Trace:" << tail_trace(12);
        // INV-B: grants must be empty after any failure.
        ASSERT_TRUE(current.grants.empty())
            << "RandomWalk step " << step
            << ": parse(invalid:'" << s << "') left non-empty grants"
            << "\n  Current: grants.size() == " << current.grants.size()
            << "\n  Expected: grants empty after any parse failure (INV-B, #81015)"
            << "\n  Seed: 0x" << std::hex << seed
            << "\n  Trace:" << tail_trace(12);
        copy_valid = false;
        break;
      }
      case 2: {
        current.set_allow_all();
        log("set_allow_all");
        // INV-C: text == "allow *", exactly one grant, is_allow_all true.
        ASSERT_EQ(current.get_str(), "allow *")
            << "RandomWalk step " << step << ": set_allow_all text mismatch"
            << "\n  Current: '" << current.get_str() << "'"
            << "\n  Seed: 0x" << std::hex << seed;
        ASSERT_TRUE(current.is_allow_all())
            << "RandomWalk step " << step << ": is_allow_all false after set_allow_all"
            << "\n  Seed: 0x" << std::hex << seed;
        ASSERT_EQ(current.grants.size(), 1u)
            << "RandomWalk step " << step << ": grants.size() != 1 after set_allow_all"
            << "\n  Seed: 0x" << std::hex << seed;
        copy_valid = false;
        break;
      }
      case 3: {
        copy_ref = current;
        copy_valid = !current.grants.empty();
        copy_text = current.get_str();
        copy_decisions = decisions(current);
        log("copy (copy_valid=" + std::string(copy_valid ? "true" : "false") + ")");
        break;
      }
      case 4: {
        auto dec = decisions(current);
        log("is_capable (grants.size=" + std::to_string(current.grants.size()) + ")");
        if (copy_valid) {
          // INV-E: if we copied from the current and then mutated current,
          // the copy is unaffected. We check this by ensuring the copy's
          // decisions still match what they were at copy time.
          auto copy_dec_now = decisions(copy_ref);
          ASSERT_EQ(copy_dec_now, copy_decisions)
              << "RandomWalk step " << step
              << ": copy's decisions changed after mutations to original"
              << "\n  At copy time: " << format_decisions(copy_decisions)
              << "\n  Now:          " << format_decisions(copy_dec_now)
              << "\n  Expected: copy is fully independent (INV-E)"
              << "\n  Seed: 0x" << std::hex << seed
              << "\n  Trace:" << tail_trace(12);
        }
        break;
      }
      case 5: {
        const std::string& s = valid_strings[rng() % valid_strings.size()];
        MgrCap ref;
        ASSERT_TRUE(ref.parse(s));
        auto ref_bl = encode_cap(ref);
        auto ref_dec = decisions(ref);

        auto it = ref_bl.cbegin();
        current.decode(it);

        auto curr_dec = decisions(current);
        log("decode(valid:'" + s + "')");

        ASSERT_EQ(current.get_str(), s)
            << "RandomWalk step " << step
            << ": decode(valid:'" << s << "') text mismatch"
            << "\n  Current: text == '" << current.get_str() << "'"
            << "\n  Expected: text == '" << s << "' (INV-D)"
            << "\n  Seed: 0x" << std::hex << seed
            << "\n  Trace:" << tail_trace(12);

        ASSERT_EQ(curr_dec, ref_dec)
            << "RandomWalk step " << step
            << ": decode(valid:'" << s << "') decisions mismatch"
            << "\n  Current: " << format_decisions(curr_dec)
            << "\n  Expected: " << format_decisions(ref_dec)
            << "\n  Seed: 0x" << std::hex << seed
            << "\n  Trace:" << tail_trace(12);
        copy_valid = false;
        break;
      }
      case 6: {
        // Decode an invalid blob (grammar-bad string encoded directly).
        MgrCap helper;
        helper.text = Corpus::kGrammarBad;
        auto bl = encode_cap(helper);

        // Capture what a fresh object looks like after the same decode.
        MgrCap fresh;
        {
          auto it = bl.cbegin();
          fresh.decode(it);
        }

        auto it = bl.cbegin();
        current.decode(it);
        log("decode(invalid grammar)");

        ASSERT_TRUE(current.grants.empty())
            << "RandomWalk step " << step
            << ": decode(invalid) left non-empty grants"
            << "\n  Current: grants.size() == " << current.grants.size()
            << "\n  Seed: 0x" << std::hex << seed
            << "\n  Trace:" << tail_trace(12);

        // INV-D: reused object must equal freshly decoded object.
        ASSERT_EQ(current.get_str(), fresh.get_str())
            << "RandomWalk step " << step
            << ": decode(invalid) text staleness bug"
            << "\n  Current (reused object): text == '" << current.get_str() << "'"
            << "\n  Expected (fresh object): text == '" << fresh.get_str() << "'"
            << "\n  INV-D violated: reused object retains stale text"
            << "\n  Function: MgrCap::decode(), src/mgr/MgrCap.cc:370"
            << "\n  Seed: 0x" << std::hex << seed
            << "\n  Trace:" << tail_trace(12);
        copy_valid = false;
        break;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
