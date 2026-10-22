// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2026 IBM
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "common/config_proxy.h"
#include "common/perf_counters.h"
#include "global/global_context.h"
#include "global/global_init.h"

#include "mgr/mgr_perf_counters.h"
#include "mgr/MgrMapCache.h"

#include "gtest/gtest.h"

PerfCounters *perfcounter = nullptr;

namespace {

using Cache = LFUCache<int>;

const char* const kA = "osd_map";
const char* const kB = "pg_dump";
const char* const kC = "pg_stats";
const char* const kD = "mon_status";
const char* const kE = "mgr_map";
const char* const kF = "osd_metadata";
const char* const kBad = "not_allowed";

const std::vector<std::string> kAllKeys = {kA, kB, kC, kD, kE, kF, kBad};

std::unique_ptr<Cache> make_cache(size_t cap, bool enabled = true)
{
  return std::make_unique<Cache>(
    std::unordered_set<std::string>{kA, kB, kC, kD, kE, kF}, cap, enabled);
}

struct StartGate {
  std::atomic<bool> go{false};
  void wait() {
    while (!go.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }
  void open() { go.store(true, std::memory_order_release); }
};

class LfuOrdering : public ::testing::Test {
protected:
  std::unique_ptr<Cache> c;
  std::vector<std::string> log;

  void make(size_t cap, bool enabled = true) {
    c = make_cache(cap, enabled);
    log.clear();
    log.push_back("construct(cap=" + std::to_string(cap) + ",enabled=" +
                  (enabled ? "true" : "false") + ")");
  }

  Cache::InsertRes ins(const std::string& k, int v) {
    auto r = c->insert(k, v);
    std::string s = "insert(" + k + "," + std::to_string(v) + ")->";
    s += r.inserted ? "inserted" : "rejected";
    if (r.replaced) s += "+replaced=" + std::to_string(*r.replaced);
    if (r.evicted) s += "+evicted=" + std::to_string(*r.evicted);
    log.push_back(s);
    return r;
  }

  bool tg(const std::string& k, bool count = true, int* out = nullptr) {
    int tmp = -1;
    bool ok = c->try_get(k, out ? out : &tmp, count);
    log.push_back("try_get(" + k + (count ? "" : ",nocount") + ")->" +
                  (ok ? "hit" : "miss"));
    return ok;
  }

  std::optional<int> get(const std::string& k) {
    try {
      int v = c->get(k);
      log.push_back("get(" + k + ")->" + std::to_string(v));
      return v;
    } catch (const std::out_of_range&) {
      log.push_back("get(" + k + ")->throw");
      return std::nullopt;
    }
  }

  bool era(const std::string& k) {
    bool r = c->erase(k);
    log.push_back("erase(" + k + ")->" + (r ? "true" : "false"));
    return r;
  }

  bool ext(const std::string& k, int* out) {
    bool r = c->extract(k, out);
    log.push_back("extract(" + k + ")->" + (r ? "true" : "false"));
    return r;
  }

  void dis() { c->set_enabled(false); log.push_back("set_enabled(false)"); }
  void ena() { c->set_enabled(true); log.push_back("set_enabled(true)"); }
  void clr() { c->clear(); log.push_back("clear()"); }

  std::string seq() const {
    std::string s;
    for (const auto& l : log) {
      if (!s.empty()) s += " ; ";
      s += l;
    }
    return s;
  }

  std::string cur() const {
    std::ostringstream o;
    o << "size=" << c->size() << " hits=" << c->get_hits()
      << " misses=" << c->get_misses()
      << " enabled=" << (c->is_enabled() ? "true" : "false") << " present={";
    bool first = true;
    for (const auto& k : kAllKeys) {
      if (c->exists(k)) {
        o << (first ? "" : ",") << k;
        first = false;
      }
    }
    o << "}";
    return o.str();
  }

