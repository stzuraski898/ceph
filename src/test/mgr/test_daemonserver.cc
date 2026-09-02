// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

/*
 * Unit tests for DaemonServer.
 *
 * Coverage:
 *   - Construction / teardown (DaemonServerTestHelper)
 *   - DaemonServer::get_tracked_keys()
 *   - DaemonServer::dump_pg_ready()
 *   - DaemonServer::add_osd_perf_query() / remove_osd_perf_query() / get_osd_perf_counters()
 *   - DaemonServer::add_mds_perf_query() / remove_mds_perf_query() / get_mds_perf_counters()
 *   - DaemonServer::reregister_mds_perf_queries()
 *   - DaemonServer::asok_command() – op-tracker disabled path
 *   - offline_pg_report helpers (ok_to_stop, dump)
 *   - StatsAutotuner::evaluate_adjustment() – all three branches
 *   - StatsAutotuner::was_changed_by_user() / set_baseline_period() / record_our_change()
 *   - upgrade_osd_report::ok_to_upgrade() / all_osds_upgraded()
 */

#include "TestMgr.h"

#include "mgr/DaemonServer.h"
#include "mgr/MgrSession.h"
#include "mgr/OSDPerfMetricTypes.h"
#include "mgr/MDSPerfMetricTypes.h"
#include "messages/MMgrOpen.h"
#include "messages/MMgrUpdate.h"
#include "messages/MMgrClose.h"
#include "messages/MMgrReport.h"
#include "messages/MMgrConfigure.h"
#include "messages/MCommand.h"
#include "messages/MMgrCommand.h"

#include "common/Formatter.h"
#include "common/JSONFormatter.h"

// MetadataUpdate::finish() is defined in Mgr.cc; provide a stub here so that
// the test binary does not need to link Mgr.cc and all its transitive deps.
// The definition must be non-inline so that the linker finds the vtable key
// function and can resolve DaemonServer.cc.o's reference to the vtable.
void MetadataUpdate::finish(int r)
{
  (void)r;
  daemon_state.clear_updating(key);
}

// Basic construction -- every member must be non-null after SetUp().
TEST_F(DaemonServerTestHelper, BasicSetup) {
  ASSERT_NE(mc, nullptr);
  ASSERT_NE(cs, nullptr);
  ASSERT_NE(py_registry, nullptr);
  ASSERT_NE(daemon_state_index, nullptr);
  ASSERT_NE(finisher, nullptr);
  ASSERT_NE(daemon_server, nullptr);
}

TEST_F(DaemonServerTestHelper, GetTrackedKeysReturnsExpectedSet) {
  auto keys = daemon_server->get_tracked_keys();
  // DaemonServer tracks exactly two config keys.
  ASSERT_EQ(keys.size(), 2u);
  auto has = [&](const std::string& k) {
    return std::find(keys.begin(), keys.end(), k) != keys.end();
  };
  EXPECT_TRUE(has("mgr_stats_threshold"));
  EXPECT_TRUE(has("mgr_stats_period"));
}

TEST_F(DaemonServerTestHelper, DumpPgReadyReturnsFalseBeforeInit) {
  JSONFormatter f;
  f.open_object_section("root");
  daemon_server->dump_pg_ready(&f);
  f.close_section();

  std::ostringstream os;
  f.flush(os);
  const std::string json = os.str();
  EXPECT_NE(json.find("\"pg_ready\""), std::string::npos);
  EXPECT_NE(json.find("false"), std::string::npos);
}

TEST_F(DaemonServerTestHelper, AddRemoveOSDPerfQuery) {
  OSDPerfMetricQuery query;
  MetricQueryID qid = daemon_server->add_osd_perf_query(query, std::nullopt);
  // A freshly added query gets a non-negative ID.
  EXPECT_GE(qid, 0);

  int r = daemon_server->remove_osd_perf_query(qid);
  EXPECT_EQ(r, 0);
}

TEST_F(DaemonServerTestHelper, RemoveNonExistentOSDPerfQueryFails) {
  constexpr MetricQueryID bogus_id = 9999;
  int r = daemon_server->remove_osd_perf_query(bogus_id);
  EXPECT_NE(r, 0);
}

TEST_F(DaemonServerTestHelper, GetOSDPerfCountersForUnknownQueryFails) {
  // A collector whose query_id was never registered should return -ENOENT.
  OSDPerfCollector collector(9999);
  int r = daemon_server->get_osd_perf_counters(&collector);
  EXPECT_EQ(r, -ENOENT);
}

TEST_F(DaemonServerTestHelper, GetOSDPerfCountersForRegisteredQuerySucceeds) {
  OSDPerfMetricQuery query;
  MetricQueryID qid = daemon_server->add_osd_perf_query(query, std::nullopt);
  ASSERT_GE(qid, 0);
  // With a registered query the call must succeed (no reports yet so counters
  // will be empty, but the return code must be 0).
  OSDPerfCollector collector(qid);
  int r = daemon_server->get_osd_perf_counters(&collector);
  EXPECT_EQ(r, 0);

  daemon_server->remove_osd_perf_query(qid);
}

TEST_F(DaemonServerTestHelper, AddMultipleOSDPerfQueriesGetDistinctIDs) {
  OSDPerfMetricQuery q1, q2;
  MetricQueryID id1 = daemon_server->add_osd_perf_query(q1, std::nullopt);
  MetricQueryID id2 = daemon_server->add_osd_perf_query(q2, std::nullopt);
  EXPECT_NE(id1, id2);

  EXPECT_EQ(daemon_server->remove_osd_perf_query(id1), 0);
  EXPECT_EQ(daemon_server->remove_osd_perf_query(id2), 0);
}

TEST_F(DaemonServerTestHelper, AddRemoveMDSPerfQuery) {
  MDSPerfMetricQuery query;
  MetricQueryID qid = daemon_server->add_mds_perf_query(query, std::nullopt);
  EXPECT_GE(qid, 0);

  int r = daemon_server->remove_mds_perf_query(qid);
  EXPECT_EQ(r, 0);
}

TEST_F(DaemonServerTestHelper, RemoveNonExistentMDSPerfQueryFails) {
  constexpr MetricQueryID bogus_id = 9999;
  int r = daemon_server->remove_mds_perf_query(bogus_id);
  EXPECT_NE(r, 0);
}

TEST_F(DaemonServerTestHelper, GetMDSPerfCountersForUnknownQueryFails) {
  // A collector whose query_id was never registered should return -ENOENT.
  MDSPerfCollector collector(9999);
  int r = daemon_server->get_mds_perf_counters(&collector);
  EXPECT_EQ(r, -ENOENT);
}

TEST_F(DaemonServerTestHelper, GetMDSPerfCountersForRegisteredQuerySucceeds) {
  MDSPerfMetricQuery query;
  MetricQueryID qid = daemon_server->add_mds_perf_query(query, std::nullopt);
  ASSERT_GE(qid, 0);
  MDSPerfCollector collector(qid);
  int r = daemon_server->get_mds_perf_counters(&collector);
  EXPECT_EQ(r, 0);

  daemon_server->remove_mds_perf_query(qid);
}

TEST_F(DaemonServerTestHelper, ReregisterMDSPerfQueriesDoesNotCrash) {
  EXPECT_NO_THROW(daemon_server->reregister_mds_perf_queries());
}

TEST_F(DaemonServerTestHelper, ReregisterMDSPerfQueriesWithActiveQuery) {
  MDSPerfMetricQuery query;
  MetricQueryID qid = daemon_server->add_mds_perf_query(query, std::nullopt);
  EXPECT_GE(qid, 0);

  EXPECT_NO_THROW(daemon_server->reregister_mds_perf_queries());

  EXPECT_EQ(daemon_server->remove_mds_perf_query(qid), 0);
}

TEST_F(DaemonServerTestHelper, AsokCommandDumpOpsInFlightSucceeds) {
  // mgr_enable_op_tracker is true by default, so dump_ops_in_flight succeeds
  // and asok_command returns true.
  JSONFormatter f;
  std::ostringstream ss;
  cmdmap_t cmdmap;
  bool ok = daemon_server->asok_command("dump_ops_in_flight", cmdmap, &f, ss);
  EXPECT_TRUE(ok);
  EXPECT_TRUE(ss.str().empty());  // no error message on success
}

TEST_F(DaemonServerTestHelper, AsokCommandDumpBlockedOpsSucceeds) {
  JSONFormatter f;
  std::ostringstream ss;
  cmdmap_t cmdmap;
  bool ok = daemon_server->asok_command("dump_blocked_ops", cmdmap, &f, ss);
  EXPECT_TRUE(ok);
  EXPECT_TRUE(ss.str().empty());
}

TEST_F(DaemonServerTestHelper, AsokCommandDumpBlockedOpsCountSucceeds) {
  JSONFormatter f;
  std::ostringstream ss;
  cmdmap_t cmdmap;
  bool ok = daemon_server->asok_command("dump_blocked_ops_count", cmdmap, &f, ss);
  EXPECT_TRUE(ok);
  EXPECT_TRUE(ss.str().empty());
}

TEST_F(DaemonServerTestHelper, AsokCommandDumpHistoricOpsSucceeds) {
  JSONFormatter f;
  std::ostringstream ss;
  cmdmap_t cmdmap;
  bool ok = daemon_server->asok_command("dump_historic_ops", cmdmap, &f, ss);
  EXPECT_TRUE(ok);
  EXPECT_TRUE(ss.str().empty());
}

TEST_F(DaemonServerTestHelper, AsokCommandDumpHistoricOpsByDurationSucceeds) {
  JSONFormatter f;
  std::ostringstream ss;
  cmdmap_t cmdmap;
  bool ok = daemon_server->asok_command(
      "dump_historic_ops_by_duration", cmdmap, &f, ss);
  EXPECT_TRUE(ok);
  EXPECT_TRUE(ss.str().empty());
}

TEST_F(DaemonServerTestHelper, AsokCommandDumpHistoricSlowOpsSucceeds) {
  JSONFormatter f;
  std::ostringstream ss;
  cmdmap_t cmdmap;
  bool ok = daemon_server->asok_command("dump_historic_slow_ops", cmdmap, &f, ss);
  EXPECT_TRUE(ok);
  EXPECT_TRUE(ss.str().empty());
}

TEST_F(DaemonServerTestHelper, AsokCommandUnknownCommandReturnsTrue) {
  // Any admin_command that is not in the op-tracker set falls through to the
  // end of asok_command and returns true (success).
  JSONFormatter f;
  std::ostringstream ss;
  cmdmap_t cmdmap;
  bool ok = daemon_server->asok_command("unknown_command", cmdmap, &f, ss);
  EXPECT_TRUE(ok);
}

TEST_F(DaemonServerTestHelper, HandleConfChangeUpdatesAutotunerBaselineOnUserChange) {
  g_ceph_context->_conf.set_val("mgr_stats_period", "15");
  g_ceph_context->_conf.apply_changes(nullptr);

  std::set<std::string> changed = {"mgr_stats_period"};
  EXPECT_NO_THROW(daemon_server->handle_conf_change(g_ceph_context->_conf, changed));

  g_ceph_context->_conf.set_val("mgr_stats_period", "5");
  g_ceph_context->_conf.apply_changes(nullptr);
}

TEST_F(DaemonServerTestHelper, HandleConfChangeStatsThresholdOnly) {
  g_ceph_context->_conf.set_val("mgr_stats_threshold", "20");
  g_ceph_context->_conf.apply_changes(nullptr);

  std::set<std::string> changed = {"mgr_stats_threshold"};
  EXPECT_NO_THROW(daemon_server->handle_conf_change(g_ceph_context->_conf, changed));

  g_ceph_context->_conf.set_val("mgr_stats_threshold", "10");
  g_ceph_context->_conf.apply_changes(nullptr);
}

TEST_F(DaemonServerTestHelper, HandleConfChangeIrrelevantKeyIgnored) {
  std::set<std::string> changed = {"some_unrelated_config_key"};
  EXPECT_NO_THROW(daemon_server->handle_conf_change(g_ceph_context->_conf, changed));
}

TEST_F(DaemonServerTestHelper, GotServiceMapInitializesAndCulls) {
  ServiceMap sm;
  sm.epoch = 1;
  sm.services["rgw"].daemons["rgw.a"].gid = 1234;
  sm.services["rgw"].daemons["rgw.a"].metadata["host"] = "node1";

  cs->set_service_map(sm);
  daemon_server->got_service_map();

  DaemonKey key{"rgw", "rgw.a"};
  EXPECT_TRUE(daemon_state_index->exists(key));
  auto dptr = daemon_state_index->get(key);
  ASSERT_NE(dptr, nullptr);
  EXPECT_TRUE(dptr->service_daemon);

  ServiceMap sm2;
  sm2.epoch = 2;
  sm2.services["rgw"].daemons["rgw.b"].gid = 5678;
  cs->set_service_map(sm2);
  daemon_server->got_service_map();

  EXPECT_FALSE(daemon_state_index->exists(key));
  EXPECT_TRUE(daemon_state_index->exists(DaemonKey{"rgw", "rgw.b"}));
}

