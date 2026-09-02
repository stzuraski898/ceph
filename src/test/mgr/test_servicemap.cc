// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <fmt/format.h>

#include <map>
#include <sstream>
#include <string>

#include "common/Formatter.h"
#include "common/JSONFormatter.h"
#include "gtest/gtest.h"
#include "include/buffer.h"
#include "include/ceph_features.h"
#include "mgr/ServiceMap.h"
#include "msg/msg_types.h"

TEST(ServiceMapDaemon, EncodeDecode)
{
  ServiceMap::Daemon d_1;
  d_1.gid = 123;
  d_1.metadata["test"] = "ing";

  bufferlist bl;
  d_1.encode(bl, 0);
  auto p = bl.cbegin();

  ServiceMap::Daemon d_2;
  d_2.decode(p);

  EXPECT_EQ(d_2.gid, 123);
  EXPECT_EQ(d_2.metadata["test"], "ing");
}

TEST(ServiceMapDaemon, Dump)
{
  ServiceMap::Daemon test_daemon;
  test_daemon.gid = 11;

  entity_addr_t addr;
  addr.parse("127.0.0.1:1234", nullptr);
  test_daemon.addr = addr;
  test_daemon.start_epoch = 33;
  test_daemon.start_stamp = utime_t(55, 5500000);

  //dump_object will call ServiceMap::Daemon::dump wrapped in an open/close section
  JSONFormatter f;
  f.dump_object("daemon", test_daemon);

  std::ostringstream oss;
  f.flush(oss);

  std::string json_str;
  json_str = oss.str();

  EXPECT_TRUE(json_str.contains("\"gid\":11"));
  EXPECT_TRUE(json_str.contains("127.0.0.1:1234"));
  EXPECT_TRUE(json_str.contains("\"start_epoch\":33"));
  EXPECT_TRUE(json_str.contains("\"start_stamp\":\"55.005500\""));
}

TEST(ServiceMapService, GetSummary)
{
  ServiceMap::Service sms;
  sms.summary = "test summary";

  EXPECT_EQ(sms.get_summary(), "test summary");
}

TEST(ServiceMapService, HasRunningTasks)
{
  ServiceMap::Service sms;

  EXPECT_FALSE(sms.has_running_tasks());

  sms.daemons["test"].task_status["task"] = "running";

  EXPECT_TRUE(sms.has_running_tasks());
}

TEST(ServiceMapService, GetTaskSummary)
{
  ServiceMap::Service sms;
  sms.daemons["test"].task_status["task"] = "running";
  sms.daemons["test2"].task_status["task"] = "idle";

  const std::string expected =
      "\n"
      "    task:\n"
      "        svc.test: running\n"
      "        svc.test2: idle";
  EXPECT_EQ(sms.get_task_summary("svc"), expected);
}

TEST(ServiceMapService, CountMetadata)
{
  ServiceMap::Service sms;
  sms.daemons["daemon1"].metadata["test"] = "t1";
  sms.daemons["daemon2"].metadata["test"] = "t2";
  sms.daemons["daemon3"].task_status["abc"] = "xyz";

  std::map<std::string, int> metadata_info;
  sms.count_metadata("test", &metadata_info);

  EXPECT_EQ(metadata_info["t1"], 1);
  EXPECT_EQ(metadata_info["t2"], 1);
  EXPECT_EQ(metadata_info["unknown"], 1);
}

TEST(ServiceMapService, EncodeDecode)
{
  ServiceMap::Service s_1;
  s_1.summary = "summary";
  s_1.daemons["d"].gid = 99;

  bufferlist bl;
  s_1.encode(bl, 0);
  auto p = bl.cbegin();

  ServiceMap::Service s_2;
  s_2.decode(p);

  EXPECT_EQ(s_2.summary, "summary");
  EXPECT_EQ(s_2.daemons["d"].gid, 99);
}

TEST(ServiceMapService, Dump)
{
  entity_addr_t addr;
  addr.parse("127.0.0.1:1234", nullptr);

  ServiceMap::Service sms;
  sms.summary = "testSummary";
  sms.daemons["d123"].gid = 123;
  sms.daemons["d123"].addr = addr;

  JSONFormatter f;
  f.dump_object("service", sms);

  std::ostringstream oss;
  f.flush(oss);

  std::string json_str;
  json_str = oss.str();

  const std::string expected =
      R"({"daemons":{"summary":"testSummary","d123":{"start_epoch":0,"start_stamp":"0.000000","gid":123,"addr":"127.0.0.1:1234/0","metadata":{},"task_status":{}}}})";
  EXPECT_EQ(json_str, expected);
}

TEST(ServiceMapMap, EncodeDecode)
{
  ServiceMap m_1;
  m_1.epoch = 123;
  m_1.services["svc"].summary = "abc";

  bufferlist bl;
  m_1.encode(bl, 0);
  auto p = bl.cbegin();

  ServiceMap m_2;
  m_2.decode(p);

  EXPECT_EQ(m_2.epoch, 123);
  EXPECT_EQ(m_2.services["svc"].summary, "abc");
}

TEST(ServiceMapMap, Dump)
{
  ServiceMap sm;
  sm.epoch = 123;
  sm.services["svc"].summary = "testSummary";

  JSONFormatter f;
  f.dump_object("servicemap", sm);

  std::ostringstream oss;
  f.flush(oss);

  std::string json_str;
  json_str = oss.str();

  const std::string expected =
      R"({"epoch":123,"modified":"0.000000","services":{"svc":{"daemons":{"summary":"testSummary"}}}})";
  EXPECT_EQ(json_str, expected);
}

TEST(ServiceMapDaemon, EncodeDecodeWithEmptyMetadata)
{
  ServiceMap::Daemon d_1;

  d_1.gid = 123;
  bufferlist bl;
  d_1.encode(bl, 0);
  auto p = bl.cbegin();

  ServiceMap::Daemon d_2;
  d_2.decode(p);

  EXPECT_EQ(d_2.gid, 123);
  EXPECT_TRUE(d_2.metadata.empty());
}

TEST(ServiceMapDaemon, DumpWithEmptyMetadata)
{
  std::ostringstream oss;
  JSONFormatter f(true);
  ServiceMap::Daemon d;

  d.gid = 123;
  f.dump_object("daemon", d);
  f.flush(oss);
  const std::string output = oss.str();

  EXPECT_TRUE(output.contains("\"gid\": 123"));
  EXPECT_TRUE(output.contains("\"metadata\": {}"));
}

TEST(ServiceMapService, GetSummaryEmptyString)
{
  ServiceMap::Service s;

  s.summary = "";

  EXPECT_EQ(s.get_summary(), "no daemons active");
}

TEST(ServiceMapService, GetTaskSummaryEmptyDaemons)
{
  ServiceMap::Service s;
  std::string summary = s.get_task_summary("svc");

  EXPECT_EQ(summary, "");
}

TEST(ServiceMapService, GetTaskSummaryEmptyTaskStatus)
{
  ServiceMap::Service s;

  s.daemons["d1"].gid = 1;
  std::string summary = s.get_task_summary("svc");

  EXPECT_EQ(summary, "");
}

TEST(ServiceMapService, CountMetadataEmptyKey)
{
  ServiceMap::Service s;
  std::map<std::string, int> out;

  s.daemons["d1"].metadata["zone"] = "z1";
  s.count_metadata("", &out);

  EXPECT_EQ(out["unknown"], 1);
}