  std::string msg(const std::string& expected) const {
    return "\n  Sequence: " + seq() + "\n  Current: " + cur() +
           "\n  Expected: " + expected;
  }
};

TEST_F(LfuOrdering, OrderingBug_M1_Capacity0_FirstInsertExceedsCapacity)
{
  make(0);
  ins(kA, 1);
  EXPECT_LE(c->size(), size_t{0})
    << msg("size() <= capacity (0) after the first insert on an empty cache");
}

TEST_F(LfuOrdering, OrderingBug_M1_Capacity0_RepeatedInserts)
{
  make(0);
  for (int i = 0; i < 4; ++i) {
    ins(kAllKeys[i], i);
    ASSERT_LE(c->size(), size_t{0})
      << msg("size() <= capacity (0) after every insert");
  }
}

TEST_F(LfuOrdering, OrderingBug_M1_Capacity1_SizeNeverExceedsOne)
{
  make(1);
  for (int i = 0; i < 6; ++i) {
    ins(kAllKeys[i], i);
    ASSERT_LE(c->size(), size_t{1})
      << msg("size() <= capacity (1) after every insert");
  }
  EXPECT_TRUE(c->exists(kF))
    << msg("the most recently inserted key is resident after inserting at capacity 1");
}

TEST_F(LfuOrdering, OrderingBug_M1_Capacity1_ReplaceKeepsSingleEntry)
{
  make(1);
  ins(kA, 1);
  auto r = ins(kA, 2);
  EXPECT_TRUE(r.inserted && r.replaced && !r.evicted && c->size() == 1)
    << msg("replace at capacity 1: inserted, replaced=1, no eviction, size 1");
}

TEST_F(LfuOrdering, OrderingBug_M3_Reenable_NoStaleEntries)
{
  make(4);
  ins(kA, 1);
  ins(kB, 2);
  ins(kC, 3);
  get(kA);
  dis();
  ena();
  for (const char* k : {kA, kB, kC}) {
    EXPECT_FALSE(c->exists(k)) << msg(std::string("no entry from before the disable is visible: ") + k);
  }
  EXPECT_EQ(c->size(), size_t{0}) << msg("size() == 0 after disable then enable");
  EXPECT_FALSE(get(kA).has_value()) << msg("get(osd_map) throws after disable then enable");
}

TEST_F(LfuOrdering, OrderingBug_M3_Reenable_CountersRestart)
{
  make(4);
  ins(kA, 1);
  ins(kB, 2);
  get(kA);
  dis();
  ena();
  EXPECT_EQ(c->get_hits(), 0u) << msg("hits == 0 after disable then enable");
  EXPECT_EQ(c->get_misses(), 0u) << msg("misses == 0 after disable then enable");
  ins(kA, 3);
  tg(kA);
  EXPECT_EQ(c->get_hits(), 1u) << msg("hits == 1 (one counted hit since re-enable)");
  EXPECT_EQ(c->get_misses(), 1u) << msg("misses == 1 (one new-key insert since re-enable)");
}

TEST_F(LfuOrdering, OrderingBug_M3_Reenable_HitCountsDoNotCarryOver)
{
  make(2);
  ins(kA, 1);
  for (int i = 0; i < 5; ++i) get(kA);
  dis();
  ena();
  ins(kA, 3);
  ins(kB, 4);
  get(kB);
  auto r = ins(kC, 5);
  ASSERT_TRUE(r.evicted.has_value()) << msg("an insert at capacity evicts one entry");
  EXPECT_EQ(*r.evicted, 3)
    << msg("evicted value 3 (re-inserted osd_map has 0 hits, pg_dump has 1)");
}

TEST_F(LfuOrdering, OrderingBug_M3_EnableWhenEnabled_KeepsEntries)
{
  make(4);
  ins(kA, 1);
  ena();
  EXPECT_TRUE(c->exists(kA))
    << msg("set_enabled(true) on an enabled cache does not flush entries");
  EXPECT_EQ(c->get_misses(), 1u) << msg("misses unchanged by set_enabled(true)");
}

TEST_F(LfuOrdering, OrderingBug_M3_DoubleDisable_Idempotent)
{
  make(4);
  ins(kA, 1);
  dis();
  dis();
  EXPECT_EQ(c->size(), size_t{0}) << msg("size() == 0");
  EXPECT_FALSE(c->is_enabled()) << msg("still disabled");
}

TEST_F(LfuOrdering, OrderingBug_M3_StartDisabled_RejectsThenAcceptsAfterEnable)
{
  make(4, false);
  auto r1 = ins(kA, 1);
  EXPECT_FALSE(r1.inserted) << msg("insert on a disabled cache is rejected");
  EXPECT_EQ(c->get_misses(), 0u) << msg("a rejected insert is not counted as a miss");
  ena();
  auto r2 = ins(kA, 1);
  EXPECT_TRUE(r2.inserted) << msg("insert after enable is accepted");
  EXPECT_EQ(c->get_misses(), 1u) << msg("misses == 1");
}

TEST_F(LfuOrdering, OrderingBug_M4_UncountedTryGet_ChangesNoCounter)
{
  make(3);
  ins(kA, 1);
  tg(kA, false);
  tg(kA, false);
  tg(kB, false);
  EXPECT_EQ(c->get_hits(), 0u) << msg("hits == 0 (count_hit=false)");
  EXPECT_EQ(c->get_misses(), 1u) << msg("misses == 1 (only the insert)");
}

TEST_F(LfuOrdering, OrderingBug_M4_UncountedHit_DoesNotProtectFromEviction)
{
  make(2);
  ins(kA, 1);
  ins(kB, 2);
  for (int i = 0; i < 5; ++i) tg(kA, false);
  tg(kB, true);
  auto r = ins(kC, 3);
  ASSERT_TRUE(r.evicted.has_value()) << msg("insert at capacity evicts");
  EXPECT_EQ(*r.evicted, 1)
    << msg("osd_map (value 1) evicted: uncounted lookups add no per-entry hits");
}

TEST_F(LfuOrdering, OrderingBug_M4_DisabledMissesCleared_ByNextClear)
{
  make(3);
  dis();
  tg(kA);
  ena();
  clr();
  EXPECT_EQ(c->get_hits() + c->get_misses(), 0u)
    << msg("clear() resets accounting accrued while disabled");
}

TEST_F(LfuOrdering, OrderingBug_M4_MixedLookups_ExactTotals)
{
  make(4);
  ins(kA, 1);
  ins(kB, 2);
  tg(kA);
  get(kB);
  tg(kC);
  tg(kA, false);
  EXPECT_EQ(c->get_hits(), 2u) << msg("hits == 2");
  EXPECT_EQ(c->get_misses(), 3u) << msg("misses == 3 (two inserts, one try_get miss)");
}

TEST_F(LfuOrdering, OrderingBug_M5_ExactFill_NoEviction_ThenEvictsMinimum)
{
  make(3);
  auto r1 = ins(kA, 1);
  auto r2 = ins(kB, 2);
  auto r3 = ins(kC, 3);
  EXPECT_FALSE(r1.evicted || r2.evicted || r3.evicted)
    << msg("no eviction while size < capacity");
  for (int i = 0; i < 3; ++i) get(kA);
  get(kB);
  auto r4 = ins(kD, 4);
  ASSERT_TRUE(r4.evicted.has_value()) << msg("insert at capacity evicts");
  EXPECT_EQ(*r4.evicted, 3) << msg("pg_stats (0 hits) evicted");
  EXPECT_EQ(c->size(), size_t{3}) << msg("size == capacity");
}

TEST_F(LfuOrdering, OrderingBug_M5_RepeatedInserts_AlwaysEvictMinimumHit)
{
  make(3);
  ins(kA, 1);
  ins(kB, 2);
  ins(kC, 3);
  for (int i = 0; i < 4; ++i) get(kA);
  for (int i = 0; i < 2; ++i) get(kB);
  int prev = 3;
  for (int i = 0; i < 6; ++i) {
    const char* k = (i % 2) ? kE : kD;
    int v = 10 + i;
    auto r = ins(k, v);
    ASSERT_TRUE(r.evicted.has_value()) << msg("insert at capacity evicts");
    ASSERT_EQ(*r.evicted, prev)
      << msg("evicted value " + std::to_string(prev) + " (the only entry with 0 hits)");
    prev = v;
  }
  EXPECT_TRUE(c->exists(kA) && c->exists(kB))
    << msg("entries with hits survive repeated inserts of new keys");
}

TEST_F(LfuOrdering, OrderingBug_M5_AllTied_EvictsResidentNotNewcomer)
{
  make(3);
  ins(kA, 1);
  ins(kB, 2);
  ins(kC, 3);
  auto r = ins(kD, 4);
  ASSERT_TRUE(r.evicted.has_value()) << msg("insert at capacity evicts");
  EXPECT_TRUE(*r.evicted >= 1 && *r.evicted <= 3)
    << msg("evicted value is one of the residents 1..3");
  EXPECT_TRUE(c->exists(kD)) << msg("the newcomer mon_status is resident");
  EXPECT_EQ(c->size(), size_t{3}) << msg("size == capacity");
}

TEST_F(LfuOrdering, OrderingBug_M6_ReplaceAtCapacity_NoEviction)
{
  make(2);
  ins(kA, 1);
  ins(kB, 2);
  auto r = ins(kA, 10);
  EXPECT_TRUE(r.inserted) << msg("inserted == true");
  ASSERT_TRUE(r.replaced.has_value()) << msg("replaced == 1");
  EXPECT_EQ(*r.replaced, 1) << msg("replaced == 1");
  EXPECT_FALSE(r.evicted.has_value()) << msg("evicted unset");
  EXPECT_EQ(c->size(), size_t{2}) << msg("size unchanged == 2");
  EXPECT_EQ(c->get_misses(), 2u) << msg("misses == 2: a replace is not a new-key miss");
  int v = -1;
  EXPECT_TRUE(tg(kA, false, &v) && v == 10) << msg("osd_map now holds 10");
  EXPECT_TRUE(c->exists(kB)) << msg("pg_dump untouched");
}

TEST_F(LfuOrdering, OrderingBug_M6_ReplaceKeepsEntryHitCount)
{
  make(2);
  ins(kA, 1);
  ins(kB, 2);
  for (int i = 0; i < 3; ++i) get(kA);
  ins(kA, 9);
  auto r = ins(kC, 3);
  ASSERT_TRUE(r.evicted.has_value()) << msg("insert at capacity evicts");
  EXPECT_EQ(*r.evicted, 2) << msg("pg_dump (0 hits) evicted, replaced osd_map keeps its 3 hits");
}

TEST_F(LfuOrdering, OrderingBug_M6_ReplaceWhenDisabled_Rejected)
{
  make(2);
  ins(kA, 1);
  dis();
  auto r = ins(kA, 2);
  EXPECT_FALSE(r.inserted || r.replaced || r.evicted)
    << msg("insert on a disabled cache: inserted=false, replaced and evicted unset");
  EXPECT_EQ(c->size(), size_t{0}) << msg("size == 0");
}

TEST_F(LfuOrdering, OrderingBug_M7_AbsentAndDisallowedKeys_NoOps)
{
  make(3);
  ins(kA, 1);
  int out = -77;
  EXPECT_FALSE(ext(kB, &out)) << msg("extract of an absent key returns false");
  EXPECT_EQ(out, -77) << msg("extract miss leaves *out unchanged");
  EXPECT_FALSE(era(kB)) << msg("erase of an absent key returns false");
  EXPECT_FALSE(era(kBad)) << msg("erase of a non-allowed key returns false");
  EXPECT_FALSE(ext(kBad, &out)) << msg("extract of a non-allowed key returns false");
  EXPECT_EQ(c->size(), size_t{1}) << msg("size == 1");
  EXPECT_EQ(c->get_hits(), 0u) << msg("hits == 0");
  EXPECT_EQ(c->get_misses(), 1u) << msg("misses == 1");
}

TEST_F(LfuOrdering, OrderingBug_M7_ExtractThenExtract)
{
  make(3);
  ins(kA, 1);
  int out = -1;
  EXPECT_TRUE(ext(kA, &out) && out == 1) << msg("first extract returns 1");
  out = -77;
  EXPECT_FALSE(ext(kA, &out)) << msg("second extract returns false");
  EXPECT_EQ(out, -77) << msg("second extract leaves *out unchanged");
  EXPECT_EQ(c->size(), size_t{0}) << msg("size == 0");
  EXPECT_EQ(c->get_hits(), 0u) << msg("extract is not a counted hit");
}

TEST_F(LfuOrdering, OrderingBug_M7_EraseThenReinsert_IsNewKey)
{
  make(3);
  ins(kA, 1);
  era(kA);
  auto r = ins(kA, 2);
  EXPECT_TRUE(r.inserted && !r.replaced && !r.evicted)
    << msg("reinsert after erase is a new key: no replaced, no evicted");
  EXPECT_EQ(c->get_misses(), 2u) << msg("misses == 2 (two new-key inserts)");
}

TEST_F(LfuOrdering, OrderingBug_M7_EraseDoesNotResetCounters)
{
  make(3);
  ins(kA, 1);
  get(kA);
  era(kA);
  EXPECT_EQ(c->get_hits(), 1u) << msg("hits == 1 after erase");
  EXPECT_EQ(c->get_misses(), 1u) << msg("misses == 1 after erase");
}

TEST_F(LfuOrdering, OrderingBug_M7_ClearResetsEntriesAndCounters)
{
  make(3);
  ins(kA, 1);
  ins(kB, 2);
  get(kA);
  clr();
  EXPECT_EQ(c->size(), size_t{0}) << msg("size == 0");
  EXPECT_EQ(c->get_hits() + c->get_misses(), 0u) << msg("hits and misses reset to 0");
  EXPECT_TRUE(c->is_enabled()) << msg("clear() does not disable");
  clr();
  EXPECT_EQ(c->size(), size_t{0}) << msg("clear() on an empty cache is a no-op");
}

TEST_F(LfuOrdering, OrderingBug_M7_Drain_EmptiesAndResets)
{
  make(4);
  ins(kA, 1);
  ins(kB, 2);
  ins(kC, 3);
  get(kA);
  std::vector<int> out;
  c->drain(out);
  log.push_back("drain()");
  std::sort(out.begin(), out.end());
  EXPECT_EQ(out, (std::vector<int>{1, 2, 3})) << msg("drain returns every value");
  EXPECT_EQ(c->size(), size_t{0}) << msg("size == 0");
  EXPECT_EQ(c->get_hits() + c->get_misses(), 0u) << msg("counters reset");
  std::vector<int> again;
  c->drain(again);
  EXPECT_TRUE(again.empty()) << msg("drain of an empty cache returns nothing");
}

TEST_F(LfuOrdering, OrderingBug_M7_AllAllowedKeys_FitExactly)
{
  make(6);
  for (int i = 0; i < 6; ++i) {
    auto r = ins(kAllKeys[i], i);
    ASSERT_TRUE(r.inserted && !r.evicted) << msg("no eviction while size < capacity");
  }
  EXPECT_EQ(c->size(), size_t{6}) << msg("size == 6");
  auto r = ins(kBad, 99);
  EXPECT_FALSE(r.inserted) << msg("non-allowed key rejected at full capacity");
  EXPECT_EQ(c->size(), size_t{6}) << msg("size unchanged");
}


TEST_F(LfuOrdering, OrderingBug_M7_EmptyAndNearMissKeys_Rejected)
{
  make(4);
  for (const char* k : {"", "osd_map_", "_osd_map", "OSD_MAP", "osd_ma", "osd_map "}) {
    auto r = ins(k, 1);
    EXPECT_FALSE(r.inserted) << msg(std::string("insert of non-allowed key '") + k + "' is rejected");
    EXPECT_FALSE(c->is_cacheable(k)) << msg(std::string("'") + k + "' is not cacheable");
    EXPECT_FALSE(era(k)) << msg(std::string("erase of '") + k + "' returns false");
  }
  EXPECT_EQ(c->size(), size_t{0}) << msg("size == 0");
  EXPECT_EQ(c->get_misses(), 0u) << msg("rejected inserts are not counted");
}

TEST_F(LfuOrdering, OrderingBug_M7_EmptyAllowList_RejectsEverything)
{
  Cache empty_allow{std::unordered_set<std::string>{}, 4};
  auto r = empty_allow.insert(kA, 1);
  EXPECT_FALSE(r.inserted) << "\n  Sequence: construct(allowed={},cap=4) ; insert(osd_map,1)"
                           << "\n  Current: inserted=" << r.inserted << " size=" << empty_allow.size()
                           << "\n  Expected: inserted=false, size=0";
  EXPECT_EQ(empty_allow.size(), size_t{0}) << "size == 0";
}

TEST_F(LfuOrdering, OrderingBug_M7_TryGetMiss_LeavesOutUntouched)
{
  make(4);
  ins(kA, 1);
  int out = -77;
  EXPECT_FALSE(tg(kB, true, &out)) << msg("miss");
  EXPECT_EQ(out, -77) << msg("try_get miss leaves *out unchanged");
  EXPECT_FALSE(tg(kBad, false, &out)) << msg("miss on non-allowed key");
  EXPECT_EQ(out, -77) << msg("try_get miss leaves *out unchanged");
}

TEST_F(LfuOrdering, OrderingBug_M8_EraseFreesSlot_NextInsertDoesNotEvict)
{
  make(2);
  ins(kA, 1);
  ins(kB, 2);
  era(kA);
  auto r = ins(kC, 3);
  EXPECT_FALSE(r.evicted.has_value()) << msg("no eviction after erase freed a slot");
  EXPECT_TRUE(c->exists(kB) && c->exists(kC)) << msg("pg_dump and pg_stats resident");
  EXPECT_EQ(c->size(), size_t{2}) << msg("size == 2");
}

TEST_F(LfuOrdering, OrderingBug_M8_ExtractFreesSlot_NextInsertDoesNotEvict)
{
  make(2);
  ins(kA, 1);
  ins(kB, 2);
  int o;
  ext(kB, &o);
  auto r = ins(kC, 3);
  EXPECT_FALSE(r.evicted.has_value()) << msg("no eviction after extract freed a slot");
  EXPECT_EQ(c->size(), size_t{2}) << msg("size == 2");
}

TEST_F(LfuOrdering, OrderingBug_M8_ClearThenRefill_FreshHitCounts)
{
  make(2);
  ins(kA, 1);
  for (int i = 0; i < 9; ++i) get(kA);
  ins(kB, 2);
  clr();
  ins(kB, 4);
  ins(kA, 3);
  get(kB);
  auto r = ins(kC, 5);
  ASSERT_TRUE(r.evicted.has_value()) << msg("insert at capacity evicts");
  EXPECT_EQ(*r.evicted, 3) << msg("osd_map (0 hits since clear) evicted, not pg_dump (1 hit)");
}

TEST_F(LfuOrdering, OrderingBug_M8_DisableMidFill_ThenReenableFillsToCapacity)
{
  make(2);
  ins(kA, 1);
  dis();
  ena();
  ins(kB, 2);
  auto r = ins(kC, 3);
  EXPECT_FALSE(r.evicted.has_value()) << msg("capacity 2 holds two entries after disable/enable: no eviction for the second insert");
  EXPECT_EQ(c->size(), size_t{2}) << msg("size == 2");
}

TEST_F(LfuOrdering, OrderingBug_M10_CanReadCanWrite_EachFalseConditionIndependently)
{
  make(4);
  EXPECT_TRUE(c->can_write_cache(kA)) << msg("enabled and allowed: writable");
  EXPECT_FALSE(c->can_read_cache(kA)) << msg("allowed but absent: not readable");
  EXPECT_FALSE(c->can_write_cache(kBad)) << msg("non-allowed: not writable");
  ins(kA, 1);
  EXPECT_TRUE(c->can_read_cache(kA)) << msg("enabled, allowed, present: readable");
  EXPECT_FALSE(c->can_read_cache(kB)) << msg("absent: not readable");
  dis();
  EXPECT_FALSE(c->can_write_cache(kA)) << msg("disabled: not writable");
  EXPECT_FALSE(c->can_read_cache(kA)) << msg("disabled: not readable");
  ena();
  EXPECT_FALSE(c->can_read_cache(kA)) << msg("re-enabled after a flush: not readable");
  EXPECT_TRUE(c->can_write_cache(kA)) << msg("re-enabled: writable");
}

struct ScopedPerfCounters {
  PerfCounters* pc;
  ScopedPerfCounters() {
    PerfCountersBuilder plb(g_ceph_context, "mgr_ordering_test", l_mgr_first, l_mgr_last);
    plb.add_u64_counter(l_mgr_cache_hit, "cache_hit", "Cache hits");
    plb.add_u64_counter(l_mgr_cache_miss, "cache_miss", "Cache miss");
    pc = plb.create_perf_counters();
    perfcounter = pc;
  }
  ~ScopedPerfCounters() {
    perfcounter = nullptr;
    delete pc;
  }
};

TEST_F(LfuOrdering, OrderingBug_M11_PerfCounters_MirrorCountedLookupsAcrossClear)
{
  ScopedPerfCounters perf;
  make(4);
  ins(kA, 1);
  get(kA);
  tg(kA);
  tg(kB);
  tg(kA, false);
  clr();
  ins(kA, 2);
  get(kA);
  EXPECT_EQ(perf.pc->get(l_mgr_cache_hit), 3u)
    << msg("perf cache_hit == 3 counted hits, monotonic across clear()");
  EXPECT_EQ(perf.pc->get(l_mgr_cache_miss), 3u)
    << msg("perf cache_miss == 3 (two new-key inserts and one try_get miss), monotonic across clear()");
  EXPECT_EQ(c->get_hits(), 1u) << msg("internal hits restarted by clear(): 1");
  EXPECT_EQ(c->get_misses(), 1u) << msg("internal misses restarted by clear(): 1");
}

TEST_F(LfuOrdering, OrderingBug_M9_HugeCapacity_NeverEvicts)
{
  make(SIZE_MAX);
  for (int i = 0; i < 6; ++i) {
    auto r = ins(kAllKeys[i], i);
    ASSERT_TRUE(r.inserted && !r.evicted) << msg("no eviction with capacity SIZE_MAX");
  }
  EXPECT_EQ(c->size(), size_t{6}) << msg("size == 6");
}

constexpr int kConcurrentIterations = 10000;

TEST(LfuConcurrentOrdering, OrderingBug_M2_DisableVsInsert_NoInsertAfterDisableCompletes)
{
  std::atomic<long> late_accept{0};
  std::atomic<long> first_bad_iter{-1};
  for (int iter = 0; iter < kConcurrentIterations; ++iter) {
    auto c = make_cache(8);
    std::atomic<bool> disable_done{false};
    StartGate gate;
    std::vector<std::thread> ts;
    for (int t = 0; t < 3; ++t) {
      ts.emplace_back([&, t] {
        gate.wait();
        for (int i = 0; i < 6; ++i) {
          const bool after = disable_done.load(std::memory_order_acquire);
          auto r = c->insert(kAllKeys[(t + i) % 6], t * 100 + i);
          if (after && r.inserted) {
            late_accept.fetch_add(1);
            long exp = -1;
            first_bad_iter.compare_exchange_strong(exp, iter);
          }
        }
      });
    }
    std::thread d([&] {
      gate.wait();
      c->set_enabled(false);
      disable_done.store(true, std::memory_order_release);
    });
    gate.open();
    for (auto& t : ts) t.join();
    d.join();
    ASSERT_EQ(c->size(), size_t{0})
      << "\n  Sequence: iteration " << iter
      << ": 3 threads x insert(6 allowed keys) concurrent with set_enabled(false); all joined"
      << "\n  Current: size=" << c->size() << " enabled=" << c->is_enabled()
      << "\n  Expected: size()==0 once set_enabled(false) and all inserts have returned";
  }
  EXPECT_EQ(late_accept.load(), 0)
    << "\n  Sequence: " << kConcurrentIterations
    << " iterations of 3 x insert concurrent with set_enabled(false)"
    << "\n  Current: " << late_accept.load()
    << " inserts reported inserted=true after set_enabled(false) had returned (first at iteration "
    << first_bad_iter.load() << ")"
    << "\n  Expected: 0";
}

TEST(LfuConcurrentOrdering, OrderingBug_C4_ToggleVsInsert_DisabledImpliesEmpty)
{
  for (int iter = 0; iter < kConcurrentIterations; ++iter) {
    auto c = make_cache(8);
    StartGate gate;
    std::vector<std::thread> ts;
    for (int t = 0; t < 2; ++t) {
      ts.emplace_back([&, t] {
        gate.wait();
        for (int i = 0; i < 8; ++i) {
          c->insert(kAllKeys[(t * 3 + i) % 6], i);
        }
      });
    }
    std::thread toggler([&] {
      gate.wait();
      c->set_enabled(false);
      c->set_enabled(true);
      c->set_enabled(false);
    });
    gate.open();
    for (auto& t : ts) t.join();
    toggler.join();
    ASSERT_EQ(c->size(), size_t{0})
      << "\n  Sequence: iteration " << iter
      << ": 2 threads x insert concurrent with set_enabled(false), set_enabled(true), set_enabled(false); all joined"
      << "\n  Current: size=" << c->size() << " enabled=" << c->is_enabled()
      << "\n  Expected: disabled implies size()==0";
  }
}

TEST(LfuConcurrentOrdering, OrderingBug_C2_ConcurrentInsertEraseExtract_SizeBounded)
{
  constexpr size_t cap = 3;
  for (int iter = 0; iter < kConcurrentIterations; ++iter) {
    auto c = make_cache(cap);
    StartGate gate;
    std::atomic<bool> done{false};
    std::atomic<size_t> max_seen{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 3; ++t) {
      ts.emplace_back([&, t] {
        gate.wait();
        for (int i = 0; i < 10; ++i) {
          const std::string k = kAllKeys[(t + i) % 6];
          switch ((t + i) % 4) {
          case 0: case 1: c->insert(k, i); break;
          case 2: c->erase(k); break;
          default: { int o; c->extract(k, &o); }
          }
        }
      });
    }
    std::thread mon([&] {
      gate.wait();
      while (!done.load(std::memory_order_acquire)) {
        size_t s = c->size();
        size_t m = max_seen.load();
        while (s > m && !max_seen.compare_exchange_weak(m, s)) {}
      }
    });
    gate.open();
    for (auto& t : ts) t.join();
    done.store(true, std::memory_order_release);
    mon.join();
    ASSERT_LE(max_seen.load(), cap)
      << "\n  Sequence: iteration " << iter
      << ": 3 threads x {insert, insert, erase, extract} over 6 keys, capacity 3, size() polled"
      << "\n  Current: max observed size=" << max_seen.load()
      << "\n  Expected: size() <= " << cap << " at every observation";
    ASSERT_LE(c->size(), cap) << "final size exceeds capacity " << cap;
  }
}

TEST(LfuConcurrentOrdering, OrderingBug_C3_ConcurrentLookups_CountersMatchOutcomes)
{
  for (int iter = 0; iter < kConcurrentIterations; ++iter) {
    auto c = make_cache(8);
    c->insert(kA, 1);
    std::atomic<uint64_t> seen_hits{0}, seen_misses{0};
    StartGate gate;
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t) {
      ts.emplace_back([&, t] {
        gate.wait();
        for (int i = 0; i < 10; ++i) {
          int v;
          const bool present = (i + t) % 2 == 0;
          if (c->try_get(present ? kA : kB, &v, true)) {
            seen_hits++;
          } else {
            seen_misses++;
          }
        }
      });
    }
    gate.open();
    for (auto& t : ts) t.join();
    ASSERT_EQ(c->get_hits(), seen_hits.load())
      << "\n  Sequence: iteration " << iter << ": insert(osd_map); 4 threads x 10 counted try_get over present/absent keys"
      << "\n  Current: hits=" << c->get_hits() << " observed hit results=" << seen_hits.load()
      << "\n  Expected: equal";
    ASSERT_EQ(c->get_misses(), seen_misses.load() + 1)
      << "\n  Sequence: iteration " << iter << ": insert(osd_map); 4 threads x 10 counted try_get over present/absent keys"
      << "\n  Current: misses=" << c->get_misses() << " observed miss results + 1 insert=" << seen_misses.load() + 1
      << "\n  Expected: equal";
  }
}

TEST(LfuConcurrentOrdering, OrderingBug_C5_ClearVsLookups_NoCounterAfterQuiesce)
{
  for (int iter = 0; iter < kConcurrentIterations; ++iter) {
    auto c = make_cache(8);
    c->insert(kA, 1);
    StartGate gate;
    std::vector<std::thread> ts;
    for (int t = 0; t < 2; ++t) {
      ts.emplace_back([&] {
        gate.wait();
        for (int i = 0; i < 6; ++i) {
          int v;
          c->try_get(kA, &v, true);
        }
      });
    }
    std::thread cl([&] { gate.wait(); c->clear(); });
    gate.open();
    for (auto& t : ts) t.join();
    cl.join();
    const uint64_t total = c->get_hits() + c->get_misses();
    ASSERT_LE(total, 12u)
      << "\n  Sequence: iteration " << iter << ": insert(osd_map); 2 threads x 6 counted try_get concurrent with clear()"
      << "\n  Current: hits+misses=" << total
      << "\n  Expected: at most the 12 counted lookups (clear resets counters, never inflates them)";
  }
}

TEST(LfuConcurrentOrdering, OrderingBug_C8_ReadersVsEvictingWriters_ValuesMatchKeys)
{
  constexpr size_t cap = 2;
  for (int iter = 0; iter < kConcurrentIterations; ++iter) {
    auto c = make_cache(cap);
    StartGate gate;
    std::atomic<int> torn{0};
    std::atomic<int> torn_key{-1};
    std::atomic<int> torn_val{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 2; ++t) {
      ts.emplace_back([&, t] {
        gate.wait();
        for (int i = 0; i < 8; ++i) {
          const int ki = (t * 2 + i) % 6;
          c->insert(kAllKeys[ki], ki * 1000 + i);
        }
      });
    }
    for (int t = 0; t < 2; ++t) {
      ts.emplace_back([&] {
        gate.wait();
        for (int i = 0; i < 16; ++i) {
          const int ki = i % 6;
          int v = -1;
          if (c->try_get(kAllKeys[ki], &v, true) && v / 1000 != ki) {
            torn++;
            torn_key.store(ki);
            torn_val.store(v);
          }
        }
      });
    }
    gate.open();
    for (auto& t : ts) t.join();
    ASSERT_EQ(torn.load(), 0)
      << "\n  Sequence: iteration " << iter << ": 2 threads x insert(key i, value i*1000+n) with capacity 2 against 2 threads x counted try_get"
      << "\n  Current: try_get(" << kAllKeys[torn_key.load() < 0 ? 0 : torn_key.load()] << ") returned " << torn_val.load()
      << "\n  Expected: a returned value always belongs to the key that was looked up";
    ASSERT_LE(c->size(), cap) << "size exceeds capacity " << cap;
  }
}

TEST(LfuConcurrentOrdering, OrderingBug_C10_ThrowingGetVsEraseAndClear_HitsMatchSuccesses)
{
  for (int iter = 0; iter < kConcurrentIterations; ++iter) {
    auto c = make_cache(8);
    c->insert(kA, 1);
    std::atomic<uint64_t> successes{0};
    std::atomic<int> wrong_value{0};
    StartGate gate;
    std::vector<std::thread> ts;
    for (int t = 0; t < 3; ++t) {
      ts.emplace_back([&] {
        gate.wait();
        for (int i = 0; i < 8; ++i) {
          try {
            if (c->get(kA) != 1) wrong_value++;
            successes++;
          } catch (const std::out_of_range&) {
          }
        }
      });
    }
    ts.emplace_back([&] {
      gate.wait();
      c->erase(kA);
      c->insert(kA, 1);
    });
    gate.open();
    for (auto& t : ts) t.join();
    ASSERT_EQ(wrong_value.load(), 0) << "get(osd_map) returned a value other than 1";
    ASSERT_LE(c->get_hits(), successes.load())
      << "\n  Sequence: iteration " << iter << ": insert(osd_map,1); 3 threads x 8 get(osd_map) (throw on miss) against erase(osd_map) ; insert(osd_map,1)"
      << "\n  Current: hits=" << c->get_hits() << " successful get() calls=" << successes.load()
      << "\n  Expected: hits <= successful get() calls (a thrown get() never counts a hit)";
  }
}

struct Op {
  enum Kind { Insert, Get, TryGetCount, TryGetNoCount, Erase, Extract, Enable, Disable, Clear };
  Kind kind;
  int key;
};

const char* kind_name(Op::Kind k)
{
  switch (k) {
  case Op::Insert: return "insert";
  case Op::Get: return "get";
  case Op::TryGetCount: return "try_get";
  case Op::TryGetNoCount: return "try_get_nocount";
  case Op::Erase: return "erase";
  case Op::Extract: return "extract";
  case Op::Enable: return "set_enabled(true)";
  case Op::Disable: return "set_enabled(false)";
  default: return "clear";
  }
}

std::string seq_str(const std::vector<Op>& ops, size_t upto)
{
  std::string s;
  for (size_t i = 0; i < upto && i < ops.size(); ++i) {
    if (i) s += " ; ";
    s += kind_name(ops[i].kind);
    if (ops[i].kind <= Op::Extract) s += "(" + kAllKeys[ops[i].key] + ")";
  }
  return s;
}

struct Model {
  struct E { int val; size_t hits; };
  std::map<std::string, E> data;
  bool enabled = true;
  uint64_t hits = 0, misses = 0;
  size_t cap;
};

std::optional<std::string> check_all(const Cache& c, const Model& m)
{
  std::ostringstream why;
  if (c.size() > m.cap) {
    why << "size " << c.size() << " > capacity " << m.cap;
    return why.str();
  }
  if (!c.is_enabled() && c.size() != 0) {
    why << "disabled but size " << c.size();
    return why.str();
  }
  for (const auto& k : kAllKeys) {
    const bool present = c.exists(k);
    if (present && k == kBad) return std::string("non-allowed key present");
    if (present != (m.data.count(k) != 0)) {
      why << "presence of " << k << " is " << present << ", model says " << !present;
      return why.str();
    }
  }
  if (c.get_hits() != m.hits || c.get_misses() != m.misses) {
    why << "hits/misses " << c.get_hits() << "/" << c.get_misses()
        << " model " << m.hits << "/" << m.misses;
    return why.str();
  }
  if (c.is_enabled() != m.enabled) return std::string("enabled flag differs");
  return std::nullopt;
}

std::optional<std::pair<size_t, std::string>>
replay(const std::vector<Op>& ops, size_t cap)
{
  auto c = make_cache(cap);
  Model m;
  m.cap = cap;
  int next_val = 1;
  for (size_t i = 0; i < ops.size(); ++i) {
    const Op& op = ops[i];
    const std::string& k = kAllKeys[op.key];
    const bool allowed = k != kBad;
    std::ostringstream why;
    switch (op.kind) {
    case Op::Insert: {
      const int v = next_val++;
      auto r = c->insert(k, v);
      const bool expect_ok = m.enabled && allowed;
      if (r.inserted != expect_ok) {
        why << "insert inserted=" << r.inserted << " expected " << expect_ok;
        return std::make_pair(i, why.str());
      }
      if (!expect_ok) break;
      auto it = m.data.find(k);
      if (it != m.data.end()) {
        if (!r.replaced || *r.replaced != it->second.val || r.evicted) {
          why << "replace result wrong";
          return std::make_pair(i, why.str());
        }
        it->second.val = v;
        break;
      }
      m.misses++;
      if (r.replaced) return std::make_pair(i, std::string("replaced set for a new key"));
      if (m.data.size() >= m.cap && !m.data.empty()) {
        if (!r.evicted) return std::make_pair(i, std::string("no eviction at capacity"));
        auto vic = m.data.end();
        size_t minh = SIZE_MAX;
        for (auto j = m.data.begin(); j != m.data.end(); ++j) {
          minh = std::min(minh, j->second.hits);
          if (j->second.val == *r.evicted) vic = j;
        }
        if (vic == m.data.end() || vic->second.hits != minh) {
          why << "evicted value " << *r.evicted << " is not a minimum-hit entry (min hits " << minh << ")";
          return std::make_pair(i, why.str());
        }
        m.data.erase(vic);
      } else if (r.evicted) {
        return std::make_pair(i, std::string("eviction below capacity"));
      }
      m.data[k] = {v, 0};
      break;
    }
    case Op::Get: {
      bool threw = false;
      int v = 0;
      try { v = c->get(k); } catch (const std::out_of_range&) { threw = true; }
      auto it = m.data.find(k);
      if (!m.enabled) {
        if (!threw) return std::make_pair(i, std::string("get succeeded while disabled"));
      } else if (it == m.data.end()) {
        if (!threw) return std::make_pair(i, std::string("get of absent key did not throw"));
      } else {
        if (threw || v != it->second.val) return std::make_pair(i, std::string("get hit wrong"));
        it->second.hits++;
        m.hits++;
      }
      break;
    }
    case Op::TryGetCount:
    case Op::TryGetNoCount: {
      const bool count = op.kind == Op::TryGetCount;
      int v = -1;
      const bool ok = c->try_get(k, &v, count);
      auto it = m.data.find(k);
      if (ok != (it != m.data.end())) return std::make_pair(i, std::string("try_get result wrong"));
      if (ok && v != it->second.val) return std::make_pair(i, std::string("try_get value wrong"));
      if (count) {
        if (ok) { it->second.hits++; m.hits++; } else { m.misses++; }
      }
      break;
    }
    case Op::Erase: {
      const bool ok = c->erase(k);
      if (ok != (m.data.erase(k) != 0)) return std::make_pair(i, std::string("erase result wrong"));
      break;
    }
    case Op::Extract: {
      int v = -1;
      const bool ok = c->extract(k, &v);
      auto it = m.data.find(k);
      if (ok != (it != m.data.end())) return std::make_pair(i, std::string("extract result wrong"));
      if (ok) {
        if (v != it->second.val) return std::make_pair(i, std::string("extract value wrong"));
        m.data.erase(it);
      }
      break;
    }
    case Op::Enable:
      c->set_enabled(true);
      m.enabled = true;
      break;
    case Op::Disable:
      c->set_enabled(false);
      m.enabled = false;
      m.data.clear();
      m.hits = m.misses = 0;
      break;
    case Op::Clear:
      c->clear();
      m.data.clear();
      m.hits = m.misses = 0;
      break;
    }
    if (auto bad = check_all(*c, m)) return std::make_pair(i, *bad);
  }
  return std::nullopt;
}

TEST(LfuRandomizedOrdering, OrderingBug_Walk_AllInvariantsAfterEveryStep)
{
  constexpr uint32_t seed = 0xC0FFEE;
  constexpr int sequences = 100000;
  std::mt19937 rng(seed);
  for (int s = 0; s < sequences; ++s) {
    const size_t cap = 1 + rng() % 4;
    std::vector<Op> ops(1 + rng() % 24);
    for (auto& op : ops) {
      op.kind = static_cast<Op::Kind>(rng() % 9);
      op.key = rng() % kAllKeys.size();
    }
    auto fail = replay(ops, cap);
    if (!fail) continue;

    std::vector<Op> mini(ops.begin(), ops.begin() + fail->first + 1);
    for (bool shrunk = true; shrunk;) {
      shrunk = false;
      for (size_t i = 0; i < mini.size(); ++i) {
        std::vector<Op> t = mini;
        t.erase(t.begin() + i);
        if (replay(t, cap)) { mini = t; shrunk = true; break; }
      }
    }
    auto mf = replay(mini, cap);
    FAIL() << "\n  Seed: " << seed << " sequence #" << s << " capacity " << cap
           << "\n  Sequence (minimal): " << seq_str(mini, mini.size())
           << "\n  Current: " << (mf ? mf->second : fail->second) << " at step " << (mf ? mf->first : fail->first)
           << "\n  Expected: size<=capacity, disabled implies empty, only allowed keys present, hits+misses match the counted-lookup model, evictions pick a minimum-hit entry";
  }
}

TEST(LfuRandomizedOrdering, OrderingBug_Walk_Capacity0_AllInvariantsAfterEveryStep)
{
  constexpr uint32_t seed = 0xBADC0DE;
  std::mt19937 rng(seed);
  for (int s = 0; s < 1000; ++s) {
    std::vector<Op> ops(1 + rng() % 12);
    for (auto& op : ops) {
      op.kind = static_cast<Op::Kind>(rng() % 9);
      op.key = rng() % kAllKeys.size();
    }
    auto fail = replay(ops, 0);
    if (!fail) continue;
    std::vector<Op> mini(ops.begin(), ops.begin() + fail->first + 1);
    for (bool shrunk = true; shrunk;) {
      shrunk = false;
      for (size_t i = 0; i < mini.size(); ++i) {
        std::vector<Op> t = mini;
        t.erase(t.begin() + i);
        if (replay(t, 0)) { mini = t; shrunk = true; break; }
      }
    }
    auto mf = replay(mini, 0);
    FAIL() << "\n  Seed: " << seed << " sequence #" << s << " capacity 0"
           << "\n  Sequence (minimal): " << seq_str(mini, mini.size())
           << "\n  Current: " << (mf ? mf->second : fail->second)
           << "\n  Expected: size<=capacity (0) at every step";
  }
}

}  // namespace