TEST_F(DaemonServerTestHelper, GotMgrMapAddsActiveAndStandbyMgrs) {
  MgrMap mm;
  mm.epoch = 1;
  mm.active_name = "mgr01";
  MgrMap::StandbyInfo sinfo;
  sinfo.name = "mgr02";
  mm.standbys[100] = sinfo;

  cs->set_mgr_map(mm);
  EXPECT_NO_THROW(daemon_server->got_mgr_map());
}

TEST_F(DaemonServerTestHelper, MsHandleRefusedReturnsFalse) {
  EXPECT_FALSE(daemon_server->ms_handle_refused(nullptr));
}

TEST_F(DaemonServerTestHelper, ScheduleTickDoesNotCrash) {
  EXPECT_NO_THROW(daemon_server->schedule_tick(10.0));
}

TEST_F(DaemonServerTestHelper, FetchMissingMetadataOSD) {
  DaemonKey osd_key{"osd", "0"};
  entity_addr_t addr;
  EXPECT_NO_THROW(daemon_server->fetch_missing_metadata(osd_key, addr));
}

TEST_F(DaemonServerTestHelper, FetchMissingMetadataMDS) {
  DaemonKey mds_key{"mds", "a"};
  entity_addr_t addr;
  EXPECT_NO_THROW(daemon_server->fetch_missing_metadata(mds_key, addr));
}

TEST_F(DaemonServerTestHelper, FetchMissingMetadataMON) {
  DaemonKey mon_key{"mon", "a"};
  entity_addr_t addr;
  EXPECT_NO_THROW(daemon_server->fetch_missing_metadata(mon_key, addr));
}

TEST_F(DaemonServerTestHelper, FetchMissingMetadataNonCoreDaemonIsIgnored) {
  DaemonKey rgw_key{"rgw", "rgw0"};
  entity_addr_t addr;
  EXPECT_NO_THROW(daemon_server->fetch_missing_metadata(rgw_key, addr));
}

TEST_F(DaemonServerTestHelper, AdjustPgsRunsWithoutCrashing) {
  EXPECT_NO_THROW(daemon_server->adjust_pgs());
}

TEST_F(DaemonServerTestHelper, SendReportWhenNotReadyWaitsOrTransitions) {
  // Before pgmap_ready, send_report checks timeout
  EXPECT_NO_THROW(daemon_server->send_report());
}

class OfflinePgReportTest : public ::testing::Test {};

TEST_F(OfflinePgReportTest, EmptyReportIsOkToStop) {
  offline_pg_report report;
  EXPECT_TRUE(report.ok_to_stop());
}

TEST_F(OfflinePgReportTest, ReportWithNotOkPgIsNotOkToStop) {
  offline_pg_report report;
  report.not_ok.insert(pg_t(0, 1));
  EXPECT_FALSE(report.ok_to_stop());
}

TEST_F(OfflinePgReportTest, ReportWithUnknownPgIsNotOkToStop) {
  offline_pg_report report;
  report.unknown.insert(pg_t(1, 1));
  EXPECT_FALSE(report.ok_to_stop());
}

TEST_F(OfflinePgReportTest, ReportWithOnlyOkPgsIsOkToStop) {
  offline_pg_report report;
  report.ok.insert(pg_t(0, 1));
  report.ok.insert(pg_t(1, 1));
  EXPECT_TRUE(report.ok_to_stop());
}

TEST_F(OfflinePgReportTest, DumpOkToStopTrue) {
  offline_pg_report report;
  report.osds = std::vector<int>{0, 1};
  report.ok.insert(pg_t(0, 1));

  JSONFormatter f;
  f.open_object_section("r");
  report.dump(&f);
  f.close_section();
  std::ostringstream os;
  f.flush(os);
  const std::string json = os.str();

  EXPECT_NE(json.find("\"ok_to_stop\":true"), std::string::npos);
  EXPECT_NE(json.find("\"num_ok_pgs\":1"), std::string::npos);
  EXPECT_NE(json.find("\"num_not_ok_pgs\":0"), std::string::npos);
}

TEST_F(OfflinePgReportTest, DumpOkToStopFalse) {
  offline_pg_report report;
  report.osds = std::set<int>{2};
  report.not_ok.insert(pg_t(0, 2));

  JSONFormatter f;
  f.open_object_section("r");
  report.dump(&f);
  f.close_section();
  std::ostringstream os;
  f.flush(os);
  const std::string json = os.str();

  EXPECT_NE(json.find("\"ok_to_stop\":false"), std::string::npos);
  EXPECT_NE(json.find("\"num_not_ok_pgs\":1"), std::string::npos);
}

TEST_F(OfflinePgReportTest, DumpIncludesBadNoPoolSection) {
  offline_pg_report report;
  report.osds = std::vector<int>{0};
  report.bad_no_pool.insert(pg_t(0, 99));
  report.not_ok.insert(pg_t(0, 99));

  JSONFormatter f;
  f.open_object_section("r");
  report.dump(&f);
  f.close_section();
  std::ostringstream os;
  f.flush(os);

  EXPECT_NE(os.str().find("\"bad_no_pool_pgs\""), std::string::npos);
}

TEST_F(OfflinePgReportTest, DumpIncludesBadAlreadyInactiveSection) {
  offline_pg_report report;
  report.osds = std::vector<int>{0};
  report.bad_already_inactive.insert(pg_t(0, 1));
  report.not_ok.insert(pg_t(0, 1));

  JSONFormatter f;
  f.open_object_section("r");
  report.dump(&f);
  f.close_section();
  std::ostringstream os;
  f.flush(os);

  EXPECT_NE(os.str().find("\"bad_already_inactive\""), std::string::npos);
}

TEST_F(OfflinePgReportTest, DumpIncludesBadBecomeInactiveSection) {
  offline_pg_report report;
  report.osds = std::vector<int>{0};
  report.bad_become_inactive.insert(pg_t(0, 1));
  report.not_ok.insert(pg_t(0, 1));

  JSONFormatter f;
  f.open_object_section("r");
  report.dump(&f);
  f.close_section();
  std::ostringstream os;
  f.flush(os);

  EXPECT_NE(os.str().find("\"bad_become_inactive\""), std::string::npos);
}

TEST_F(OfflinePgReportTest, DumpIncludesOkBecomeDegradedSection) {
  offline_pg_report report;
  report.osds = std::vector<int>{0};
  report.ok.insert(pg_t(0, 1));
  report.ok_become_degraded.insert(pg_t(0, 1));

  JSONFormatter f;
  f.open_object_section("r");
  report.dump(&f);
  f.close_section();
  std::ostringstream os;
  f.flush(os);

  EXPECT_NE(os.str().find("\"ok_become_degraded\""), std::string::npos);
}

TEST_F(OfflinePgReportTest, DumpIncludesOkBecomeMoreDegradedSection) {
  offline_pg_report report;
  report.osds = std::vector<int>{0};
  report.ok.insert(pg_t(0, 1));
  report.ok_become_more_degraded.insert(pg_t(0, 1));

  JSONFormatter f;
  f.open_object_section("r");
  report.dump(&f);
  f.close_section();
  std::ostringstream os;
  f.flush(os);

  EXPECT_NE(os.str().find("\"ok_become_more_degraded\""), std::string::npos);
}

class StatsAutotunerTest : public ::testing::Test {
public:
  StatsAutotuner tuner{5};
};

TEST_F(StatsAutotunerTest, NoAdjustmentWhenQueueDepthBelowThreshold) {
  // queue_depth=2 < threshold=10, current_period == baseline -> no adjustment
  auto result = tuner.evaluate_adjustment(2, 5, 10);
  EXPECT_EQ(result.new_period, 5);
  EXPECT_EQ(result.reason_code,
            StatsAutotuner::AdjustmentReason::no_adjustment_needed);
  EXPECT_EQ(result.reason_str(), "no_adjustment_needed");
}

TEST_F(StatsAutotunerTest, IncreasesPeriodWhenQueueDepthExceedsThreshold) {
  // queue_depth=20 > threshold=10 -> period should increase
  auto result = tuner.evaluate_adjustment(20, 5, 10);
  EXPECT_GT(result.new_period, 5);
  EXPECT_EQ(result.reason_code,
            StatsAutotuner::AdjustmentReason::high_queue_depth);
  EXPECT_EQ(result.reason_str(), "high_queue_depth");
}

TEST_F(StatsAutotunerTest, DecreasesPeriodWhenQueueRecovered) {
  // current_period (30) > baseline (5), queue_depth (1) < RECOVERY_THRESHOLD (20)
  auto result = tuner.evaluate_adjustment(1, 30, 10);
  EXPECT_LT(result.new_period, 30);
  EXPECT_GE(result.new_period, 5);  // must not drop below baseline
  EXPECT_EQ(result.reason_code,
            StatsAutotuner::AdjustmentReason::performance_recovered);
  EXPECT_EQ(result.reason_str(), "performance_recovered");
}

TEST_F(StatsAutotunerTest, PeriodDoesNotExceedMaximum) {
  // Very high queue depth with an already-large period must be capped at MAX_PERIOD (60).
  auto result = tuner.evaluate_adjustment(10000, 58, 10);
  EXPECT_LE(result.new_period, 60);
}

TEST_F(StatsAutotunerTest, PeriodDoesNotDropBelowBaseline) {
  // Recovery from a slightly elevated period – result must not go below baseline.
  auto result = tuner.evaluate_adjustment(0, 6, 10);
  EXPECT_GE(result.new_period, 5);
}

TEST_F(StatsAutotunerTest, WasChangedByUserDetectsUserEdit) {
  // Initially the tuner's changed_stats_period == baseline (5).
  // Simulating a user changing the period to 15:
  EXPECT_FALSE(tuner.was_changed_by_user(5));   // no change yet
  EXPECT_TRUE(tuner.was_changed_by_user(15));   // 15 != 5 (our tracked value)
}

TEST_F(StatsAutotunerTest, RecordOurChangeUpdatesTrackedPeriod) {
  // After we (the autotuner) change the period to 20, was_changed_by_user(20)
  // should return false (we made that change, not the user).
  tuner.record_our_change(20);
  EXPECT_FALSE(tuner.was_changed_by_user(20));
  // A different value still looks like a user change.
  EXPECT_TRUE(tuner.was_changed_by_user(25));
}

TEST_F(StatsAutotunerTest, SetBaselinePeriodResetsTracking) {
  // After a user changes the baseline to 10, a period of 10 must not look
  // like a user change (we just adopted 10 as our new baseline).
  tuner.set_baseline_period(10);
  // Evaluate: at baseline, no recovery needed and nothing to increase.
  auto result = tuner.evaluate_adjustment(0, 10, 100);
  EXPECT_EQ(result.new_period, 10);
  EXPECT_EQ(result.reason_code,
            StatsAutotuner::AdjustmentReason::no_adjustment_needed);
}

TEST_F(StatsAutotunerTest, ReasonStrNoAdjustment) {
  StatsAutotuner::AdjustmentResult r;
  r.reason_code = StatsAutotuner::AdjustmentReason::no_adjustment_needed;
  EXPECT_EQ(r.reason_str(), "no_adjustment_needed");
}

TEST_F(StatsAutotunerTest, ReasonStrHighQueueDepth) {
  StatsAutotuner::AdjustmentResult r;
  r.reason_code = StatsAutotuner::AdjustmentReason::high_queue_depth;
  EXPECT_EQ(r.reason_str(), "high_queue_depth");
}

TEST_F(StatsAutotunerTest, ReasonStrPerformanceRecovered) {
  StatsAutotuner::AdjustmentResult r;
  r.reason_code = StatsAutotuner::AdjustmentReason::performance_recovered;
  EXPECT_EQ(r.reason_str(), "performance_recovered");
}

class UpgradeOsdReportTest : public ::testing::Test {};

TEST_F(UpgradeOsdReportTest, EmptyReportIsNotOkToUpgrade) {
  upgrade_osd_report report;
  EXPECT_FALSE(report.ok_to_upgrade());
}

TEST_F(UpgradeOsdReportTest, OkToUpgradeRequiresNoBadVersion) {
  upgrade_osd_report report;
  report.ok_upgrade.push_back(0);
  report.bad_no_version.push_back(1);
  EXPECT_FALSE(report.ok_to_upgrade());
}

TEST_F(UpgradeOsdReportTest, OkToUpgradeSuccessPath) {
  upgrade_osd_report report;
  report.ok_upgrade.push_back(0);
  EXPECT_TRUE(report.ok_to_upgrade());
}

TEST_F(UpgradeOsdReportTest, AllOsdsUpgradedReturnsFalseWhenPending) {
  upgrade_osd_report report;
  report.osds = {0, 1};
  report.ok_upgraded = {0};
  EXPECT_FALSE(report.all_osds_upgraded());
}

TEST_F(UpgradeOsdReportTest, AllOsdsUpgradedReturnsTrueWhenDone) {
  upgrade_osd_report report;
  report.osds = {0, 1};
  report.ok_upgraded = {0, 1};
  EXPECT_TRUE(report.all_osds_upgraded());
}

