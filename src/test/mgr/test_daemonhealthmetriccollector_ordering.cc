// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "common/Formatter.h"
#include "common/JSONFormatter.h"
#include "include/buffer.h"
#include "include/encoding.h"
#include "include/health.h"
#include "mgr/DaemonHealthMetric.h"
#include "mgr/DaemonHealthMetricCollector.h"
#include "mgr/DaemonKey.h"
#include "mon/health_check.h"

namespace {

constexpr const char* SLOW_CODE = "SLOW_OPS";
constexpr const char* PGS_CODE = "PENDING_CREATING_PGS";
constexpr const char* DEBUG_CODE = "HIGH_DEBUG_LEVEL";

DaemonKey osd(int id)
{
  return DaemonKey{"osd", std::to_string(id)};
}

DaemonHealthMetric slow_metric(uint32_t n, uint32_t blocked)
{
  return DaemonHealthMetric(daemon_metric::SLOW_OPS, n, blocked);
}

DaemonHealthMetric pgs_metric(uint64_t n)
{
  return DaemonHealthMetric(daemon_metric::PENDING_CREATING_PGS, n);
}

DaemonHealthMetric debug_metric(uint32_t count, uint32_t level)
{
  return DaemonHealthMetric(daemon_metric::HIGH_DEBUG_LEVEL, count, level);
}

DaemonHealthMetric none_metric(uint64_t n)
{
  return DaemonHealthMetric(daemon_metric::NONE, n);
}

std::string join(const std::vector<std::string>& v, const std::string& sep)
{
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) {
      out += sep;
    }
    out += v[i];
  }
  return out;
}

std::string named(const std::vector<int>& ids, size_t limit)
{
  std::vector<std::string> names;
  for (size_t i = 0; i < ids.size() && i < limit; ++i) {
    names.push_back("osd." + std::to_string(ids[i]));
  }
  return "[" + join(names, ",") + "]";
}

size_t named_count(const std::string& s)
{
  size_t n = 0;
  for (size_t p = s.find("osd."); p != std::string::npos;
       p = s.find("osd.", p + 1)) {
    ++n;
  }
  return n;
}

std::string slow_who(const std::vector<int>& ids)
{
  if (ids.size() == 1) {
    return "osd." + std::to_string(ids[0]) + " has slow ops";
  }
  if (ids.size() > 10) {
    return "daemons " + named(ids, 10) + "... have slow ops.";
  }
  return "daemons " + named(ids, ids.size()) + " have slow ops.";
}

std::string slow_summary(uint64_t total, uint64_t blocked,
			 const std::vector<int>& ids)
{
  return std::to_string(total) + " slow ops, oldest one blocked for " +
    std::to_string(blocked) + " sec, " + slow_who(ids);
}

std::string pgs_summary(uint64_t total)
{
  return std::to_string(total) + " PGs pending on creation";
}

std::string pgs_detail(const std::vector<int>& ids)
{
  if (ids.size() == 1) {
    return "osd." + std::to_string(ids[0]) + " has pending PGs";
  }
  return "osds " + named(ids, ids.size()) + " have pending PGs.";
}

std::string debug_summary(uint64_t total, uint64_t max_level,
			  const std::vector<int>& ids)
{
  std::string who;
  if (ids.size() == 1) {
    who = "osd." + std::to_string(ids[0]);
  } else if (ids.size() > 10) {
    who = named(ids, 10) + "...";
  } else {
    who = named(ids, ids.size());
  }
  return std::to_string(total) + " subsystems across " +
    std::to_string(ids.size()) + " daemon(s) have high debug levels (max " +
    std::to_string(max_level) + "): " + who;
}

std::string describe_check(const std::string& code, int severity,
			   int64_t count, const std::string& summary,
			   const std::vector<std::string>& detail)
{
  std::ostringstream os;
  os << code << "{severity=" << severity << ",count=" << count
     << ",summary='" << summary << "',detail=[" << join(detail, "|") << "]}";
  return os.str();
}

std::string describe(const health_check_map_t& cm)
{
  if (cm.checks.empty()) {
    return "<empty map>";
  }
  std::vector<std::string> parts;
  for (const auto& [code, check] : cm.checks) {
    parts.push_back(describe_check(
      code, static_cast<int>(check.severity), check.count, check.summary,
      std::vector<std::string>(check.detail.begin(), check.detail.end())));
  }
  return join(parts, " ");
}

std::unique_ptr<DaemonHealthMetricCollector> checked_create(daemon_metric t)
{
  return DaemonHealthMetricCollector::create(t);
}

class CollectorOrdering : public ::testing::Test {
protected:
  std::vector<std::string> ops;

  std::unique_ptr<DaemonHealthMetricCollector> make(daemon_metric t)
  {
    ops.push_back(std::string("create(") + daemon_metric_name(t) + ")");
    auto c = checked_create(t);
    EXPECT_TRUE(c != nullptr) << "Sequence: " << seq()
			  << "\nCurrent: create returned null"
			  << "\nExpected: non-null collector";
    return c;
  }

  void update(DaemonHealthMetricCollector& c, const DaemonKey& k,
	      const DaemonHealthMetric& m)
  {
    std::ostringstream os;
    os << "update(" << k << "," << m << ")";
    ops.push_back(os.str());
    c.update(k, m);
  }

  void summarize_into(DaemonHealthMetricCollector& c, health_check_map_t& cm)
  {
    ops.push_back("summarize");
    c.summarize(cm);
  }

  health_check_map_t summarize(DaemonHealthMetricCollector& c)
  {
    health_check_map_t cm;
    summarize_into(c, cm);
    return cm;
  }

  std::string seq() const
  {
    if (ops.size() <= 12) {
      return join(ops, "; ");
    }
    std::vector<std::string> head(ops.begin(), ops.begin() + 4);
    std::vector<std::string> tail(ops.end() - 3, ops.end());
    return join(head, "; ") + "; ...(" + std::to_string(ops.size() - 7) +
      " more)...; " + join(tail, "; ");
  }

  std::string msg(const health_check_map_t& cm,
		  const std::string& expected) const
  {
    return "\nSequence: " + seq() + "\nCurrent: " + describe(cm) +
      "\nExpected: " + expected;
  }

  const health_check_t* find_check(const health_check_map_t& cm,
				   const char* code) const
  {
    auto it = cm.checks.find(code);
    return it == cm.checks.end() ? nullptr : &it->second;
  }

  void expect_summary(const health_check_map_t& cm, const char* code,
		      const std::string& expected_summary)
  {
    const auto* check = find_check(cm, code);
    ASSERT_NE(check, nullptr) << msg(cm, std::string(code) + " present with summary '" +
				     expected_summary + "'");
    EXPECT_EQ(check->summary, expected_summary)
      << msg(cm, std::string(code) + " summary '" + expected_summary + "'");
    EXPECT_EQ(check->count, 1)
      << msg(cm, std::string(code) + " count=1");
    EXPECT_EQ(static_cast<int>(check->severity), static_cast<int>(HEALTH_WARN))
      << msg(cm, std::string(code) + " severity HEALTH_WARN");
  }