namespace {

constexpr int kPendingQueueLimit = 32;

class PyCacheOrdering : public ::testing::Test {
protected:
  std::vector<PyObject*> objs;
  std::vector<Py_ssize_t> base;
  std::vector<std::string> log;
  std::unique_ptr<MgrMapCache<PyObject*>> pc;

  void SetUp() override {
    ASSERT_EQ(0, Py_MakePendingCalls());
    ASSERT_TRUE(PyGILState_Check());
  }

  void TearDown() override {
    pc.reset();
    Py_MakePendingCalls();
    for (auto* o : objs) Py_DECREF(o);
  }

  void make(uint16_t cap) {
    pc = std::make_unique<MgrMapCache<PyObject*>>(cap);
    log.clear();
    log.push_back("construct(cap=" + std::to_string(cap) + ")");
  }

  PyObject* mk() {
    PyObject* o = PyList_New(0);
    objs.push_back(o);
    base.push_back(Py_REFCNT(o));
    return o;
  }

  Py_ssize_t baseline(PyObject* o) const {
    for (size_t i = 0; i < objs.size(); ++i) {
      if (objs[i] == o) return base[i];
    }
    return -1;
  }

  std::string seq() const {
    std::string s;
    for (const auto& l : log) {
      if (!s.empty()) s += " ; ";
      s += l;
    }
    return s;
  }