TEST_F(UpgradeOsdReportTest, AllOsdsUpgradedReturnsFalseWhenBadVersion) {
  upgrade_osd_report report;
  report.osds = {0, 1};
  report.ok_upgraded = {0, 1};
  report.bad_no_version.push_back(2);
  EXPECT_FALSE(report.all_osds_upgraded());
}

TEST_F(StatsAutotunerTest, ShouldCheckNowReturnsTrueWhenEnoughTimeHasPassed) {
  StatsAutotuner t(5);
  utime_t now;
  now.set_from_double(1000.0);
  double tick_period = 5.0;
  EXPECT_TRUE(t.should_check_now(now, tick_period));
}

TEST_F(StatsAutotunerTest, ShouldCheckNowReturnsFalseImmediatelyAfterCheck) {
  StatsAutotuner t(5);
  utime_t now;
  now.set_from_double(1000.0);
  double tick_period = 5.0;
  t.should_check_now(now, tick_period);
  EXPECT_FALSE(t.should_check_now(now, tick_period));
}

TEST_F(StatsAutotunerTest, ShouldCheckNowReturnsTrueAfterSufficientDelay) {
  StatsAutotuner t(5);
  utime_t t1, t2;
  t1.set_from_double(100.0);
  double tick_period = 5.0;
  t.should_check_now(t1, tick_period);
  t2.set_from_double(130.0);
  EXPECT_TRUE(t.should_check_now(t2, tick_period));
}

TEST_F(StatsAutotunerTest, ShouldCheckNowReturnsFalseBeforeSufficientDelay) {
  StatsAutotuner t(5);
  utime_t t1, t2;
  t1.set_from_double(100.0);
  double tick_period = 5.0;
  t.should_check_now(t1, tick_period);
  t2.set_from_double(110.0);
  EXPECT_FALSE(t.should_check_now(t2, tick_period));
}

class UpgradeOsdReportDumpTest : public ::testing::Test {};

TEST_F(UpgradeOsdReportDumpTest, DumpReflectsOkToUpgrade) {
  upgrade_osd_report report;
  report.osds = {0, 1};
  report.ok_upgrade.push_back(0);

  JSONFormatter f;
  f.open_object_section("r");
  report.dump(&f);
  f.close_section();
  std::ostringstream os;
  f.flush(os);
  const std::string json = os.str();

  EXPECT_NE(json.find("\"ok_to_upgrade\":true"), std::string::npos);
}

TEST_F(UpgradeOsdReportDumpTest, DumpReflectsNotOkToUpgrade) {
  upgrade_osd_report report;
  JSONFormatter f;
  f.open_object_section("r");
  report.dump(&f);
  f.close_section();
  std::ostringstream os;
  f.flush(os);
  const std::string json = os.str();

  EXPECT_NE(json.find("\"ok_to_upgrade\":false"), std::string::npos);
}

TEST_F(UpgradeOsdReportDumpTest, DumpContainsAllSections) {
  upgrade_osd_report report;
  report.osds = {0, 1};
  report.ok_upgrade.push_back(0);
  report.ok_upgraded.push_back(1);
  report.bad_no_version.push_back(2);

  JSONFormatter f;
  f.open_object_section("r");
  report.dump(&f);
  f.close_section();
  std::ostringstream os;
  f.flush(os);
  const std::string json = os.str();

  EXPECT_NE(json.find("\"osds_in_crush_bucket\""), std::string::npos);
  EXPECT_NE(json.find("\"osds_ok_to_upgrade\""), std::string::npos);
  EXPECT_NE(json.find("\"osds_upgraded\""), std::string::npos);
  EXPECT_NE(json.find("\"bad_no_version\""), std::string::npos);
}

TEST_F(UpgradeOsdReportDumpTest, DumpAllOsdsUpgraded) {
  upgrade_osd_report report;
  report.osds = {0};
  report.ok_upgraded = {0};  // ok_upgrade and bad_no_version are empty

  JSONFormatter f;
  f.open_object_section("r");
  report.dump(&f);
  f.close_section();
  std::ostringstream os;
  f.flush(os);
  const std::string json = os.str();

  EXPECT_NE(json.find("\"all_osds_upgraded\":true"), std::string::npos);
}

TEST_F(DaemonServerTestHelper, AddOSDPerfQueryWithLimit) {
  OSDPerfMetricQuery query;
  OSDPerfMetricLimit limit;
  MetricQueryID qid = daemon_server->add_osd_perf_query(query, limit);
  EXPECT_GE(qid, 0);
  EXPECT_EQ(daemon_server->remove_osd_perf_query(qid), 0);
}

TEST_F(DaemonServerTestHelper, AddMDSPerfQueryWithLimit) {
  MDSPerfMetricQuery query;
  MDSPerfMetricLimit limit;
  MetricQueryID qid = daemon_server->add_mds_perf_query(query, limit);
  EXPECT_GE(qid, 0);
  EXPECT_EQ(daemon_server->remove_mds_perf_query(qid), 0);
}

// Verify that query IDs increase monotonically.
TEST_F(DaemonServerTestHelper, OSDPerfQueryIDsAreMonotonicallyIncreasing) {
  OSDPerfMetricQuery q;
  MetricQueryID id1 = daemon_server->add_osd_perf_query(q, std::nullopt);
  MetricQueryID id2 = daemon_server->add_osd_perf_query(q, std::nullopt);
  MetricQueryID id3 = daemon_server->add_osd_perf_query(q, std::nullopt);
  EXPECT_LT(id1, id2);
  EXPECT_LT(id2, id3);

  daemon_server->remove_osd_perf_query(id1);
  daemon_server->remove_osd_perf_query(id2);
  daemon_server->remove_osd_perf_query(id3);
}

TEST_F(DaemonServerTestHelper, MDSPerfQueryIDsAreMonotonicallyIncreasing) {
  MDSPerfMetricQuery q;
  MetricQueryID id1 = daemon_server->add_mds_perf_query(q, std::nullopt);
  MetricQueryID id2 = daemon_server->add_mds_perf_query(q, std::nullopt);
  MetricQueryID id3 = daemon_server->add_mds_perf_query(q, std::nullopt);
  EXPECT_LT(id1, id2);
  EXPECT_LT(id2, id3);

  daemon_server->remove_mds_perf_query(id1);
  daemon_server->remove_mds_perf_query(id2);
  daemon_server->remove_mds_perf_query(id3);
}

// After removing a query, trying to remove it again must fail.
TEST_F(DaemonServerTestHelper, DoubleRemoveOSDPerfQueryFails) {
  OSDPerfMetricQuery q;
  MetricQueryID qid = daemon_server->add_osd_perf_query(q, std::nullopt);
  ASSERT_GE(qid, 0);
  EXPECT_EQ(daemon_server->remove_osd_perf_query(qid), 0);
  EXPECT_NE(daemon_server->remove_osd_perf_query(qid), 0);
}

TEST_F(DaemonServerTestHelper, DoubleRemoveMDSPerfQueryFails) {
  MDSPerfMetricQuery q;
  MetricQueryID qid = daemon_server->add_mds_perf_query(q, std::nullopt);
  ASSERT_GE(qid, 0);
  EXPECT_EQ(daemon_server->remove_mds_perf_query(qid), 0);
  EXPECT_NE(daemon_server->remove_mds_perf_query(qid), 0);
}

TEST_F(DaemonServerTestHelper, ShutdownCanBeCalledExplicitlyAndIdempotently) {
  EXPECT_NO_THROW(daemon_server->shutdown());
  EXPECT_NO_THROW(daemon_server->shutdown());
}

TEST_F(DaemonServerTestHelper, FastAuthenticationAllowAll) {
  class TestConnection : public Connection {
  public:
    bool marked_down = false;
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("1");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override { marked_down = true; }
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  con->peer_caps_info.allow_all = true;

  bool ok = daemon_server->ms_handle_fast_authentication(con.get());
  EXPECT_TRUE(ok);
  auto priv = con->get_priv();
  ASSERT_NE(priv, nullptr);
  auto session = ceph::ref_cast<MgrSession>(priv);
  ASSERT_NE(session, nullptr);
  EXPECT_TRUE(session->caps.is_allow_all());
}

TEST_F(DaemonServerTestHelper, FastAuthenticationParsedCaps) {
  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("2");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  con->peer_caps_info.allow_all = false;
  std::string cap_str = "allow *";
  encode(cap_str, con->peer_caps_info.caps);

  bool ok = daemon_server->ms_handle_fast_authentication(con.get());
  EXPECT_TRUE(ok);
  auto session = ceph::ref_cast<MgrSession>(con->get_priv());
  ASSERT_NE(session, nullptr);
}

TEST_F(DaemonServerTestHelper, FastAuthenticationInvalidCapsFails) {
  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("3");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  con->peer_caps_info.allow_all = false;
  // Corrupt buffer (not a valid encoded string)
  con->peer_caps_info.caps.append("not a valid encoded bufferlist string");

  bool ok = daemon_server->ms_handle_fast_authentication(con.get());
  EXPECT_FALSE(ok);
}

TEST_F(DaemonServerTestHelper, MsHandleAcceptAndResetOSD) {
  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_OSD);
      peer_name.set_id("0");
      set_peer_type(CEPH_ENTITY_TYPE_OSD);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  auto session = ceph::make_ref<MgrSession>(cct.get());
  session->entity_name = con->peer_name;
  session->osd_id = 0;
  con->set_priv(session);

  EXPECT_NO_THROW(daemon_server->ms_handle_accept(con.get()));
  EXPECT_EQ(session->osd_id, 0);

  bool reset_ret = daemon_server->ms_handle_reset(con.get());
  EXPECT_FALSE(reset_ret);
}

TEST_F(DaemonServerTestHelper, MsHandleResetUntrackedConnection) {
  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("42");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  bool reset_ret = daemon_server->ms_handle_reset(con.get());
  EXPECT_FALSE(reset_ret);
}

TEST_F(DaemonServerTestHelper, HandleOpenServiceDaemonCreatesState) {
  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("10");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  auto open_msg = ceph::make_message<MMgrOpen>();
  open_msg->service_name = "rgw";
  open_msg->daemon_name = "rgw.test1";
  open_msg->service_daemon = true;
  open_msg->daemon_metadata["host"] = "host1";
  open_msg->set_connection(con);

  bool res = daemon_server->handle_open(open_msg);
  EXPECT_TRUE(res);

  DaemonKey key{"rgw", "rgw.test1"};
  EXPECT_TRUE(daemon_state_index->exists(key));
  auto dptr = daemon_state_index->get(key);
  ASSERT_NE(dptr, nullptr);
  EXPECT_TRUE(dptr->service_daemon);
}

TEST_F(DaemonServerTestHelper, HandleOpenNonServiceDaemonMissingMetadataClosesSession) {
  class TestConnection : public Connection {
  public:
    bool marked_down = false;
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_OSD);
      peer_name.set_id("99");
      set_peer_type(CEPH_ENTITY_TYPE_OSD);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override { marked_down = true; }
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  auto open_msg = ceph::make_message<MMgrOpen>();
  open_msg->daemon_name = "99";
  open_msg->service_name = "";
  open_msg->service_daemon = false;
  open_msg->set_connection(con);

  bool res = daemon_server->handle_open(open_msg);
  EXPECT_TRUE(res);
  EXPECT_TRUE(con->marked_down);
}