  void slow_cap(int n)
  {
    auto c = make(daemon_metric::SLOW_OPS);
    std::vector<int> ids;
    for (int i = 0; i < n; ++i) {
      update(*c, osd(i), slow_metric(1, i + 1));
      ids.push_back(i);
    }
    auto cm = summarize(*c);
    const std::string expected = slow_summary(n, n, ids);
    expect_summary(cm, SLOW_CODE, expected);
    const auto* check = find_check(cm, SLOW_CODE);
    ASSERT_NE(check, nullptr) << msg(cm, expected);
    EXPECT_LE(named_count(check->summary), 10u)
      << msg(cm, "at most 10 daemons named in SLOW_OPS summary");
    EXPECT_TRUE(check->detail.empty())
      << msg(cm, "no detail lines for SLOW_OPS");
  }

  void pgs_cap(int n)
  {
    auto c = make(daemon_metric::PENDING_CREATING_PGS);
    std::vector<int> ids;
    uint64_t total = 0;
    for (int i = 0; i < n; ++i) {
      update(*c, osd(i), pgs_metric(i + 1));
      total += i + 1;
      ids.push_back(i);
    }
    auto cm = summarize(*c);
    expect_summary(cm, PGS_CODE, pgs_summary(total));
    const auto* check = find_check(cm, PGS_CODE);
    ASSERT_NE(check, nullptr) << msg(cm, pgs_summary(total));
    ASSERT_EQ(check->detail.size(), 1u)
      << msg(cm, "exactly one detail line");
    EXPECT_LE(named_count(check->detail.front()), 10u)
      << msg(cm, "at most 10 daemons named in PENDING_CREATING_PGS detail "
		 "(same cap as SLOW_OPS); detail would be e.g. " +
		 (n <= 10 ? pgs_detail(ids) : "osds " + named(ids, 10) +
		  "... have pending PGs."));
    if (n <= 10) {
      EXPECT_EQ(check->detail.front(), pgs_detail(ids))
	<< msg(cm, pgs_detail(ids));
    }
  }

  void zero_mix(bool zero_first)
  {
    {
      auto c = make(daemon_metric::SLOW_OPS);
      if (zero_first) {
	update(*c, osd(2), slow_metric(0, 0));
      }
      update(*c, osd(1), slow_metric(5, 7));
      if (!zero_first) {
	update(*c, osd(2), slow_metric(0, 0));
      }
      auto cm = summarize(*c);
      expect_summary(cm, SLOW_CODE, slow_summary(5, 7, {1}));
    }
    ops.clear();
    {
      auto c = make(daemon_metric::PENDING_CREATING_PGS);
      if (zero_first) {
	update(*c, osd(2), pgs_metric(0));
      }
      update(*c, osd(1), pgs_metric(4));
      if (!zero_first) {
	update(*c, osd(2), pgs_metric(0));
      }
      auto cm = summarize(*c);
      expect_summary(cm, PGS_CODE, pgs_summary(4));
      const auto* check = find_check(cm, PGS_CODE);
      ASSERT_NE(check, nullptr) << msg(cm, pgs_summary(4));
      EXPECT_EQ(std::vector<std::string>(check->detail.begin(),
					 check->detail.end()),
		std::vector<std::string>{pgs_detail({1})})
	<< msg(cm, "detail [" + pgs_detail({1}) + "]");
    }
    ops.clear();
    {
      auto c = make(daemon_metric::HIGH_DEBUG_LEVEL);
      if (zero_first) {
	update(*c, osd(2), debug_metric(0, 0));
      }
      update(*c, osd(1), debug_metric(3, 4));
      if (!zero_first) {
	update(*c, osd(2), debug_metric(0, 0));
      }
      auto cm = summarize(*c);
      expect_summary(cm, DEBUG_CODE, debug_summary(3, 4, {1}));
    }
  }

  void expect_ignores_foreign(daemon_metric target,
			      const DaemonHealthMetric& own,
			      const std::vector<DaemonHealthMetric>& foreign)
  {
    {
      auto c = make(target);
      for (const auto& m : foreign) {
	update(*c, osd(1), m);
      }
      auto cm = summarize(*c);
      EXPECT_TRUE(cm.checks.empty())
	<< msg(cm, "<empty map>: foreign metric types must not set reported");
    }
    ops.clear();
    health_check_map_t control;
    {
      auto c = make(target);
      update(*c, osd(1), own);
      control = summarize(*c);
    }
    ops.clear();
    auto c = make(target);
    update(*c, osd(3), foreign.front());
    update(*c, osd(1), own);
    update(*c, osd(4), foreign.back());
    auto cm = summarize(*c);
    EXPECT_EQ(cm, control)
      << msg(cm, describe(control) +
		 " (foreign updates interleaved must not change the result)");
  }
};

}  // namespace

namespace {

DaemonHealthMetric unknown_metric(uint64_t n)
{
  return DaemonHealthMetric(static_cast<daemon_metric>(77), n);
}

DaemonHealthMetric roundtrip(const DaemonHealthMetric& in)
{
  ceph::buffer::list bl;
  encode(in, bl);
  auto p = bl.cbegin();
  DaemonHealthMetric out;
  decode(out, p);
  return out;
}

}  // namespace

TEST_F(CollectorOrdering, OrderingBug_D0_DefaultConstructedMetricValueIsZero)
{
  alignas(DaemonHealthMetric) unsigned char buf[sizeof(DaemonHealthMetric)];
  memset(buf, 0xFF, sizeof(buf));
  ops.push_back("memset(storage, 0xFF)");
  ops.push_back("new (storage) DaemonHealthMetric");
  auto* m = new (buf) DaemonHealthMetric;
  const uint64_t n = m->get_n();
  const uint32_t n1 = m->get_n1();
  const uint32_t n2 = m->get_n2();
  const daemon_metric t = m->get_type();
  m->~DaemonHealthMetric();
  EXPECT_EQ(n, 0u) << "Sequence: " << seq() << "\nCurrent: get_n()=" << n
		   << "\nExpected: get_n()=0";
  EXPECT_EQ(n1, 0u) << "Sequence: " << seq() << "\nCurrent: get_n1()=" << n1
		    << "\nExpected: get_n1()=0";
  EXPECT_EQ(n2, 0u) << "Sequence: " << seq() << "\nCurrent: get_n2()=" << n2
		    << "\nExpected: get_n2()=0";
  EXPECT_EQ(t, daemon_metric::NONE)
    << "Sequence: " << seq() << "\nCurrent: type=" << daemon_metric_name(t)
    << "\nExpected: type=NONE";
}