TEST(ServiceMapService, CountMetadataDNEKey)
{
  ServiceMap::Service s;
  std::map<std::string, int> out;

  s.daemons["d1"].metadata["zone"] = "z1";
  s.count_metadata("DNE", &out);

  EXPECT_EQ(out["unknown"], 1);
}

TEST(ServiceMapService, EncodeDecodeEmptyDaemons)
{
  ServiceMap::Service s_1;
  ServiceMap::Service s_2;
  bufferlist bl;

  s_1.summary = "summary";
  s_1.encode(bl, 0);
  auto p = bl.cbegin();
  s_2.decode(p);

  EXPECT_EQ(s_2.summary, "summary");
  EXPECT_TRUE(s_2.daemons.empty());
}

TEST(ServiceMapMap, EncodeDecodeEmptyServices)
{
  ServiceMap m_1;
  ServiceMap m_2;
  bufferlist bl;

  m_1.epoch = 123;
  m_1.encode(bl, 0);
  auto p = bl.cbegin();
  m_2.decode(p);

  EXPECT_EQ(m_2.epoch, 123);
  EXPECT_TRUE(m_2.services.empty());
}

TEST(ServiceMapService, GetSummaryNoDaemonsActive)
{
  ServiceMap::Service s;
  EXPECT_EQ(s.get_summary(), "no daemons active");
}

TEST(ServiceMapService, GetSummaryOneDaemon)
{
  // empty summary + one daemon => auto-generated "1 daemon active"
  ServiceMap::Service s;
  s.daemons["inst0"].gid = 1;
  EXPECT_EQ(s.get_summary(), "1 daemon active");
}

TEST(ServiceMapService, GetSummaryMultipleDaemons)
{
  ServiceMap::Service s;
  s.daemons["inst0"].gid = 1;
  s.daemons["inst1"].gid = 2;
  s.daemons["inst2"].gid = 3;
  EXPECT_EQ(s.get_summary(), "3 daemons active");
}

TEST(ServiceMapService, GetSummaryWithDaemonType)
{
  ServiceMap::Service s;
  s.daemons["gw0"].metadata["daemon_type"] = "gateway";
  s.daemons["gw1"].metadata["daemon_type"] = "gateway";
  EXPECT_EQ(s.get_summary(), "2 gateways active");
}

TEST(ServiceMapService, GetSummaryWithHostname)
{
  ServiceMap::Service s;
  s.daemons["d0"].metadata["hostname"] = "host-a";
  s.daemons["d1"].metadata["hostname"] = "host-b";
  const std::string summary = s.get_summary();
  EXPECT_TRUE(summary.find("2 daemons active") != std::string::npos);
  EXPECT_TRUE(summary.find("2 hosts") != std::string::npos);
}

TEST(ServiceMapService, GetSummaryWithZone)
{
  ServiceMap::Service s;
  s.daemons["d0"].metadata["zone_id"] = "us-east";
  s.daemons["d1"].metadata["zone_id"] = "us-east";
  s.daemons["d2"].metadata["zone_id"] = "us-west";
  const std::string summary = s.get_summary();
  EXPECT_TRUE(summary.find("3 daemons active") != std::string::npos);
  EXPECT_TRUE(summary.find("2 zones") != std::string::npos);
}

TEST(ServiceMapService, GetSummaryWithHostAndZone)
{
  ServiceMap::Service s;
  s.daemons["d0"].metadata["hostname"] = "h1";
  s.daemons["d0"].metadata["zone_id"] = "z1";
  s.daemons["d1"].metadata["hostname"] = "h2";
  s.daemons["d1"].metadata["zone_id"] = "z1";
  const std::string summary = s.get_summary();
  EXPECT_TRUE(summary.find("2 daemons active") != std::string::npos);
  EXPECT_TRUE(summary.find("2 hosts") != std::string::npos);
  EXPECT_TRUE(summary.find("1 zone") != std::string::npos);
}

TEST(ServiceMapMap, GetDaemonNewService)
{
  ServiceMap sm;
  auto [d, added] = sm.get_daemon("rgw", "inst0");
  EXPECT_TRUE(added);
  ASSERT_NE(d, nullptr);
  EXPECT_EQ(sm.services.count("rgw"), 1u);
  EXPECT_EQ(sm.services["rgw"].daemons.count("inst0"), 1u);
}

TEST(ServiceMapMap, GetDaemonExistingService)
{
  ServiceMap sm;
  sm.get_daemon("rgw", "inst0");
  // Second call: same service, same daemon — not added
  auto [d, added] = sm.get_daemon("rgw", "inst0");
  EXPECT_FALSE(added);
  ASSERT_NE(d, nullptr);
}

TEST(ServiceMapMap, GetDaemonTwoDaemons)
{
  ServiceMap sm;
  auto [d0, added0] = sm.get_daemon("rgw", "inst0");
  auto [d1, added1] = sm.get_daemon("rgw", "inst1");
  EXPECT_TRUE(added0);
  EXPECT_TRUE(added1);
  EXPECT_EQ(sm.services["rgw"].daemons.size(), 2u);
}

TEST(ServiceMapMap, GetDaemonSetsPointer)
{
  ServiceMap sm;
  auto [d, added] = sm.get_daemon("iscsi", "portal0");
  EXPECT_TRUE(added);
  d->gid = 42;
  EXPECT_EQ(sm.services["iscsi"].daemons["portal0"].gid, 42u);
}

TEST(ServiceMapMap, RmDaemonExisting)
{
  ServiceMap sm;
  sm.get_daemon("rgw", "inst0");
  EXPECT_TRUE(sm.rm_daemon("rgw", "inst0"));
  // service should be removed when its last daemon is removed
  EXPECT_EQ(sm.services.count("rgw"), 0u);
}

TEST(ServiceMapMap, RmDaemonNonExistentService)
{
  ServiceMap sm;
  EXPECT_FALSE(sm.rm_daemon("rgw", "inst0"));
}

TEST(ServiceMapMap, RmDaemonNonExistentDaemon)
{
  ServiceMap sm;
  sm.get_daemon("rgw", "inst0");
  EXPECT_FALSE(sm.rm_daemon("rgw", "inst99"));
  // inst0 should still be there
  EXPECT_EQ(sm.services["rgw"].daemons.count("inst0"), 1u);
}

TEST(ServiceMapMap, RmDaemonKeepsServiceWithRemainingDaemons)
{
  ServiceMap sm;
  sm.get_daemon("rgw", "inst0");
  sm.get_daemon("rgw", "inst1");
  EXPECT_TRUE(sm.rm_daemon("rgw", "inst0"));
  // service should survive because inst1 is still present
  EXPECT_EQ(sm.services.count("rgw"), 1u);
  EXPECT_EQ(sm.services["rgw"].daemons.count("inst1"), 1u);
}

TEST(ServiceMapMap, IsNormalCephEntityOsd)
{
  EXPECT_TRUE(ServiceMap::is_normal_ceph_entity("osd"));
}

TEST(ServiceMapMap, IsNormalCephEntityClient)
{
  EXPECT_TRUE(ServiceMap::is_normal_ceph_entity("client"));
}

TEST(ServiceMapMap, IsNormalCephEntityMon)
{
  EXPECT_TRUE(ServiceMap::is_normal_ceph_entity("mon"));
}

TEST(ServiceMapMap, IsNormalCephEntityMds)
{
  EXPECT_TRUE(ServiceMap::is_normal_ceph_entity("mds"));
}

TEST(ServiceMapMap, IsNormalCephEntityMgr)
{
  EXPECT_TRUE(ServiceMap::is_normal_ceph_entity("mgr"));
}