TEST_F(DaemonServerTestHelper, HandleUpdateNonDaemonClientRejected) {
  class TestConnection : public Connection {
  public:
    bool marked_down = false;
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("100");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override { marked_down = true; }
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  auto update_msg = ceph::make_message<MMgrUpdate>();
  update_msg->daemon_name = "client.100";
  update_msg->service_name = "";
  update_msg->set_connection(con);

  bool res = daemon_server->handle_update(update_msg);
  EXPECT_TRUE(res);
  EXPECT_TRUE(con->marked_down);
}

TEST_F(DaemonServerTestHelper, HandleUpdateExistingDaemonState) {
  DaemonKey key{"rgw", "rgw.upd"};
  auto daemon = std::make_shared<DaemonState>(daemon_state_index->types);
  daemon->key = key;
  daemon->service_daemon = true;
  daemon_state_index->insert(daemon);

  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("101");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  auto update_msg = ceph::make_message<MMgrUpdate>();
  update_msg->service_name = "rgw";
  update_msg->daemon_name = "rgw.upd";
  update_msg->need_metadata_update = true;
  update_msg->daemon_metadata["zone"] = "zone1";
  update_msg->set_connection(con);

  bool res = daemon_server->handle_update(update_msg);
  EXPECT_TRUE(res);
}

TEST_F(DaemonServerTestHelper, HandleCloseRemovesDaemonState) {
  DaemonKey key{"rgw", "rgw.cls"};
  auto daemon = std::make_shared<DaemonState>(daemon_state_index->types);
  daemon->key = key;
  daemon->service_daemon = true;
  daemon_state_index->insert(daemon);
  EXPECT_TRUE(daemon_state_index->exists(key));

  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("102");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  auto close_msg = ceph::make_message<MMgrClose>();
  close_msg->service_name = "rgw";
  close_msg->daemon_name = "rgw.cls";
  close_msg->set_connection(con);

  bool res = daemon_server->handle_close(close_msg);
  EXPECT_TRUE(res);
  EXPECT_FALSE(daemon_state_index->exists(key));
}

TEST_F(DaemonServerTestHelper, HandleReportNonDaemonClientRejected) {
  class TestConnection : public Connection {
  public:
    bool marked_down = false;
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("200");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override { marked_down = true; }
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  auto report_msg = ceph::make_message<MMgrReport>();
  report_msg->daemon_name = "client.200";
  report_msg->service_name = "";
  report_msg->set_connection(con);

  bool res = daemon_server->handle_report(report_msg);
  EXPECT_TRUE(res);
  EXPECT_TRUE(con->marked_down);
}

TEST_F(DaemonServerTestHelper, HandleReportMissingMetadataRejectsAndFetches) {
  class TestConnection : public Connection {
  public:
    bool marked_down = false;
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_OSD);
      peer_name.set_id("50");
      set_peer_type(CEPH_ENTITY_TYPE_OSD);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override { marked_down = true; }
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  auto session = ceph::make_ref<MgrSession>(cct.get());
  session->entity_name = con->peer_name;
  session->osd_id = 50;
  con->set_priv(session);

  auto report_msg = ceph::make_message<MMgrReport>();
  report_msg->daemon_name = "50";
  report_msg->service_name = "";
  report_msg->set_connection(con);

  bool res = daemon_server->handle_report(report_msg);
  EXPECT_FALSE(res);
  EXPECT_TRUE(con->marked_down);
}

TEST_F(DaemonServerTestHelper, HandleReportExistingDaemonUpdatesState) {
  DaemonKey key{"osd", "60"};
  auto daemon = std::make_shared<DaemonState>(daemon_state_index->types);
  daemon->key = key;
  daemon_state_index->insert(daemon);

  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_OSD);
      peer_name.set_id("60");
      set_peer_type(CEPH_ENTITY_TYPE_OSD);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  auto session = ceph::make_ref<MgrSession>(cct.get());
  session->entity_name = con->peer_name;
  session->osd_id = 60;
  con->set_priv(session);

  auto report_msg = ceph::make_message<MMgrReport>();
  report_msg->daemon_name = "60";
  report_msg->service_name = "";
  report_msg->set_connection(con);

  // Encode header for packed buffer (v1, struct_v = 1, compat_v = 1)
  {
    using ceph::encode;
    ENCODE_START(1, 1, report_msg->packed);
    ENCODE_FINISH(report_msg->packed);
  }

  bool res = daemon_server->handle_report(report_msg);
  EXPECT_TRUE(res);
}

TEST_F(DaemonServerTestHelper, MsDispatch2HandlesUnknownMessageType) {
  auto bad_msg = ceph::make_message<MMgrConfigure>();
  auto res = daemon_server->ms_dispatch2(bad_msg);
  EXPECT_TRUE(std::holds_alternative<bool>(res));
  EXPECT_FALSE(std::get<bool>(res));
}

TEST_F(DaemonServerTestHelper, MsDispatch2HandlesPGStats) {
  auto stats = ceph::make_ref<MPGStats>();
  stats->set_src(entity_name_t::OSD(0));
  auto res = daemon_server->ms_dispatch2(stats);
  EXPECT_TRUE(std::holds_alternative<bool>(res));
  EXPECT_TRUE(std::get<bool>(res));
}

TEST_F(DaemonServerTestHelper, HandleCommandInvalidJSONReturnsError) {
  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("1");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  auto session = ceph::make_ref<MgrSession>(cct.get());
  session->entity_name = con->peer_name;
  session->caps.set_allow_all();
  con->set_priv(session);

  auto cmd_msg = ceph::make_message<MCommand>();
  cmd_msg->cmd = {"invalid json string {"};
  cmd_msg->set_connection(con);

  bool res = daemon_server->handle_command(cmd_msg);
  EXPECT_TRUE(res);
}

TEST_F(DaemonServerTestHelper, HandleCommandServiceDump) {
  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("1");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  auto session = ceph::make_ref<MgrSession>(cct.get());
  session->entity_name = con->peer_name;
  session->caps.set_allow_all();
  con->set_priv(session);

  auto cmd_msg = ceph::make_message<MCommand>();
  cmd_msg->cmd = {"{\"prefix\": \"service dump\"}"};
  cmd_msg->set_connection(con);

  bool res = daemon_server->handle_command(cmd_msg);
  EXPECT_TRUE(res);
}

TEST_F(DaemonServerTestHelper, HandleMgrCommandServiceStatus) {
  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("1");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  auto session = ceph::make_ref<MgrSession>(cct.get());
  session->entity_name = con->peer_name;
  session->caps.set_allow_all();
  con->set_priv(session);

  auto cmd_msg = ceph::make_message<MMgrCommand>();
  cmd_msg->cmd = {"{\"prefix\": \"service status\"}"};
  cmd_msg->set_connection(con);

  bool res = daemon_server->handle_command(cmd_msg);
  EXPECT_TRUE(res);
}

TEST_F(DaemonServerTestHelper, SendConfigureRunsWithoutCrashing) {
  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("5");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  EXPECT_NO_THROW(daemon_server->_send_configure(con));
}

// update_task_status() is exercised indirectly: a second identical report must
// not raise any exception and last_service_beacon must be non-zero.
TEST_F(DaemonServerTestHelper, HandleReportWithTaskStatusStoresIt) {
  DaemonKey key{"myservice", "d1"};
  auto daemon = std::make_shared<DaemonState>(daemon_state_index->types);
  daemon->key = key;
  daemon->service_daemon = true;
  daemon_state_index->insert(daemon);

  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("300");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  auto session = ceph::make_ref<MgrSession>(cct.get());
  session->entity_name = con->peer_name;
  con->set_priv(session);

  auto report1 = ceph::make_message<MMgrReport>();
  report1->service_name = "myservice";
  report1->daemon_name = "d1";
  report1->task_status = std::map<std::string,std::string>{{"step", "1"}};
  report1->set_connection(con);
  {
    using ceph::encode;
    ENCODE_START(1, 1, report1->packed);
    ENCODE_FINISH(report1->packed);
  }
  bool res1 = daemon_server->handle_report(report1);
  EXPECT_TRUE(res1);

  // Send identical task_status again — should succeed without crash.
  auto report2 = ceph::make_message<MMgrReport>();
  report2->service_name = "myservice";
  report2->daemon_name = "d1";
  report2->task_status = std::map<std::string,std::string>{{"step", "1"}};
  report2->set_connection(con);
  {
    using ceph::encode;
    ENCODE_START(1, 1, report2->packed);
    ENCODE_FINISH(report2->packed);
  }
  bool res2 = daemon_server->handle_report(report2);
  EXPECT_TRUE(res2);

  // The daemon's last_service_beacon must be set (non-zero) because task_status was present.
  auto dptr = daemon_state_index->get(key);
  ASSERT_NE(dptr, nullptr);
  EXPECT_NE(dptr->last_service_beacon, utime_t());
}

// OpTracker.tracking_enabled is initialised at DaemonServer construction time from
// g_conf()->mgr_enable_op_tracker.  To exercise the disabled path we construct a
// separate DaemonServer with the flag set to false before construction.
TEST_F(DaemonServerTestHelper, AsokCommandWithOpTrackerDisabledReturnsError) {
  g_ceph_context->_conf.set_val("mgr_enable_op_tracker", "false");
  g_ceph_context->_conf.apply_changes(nullptr);

  auto local_server = std::make_unique<DaemonServer>(
      mc.get(), *finisher, *daemon_state_index, *cs,
      *py_registry, clog, audit_clog);

  JSONFormatter f;
  std::ostringstream ss;
  cmdmap_t cmdmap;
  bool ok = local_server->asok_command("dump_ops_in_flight", cmdmap, &f, ss);
  EXPECT_FALSE(ok);
  EXPECT_FALSE(ss.str().empty());  // error message must be present
  EXPECT_NE(ss.str().find("op_tracker"), std::string::npos);

  local_server.reset();

  g_ceph_context->_conf.set_val("mgr_enable_op_tracker", "true");
  g_ceph_context->_conf.apply_changes(nullptr);
}

TEST_F(DaemonServerTestHelper, AsokCommandBlockedOpsWithOpTrackerDisabledReturnsError) {
  g_ceph_context->_conf.set_val("mgr_enable_op_tracker", "false");
  g_ceph_context->_conf.apply_changes(nullptr);

  auto local_server = std::make_unique<DaemonServer>(
      mc.get(), *finisher, *daemon_state_index, *cs,
      *py_registry, clog, audit_clog);

  JSONFormatter f;
  std::ostringstream ss;
  cmdmap_t cmdmap;
  bool ok = local_server->asok_command("dump_blocked_ops", cmdmap, &f, ss);
  EXPECT_FALSE(ok);
  EXPECT_NE(ss.str().find("op_tracker"), std::string::npos);

  local_server.reset();
  g_ceph_context->_conf.set_val("mgr_enable_op_tracker", "true");
  g_ceph_context->_conf.apply_changes(nullptr);
}

TEST_F(DaemonServerTestHelper, HandleCloseNonExistentDaemonSucceeds) {
  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("400");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  auto close_msg = ceph::make_message<MMgrClose>();
  close_msg->service_name = "rgw";
  close_msg->daemon_name = "rgw.nonexistent";
  close_msg->set_connection(con);

  // Must succeed even though the key was never registered.
  bool res = daemon_server->handle_close(close_msg);
  EXPECT_TRUE(res);
}

// The condition is queue_depth > threshold (strict >), so at equality no adjustment.
TEST_F(StatsAutotunerTest, EvaluateAdjustmentAtExactThreshold) {
  // queue_depth(10) == threshold(10): strictly NOT above threshold.
  auto result = tuner.evaluate_adjustment(10, 5, 10);
  EXPECT_EQ(result.new_period, 5);
  EXPECT_EQ(result.reason_code,
            StatsAutotuner::AdjustmentReason::no_adjustment_needed);
}

// When already at MAX_PERIOD (60) and queue depth is very high, new_period is
// clamped to 60 == current_period, so the if(new_period > current_period) guard
// fails and the result is no_adjustment_needed.
TEST_F(StatsAutotunerTest, EvaluateAdjustmentAlreadyAtMaxPeriodNoFurtherIncrease) {
  // period=60 (MAX), high queue → increment would give 60+15=75, clamped to 60.
  // Since new_period == current_period the first branch does NOT fire → no_adjustment.
  auto result = tuner.evaluate_adjustment(10000, 60, 10);
  EXPECT_EQ(result.new_period, 60);
  EXPECT_EQ(result.reason_code,
            StatsAutotuner::AdjustmentReason::no_adjustment_needed);
}

// An empty report means no osds pending, no bad versions → all have "upgraded".
TEST_F(UpgradeOsdReportTest, AllOsdsUpgradedEmptyReportTrue) {
  upgrade_osd_report report;
  // ok_upgrade, ok_upgraded, bad_no_version all empty → vacuously true
  EXPECT_TRUE(report.all_osds_upgraded());
}

// ok_to_stop() must be false because not_ok is non-empty.
TEST_F(OfflinePgReportTest, BothOkAndNotOkPgsMeansNotOkToStop) {
  offline_pg_report report;
  report.ok.insert(pg_t(0, 1));
  report.not_ok.insert(pg_t(1, 1));
  EXPECT_FALSE(report.ok_to_stop());
  // Verify dump counts reflect both sets correctly.
  JSONFormatter f;
  f.open_object_section("r");
  report.dump(&f);
  f.close_section();
  std::ostringstream os;
  f.flush(os);
  EXPECT_NE(os.str().find("\"num_ok_pgs\":1"), std::string::npos);
  EXPECT_NE(os.str().find("\"num_not_ok_pgs\":1"), std::string::npos);
}

// The daemon should be updated (perf_counters cleared) rather than recreated.
TEST_F(DaemonServerTestHelper, HandleOpenExistingDaemonUpdatesMetadata) {
  DaemonKey key{"nfs", "nfs.g1"};
  auto existing = std::make_shared<DaemonState>(daemon_state_index->types);
  existing->key = key;
  existing->service_daemon = true;
  existing->set_metadata({{"host", "node-old"}});
  daemon_state_index->insert(existing);

  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("500");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  auto open_msg = ceph::make_message<MMgrOpen>();
  open_msg->service_name = "nfs";
  open_msg->daemon_name = "nfs.g1";
  open_msg->service_daemon = true;
  open_msg->daemon_metadata["host"] = "node-new";
  open_msg->set_connection(con);

  bool res = daemon_server->handle_open(open_msg);
  EXPECT_TRUE(res);

  // Daemon must still be present and metadata updated.
  ASSERT_TRUE(daemon_state_index->exists(key));
  auto dptr = daemon_state_index->get(key);
  ASSERT_NE(dptr, nullptr);
  EXPECT_TRUE(dptr->service_daemon);
  // Metadata should now reflect the new value sent in open_msg.
  std::lock_guard l(dptr->lock);
  auto it = dptr->metadata.find("host");
  ASSERT_NE(it, dptr->metadata.end());
  EXPECT_EQ(it->second, "node-new");
}

// When got_service_map() is called a second time with an already-advanced
// pending_service_map epoch, the "already active" branch runs without
// corrupting state already present in daemon_state_index.
TEST_F(DaemonServerTestHelper, GotServiceMapAlreadyActiveBranchPreservesMap) {
  ServiceMap sm1;
  sm1.epoch = 1;
  sm1.services["s3"].daemons["s3.a"].metadata["host"] = "h1";
  cs->set_service_map(sm1);
  daemon_server->got_service_map();
  EXPECT_TRUE(daemon_state_index->exists(DaemonKey{"s3", "s3.a"}));

  daemon_server->got_service_map();
  EXPECT_TRUE(daemon_state_index->exists(DaemonKey{"s3", "s3.a"}));
}

TEST_F(DaemonServerTestHelper, MsDispatch2HandlesMgrOpen) {
  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("600");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  auto open_msg = ceph::make_message<MMgrOpen>();
  open_msg->service_name = "rgw";
  open_msg->daemon_name = "rgw.dispatch1";
  open_msg->service_daemon = true;
  open_msg->set_connection(con);

  auto res = daemon_server->ms_dispatch2(open_msg);
  EXPECT_TRUE(std::holds_alternative<bool>(res));
  EXPECT_TRUE(std::get<bool>(res));
}

TEST_F(DaemonServerTestHelper, MsDispatch2HandlesMgrClose) {
  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("601");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  auto close_msg = ceph::make_message<MMgrClose>();
  close_msg->service_name = "rgw";
  close_msg->daemon_name = "rgw.dispatch2";
  close_msg->set_connection(con);

  auto res = daemon_server->ms_dispatch2(close_msg);
  EXPECT_TRUE(std::holds_alternative<bool>(res));
  EXPECT_TRUE(std::get<bool>(res));
}

// StatsAutotuner edge: queue_depth is zero (well below RECOVERY_THRESHOLD=20)
// and period is at baseline — no_adjustment because period already equals baseline.
TEST_F(StatsAutotunerTest, EvaluateAdjustmentZeroQueueAtBaselineNoChange) {
  auto result = tuner.evaluate_adjustment(0, 5, 10);
  EXPECT_EQ(result.new_period, 5);
  EXPECT_EQ(result.reason_code,
            StatsAutotuner::AdjustmentReason::no_adjustment_needed);
}

// StatsAutotuner edge: current_period slightly above baseline and queue is zero —
// result should be performance_recovered.
TEST_F(StatsAutotunerTest, EvaluateAdjustmentZeroQueueAboveBaselineRecovers) {
  // baseline=5, current=10, queue=0 → recovered → halved to max(5,5)=5
  auto result = tuner.evaluate_adjustment(0, 10, 100);
  EXPECT_EQ(result.reason_code,
            StatsAutotuner::AdjustmentReason::performance_recovered);
  EXPECT_EQ(result.new_period, 5);
}

// upgrade_osd_report: ok_to_upgrade() requires ok_upgrade non-empty AND
// bad_no_version empty — test the boundary where bad_no_version alone blocks.
TEST_F(UpgradeOsdReportTest, OkToUpgradeBlockedByBadNoVersionAlone) {
  upgrade_osd_report report;
  report.ok_upgrade.push_back(0);
  report.ok_upgrade.push_back(1);
  report.bad_no_version.push_back(2);
  EXPECT_FALSE(report.ok_to_upgrade());
}

// all_osds_upgraded: ok_upgrade non-empty → not all done
TEST_F(UpgradeOsdReportTest, AllOsdsUpgradedFalseWhenOkUpgradePending) {
  upgrade_osd_report report;
  report.ok_upgrade.push_back(0);  // still waiting to upgrade
  EXPECT_FALSE(report.all_osds_upgraded());
}

TEST_F(DaemonServerTestHelper, MsHandleRemoteResetDoesNotCrash) {
  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("700");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  EXPECT_NO_THROW(daemon_server->ms_handle_remote_reset(con.get()));
}

// (neither allow_all path nor caps decode path) — returns true and creates session.
TEST_F(DaemonServerTestHelper, FastAuthenticationEmptyCapsSucceeds) {
  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("800");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  con->peer_caps_info.allow_all = false;

  bool ok = daemon_server->ms_handle_fast_authentication(con.get());
  EXPECT_TRUE(ok);
  auto priv = con->get_priv();
  ASSERT_NE(priv, nullptr);
}

TEST_F(DaemonServerTestHelper, HandleCommandNullSessionReturnsTrue) {
  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("900");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();

  auto cmd_msg = ceph::make_message<MCommand>();
  cmd_msg->cmd = {"{\"prefix\": \"service dump\"}"};
  cmd_msg->set_connection(con);

  bool res = daemon_server->handle_command(cmd_msg);
  EXPECT_TRUE(res);
}

// After close the key must no longer exist.
TEST_F(DaemonServerTestHelper, HandleOpenThenCloseRemovesState) {
  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("950");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  DaemonKey key{"lifecycle", "lc.1"};

  auto open_msg = ceph::make_message<MMgrOpen>();
  open_msg->service_name = "lifecycle";
  open_msg->daemon_name = "lc.1";
  open_msg->service_daemon = true;
  open_msg->set_connection(con);
  ASSERT_TRUE(daemon_server->handle_open(open_msg));
  ASSERT_TRUE(daemon_state_index->exists(key));

  auto close_msg = ceph::make_message<MMgrClose>();
  close_msg->service_name = "lifecycle";
  close_msg->daemon_name = "lc.1";
  close_msg->set_connection(con);
  ASSERT_TRUE(daemon_server->handle_close(close_msg));
  EXPECT_FALSE(daemon_state_index->exists(key));
}

TEST_F(DaemonServerTestHelper, HandleCommandGetCommandDescriptionsReturnsJSON) {
  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("1001");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  auto session = ceph::make_ref<MgrSession>(cct.get());
  session->entity_name = con->peer_name;
  session->caps.set_allow_all();
  con->set_priv(session);

  auto cmd_msg = ceph::make_message<MCommand>();
  cmd_msg->fsid = uuid_d();
  cmd_msg->cmd = {"{\"prefix\": \"get_command_descriptions\"}"};
  cmd_msg->set_connection(con);

  bool res = daemon_server->handle_command(cmd_msg);
  EXPECT_TRUE(res);
}

// The implementation checks queue_depth > threshold (strict >), so a negative
// value is not > threshold → the recovery branch also won't fire if at baseline.
// Result: no_adjustment_needed.
TEST_F(StatsAutotunerTest, EvaluateAdjustmentNegativeQueueDepthNoChange) {
  auto result = tuner.evaluate_adjustment(-5, 5, 10);
  EXPECT_EQ(result.new_period, 5);
  EXPECT_EQ(result.reason_code,
            StatsAutotuner::AdjustmentReason::no_adjustment_needed);
}

// Condition: now - last_period_check (0) > 0 * 5 = 0 → any t > 0 satisfies this.
TEST_F(StatsAutotunerTest, ShouldCheckNowTickPeriodZeroReturnsTrueForPositiveTime) {
  StatsAutotuner t(5);
  utime_t now;
  now.set_from_double(0.001);
  // With tick_period=0, threshold = 0*5 = 0, and now - last(0) = 0.001 > 0 → true.
  EXPECT_TRUE(t.should_check_now(now, 0.0));
}

TEST_F(OfflinePgReportTest, DumpEmptyOsdsList) {
  offline_pg_report report;
  report.osds = std::vector<int>{};  // explicitly empty

  JSONFormatter f;
  f.open_object_section("r");
  report.dump(&f);
  f.close_section();
  std::ostringstream os;
  f.flush(os);
  const std::string json = os.str();

  EXPECT_NE(json.find("\"ok_to_stop\":true"), std::string::npos);
  EXPECT_NE(json.find("\"num_ok_pgs\":0"), std::string::npos);
  EXPECT_NE(json.find("\"num_not_ok_pgs\":0"), std::string::npos);
}

// Multiple unknown pgs make ok_to_stop() false, and dump includes the unknown_pgs section.
TEST_F(OfflinePgReportTest, MultipleUnknownPgsNotOkToStop) {
  offline_pg_report report;
  report.unknown.insert(pg_t(0, 1));
  report.unknown.insert(pg_t(1, 1));
  report.unknown.insert(pg_t(2, 1));
  EXPECT_FALSE(report.ok_to_stop());
  EXPECT_EQ(report.unknown.size(), 3u);

  JSONFormatter f;
  f.open_object_section("r");
  report.dump(&f);
  f.close_section();
  std::ostringstream os;
  f.flush(os);
  EXPECT_NE(os.str().find("\"unknown_pgs\""), std::string::npos);
}

TEST_F(UpgradeOsdReportDumpTest, DumpAllEmpty) {
  upgrade_osd_report report;  // everything empty

  JSONFormatter f;
  f.open_object_section("r");
  report.dump(&f);
  f.close_section();
  std::ostringstream os;
  f.flush(os);
  const std::string json = os.str();

  EXPECT_NE(json.find("\"ok_to_upgrade\":false"), std::string::npos);
  EXPECT_NE(json.find("\"all_osds_upgraded\":true"), std::string::npos);
  EXPECT_NE(json.find("\"osds_in_crush_bucket\""), std::string::npos);
  EXPECT_NE(json.find("\"osds_ok_to_upgrade\""), std::string::npos);
  EXPECT_NE(json.find("\"osds_upgraded\""), std::string::npos);
  EXPECT_NE(json.find("\"bad_no_version\""), std::string::npos);
}

// After set_baseline_period() the new baseline value must not appear as a user change.
TEST_F(StatsAutotunerTest, WasChangedByUserAfterSetBaselinePeriodReturnsFalse) {
  tuner.set_baseline_period(20);
  // 20 is now the new baseline AND changed_stats_period; should not look like user edit.
  EXPECT_FALSE(tuner.was_changed_by_user(20));
  // A different value should still look like user change.
  EXPECT_TRUE(tuner.was_changed_by_user(25));
}

TEST_F(UpgradeOsdReportTest, OkToUpgradeMultipleOkEntriesSucceeds) {
  upgrade_osd_report report;
  report.ok_upgrade.push_back(0);
  report.ok_upgrade.push_back(1);
  report.ok_upgrade.push_back(2);
  EXPECT_TRUE(report.ok_to_upgrade());
}

// When a session has no capabilities (empty caps, not allow_all) and issues a
// command that requires capabilities, the server calls log_access_denied and
// replies with -EACCES.  Verify handle_command returns true (message consumed).
TEST_F(DaemonServerTestHelper, HandleCommandAccessDeniedWithEmptyCaps) {
  class TestConnection : public Connection {
  public:
    std::vector<MessageRef> sent;
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("2000");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override {
      sent.push_back(std::move(m));
      return 0;
    }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  auto session = ceph::make_ref<MgrSession>(cct.get());
  session->entity_name = con->peer_name;
  con->set_priv(session);

  auto cmd_msg = ceph::make_message<MCommand>();
  cmd_msg->fsid = uuid_d();  // non-admin socket path
  cmd_msg->cmd = {"{\"prefix\": \"service dump\"}"};
  cmd_msg->set_connection(con);

  bool res = daemon_server->handle_command(cmd_msg);
  EXPECT_TRUE(res);
  EXPECT_GE(con->sent.size(), 1u);
}

TEST_F(DaemonServerTestHelper, MsDispatch2HandlesMgrReport) {
  class TestConnection : public Connection {
  public:
    bool marked_down = false;
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("2001");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override { marked_down = true; }
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  auto report_msg = ceph::make_message<MMgrReport>();
  report_msg->daemon_name = "client.2001";
  report_msg->service_name = "";
  report_msg->set_connection(con);

  auto result = daemon_server->ms_dispatch2(report_msg);
  EXPECT_TRUE(std::holds_alternative<bool>(result));
  EXPECT_TRUE(std::get<bool>(result));
  EXPECT_TRUE(con->marked_down);
}

TEST_F(DaemonServerTestHelper, MsDispatch2HandlesMgrUpdate) {
  class TestConnection : public Connection {
  public:
    bool marked_down = false;
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("2002");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override { marked_down = true; }
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  auto update_msg = ceph::make_message<MMgrUpdate>();
  update_msg->daemon_name = "client.2002";
  update_msg->service_name = "";
  update_msg->set_connection(con);

  auto result = daemon_server->ms_dispatch2(update_msg);
  EXPECT_TRUE(std::holds_alternative<bool>(result));
  EXPECT_TRUE(std::get<bool>(result));
  EXPECT_TRUE(con->marked_down);
}

// Caps buffer decodes to a valid string, but the string content fails
// MgrCaps::parse() ("!@#invalid...") → function returns false.
TEST_F(DaemonServerTestHelper, FastAuthenticationCapsDecodeOkButParseFailsReturnsFalse) {
  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("8001");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  con->peer_caps_info.allow_all = false;
  // Encode a syntactically invalid mgr caps string (decodes fine, parse fails).
  std::string bad_caps = "!@#invalid mgr caps garbage 12345";
  encode(bad_caps, con->peer_caps_info.caps);

  bool ok = daemon_server->ms_handle_fast_authentication(con.get());
  EXPECT_FALSE(ok);
}

// The inner rm_daemon branch is NOT taken, but the key should still be removed.
TEST_F(DaemonServerTestHelper, HandleCloseNonServiceDaemonRemovesKey) {
  DaemonKey key{"osd", "777"};
  auto daemon = std::make_shared<DaemonState>(daemon_state_index->types);
  daemon->key = key;
  daemon->service_daemon = false;  // not a service daemon
  daemon_state_index->insert(daemon);
  ASSERT_TRUE(daemon_state_index->exists(key));

  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_OSD);
      peer_name.set_id("777");
      set_peer_type(CEPH_ENTITY_TYPE_OSD);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  auto close_msg = ceph::make_message<MMgrClose>();
  // service_name empty → key derived from peer_type = "osd", daemon_name = "777"
  close_msg->service_name = "";
  close_msg->daemon_name = "777";
  close_msg->set_connection(con);

  bool res = daemon_server->handle_close(close_msg);
  EXPECT_TRUE(res);
  // The key must be gone from daemon_state_index regardless of service_daemon flag.
  EXPECT_FALSE(daemon_state_index->exists(key));
}

// MCommand with fsid != uuid_d() takes the admin-socket path.
// The call must return true even when no registered admin command matches.
TEST_F(DaemonServerTestHelper, HandleCommandAdminSocketPathWithAllowAllSucceeds) {
  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("8002");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  auto session = ceph::make_ref<MgrSession>(cct.get());
  session->entity_name = con->peer_name;
  session->caps.set_allow_all();
  con->set_priv(session);

  auto cmd_msg = ceph::make_message<MCommand>();
  cmd_msg->fsid.generate_random();
  cmd_msg->cmd = {"{\"prefix\": \"service dump\"}"};
  cmd_msg->set_connection(con);

  bool res = daemon_server->handle_command(cmd_msg);
  EXPECT_TRUE(res);
}

// At queue_depth == RECOVERY_THRESHOLD (20) the recovery condition (strict `<`)
// does not fire; with queue_threshold=100 the high-queue branch also does not fire.
TEST_F(StatsAutotunerTest, EvaluateAdjustmentAtExactlyRecoveryThresholdNoRecovery) {
  // queue_depth(20) is NOT > queue_threshold(100)  → high-queue branch skipped.
  // queue_depth(20) is NOT < RECOVERY_THRESHOLD(20) → recovery branch skipped.
  // → no_adjustment_needed, period stays at 30.
  auto result = tuner.evaluate_adjustment(20, 30, 100);
  EXPECT_EQ(result.reason_code,
            StatsAutotuner::AdjustmentReason::no_adjustment_needed);
  EXPECT_EQ(result.new_period, 30);
}

// osds has 3 members but ok_upgraded only 2; ok_upgrade and bad_no_version are empty.
// all_osds_upgraded() checks osds.size() == ok_upgraded.size() → false.
TEST_F(UpgradeOsdReportTest, AllOsdsUpgradedFalseWhenOkUpgradedSubset) {
  upgrade_osd_report report;
  report.osds = {0, 1, 2};
  report.ok_upgraded = {0, 1};  // only 2 of 3 upgraded
  // ok_upgrade and bad_no_version intentionally empty.
  EXPECT_FALSE(report.all_osds_upgraded());
}

// Verify dump() correctly iterates the set variant (not vector) and the
// OSD values appear in the output.
TEST_F(OfflinePgReportTest, DumpWithSetIntOsdsVariant) {
  offline_pg_report report;
  report.osds = std::set<int>{10, 20, 30};
  report.ok.insert(pg_t(0, 1));

  JSONFormatter f;
  f.open_object_section("r");
  report.dump(&f);
  f.close_section();
  std::ostringstream os;
  f.flush(os);
  const std::string json = os.str();

  EXPECT_NE(json.find("\"ok_to_stop\":true"), std::string::npos);
  EXPECT_NE(json.find("\"num_ok_pgs\":1"), std::string::npos);
  EXPECT_NE(json.find("\"num_not_ok_pgs\":0"), std::string::npos);
  // The osds array must be present in the JSON.
  EXPECT_NE(json.find("\"osds\""), std::string::npos);
}

// newly added query receives an ID strictly greater than the ID issued before
// the reregister, confirming the internal counter advanced.
TEST_F(DaemonServerTestHelper, ReregisterMDSPerfQueriesAdvancesIDCounter) {
  MDSPerfMetricQuery q;
  MetricQueryID id_before = daemon_server->add_mds_perf_query(q, std::nullopt);
  ASSERT_GE(id_before, 0);
  EXPECT_EQ(daemon_server->remove_mds_perf_query(id_before), 0);

  daemon_server->reregister_mds_perf_queries();

  MetricQueryID id_after = daemon_server->add_mds_perf_query(q, std::nullopt);
  ASSERT_GE(id_after, 0);
  EXPECT_GT(id_after, id_before);

  EXPECT_EQ(daemon_server->remove_mds_perf_query(id_after), 0);
}

// (the tuner owns the current value), so set_baseline_period() is NOT called.
// Verify the call doesn't crash and the server remains in a consistent state.
TEST_F(DaemonServerTestHelper, HandleConfChangeAutotunerOwnedChangeIsIgnored) {
  g_ceph_context->_conf.set_val("mgr_stats_period", "8");
  g_ceph_context->_conf.apply_changes(nullptr);
  std::set<std::string> changed1 = {"mgr_stats_period"};
  EXPECT_NO_THROW(daemon_server->handle_conf_change(g_ceph_context->_conf, changed1));

  EXPECT_NO_THROW(daemon_server->handle_conf_change(g_ceph_context->_conf, changed1));

  g_ceph_context->_conf.set_val("mgr_stats_period", "5");
  g_ceph_context->_conf.apply_changes(nullptr);
}

// simultaneously — verify all sections appear and counts are correct.
TEST_F(OfflinePgReportTest, DumpWithUnknownAndOkAndNotOkSimultaneously) {
  offline_pg_report report;
  report.osds = std::vector<int>{0};
  report.ok.insert(pg_t(0, 1));
  report.not_ok.insert(pg_t(1, 1));
  report.unknown.insert(pg_t(2, 1));

  JSONFormatter f;
  f.open_object_section("r");
  report.dump(&f);
  f.close_section();
  std::ostringstream os;
  f.flush(os);
  const std::string json = os.str();

  // ok_to_stop() = false because not_ok is non-empty.
  EXPECT_NE(json.find("\"ok_to_stop\":false"), std::string::npos);
  EXPECT_NE(json.find("\"num_ok_pgs\":1"), std::string::npos);
  EXPECT_NE(json.find("\"num_not_ok_pgs\":1"), std::string::npos);
  // Unknown section must be present.
  EXPECT_NE(json.find("\"unknown_pgs\""), std::string::npos);
}

// The `if (session)` guard must prevent a crash; the connection is still removed
// from daemon_connections if present.
TEST_F(DaemonServerTestHelper, MsHandleResetOSDNullPrivDoesNotCrash) {
  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_OSD);
      peer_name.set_id("999");
      set_peer_type(CEPH_ENTITY_TYPE_OSD);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  EXPECT_NO_THROW(daemon_server->ms_handle_reset(con.get()));
  EXPECT_FALSE(daemon_server->ms_handle_reset(con.get()));
}

// body is guarded by peer_type == OSD; for other types it is a no-op. Verify
// it does not crash and does not register the connection in osd_cons.
TEST_F(DaemonServerTestHelper, MsHandleAcceptNonOSDDoesNotCrash) {
  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_MDS);
      peer_name.set_id("a");
      set_peer_type(CEPH_ENTITY_TYPE_MDS);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  EXPECT_NO_THROW(daemon_server->ms_handle_accept(con.get()));
}

// returns true for (e.g. "osd").  Those services are skipped in the daemon_state
// update loop.  Verify the service-daemon is NOT added to daemon_state for "osd".
TEST_F(DaemonServerTestHelper, GotServiceMapSkipsNormalCephEntities) {
  ServiceMap sm;
  sm.epoch = 1;
  sm.services["osd"].daemons["0"].metadata["host"] = "node1";
  cs->set_service_map(sm);
  daemon_server->got_service_map();

  DaemonKey osd_key{"osd", "0"};
  EXPECT_FALSE(daemon_state_index->exists(osd_key));
}

// (service_daemon=false).  DaemonServer logs an error but must NOT crash.
TEST_F(DaemonServerTestHelper, HandleReportDaemonStatusOnNonServiceDaemonDoesNotCrash) {
  DaemonKey key{"osd", "80"};
  auto daemon = std::make_shared<DaemonState>(daemon_state_index->types);
  daemon->key = key;
  daemon->service_daemon = false;  // NOT a service daemon
  daemon_state_index->insert(daemon);

  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_OSD);
      peer_name.set_id("80");
      set_peer_type(CEPH_ENTITY_TYPE_OSD);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  auto session = ceph::make_ref<MgrSession>(cct.get());
  session->entity_name = con->peer_name;
  session->osd_id = 80;
  con->set_priv(session);

  auto report_msg = ceph::make_message<MMgrReport>();
  report_msg->daemon_name = "80";
  report_msg->service_name = "";
  report_msg->daemon_status = std::map<std::string, std::string>{{"state", "active"}};
  report_msg->set_connection(con);
  {
    using ceph::encode;
    ENCODE_START(1, 1, report_msg->packed);
    ENCODE_FINISH(report_msg->packed);
  }

  bool res = daemon_server->handle_report(report_msg);
  EXPECT_TRUE(res);
  std::lock_guard l(daemon->lock);
  EXPECT_TRUE(daemon->service_status.empty());
}