TEST_F(CollectorOrdering, OrderingBug_D1_AllZeroUpdatesThenSummarize)
{
  {
    auto c = make(daemon_metric::SLOW_OPS);
    for (int i = 0; i < 5; ++i) {
      update(*c, osd(i), slow_metric(0, 0));
    }
    auto cm = summarize(*c);
    EXPECT_TRUE(cm.checks.empty()) << msg(cm, "<empty map>");
  }
  ops.clear();
  {
    auto c = make(daemon_metric::PENDING_CREATING_PGS);
    for (int i = 0; i < 5; ++i) {
      update(*c, osd(i), pgs_metric(0));
    }
    auto cm = summarize(*c);
    EXPECT_TRUE(cm.checks.empty()) << msg(cm, "<empty map>");
  }
  ops.clear();
  {
    auto c = make(daemon_metric::HIGH_DEBUG_LEVEL);
    for (int i = 0; i < 5; ++i) {
      update(*c, osd(i), debug_metric(0, 0));
    }
    auto cm = summarize(*c);
    EXPECT_TRUE(cm.checks.empty()) << msg(cm, "<empty map>");
  }
}

TEST_F(CollectorOrdering, OrderingBug_D1_NonZeroThenZeroFromOtherDaemon)
{
  zero_mix(false);
}

TEST_F(CollectorOrdering, OrderingBug_D1_ZeroThenNonZeroFromOtherDaemon)
{
  zero_mix(true);
}

TEST_F(CollectorOrdering, OrderingBug_D1_SummarizeDoesNotMutateCollector)
{
  auto c = make(daemon_metric::SLOW_OPS);
  update(*c, osd(1), slow_metric(2, 3));
  auto first = summarize(*c);
  auto second = summarize(*c);
  EXPECT_EQ(first, second)
    << msg(second, describe(first) + " (summarize into a fresh map is repeatable)");
  update(*c, osd(2), slow_metric(4, 1));
  auto third = summarize(*c);
  expect_summary(third, SLOW_CODE, slow_summary(6, 3, {1, 2}));
}

TEST_F(CollectorOrdering, OrderingBug_D2_SameDaemonTwice_SlowOps)
{
  auto c = make(daemon_metric::SLOW_OPS);
  update(*c, osd(1), slow_metric(2, 3));
  update(*c, osd(1), slow_metric(2, 3));
  auto cm = summarize(*c);
  expect_summary(cm, SLOW_CODE, slow_summary(2, 3, {1}));
}

TEST_F(CollectorOrdering, OrderingBug_D2_SameDaemonTwice_PendingPGs)
{
  auto c = make(daemon_metric::PENDING_CREATING_PGS);
  update(*c, osd(1), pgs_metric(4));
  update(*c, osd(1), pgs_metric(4));
  auto cm = summarize(*c);
  expect_summary(cm, PGS_CODE, pgs_summary(4));
  const auto* check = find_check(cm, PGS_CODE);
  ASSERT_NE(check, nullptr) << msg(cm, pgs_summary(4));
  EXPECT_EQ(std::vector<std::string>(check->detail.begin(), check->detail.end()),
	    std::vector<std::string>{pgs_detail({1})})
    << msg(cm, "detail [" + pgs_detail({1}) + "]");
}

TEST_F(CollectorOrdering, OrderingBug_D2_SameDaemonTwice_HighDebugLevel)
{
  auto c = make(daemon_metric::HIGH_DEBUG_LEVEL);
  update(*c, osd(1), debug_metric(3, 4));
  update(*c, osd(1), debug_metric(3, 4));
  auto cm = summarize(*c);
  expect_summary(cm, DEBUG_CODE, debug_summary(3, 4, {1}));
}

TEST_F(CollectorOrdering, OrderingBug_D6_PendingPGsTwoDaemonsWording)
{
  auto c = make(daemon_metric::PENDING_CREATING_PGS);
  update(*c, osd(5), pgs_metric(1));
  update(*c, osd(2), pgs_metric(2));
  auto cm = summarize(*c);
  expect_summary(cm, PGS_CODE, pgs_summary(3));
  const auto* check = find_check(cm, PGS_CODE);
  ASSERT_NE(check, nullptr) << msg(cm, pgs_summary(3));
  EXPECT_EQ(std::vector<std::string>(check->detail.begin(), check->detail.end()),
	    std::vector<std::string>{pgs_detail({5, 2})})
    << msg(cm, "detail [" + pgs_detail({5, 2}) + "]");
}

TEST_F(CollectorOrdering, OrderingBug_D3_SingleSummarizeYieldsCountOneAndOneDetailLine)
{
  auto c = make(daemon_metric::PENDING_CREATING_PGS);
  update(*c, osd(1), pgs_metric(4));
  update(*c, osd(2), pgs_metric(5));
  auto cm = summarize(*c);
  const auto* check = find_check(cm, PGS_CODE);
  ASSERT_NE(check, nullptr) << msg(cm, PGS_CODE + std::string(" present"));
  EXPECT_EQ(check->count, 1) << msg(cm, "count 1 after one summarize");
  EXPECT_EQ(check->detail.size(), 1u)
    << msg(cm, "exactly one detail line after one summarize");
}

TEST_F(CollectorOrdering, OrderingBug_D3_PreExistingCheckInMapKeepsItsDetailAndCount)
{
  health_check_map_t cm;
  auto& pre = cm.add(PGS_CODE, HEALTH_ERR, "from elsewhere", 5);
  pre.detail.push_back("pre-existing detail");
  auto c = make(daemon_metric::PENDING_CREATING_PGS);
  update(*c, osd(1), pgs_metric(4));
  summarize_into(*c, cm);
  const auto* check = find_check(cm, PGS_CODE);
  ASSERT_NE(check, nullptr) << msg(cm, PGS_CODE + std::string(" present"));
  EXPECT_EQ(check->summary, pgs_summary(4))
    << msg(cm, "summary replaced by '" + pgs_summary(4) + "'");
  EXPECT_EQ(static_cast<int>(check->severity), static_cast<int>(HEALTH_WARN))
    << msg(cm, "severity HEALTH_WARN");
  EXPECT_EQ(check->count, 6)
    << msg(cm, "count 5 (pre-existing) + 1 (this collector)");
  EXPECT_EQ(std::vector<std::string>(check->detail.begin(), check->detail.end()),
	    (std::vector<std::string>{"pre-existing detail", pgs_detail({1})}))
    << msg(cm, "detail [pre-existing detail | " + pgs_detail({1}) + "]");
}