TEST(ServiceMapMap, IsNormalCephEntityRgwIsFalse)
{
  EXPECT_FALSE(ServiceMap::is_normal_ceph_entity("rgw"));
}

TEST(ServiceMapMap, IsNormalCephEntityIscsiIsFalse)
{
  EXPECT_FALSE(ServiceMap::is_normal_ceph_entity("iscsi"));
}

TEST(ServiceMapMap, IsNormalCephEntityEmptyIsFalse)
{
  EXPECT_FALSE(ServiceMap::is_normal_ceph_entity(""));
}

TEST(ServiceMapDaemon, GenerateTestInstances)
{
  auto instances = ServiceMap::Daemon::generate_test_instances();
  EXPECT_EQ(instances.size(), 2u);
  auto it = instances.begin();
  // first instance is default-constructed
  EXPECT_EQ(it->gid, 0u);
  ++it;
  // second instance has known fields set
  EXPECT_EQ(it->gid, 222u);
  EXPECT_EQ(it->metadata.at("this"), "that");
  EXPECT_EQ(it->task_status.at("task1"), "running");
}

TEST(ServiceMapService, GenerateTestInstances)
{
  auto instances = ServiceMap::Service::generate_test_instances();
  EXPECT_EQ(instances.size(), 2u);
  auto it = instances.begin();
  EXPECT_TRUE(it->daemons.empty());
  ++it;
  EXPECT_EQ(it->daemons.at("one").gid, 1u);
  EXPECT_EQ(it->daemons.at("two").gid, 2u);
}

TEST(ServiceMapMap, GenerateTestInstances)
{
  auto instances = ServiceMap::generate_test_instances();
  EXPECT_EQ(instances.size(), 2u);
  auto it = instances.begin();
  EXPECT_EQ(it->epoch, 0u);
  EXPECT_TRUE(it->services.empty());
  ++it;
  EXPECT_EQ(it->epoch, 123u);
  EXPECT_EQ(it->services.at("rgw").daemons.at("one").gid, 123u);
  EXPECT_EQ(it->services.at("rgw").daemons.at("two").gid, 344u);
  EXPECT_EQ(it->services.at("iscsi").daemons.at("foo").gid, 3222u);
}

TEST(ServiceMapDaemon, EncodeDecodeWithTaskStatus)
{
  ServiceMap::Daemon d_1;
  d_1.gid = 77;
  d_1.task_status["rebalance"] = "active";
  d_1.task_status["scrub"] = "idle";

  bufferlist bl;
  d_1.encode(bl, 0);
  auto p = bl.cbegin();

  ServiceMap::Daemon d_2;
  d_2.decode(p);

  EXPECT_EQ(d_2.gid, 77u);
  EXPECT_EQ(d_2.task_status.at("rebalance"), "active");
  EXPECT_EQ(d_2.task_status.at("scrub"), "idle");
}

TEST(ServiceMapDaemon, DumpWithTaskStatus)
{
  ServiceMap::Daemon d;
  d.gid = 5;
  d.task_status["rebalance"] = "active";

  JSONFormatter f;
  f.dump_object("daemon", d);
  std::ostringstream oss;
  f.flush(oss);
  const std::string json_str = oss.str();

  EXPECT_TRUE(json_str.find("\"gid\":5") != std::string::npos);
  EXPECT_TRUE(json_str.find("\"rebalance\":\"active\"") != std::string::npos);
}

TEST(ServiceMapMap, EncodeDecodePreservesModified)
{
  ServiceMap m_1;
  m_1.epoch = 7;
  m_1.modified = utime_t(1000, 500000);

  bufferlist bl;
  m_1.encode(bl, 0);
  auto p = bl.cbegin();

  ServiceMap m_2;
  m_2.decode(p);

  EXPECT_EQ(m_2.epoch, 7u);
  EXPECT_EQ(m_2.modified, utime_t(1000, 500000));
}

TEST(ServiceMapMap, DumpWithModified)
{
  ServiceMap sm;
  sm.epoch = 5;
  sm.modified = utime_t(1000, 0);

  JSONFormatter f;
  f.dump_object("servicemap", sm);
  std::ostringstream oss;
  f.flush(oss);
  const std::string json_str = oss.str();

  EXPECT_TRUE(json_str.find("\"epoch\":5") != std::string::npos);
  EXPECT_TRUE(json_str.find("\"modified\":\"1000.000000\"") != std::string::npos);
}

TEST(ServiceMapService, GetTaskSummaryMultipleTasks)
{
  ServiceMap::Service s;
  s.daemons["d1"].task_status["scrub"] = "running";
  s.daemons["d1"].task_status["rebalance"] = "idle";
  s.daemons["d2"].task_status["scrub"] = "idle";

  const std::string summary = s.get_task_summary("svc");
  // Both task names must appear
  EXPECT_TRUE(summary.find("scrub:") != std::string::npos);
  EXPECT_TRUE(summary.find("rebalance:") != std::string::npos);
  // Daemon references must appear with the correct prefix
  EXPECT_TRUE(summary.find("svc.d1") != std::string::npos);
  EXPECT_TRUE(summary.find("svc.d2") != std::string::npos);
}

TEST(ServiceMapDaemon, EncodeDecodeAllFields)
{
  // Verify that start_epoch, start_stamp, and addr all survive round-trip
  ServiceMap::Daemon d_1;
  d_1.gid = 10;
  d_1.start_epoch = 5;
  d_1.start_stamp = utime_t(200, 100000);
  entity_addr_t addr;
  addr.parse("10.0.0.1:6789", nullptr);
  d_1.addr = addr;
  d_1.metadata["host"] = "ceph-node-1";

  bufferlist bl;
  d_1.encode(bl, 0);
  auto p = bl.cbegin();

  ServiceMap::Daemon d_2;
  d_2.decode(p);

  EXPECT_EQ(d_2.gid, 10u);
  EXPECT_EQ(d_2.start_epoch, 5u);
  EXPECT_EQ(d_2.start_stamp, utime_t(200, 100000));
  EXPECT_EQ(d_2.metadata.at("host"), "ceph-node-1");
  EXPECT_EQ(d_2.addr.get_legacy_str(), addr.get_legacy_str());
}

TEST(ServiceMapService, DumpIncludesTaskStatus)
{
  ServiceMap::Service sms;
  sms.summary = "active";
  sms.daemons["gw0"].gid = 7;
  sms.daemons["gw0"].task_status["rebalance"] = "running";

  JSONFormatter f;
  f.dump_object("service", sms);
  std::ostringstream oss;
  f.flush(oss);
  const std::string json_str = oss.str();

  EXPECT_TRUE(json_str.find("\"gid\":7") != std::string::npos);
  EXPECT_TRUE(json_str.find("\"rebalance\":\"running\"") != std::string::npos);
  EXPECT_TRUE(json_str.find("\"summary\":\"active\"") != std::string::npos);
}

TEST(ServiceMapMap, GetDaemonTwoServices)
{
  ServiceMap sm;
  auto [d0, added0] = sm.get_daemon("rgw", "inst0");
  auto [d1, added1] = sm.get_daemon("iscsi", "portal0");
  EXPECT_TRUE(added0);
  EXPECT_TRUE(added1);
  EXPECT_EQ(sm.services.size(), 2u);
  EXPECT_EQ(sm.services.count("rgw"), 1u);
  EXPECT_EQ(sm.services.count("iscsi"), 1u);
}

TEST(ServiceMapService, GetSummaryDaemonTypeSingular)
{
  ServiceMap::Service s;
  s.daemons["gw0"].metadata["daemon_type"] = "gateway";
  // 1 gateway active (singular — no trailing 's')
  EXPECT_EQ(s.get_summary(), "1 gateway active");
}