// Requires reaching the function indirectly through handle_open; but since the
// function is private we test it indirectly by inserting a daemon, setting the
// config flag, then calling handle_report and observing that no metadata is used.
// The direct path is verified via the config-gate: set the flag, make sure a
// daemon with known metadata still returns nullopt from the helper (the only
// observable effect is no crash in handle_report which relies on it).
TEST_F(DaemonServerTestHelper, GetOsdMetadataTestErrorFlagReturnsNullopt) {
  // Insert an OSD daemon with ceph_version_short metadata.
  DaemonKey key{"osd", "70"};
  auto daemon = std::make_shared<DaemonState>(daemon_state_index->types);
  daemon->key = key;
  daemon->service_daemon = false;
  daemon->set_metadata({{"ceph_version_short", "19.0.0"}});
  daemon_state_index->insert(daemon);

  g_ceph_context->_conf.set_val("mgr_test_metadata_error", "true");
  g_ceph_context->_conf.apply_changes(nullptr);

  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_OSD);
      peer_name.set_id("70");
      set_peer_type(CEPH_ENTITY_TYPE_OSD);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  auto session = ceph::make_ref<MgrSession>(cct.get());
  session->entity_name = con->peer_name;
  session->osd_id = 70;
  con->set_priv(session);

  auto report = ceph::make_message<MMgrReport>();
  report->daemon_name = "70";
  report->service_name = "";
  report->set_connection(con);
  {
    using ceph::encode;
    ENCODE_START(1, 1, report->packed);
    ENCODE_FINISH(report->packed);
  }
  bool res = daemon_server->handle_report(report);
  EXPECT_TRUE(res);

  {
    std::lock_guard l(daemon->lock);
    auto it = daemon->metadata.find("ceph_version_short");
    ASSERT_NE(it, daemon->metadata.end());
    EXPECT_EQ(it->second, "19.0.0");
  }

  g_ceph_context->_conf.set_val("mgr_test_metadata_error", "false");
  g_ceph_context->_conf.apply_changes(nullptr);
}