TEST_F(CollectorOrdering, OrderingBug_D3_ThreeCollectorsOneMapAnyOrder)
{
  health_check_map_t reference;
  std::vector<int> order{0, 1, 2};
  bool have_reference = false;
  do {
    ops.clear();
    auto slow = make(daemon_metric::SLOW_OPS);
    auto pgs = make(daemon_metric::PENDING_CREATING_PGS);
    auto dbg = make(daemon_metric::HIGH_DEBUG_LEVEL);
    update(*slow, osd(1), slow_metric(2, 3));
    update(*pgs, osd(2), pgs_metric(7));
    update(*dbg, osd(3), debug_metric(1, 20));
    DaemonHealthMetricCollector* all[] = {slow.get(), pgs.get(), dbg.get()};
    health_check_map_t cm;
    for (int i : order) {
      summarize_into(*all[i], cm);
    }
    if (!have_reference) {
      reference = cm;
      have_reference = true;
    }
    EXPECT_EQ(cm.checks.size(), 3u) << msg(cm, "three independent checks");
    expect_summary(cm, SLOW_CODE, slow_summary(2, 3, {1}));
    expect_summary(cm, PGS_CODE, pgs_summary(7));
    expect_summary(cm, DEBUG_CODE, debug_summary(1, 20, {3}));
    EXPECT_EQ(cm, reference)
      << msg(cm, describe(reference) + " (result independent of summarize order)");
  } while (std::next_permutation(order.begin(), order.end()));
}

TEST_F(CollectorOrdering, OrderingBug_D4_ForeignTypesIgnored_SlowOps)
{
  expect_ignores_foreign(daemon_metric::SLOW_OPS, slow_metric(2, 3),
			 {pgs_metric(5), debug_metric(2, 3), none_metric(9),
			  unknown_metric(5)});
}

TEST_F(CollectorOrdering, OrderingBug_D4_ForeignTypesIgnored_PendingPGs)
{
  expect_ignores_foreign(daemon_metric::PENDING_CREATING_PGS, pgs_metric(5),
			 {slow_metric(2, 3), debug_metric(2, 3), none_metric(9),
			  unknown_metric(5)});
}

TEST_F(CollectorOrdering, OrderingBug_D4_ForeignTypesIgnored_HighDebugLevel)
{
  expect_ignores_foreign(daemon_metric::HIGH_DEBUG_LEVEL, debug_metric(2, 3),
			 {slow_metric(2, 3), pgs_metric(5), none_metric(9),
			  unknown_metric(5)});
}

TEST_F(CollectorOrdering, OrderingBug_D5_Uint64AndPairConstructorsAgree)
{
  const DaemonHealthMetric wide(daemon_metric::SLOW_OPS,
				(uint64_t(9) << 32) | uint64_t(7));
  const DaemonHealthMetric pair(daemon_metric::SLOW_OPS, uint32_t(7), uint32_t(9));
  auto a = make(daemon_metric::SLOW_OPS);
  update(*a, osd(1), wide);
  auto cm_wide = summarize(*a);
  ops.clear();
  auto b = make(daemon_metric::SLOW_OPS);
  update(*b, osd(1), pair);
  auto cm_pair = summarize(*b);
  expect_summary(cm_wide, SLOW_CODE, slow_summary(7, 9, {1}));
  EXPECT_EQ(cm_wide, cm_pair)
    << msg(cm_wide, describe(cm_pair) + " (64-bit and pair construction agree)");
}

TEST_F(CollectorOrdering, OrderingBug_D5_EncodeDecodeRoundTripKeepsSummary)
{
  const DaemonHealthMetric orig(daemon_metric::SLOW_OPS, uint32_t(7), uint32_t(9));
  const DaemonHealthMetric copy = roundtrip(orig);
  EXPECT_EQ(copy.get_type(), orig.get_type())
    << "Sequence: encode;decode\nCurrent: " << copy << "\nExpected: " << orig;
  EXPECT_EQ(copy.get_n(), orig.get_n())
    << "Sequence: encode;decode\nCurrent: " << copy << "\nExpected: " << orig;
  auto c = make(daemon_metric::SLOW_OPS);
  update(*c, osd(1), copy);
  auto cm = summarize(*c);
  expect_summary(cm, SLOW_CODE, slow_summary(7, 9, {1}));
}

TEST_F(CollectorOrdering, OrderingBug_D5_EncodeDecodeRoundTripKeepsPendingPGs)
{
  const DaemonHealthMetric orig(daemon_metric::PENDING_CREATING_PGS,
				uint64_t(1) << 40);
  const DaemonHealthMetric copy = roundtrip(orig);
  auto c = make(daemon_metric::PENDING_CREATING_PGS);
  update(*c, osd(1), copy);
  auto cm = summarize(*c);
  expect_summary(cm, PGS_CODE, pgs_summary(uint64_t(1) << 40));
}

TEST_F(CollectorOrdering, OrderingBug_D5_SlowOpsSumBeyond32Bits)
{
  auto c = make(daemon_metric::SLOW_OPS);
  std::vector<int> ids;
  for (int i = 0; i < 3; ++i) {
    update(*c, osd(i), slow_metric(0x80000000u, 1));
    ids.push_back(i);
  }
  auto cm = summarize(*c);
  expect_summary(cm, SLOW_CODE, slow_summary(3 * uint64_t(0x80000000u), 1, ids));
}

TEST_F(CollectorOrdering, OrderingBug_D5_SlowOpsTwoDaemonsSumToExactly2Pow32)
{
  auto c = make(daemon_metric::SLOW_OPS);
  update(*c, osd(0), slow_metric(0xFFFFFFFFu, 1));
  update(*c, osd(1), slow_metric(1, 1));
  auto cm = summarize(*c);
  expect_summary(cm, SLOW_CODE, slow_summary(uint64_t(1) << 32, 1, {0, 1}));
}

TEST_F(CollectorOrdering, OrderingBug_D5_SlowOpsSingleDaemonAtUint32Max)
{
  auto c = make(daemon_metric::SLOW_OPS);
  update(*c, osd(1), slow_metric(0xFFFFFFFFu, 0xFFFFFFFFu));
  auto cm = summarize(*c);
  expect_summary(cm, SLOW_CODE, slow_summary(0xFFFFFFFFu, 0xFFFFFFFFu, {1}));
}

TEST_F(CollectorOrdering, OrderingBug_D5_HighDebugLevelSumBeyond32Bits)
{
  auto c = make(daemon_metric::HIGH_DEBUG_LEVEL);
  std::vector<int> ids;
  for (int i = 0; i < 3; ++i) {
    update(*c, osd(i), debug_metric(0x80000000u, 5));
    ids.push_back(i);
  }
  auto cm = summarize(*c);
  expect_summary(cm, DEBUG_CODE,
		 debug_summary(3 * uint64_t(0x80000000u), 5, ids));
}

TEST_F(CollectorOrdering, OrderingBug_D5_PendingPGsSumBeyond32Bits)
{
  auto c = make(daemon_metric::PENDING_CREATING_PGS);
  std::vector<int> ids;
  uint64_t total = 0;
  for (int i = 0; i < 3; ++i) {
    update(*c, osd(i), pgs_metric(uint64_t(1) << 40));
    total += uint64_t(1) << 40;
    ids.push_back(i);
  }
  auto cm = summarize(*c);
  expect_summary(cm, PGS_CODE, pgs_summary(total));
}

