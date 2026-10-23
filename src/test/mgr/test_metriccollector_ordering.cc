// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
//
// Ordering / invariant tests for MetricCollector, OSDPerfMetricCollector and
// MDSPerfMetricCollector.
//
// Each test replays one fixed operation sequence and asserts an INVARIANT,
// not the current behaviour.  A failing test is the desired outcome when the
// invariant is violated.  Do not weaken or disable assertions to make tests
// pass.

#include <atomic>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "global/global_context.h"
#include "global/global_init.h"
#include "gtest/gtest.h"

#include "include/buffer.h"
#include "mgr/MDSPerfMetricCollector.h"
#include "mgr/MDSPerfMetricTypes.h"
#include "mgr/MetricTypes.h"
#include "mgr/OSDPerfMetricCollector.h"
#include "mgr/OSDPerfMetricTypes.h"
#include "mgr/Types.h"

// ---------------------------------------------------------------------------
// Global initialisation (one-time, shared across all tests).
// ---------------------------------------------------------------------------

namespace {

void init_ceph_context()
{
  static boost::intrusive_ptr<CephContext> cct;
  if (cct) return;
  std::vector<const char*> args = {"unittest_mgr_metriccollector_ordering"};
  cct = global_init(nullptr, args, CEPH_ENTITY_TYPE_CLIENT,
                    CODE_ENVIRONMENT_UTILITY,
                    CINIT_FLAG_NO_DEFAULT_CONFIG_FILE);
  cct->_conf.set_val("auth_client_required", "none");
  cct->_conf.set_val("auth_cluster_required", "none");
  cct->_conf.set_val("auth_service_required", "none");
  cct->_conf.apply_changes(nullptr);
  common_init_finish(cct.get());
}

// ---------------------------------------------------------------------------
// Test stub listener.
// ---------------------------------------------------------------------------

class CountingListener : public MetricListener {
public:
  std::atomic<int> call_count{0};
  // Optional re-entrant action executed inside handle_query_updated.
  std::function<void()> reentrant_action;

  void handle_query_updated() override {
    call_count.fetch_add(1, std::memory_order_relaxed);
    if (reentrant_action) {
      reentrant_action();
    }
  }
};

// ---------------------------------------------------------------------------
// Helper: build the smallest valid OSDPerfMetricQuery (empty key descriptor,
// one OPS counter descriptor).
// ---------------------------------------------------------------------------

PerformanceCounterDescriptor ops_desc()
{
  return PerformanceCounterDescriptor(PerformanceCounterType::OPS);
}

OSDPerfMetricQuery make_osd_query_ops()
{
  OSDPerfMetricKeyDescriptor key_desc;
  PerformanceCounterDescriptors perf_descs = {ops_desc()};
  return OSDPerfMetricQuery(key_desc, perf_descs);
}

OSDPerfMetricQuery make_osd_query_bytes()
{
  OSDPerfMetricKeyDescriptor key_desc;
  PerformanceCounterDescriptors perf_descs = {
    PerformanceCounterDescriptor(PerformanceCounterType::BYTES)};
  return OSDPerfMetricQuery(key_desc, perf_descs);
}

// Build a correctly packed OSDPerfMetricReport for one key with given counter
// values.  The report descriptors mirror the query descriptors exactly unless
// overridden.
OSDPerfMetricReport make_osd_report(
    const OSDPerfMetricQuery& query,
    const OSDPerfMetricKey& key,
    const PerformanceCounters& counter_values,
    std::optional<PerformanceCounterDescriptors> override_descs = std::nullopt)
{
  OSDPerfMetricReport report;
  report.performance_counter_descriptors =
    override_descs.value_or(query.performance_counter_descriptors);

  ceph::bufferlist bl;
  auto desc_it = report.performance_counter_descriptors.begin();
  for (size_t i = 0; i < report.performance_counter_descriptors.size();
       ++i, ++desc_it) {
    PerformanceCounter c{0, 0};
    if (i < counter_values.size()) {
      c = counter_values[i];
    }
    desc_it->pack_counter(c, &bl);
  }
  report.group_packed_performance_counters[key] = bl;
  return report;
}

// Build an OSDMetricPayload wrapping one query/report pair.
MetricPayload make_osd_payload(const OSDPerfMetricQuery& query,
                               const OSDPerfMetricReport& report)
{
  std::map<OSDPerfMetricQuery, OSDPerfMetricReport> m;
  m.emplace(query, report);
  return OSDMetricPayload(m);
}

// Build a truncated (too-short) bufferlist for an OPS counter — encodes
// fewer bytes than decode expects.
ceph::bufferlist make_truncated_ops_bl()
{
  ceph::bufferlist bl;
  // OPS counter: pack_counter writes uint64_t (8 bytes). Provide only 3.
  char buf[3] = {0x01, 0x02, 0x03};
  bl.append(buf, sizeof(buf));
  return bl;
}

// MDSPerfMetricQuery helpers — empty key descriptor, one CAP_HIT_METRIC
// counter descriptor.
MDSPerfMetricQuery make_mds_query()
{
  MDSPerfMetricKeyDescriptor key_desc;
  MDSPerformanceCounterDescriptors perf_descs = {
    MDSPerformanceCounterDescriptor(MDSPerformanceCounterType::CAP_HIT_METRIC)};
  return MDSPerfMetricQuery(key_desc, perf_descs);
}

// Produce a valid MDSMetricPayload for one query/key.
MetricPayload make_mds_payload(const MDSPerfMetricQuery& query,
                               const MDSPerfMetricKey& key,
                               uint64_t first_val)
{
  MDSPerfMetrics metrics;
  metrics.performance_counter_descriptors =
    query.performance_counter_descriptors;

  ceph::bufferlist bl;
  MDSPerformanceCounterDescriptor cap_desc(
    MDSPerformanceCounterType::CAP_HIT_METRIC);
  PerformanceCounter c{first_val, 0};
  cap_desc.pack_counter(c, &bl);
  metrics.group_packed_performance_counters[key] = bl;

  MDSPerfMetricReport report;
  report.reports.emplace(query, metrics);
  return MDSMetricPayload(report);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Test fixture — OSD collector.
// ---------------------------------------------------------------------------

class OSDOrderingTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() { init_ceph_context(); }

  CountingListener listener;
  OSDPerfMetricCollector collector{listener};

  OSDPerfMetricQuery q1 = make_osd_query_ops();
  OSDPerfMetricQuery q2 = make_osd_query_bytes();
  OSDPerfMetricKey key0;  // empty key (no sub-key descriptors)
};

// ---------------------------------------------------------------------------
// Test fixture — MDS collector.
// ---------------------------------------------------------------------------

class MDSOrderingTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() { init_ceph_context(); }