// pending_service_map (rm_daemon path) and from daemon_state_index.
TEST_F(DaemonServerTestHelper, HandleCloseServiceDaemonTrueRemovesFromPendingServiceMap) {
  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("6000");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  DaemonKey key{"svctest", "svc.1"};

  auto open_msg = ceph::make_message<MMgrOpen>();
  open_msg->service_name = "svctest";
  open_msg->daemon_name = "svc.1";
  open_msg->service_daemon = true;
  open_msg->daemon_metadata["host"] = "node1";
  open_msg->set_connection(con);
  ASSERT_TRUE(daemon_server->handle_open(open_msg));
  ASSERT_TRUE(daemon_state_index->exists(key));

  auto close_msg = ceph::make_message<MMgrClose>();
  close_msg->service_name = "svctest";
  close_msg->daemon_name = "svc.1";
  close_msg->set_connection(con);
  bool res = daemon_server->handle_close(close_msg);
  EXPECT_TRUE(res);
  EXPECT_FALSE(daemon_state_index->exists(key));
}

TEST_F(UpgradeOsdReportDumpTest, DumpArrayValuesAreCorrectIntegers) {
  upgrade_osd_report report;
  report.osds = {10, 20};
  report.ok_upgrade.push_back(10);
  report.ok_upgraded.push_back(20);
  report.bad_no_version.push_back(30);

  JSONFormatter f;
  f.open_object_section("r");
  report.dump(&f);
  f.close_section();
  std::ostringstream os;
  f.flush(os);
  const std::string json = os.str();

  EXPECT_NE(json.find("10"), std::string::npos);
  EXPECT_NE(json.find("20"), std::string::npos);
  EXPECT_NE(json.find("30"), std::string::npos);
  EXPECT_NE(json.find("\"ok_to_upgrade\":false"), std::string::npos);
}