TEST_F(CollectorOrdering, OrderingBug_D6_SlowOpsCap_1) { slow_cap(1); }
TEST_F(CollectorOrdering, OrderingBug_D6_SlowOpsCap_10) { slow_cap(10); }
TEST_F(CollectorOrdering, OrderingBug_D6_SlowOpsCap_11) { slow_cap(11); }
TEST_F(CollectorOrdering, OrderingBug_D6_SlowOpsCap_50) { slow_cap(50); }

TEST_F(CollectorOrdering, OrderingBug_D6_PendingPGsCap_1) { pgs_cap(1); }
TEST_F(CollectorOrdering, OrderingBug_D6_PendingPGsCap_10) { pgs_cap(10); }
TEST_F(CollectorOrdering, OrderingBug_D6_PendingPGsCap_11) { pgs_cap(11); }
TEST_F(CollectorOrdering, OrderingBug_D6_PendingPGsCap_50) { pgs_cap(50); }

TEST_F(CollectorOrdering, OrderingBug_D6_HighDebugLevelCap)
{
  for (int n : {1, 10, 11, 50}) {
    ops.clear();
    auto c = make(daemon_metric::HIGH_DEBUG_LEVEL);
    std::vector<int> ids;
    for (int i = 0; i < n; ++i) {
      update(*c, osd(i), debug_metric(1, i + 1));
      ids.push_back(i);
    }
    auto cm = summarize(*c);
    expect_summary(cm, DEBUG_CODE, debug_summary(n, n, ids));
    const auto* check = find_check(cm, DEBUG_CODE);
    ASSERT_NE(check, nullptr) << msg(cm, debug_summary(n, n, ids));
    EXPECT_LE(named_count(check->summary), 10u)
      << msg(cm, "at most 10 daemons named for " + std::to_string(n) + " reporters");
  }
}

TEST_F(CollectorOrdering, OrderingBug_D6_PrometheusLeadingTokenIsCount)
{
  for (int n : {1, 2, 10, 11, 50}) {
    ops.clear();
    auto c = make(daemon_metric::SLOW_OPS);
    for (int i = 0; i < n; ++i) {
      update(*c, osd(i), slow_metric(3, 2));
    }
    auto cm = summarize(*c);
    const auto* check = find_check(cm, SLOW_CODE);
    ASSERT_NE(check, nullptr) << msg(cm, "SLOW_OPS present");
    const std::string& s = check->summary;
    const std::string first = s.substr(0, s.find(' '));
    EXPECT_EQ(first, std::to_string(3 * n))
      << msg(cm, "first space-delimited token is the slow-op total " +
		 std::to_string(3 * n) + " (parsed by mgr/prometheus)");
    EXPECT_NE(s.find(" slow ops, oldest one blocked for 2 sec, "),
	      std::string::npos)
      << msg(cm, "'<n> slow ops, oldest one blocked for 2 sec, <daemons>'");
  }
}

TEST_F(CollectorOrdering, OrderingBug_D6_ListsAreInArrivalOrderNotSorted)
{
  auto c = make(daemon_metric::SLOW_OPS);
  update(*c, osd(3), slow_metric(1, 1));
  update(*c, osd(1), slow_metric(1, 1));
  update(*c, osd(2), slow_metric(1, 1));
  auto cm = summarize(*c);
  expect_summary(cm, SLOW_CODE, slow_summary(3, 1, {3, 1, 2}));
}

TEST_F(CollectorOrdering, OrderingBug_D7_CreateReturnsNullForUnhandledTypes)
{
  for (daemon_metric t : {daemon_metric::NONE, static_cast<daemon_metric>(4),
			  static_cast<daemon_metric>(77),
			  static_cast<daemon_metric>(255)}) {
    auto c = DaemonHealthMetricCollector::create(t);
    EXPECT_EQ(c, nullptr)
      << "Sequence: create(" << static_cast<int>(t) << ")\nCurrent: non-null collector"
      << "\nExpected: null unique_ptr";
  }
}

TEST_F(CollectorOrdering, OrderingBug_D7_CreateReturnsCollectorForHandledTypes)
{
  for (daemon_metric t : {daemon_metric::SLOW_OPS,
			  daemon_metric::PENDING_CREATING_PGS,
			  daemon_metric::HIGH_DEBUG_LEVEL}) {
    auto c = DaemonHealthMetricCollector::create(t);
    EXPECT_TRUE(c != nullptr)
      << "Sequence: create(" << daemon_metric_name(t) << ")\nCurrent: null"
      << "\nExpected: non-null collector";
  }
}

TEST_F(CollectorOrdering, OrderingBug_D7_CallerPatternSkipsUnhandledAndSummarizesRest)
{
  std::vector<std::pair<DaemonKey, DaemonHealthMetric>> reports{
    {osd(1), none_metric(4)},
    {osd(2), slow_metric(2, 3)},
    {osd(3), unknown_metric(8)},
    {osd(4), pgs_metric(6)},
  };
  std::map<daemon_metric, std::unique_ptr<DaemonHealthMetricCollector>> acc;
  for (const auto& [key, metric] : reports) {
    std::ostringstream os;
    os << "caller(" << key << "," << metric << ")";
    ops.push_back(os.str());
    auto it = acc.find(metric.get_type());
    if (it == acc.end()) {
      auto c = DaemonHealthMetricCollector::create(metric.get_type());
      if (!c) {
	continue;
      }
      it = acc.emplace(metric.get_type(), std::move(c)).first;
    }
    it->second->update(key, metric);
  }
  health_check_map_t cm;
  for (const auto& a : acc) {
    summarize_into(*a.second, cm);
  }
  EXPECT_EQ(cm.checks.size(), 2u) << msg(cm, "SLOW_OPS and PENDING_CREATING_PGS only");
  expect_summary(cm, SLOW_CODE, slow_summary(2, 3, {2}));
  expect_summary(cm, PGS_CODE, pgs_summary(6));
}

TEST_F(CollectorOrdering, OrderingBug_D8_HighDebugLevelUnlistedDaemonDoesNotRaiseMax)
{
  {
    auto c = make(daemon_metric::HIGH_DEBUG_LEVEL);
    update(*c, osd(2), debug_metric(0, 9));
    update(*c, osd(1), debug_metric(3, 4));
    auto cm = summarize(*c);
    expect_summary(cm, DEBUG_CODE, debug_summary(3, 4, {1}));
  }
  ops.clear();
  {
    auto c = make(daemon_metric::HIGH_DEBUG_LEVEL);
    update(*c, osd(1), debug_metric(3, 4));
    update(*c, osd(2), debug_metric(0, 9));
    auto cm = summarize(*c);
    expect_summary(cm, DEBUG_CODE, debug_summary(3, 4, {1}));
  }
}