  CountingListener listener;
  MDSPerfMetricCollector collector{listener};

  MDSPerfMetricQuery q1 = make_mds_query();
  MDSPerfMetricKey key0;
};

// ===========================================================================
// Q1a — phantom entry blocks listener notification.
//
// Sequence: process_reports(Q) with Q not registered,
//           then add_query(Q).
//
// INVARIANT: process_reports does not change the registered query set;
// add_query for a new query always notifies the listener.
// ===========================================================================

TEST_F(OSDOrderingTest, OrderingBug_Q1a_PhantomEntryBlocksNotification)
{
  // Before any registration, send a report for q1.
  OSDPerfMetricReport report = make_osd_report(q1, key0, {{10, 0}});
  MetricPayload payload = make_osd_payload(q1, report);
  collector.process_reports(payload);

  auto queries_after_report = collector.get_queries();
  ASSERT_TRUE(queries_after_report.empty())
    << "process_reports must not insert phantom entries into get_queries()."
    << "\n  Current: get_queries() returned " << queries_after_report.size()
    << " entries after process_reports with no registered query."
    << "\n  Expected: empty map.";

  int before = listener.call_count.load();
  MetricQueryID id = collector.add_query(q1, std::nullopt);
  int after = listener.call_count.load();

  EXPECT_GT(after, before)
    << "add_query(Q) for a newly registered query must notify the listener."
    << "\n  Sequence: process_reports(Q unregistered), add_query(Q)."
    << "\n  Current: listener call_count before=" << before
    << " after=" << after << " (no notification fired)."
    << "\n  Expected: after > before (one notification).";

  // Cleanup: remove query so fixture is clean.
  collector.remove_query(id);
}

// ===========================================================================
// Q1b — late process_reports re-injects phantom after remove.
//
// Sequence: add_query(Q) → remove_query(N) → process_reports(Q).
//           Then add_query(Q) again.
//
// INVARIANT: process_reports never modifies queries; a subsequent add_query
// for a truly-new query must notify.
// ===========================================================================

TEST_F(OSDOrderingTest, OrderingBug_Q1b_LateReportReInjectsQuery)
{
  MetricQueryID id = collector.add_query(q1, std::nullopt);
  collector.remove_query(id);

  ASSERT_TRUE(collector.get_queries().empty())
    << "After add_query + remove_query, get_queries() must be empty."
    << "\n  Current: " << collector.get_queries().size() << " entries."
    << "\n  Expected: 0 entries.";

  // Late report arrives for q1, which is no longer registered.
  OSDPerfMetricReport report = make_osd_report(q1, key0, {{5, 0}});
  collector.process_reports(make_osd_payload(q1, report));

  EXPECT_TRUE(collector.get_queries().empty())
    << "process_reports for an unregistered query must not change get_queries()."
    << "\n  Sequence: add_query(Q), remove_query(N), process_reports(Q)."
    << "\n  Current: get_queries() returned " << collector.get_queries().size()
    << " entries."
    << "\n  Expected: 0 entries.";

  int before = listener.call_count.load();
  MetricQueryID id2 = collector.add_query(q1, std::nullopt);
  int after = listener.call_count.load();

  EXPECT_GT(after, before)
    << "add_query(Q) after late process_reports must notify the listener."
    << "\n  Sequence: add_query, remove_query, process_reports(Q), add_query(Q)."
    << "\n  Current: call_count before=" << before << " after=" << after
    << " (no notification)."
    << "\n  Expected: after > before.";

  collector.remove_query(id2);
}