  std::string refs() const {
    std::ostringstream o;
    o << "refcnt={";
    for (size_t i = 0; i < objs.size(); ++i) {
      o << (i ? "," : "") << "obj" << i << ":" << Py_REFCNT(objs[i]) << "(base " << base[i] << ")";
    }
    o << "}";
    return o.str();
  }

  std::string state() const {
    std::ostringstream o;
    o << "size=" << pc->size() << " hits=" << pc->get_hits()
      << " misses=" << pc->get_misses() << " " << refs();
    return o.str();
  }

  std::string msg(const std::string& expected) const {
    return "\n  Sequence: " + seq() + "\n  Current: " + state() + "\n  Expected: " + expected;
  }
};

TEST_F(PyCacheOrdering, OrderingBug_P1_EraseBeyondPendingQueue_RefcountsRestored)
{
  make(8);
  const int n = kPendingQueueLimit + 8;
  for (int i = 0; i < n; ++i) {
    PyObject* o = mk();
    pc->insert(kA, o);
    log.push_back("insert(osd_map,obj" + std::to_string(i) + ")");
    pc->erase(kA);
    log.push_back("erase(osd_map)");
  }
  ASSERT_EQ(0, Py_MakePendingCalls());
  log.push_back("Py_MakePendingCalls()");
  int leaked = 0;
  for (auto* o : objs) {
    if (Py_REFCNT(o) != baseline(o)) leaked++;
  }
  EXPECT_EQ(leaked, 0)
    << msg("all " + std::to_string(n) + " cached-then-erased objects back at their original refcount after every pending call ran; "
           + std::to_string(leaked) + " still hold the cache's reference");
}

TEST_F(PyCacheOrdering, OrderingBug_P1_EraseWithinQueue_RefcountsRestored)
{
  make(8);
  const int n = kPendingQueueLimit / 2;
  for (int i = 0; i < n; ++i) {
    PyObject* o = mk();
    pc->insert(kA, o);
    log.push_back("insert(osd_map,obj" + std::to_string(i) + ")");
    pc->erase(kA);
    log.push_back("erase(osd_map)");
  }
  ASSERT_EQ(0, Py_MakePendingCalls());
  log.push_back("Py_MakePendingCalls()");
  for (size_t i = 0; i < objs.size(); ++i) {
    EXPECT_EQ(Py_REFCNT(objs[i]), baseline(objs[i]))
      << msg("obj" + std::to_string(i) + " back at original refcount after pending calls ran");
  }
}

TEST_F(PyCacheOrdering, OrderingBug_P1_EraseBurstAcrossKeys_RefcountsRestored)
{
  make(8);
  const char* keys[] = {kA, kB, kC, kD, kE, kF};
  int idx = 0;
  for (int round = 0; round < 8; ++round) {
    for (const char* k : keys) {
      PyObject* o = mk();
      pc->insert(k, o);
      log.push_back(std::string("insert(") + k + ",obj" + std::to_string(idx++) + ")");
    }
    for (const char* k : keys) {
      pc->erase(k);
      log.push_back(std::string("erase(") + k + ")");
    }
  }
  ASSERT_EQ(0, Py_MakePendingCalls());
  int leaked = 0;
  for (auto* o : objs) {
    if (Py_REFCNT(o) != baseline(o)) leaked++;
  }
  EXPECT_EQ(leaked, 0)
    << msg("every erased object back at its original refcount; " + std::to_string(leaked) + " leaked");
}

TEST_F(PyCacheOrdering, OrderingBug_P3_ReplaceBeyondPendingQueue_NoLeak)
{
  make(8);
  const int n = kPendingQueueLimit + 8;
  for (int i = 0; i < n; ++i) {
    pc->insert(kA, mk());
    log.push_back("insert(osd_map,obj" + std::to_string(i) + ")");
  }
  ASSERT_EQ(0, Py_MakePendingCalls());
  log.push_back("Py_MakePendingCalls()");
  for (int i = 0; i < n - 1; ++i) {
    EXPECT_EQ(Py_REFCNT(objs[i]), baseline(objs[i]))
      << msg("replaced obj" + std::to_string(i) + " back at original refcount");
  }
  EXPECT_EQ(Py_REFCNT(objs[n - 1]), baseline(objs[n - 1]) + 1)
    << msg("the resident obj" + std::to_string(n - 1) + " holds exactly the cache's reference");
}

TEST_F(PyCacheOrdering, OrderingBug_P3_EvictBeyondPendingQueue_NoLeak)
{
  make(1);
  const char* keys[] = {kA, kB};
  const int n = kPendingQueueLimit + 8;
  for (int i = 0; i < n; ++i) {
    pc->insert(keys[i % 2], mk());
    log.push_back(std::string("insert(") + keys[i % 2] + ",obj" + std::to_string(i) + ")");
  }
  ASSERT_EQ(0, Py_MakePendingCalls());
  log.push_back("Py_MakePendingCalls()");
  for (int i = 0; i < n - 1; ++i) {
    EXPECT_EQ(Py_REFCNT(objs[i]), baseline(objs[i]))
      << msg("evicted obj" + std::to_string(i) + " back at original refcount");
  }
  EXPECT_EQ(pc->size(), size_t{1}) << msg("size == capacity (1)");
}

TEST_F(PyCacheOrdering, OrderingBug_P2_ThreadWithoutGil_InsertChangesNothing)
{
  make(4);
  PyObject* o = mk();
  const Py_ssize_t before = Py_REFCNT(o);
  std::thread t([&] { pc->insert(kA, o); });
  t.join();
  log.push_back("insert(osd_map,obj0) from a thread without the GIL");
  EXPECT_EQ(pc->size(), size_t{0}) << msg("size == 0: GIL-less insert never changes the cache");
  EXPECT_EQ(Py_REFCNT(o), before) << msg("refcount unchanged by a GIL-less insert");
  EXPECT_EQ(pc->get_misses(), 0u) << msg("no miss counted for a GIL-less insert");
}

TEST_F(PyCacheOrdering, OrderingBug_P2_ThreadWithoutGil_GetChangesNothing)
{
  make(4);
  PyObject* o = mk();
  pc->insert(kA, o);
  log.push_back("insert(osd_map,obj0)");
  const Py_ssize_t before = Py_REFCNT(o);
  const uint64_t h = pc->get_hits(), m = pc->get_misses();
  PyObject* got = o;
  PyObject* missed = o;
  std::thread t([&] { got = pc->get(kA); missed = pc->get(kB); });
  t.join();
  log.push_back("get(osd_map), get(pg_dump) from a thread without the GIL");
  EXPECT_EQ(got, nullptr) << msg("GIL-less get returns nullptr for a present key");
  EXPECT_EQ(missed, nullptr) << msg("GIL-less get returns nullptr for an absent key");
  EXPECT_EQ(Py_REFCNT(o), before) << msg("refcount unchanged by a GIL-less get");
  EXPECT_EQ(pc->get_hits(), h) << msg("hits unchanged by a GIL-less get");
  EXPECT_EQ(pc->get_misses(), m) << msg("misses unchanged by a GIL-less get");
  EXPECT_EQ(pc->size(), size_t{1}) << msg("entry still cached");
}

TEST_F(PyCacheOrdering, OrderingBug_P2_ReleasedGilOnSameThread_NoChange)
{
  make(4);
  PyObject* o = mk();
  pc->insert(kA, o);
  const Py_ssize_t before = Py_REFCNT(o);
  PyObject* other = mk();
  const Py_ssize_t other_before = Py_REFCNT(other);
  PyObject* got = o;
  PyThreadState* ts = PyEval_SaveThread();
  pc->insert(kB, other);
  got = pc->get(kA);
  PyEval_RestoreThread(ts);
  log.push_back("insert(osd_map,obj0) ; PyEval_SaveThread ; insert(pg_dump,obj1) ; get(osd_map) ; PyEval_RestoreThread");
  EXPECT_EQ(got, nullptr) << msg("get without the GIL returns nullptr");
  EXPECT_EQ(Py_REFCNT(o), before) << msg("obj0 refcount unchanged");
  EXPECT_EQ(Py_REFCNT(other), other_before) << msg("obj1 refcount unchanged");
  EXPECT_EQ(pc->size(), size_t{1}) << msg("size == 1: the GIL-less insert did not add pg_dump");
  EXPECT_FALSE(pc->exists(kB)) << msg("pg_dump absent");
}

TEST_F(PyCacheOrdering, OrderingBug_P4_ConfigDisable_ReleasesAllAndRejects)
{
  make(8);
  PyObject* a = mk();
  PyObject* b = mk();
  pc->insert(kA, a);
  pc->insert(kB, b);
  log.push_back("insert(osd_map,obj0) ; insert(pg_dump,obj1)");
  ASSERT_EQ(0, g_conf().set_val("mgr_map_cache_enabled", "false"));
  g_conf().apply_changes(nullptr);
  log.push_back("config mgr_map_cache_enabled=false");
  EXPECT_FALSE(pc->is_enabled()) << msg("cache disabled by the config observer");
  EXPECT_EQ(pc->size(), size_t{0}) << msg("size == 0 after disable");
  EXPECT_EQ(Py_REFCNT(a), baseline(a)) << msg("obj0 released by disable");
  EXPECT_EQ(Py_REFCNT(b), baseline(b)) << msg("obj1 released by disable");
  pc->insert(kA, a);
  EXPECT_EQ(pc->size(), size_t{0}) << msg("insert while disabled is ignored");
  EXPECT_EQ(Py_REFCNT(a), baseline(a)) << msg("rejected insert leaves the refcount at base");
  EXPECT_EQ(pc->get(kA), nullptr) << msg("get while disabled returns nullptr");
  ASSERT_EQ(0, g_conf().set_val("mgr_map_cache_enabled", "true"));
  g_conf().apply_changes(nullptr);
  log.push_back("config mgr_map_cache_enabled=true");
  EXPECT_EQ(pc->get(kA), nullptr) << msg("no entry from before the disable is visible after re-enable");
  EXPECT_EQ(pc->get_hits() + pc->get_misses(), 1u)
    << msg("counters restarted at the disable: exactly the one miss from the get after re-enable");
}

TEST_F(PyCacheOrdering, OrderingBug_P5_GetInsertEraseGet_RefcountAndCounters)
{
  make(4);
  PyObject* o = mk();
  EXPECT_EQ(pc->get(kA), nullptr) << msg("get on an empty cache returns nullptr");
  pc->insert(kA, o);
  EXPECT_EQ(Py_REFCNT(o), baseline(o) + 1) << msg("cache holds one reference");
  PyObject* g = pc->get(kA);
  log.push_back("get(osd_map)");
  EXPECT_EQ(g, o) << msg("get returns the cached object");
  EXPECT_EQ(Py_REFCNT(o), baseline(o) + 2) << msg("a hit hands the caller its own reference");
  Py_XDECREF(g);
  pc->erase(kA);
  ASSERT_EQ(0, Py_MakePendingCalls());
  EXPECT_EQ(Py_REFCNT(o), baseline(o)) << msg("back at base after erase and pending calls");
  EXPECT_EQ(pc->get(kA), nullptr) << msg("get after erase returns nullptr");
  EXPECT_EQ(pc->get_hits(), 1u) << msg("hits == 1");
}

TEST_F(PyCacheOrdering, OrderingBug_P9_EraseOnWorker_ReleasedWithoutMainThreadPython)
{
  make(4);
  PyObject* o = mk();
  const Py_ssize_t base_ref = baseline(o);
  PyThreadState* main_ts = PyEval_SaveThread();
  std::thread w([&] {
    PyGILState_STATE st = PyGILState_Ensure();
    pc->insert(kA, o);
    pc->erase(kA);
    for (int i = 0; i < 50; ++i) {
      PyRun_SimpleString("x = sum(range(1000))");
    }
    PyGILState_Release(st);
  });
  w.join();
  PyEval_RestoreThread(main_ts);
  log.push_back("main thread parked outside Python ; worker: insert(osd_map,obj0) ; erase(osd_map) ; 50 x PyRun_SimpleString");
  EXPECT_EQ(Py_REFCNT(o), base_ref)
    << msg("obj0 back at its original refcount: ceph-mgr's main thread parks outside Python after PyModuleRegistry::init and only worker threads run bytecode");
}

TEST_F(PyCacheOrdering, OrderingBug_P5_NonCacheableKey_NoRefcountOrCacheEffect)
{
  make(4);
  PyObject* o = mk();
  pc->insert(kBad, o);
  log.push_back("insert(not_allowed,obj0)");
  EXPECT_EQ(Py_REFCNT(o), baseline(o)) << msg("refcount unchanged for a non-cacheable key");
  EXPECT_EQ(pc->size(), size_t{0}) << msg("size == 0");
  EXPECT_EQ(pc->get(kBad), nullptr) << msg("get of a non-cacheable key returns nullptr");
  pc->erase(kBad);
  EXPECT_EQ(pc->get_hits() + pc->get_misses(), 0u) << msg("no accounting for a non-cacheable key");
}

TEST_F(PyCacheOrdering, OrderingBug_P5_EraseAbsentKey_NoOp)
{
  make(4);
  PyObject* o = mk();
  pc->insert(kA, o);
  pc->erase(kB);
  ASSERT_EQ(0, Py_MakePendingCalls());
  EXPECT_EQ(Py_REFCNT(o), baseline(o) + 1) << msg("erase of an absent key leaves the resident object untouched");
  EXPECT_EQ(pc->size(), size_t{1}) << msg("size == 1");
}

TEST_F(PyCacheOrdering, OrderingBug_P6_DestroyWithEntries_ReleasesAll)
{
  make(8);
  PyObject* a = mk();
  PyObject* b = mk();
  pc->insert(kA, a);
  pc->insert(kB, b);
  pc->insert(kA, b);
  log.push_back("insert(osd_map,obj0) ; insert(pg_dump,obj1) ; insert(osd_map,obj1)");
  pc.reset();
  ASSERT_EQ(0, Py_MakePendingCalls());
  log.push_back("destroy cache ; Py_MakePendingCalls()");
  EXPECT_EQ(Py_REFCNT(a), baseline(a)) << "\n  Sequence: " << seq() << "\n  Current: refcnt(obj0)=" << Py_REFCNT(a) << "\n  Expected: base " << baseline(a);
  EXPECT_EQ(Py_REFCNT(b), baseline(b)) << "\n  Sequence: " << seq() << "\n  Current: refcnt(obj1)=" << Py_REFCNT(b) << "\n  Expected: base " << baseline(b);
}

TEST_F(PyCacheOrdering, OrderingBug_P15_ConfigChangeAfterDestruction_ObserverRemoved)
{
  make(4);
  PyObject* o = mk();
  pc->insert(kA, o);
  pc.reset();
  ASSERT_EQ(0, Py_MakePendingCalls());
  log.push_back("insert(osd_map,obj0) ; destroy cache ; Py_MakePendingCalls()");
  ASSERT_EQ(0, g_conf().set_val("mgr_map_cache_enabled", "false"));
  g_conf().apply_changes(nullptr);
  ASSERT_EQ(0, g_conf().set_val("mgr_map_cache_enabled", "true"));
  g_conf().apply_changes(nullptr);
  log.push_back("config mgr_map_cache_enabled=false ; config mgr_map_cache_enabled=true");
  EXPECT_EQ(Py_REFCNT(o), baseline(o))
    << "\n  Sequence: " << seq()
    << "\n  Current: refcnt(obj0)=" << Py_REFCNT(o)
    << "\n  Expected: base " << baseline(o) << " and no callback into the destroyed cache";
}

TEST_F(PyCacheOrdering, OrderingBug_P19_ConfigChange_RedundantEnableAndUnrelatedKeyKeepEntries)
{
  make(4);
  PyObject* o = mk();
  pc->insert(kA, o);
  ASSERT_EQ(0, g_conf().set_val("mgr_map_cache_enabled", "true"));
  g_conf().apply_changes(nullptr);
  ASSERT_EQ(0, g_conf().set_val("mgr_stats_period", "7"));
  g_conf().apply_changes(nullptr);
  log.push_back("insert(osd_map,obj0) ; config mgr_map_cache_enabled=true ; config mgr_stats_period=7");
  EXPECT_TRUE(pc->is_enabled()) << msg("still enabled");
  EXPECT_EQ(pc->size(), size_t{1}) << msg("a redundant enable and an unrelated option leave the entry cached");
  EXPECT_EQ(Py_REFCNT(o), baseline(o) + 1) << msg("obj0 still held once by the cache");
  ASSERT_EQ(0, g_conf().set_val("mgr_stats_period", "5"));
  g_conf().apply_changes(nullptr);
}

TEST_F(PyCacheOrdering, OrderingBug_P10_ReinsertSamePointerSameKey_RefcountStable)
{
  make(4);
  PyObject* o = mk();
  for (int i = 0; i < 5; ++i) {
    pc->insert(kA, o);
    log.push_back("insert(osd_map,obj0)");
  }
  ASSERT_EQ(0, Py_MakePendingCalls());
  log.push_back("Py_MakePendingCalls()");
  EXPECT_EQ(Py_REFCNT(o), baseline(o) + 1)
    << msg("obj0 held exactly once by the cache after re-inserting the same pointer under the same key");
  EXPECT_EQ(pc->size(), size_t{1}) << msg("size == 1");
  pc->erase(kA);
  ASSERT_EQ(0, Py_MakePendingCalls());
  EXPECT_EQ(Py_REFCNT(o), baseline(o)) << msg("obj0 back at base after erase");
}

TEST_F(PyCacheOrdering, OrderingBug_P11_EraseWhileDisabled_NoRefcountEffect)
{
  make(4);
  PyObject* o = mk();
  pc->insert(kA, o);
  pc->set_enabled(false);
  log.push_back("insert(osd_map,obj0) ; set_enabled(false)");
  pc->erase(kA);
  log.push_back("erase(osd_map)");
  ASSERT_EQ(0, Py_MakePendingCalls());
  EXPECT_EQ(Py_REFCNT(o), baseline(o)) << msg("obj0 already released by the disable; erase must not release it again");
  EXPECT_EQ(pc->size(), size_t{0}) << msg("size == 0");
}

TEST_F(PyCacheOrdering, OrderingBug_P12_SameObjectUnderTwoKeys_EraseOneKeepsOther)
{
  make(4);
  PyObject* o = mk();
  pc->insert(kA, o);
  pc->insert(kB, o);
  pc->erase(kA);
  ASSERT_EQ(0, Py_MakePendingCalls());
  log.push_back("insert(osd_map,obj0) ; insert(pg_dump,obj0) ; erase(osd_map) ; Py_MakePendingCalls()");
  EXPECT_EQ(Py_REFCNT(o), baseline(o) + 1) << msg("obj0 still held once, by pg_dump");
  PyObject* g = pc->get(kB);
  EXPECT_EQ(g, o) << msg("pg_dump still returns obj0");
  Py_XDECREF(g);
}

TEST_F(PyCacheOrdering, OrderingBug_P13_ReplaceOnWorkerThread_ReleasedWithoutMainThreadPython)
{
  make(4);
  PyObject* first = mk();
  PyObject* second = mk();
  PyThreadState* main_ts = PyEval_SaveThread();
  std::thread w([&] {
    PyGILState_STATE st = PyGILState_Ensure();
    pc->insert(kA, first);
    pc->insert(kA, second);
    for (int i = 0; i < 50; ++i) {
      PyRun_SimpleString("x = sum(range(1000))");
    }
    PyGILState_Release(st);
  });
  w.join();
  PyEval_RestoreThread(main_ts);
  log.push_back("main thread parked outside Python ; worker: insert(osd_map,obj0) ; insert(osd_map,obj1) ; 50 x PyRun_SimpleString");
  EXPECT_EQ(Py_REFCNT(first), baseline(first))
    << msg("the replaced obj0 is back at its original refcount");
  EXPECT_EQ(Py_REFCNT(second), baseline(second) + 1)
    << msg("obj1 held exactly once by the cache");
}

TEST_F(PyCacheOrdering, OrderingBug_P14_EvictionPicksMinimumHitObject)
{
  make(2);
  PyObject* a = mk();
  PyObject* b = mk();
  PyObject* c = mk();
  pc->insert(kA, a);
  pc->insert(kB, b);
  for (int i = 0; i < 3; ++i) Py_XDECREF(pc->get(kA));
  Py_XDECREF(pc->get(kB));
  pc->insert(kC, c);
  ASSERT_EQ(0, Py_MakePendingCalls());
  log.push_back("insert(osd_map,obj0) ; insert(pg_dump,obj1) ; 3 x get(osd_map) ; 1 x get(pg_dump) ; insert(pg_stats,obj2) ; Py_MakePendingCalls()");
  EXPECT_TRUE(pc->exists(kA) && pc->exists(kC) && !pc->exists(kB))
    << msg("pg_dump (1 hit, the minimum) evicted; osd_map (3 hits) and the new pg_stats resident");
  EXPECT_EQ(Py_REFCNT(b), baseline(b)) << msg("the evicted obj1 is released");
}

TEST_F(PyCacheOrdering, OrderingBug_P16_ClearFromThreadWithoutGil_ReleasesEntries)
{
  make(4);
  PyObject* a = mk();
  PyObject* b = mk();
  pc->insert(kA, a);
  pc->insert(kB, b);
  PyThreadState* main_ts = PyEval_SaveThread();
  std::thread t([&] { pc->clear(); });
  t.join();
  PyEval_RestoreThread(main_ts);
  log.push_back("insert(osd_map,obj0) ; insert(pg_dump,obj1) ; clear() from a thread without the GIL");
  EXPECT_EQ(pc->size(), size_t{0}) << msg("size == 0 after clear()");
  EXPECT_EQ(Py_REFCNT(a), baseline(a)) << msg("obj0 released by clear()");
  EXPECT_EQ(Py_REFCNT(b), baseline(b)) << msg("obj1 released by clear()");
  EXPECT_EQ(pc->get_hits() + pc->get_misses(), 0u) << msg("counters reset by clear()");
}

TEST_F(PyCacheOrdering, OrderingBug_P18_ClearEmptyFromThreadWithoutGil_NoOp)
{
  make(4);
  PyThreadState* main_ts = PyEval_SaveThread();
  std::thread t([&] { pc->clear(); pc->clear(); });
  t.join();
  PyEval_RestoreThread(main_ts);
  log.push_back("clear() ; clear() on an empty cache from a thread without the GIL");
  EXPECT_EQ(pc->size(), size_t{0}) << msg("size == 0");
  EXPECT_TRUE(pc->is_enabled()) << msg("clear() does not disable the cache");
}

TEST_F(PyCacheOrdering, OrderingBug_P17_ErasePerKeyPendingDoesNotDoubleRelease)
{
  make(4);
  PyObject* o = mk();
  pc->insert(kA, o);
  pc->erase(kA);
  pc->erase(kA);
  ASSERT_EQ(0, Py_MakePendingCalls());
  log.push_back("insert(osd_map,obj0) ; erase(osd_map) ; erase(osd_map) ; Py_MakePendingCalls()");
  EXPECT_EQ(Py_REFCNT(o), baseline(o)) << msg("obj0 released exactly once by two erases");
}

TEST_F(PyCacheOrdering, OrderingBug_P7_Capacity0_SizeBound)
{
  make(0);
  PyObject* a = mk();
  PyObject* b = mk();
  pc->insert(kA, a);
  log.push_back("insert(osd_map,obj0)");
  EXPECT_LE(pc->size(), size_t{0}) << msg("size() <= capacity (0) after the first insert");
  pc->insert(kB, b);
  log.push_back("insert(pg_dump,obj1)");
  ASSERT_EQ(0, Py_MakePendingCalls());
  EXPECT_LE(pc->size(), size_t{0}) << msg("size() <= capacity (0) after the second insert");
  EXPECT_EQ(Py_REFCNT(a), baseline(a) + (pc->exists(kA) ? 1 : 0))
    << msg("obj0 holds a cache reference only while resident");
  EXPECT_EQ(Py_REFCNT(b), baseline(b) + (pc->exists(kB) ? 1 : 0))
    << msg("obj1 holds a cache reference only while resident");
}

TEST_F(PyCacheOrdering, OrderingBug_C6_ConcurrentDisableVsInsert_ReferencesBalanced)
{
  constexpr int iterations = 10000;
  std::vector<PyObject*> pool;
  std::vector<Py_ssize_t> pool_base;
  for (int i = 0; i < 4; ++i) {
    pool.push_back(PyList_New(0));
    pool_base.push_back(Py_REFCNT(pool.back()));
  }
  int failures = 0;
  std::string first_fail;
  PyThreadState* main_ts = PyEval_SaveThread();
  for (int iter = 0; iter < iterations && failures == 0; ++iter) {
    auto cache = std::make_unique<MgrMapCache<PyObject*>>(2);
    StartGate gate;
    std::vector<std::thread> ts;
    for (int t = 0; t < 2; ++t) {
      ts.emplace_back([&, t] {
        gate.wait();
        PyGILState_STATE st = PyGILState_Ensure();
        for (int i = 0; i < 4; ++i) {
          cache->insert(kAllKeys[(t + i) % 6], pool[(t + i) % 4]);
        }
        PyGILState_Release(st);
      });
    }
    std::thread d([&] { gate.wait(); cache->set_enabled(false); });
    gate.open();
    for (auto& t : ts) t.join();
    d.join();
    const size_t left = cache->size();
    cache.reset();
    PyEval_RestoreThread(main_ts);
    Py_MakePendingCalls();
    if (left != 0) {
      failures++;
      first_fail = "iteration " + std::to_string(iter) + ": size_after_disable=" + std::to_string(left);
    }
    for (int i = 0; i < 4 && failures == 0; ++i) {
      if (Py_REFCNT(pool[i]) != pool_base[i]) {
        failures++;
        first_fail = "iteration " + std::to_string(iter) + ": pool" + std::to_string(i) +
                     " refcnt " + std::to_string(Py_REFCNT(pool[i])) + " base " + std::to_string(pool_base[i]);
      }
    }
    main_ts = PyEval_SaveThread();
  }
  PyEval_RestoreThread(main_ts);
  Py_MakePendingCalls();
  for (auto* o : pool) Py_DECREF(o);
  EXPECT_EQ(failures, 0)
    << "\n  Sequence: per iteration: 2 GIL-holding threads x 4 insert(allowed key, shared pool object) concurrent with set_enabled(false) from a thread without the GIL; all joined; cache destroyed; Py_MakePendingCalls()"
    << "\n  Current: " << first_fail
    << "\n  Expected: size()==0 once set_enabled(false) returned and every pool object back at its original refcount (rejected, replaced, evicted and drained references all released)";
}

TEST_F(PyCacheOrdering, OrderingBug_C7_ConcurrentGetVsEraseVsInsert_ReferencesBalanced)
{
  constexpr int iterations = 10000;
  PyObject* o = PyList_New(0);
  const Py_ssize_t o_base = Py_REFCNT(o);
  auto cache = std::make_unique<MgrMapCache<PyObject*>>(4);
  std::atomic<long> bad_get{0};
  PyThreadState* main_ts = PyEval_SaveThread();
  for (int iter = 0; iter < iterations; ++iter) {
    StartGate gate;
    std::vector<std::thread> ts;
    ts.emplace_back([&] {
      gate.wait();
      PyGILState_STATE st = PyGILState_Ensure();
      for (int i = 0; i < 4; ++i) {
        PyObject* g = cache->get(kA);
        if (g && g != o) bad_get++;
        Py_XDECREF(g);
      }
      PyGILState_Release(st);
    });
    ts.emplace_back([&] {
      gate.wait();
      PyGILState_STATE st = PyGILState_Ensure();
      for (int i = 0; i < 2; ++i) cache->erase(kA);
      PyGILState_Release(st);
    });
    ts.emplace_back([&] {
      gate.wait();
      PyGILState_STATE st = PyGILState_Ensure();
      for (int i = 0; i < 2; ++i) cache->insert(kA, o);
      PyGILState_Release(st);
    });
    gate.open();
    for (auto& t : ts) t.join();
    PyEval_RestoreThread(main_ts);
    Py_MakePendingCalls();
    main_ts = PyEval_SaveThread();
  }
  PyEval_RestoreThread(main_ts);
  cache->clear();
  Py_MakePendingCalls();
  const Py_ssize_t o_now = Py_REFCNT(o);
  cache.reset();
  EXPECT_EQ(bad_get.load(), 0) << "get() returned an object other than the one inserted";
  EXPECT_EQ(o_now, o_base)
    << "\n  Sequence: " << iterations << " iterations of 3 concurrent GIL-holding threads: {4 x get(osd_map)+DECREF}, {2 x erase(osd_map)}, {2 x insert(osd_map,obj)}; then clear() and Py_MakePendingCalls()"
    << "\n  Current: refcnt(obj)=" << o_now << " base=" << o_base
    << "\n  Expected: refcnt(obj)==base once the cache is empty and pending calls ran";
  Py_DECREF(o);
}

TEST_F(PyCacheOrdering, OrderingBug_C9_ConcurrentGetVsDisable_ReferencesBalanced)
{
  constexpr int iterations = 10000;
  PyObject* o = PyList_New(0);
  const Py_ssize_t o_base = Py_REFCNT(o);
  std::atomic<long> bad_get{0};
  PyThreadState* main_ts = PyEval_SaveThread();
  for (int iter = 0; iter < iterations; ++iter) {
    auto cache = std::make_unique<MgrMapCache<PyObject*>>(4);
    {
      PyGILState_STATE st = PyGILState_Ensure();
      cache->insert(kA, o);
      PyGILState_Release(st);
    }
    StartGate gate;
    std::vector<std::thread> ts;
    for (int t = 0; t < 2; ++t) {
      ts.emplace_back([&] {
        gate.wait();
        PyGILState_STATE st = PyGILState_Ensure();
        for (int i = 0; i < 4; ++i) {
          PyObject* g = cache->get(kA);
          if (g && g != o) bad_get++;
          Py_XDECREF(g);
        }
        PyGILState_Release(st);
      });
    }
    ts.emplace_back([&] { gate.wait(); cache->set_enabled(false); });
    gate.open();
    for (auto& t : ts) t.join();
    const size_t left = cache->size();
    cache.reset();
    PyEval_RestoreThread(main_ts);
    Py_MakePendingCalls();
    const Py_ssize_t now = Py_REFCNT(o);
    main_ts = PyEval_SaveThread();
    if (now != o_base || left != 0) {
      PyEval_RestoreThread(main_ts);
      ADD_FAILURE()
        << "\n  Sequence: iteration " << iter << ": insert(osd_map,obj) ; 2 GIL-holding threads x 4 get(osd_map)+DECREF concurrent with set_enabled(false) ; destroy cache ; Py_MakePendingCalls()"
        << "\n  Current: refcnt(obj)=" << now << " size_after_disable=" << left
        << "\n  Expected: refcnt(obj)==" << o_base << " and size_after_disable==0";
      Py_DECREF(o);
      return;
    }
  }
  PyEval_RestoreThread(main_ts);
  EXPECT_EQ(bad_get.load(), 0) << "get() returned an object other than the one inserted";
  Py_DECREF(o);
}

TEST_F(PyCacheOrdering, OrderingBug_Walk_PyRefcountsAfterEveryStep)
{
  constexpr uint32_t seed = 0x5EED5;
  constexpr int sequences = 20000;
  std::mt19937 rng(seed);
  std::vector<PyObject*> pool;
  for (int i = 0; i < 4; ++i) pool.push_back(mk());
  for (int sidx = 0; sidx < sequences; ++sidx) {
    const size_t cap = 1 + rng() % 3;
    pc = std::make_unique<MgrMapCache<PyObject*>>(static_cast<uint16_t>(cap));
    bool enabled = true;
    std::vector<std::string> steps;
    const int len = 1 + rng() % 24;
    for (int i = 0; i < len; ++i) {
      const int kind = rng() % 6;
      const std::string k = kAllKeys[rng() % kAllKeys.size()];
      const int oi = rng() % 4;
      switch (kind) {
      case 0:
        pc->insert(k, pool[oi]);
        steps.push_back("insert(" + k + ",pool" + std::to_string(oi) + ")");
        break;
      case 1:
        Py_XDECREF(pc->get(k));
        steps.push_back("get(" + k + ")");
        break;
      case 2:
        pc->erase(k);
        steps.push_back("erase(" + k + ")");
        break;
      case 3:
        pc->set_enabled(false);
        steps.push_back("set_enabled(false)");
        enabled = false;
        break;
      case 4:
        pc->set_enabled(true);
        steps.push_back("set_enabled(true)");
        enabled = true;
        break;
      default:
        pc->clear();
        steps.push_back("clear()");
        break;
      }
      Py_MakePendingCalls();
      std::string why;
      if (pc->size() > cap) why = "size " + std::to_string(pc->size()) + " > capacity " + std::to_string(cap);
      size_t present = 0;
      for (const auto& kk : kAllKeys) present += pc->exists(kk) ? 1 : 0;
      if (why.empty() && !enabled && present != 0) why = "disabled but " + std::to_string(present) + " entries present";
      if (why.empty() && pc->exists(kBad)) why = "non-allowed key present";
      if (why.empty()) {
        Py_ssize_t resident_refs[4] = {0, 0, 0, 0};
        for (const auto& kk : kAllKeys) {
          if (!pc->exists(kk)) continue;
          PyObject* g = pc->get(kk);
          for (int pi = 0; pi < 4; ++pi) if (g == pool[pi]) resident_refs[pi]++;
          Py_XDECREF(g);
        }
        for (int pi = 0; pi < 4 && why.empty(); ++pi) {
          if (Py_REFCNT(pool[pi]) != base[pi] + resident_refs[pi]) {
            why = "pool" + std::to_string(pi) + " refcnt " + std::to_string(Py_REFCNT(pool[pi])) +
                  " expected base " + std::to_string(base[pi]) + " + " + std::to_string(resident_refs[pi]) + " resident";
          }
        }
      }
      if (!why.empty()) {
        std::string sq;
        for (const auto& st : steps) sq += (sq.empty() ? "" : " ; ") + st;
        FAIL() << "\n  Seed: " << seed << " sequence #" << sidx << " capacity " << cap
               << "\n  Sequence: " << sq
               << "\n  Current: " << why
               << "\n  Expected: size<=capacity, disabled implies empty, only allowed keys present, every object's refcount == base + number of cache entries holding it once pending calls ran";
      }
    }
    pc.reset();
    Py_MakePendingCalls();
  }
}

}  // namespace

int main(int argc, char** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  std::vector<const char*> args(argv, argv + argc);
  auto cct = global_init(nullptr, args, CEPH_ENTITY_TYPE_CLIENT,
                         CODE_ENVIRONMENT_UTILITY,
                         CINIT_FLAG_NO_DEFAULT_CONFIG_FILE);
  common_init_finish(g_ceph_context);
  Py_Initialize();
  return RUN_ALL_TESTS();
}