// The grouping label always appends 's' regardless of count (implementation
// uses `size() ? "s" : ""` which is truthy for any non-zero size).
TEST(ServiceMapService, GetSummaryAllSameHost)
{
  ServiceMap::Service s;
  s.daemons["d0"].metadata["hostname"] = "node-1";
  s.daemons["d1"].metadata["hostname"] = "node-1";
  const std::string summary = s.get_summary();
  EXPECT_TRUE(summary.find("2 daemons active") != std::string::npos);
  // One unique host value — grouping always pluralises → "1 hosts"
  EXPECT_TRUE(summary.find("1 hosts") != std::string::npos);
}

TEST(ServiceMapService, GetSummaryDaemonTypeWithZone)
{
  ServiceMap::Service s;
  s.daemons["gw0"].metadata["daemon_type"] = "gateway";
  s.daemons["gw0"].metadata["zone_id"] = "us-east";
  s.daemons["gw1"].metadata["daemon_type"] = "gateway";
  s.daemons["gw1"].metadata["zone_id"] = "us-west";
  const std::string summary = s.get_summary();
  EXPECT_TRUE(summary.find("2 gateways active") != std::string::npos);
  EXPECT_TRUE(summary.find("2 zones") != std::string::npos);
  // No hostname metadata → no "host" grouping
  EXPECT_TRUE(summary.find("host") == std::string::npos);
}

TEST(ServiceMapService, CountMetadataDuplicateValue)
{
  ServiceMap::Service s;
  s.daemons["d0"].metadata["zone"] = "us-east";
  s.daemons["d1"].metadata["zone"] = "us-east";
  s.daemons["d2"].metadata["zone"] = "us-west";

  std::map<std::string, int> out;
  s.count_metadata("zone", &out);

  EXPECT_EQ(out["us-east"], 2);
  EXPECT_EQ(out["us-west"], 1);
}

// The key exists so it should be stored under "" not "unknown"
TEST(ServiceMapService, CountMetadataEmptyValue)
{
  ServiceMap::Service s;
  s.daemons["d0"].metadata["zone"] = "";  // key present, value is empty string

  std::map<std::string, int> out;
  s.count_metadata("zone", &out);

  EXPECT_EQ(out[""], 1);
  EXPECT_EQ(out["unknown"], 0);
}

TEST(ServiceMapMap, GetDaemonExistingRetainsData)
{
  ServiceMap sm;
  auto [d0, added0] = sm.get_daemon("rgw", "inst0");
  d0->gid = 77;
  d0->metadata["host"] = "node-1";

  // Second get_daemon call: same service/daemon — should not reset fields
  auto [d1, added1] = sm.get_daemon("rgw", "inst0");
  EXPECT_FALSE(added1);
  ASSERT_NE(d1, nullptr);
  EXPECT_EQ(d1->gid, 77u);
  EXPECT_EQ(d1->metadata.at("host"), "node-1");
}

// service removal is driven solely by an empty daemons map, not by the summary string
TEST(ServiceMapMap, RmDaemonServiceWithSummaryGetsRemoved)
{
  ServiceMap sm;
  sm.get_daemon("rgw", "inst0");
  sm.services["rgw"].summary = "some status text";

  EXPECT_TRUE(sm.rm_daemon("rgw", "inst0"));
  // service removed even though it had a summary
  EXPECT_EQ(sm.services.count("rgw"), 0u);
}

// The loop overwrites `type` for every daemon that has the key, so the last
// alphabetically-ordered daemon with daemon_type set wins.
TEST(ServiceMapService, GetSummaryMixedDaemonType)
{
  ServiceMap::Service s;
  // "a_gw" has daemon_type; "b_plain" does not.  Map iteration is ordered,
  // so "a_gw" is visited first and sets type="gateway"; then "b_plain" has
  // no daemon_type key so type stays "gateway".
  s.daemons["a_gw"].metadata["daemon_type"] = "gateway";
  s.daemons["b_plain"].gid = 2; // no daemon_type
  // 2 gateways active  (last-seen daemon_type was "gateway")
  EXPECT_EQ(s.get_summary(), "2 gateways active");
}

// map keys are ordered alphabetically, so "host" always appears before "zone" in the output
TEST(ServiceMapService, GetSummaryGroupingOrder)
{
  ServiceMap::Service s;
  s.daemons["d0"].metadata["hostname"] = "h1";
  s.daemons["d0"].metadata["zone_id"] = "z1";
  s.daemons["d1"].metadata["hostname"] = "h2";
  s.daemons["d1"].metadata["zone_id"] = "z2";
  const std::string summary = s.get_summary();
  // exact parenthetical: "(2 hosts, 2 zones)" — "host" < "zone" in map order
  EXPECT_EQ(summary, "2 daemons active (2 hosts, 2 zones)");
}

TEST(ServiceMapService, GetTaskSummarySingleTask)
{
  ServiceMap::Service s;
  s.daemons["worker0"].task_status["scrub"] = "running";
  const std::string expected =
      "\n"
      "    scrub:\n"
      "        mypool.worker0: running";
  EXPECT_EQ(s.get_task_summary("mypool"), expected);
}

TEST(ServiceMapService, CountMetadataNoDeamons)
{
  ServiceMap::Service s;
  std::map<std::string, int> out;
  s.count_metadata("zone", &out);
  EXPECT_TRUE(out.empty());
}

TEST(ServiceMapMap, DumpWithService)
{
  ServiceMap sm;
  sm.epoch = 1;
  sm.modified = utime_t(0, 0);
  sm.services["nfs"].summary = "ok";

  JSONFormatter f;
  f.dump_object("servicemap", sm);
  std::ostringstream oss;
  f.flush(oss);
  const std::string json_str = oss.str();

  EXPECT_TRUE(json_str.find("\"epoch\":1") != std::string::npos);
  EXPECT_TRUE(json_str.find("\"nfs\"") != std::string::npos);
  EXPECT_TRUE(json_str.find("\"summary\":\"ok\"") != std::string::npos);
}

// Build a minimal v1-format bufferlist by hand: encode only the fields that
// existed in v1 (gid, addr, start_epoch, start_stamp, metadata), wrapped in
// a version header whose compat=1, head=1.  task_status must remain empty
// because the v1 encoder never wrote it.
TEST(ServiceMapDaemon, DecodeV1StreamHasEmptyTaskStatus)
{
  bufferlist bl;
  {
    using ceph::encode;
    ENCODE_START(1, 1, bl);
    uint64_t gid = 55;
    encode(gid, bl);
    entity_addr_t addr;
    encode(addr, bl, 0);
    epoch_t start_epoch = 3;
    encode(start_epoch, bl);
    utime_t start_stamp;
    encode(start_stamp, bl);
    std::map<std::string, std::string> metadata;
    metadata["key"] = "val";
    encode(metadata, bl);
    // deliberately omit task_status — this is what struct_v=1 looks like
    ENCODE_FINISH(bl);
  }

  auto p = bl.cbegin();
  ServiceMap::Daemon d;
  d.decode(p);

  EXPECT_EQ(d.gid, 55u);
  EXPECT_EQ(d.metadata.at("key"), "val");
  // task_status must be untouched (empty) because the stream was v1
  EXPECT_TRUE(d.task_status.empty());
}