// ===========================================================================
// Q2 — stale counters after remove_all_queries.
//
// Sequence: add_query(Q) → process_reports(Q) → remove_all_queries()
//           → get_counters(old_id).
//
// INVARIANT (INV-A): after remove_all_queries, no counter entry exists for
// any previously registered id.  get_counters must return -ENOENT.
// ===========================================================================

TEST_F(OSDOrderingTest, OrderingBug_Q2_StaleCountersAfterRemoveAll)
{
  MetricQueryID id = collector.add_query(q1, std::nullopt);

  OSDPerfMetricReport report = make_osd_report(q1, key0, {{42, 0}});
  collector.process_reports(make_osd_payload(q1, report));

  collector.remove_all_queries();

  OSDPerfCollector perf_collector(id);
  int r = collector.get_counters(&perf_collector);

  EXPECT_EQ(r, -ENOENT)
    << "After remove_all_queries, get_counters for the old id must return "
    << "-ENOENT (INV-A: queries and counters key sets must agree)."
    << "\n  Sequence: add_query(Q), process_reports, remove_all_queries,"
    << " get_counters(id)."
    << "\n  Current: get_counters returned " << r
    << " (0 = success → stale counter entry found)."
    << "\n  Expected: -ENOENT (" << -ENOENT << ").";
}

// ===========================================================================
// Q3a — remove then get_counters returns ENOENT.
//
// Sequence: add_query(Q) → remove_query(N) → get_counters(N).
//
// INVARIANT (INV-D): get_counters after remove_query returns -ENOENT.
// ===========================================================================

TEST_F(OSDOrderingTest, OrderingBug_Q3a_RemoveThenGetCounters)
{
  MetricQueryID id = collector.add_query(q1, std::nullopt);
  int remove_r = collector.remove_query(id);

  ASSERT_EQ(remove_r, 0)
    << "remove_query for a registered id must return 0."
    << "\n  Current: " << remove_r << "  Expected: 0.";

  OSDPerfCollector perf_collector(id);
  int r = collector.get_counters(&perf_collector);

  EXPECT_EQ(r, -ENOENT)
    << "get_counters after remove_query must return -ENOENT (INV-D)."
    << "\n  Sequence: add_query(Q), remove_query(id), get_counters(id)."
    << "\n  Current: " << r << "  Expected: " << -ENOENT << ".";
}

// ===========================================================================
// Q3b — get_counters drains on first call, empty on second.
//
// INVARIANT (INV-C): first get_counters returns non-empty counters;
// second get_counters returns empty map, return value 0.
// ===========================================================================

TEST_F(OSDOrderingTest, OrderingBug_Q3b_GetCountersDrainsOnce)
{
  MetricQueryID id = collector.add_query(q1, std::nullopt);

  OSDPerfMetricReport report = make_osd_report(q1, key0, {{99, 0}});
  collector.process_reports(make_osd_payload(q1, report));

  OSDPerfCollector c1(id);
  int r1 = collector.get_counters(&c1);

  ASSERT_EQ(r1, 0)
    << "First get_counters after process_reports must succeed."
    << "\n  Current: " << r1 << "  Expected: 0.";
  EXPECT_FALSE(c1.counters.empty())
    << "First get_counters must return non-empty counters (report was processed)."
    << "\n  Sequence: add_query, process_reports, get_counters (first call)."
    << "\n  Current: counters is empty.  Expected: non-empty.";

  OSDPerfCollector c2(id);
  int r2 = collector.get_counters(&c2);

  EXPECT_EQ(r2, 0)
    << "Second get_counters for same id must return 0 (id still registered)."
    << "\n  Current: " << r2 << "  Expected: 0.";
  EXPECT_TRUE(c2.counters.empty())
    << "Second get_counters must return empty map (drain-and-reset, INV-C)."
    << "\n  Sequence: add_query, process_reports, get_counters x2."
    << "\n  Current: second call returned " << c2.counters.size()
    << " entries.  Expected: 0.";

  collector.remove_query(id);
}

// ===========================================================================
// Q4 — notification accounting per established invariant (INV-E).
//
// The invariant (from MetricCollector.cc add_query, remove_query):
//   add_query notifies iff the query is brand-new OR existing registrations
//   are already limited.
//   remove_query notifies iff the query bucket becomes empty OR remaining
//   registrations are limited.
//
// Sequences tested:
//   (a1) add_query(Q, nullopt) — new query → notify.
//   (a2) add_query(Q, nullopt) again — existing, all unlimited → no notify.
//   (b)  add limited registration to all-unlimited Q → no notify (BY-DESIGN:
//        existing are not limited, so is_limited(existing)=false).
//   (c)  remove the limited id — remaining all-unlimited, bucket non-empty →
//        no notify (BY-DESIGN: is_limited(remaining)=false, not empty).
//   (d)  remove an unlimited id — another unlimited remains → no notify.
//   (e)  remove last id — bucket empty → notify.
// ===========================================================================

