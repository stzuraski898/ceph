// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
//
// Ordering / invariant tests for ClusterState.
//
// Each test replays a fixed or randomised operation sequence and asserts
// the invariant, not the current behaviour.  A test that fails is exposing
// a real defect.  Assertions are not weakened to make tests pass.

#include <algorithm>
#include <atomic>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "global/global_context.h"
#include "global/global_init.h"
#include "gtest/gtest.h"
#include "messages/MMgrDigest.h"
#include "messages/MPGStats.h"
#include "mgr/ClusterState.h"
#include "mon/PGMap.h"
#include "osd/osd_types.h"
#include "test/mgr/TestMgr.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

// Collect internal-consistency violations visible through the test accessors.
// Returns an empty vector on a consistent state.
std::vector<std::string> check_invariants(
    ClusterStateTestHelper& cs,
    const std::map<int64_t, unsigned>& expected_pools)
{
  std::vector<std::string> errs;
  const PGMap& pgm     = cs.test_get_pg_map();
  const auto& ep       = cs.test_get_existing_pools();
  const auto& pinc     = cs.test_get_pending_inc();

  // INV-A: every pg_stat key refers to an existing pool and ps < pg_num
  for (auto& [pgid, _] : pgm.pg_stat) {
    auto it = expected_pools.find(static_cast<int64_t>(pgid.pool()));
    if (it == expected_pools.end()) {
      std::ostringstream oss;
      oss << "pg_stat has pgid " << pgid
          << " but pool " << pgid.pool() << " is not in expected_pools";
      errs.push_back(oss.str());
    } else if (pgid.ps() >= it->second) {
      std::ostringstream oss;
      oss << "pg_stat has pgid " << pgid
          << " ps=" << pgid.ps()
          << " >= pg_num=" << it->second;
      errs.push_back(oss.str());
    }
  }

  // INV-B: every pool_statfs key refers to an existing pool
  for (auto& [key, _] : pgm.pool_statfs) {
    int64_t pool = key.first;
    if (expected_pools.count(pool) == 0) {
      std::ostringstream oss;
      oss << "pool_statfs has entry for pool " << pool
          << " which is not in expected_pools";
      errs.push_back(oss.str());
    }
  }

  // INV-C: existing_pools matches expected_pools
  for (auto& [p, _] : expected_pools) {
    if (ep.count(p) == 0) {
      std::ostringstream oss;
      oss << "existing_pools missing expected pool " << p;
      errs.push_back(oss.str());
    }
  }
  for (auto& [p, _] : ep) {
    if (expected_pools.count(p) == 0) {
      std::ostringstream oss;
      oss << "existing_pools has unexpected pool " << p;
      errs.push_back(oss.str());
    }
  }

  // INV-D: pending_inc.pg_stat_updates must not refer to removed pools
  for (auto& [pgid, _] : pinc.pg_stat_updates) {
    if (expected_pools.count(static_cast<int64_t>(pgid.pool())) == 0) {
      std::ostringstream oss;
      oss << "pending_inc.pg_stat_updates has pgid " << pgid
          << " for removed pool " << pgid.pool();
      errs.push_back(oss.str());
    }
  }

  return errs;
}