// Daemons that have no task_status must not appear in the output.
TEST(ServiceMapService, GetTaskSummaryMixedDaemons)
{
  ServiceMap::Service s;
  s.daemons["active"].task_status["scrub"] = "running";
  s.daemons["idle"].gid = 99; // no task_status

  const std::string summary = s.get_task_summary("pool");

  // The active daemon must appear
  EXPECT_TRUE(summary.find("pool.active") != std::string::npos);
  // The idle daemon (no task_status) must NOT appear
  EXPECT_TRUE(summary.find("pool.idle") == std::string::npos);
  // The task name itself must appear
  EXPECT_TRUE(summary.find("scrub:") != std::string::npos);
}

TEST(ServiceMapService, HasRunningTasksOnlyOneDaemonActive)
{
  ServiceMap::Service s;
  s.daemons["d_idle"].gid = 1;            // empty task_status
  s.daemons["d_busy"].task_status["scrub"] = "running";

  EXPECT_TRUE(s.has_running_tasks());
}

TEST(ServiceMapMap, IsNormalCephEntityPrefixNotEqual)
{
  EXPECT_FALSE(ServiceMap::is_normal_ceph_entity("osdd"));
  EXPECT_FALSE(ServiceMap::is_normal_ceph_entity("monitor"));
  EXPECT_FALSE(ServiceMap::is_normal_ceph_entity("mdss"));
  EXPECT_FALSE(ServiceMap::is_normal_ceph_entity("clients"));
  EXPECT_FALSE(ServiceMap::is_normal_ceph_entity("mgr2"));
}

// Insert in reverse alphabetical order to confirm std::map re-orders them
TEST(ServiceMapService, GetTaskSummaryDaemonOrdering)
{
  ServiceMap::Service s;
  s.daemons["z_worker"].task_status["scrub"] = "running";
  s.daemons["a_worker"].task_status["scrub"] = "idle";

  const std::string summary = s.get_task_summary("pool");

  // a_worker must appear before z_worker in the string
  const auto pos_a = summary.find("pool.a_worker");
  const auto pos_z = summary.find("pool.z_worker");
  ASSERT_NE(pos_a, std::string::npos);
  ASSERT_NE(pos_z, std::string::npos);
  EXPECT_LT(pos_a, pos_z);
}

TEST(ServiceMapMap, RmDaemonDoubleRemove)
{
  ServiceMap sm;
  sm.get_daemon("rgw", "inst0");

  // First remove succeeds and erases the service
  EXPECT_TRUE(sm.rm_daemon("rgw", "inst0"));
  EXPECT_EQ(sm.services.count("rgw"), 0u);

  // Second remove: service no longer exists → must return false
  EXPECT_FALSE(sm.rm_daemon("rgw", "inst0"));
  EXPECT_EQ(sm.services.count("rgw"), 0u);
}

// Only the "summary" key should appear inside the "daemons" section; no daemon entries.
TEST(ServiceMapService, DumpEmptyDaemons)
{
  ServiceMap::Service s;
  s.summary = "offline";

  JSONFormatter f;
  f.dump_object("service", s);
  std::ostringstream oss;
  f.flush(oss);
  const std::string json_str = oss.str();

  // The outer key must be "daemons" per Service::dump
  EXPECT_TRUE(json_str.find("\"daemons\"") != std::string::npos);
  // The summary must appear
  EXPECT_TRUE(json_str.find("\"summary\":\"offline\"") != std::string::npos);
  // There should be no "gid" key — no daemon objects were added
  EXPECT_TRUE(json_str.find("\"gid\"") == std::string::npos);
}

// The implementation formats every entry regardless of the status content;
// an empty string status should appear verbatim in the output.
TEST(ServiceMapService, GetTaskSummaryEmptyStatusValue)
{
  ServiceMap::Service s;
  s.daemons["d0"].task_status["scrub"] = "";

  const std::string summary = s.get_task_summary("pool");

  // The task name must appear
  EXPECT_TRUE(summary.find("scrub:") != std::string::npos);
  // The daemon identifier must appear
  EXPECT_TRUE(summary.find("pool.d0") != std::string::npos);
  // The status is the empty string — the colon-space separator must still be
  // present, giving "pool.d0: " at the end of that line
  EXPECT_TRUE(summary.find("pool.d0: ") != std::string::npos);
}

TEST(ServiceMapMap, DumpMultipleServices)
{
  ServiceMap sm;
  sm.epoch = 10;
  sm.services["rgw"].summary = "rgw-ok";
  sm.services["iscsi"].summary = "iscsi-ok";

  JSONFormatter f;
  f.dump_object("servicemap", sm);
  std::ostringstream oss;
  f.flush(oss);
  const std::string json_str = oss.str();

  EXPECT_TRUE(json_str.find("\"epoch\":10") != std::string::npos);
  EXPECT_TRUE(json_str.find("\"rgw\"") != std::string::npos);
  EXPECT_TRUE(json_str.find("\"iscsi\"") != std::string::npos);
  EXPECT_TRUE(json_str.find("\"summary\":\"rgw-ok\"") != std::string::npos);
  EXPECT_TRUE(json_str.find("\"summary\":\"iscsi-ok\"") != std::string::npos);
}

TEST(ServiceMapMap, EncodeDecodeMultipleServicesWithModified)
{
  ServiceMap m_1;
  m_1.epoch = 42;
  m_1.modified = utime_t(9999, 123456);
  m_1.services["rgw"].daemons["gw0"].gid = 7;
  m_1.services["rgw"].summary = "2 gateways active";
  m_1.services["iscsi"].daemons["portal0"].gid = 8;

  bufferlist bl;
  m_1.encode(bl, 0);
  auto p = bl.cbegin();

  ServiceMap m_2;
  m_2.decode(p);

  EXPECT_EQ(m_2.epoch, 42u);
  EXPECT_EQ(m_2.modified, utime_t(9999, 123456));
  EXPECT_EQ(m_2.services.count("rgw"), 1u);
  EXPECT_EQ(m_2.services.count("iscsi"), 1u);
  EXPECT_EQ(m_2.services["rgw"].daemons["gw0"].gid, 7u);
  EXPECT_EQ(m_2.services["rgw"].summary, "2 gateways active");
  EXPECT_EQ(m_2.services["iscsi"].daemons["portal0"].gid, 8u);
}

TEST(ServiceMapMap, EncodeDecodeMultipleServices)
{
  ServiceMap m_1;
  m_1.epoch = 5;
  m_1.services["nfs"].daemons["server0"].gid = 100;
  m_1.services["nfs"].daemons["server1"].gid = 101;
  m_1.services["smb"].daemons["share0"].gid = 200;

  bufferlist bl;
  m_1.encode(bl, 0);
  auto p = bl.cbegin();

  ServiceMap m_2;
  m_2.decode(p);

  EXPECT_EQ(m_2.epoch, 5u);
  EXPECT_EQ(m_2.services.size(), 2u);
  EXPECT_EQ(m_2.services["nfs"].daemons.size(), 2u);
  EXPECT_EQ(m_2.services["nfs"].daemons["server0"].gid, 100u);
  EXPECT_EQ(m_2.services["nfs"].daemons["server1"].gid, 101u);
  EXPECT_EQ(m_2.services["smb"].daemons["share0"].gid, 200u);
}

// count_metadata uses (*out)[key]++ so existing counts are preserved and
// incremented, not overwritten.
TEST(ServiceMapService, CountMetadataAccumulates)
{
  ServiceMap::Service s;
  s.daemons["d0"].metadata["zone"] = "us-east";
  s.daemons["d1"].metadata["zone"] = "us-east";

  std::map<std::string, int> out;
  out["us-east"] = 10;  // pre-existing count

  s.count_metadata("zone", &out);

  // 10 pre-existing + 2 from daemons = 12
  EXPECT_EQ(out["us-east"], 12);
}