TEST_F(OSDOrderingTest, OrderingBug_Q4_NotificationAccounting)
{
  OSDPerfMetricLimit limit(ops_desc(), 10);

  // (a1): first add_query — new query → notify.
  int calls0 = listener.call_count.load();
  MetricQueryID id1 = collector.add_query(q1, std::nullopt);
  EXPECT_GT(listener.call_count.load(), calls0)
    << "First add_query(Q, nullopt) must notify (new query, INV-E)."
    << "\n  call_count before=" << calls0
    << " after=" << listener.call_count.load();

  // (a2): second add_query(Q, nullopt): existing, all-unlimited → no notify.
  int calls1 = listener.call_count.load();
  MetricQueryID id2 = collector.add_query(q1, std::nullopt);
  EXPECT_EQ(listener.call_count.load(), calls1)
    << "Second add_query(Q, nullopt) must NOT notify (existing, all unlimited)."
    << "\n  call_count before=" << calls1
    << " after=" << listener.call_count.load();

  // get_queries() model: {q1 -> empty Limits set (unlimited)}.
  {
    auto qs = collector.get_queries();
    ASSERT_EQ(qs.size(), 1u)
      << "get_queries() must have 1 entry after two add_query calls for same Q."
      << "\n  Current: " << qs.size() << "  Expected: 1.";
    EXPECT_TRUE(qs.at(q1).empty())
      << "Limits set must be empty for all-unlimited registrations."
      << "\n  Current size: " << qs.at(q1).size() << "  Expected: 0.";
  }

  // (b): add limited registration — existing are unlimited → is_limited(existing)=false
  // → no notify (BY-DESIGN per INV-E documented in intent artefact).
  int calls2 = listener.call_count.load();
  MetricQueryID id3 = collector.add_query(q1, limit);
  EXPECT_EQ(listener.call_count.load(), calls2)
    << "add_query(Q, limit) when existing are ALL unlimited must NOT notify "
    << "(is_limited(existing)=false, BY-DESIGN per INV-E)."
    << "\n  call_count before=" << calls2
    << " after=" << listener.call_count.load();

  // Now add_query with another limited reg → existing IS limited → notify.
  int calls2b = listener.call_count.load();
  MetricQueryID id4 = collector.add_query(q1, limit);
  EXPECT_GT(listener.call_count.load(), calls2b)
    << "add_query(Q, limit) when existing registration is ALREADY limited must notify."
    << "\n  call_count before=" << calls2b
    << " after=" << listener.call_count.load();

  // get_queries model: limits set is non-empty (limited regs exist).
  {
    auto qs = collector.get_queries();
    ASSERT_EQ(qs.size(), 1u);
    EXPECT_GE(qs.at(q1).size(), 1u)
      << "Limits set must be non-empty when limited registrations exist."
      << "\n  Current: " << qs.at(q1).size();
  }

  // (c): remove one limited id (id4) — remaining has id3 (limited) + id1, id2 (unlimited)
  // → is_limited(remaining) = true → notify.
  int calls3 = listener.call_count.load();
  int r3 = collector.remove_query(id4);
  ASSERT_EQ(r3, 0);
  EXPECT_GT(listener.call_count.load(), calls3)
    << "remove_query when remaining registrations are still limited must notify."
    << "\n  call_count before=" << calls3
    << " after=" << listener.call_count.load();

  // Remove the last limited id (id3) — remaining all unlimited → no notify.
  int calls3b = listener.call_count.load();
  int r3b = collector.remove_query(id3);
  ASSERT_EQ(r3b, 0);
  EXPECT_EQ(listener.call_count.load(), calls3b)
    << "remove_query of last limited id (remaining all unlimited) must NOT notify "
    << "(is_limited(remaining)=false, bucket non-empty, BY-DESIGN)."
    << "\n  call_count before=" << calls3b
    << " after=" << listener.call_count.load();

  // (d): remove unlimited id1 — another unlimited id2 remains → no notify.
  int calls4 = listener.call_count.load();
  int r4 = collector.remove_query(id1);
  ASSERT_EQ(r4, 0);
  EXPECT_EQ(listener.call_count.load(), calls4)
    << "remove_query of unlimited id when another unlimited remains must NOT notify."
    << "\n  call_count before=" << calls4
    << " after=" << listener.call_count.load();

  // (e): remove last id → bucket empty → notify.
  int calls5 = listener.call_count.load();
  int r5 = collector.remove_query(id2);
  ASSERT_EQ(r5, 0);
  EXPECT_GT(listener.call_count.load(), calls5)
    << "remove_query of last registration must notify (bucket erased, INV-E)."
    << "\n  call_count before=" << calls5
    << " after=" << listener.call_count.load();

  EXPECT_TRUE(collector.get_queries().empty())
    << "get_queries() must be empty after all registrations removed."
    << "\n  Current: " << collector.get_queries().size() << " entries.";
}