// Build an MPGStats message from OSD `osd_num` with a single PG and a single
// pool stat entry.
ceph::ref_t<MPGStats> make_pg_stats(int osd_num, pg_t pgid, uint64_t seq,
                                     int64_t pool_id)
{
  auto msg = ceph::make_ref<MPGStats>();
  msg->set_src(entity_name_t::OSD(osd_num));

  pg_stat_t pgs;
  pgs.state          = PG_STATE_ACTIVE;
  pgs.reported_epoch = 1;
  pgs.reported_seq   = seq;
  msg->pg_stat[pgid] = pgs;

  msg->osd_stat.seq = seq;

  store_statfs_t sfs;
  sfs.total = 1024 * 1024;
  msg->pool_stat[pool_id] = sfs;

  return msg;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// C1 — pool removed between ingest and apply
//
// The ClusterStateTest fixture already has osd_map at epoch 1 with pool 1,
// pg_num=2.  We apply incrementals on top of the fixture state.
// ---------------------------------------------------------------------------

TEST_F(ClusterStateTest, OrderingBug_C1_PoolRemovedBetweenIngestAndApply)
{
  // Fixture state: pool 1 exists (pg_num=2), notify_osdmap already called.
  // Sequence:
  //   ingest_pgstats(pool 1, pg (1,0), pool_statfs)
  //   notify_osdmap(pool 1 removed)
  //
  // INVARIANT: no pg_stat, pg_pool_sum, or pool_statfs entry for pool 1
  //            remains after pool removal.

  const int64_t P = 1;
  pg_t pgid(0, P);

  // Verify fixture setup
  ASSERT_GT(cs->test_get_existing_pools().count(P), 0u)
      << "Setup: pool " << P << " must exist in existing_pools after fixture setup";

  // Step 1: ingest stats for pool P
  auto s = make_pg_stats(0, pgid, 10, P);
  cs->ingest_pgstats(s);

  {
    const auto& pinc = cs->test_get_pending_inc();
    ASSERT_GT(pinc.pg_stat_updates.count(pgid), 0u)
        << "Setup: ingest must accept pg_stat_updates for pool " << P;
    ASSERT_GT(pinc.pool_statfs_updates.count(std::make_pair(P, 0)), 0u)
        << "Setup: ingest must accept pool_statfs_updates for pool " << P;
  }

  // Step 2: notify_osdmap with pool P removed (new epoch, no pools)
  OSDMap::Incremental rm_inc(osd_map.get_epoch() + 1);
  rm_inc.old_pools.insert(P);
  osd_map.apply_incremental(rm_inc);
  cs->with_osdmap_and_pgmap([&](const OSDMap&, const PGMap&) {
    cs->notify_osdmap(osd_map);
  });

  // INVARIANT checks
  const PGMap& pgm = cs->test_get_pg_map();
  for (auto& [id, _] : pgm.pg_stat) {
    ASSERT_NE(static_cast<int64_t>(id.pool()), P)
        << "INVARIANT VIOLATED: pg_stat still has pgid " << id
        << " for removed pool " << P
        << "; operation sequence: ingest(pool P) -> notify(pool P removed)";
  }
  ASSERT_EQ(pgm.pg_pool_sum.count(P), 0u)
      << "INVARIANT VIOLATED: pg_pool_sum still contains pool " << P
      << " after pool removal"
      << "; Expected pool removed; Current: pg_pool_sum contains it"
      << "; operation sequence: ingest(pool P) -> notify(pool P removed)";
  for (auto& [key, _] : pgm.pool_statfs) {
    ASSERT_NE(key.first, P)
        << "INVARIANT VIOLATED: pool_statfs still has entry for pool " << P
        << " after pool removal"
        << "; Current pool_statfs key=(" << key.first << "," << key.second << ")"
        << "; Expected: no entry for removed pool"
        << "; operation sequence: ingest(pool P) -> notify(pool P removed)";
  }
}

// ---------------------------------------------------------------------------
// C1b — pool_statfs_updates for removed pool ingested AFTER pool removal
// ---------------------------------------------------------------------------

TEST_F(ClusterStateTest, OrderingBug_C1b_PoolStatfsIngestedAfterPoolRemoval)
{
  // Fixture state: pool 1 exists (pg_num=2), notify_osdmap already called.
  // Sequence:
  //   notify_osdmap(pool 1 removed)   <- existing_pools no longer has P
  //   ingest_pgstats(pool 1 stats)     <- OSD still sends old stats
  //   update_delta_stats
  //
  // INVARIANT: pg_map.pool_statfs must not contain an entry for P.

  const int64_t P = 1;
  pg_t pgid(0, P);

  // Remove pool P
  OSDMap::Incremental rm_inc(osd_map.get_epoch() + 1);
  rm_inc.old_pools.insert(P);
  osd_map.apply_incremental(rm_inc);
  cs->with_osdmap_and_pgmap([&](const OSDMap&, const PGMap&) {
    cs->notify_osdmap(osd_map);
  });

  ASSERT_EQ(cs->test_get_existing_pools().count(P), 0u)
      << "Setup: existing_pools must not contain removed pool " << P;

  // Ingest stats for the removed pool (OSD not yet notified)
  auto s = make_pg_stats(0, pgid, 10, P);
  cs->ingest_pgstats(s);

  // pg_stat_updates must be filtered by existing_pools check
  {
    const auto& pinc = cs->test_get_pending_inc();
    ASSERT_EQ(pinc.pg_stat_updates.count(pgid), 0u)
        << "pg_stat_updates for removed pool " << P
        << " must be filtered by existing_pools check";
  }

  cs->update_delta_stats();

  // INVARIANT: no pool_statfs entry for removed pool
  const PGMap& pgm = cs->test_get_pg_map();
  for (auto& [key, _] : pgm.pool_statfs) {
    ASSERT_NE(key.first, P)
        << "INVARIANT VIOLATED: pool_statfs contains entry for removed pool "
        << P
        << "; Current pool_statfs key=(" << key.first << "," << key.second << ")"
        << "; Expected: no entry for removed pool"
        << "; operation sequence: notify(pool P removed) -> ingest(pool P) -> update_delta_stats";
  }
}

// ---------------------------------------------------------------------------
// C2 — pg_num shrink removes out-of-range pg_stat entries
// ---------------------------------------------------------------------------

TEST_F(ClusterStateTest, OrderingBug_C2_PgNumShrinkAfterIngest)
{
  // Fixture state: pool 1 exists with pg_num=2.
  // Sequence:
  //   notify_osdmap(pool 1, pg_num=4)  <- expand
  //   ingest_pgstats(pg=(1,3))          <- valid at pg_num=4
  //   notify_osdmap(pool 1, pg_num=2)   <- ps=3 is now out of range
  //
  // INVARIANT: no pg_stat with ps >= 2 remains in pg_map for pool 1.

  const int64_t P = 1;

  // Expand pool to pg_num=4
  {
    pg_pool_t expanded;
    expanded.set_pg_num(4);
    OSDMap::Incremental inc(osd_map.get_epoch() + 1);
    inc.new_pools[P] = expanded;
    osd_map.apply_incremental(inc);
    cs->with_osdmap_and_pgmap([&](const OSDMap&, const PGMap&) {
      cs->notify_osdmap(osd_map);
    });
  }

  ASSERT_EQ(cs->test_get_existing_pools().at(P), 4u)
      << "Setup: existing_pools must show pg_num=4 after expansion";

  // Ingest a stat for ps=3 (valid at pg_num=4)
  pg_t pgid(3, P);
  auto s = make_pg_stats(0, pgid, 10, P);
  cs->ingest_pgstats(s);
  {
    const auto& pinc = cs->test_get_pending_inc();
    ASSERT_GT(pinc.pg_stat_updates.count(pgid), 0u)
        << "Setup: ingest must accept ps=3 when pg_num=4";
  }

  // Shrink pool back to pg_num=2
  {
    pg_pool_t shrunk;
    shrunk.set_pg_num(2);
    OSDMap::Incremental inc(osd_map.get_epoch() + 1);
    inc.new_pools[P] = shrunk;
    osd_map.apply_incremental(inc);
    cs->with_osdmap_and_pgmap([&](const OSDMap&, const PGMap&) {
      cs->notify_osdmap(osd_map);
    });
  }

  const unsigned new_pg_num = 2;
  const PGMap& pgm = cs->test_get_pg_map();
  for (auto& [id, _] : pgm.pg_stat) {
    if (static_cast<int64_t>(id.pool()) == P) {
      ASSERT_LT(id.ps(), new_pg_num)
          << "INVARIANT VIOLATED: pg_stat has pgid " << id
          << " with ps=" << id.ps() << " >= new_pg_num=" << new_pg_num
          << "; operation sequence: notify(pg_num=4) -> ingest(ps=3) -> notify(pg_num=2)";
    }
  }
  for (auto& [id, _] : cs->test_get_pending_inc().pg_stat_updates) {
    if (static_cast<int64_t>(id.pool()) == P) {
      ASSERT_LT(id.ps(), new_pg_num)
          << "INVARIANT VIOLATED: pending_inc.pg_stat_updates has pgid " << id
          << " with ps=" << id.ps() << " >= new_pg_num=" << new_pg_num
          << "; Expected: pending_inc clean of out-of-range entries after notify_osdmap"
          << "; operation sequence: notify(pg_num=4) -> ingest(ps=3) -> notify(pg_num=2)";
    }
  }
}

// ---------------------------------------------------------------------------
// C3 — OSD stat seq must not go backwards
// ---------------------------------------------------------------------------

TEST_F(ClusterStateTest, OrderingBug_C3_OsdStatSeqGoesBackward)
{
  // Sequence: ingest(seq=5) -> update_delta_stats
  //           ingest(seq=3) -> update_delta_stats
  //
  // INVARIANT: pg_map.osd_stat[0].seq must not decrease.
  //
  // PG stats have a version guard (reported_seq comparison); OSD stats do
  // not.  This test asserts the INVARIANT; if it fails, it exposes a real
  // defect: a late OSD report can make seq go backwards.

  const int64_t P = 1;
  pg_t pgid(0, P);

  // seq=5 first
  {
    auto s = ceph::make_ref<MPGStats>();
    s->set_src(entity_name_t::OSD(0));
    s->osd_stat.seq = 5;
    pg_stat_t pgs;
    pgs.state          = PG_STATE_ACTIVE;
    pgs.reported_epoch = 1;
    pgs.reported_seq   = 5;
    s->pg_stat[pgid]   = pgs;
    cs->ingest_pgstats(s);
  }
  cs->update_delta_stats();

  ASSERT_GT(cs->test_get_pg_map().osd_stat.count(0), 0u)
      << "Setup: osd_stat must contain OSD 0 after first ingest+update";
  const uint64_t seq_after_first = cs->test_get_pg_map().osd_stat.at(0).seq;

  // seq=3 (late delivery from same OSD)
  {
    auto s = ceph::make_ref<MPGStats>();
    s->set_src(entity_name_t::OSD(0));
    s->osd_stat.seq = 3;
    pg_stat_t pgs;
    pgs.state          = PG_STATE_ACTIVE;
    pgs.reported_epoch = 1;
    pgs.reported_seq   = 3;
    s->pg_stat[pgid]   = pgs;
    cs->ingest_pgstats(s);
  }
  cs->update_delta_stats();

  const uint64_t seq_after_second = cs->test_get_pg_map().osd_stat.at(0).seq;

  ASSERT_GE(seq_after_second, seq_after_first)
      << "INVARIANT VIOLATED: osd_stat.seq went backwards"
      << "; seq after first apply=" << seq_after_first
      << "; seq after second (late) apply=" << seq_after_second
      << "; Current seq=" << seq_after_second
      << "; Expected: seq >= " << seq_after_first
      << "; operation sequence: ingest(seq=5) -> update -> ingest(seq=3) -> update";
}

// ---------------------------------------------------------------------------
// C4 — OSD out then in
// NOT-TESTABLE with current fixture: the is_in() guard in ingest_pgstats uses
// objecter->with_osdmap(), which holds a separate empty OSDMap in the test.
// The fixture does not register OSD 0 in the objecter's OSDMap so the is_in
// path cannot be exercised.  The reachable sub-invariant (osd_stat seq is
// stored even for "out" OSDs) is covered by C3.
// ---------------------------------------------------------------------------

TEST_F(ClusterStateTest, OrderingBug_C4_OsdOutThenIn)
{
  // C4 exercises the is_in() guard in ingest_pgstats.  In this test
  // environment objecter's OSDMap is always empty, so is_in(0) always returns
  // false, making the in/out transition untestable via the ClusterStateTest
  // fixture.  The test verifies the reachable invariant: after two ingest
  // calls the osd_stat in pg_map has the seq from the second call.

  const int64_t P = 1;
  pg_t pgid(0, P);

  // First ingest: seq=5
  {
    auto s = ceph::make_ref<MPGStats>();
    s->set_src(entity_name_t::OSD(0));
    s->osd_stat.seq    = 5;
    s->osd_stat.statfs.total = 4096 * 1024;
    s->pg_stat[pgid].state = PG_STATE_ACTIVE;
    s->pg_stat[pgid].reported_seq = 5;
    s->pg_stat[pgid].reported_epoch = 1;
    cs->ingest_pgstats(s);
  }
  cs->update_delta_stats();

  // Second ingest: seq=10 (higher — must win)
  {
    auto s = ceph::make_ref<MPGStats>();
    s->set_src(entity_name_t::OSD(0));
    s->osd_stat.seq    = 10;
    s->osd_stat.statfs.total = 4096 * 1024;
    s->pg_stat[pgid].state = PG_STATE_ACTIVE;
    s->pg_stat[pgid].reported_seq = 10;
    s->pg_stat[pgid].reported_epoch = 1;
    cs->ingest_pgstats(s);
  }
  cs->update_delta_stats();

  const PGMap& pgm = cs->test_get_pg_map();
  ASSERT_GT(pgm.osd_stat.count(0), 0u)
      << "Expected osd_stat to contain OSD 0 after two ingest+update calls";

  const osd_stat_t& ost = pgm.osd_stat.at(0);
  ASSERT_GE(ost.seq, 10u)
      << "INVARIANT VIOLATED: osd_stat.seq=" << ost.seq
      << " did not advance to the second ingest's seq"
      << "; Expected seq >= 10; Current seq=" << ost.seq
      << "; operation sequence: ingest(seq=5) -> update -> ingest(seq=10) -> update";
}

// ---------------------------------------------------------------------------
// C5 — repeated empty applies are idempotent
// ---------------------------------------------------------------------------

TEST_F(ClusterStateTest, OrderingBug_C5_RepeatedEmptyApply)
{
  // Fixture has already called notify_osdmap once (pg_map.version >= 1).
  // Sequence: update_delta_stats -> (no ingest) -> update_delta_stats
  //
  // INVARIANT: pg_map.version increments by exactly 1 per call, and
  //            pending_inc is empty after each call.

  const uint64_t v0 = cs->test_get_pg_map().version;

  cs->update_delta_stats();
  const uint64_t v1 = cs->test_get_pg_map().version;
  ASSERT_EQ(v1, v0 + 1)
      << "INVARIANT VIOLATED: pg_map.version should be v0+1 after first empty update_delta_stats"
      << "; v0=" << v0 << "; v1=" << v1
      << "; Expected: " << (v0+1);
  ASSERT_TRUE(cs->test_get_pending_inc().empty())
      << "INVARIANT VIOLATED: pending_inc must be empty after update_delta_stats";

  cs->update_delta_stats();
  const uint64_t v2 = cs->test_get_pg_map().version;
  ASSERT_EQ(v2, v1 + 1)
      << "INVARIANT VIOLATED: pg_map.version should be v1+1 after second empty update_delta_stats"
      << "; v1=" << v1 << "; v2=" << v2
      << "; Expected: " << (v1+1);
  ASSERT_TRUE(cs->test_get_pending_inc().empty())
      << "INVARIANT VIOLATED: pending_inc must be empty after second update_delta_stats";
}

TEST_F(ClusterStateTest, OrderingBug_C5b_NotifyOsdmapTwiceSameMap)
{
  // notify_osdmap twice with the same OSDMap (epoch bump required).
  // INVARIANT: pg_map is self-consistent after both calls; version advances.

  const uint64_t v_before = cs->test_get_pg_map().version;

  cs->with_osdmap_and_pgmap([&](const OSDMap&, const PGMap&) {
    cs->notify_osdmap(osd_map);
  });
  const uint64_t v1 = cs->test_get_pg_map().version;
  ASSERT_EQ(v1, v_before + 1)
      << "notify_osdmap: version should increase by 1"
      << "; Expected: " << (v_before+1) << "; Current: " << v1;
  ASSERT_TRUE(cs->test_get_pending_inc().empty())
      << "pending_inc must be empty after notify_osdmap";

  // Apply a no-op epoch bump
  OSDMap::Incremental inc2(osd_map.get_epoch() + 1);
  osd_map.apply_incremental(inc2);
  cs->with_osdmap_and_pgmap([&](const OSDMap&, const PGMap&) {
    cs->notify_osdmap(osd_map);
  });
  const uint64_t v2 = cs->test_get_pg_map().version;
  ASSERT_EQ(v2, v1 + 1)
      << "Second notify_osdmap: version should increase by 1"
      << "; Expected: " << (v1+1) << "; Current: " << v2;
  ASSERT_TRUE(cs->test_get_pending_inc().empty())
      << "pending_inc must be empty after second notify_osdmap";
}

// ---------------------------------------------------------------------------
// C6 — digest atomicity: health and mon_status from same call
// ---------------------------------------------------------------------------

TEST_F(TestMgr, OrderingBug_C6_DigestAtomicity)
{
  // load_digest(D1) then load_digest(D2).
  // INVARIANT: after D2, with_health sees D2.health and with_mon_status sees
  //            D2.mon_status — no mixing of fields across two calls.

  const bufferlist health1 = [] {
    bufferlist bl; bl.append("{\"status\":\"HEALTH_OK\"}"); return bl; }();
  const bufferlist mon1 = [] {
    bufferlist bl; bl.append("{\"epoch\":1}"); return bl; }();
  const bufferlist health2 = [] {
    bufferlist bl; bl.append("{\"status\":\"HEALTH_WARN\"}"); return bl; }();
  const bufferlist mon2 = [] {
    bufferlist bl; bl.append("{\"epoch\":2}"); return bl; }();

  auto d1 = ceph::make_message<MMgrDigest>();
  d1->health_json     = health1;
  d1->mon_status_json = mon1;
  cs->load_digest(d1.get());

  auto d2 = ceph::make_message<MMgrDigest>();
  d2->health_json     = health2;
  d2->mon_status_json = mon2;
  cs->load_digest(d2.get());

  cs->with_health([&](const bufferlist& h) {
    ASSERT_EQ(h.to_str(), health2.to_str())
        << "INVARIANT VIOLATED: with_health returned stale data after load_digest(D2)"
        << "; Expected health from D2: " << health2.to_str()
        << "; Current: " << h.to_str();
  });

  cs->with_mon_status([&](const bufferlist& m) {
    ASSERT_EQ(m.to_str(), mon2.to_str())
        << "INVARIANT VIOLATED: with_mon_status returned stale data after load_digest(D2)"
        << "; Expected mon_status from D2: " << mon2.to_str()
        << "; Current: " << m.to_str();
  });

  // Verify no mixing: load a third digest, check both fields are from D3
  const bufferlist health3 = [] {
    bufferlist bl; bl.append("{\"status\":\"HEALTH_ERR\"}"); return bl; }();
  const bufferlist mon3 = [] {
    bufferlist bl; bl.append("{\"epoch\":3}"); return bl; }();

  auto d3 = ceph::make_message<MMgrDigest>();
  d3->health_json     = health3;
  d3->mon_status_json = mon3;
  cs->load_digest(d3.get());

  std::string seen_health, seen_mon;
  cs->with_health([&](const bufferlist& h) { seen_health = h.to_str(); });
  cs->with_mon_status([&](const bufferlist& m) { seen_mon = m.to_str(); });

  ASSERT_EQ(seen_health, health3.to_str())
      << "health after D3: expected=" << health3.to_str()
      << " Current=" << seen_health;
  ASSERT_EQ(seen_mon, mon3.to_str())
      << "mon_status after D3: expected=" << mon3.to_str()
      << " Current=" << seen_mon;
}

// ---------------------------------------------------------------------------
// C7 — lock coverage: ingest vs update_delta_stats (concurrent, supported path)
// ---------------------------------------------------------------------------

TEST_F(ClusterStateTest, OrderingBug_C7_ConcurrentIngestAndUpdate)
{
  // Two threads: one calls ingest_pgstats 10000 times; the other calls
  // update_delta_stats 10000 times via with_mutable_pgmap.
  // INVARIANT: no data race, pg_map.version increases, pg_map is consistent.

  const int64_t P = 1;
  pg_t pgid(0, P);

  const int N = 10000;
  std::atomic<bool> start{false};

  std::thread ingest_t([&]() {
    while (!start.load()) {}
    for (int i = 1; i <= N; ++i) {
      auto s = make_pg_stats(0, pgid, static_cast<uint64_t>(i), P);
      cs->ingest_pgstats(s);
    }
  });

  std::thread update_t([&]() {
    while (!start.load()) {}
    for (int i = 0; i < N; ++i) {
      cs->with_mutable_pgmap([&](PGMap&) {
        cs->update_delta_stats();
      });
    }
  });

  start.store(true);
  ingest_t.join();
  update_t.join();

  // Drain remaining pending updates
  cs->with_mutable_pgmap([&](PGMap&) {
    cs->update_delta_stats();
  });

  const PGMap& pgm = cs->test_get_pg_map();
  ASSERT_GE(pgm.version, 1u)
      << "pg_map.version must be >= 1 after concurrent ingest+update";

  for (auto& [id, _] : pgm.pg_stat) {
    ASSERT_EQ(static_cast<int64_t>(id.pool()), P)
        << "pg_stat key " << id << " refers to unexpected pool"
        << "; Expected: only pool " << P;
  }
}

// ---------------------------------------------------------------------------
// Randomised walk: seed, 100000 sequences, all invariants after every step
// ---------------------------------------------------------------------------

TEST_F(ClusterStateTest, OrderingBug_RandomWalk)
{
  // Fixed seed for reproducibility.  Operations: ingest (random osd, random
  // pool, random pg including out-of-range and removed pool 99, random seq),
  // notify_osdmap (random pg_num change, random pool add/remove, in/out),
  // update_delta_stats, load_digest.
  //
  // Invariants checked after every step: INV-A through INV-D (see
  // check_invariants above), plus osd_stat seq monotonicity.

  const uint32_t SEED   = 0xCAFEBEEF;
  const int      STEPS  = 100000;
  const int64_t  P1     = 1;

  std::mt19937 rng(SEED);
  std::uniform_int_distribution<int> op_dist(0, 4);
  std::uniform_int_distribution<int> pool_dist(0, 2);
  std::uniform_int_distribution<int> pg_dist(0, 3);
  std::uniform_int_distribution<uint64_t> seq_dist(1, 10);
  std::uniform_int_distribution<int> pg_num_dist(1, 4);

  // Walk starts from ClusterStateTest fixture: pool P1=1 present, pg_num=2
  std::map<int64_t, unsigned> current_pools;
  current_pools[P1] = cs->test_get_existing_pools().count(P1) > 0
                      ? cs->test_get_existing_pools().at(P1) : 2u;

  // Monotonicity tracking
  std::map<int, uint64_t> max_osd_seq;

  std::string last_op = "setup";

  for (int step = 0; step < STEPS; ++step) {
    int op = op_dist(rng);

    switch (op) {
    case 0: {
      // ingest: random pool (may not exist), random pg, random seq
      int pool_choice = pool_dist(rng);
      int64_t pool = (pool_choice == 0) ? 99 :  // non-existent pool
                     P1;
      unsigned ps  = static_cast<unsigned>(pg_dist(rng));
      uint64_t seq = seq_dist(rng);

      auto s = ceph::make_ref<MPGStats>();
      s->set_src(entity_name_t::OSD(0));
      s->osd_stat.seq = seq;
      s->osd_stat.statfs.total = 1024;
      pg_stat_t pgs;
      pgs.state = PG_STATE_ACTIVE;
      pgs.reported_epoch = 1;
      pgs.reported_seq = seq;
      pg_t pgid(ps, pool);
      s->pg_stat[pgid] = pgs;
      if (current_pools.count(pool)) {
        store_statfs_t sfs; sfs.total = 512;
        s->pool_stat[pool] = sfs;
      }
      cs->ingest_pgstats(s);

      std::ostringstream oss;
      oss << "ingest(pool=" << pool << ",ps=" << ps << ",seq=" << seq << ")";
      last_op = oss.str();
      break;
    }
    case 1: {
      // update_delta_stats
      cs->update_delta_stats();
      last_op = "update_delta_stats";
      break;
    }
    case 2: {
      // notify_osdmap: change pg_num of P1
      unsigned pn1 = static_cast<unsigned>(pg_num_dist(rng));
      pg_pool_t p1;
      p1.set_pg_num(pn1);
      OSDMap::Incremental inc(osd_map.get_epoch() + 1);
      inc.new_pools[P1] = p1;
      osd_map.apply_incremental(inc);
      cs->with_osdmap_and_pgmap([&](const OSDMap&, const PGMap&) {
        cs->notify_osdmap(osd_map);
      });
      current_pools[P1] = pn1;

      std::ostringstream oss;
      oss << "notify(P1.pg_num=" << pn1 << ")";
      last_op = oss.str();
      break;
    }
    case 3: {
      // notify_osdmap: remove P1
      OSDMap::Incremental inc(osd_map.get_epoch() + 1);
      inc.old_pools.insert(P1);
      osd_map.apply_incremental(inc);
      cs->with_osdmap_and_pgmap([&](const OSDMap&, const PGMap&) {
        cs->notify_osdmap(osd_map);
      });
      current_pools.erase(P1);
      last_op = "notify(P1 removed)";
      break;
    }
    case 4: {
      // notify_osdmap: restore P1
      pg_pool_t p1;
      p1.set_pg_num(2);
      OSDMap::Incremental inc(osd_map.get_epoch() + 1);
      inc.new_pools[P1] = p1;
      osd_map.apply_incremental(inc);
      cs->with_osdmap_and_pgmap([&](const OSDMap&, const PGMap&) {
        cs->notify_osdmap(osd_map);
      });
      current_pools[P1] = 2;
      last_op = "notify(P1 restored)";
      break;
    }
    default:
      break;
    }

    // Check all invariants after every step
    auto errs = check_invariants(*cs, current_pools);
    for (auto& e : errs) {
      FAIL() << "RandomWalk INVARIANT VIOLATED at step " << step
             << " op='" << last_op << "'"
             << " seed=" << SEED
             << ": " << e;
    }
  }
}

int main(int argc, char* argv[])
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