TEST_F(CollectorOrdering, OrderingBug_D8_SlowOpsMaxBlockedIsOrderIndependent)
{
  std::vector<int> perm{0, 1, 2, 3};
  const uint32_t blocked[] = {5, 17, 3, 11};
  do {
    ops.clear();
    auto c = make(daemon_metric::SLOW_OPS);
    for (int i : perm) {
      update(*c, osd(i), slow_metric(i + 1, blocked[i]));
    }
    auto cm = summarize(*c);
    const auto* check = find_check(cm, SLOW_CODE);
    ASSERT_NE(check, nullptr) << msg(cm, "SLOW_OPS present");
    EXPECT_EQ(check->summary.substr(0, check->summary.find(", daemons")),
	      "10 slow ops, oldest one blocked for 17 sec")
      << msg(cm, "10 slow ops, oldest one blocked for 17 sec for every arrival order");
  } while (std::next_permutation(perm.begin(), perm.end()));
}

TEST_F(CollectorOrdering, OrderingBug_D8_MonAndOsdMixedKeysAreAllNamed)
{
  auto c = make(daemon_metric::SLOW_OPS);
  update(*c, DaemonKey{"osd", "1"}, slow_metric(1, 2));
  update(*c, DaemonKey{"mon", "a"}, slow_metric(0, 5));
  auto cm = summarize(*c);
  expect_summary(cm, SLOW_CODE,
		 "1 slow ops, oldest one blocked for 5 sec, "
		 "daemons [osd.1,mon.a] have slow ops.");
}

TEST_F(CollectorOrdering, OrderingBug_D1_SummarizeOnEmptyCollector)
{
  for (daemon_metric t : {daemon_metric::SLOW_OPS,
			  daemon_metric::PENDING_CREATING_PGS,
			  daemon_metric::HIGH_DEBUG_LEVEL}) {
    ops.clear();
    auto c = make(t);
    auto cm = summarize(*c);
    EXPECT_TRUE(cm.checks.empty()) << msg(cm, "<empty map>");
  }
}

TEST_F(CollectorOrdering, OrderingBug_D1_BlockedTimeWithoutSlowOpsIsListed)
{
  auto c = make(daemon_metric::SLOW_OPS);
  update(*c, osd(1), slow_metric(0, 9));
  update(*c, osd(2), slow_metric(0, 0));
  auto cm = summarize(*c);
  expect_summary(cm, SLOW_CODE, slow_summary(0, 9, {1}));
}

TEST_F(CollectorOrdering, OrderingBug_D2_SameNameDifferentTypeAreDistinctDaemons)
{
  auto c = make(daemon_metric::SLOW_OPS);
  update(*c, DaemonKey{"osd", "1"}, slow_metric(1, 1));
  update(*c, DaemonKey{"mon", "1"}, slow_metric(2, 1));
  auto cm = summarize(*c);
  expect_summary(cm, SLOW_CODE,
		 "3 slow ops, oldest one blocked for 1 sec, "
		 "daemons [osd.1,mon.1] have slow ops.");
}

TEST_F(CollectorOrdering, OrderingBug_D5_BlockedTimeMaxIsNotSummed)
{
  auto c = make(daemon_metric::SLOW_OPS);
  update(*c, osd(1), slow_metric(1, 0xFFFFFFFFu));
  update(*c, osd(2), slow_metric(1, 0xFFFFFFFFu));
  auto cm = summarize(*c);
  expect_summary(cm, SLOW_CODE, slow_summary(2, 0xFFFFFFFFu, {1, 2}));
}

TEST_F(CollectorOrdering, OrderingBug_D1_UpdateAfterSummarizeIsReflectedInNextSummary)
{
  auto c = make(daemon_metric::PENDING_CREATING_PGS);
  auto empty = summarize(*c);
  EXPECT_TRUE(empty.checks.empty()) << msg(empty, "<empty map>");
  update(*c, osd(1), pgs_metric(2));
  auto one = summarize(*c);
  expect_summary(one, PGS_CODE, pgs_summary(2));
  update(*c, osd(2), pgs_metric(3));
  auto two = summarize(*c);
  expect_summary(two, PGS_CODE, pgs_summary(5));
  const auto* check = find_check(two, PGS_CODE);
  ASSERT_NE(check, nullptr) << msg(two, pgs_summary(5));
  EXPECT_EQ(std::vector<std::string>(check->detail.begin(), check->detail.end()),
	    std::vector<std::string>{pgs_detail({1, 2})})
    << msg(two, "detail [" + pgs_detail({1, 2}) + "]");
}

TEST_F(CollectorOrdering, OrderingBug_D5_RoundTripPreservesPairForEveryType)
{
  const std::vector<DaemonHealthMetric> inputs{
    slow_metric(0xFFFFFFFFu, 0x80000001u),
    DaemonHealthMetric(daemon_metric::PENDING_CREATING_PGS, uint32_t(3), uint32_t(4)),
    debug_metric(12, 20),
    none_metric(0),
  };
  for (const auto& in : inputs) {
    const DaemonHealthMetric out = roundtrip(in);
    EXPECT_TRUE(out.get_type() == in.get_type() && out.get_n() == in.get_n() &&
		out.get_n1() == in.get_n1() && out.get_n2() == in.get_n2())
      << "Sequence: encode;decode\nCurrent: " << out << "\nExpected: " << in;
  }
}

TEST_F(CollectorOrdering, OrderingBug_D5_PendingPGsPairAndWideConstructorsAgree)
{
  const DaemonHealthMetric wide(daemon_metric::PENDING_CREATING_PGS,
				(uint64_t(4) << 32) | uint64_t(3));
  const DaemonHealthMetric pair(daemon_metric::PENDING_CREATING_PGS,
				uint32_t(3), uint32_t(4));
  auto a = make(daemon_metric::PENDING_CREATING_PGS);
  update(*a, osd(1), wide);
  auto cm_wide = summarize(*a);
  ops.clear();
  auto b = make(daemon_metric::PENDING_CREATING_PGS);
  update(*b, osd(1), pair);
  auto cm_pair = summarize(*b);
  expect_summary(cm_wide, PGS_CODE, pgs_summary((uint64_t(4) << 32) | 3));
  EXPECT_EQ(cm_wide, cm_pair)
    << msg(cm_wide, describe(cm_pair) + " (64-bit and pair construction agree)");
}

TEST_F(CollectorOrdering, OrderingBug_D5_UnknownTypeSurvivesRoundTripAndIsIgnored)
{
  const DaemonHealthMetric copy = roundtrip(unknown_metric(5));
  EXPECT_EQ(static_cast<int>(copy.get_type()), 77)
    << "Sequence: encode(type=77);decode\nCurrent: type="
    << static_cast<int>(copy.get_type()) << "\nExpected: type=77";
  for (daemon_metric t : {daemon_metric::SLOW_OPS,
			  daemon_metric::PENDING_CREATING_PGS,
			  daemon_metric::HIGH_DEBUG_LEVEL}) {
    ops.clear();
    auto c = make(t);
    update(*c, osd(1), copy);
    auto cm = summarize(*c);
    EXPECT_TRUE(cm.checks.empty()) << msg(cm, "<empty map>");
  }
}