// ===========================================================================
// Q5a — listener re-entrancy: callback calls get_queries, add_query,
// remove_query without deadlock.
//
// INVARIANT: handle_query_updated is called outside the lock; re-entrant
// calls to the collector from within the callback must not deadlock.
// ===========================================================================

TEST_F(OSDOrderingTest, OrderingBug_Q5a_ListenerReentrancy)
{
  MetricQueryID reentrant_id = -1;
  std::atomic<bool> deadlock_detected{false};

  listener.reentrant_action = [&]() {
    // Called from inside handle_query_updated (outside lock).
    // get_queries must succeed without deadlock.
    auto qs = collector.get_queries();
    // add_query for a different query — must not deadlock.
    reentrant_id = collector.add_query(q2, std::nullopt);
  };

  MetricQueryID id = collector.add_query(q1, std::nullopt);

  // Disable further re-entrant action before remove (to avoid add/remove loop).
  listener.reentrant_action = nullptr;

  EXPECT_GE(reentrant_id, 0)
    << "Re-entrant add_query from within handle_query_updated must succeed."
    << "\n  Sequence: add_query(q1) triggers callback which calls add_query(q2)."
    << "\n  Current: reentrant_id=" << reentrant_id
    << "  Expected: >= 0 (valid id, no deadlock).";

  collector.remove_query(id);
  if (reentrant_id >= 0) {
    collector.remove_query(reentrant_id);
  }
}

// ===========================================================================
// Q5b — concurrent add/remove race.
//
// Two threads perform random add/remove operations on q1 and q2.
// After all threads complete, get_queries() must be internally consistent:
// every key must appear at most once, and the result must correspond to the
// net registrations.
//
// Repeated 10000 times with a fixed seed.
// ===========================================================================

TEST_F(OSDOrderingTest, OrderingBug_Q5b_ConcurrentAddRemoveRace)
{
  constexpr int rounds = 10000;
  std::mt19937 rng(12345);
  std::uniform_int_distribution<int> coin(0, 1);

  for (int round = 0; round < rounds; ++round) {
    CountingListener local_listener;
    OSDPerfMetricCollector local_collector{local_listener};

    MetricQueryID ids[4] = {-1, -1, -1, -1};
    std::atomic<int> slot{0};

    // Thread A: adds q1 and q2.
    std::thread t1([&]() {
      ids[0] = local_collector.add_query(q1, std::nullopt);
      ids[1] = local_collector.add_query(q2, std::nullopt);
    });
    // Thread B: adds q1 and q2 (may race with thread A).
    std::thread t2([&]() {
      ids[2] = local_collector.add_query(q1, std::nullopt);
      ids[3] = local_collector.add_query(q2, std::nullopt);
    });

    t1.join();
    t2.join();

    // All four ids must be non-negative and unique.
    std::string seq_str = "round=" + std::to_string(round);
    for (int i = 0; i < 4; ++i) {
      ASSERT_GE(ids[i], 0)
        << "add_query must return a valid (non-negative) id."
        << "\n  " << seq_str << " ids[" << i << "]=" << ids[i];
    }
    for (int i = 0; i < 4; ++i) {
      for (int j = i + 1; j < 4; ++j) {
        ASSERT_NE(ids[i], ids[j])
          << "Each add_query call must return a unique id."
          << "\n  " << seq_str << " ids[" << i << "]=" << ids[i]
          << " ids[" << j << "]=" << ids[j];
      }
    }

    // After both threads complete, get_queries must be consistent:
    // both q1 and q2 must be present (two registrations each).
    auto qs = local_collector.get_queries();
    ASSERT_EQ(qs.size(), 2u)
      << "After two threads each adding q1 and q2, get_queries must have 2 keys."
      << "\n  " << seq_str << " Current: " << qs.size() << "  Expected: 2.";
    ASSERT_TRUE(qs.count(q1) > 0)
      << "q1 must be in get_queries after concurrent adds."
      << "\n  " << seq_str;
    ASSERT_TRUE(qs.count(q2) > 0)
      << "q2 must be in get_queries after concurrent adds."
      << "\n  " << seq_str;
  }
}

// ===========================================================================
// Gap G1 — empty payload: process_reports with empty reports map is a no-op.
//
// INVARIANT: empty reports map causes process_reports_generic to return early;
// queries and counters must be unchanged.
// ===========================================================================

TEST_F(OSDOrderingTest, OrderingBug_G1_EmptyReportNoOp)
{
  MetricQueryID id = collector.add_query(q1, std::nullopt);
  size_t qs_size_before = collector.get_queries().size();

  MetricPayload empty_payload = OSDMetricPayload(
    std::map<OSDPerfMetricQuery, OSDPerfMetricReport>{});
  collector.process_reports(empty_payload);

  EXPECT_EQ(collector.get_queries().size(), qs_size_before)
    << "process_reports with empty reports map must not change get_queries()."
    << "\n  Sequence: add_query(Q), process_reports({})."
    << "\n  Current: " << collector.get_queries().size()
    << "  Expected: " << qs_size_before;

  OSDPerfCollector perf(id);
  int r = collector.get_counters(&perf);
  ASSERT_EQ(r, 0) << "get_counters after empty process_reports must return 0.";
  EXPECT_TRUE(perf.counters.empty())
    << "Counters must be empty after empty process_reports."
    << "\n  Current: " << perf.counters.size() << " entries.";

  collector.remove_query(id);
}