// A subsequent get_daemon call after rm_daemon erased the service must
// recreate the entry with added=true and a fresh default-initialised Daemon.
TEST(ServiceMapMap, GetDaemonAfterRmDaemon)
{
  ServiceMap sm;

  // Create the initial daemon, set a field to distinguish it
  auto [d0, added0] = sm.get_daemon("rgw", "inst0");
  EXPECT_TRUE(added0);
  d0->gid = 99;

  // Remove it — service should be erased
  EXPECT_TRUE(sm.rm_daemon("rgw", "inst0"));
  EXPECT_EQ(sm.services.count("rgw"), 0u);

  // Re-add with the same names — must be treated as new (added=true)
  auto [d1, added1] = sm.get_daemon("rgw", "inst0");
  EXPECT_TRUE(added1);
  ASSERT_NE(d1, nullptr);
  // Fresh daemon — gid must be the default zero, not the old value
  EXPECT_EQ(d1->gid, 0u);
  EXPECT_EQ(sm.services.count("rgw"), 1u);
}

// task_status is encoded by Daemon::encode (not Service::encode), but this test
// exercises the full round-trip through the Service codec boundary.
TEST(ServiceMapService, EncodeDecodePreservesDaemonTaskStatus)
{
  ServiceMap::Service s_1;
  s_1.summary = "active";
  s_1.daemons["gw0"].gid = 5;
  s_1.daemons["gw0"].task_status["scrub"] = "running";
  s_1.daemons["gw0"].task_status["rebalance"] = "idle";

  bufferlist bl;
  s_1.encode(bl, 0);
  auto p = bl.cbegin();

  ServiceMap::Service s_2;
  s_2.decode(p);

  EXPECT_EQ(s_2.summary, "active");
  EXPECT_EQ(s_2.daemons["gw0"].gid, 5u);
  EXPECT_EQ(s_2.daemons["gw0"].task_status.at("scrub"), "running");
  EXPECT_EQ(s_2.daemons["gw0"].task_status.at("rebalance"), "idle");
}

// The set-size check `(i->second.size() ? "s" : "")` always appends "s" for
// any non-empty set, so the output must be "1 hosts, 1 zones".
TEST(ServiceMapService, GetSummaryOneHostOneZone)
{
  ServiceMap::Service s;
  s.daemons["d0"].metadata["hostname"] = "node-1";
  s.daemons["d0"].metadata["zone_id"] = "us-east";
  s.daemons["d1"].metadata["hostname"] = "node-1"; // same host
  s.daemons["d1"].metadata["zone_id"] = "us-east"; // same zone

  const std::string summary = s.get_summary();
  // 2 daemons, 1 unique host, 1 unique zone
  EXPECT_TRUE(summary.find("2 daemons active") != std::string::npos);
  EXPECT_TRUE(summary.find("1 hosts") != std::string::npos);
  EXPECT_TRUE(summary.find("1 zones") != std::string::npos);
}

// With type="" and num=2: "2 s active"; with num=1: "1  active" (empty suffix, double space)
TEST(ServiceMapService, GetSummaryEmptyDaemonType)
{
  ServiceMap::Service s;
  s.daemons["d0"].metadata["daemon_type"] = "";
  s.daemons["d1"].metadata["daemon_type"] = "";

  const std::string summary = s.get_summary();
  EXPECT_EQ(summary, "2 s active");
}

TEST(ServiceMapService, GetSummaryEmptyDaemonTypeSingular)
{
  ServiceMap::Service s;
  s.daemons["d0"].metadata["daemon_type"] = "";

  const std::string summary = s.get_summary();
  // type="" so num=1 → "1  active" (no trailing "s", double space)
  EXPECT_EQ(summary, "1  active");
}

TEST(ServiceMapMap, IsNormalCephEntityLeadingWhitespace)
{
  EXPECT_FALSE(ServiceMap::is_normal_ceph_entity(" osd"));
  EXPECT_FALSE(ServiceMap::is_normal_ceph_entity(" client"));
  EXPECT_FALSE(ServiceMap::is_normal_ceph_entity(" mon"));
}

TEST(ServiceMapMap, IsNormalCephEntityTrailingWhitespace)
{
  EXPECT_FALSE(ServiceMap::is_normal_ceph_entity("osd "));
  EXPECT_FALSE(ServiceMap::is_normal_ceph_entity("mds "));
  EXPECT_FALSE(ServiceMap::is_normal_ceph_entity("mgr "));
}

// A default entity_addr_t has TYPE_NONE / AF_UNSPEC; get_legacy_str() calls
// get_sockaddr() which hits the AF_UNSPEC default branch and produces
// "(unrecognized address family 0)/0".
TEST(ServiceMapDaemon, DumpDefaultAddrKey)
{
  ServiceMap::Daemon d;
  d.gid = 1;
  // addr is default-constructed (TYPE_NONE, AF_UNSPEC)

  JSONFormatter f;
  f.dump_object("daemon", d);
  std::ostringstream oss;
  f.flush(oss);
  const std::string json_str = oss.str();

  // The "addr" key must be present in the JSON output
  EXPECT_TRUE(json_str.find("\"addr\"") != std::string::npos);
  // The default entity_addr_t serialises via the AF_UNSPEC fallback path
  EXPECT_TRUE(json_str.find("unrecognized address family") != std::string::npos);
}

// generate_test_instances does not set modified, so both instances have the default utime_t(0,0)
TEST(ServiceMapMap, GenerateTestInstancesModifiedField)
{
  auto instances = ServiceMap::generate_test_instances();
  ASSERT_EQ(instances.size(), 2u);
  auto it = instances.begin();
  EXPECT_EQ(it->epoch, 0u);
  EXPECT_EQ(it->modified, utime_t(0, 0));
  ++it;
  EXPECT_EQ(it->epoch, 123u);
  EXPECT_EQ(it->modified, utime_t(0, 0));
}

// generate_test_instances does not set summary on either instance
TEST(ServiceMapService, GenerateTestInstancesSummaryField)
{
  auto instances = ServiceMap::Service::generate_test_instances();
  ASSERT_EQ(instances.size(), 2u);
  auto it = instances.begin();
  EXPECT_EQ(it->summary, "");
  EXPECT_TRUE(it->daemons.empty());
  ++it;
  EXPECT_EQ(it->summary, "");
  EXPECT_EQ(it->daemons.size(), 2u);
}

// groupings["host"] collects values only from daemons that have the key;
// the daemon without it does not contribute to the set.
TEST(ServiceMapService, GetSummaryPartialHostname)
{
  ServiceMap::Service s;
  s.daemons["d0"].metadata["hostname"] = "node-1";
  s.daemons["d1"].gid = 2; // no hostname key

  const std::string summary = s.get_summary();
  // 2 daemons, but only 1 contributed a hostname value
  EXPECT_TRUE(summary.find("2 daemons active") != std::string::npos);
  EXPECT_TRUE(summary.find("1 hosts") != std::string::npos);
}

TEST(ServiceMapMap, IsNormalCephEntityUppercase)
{
  EXPECT_FALSE(ServiceMap::is_normal_ceph_entity("OSD"));
  EXPECT_FALSE(ServiceMap::is_normal_ceph_entity("MON"));
  EXPECT_FALSE(ServiceMap::is_normal_ceph_entity("MDS"));
  EXPECT_FALSE(ServiceMap::is_normal_ceph_entity("MGR"));
  EXPECT_FALSE(ServiceMap::is_normal_ceph_entity("CLIENT"));
}