TEST_F(CollectorOrdering, OrderingBug_D5_DumpAndStreamAgreeWithAccessors)
{
  const DaemonHealthMetric m = slow_metric(5, 7);
  ceph::JSONFormatter f;
  f.open_object_section("");
  m.dump(&f);
  f.close_section();
  std::ostringstream json;
  f.flush(json);
  std::ostringstream text;
  text << m;
  const std::string want_json =
    "{\"type\":\"SLOW_OPS\",\"n\":30064771077,\"n1\":5,\"n2\":7}";
  EXPECT_EQ(json.str(), want_json)
    << "Sequence: dump(slow_metric(5,7))\nCurrent: " << json.str()
    << "\nExpected: " << want_json;
  EXPECT_EQ(text.str(), "SLOW_OPS(30064771077|(5,7))")
    << "Sequence: operator<<(slow_metric(5,7))\nCurrent: " << text.str()
    << "\nExpected: SLOW_OPS(30064771077|(5,7))";
}

TEST_F(CollectorOrdering, OrderingBug_D5_DecodeTruncatedBufferThrows)
{
  ceph::buffer::list bl;
  encode(slow_metric(7, 9), bl);
  ceph::buffer::list cut;
  cut.substr_of(bl, 0, bl.length() - 3);
  auto p = cut.cbegin();
  DaemonHealthMetric out;
  bool threw = false;
  try {
    decode(out, p);
  } catch (const ceph::buffer::error&) {
    threw = true;
  }
  EXPECT_TRUE(threw) << "Sequence: encode;truncate 3 bytes;decode"
		     << "\nCurrent: decode returned " << out
		     << "\nExpected: ceph::buffer::error";
}

TEST_F(CollectorOrdering, OrderingBug_D2_EmptyDaemonKeyIsListed)
{
  auto c = make(daemon_metric::SLOW_OPS);
  update(*c, DaemonKey{"", ""}, slow_metric(1, 1));
  auto cm = summarize(*c);
  expect_summary(cm, SLOW_CODE,
		 "1 slow ops, oldest one blocked for 1 sec, . has slow ops");
}

namespace {

struct WalkOp {
  bool is_summarize = false;
  int daemon = 0;
  int type_sel = 0;
  uint32_t a = 0;
  uint32_t b = 0;
  uint64_t wide = 0;
};

std::string describe_op(const WalkOp& op)
{
  if (op.is_summarize) {
    return "summarize";
  }
  static const char* names[] = {"SLOW_OPS", "PENDING_CREATING_PGS",
				"HIGH_DEBUG_LEVEL", "NONE", "unknown(77)"};
  std::ostringstream os;
  os << "update(osd." << op.daemon << "," << names[op.type_sel] << ",a=" << op.a
     << ",b=" << op.b << ",wide=" << op.wide << ")";
  return os.str();
}

std::string describe_ops(const std::vector<WalkOp>& ops)
{
  std::vector<std::string> v;
  for (const auto& op : ops) {
    v.push_back(describe_op(op));
  }
  return join(v, "; ");
}

DaemonHealthMetric walk_metric(const WalkOp& op)
{
  switch (op.type_sel) {
  case 0:
    return DaemonHealthMetric(daemon_metric::SLOW_OPS, op.a, op.b);
  case 1:
    return DaemonHealthMetric(daemon_metric::PENDING_CREATING_PGS, op.wide);
  case 2:
    return DaemonHealthMetric(daemon_metric::HIGH_DEBUG_LEVEL, op.a, op.b);
  case 3:
    return DaemonHealthMetric(daemon_metric::NONE, op.wide);
  default:
    return DaemonHealthMetric(static_cast<daemon_metric>(77), op.wide);
  }
}

struct WalkModel {
  std::vector<int> slow_ids, pgs_ids, dbg_ids;
  uint64_t slow_total = 0, slow_blocked = 0, pgs_total = 0, dbg_total = 0,
    dbg_max = 0;
  std::set<std::pair<int, int>> seen;

  bool apply(const WalkOp& op)
  {
    if (op.type_sel <= 2 && !seen.insert({op.daemon, op.type_sel}).second) {
      return false;
    }
    if (op.type_sel == 0) {
      slow_total += op.a;
      slow_blocked = std::max<uint64_t>(slow_blocked, op.b);
      if (op.a || op.b) {
	slow_ids.push_back(op.daemon);
      }
    } else if (op.type_sel == 1) {
      pgs_total += op.wide;
      if (op.wide) {
	pgs_ids.push_back(op.daemon);
      }
    } else if (op.type_sel == 2 && op.a) {
      dbg_total += op.a;
      dbg_max = std::max<uint64_t>(dbg_max, op.b);
      dbg_ids.push_back(op.daemon);
    }
    return true;
  }

  health_check_map_t expected() const
  {
    health_check_map_t cm;
    if (!slow_ids.empty()) {
      auto& c = cm.add(SLOW_CODE, HEALTH_WARN,
		       slow_summary(slow_total, slow_blocked, slow_ids), 1);
      (void)c;
    }
    if (!pgs_ids.empty()) {
      auto& c = cm.add(PGS_CODE, HEALTH_WARN, pgs_summary(pgs_total), 1);
      c.detail.push_back(pgs_detail(pgs_ids));
    }
    if (!dbg_ids.empty()) {
      cm.add(DEBUG_CODE, HEALTH_WARN,
	     debug_summary(dbg_total, dbg_max, dbg_ids), 1);
    }
    return cm;
  }
};

std::string replay(const std::vector<WalkOp>& ops, daemon_metric target)
{
  const char* code = target == daemon_metric::SLOW_OPS ? SLOW_CODE :
    target == daemon_metric::PENDING_CREATING_PGS ? PGS_CODE : DEBUG_CODE;
  auto collector = DaemonHealthMetricCollector::create(target);
  WalkModel model;
  std::vector<WalkOp> applied;
  for (const auto& op : ops) {
    if (!op.is_summarize) {
      if (!model.apply(op)) {
	continue;
      }
      collector->update(osd(op.daemon), walk_metric(op));
    }
    applied.push_back(op);
    health_check_map_t cm;
    collector->summarize(cm);
    health_check_map_t expected = model.expected();
    for (auto it = expected.checks.begin(); it != expected.checks.end();) {
      it = it->first == code ? std::next(it) : expected.checks.erase(it);
    }
    if (!(cm == expected)) {
      return "Sequence: " + describe_ops(applied) + "\nCurrent: " +
	describe(cm) + "\nExpected: " + describe(expected);
    }
  }
  return {};
}

std::vector<WalkOp> shrink(std::vector<WalkOp> ops, daemon_metric target)
{
  bool progress = true;
  while (progress) {
    progress = false;
    for (size_t i = 0; i < ops.size(); ++i) {
      std::vector<WalkOp> trial = ops;
      trial.erase(trial.begin() + i);
      if (!replay(trial, target).empty()) {
	ops = std::move(trial);
	progress = true;
	break;
      }
    }
  }
  return ops;
}

void run_walk(daemon_metric target, bool sender_domain)
{
  constexpr uint32_t seed = 0x5eed1234;
  constexpr int iterations = 100000;
  const bool small_counts = sender_domain ||
    target == daemon_metric::HIGH_DEBUG_LEVEL;
  const uint32_t a_vals[] = {0, 1, 2, 100, small_counts ? 93u : 0x80000000u};
  const uint32_t b_vals[] = {0, 1, 5, 1000, 0xFFFFFFFFu};
  const uint64_t w_vals[] = {0, 1, 7, uint64_t(1) << 40, uint64_t(1) << 50};
  std::mt19937 rng(seed);
  auto pick = [&](size_t n) {
    return std::uniform_int_distribution<size_t>(0, n - 1)(rng);
  };
  for (int it = 0; it < iterations; ++it) {
    std::vector<WalkOp> ops(1 + pick(12));
    for (auto& op : ops) {
      op.is_summarize = pick(4) == 0;
      op.daemon = pick(5);
      op.type_sel = pick(5);
      op.a = a_vals[pick(5)];
      op.b = b_vals[pick(5)];
      op.wide = w_vals[pick(5)];
      if (sender_domain && op.type_sel == 2 && op.a == 0) {
	op.b = 0;
      }
    }
    if (!replay(ops, target).empty()) {
      const auto minimal = shrink(ops, target);
      FAIL() << "seed=" << seed << " iteration=" << it << " collector="
	     << daemon_metric_name(target) << " sender_domain=" << sender_domain
	     << "\nMinimal failing sequence: "
	     << describe_ops(minimal) << "\n" << replay(minimal, target);
    }
  }
}

}  // namespace