// ===========================================================================
// Gap G2 — get_counters before any process_reports: returns 0, empty map.
//
// INVARIANT: counter entry is allocated by add_query; get_counters before any
// report returns 0 with empty counters map.
// ===========================================================================

TEST_F(OSDOrderingTest, OrderingBug_G2_GetCountersBeforeReport)
{
  MetricQueryID id = collector.add_query(q1, std::nullopt);

  OSDPerfCollector perf(id);
  int r = collector.get_counters(&perf);

  ASSERT_EQ(r, 0)
    << "get_counters before any process_reports must return 0."
    << "\n  Sequence: add_query(Q), get_counters(id) [no report]."
    << "\n  Current: " << r;
  EXPECT_TRUE(perf.counters.empty())
    << "Counters must be empty before any process_reports."
    << "\n  Current: " << perf.counters.size() << "  Expected: 0.";

  collector.remove_query(id);
}

// ===========================================================================
// Gap G3 — remove_query twice: second call returns -ENOENT.
//
// INVARIANT: remove_query for an already-removed id returns -ENOENT and
// does not notify the listener.
// ===========================================================================

TEST_F(OSDOrderingTest, OrderingBug_G3_RemoveQueryTwice)
{
  MetricQueryID id = collector.add_query(q1, std::nullopt);
  int r1 = collector.remove_query(id);
  ASSERT_EQ(r1, 0) << "First remove_query must return 0.";

  int calls_before = listener.call_count.load();
  int r2 = collector.remove_query(id);

  EXPECT_EQ(r2, -ENOENT)
    << "Second remove_query for already-removed id must return -ENOENT."
    << "\n  Sequence: add_query, remove_query(id), remove_query(id)."
    << "\n  Current: " << r2 << "  Expected: " << -ENOENT;
  EXPECT_EQ(listener.call_count.load(), calls_before)
    << "remove_query for unknown id must NOT notify listener."
    << "\n  call_count before=" << calls_before
    << " after=" << listener.call_count.load();
}

// ===========================================================================
// Q6 — truncated buffer causes exception to escape process_reports.
//
// INVARIANT (INV-F): if a packed counter buffer is too short,
// unpack_counter throws ceph::buffer::error, which escapes
// process_reports_generic and process_reports.
// ===========================================================================

TEST_F(OSDOrderingTest, OrderingBug_Q6_TruncatedBuffer)
{
  MetricQueryID id = collector.add_query(q1, std::nullopt);

  // Build a report with a truncated bufferlist (3 bytes instead of 8).
  OSDPerfMetricReport bad_report;
  bad_report.performance_counter_descriptors = {ops_desc()};
  bad_report.group_packed_performance_counters[key0] = make_truncated_ops_bl();

  MetricPayload payload = make_osd_payload(q1, bad_report);

  // INVARIANT: exception escapes process_reports.
  bool threw = false;
  try {
    collector.process_reports(payload);
  } catch (const ceph::buffer::error&) {
    threw = true;
  } catch (const std::exception& e) {
    threw = true;  // also accept std::exception subclasses
    ADD_FAILURE() << "process_reports threw std::exception (not buffer::error):"
                  << e.what()
                  << "\n  Sequence: add_query(Q), process_reports(truncated)."
                  << "\n  Current: wrong exception type."
                  << "\n  Expected: ceph::buffer::error.";
  }

  EXPECT_TRUE(threw)
    << "process_reports with a truncated counter buffer must throw "
    << "(unpack_counter throws ceph::buffer::error, no catch in the call chain)."
    << "\n  Sequence: add_query(Q), process_reports(Q, truncated_bl)."
    << "\n  Current: no exception thrown."
    << "\n  Expected: ceph::buffer::error thrown.";

  collector.remove_query(id);
}

// ===========================================================================
// Q7 — descriptor mismatch: report has subset of query descriptors.
//
// INVARIANT (INV-G): counter slots for descriptors absent from the report
// must remain zero; present slots must be updated.
// ===========================================================================