// generate_test_instances does not set summary on service entries
TEST(ServiceMapMap, GenerateTestInstancesServiceSummary)
{
  auto instances = ServiceMap::generate_test_instances();
  ASSERT_EQ(instances.size(), 2u);
  auto it = instances.begin();
  ++it;
  EXPECT_EQ(it->services.at("rgw").summary, "");
  EXPECT_EQ(it->services.at("iscsi").summary, "");
}

// Three daemons present, none with any task_status entries — distinct from the
// false-case test that uses a service with no daemons at all.
TEST(ServiceMapService, HasRunningTasksAllDaemonsEmptyTaskStatus)
{
  ServiceMap::Service s;
  s.daemons["d0"].gid = 1;  // task_status is default-empty
  s.daemons["d1"].gid = 2;  // task_status is default-empty
  s.daemons["d2"].gid = 3;  // task_status is default-empty

  EXPECT_FALSE(s.has_running_tasks());
}

TEST(ServiceMapService, GetTaskSummarySingleDaemonTwoTasks)
{
  ServiceMap::Service s;
  s.daemons["worker0"].task_status["scrub"] = "running";
  s.daemons["worker0"].task_status["rebalance"] = "idle";

  const std::string summary = s.get_task_summary("pool");

  // Both task sections must appear
  EXPECT_TRUE(summary.find("scrub:") != std::string::npos);
  EXPECT_TRUE(summary.find("rebalance:") != std::string::npos);
  // The daemon must appear under each task
  EXPECT_TRUE(summary.find("pool.worker0") != std::string::npos);
  // Both status values must be present
  EXPECT_TRUE(summary.find("running") != std::string::npos);
  EXPECT_TRUE(summary.find("idle") != std::string::npos);
}

TEST(ServiceMapMap, RmDaemonSequentialRemovalEmptiesService)
{
  ServiceMap sm;
  sm.get_daemon("nfs", "server0");
  sm.get_daemon("nfs", "server1");
  EXPECT_EQ(sm.services["nfs"].daemons.size(), 2u);

  // Remove first daemon — service must survive because server1 remains
  EXPECT_TRUE(sm.rm_daemon("nfs", "server0"));
  EXPECT_EQ(sm.services.count("nfs"), 1u);
  EXPECT_EQ(sm.services["nfs"].daemons.count("server0"), 0u);
  EXPECT_EQ(sm.services["nfs"].daemons.count("server1"), 1u);

  // Remove second (last) daemon — service must be erased
  EXPECT_TRUE(sm.rm_daemon("nfs", "server1"));
  EXPECT_EQ(sm.services.count("nfs"), 0u);
}

TEST(ServiceMapMap, GetDaemonMutationDoesNotAffectSibling)
{
  ServiceMap sm;

  auto [d0, added0] = sm.get_daemon("rgw", "inst0");
  auto [d1, added1] = sm.get_daemon("rgw", "inst1");
  EXPECT_TRUE(added0);
  EXPECT_TRUE(added1);

  // Mutate inst0 via its pointer
  d0->gid = 100;
  d0->metadata["zone"] = "us-east";

  // inst1 must be unaffected — default-constructed values
  EXPECT_EQ(d1->gid, 0u);
  EXPECT_TRUE(d1->metadata.empty());

  // Verify via map access as well
  EXPECT_EQ(sm.services["rgw"].daemons["inst0"].gid, 100u);
  EXPECT_EQ(sm.services["rgw"].daemons["inst0"].metadata.at("zone"), "us-east");
  EXPECT_EQ(sm.services["rgw"].daemons["inst1"].gid, 0u);
  EXPECT_TRUE(sm.services["rgw"].daemons["inst1"].metadata.empty());
}

// When CEPH_FEATURE_MSG_ADDR2 is set, entity_addr_t is encoded with the v2 wire
// format; the decode side auto-detects the format from the buffer.
TEST(ServiceMapDaemon, EncodeDecodeWithAddr2Features)
{
  ServiceMap::Daemon d_1;
  d_1.gid = 42;
  d_1.start_epoch = 7;
  d_1.metadata["host"] = "node-x";
  entity_addr_t addr;
  addr.parse("192.168.1.1:6789", nullptr);
  d_1.addr = addr;

  bufferlist bl;
  // Encode using the MSG_ADDR2 feature flag — triggers a different addr wire format
  d_1.encode(bl, CEPH_FEATURE_MSG_ADDR2);
  auto p = bl.cbegin();

  ServiceMap::Daemon d_2;
  d_2.decode(p);

  EXPECT_EQ(d_2.gid, 42u);
  EXPECT_EQ(d_2.start_epoch, 7u);
  EXPECT_EQ(d_2.metadata.at("host"), "node-x");
  // The decoded address must round-trip to the same legacy string representation
  EXPECT_EQ(d_2.addr.get_legacy_str(), d_1.addr.get_legacy_str());
}

TEST(ServiceMapDaemon, EncodeDecodeMaxValues)
{
  ServiceMap::Daemon d_1;
  d_1.gid = std::numeric_limits<uint64_t>::max();
  d_1.start_epoch = std::numeric_limits<epoch_t>::max();
  d_1.start_stamp = utime_t(std::numeric_limits<time_t>::max(), 999999);
  entity_addr_t addr;
  addr.parse("192.168.1.100:6800", nullptr);
  d_1.addr = addr;
  d_1.metadata["key"] = "val";
  d_1.task_status["task"] = "status";

  bufferlist bl;
  d_1.encode(bl, 0);
  auto p = bl.cbegin();

  ServiceMap::Daemon d_2;
  d_2.decode(p);

  EXPECT_EQ(d_2.gid, std::numeric_limits<uint64_t>::max());
  EXPECT_EQ(d_2.start_epoch, std::numeric_limits<epoch_t>::max());
  EXPECT_EQ(d_2.start_stamp, utime_t(std::numeric_limits<time_t>::max(), 999999));
  EXPECT_EQ(d_2.addr.get_legacy_str(), addr.get_legacy_str());
  EXPECT_EQ(d_2.metadata.at("key"), "val");
  EXPECT_EQ(d_2.task_status.at("task"), "status");
}

TEST(ServiceMapDaemon, DumpMetadataAndTaskStatus)
{
  ServiceMap::Daemon d;
  d.gid = 88;
  d.start_epoch = 12;
  d.start_stamp = utime_t(100, 200000000);
  entity_addr_t addr;
  addr.parse("10.0.0.2:7000", nullptr);
  d.addr = addr;
  d.metadata["arch"] = "x86_64";
  d.metadata["distro"] = "centos";
  d.task_status["sync"] = "active";
  d.task_status["gc"] = "running";

  JSONFormatter f;
  f.dump_object("daemon", d);
  std::ostringstream oss;
  f.flush(oss);
  const std::string json_str = oss.str();

  EXPECT_TRUE(json_str.find("\"gid\":88") != std::string::npos);
  EXPECT_TRUE(json_str.find("\"start_epoch\":12") != std::string::npos);
  EXPECT_TRUE(json_str.find("\"start_stamp\":\"100.200000\"") != std::string::npos);
  EXPECT_TRUE(json_str.find("\"arch\":\"x86_64\"") != std::string::npos);
  EXPECT_TRUE(json_str.find("\"distro\":\"centos\"") != std::string::npos);
  EXPECT_TRUE(json_str.find("\"sync\":\"active\"") != std::string::npos);
  EXPECT_TRUE(json_str.find("\"gc\":\"running\"") != std::string::npos);
}