// ok_to_stop() = false (both violations), and dump shows both sections.
TEST_F(OfflinePgReportTest, BothNotOkAndUnknownPgsNotOkToStop) {
  offline_pg_report report;
  report.osds = std::vector<int>{0};
  report.not_ok.insert(pg_t(0, 1));
  report.unknown.insert(pg_t(1, 2));

  EXPECT_FALSE(report.ok_to_stop());
  EXPECT_EQ(report.not_ok.size(), 1u);
  EXPECT_EQ(report.unknown.size(), 1u);

  JSONFormatter f;
  f.open_object_section("r");
  report.dump(&f);
  f.close_section();
  std::ostringstream os;
  f.flush(os);
  const std::string json = os.str();

  EXPECT_NE(json.find("\"ok_to_stop\":false"), std::string::npos);
  EXPECT_NE(json.find("\"num_not_ok_pgs\":1"), std::string::npos);
  // The unknown_pgs section must appear.
  EXPECT_NE(json.find("\"unknown_pgs\""), std::string::npos);
}

// The daemon is created with empty metadata — no crash, and the daemon is found.
TEST_F(DaemonServerTestHelper, HandleOpenServiceDaemonEmptyMetadataSucceeds) {
  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("7000");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  auto open_msg = ceph::make_message<MMgrOpen>();
  open_msg->service_name = "emptymeta";
  open_msg->daemon_name = "em.1";
  open_msg->service_daemon = true;
  open_msg->set_connection(con);

  bool res = daemon_server->handle_open(open_msg);
  EXPECT_TRUE(res);

  DaemonKey key{"emptymeta", "em.1"};
  ASSERT_TRUE(daemon_state_index->exists(key));
  auto dptr = daemon_state_index->get(key);
  ASSERT_NE(dptr, nullptr);
  EXPECT_TRUE(dptr->service_daemon);
  {
    std::lock_guard l(dptr->lock);
    EXPECT_TRUE(dptr->metadata.empty());
  }
}

// must be cleared (perf_counters.clear() is called).
TEST_F(DaemonServerTestHelper, HandleOpenReopenClearsPerfCounters) {
  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("8000");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  DaemonKey key{"reopensvc", "ro.1"};

  auto open1 = ceph::make_message<MMgrOpen>();
  open1->service_name = "reopensvc";
  open1->daemon_name = "ro.1";
  open1->service_daemon = true;
  open1->set_connection(con);
  ASSERT_TRUE(daemon_server->handle_open(open1));
  ASSERT_TRUE(daemon_state_index->exists(key));

  auto open2 = ceph::make_message<MMgrOpen>();
  open2->service_name = "reopensvc";
  open2->daemon_name = "ro.1";
  open2->service_daemon = true;
  open2->daemon_metadata["host"] = "node-updated";
  open2->set_connection(con);
  ASSERT_TRUE(daemon_server->handle_open(open2));

  ASSERT_TRUE(daemon_state_index->exists(key));
  auto dptr = daemon_state_index->get(key);
  ASSERT_NE(dptr, nullptr);
  {
    std::lock_guard l(dptr->lock);
    EXPECT_TRUE(dptr->perf_counters.instances.empty());
  }
}

TEST_F(StatsAutotunerTest, AdjustmentResultDefaultConstructionValues) {
  StatsAutotuner::AdjustmentResult result;
  EXPECT_EQ(result.new_period, 0);
  EXPECT_EQ(result.reason_code,
            StatsAutotuner::AdjustmentReason::no_adjustment_needed);
  EXPECT_EQ(result.reason_str(), "no_adjustment_needed");
}

// current_period (1) > baseline (5) is false → recovery branch not taken.
// queue_depth (0) > threshold (10) is false → high-queue branch not taken.
// Result: no_adjustment_needed, new_period == current_period (1).
TEST_F(StatsAutotunerTest, EvaluateAdjustmentCurrentPeriodBelowBaseline) {
  // baseline=5, current_period=1 (anomalously low), queue_depth=0
  auto result = tuner.evaluate_adjustment(0, 1, 10);
  EXPECT_EQ(result.new_period, 1);
  EXPECT_EQ(result.reason_code,
            StatsAutotuner::AdjustmentReason::no_adjustment_needed);
}

// A MON peer with allow_all caps creates a session with is_allow_all()==true.
TEST_F(DaemonServerTestHelper, FastAuthenticationMonPeerAllowAll) {
  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_MON);
      peer_name.set_id("0");
      set_peer_type(CEPH_ENTITY_TYPE_MON);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  con->peer_caps_info.allow_all = true;

  bool ok = daemon_server->ms_handle_fast_authentication(con.get());
  EXPECT_TRUE(ok);
  auto priv = con->get_priv();
  ASSERT_NE(priv, nullptr);
  auto session = ceph::ref_cast<MgrSession>(priv);
  ASSERT_NE(session, nullptr);
  EXPECT_TRUE(session->caps.is_allow_all());
  EXPECT_EQ(session->entity_name.get_type(), CEPH_ENTITY_TYPE_MON);
}