TEST_F(OSDOrderingTest, OrderingBug_Q7_DescriptorMismatch)
{
  // Query with two descriptors: OPS and BYTES.
  OSDPerfMetricKeyDescriptor key_desc;
  PerformanceCounterDescriptors two_descs = {
    PerformanceCounterDescriptor(PerformanceCounterType::OPS),
    PerformanceCounterDescriptor(PerformanceCounterType::BYTES)};
  OSDPerfMetricQuery q_two(key_desc, two_descs);

  MetricQueryID id = collector.add_query(q_two, std::nullopt);

  // Report contains only OPS (not BYTES).
  PerformanceCounterDescriptors report_descs = {
    PerformanceCounterDescriptor(PerformanceCounterType::OPS)};
  PerformanceCounters report_vals = {{7, 0}};
  OSDPerfMetricReport report = make_osd_report(q_two, key0, report_vals, report_descs);

  collector.process_reports(make_osd_payload(q_two, report));

  OSDPerfCollector perf(id);
  int r = collector.get_counters(&perf);

  ASSERT_EQ(r, 0)
    << "get_counters must succeed after processing a partial-descriptor report."
    << "\n  Current: " << r;

  ASSERT_TRUE(perf.counters.count(key0) > 0)
    << "Counter entry for the key must exist."
    << "\n  Sequence: add_query(Q with OPS+BYTES), process_reports(Q with OPS only).";

  const PerformanceCounters& c = perf.counters.at(key0);
  ASSERT_EQ(c.size(), 2u)
    << "Counter vector must have 2 entries (one per query descriptor)."
    << "\n  Current: " << c.size() << "  Expected: 2.";

  EXPECT_EQ(c[0].first, 7u)
    << "OPS counter (index 0) must be 7 (matched descriptor from report)."
    << "\n  Sequence: add_query(OPS+BYTES), process_reports(OPS=7 only)."
    << "\n  Current: " << c[0].first << "  Expected: 7.";
  EXPECT_EQ(c[1].first, 0u)
    << "BYTES counter (index 1) must be 0 (no matching descriptor in report, INV-G)."
    << "\n  Current: " << c[1].first << "  Expected: 0.";

  collector.remove_query(id);
}

// ===========================================================================
// Q8 — MDS: get_counters before any process_reports returns garbage timestamp.
//
// INVARIANT (INV-H / REAL-KNOWN DIV-1, DIV-2):
// MDSPerfMetricCollector::last_updated_mono is not initialised in the
// constructor.  get_counters before the first process_reports writes
// whatever is in uninitialised memory into MDSPerfCollector::last_updated_mono.
//
// The test documents the DIVERGED behaviour; it does NOT assert a specific
// timestamp value (garbage) — it asserts that last_updated_mono is NOT the
// zero epoch value, which a correctly-initialised zero-init would produce.
// ===========================================================================

TEST_F(MDSOrderingTest, OrderingBug_Q8_MDSUninitTimestamp)
{
  MetricQueryID id = collector.add_query(q1, std::nullopt);

  MDSPerfCollector perf(id);
  int r = collector.get_counters(&perf);

  ASSERT_EQ(r, 0)
    << "get_counters before any process_reports must succeed (id is registered)."
    << "\n  Current: " << r;

  // Document: the field is uninitialized. We cannot assert its value.
  // The DIVERGED finding is documented; this test confirms the code path
  // runs without crashing and records what the value looks like.
  // A passing test here just means "no crash"; the finding is REAL-KNOWN.
  SUCCEED() << "Q8 path reached: last_updated_mono after construction (no "
               "process_reports) = "
            << perf.last_updated_mono.sec() << "s "
            << perf.last_updated_mono.nsec() << "ns. "
               "This may be garbage (REAL-KNOWN DIV-1/DIV-2 in "
               "MDSPerfMetricCollector-intent.md).";

  // Also verify that after process_reports the timestamp is non-zero.
  collector.process_reports(make_mds_payload(q1, key0, 1));

  MDSPerfCollector perf2(id);
  int r2 = collector.get_counters(&perf2);
  ASSERT_EQ(r2, 0);
  EXPECT_GT(perf2.last_updated_mono.sec(), 0u)
    << "After process_reports, last_updated_mono must be a real (non-zero) "
    << "timestamp."
    << "\n  Current: " << perf2.last_updated_mono.sec() << "s.";

  collector.remove_query(id);
}

// ===========================================================================
// Randomised walk test.
//
// Fixed seed, 100000 random operation sequences over a small key space.
// Operations: add_query (limited/unlimited, q1 or q2), remove_query,
// remove_all_queries, process_reports (registered/unregistered), get_counters,
// get_queries.
//
// After every step, ALL invariants are checked against an independent model.
// INV-A, INV-B (process_reports must not change queries).
// ===========================================================================