TEST(ServiceMapService, DumpMultipleDaemons)
{
  ServiceMap::Service s;
  s.summary = "running";
  s.daemons["d1"].gid = 101;
  s.daemons["d2"].gid = 102;

  JSONFormatter f;
  f.dump_object("service", s);
  std::ostringstream oss;
  f.flush(oss);
  const std::string json_str = oss.str();

  EXPECT_TRUE(json_str.find("\"summary\":\"running\"") != std::string::npos);
  EXPECT_TRUE(json_str.find("\"d1\"") != std::string::npos);
  EXPECT_TRUE(json_str.find("\"d2\"") != std::string::npos);
  EXPECT_TRUE(json_str.find("\"gid\":101") != std::string::npos);
  EXPECT_TRUE(json_str.find("\"gid\":102") != std::string::npos);
}

TEST(ServiceMapService, GetTaskSummaryEmptyPrefix)
{
  ServiceMap::Service s;
  s.daemons["worker"].task_status["sync"] = "idle";

  const std::string summary = s.get_task_summary("");
  const std::string expected =
      "\n"
      "    sync:\n"
      "        .worker: idle";
  EXPECT_EQ(summary, expected);
}

TEST(ServiceMapService, GetSummaryMultipleHostsSameZone)
{
  ServiceMap::Service s;
  s.daemons["d0"].metadata["hostname"] = "host1";
  s.daemons["d0"].metadata["zone_id"] = "zoneA";
  s.daemons["d1"].metadata["hostname"] = "host2";
  s.daemons["d1"].metadata["zone_id"] = "zoneA";
  s.daemons["d2"].metadata["hostname"] = "host3";
  s.daemons["d2"].metadata["zone_id"] = "zoneA";

  const std::string summary = s.get_summary();
  EXPECT_TRUE(summary.find("3 daemons active") != std::string::npos);
  EXPECT_TRUE(summary.find("3 hosts") != std::string::npos);
  EXPECT_TRUE(summary.find("1 zone") != std::string::npos);
}

TEST(ServiceMapService, CountMetadataSpecialCharacters)
{
  ServiceMap::Service s;
  s.daemons["d0"].metadata["my:field/name.1"] = "val@123!";
  s.daemons["d1"].metadata["my:field/name.1"] = "val@123!";
  s.daemons["d2"].metadata["my:field/name.1"] = "other#value$";

  std::map<std::string, int> out;
  s.count_metadata("my:field/name.1", &out);

  EXPECT_EQ(out["val@123!"], 2);
  EXPECT_EQ(out["other#value$"], 1);
}

TEST(ServiceMapMap, IsNormalCephEntitySubstrings)
{
  EXPECT_FALSE(ServiceMap::is_normal_ceph_entity("os"));
  EXPECT_FALSE(ServiceMap::is_normal_ceph_entity("mo"));
  EXPECT_FALSE(ServiceMap::is_normal_ceph_entity("md"));
  EXPECT_FALSE(ServiceMap::is_normal_ceph_entity("mg"));
  EXPECT_FALSE(ServiceMap::is_normal_ceph_entity("clien"));
  EXPECT_FALSE(ServiceMap::is_normal_ceph_entity("Osd"));
  EXPECT_FALSE(ServiceMap::is_normal_ceph_entity("Mon"));
  EXPECT_FALSE(ServiceMap::is_normal_ceph_entity("Mds"));
  EXPECT_FALSE(ServiceMap::is_normal_ceph_entity("Mgr"));
  EXPECT_FALSE(ServiceMap::is_normal_ceph_entity("Client"));
}

TEST(ServiceMapMap, RmDaemonMultipleServicesDrainToEmpty)
{
  ServiceMap sm;
  sm.get_daemon("rgw", "gw1");
  sm.get_daemon("rgw", "gw2");
  sm.get_daemon("iscsi", "p1");
  sm.get_daemon("mds_legacy", "m1");

  EXPECT_EQ(sm.services.size(), 3u);

  // Remove non-existent daemon on existing service
  EXPECT_FALSE(sm.rm_daemon("rgw", "nonexistent"));
  EXPECT_EQ(sm.services["rgw"].daemons.size(), 2u);

  // Remove gw1 from rgw
  EXPECT_TRUE(sm.rm_daemon("rgw", "gw1"));
  EXPECT_EQ(sm.services.count("rgw"), 1u);
  EXPECT_EQ(sm.services["rgw"].daemons.size(), 1u);

  // Remove gw2 from rgw -> rgw erased
  EXPECT_TRUE(sm.rm_daemon("rgw", "gw2"));
  EXPECT_EQ(sm.services.count("rgw"), 0u);
  EXPECT_EQ(sm.services.size(), 2u);

  // Remove p1 from iscsi -> iscsi erased
  EXPECT_TRUE(sm.rm_daemon("iscsi", "p1"));
  EXPECT_EQ(sm.services.count("iscsi"), 0u);
  EXPECT_EQ(sm.services.size(), 1u);

  // Remove m1 from mds_legacy -> mds_legacy erased
  EXPECT_TRUE(sm.rm_daemon("mds_legacy", "m1"));
  EXPECT_EQ(sm.services.count("mds_legacy"), 0u);
  EXPECT_TRUE(sm.services.empty());
}

TEST(ServiceMapMap, EncodeDecodeMaxValues)
{
  ServiceMap m_1;
  m_1.epoch = std::numeric_limits<epoch_t>::max();
  m_1.modified = utime_t(std::numeric_limits<time_t>::max(), 999999);
  m_1.services["svc"].summary = "max-test";
  m_1.services["svc"].daemons["d"].gid = std::numeric_limits<uint64_t>::max();

  bufferlist bl;
  m_1.encode(bl, 0);
  auto p = bl.cbegin();

  ServiceMap m_2;
  m_2.decode(p);

  EXPECT_EQ(m_2.epoch, std::numeric_limits<epoch_t>::max());
  EXPECT_EQ(m_2.modified, utime_t(std::numeric_limits<time_t>::max(), 999999));
  EXPECT_EQ(m_2.services.count("svc"), 1u);
  EXPECT_EQ(m_2.services["svc"].summary, "max-test");
  EXPECT_EQ(m_2.services["svc"].daemons["d"].gid, std::numeric_limits<uint64_t>::max());
}

TEST(ServiceMapMap, StateTransitionsWithModifiedAndEpoch)
{
  ServiceMap sm;
  EXPECT_EQ(sm.epoch, 0u);
  EXPECT_EQ(sm.modified, utime_t(0, 0));
  EXPECT_TRUE(sm.services.empty());

  sm.epoch = 1;
  sm.modified = utime_t(100, 0);
  auto [d0, added0] = sm.get_daemon("rgw", "gw0");
  EXPECT_TRUE(added0);
  d0->gid = 1;

  sm.epoch = 2;
  sm.modified = utime_t(200, 0);
  auto [d1, added1] = sm.get_daemon("rgw", "gw1");
  EXPECT_TRUE(added1);
  d1->gid = 2;

  EXPECT_EQ(sm.epoch, 2u);
  EXPECT_EQ(sm.modified, utime_t(200, 0));
  EXPECT_EQ(sm.services["rgw"].daemons.size(), 2u);

  sm.epoch = 3;
  sm.modified = utime_t(300, 0);
  EXPECT_TRUE(sm.rm_daemon("rgw", "gw0"));

  EXPECT_EQ(sm.epoch, 3u);
  EXPECT_EQ(sm.modified, utime_t(300, 0));
  EXPECT_EQ(sm.services["rgw"].daemons.size(), 1u);
  EXPECT_EQ(sm.services["rgw"].daemons.count("gw1"), 1u);
}