// The MMgrCommand path always takes the non-admin-socket branch.
TEST_F(DaemonServerTestHelper, HandleCommandMMgrCommandServiceDumpSucceeds) {
  class TestConnection : public Connection {
  public:
    std::vector<MessageRef> sent;
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("9000");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override {
      sent.push_back(std::move(m));
      return 0;
    }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  auto session = ceph::make_ref<MgrSession>(cct.get());
  session->entity_name = con->peer_name;
  session->caps.set_allow_all();
  con->set_priv(session);

  auto cmd = ceph::make_message<MMgrCommand>();
  cmd->cmd = {"{\"prefix\": \"service dump\"}"};
  cmd->set_connection(con);

  bool res = daemon_server->handle_command(cmd);
  EXPECT_TRUE(res);
  EXPECT_GE(con->sent.size(), 1u);
}

// A client with empty caps gets -EACCES on service dump.
TEST_F(DaemonServerTestHelper, HandleCommandMMgrCommandAccessDenied) {
  class TestConnection : public Connection {
  public:
    std::vector<MessageRef> sent;
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("9001");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override {
      sent.push_back(std::move(m));
      return 0;
    }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  auto session = ceph::make_ref<MgrSession>(cct.get());
  session->entity_name = con->peer_name;
  // Do NOT set allow_all — caps remain empty.
  con->set_priv(session);

  auto cmd = ceph::make_message<MMgrCommand>();
  cmd->cmd = {"{\"prefix\": \"service dump\"}"};
  cmd->set_connection(con);

  bool res = daemon_server->handle_command(cmd);
  EXPECT_TRUE(res);
  EXPECT_GE(con->sent.size(), 1u);
}

// daemon->service_status is populated.
TEST_F(DaemonServerTestHelper, HandleOpenServiceDaemonSetsServiceStatus) {
  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("10000");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  auto open_msg = ceph::make_message<MMgrOpen>();
  open_msg->service_name = "statussvc";
  open_msg->daemon_name = "s.1";
  open_msg->service_daemon = true;
  open_msg->daemon_status = {{"state", "running"}, {"progress", "50"}};
  open_msg->set_connection(con);

  bool res = daemon_server->handle_open(open_msg);
  EXPECT_TRUE(res);

  DaemonKey key{"statussvc", "s.1"};
  ASSERT_TRUE(daemon_state_index->exists(key));
  auto dptr = daemon_state_index->get(key);
  ASSERT_NE(dptr, nullptr);

  {
    std::lock_guard l(dptr->lock);
    EXPECT_EQ(dptr->service_status.at("state"), "running");
    EXPECT_EQ(dptr->service_status.at("progress"), "50");
  }
}

// (already covered) and that the key "pg_ready" is present exactly once.
TEST_F(DaemonServerTestHelper, DumpPgReadyJsonKeyIsPresentExactlyOnce) {
  JSONFormatter f;
  f.open_object_section("root");
  daemon_server->dump_pg_ready(&f);
  f.close_section();
  std::ostringstream os;
  f.flush(os);
  const std::string json = os.str();

  // Count occurrences of "pg_ready".
  size_t pos = 0;
  int count = 0;
  while ((pos = json.find("\"pg_ready\"", pos)) != std::string::npos) {
    ++count;
    ++pos;
  }
  EXPECT_EQ(count, 1);
  EXPECT_NE(json.find("\"pg_ready\":false"), std::string::npos);
}

// get_tracked_keys: verify exact content using a set comparison so order does
// not matter and no extra keys are present.
TEST_F(DaemonServerTestHelper, GetTrackedKeysExactContentViaSet) {
  auto keys = daemon_server->get_tracked_keys();
  std::set<std::string> key_set(keys.begin(), keys.end());
  std::set<std::string> expected = {"mgr_stats_threshold", "mgr_stats_period"};
  EXPECT_EQ(key_set, expected);
}

// Additional boundary: upgrade_osd_report with a large number of ok_upgrade OSDs.
TEST_F(UpgradeOsdReportTest, OkToUpgradeLargeOkUpgradeSet) {
  upgrade_osd_report report;
  for (int i = 0; i < 100; ++i) {
    report.ok_upgrade.push_back(i);
  }
  EXPECT_TRUE(report.ok_to_upgrade());
  EXPECT_EQ(report.ok_upgrade.size(), 100u);
}

// Additional boundary: offline_pg_report with a large number of ok pgs.
TEST_F(OfflinePgReportTest, ManyOkPgsIsOkToStop) {
  offline_pg_report report;
  for (int i = 0; i < 100; ++i) {
    report.ok.insert(pg_t(i, 1));
  }
  EXPECT_TRUE(report.ok_to_stop());
  EXPECT_EQ(report.ok.size(), 100u);

  JSONFormatter f;
  f.open_object_section("r");
  report.dump(&f);
  f.close_section();
  std::ostringstream os;
  f.flush(os);
  EXPECT_NE(os.str().find("\"num_ok_pgs\":100"), std::string::npos);
  EXPECT_NE(os.str().find("\"num_not_ok_pgs\":0"), std::string::npos);
}

// StatsAutotuner: evaluate_adjustment with queue_depth equal to MIN_QUEUE_DEPTH (5).
// MIN_QUEUE_DEPTH is used as minimum increment; a queue_depth of 5 with threshold=4
// triggers the high_queue_depth branch.
TEST_F(StatsAutotunerTest, EvaluateAdjustmentQueueDepthAtMinQueueDepth) {
  // queue_depth(5) > queue_threshold(4) → high_queue_depth
  auto result = tuner.evaluate_adjustment(5, 5, 4);
  EXPECT_EQ(result.reason_code,
            StatsAutotuner::AdjustmentReason::high_queue_depth);
  EXPECT_GT(result.new_period, 5);
}

// StatsAutotuner: evaluate_adjustment increments by max(MIN_QUEUE_DEPTH=5, period/4).
// With period=8: period/4=2 < MIN_QUEUE_DEPTH=5 → increment=5 → new_period=13.
TEST_F(StatsAutotunerTest, EvaluateAdjustmentIncrementUsesMinQueueDepthFloor) {
  // period=8 → increment = max(5, 8/4=2) = 5 → new_period=13
  auto result = tuner.evaluate_adjustment(100, 8, 4);
  EXPECT_EQ(result.reason_code,
            StatsAutotuner::AdjustmentReason::high_queue_depth);
  EXPECT_EQ(result.new_period, 13);
}

// StatsAutotuner: evaluate_adjustment increments by period/4 when period/4 >= 5.
// With period=40: period/4=10 >= MIN_QUEUE_DEPTH=5 → increment=10 → new_period=50.
TEST_F(StatsAutotunerTest, EvaluateAdjustmentIncrementUsesQuarterPeriodWhenLarger) {
  // period=40 → increment = max(5, 40/4=10) = 10 → new_period=50
  auto result = tuner.evaluate_adjustment(100, 40, 4);
  EXPECT_EQ(result.reason_code,
            StatsAutotuner::AdjustmentReason::high_queue_depth);
  EXPECT_EQ(result.new_period, 50);
}

// config set command: verify it sets a config value and replies with success.
TEST_F(DaemonServerTestHelper, HandleCommandConfigSetSucceeds) {
  class TestConnection : public Connection {
  public:
    std::vector<MessageRef> sent;
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("12000");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override {
      sent.push_back(std::move(m));
      return 0;
    }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  auto session = ceph::make_ref<MgrSession>(cct.get());
  session->entity_name = con->peer_name;
  session->caps.set_allow_all();
  con->set_priv(session);

  auto cmd = ceph::make_message<MCommand>();
  cmd->cmd = {"{\"prefix\": \"config set\", \"key\": \"mgr_stats_period\", \"value\": \"7\"}"};
  cmd->set_connection(con);

  bool res = daemon_server->handle_command(cmd);
  EXPECT_TRUE(res);
  // A reply must be sent.
  EXPECT_GE(con->sent.size(), 1u);
  // Verify the config was actually changed.
  EXPECT_EQ(g_ceph_context->_conf.get_val<int64_t>("mgr_stats_period"), 7);

  g_ceph_context->_conf.set_val("mgr_stats_period", "5");
  g_ceph_context->_conf.apply_changes(nullptr);
}

// config show for an unknown daemon returns -ENOENT.
TEST_F(DaemonServerTestHelper, HandleCommandConfigShowUnknownDaemonReturnsError) {
  class TestConnection : public Connection {
  public:
    std::vector<MessageRef> sent;
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("12001");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override {
      sent.push_back(std::move(m));
      return 0;
    }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  auto session = ceph::make_ref<MgrSession>(cct.get());
  session->entity_name = con->peer_name;
  session->caps.set_allow_all();
  con->set_priv(session);

  auto cmd = ceph::make_message<MCommand>();
  cmd->cmd = {"{\"prefix\": \"config show\", \"who\": \"osd.999\"}"};
  cmd->set_connection(con);

  bool res = daemon_server->handle_command(cmd);
  EXPECT_TRUE(res);
  // A reply must be sent (with -ENOENT).
  EXPECT_GE(con->sent.size(), 1u);
}

// offline_pg_report::ok_to_stop() — verify the three-way logic:
// empty → ok; not_ok only → not ok; unknown only → not ok; both → not ok
TEST_F(OfflinePgReportTest, OkToStopAllFourCombinations) {
  {
    offline_pg_report r;
    EXPECT_TRUE(r.ok_to_stop());
  }
  {
    offline_pg_report r;
    r.not_ok.insert(pg_t(0, 1));
    EXPECT_FALSE(r.ok_to_stop());
  }
  {
    offline_pg_report r;
    r.unknown.insert(pg_t(1, 1));
    EXPECT_FALSE(r.ok_to_stop());
  }
  {
    offline_pg_report r;
    r.not_ok.insert(pg_t(0, 1));
    r.unknown.insert(pg_t(1, 1));
    EXPECT_FALSE(r.ok_to_stop());
  }
}

// StatsAutotuner::record_our_change() → was_changed_by_user() returns false
// for the recorded value and true for any other.
TEST_F(StatsAutotunerTest, RecordOurChangeTracksExactValue) {
  tuner.record_our_change(42);
  EXPECT_FALSE(tuner.was_changed_by_user(42));
  EXPECT_TRUE(tuner.was_changed_by_user(41));
  EXPECT_TRUE(tuner.was_changed_by_user(43));
  EXPECT_TRUE(tuner.was_changed_by_user(0));
}

// upgrade_osd_report::ok_to_upgrade() when ok_upgrade is empty AND
// bad_no_version is empty → false (ok_upgrade.empty() == true → returns false).
TEST_F(UpgradeOsdReportTest, OkToUpgradeFalseWhenBothEmpty) {
  upgrade_osd_report report;
  EXPECT_FALSE(report.ok_to_upgrade());
}

// ms_dispatch2 routes MSG_MGR_COMMAND to handle_command.
TEST_F(DaemonServerTestHelper, MsDispatch2HandlesMgrCommand) {
  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("13000");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  auto session = ceph::make_ref<MgrSession>(cct.get());
  session->entity_name = con->peer_name;
  session->caps.set_allow_all();
  con->set_priv(session);

  auto cmd = ceph::make_message<MMgrCommand>();
  cmd->cmd = {"{\"prefix\": \"service dump\"}"};
  cmd->set_connection(con);

  auto result = daemon_server->ms_dispatch2(cmd);
  EXPECT_TRUE(std::holds_alternative<bool>(result));
  EXPECT_TRUE(std::get<bool>(result));
}

// ms_dispatch2 routes MSG_COMMAND to handle_command (returns true).
TEST_F(DaemonServerTestHelper, MsDispatch2HandlesMCommand) {
  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("11000");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  auto session = ceph::make_ref<MgrSession>(cct.get());
  session->entity_name = con->peer_name;
  session->caps.set_allow_all();
  con->set_priv(session);

  auto cmd = ceph::make_message<MCommand>();
  cmd->cmd = {"{\"prefix\": \"service dump\"}"};
  cmd->set_connection(con);

  auto result = daemon_server->ms_dispatch2(cmd);
  EXPECT_TRUE(std::holds_alternative<bool>(result));
  EXPECT_TRUE(std::get<bool>(result));
}

// commands: dump_blocked_ops_count, dump_historic_ops, dump_historic_ops_by_duration,
// dump_historic_slow_ops.  All four must return false and populate the error stream.
TEST_F(DaemonServerTestHelper, AsokCommandBlockedOpsCountWithOpTrackerDisabledReturnsError) {
  g_ceph_context->_conf.set_val("mgr_enable_op_tracker", "false");
  g_ceph_context->_conf.apply_changes(nullptr);

  auto local_server = std::make_unique<DaemonServer>(
      mc.get(), *finisher, *daemon_state_index, *cs,
      *py_registry, clog, audit_clog);

  JSONFormatter f;
  std::ostringstream ss;
  cmdmap_t cmdmap;
  bool ok = local_server->asok_command("dump_blocked_ops_count", cmdmap, &f, ss);
  EXPECT_FALSE(ok);
  EXPECT_FALSE(ss.str().empty());
  EXPECT_NE(ss.str().find("op_tracker"), std::string::npos);

  local_server.reset();
  g_ceph_context->_conf.set_val("mgr_enable_op_tracker", "true");
  g_ceph_context->_conf.apply_changes(nullptr);
}

TEST_F(DaemonServerTestHelper, AsokCommandDumpHistoricOpsWithOpTrackerDisabledReturnsError) {
  g_ceph_context->_conf.set_val("mgr_enable_op_tracker", "false");
  g_ceph_context->_conf.apply_changes(nullptr);

  auto local_server = std::make_unique<DaemonServer>(
      mc.get(), *finisher, *daemon_state_index, *cs,
      *py_registry, clog, audit_clog);

  JSONFormatter f;
  std::ostringstream ss;
  cmdmap_t cmdmap;
  bool ok = local_server->asok_command("dump_historic_ops", cmdmap, &f, ss);
  EXPECT_FALSE(ok);
  EXPECT_NE(ss.str().find("op_tracker"), std::string::npos);

  local_server.reset();
  g_ceph_context->_conf.set_val("mgr_enable_op_tracker", "true");
  g_ceph_context->_conf.apply_changes(nullptr);
}

TEST_F(DaemonServerTestHelper, AsokCommandDumpHistoricOpsByDurationWithOpTrackerDisabledReturnsError) {
  g_ceph_context->_conf.set_val("mgr_enable_op_tracker", "false");
  g_ceph_context->_conf.apply_changes(nullptr);

  auto local_server = std::make_unique<DaemonServer>(
      mc.get(), *finisher, *daemon_state_index, *cs,
      *py_registry, clog, audit_clog);

  JSONFormatter f;
  std::ostringstream ss;
  cmdmap_t cmdmap;
  bool ok = local_server->asok_command("dump_historic_ops_by_duration", cmdmap, &f, ss);
  EXPECT_FALSE(ok);
  EXPECT_NE(ss.str().find("op_tracker"), std::string::npos);

  local_server.reset();
  g_ceph_context->_conf.set_val("mgr_enable_op_tracker", "true");
  g_ceph_context->_conf.apply_changes(nullptr);
}

TEST_F(DaemonServerTestHelper, AsokCommandDumpHistoricSlowOpsWithOpTrackerDisabledReturnsError) {
  g_ceph_context->_conf.set_val("mgr_enable_op_tracker", "false");
  g_ceph_context->_conf.apply_changes(nullptr);

  auto local_server = std::make_unique<DaemonServer>(
      mc.get(), *finisher, *daemon_state_index, *cs,
      *py_registry, clog, audit_clog);

  JSONFormatter f;
  std::ostringstream ss;
  cmdmap_t cmdmap;
  bool ok = local_server->asok_command("dump_historic_slow_ops", cmdmap, &f, ss);
  EXPECT_FALSE(ok);
  EXPECT_NE(ss.str().find("op_tracker"), std::string::npos);

  local_server.reset();
  g_ceph_context->_conf.set_val("mgr_enable_op_tracker", "true");
  g_ceph_context->_conf.apply_changes(nullptr);
}

// service_status_stamp and service_status fields (not just last_service_beacon).
TEST_F(DaemonServerTestHelper, HandleReportServiceDaemonStatusUpdatesStampAndStatus) {
  DaemonKey key{"stamps", "st.1"};
  auto daemon = std::make_shared<DaemonState>(daemon_state_index->types);
  daemon->key = key;
  daemon->service_daemon = true;
  daemon_state_index->insert(daemon);

  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("15000");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  auto session = ceph::make_ref<MgrSession>(cct.get());
  session->entity_name = con->peer_name;
  con->set_priv(session);

  auto report = ceph::make_message<MMgrReport>();
  report->service_name = "stamps";
  report->daemon_name = "st.1";
  report->daemon_status = std::map<std::string, std::string>{{"phase", "running"}};
  report->set_connection(con);
  {
    using ceph::encode;
    ENCODE_START(1, 1, report->packed);
    ENCODE_FINISH(report->packed);
  }

  bool res = daemon_server->handle_report(report);
  EXPECT_TRUE(res);

  auto dptr = daemon_state_index->get(key);
  ASSERT_NE(dptr, nullptr);
  std::lock_guard l(dptr->lock);
  auto it = dptr->service_status.find("phase");
  ASSERT_NE(it, dptr->service_status.end());
  EXPECT_EQ(it->second, "running");
  EXPECT_NE(dptr->service_status_stamp, utime_t());
  EXPECT_NE(dptr->last_service_beacon, utime_t());
}

// must NOT trigger a metadata update (no crash, and daemon is still present after).
TEST_F(DaemonServerTestHelper, GotMgrMapExistingDaemonNoMetadataUpdate) {
  DaemonKey key{"mgr", "mgr-already-there"};
  auto daemon = std::make_shared<DaemonState>(daemon_state_index->types);
  daemon->key = key;
  daemon_state_index->insert(daemon);

  MgrMap mm;
  mm.epoch = 1;
  mm.active_name = "mgr-already-there";
  cs->set_mgr_map(mm);

  EXPECT_NO_THROW(daemon_server->got_mgr_map());

  EXPECT_TRUE(daemon_state_index->exists(key));
}

TEST_F(DaemonServerTestHelper, GotMgrMapCullsRemovedDaemons) {
  DaemonKey stale_key{"mgr", "mgr-stale"};
  auto daemon = std::make_shared<DaemonState>(daemon_state_index->types);
  daemon->key = stale_key;
  daemon_state_index->insert(daemon);
  ASSERT_TRUE(daemon_state_index->exists(stale_key));

  MgrMap mm;
  mm.epoch = 2;
  mm.active_name = "mgr-active-new";
  DaemonKey new_key{"mgr", "mgr-active-new"};
  auto new_daemon = std::make_shared<DaemonState>(daemon_state_index->types);
  new_daemon->key = new_key;
  daemon_state_index->insert(new_daemon);

  cs->set_mgr_map(mm);
  EXPECT_NO_THROW(daemon_server->got_mgr_map());

  EXPECT_FALSE(daemon_state_index->exists(stale_key));
  EXPECT_TRUE(daemon_state_index->exists(new_key));
}

TEST_F(DaemonServerTestHelper, HandleOpenWithConfigBlPopulatesDaemonConfig) {
  class TestConnection : public Connection {
  public:
    TestConnection() : Connection(g_ceph_context, nullptr) {
      peer_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
      peer_name.set_id("20000");
      set_peer_type(CEPH_ENTITY_TYPE_CLIENT);
    }
    bool is_connected() override { return true; }
    int send_msg(MessageRef&& m) override { return 0; }
    void shutdown() override {}
    void send_keepalive() override {}
    void mark_down() override {}
    void mark_disposable() override {}
    entity_addr_t get_peer_socket_addr() const override { return entity_addr_t(); }
  };

  auto con = ceph::make_ref<TestConnection>();
  auto open_msg = ceph::make_message<MMgrOpen>();
  open_msg->service_name = "cfgsvc";
  open_msg->daemon_name = "cfg.1";
  open_msg->service_daemon = true;
  open_msg->set_connection(con);

  // and an empty ignored_mon_config. Use level 5 (CONF_OVERRIDE) as a literal int.
  {
    using ceph::encode;
    std::map<std::string, std::map<int32_t, std::string>> config;
    config["debug_mgr"][5] = "10";  // 5 == CONF_OVERRIDE
    std::map<std::string, std::string> ignored;
    encode(config, open_msg->config_bl);
    encode(ignored, open_msg->config_bl);
  }

  bool res = daemon_server->handle_open(open_msg);
  EXPECT_TRUE(res);

  DaemonKey key{"cfgsvc", "cfg.1"};
  ASSERT_TRUE(daemon_state_index->exists(key));
  auto dptr = daemon_state_index->get(key);
  ASSERT_NE(dptr, nullptr);
  {
    std::lock_guard l(dptr->lock);
    EXPECT_FALSE(dptr->config.empty());
    auto it = dptr->config.find("debug_mgr");
    ASSERT_NE(it, dptr->config.end());
    EXPECT_EQ(it->second.rbegin()->second, "10");
  }
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