TEST_F(CollectorOrdering, OrderingBug_Walk_SlowOpsAgainstModel)
{
  run_walk(daemon_metric::SLOW_OPS, false);
}

TEST_F(CollectorOrdering, OrderingBug_Walk_PendingPGsAgainstModel)
{
  run_walk(daemon_metric::PENDING_CREATING_PGS, false);
}

TEST_F(CollectorOrdering, OrderingBug_Walk_HighDebugLevelAgainstModel)
{
  run_walk(daemon_metric::HIGH_DEBUG_LEVEL, false);
}

TEST_F(CollectorOrdering, OrderingBug_Walk_SlowOpsAgainstModel_SenderDomain)
{
  run_walk(daemon_metric::SLOW_OPS, true);
}

TEST_F(CollectorOrdering, OrderingBug_Walk_HighDebugLevelAgainstModel_SenderDomain)
{
  run_walk(daemon_metric::HIGH_DEBUG_LEVEL, true);
}

TEST_F(CollectorOrdering, OrderingBug_D9_IndependentCollectorsAcrossThreads)
{
  constexpr int threads = 4;
  constexpr int iterations = 10000;
  auto run_once = [](int salt) {
    auto slow = DaemonHealthMetricCollector::create(daemon_metric::SLOW_OPS);
    auto pgs = DaemonHealthMetricCollector::create(
      daemon_metric::PENDING_CREATING_PGS);
    auto dbg = DaemonHealthMetricCollector::create(
      daemon_metric::HIGH_DEBUG_LEVEL);
    for (int i = 0; i < 4; ++i) {
      slow->update(osd(i), slow_metric(1 + i + salt % 3, 2 + i));
      pgs->update(osd(i), pgs_metric(3 + i + salt % 5));
      dbg->update(osd(i), debug_metric(1 + i, 10 + i));
    }
    health_check_map_t cm;
    slow->summarize(cm);
    pgs->summarize(cm);
    dbg->summarize(cm);
    return cm;
  };
  std::vector<health_check_map_t> reference;
  for (int salt = 0; salt < 15; ++salt) {
    reference.push_back(run_once(salt));
  }
  std::mutex m;
  std::vector<std::string> failures;
  std::vector<std::thread> pool;
  for (int t = 0; t < threads; ++t) {
    pool.emplace_back([&, t] {
      for (int i = 0; i < iterations; ++i) {
	const int salt = (i + t) % 15;
	const auto cm = run_once(salt);
	if (!(cm == reference[salt])) {
	  std::lock_guard l(m);
	  failures.push_back("thread=" + std::to_string(t) + " iteration=" +
			     std::to_string(i) + "\nCurrent: " + describe(cm) +
			     "\nExpected: " + describe(reference[salt]));
	  return;
	}
      }
    });
  }
  for (auto& th : pool) {
    th.join();
  }
  EXPECT_TRUE(failures.empty())
    << "Sequence: " << threads << " threads x " << iterations
    << " iterations of create/update x4 x3 types/summarize on private collectors\n"
    << (failures.empty() ? "" : failures.front());
}

TEST_F(CollectorOrdering, OrderingBug_D9_ConcurrentSummarizeOnSharedCollectorIsStable)
{
  constexpr int threads = 4;
  constexpr int iterations = 10000;
  auto slow = DaemonHealthMetricCollector::create(daemon_metric::SLOW_OPS);
  auto pgs = DaemonHealthMetricCollector::create(
    daemon_metric::PENDING_CREATING_PGS);
  auto dbg = DaemonHealthMetricCollector::create(
    daemon_metric::HIGH_DEBUG_LEVEL);
  for (int i = 0; i < 12; ++i) {
    slow->update(osd(i), slow_metric(1 + i, 2 + i));
    pgs->update(osd(i), pgs_metric(3 + i));
    dbg->update(osd(i), debug_metric(1 + i, 10 + i));
  }
  health_check_map_t reference;
  slow->summarize(reference);
  pgs->summarize(reference);
  dbg->summarize(reference);
  std::mutex m;
  std::vector<std::string> failures;
  std::vector<std::thread> pool;
  for (int t = 0; t < threads; ++t) {
    pool.emplace_back([&, t] {
      for (int i = 0; i < iterations; ++i) {
	health_check_map_t cm;
	slow->summarize(cm);
	pgs->summarize(cm);
	dbg->summarize(cm);
	if (!(cm == reference)) {
	  std::lock_guard l(m);
	  failures.push_back("thread=" + std::to_string(t) + " iteration=" +
			     std::to_string(i) + "\nCurrent: " + describe(cm) +
			     "\nExpected: " + describe(reference));
	  return;
	}
      }
    });
  }
  for (auto& th : pool) {
    th.join();
  }
  EXPECT_TRUE(failures.empty())
    << "Sequence: 12 updates x3 types on one shared set of collectors; "
    << threads << " threads x " << iterations
    << " iterations of summarize into a private map\n"
    << (failures.empty() ? "" : failures.front());
}