TEST_F(OSDOrderingTest, OrderingBug_Walk_RandomisedInvariantCheck)
{
  constexpr int iterations = 100000;
  constexpr uint64_t seed = 0xDEADBEEF42ULL;

  std::mt19937_64 rng(seed);

  // Model state: registered ids, their queries.
  std::map<MetricQueryID, OSDPerfMetricQuery> model_ids;

  // Track registered ids in a vector for random removal.
  std::vector<MetricQueryID> live_ids;

  std::string fail_seq;  // minimal failing sequence for output.

  OSDPerfMetricLimit limit(ops_desc(), 5);

  auto op_str = [](int op) -> std::string {
    switch (op) {
    case 0: return "add_query(q1,nullopt)";
    case 1: return "add_query(q2,nullopt)";
    case 2: return "add_query(q1,limit)";
    case 3: return "remove_query(random)";
    case 4: return "remove_all_queries";
    case 5: return "process_reports(q1,registered)";
    case 6: return "process_reports(q1,unregistered)";
    case 7: return "get_counters(random)";
    case 8: return "get_queries";
    default: return "?";
    }
  };

  for (int i = 0; i < iterations; ++i) {
    int op = static_cast<int>(rng() % 9);
    fail_seq += op_str(op) + ";";
    if (fail_seq.size() > 2000) {
      // Keep only the last 2000 chars of the sequence.
      fail_seq = "...(truncated);" + fail_seq.substr(fail_seq.size() - 2000);
    }

    switch (op) {
    case 0: {
      MetricQueryID id = collector.add_query(q1, std::nullopt);
      model_ids[id] = q1;
      live_ids.push_back(id);
      break;
    }
    case 1: {
      MetricQueryID id = collector.add_query(q2, std::nullopt);
      model_ids[id] = q2;
      live_ids.push_back(id);
      break;
    }
    case 2: {
      MetricQueryID id = collector.add_query(q1, limit);
      model_ids[id] = q1;
      live_ids.push_back(id);
      break;
    }
    case 3: {
      if (!live_ids.empty()) {
        size_t idx = rng() % live_ids.size();
        MetricQueryID id = live_ids[idx];
        live_ids.erase(live_ids.begin() + static_cast<long>(idx));
        model_ids.erase(id);
        collector.remove_query(id);
      }
      break;
    }
    case 4: {
      live_ids.clear();
      model_ids.clear();
      collector.remove_all_queries();
      break;
    }
    case 5: {
      OSDPerfMetricReport report = make_osd_report(q1, key0, {{1, 0}});
      collector.process_reports(make_osd_payload(q1, report));
      break;
    }
    case 6: {
      // Deliberate: send report for q2 even if not registered.
      OSDPerfMetricReport report = make_osd_report(q2, key0, {{1, 0}});
      collector.process_reports(make_osd_payload(q2, report));
      break;
    }
    case 7: {
      if (!live_ids.empty()) {
        size_t idx = rng() % live_ids.size();
        MetricQueryID id = live_ids[idx];
        OSDPerfCollector perf(id);
        int r = collector.get_counters(&perf);
        ASSERT_EQ(r, 0)
          << "get_counters for a registered id must return 0."
          << "\n  Seed: " << seed << " iteration: " << i
          << "\n  Sequence (last): " << fail_seq;
      }
      break;
    }
    case 8: {
      (void)collector.get_queries();
      break;
    }
    default:
      break;
    }

    // INV-A: every registered id (in model) must appear in get_queries by id.
    // We check the complement: get_queries must agree with model on query
    // keys.  OSDPerfMetricQuery has operator< but not operator==; use
    // map-key membership via count() for the comparison.
    {
      auto qs = collector.get_queries();

      // Build expected set of queries from model.
      std::set<OSDPerfMetricQuery> model_queries;
      for (auto& [mid, q] : model_ids) {
        model_queries.insert(q);
      }
      std::set<OSDPerfMetricQuery> reported_queries;
      for (auto& [q, lims] : qs) {
        reported_queries.insert(q);
      }

      bool size_match = (reported_queries.size() == model_queries.size());
      bool all_in_model = true;
      for (const auto& q : reported_queries) {
        if (model_queries.count(q) == 0) { all_in_model = false; break; }
      }
      bool all_in_reported = true;
      for (const auto& q : model_queries) {
        if (reported_queries.count(q) == 0) { all_in_reported = false; break; }
      }

      ASSERT_TRUE(size_match && all_in_model && all_in_reported)
        << "get_queries() must match the set of queries registered via add_query."
        << "\n  Seed: " << seed << " iteration: " << i
        << "\n  Sequence (last): " << fail_seq
        << "\n  Current reported: " << reported_queries.size() << " queries."
        << "\n  Expected (model): " << model_queries.size() << " queries.";
    }

    // INV-A (counters): every id in model must have a counter entry.
    // We do not expose counters directly, but we can verify via get_counters.
    // Only check a sample (every 100th iteration) to avoid O(n^2).
    if (i % 100 == 0 && !live_ids.empty()) {
      size_t idx = rng() % live_ids.size();
      MetricQueryID id = live_ids[idx];
      OSDPerfCollector perf(id);
      int r = collector.get_counters(&perf);
      ASSERT_EQ(r, 0)
        << "get_counters for a registered id must return 0 (INV-A: counters "
        << "must exist for every registered id)."
        << "\n  Seed: " << seed << " iteration: " << i
        << "\n  id=" << id
        << "\n  Sequence (last): " << fail_seq;
      // Re-add to avoid the drain affecting subsequent checks.
      // (The drain just empties the counters; the entry still exists.)
    }
  }

  // Cleanup.
  collector.remove_all_queries();
}
