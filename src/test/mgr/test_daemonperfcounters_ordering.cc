// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <algorithm>
#include <atomic>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "global/global_context.h"
#include "global/global_init.h"
#include "gtest/gtest.h"

#include "messages/MMgrReport.h"
#include "mgr/DaemonPerfCounters.h"
#include "mgr/MgrSession.h"
#include "mgr/PerfCounterInstance.h"
#include "msg/Connection.h"

namespace {

constexpr perfcounter_type_d kScalar = PERFCOUNTER_U64;
constexpr perfcounter_type_d kAvg =
  static_cast<perfcounter_type_d>(PERFCOUNTER_U64 | PERFCOUNTER_LONGRUNAVG);

bool is_avg(perfcounter_type_d t)
{
  return t & PERFCOUNTER_LONGRUNAVG;
}

const char* kind_name(perfcounter_type_d t)
{
  return is_avg(t) ? "avg" : "scalar";
}

class StubConnection : public Connection {
public:
  StubConnection() : Connection(g_ceph_context, nullptr) {}
  bool is_connected() override { return true; }
  void send_keepalive() override {}
  void mark_down() override {}
  void mark_disposable() override {}
  void shutdown() override {}
  entity_addr_t get_peer_socket_addr() const override
  {
    return entity_addr_t();
  }

protected:
  int send_msg(MessageRef&&) override { return 0; }
};

// Models one daemon-side MgrClient session: what it has declared and the
// report it would encode, in the layout MgrClient::_send_report produces.
// The i-th declared path (sorted) carries value val+i and count cnt+i.
struct Sess {
  ceph::ref_t<MgrSession> session;
  ceph::ref_t<StubConnection> con;
  std::map<std::string, perfcounter_type_d> declared;
  std::vector<PerfCounterType> pending_declare;
  std::vector<std::string> pending_undeclare;

  Sess()
    : session(ceph::make_ref<MgrSession>(g_ceph_context)),
      con(ceph::make_ref<StubConnection>())
  {
    con->set_priv(session);
  }

  void declare(const std::string& path, perfcounter_type_d kind)
  {
    PerfCounterType t;
    t.path = path;
    t.type = kind;
    pending_declare.push_back(t);
    declared[path] = kind;
  }

  void undeclare(const std::string& path)
  {
    pending_undeclare.push_back(path);
    declared.erase(path);
  }

  ceph::buffer::list pack(uint64_t val, uint64_t cnt) const
  {
    ceph::buffer::list bl;
    ENCODE_START(1, 1, bl);
    uint64_t i = 0;
    for (const auto& [path, kind] : declared) {
      encode(val + i, bl);
      if (is_avg(kind)) {
	encode(cnt + i, bl);
	encode(static_cast<uint64_t>(7777), bl);
      }
      ++i;
    }
    ENCODE_FINISH(bl);
    return bl;
  }

  ceph::ref_t<MMgrReport> wrap(const ceph::buffer::list& packed)
  {
    auto m = ceph::make_message<MMgrReport>();
    m->declare_types = std::move(pending_declare);
    m->undeclare_types = std::move(pending_undeclare);
    pending_declare.clear();
    pending_undeclare.clear();
    m->packed = packed;
    m->set_connection(con);
    return m;
  }

  ceph::ref_t<MMgrReport> report(uint64_t val, uint64_t cnt = 0)
  {
    return wrap(pack(val, cnt));
  }
};

ceph::buffer::list frame(const std::vector<uint64_t>& words)
{
  ceph::buffer::list bl;
  ENCODE_START(1, 1, bl);
  for (auto w : words) {
    encode(w, bl);
  }
  ENCODE_FINISH(bl);
  return bl;
}

std::string describe(const DaemonPerfCounters& d)
{
  std::ostringstream o;
  o << "instances{";
  for (const auto& [path, inst] : d.instances) {
    o << " " << path << "="
      << (inst.get_data_avg().capacity() ? "avg(" : "scalar(");
    for (const auto& dp : inst.get_data()) {
      o << dp.v << ",";
    }
    for (const auto& dp : inst.get_data_avg()) {
      o << dp.s << "/" << dp.c << ",";
    }
    o << ")";
  }
  o << " }";
  return o.str();
}

std::string describe(const PerfCounterTypes& types)
{
  std::ostringstream o;
  o << "types{";
  for (const auto& [path, t] : types) {
    o << " " << path << "=" << kind_name(t.type);
  }
  o << " }";
  return o.str();
}

std::string describe(const std::set<std::string>& s)
{
  std::ostringstream o;
  o << "{";
  for (const auto& e : s) {
    o << " " << e;
  }
  o << " }";
  return o.str();
}

std::string ctx(const std::string& seq, const std::string& current,
		const std::string& expected)
{
  return "\n  Sequence: " + seq + "\n  Current: " + current +
         "\n  Expected: " + expected;
}

size_t points(const PerfCounterInstance& i)
{
  return i.get_data().size() + i.get_data_avg().size();
}

} // namespace

class PerfCountersOrdering : public ::testing::Test {
protected:
  PerfCounterTypes types;
  DaemonPerfCounters dpc{types};

  std::string state()
  {
    return describe(dpc) + " " + describe(types);
  }
};

class PerfCounterInstanceOrdering : public ::testing::Test {
protected:
  void SetUp() override
  {
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  }
};

TEST_F(PerfCountersOrdering, OrderingBug_P1_ScalarThenAvg_AvgCountKept)
{
  const std::string seq =
    "A.declare(X,scalar); A.report(5); B.declare(X,avg); B.report(val=7,count=3)";
  DaemonPerfCounters dpc_b(types);
  Sess a, b;
  a.declare("X", kScalar);
  ASSERT_NO_THROW(dpc.update(*a.report(5)))
    << ctx(seq, "update() threw", "update() accepts a well-formed report");
  b.declare("X", kAvg);
  ASSERT_NO_THROW(dpc_b.update(*b.report(7, 3)))
    << ctx(seq, "update threw " + describe(types),
	   "B's report decoded with the type B declared (avg)");
  auto it = dpc_b.instances.find("X");
  ASSERT_NE(it, dpc_b.instances.end())
    << ctx(seq, "daemon B has no instance for X",
	   "an avg instance for X holding (sum 7, count 3)");
  ASSERT_EQ(it->second.get_data_avg().size(), 1u)
    << ctx(seq, "B: " + describe(dpc_b) + " " + describe(types),
	   "B: avg(7/3,) - the avgcount B sent is kept");
  EXPECT_EQ(it->second.get_latest_data_avg().s, 7u)
    << ctx(seq, "B: " + describe(dpc_b), "sum 7");
  EXPECT_EQ(it->second.get_latest_data_avg().c, 3u)
    << ctx(seq, "B: " + describe(dpc_b), "count 3");
}

TEST_F(PerfCountersOrdering, OrderingBug_P1_AvgThenScalar_ReportDoesNotThrow)
{
  const std::string seq =
    "A.declare(X,avg); A.report(5,2); B.declare(X,scalar); B.report(val=7)";
  DaemonPerfCounters dpc_b(types);
  Sess a, b;
  a.declare("X", kAvg);
  ASSERT_NO_THROW(dpc.update(*a.report(5, 2)))
    << ctx(seq, "update() threw", "update() accepts a well-formed report");
  b.declare("X", kScalar);
  EXPECT_NO_THROW(dpc_b.update(*b.report(7)))
    << ctx(seq, "update threw: B's 8-byte value was decoded as avg " +
		describe(types),
	   "B's report decoded with the type B declared (scalar)");
}

TEST_F(PerfCountersOrdering, OrderingBug_P1_TwoPaths_NeighbourValueNotShifted)
{
  const std::string seq =
    "A.declare(X,scalar); A.report(5); B.declare(X,avg); B.declare(Y,scalar); "
    "B.report(val=10,count=100)";
  DaemonPerfCounters dpc_b(types);
  Sess a, b;
  a.declare("X", kScalar);
  ASSERT_NO_THROW(dpc.update(*a.report(5)))
    << ctx(seq, "update() threw", "update() accepts a well-formed report");
  b.declare("X", kAvg);
  b.declare("Y", kScalar);
  ASSERT_NO_THROW(dpc_b.update(*b.report(10, 100)))
    << ctx(seq, "update threw", "no throw");
  auto it = dpc_b.instances.find("Y");
  ASSERT_NE(it, dpc_b.instances.end())
    << ctx(seq, "no instance for Y", "scalar instance Y holding 11");
  ASSERT_EQ(it->second.get_data().size(), 1u)
    << ctx(seq, "B: " + describe(dpc_b), "Y has one point");
  EXPECT_EQ(it->second.get_latest_data().v, 11u)
    << ctx(seq, "B: " + describe(dpc_b) + " " + describe(types),
	   "Y=11, the value B sent for Y; the avgcount of X must not be "
	   "read as Y");
}

TEST_F(PerfCountersOrdering, OrderingBug_P1_SecondDaemonSameKind_Independent)
{
  const std::string seq =
    "A.declare(X,scalar); A.report(5); B.declare(X,scalar); B.report(9)";
  DaemonPerfCounters dpc_b(types);
  Sess a, b;
  a.declare("X", kScalar);
  b.declare("X", kScalar);
  dpc.update(*a.report(5));
  dpc_b.update(*b.report(9));
  ASSERT_EQ(dpc.instances.at("X").get_data().size(), 1u)
    << ctx(seq, "A: " + describe(dpc), "A has one point");
  ASSERT_EQ(dpc_b.instances.at("X").get_data().size(), 1u)
    << ctx(seq, "B: " + describe(dpc_b), "B has one point");
  EXPECT_EQ(dpc.instances.at("X").get_latest_data().v, 5u)
    << ctx(seq, "A: " + describe(dpc), "A holds only its own value 5");
  EXPECT_EQ(dpc_b.instances.at("X").get_latest_data().v, 9u)
    << ctx(seq, "B: " + describe(dpc_b), "B holds only its own value 9");
}

TEST_F(PerfCounterInstanceOrdering, OrderingBug_P1_PushAvgOnScalarInstance_NotSilentlyDropped)
{
  PerfCounterInstance inst(kScalar);
  bool threw = false;
  try {
    inst.push_avg(utime_t(1, 0), 7, 3);
  } catch (const std::exception&) {
    threw = true;
  }
  EXPECT_TRUE(threw || points(inst) == 1u)
    << ctx("PerfCounterInstance(scalar).push_avg(t,7,3)",
	   "no exception, points=" + std::to_string(points(inst)),
	   "the point is stored or the call is rejected");
}

TEST_F(PerfCounterInstanceOrdering, OrderingBug_P1_PushOnAvgInstance_NotSilentlyDropped)
{
  PerfCounterInstance inst(kAvg);
  bool threw = false;
  try {
    inst.push(utime_t(1, 0), 7);
  } catch (const std::exception&) {
    threw = true;
  }
  EXPECT_TRUE(threw || points(inst) == 1u)
    << ctx("PerfCounterInstance(avg).push(t,7)",
	   "no exception, points=" + std::to_string(points(inst)),
	   "the point is stored or the call is rejected");
}

TEST_F(PerfCountersOrdering, OrderingBug_P2_ShortWellFramedPacked_InstancesUnchanged)
{
  const std::string seq =
    "declare(X,scalar); declare(Y,scalar); report(packed = frame{5}) [needs 2 values]";
  Sess s;
  s.declare("X", kScalar);
  s.declare("Y", kScalar);
  auto m = s.wrap(frame({5}));
  EXPECT_THROW(dpc.update(*m), ceph::buffer::error)
    << ctx(seq, "no buffer::error", "buffer::error from the short decode");
  EXPECT_TRUE(dpc.instances.empty())
    << ctx(seq, state(),
	   "instances unchanged (empty): a failed report leaves no partial update");
}

TEST_F(PerfCountersOrdering, OrderingBug_P2_ShortWellFramedPacked_AfterGoodReport)
{
  const std::string seq =
    "declare(X,Y scalar); report(val=1); report(packed = frame{2}) [needs 2 values]";
  Sess s;
  s.declare("X", kScalar);
  s.declare("Y", kScalar);
  ASSERT_NO_THROW(dpc.update(*s.report(1)))
    << ctx(seq, "update() threw", "update() accepts a well-formed report");
  const std::string before = describe(dpc);
  auto m = s.wrap(frame({2}));
  EXPECT_THROW(dpc.update(*m), ceph::buffer::error)
    << ctx(seq, "no buffer::error was thrown", "buffer::error from the short decode");
  EXPECT_EQ(describe(dpc), before)
    << ctx(seq, describe(dpc), "unchanged: " + before);
}

TEST_F(PerfCountersOrdering, OrderingBug_P2_TruncatedFrame_NoPartialUpdate)
{
  const std::string seq =
    "declare(X,Y scalar); report(packed = valid packed minus last 8 bytes)";
  Sess s;
  s.declare("X", kScalar);
  s.declare("Y", kScalar);
  auto full = s.pack(1, 0);
  ceph::buffer::list cut;
  cut.substr_of(full, 0, full.length() - 8);
  auto m = s.wrap(cut);
  EXPECT_THROW(dpc.update(*m), ceph::buffer::error)
    << ctx(seq, "no buffer::error", "buffer::error");
  EXPECT_TRUE(dpc.instances.empty())
    << ctx(seq, state(), "instances unchanged (empty)");
}

TEST_F(PerfCountersOrdering, OrderingBug_P2_FailedReport_LeavesNoEmptyInstance)
{
  const std::string seq =
    "declare(X,Y scalar); report(packed = frame{5})";
  Sess s;
  s.declare("X", kScalar);
  s.declare("Y", kScalar);
  auto m = s.wrap(frame({5}));
  EXPECT_THROW(dpc.update(*m), ceph::buffer::error)
    << ctx(seq, "no buffer::error was thrown", "buffer::error from the short decode");
  for (const auto& [path, inst] : dpc.instances) {
    EXPECT_GT(points(inst), 0u)
      << ctx(seq, state() + ": instance " + path + " is empty",
	     "no instance without a point: PerfCounterInstance::get_latest_data "
	     "on it is undefined behaviour");
  }
}

TEST_F(PerfCountersOrdering, OrderingBug_P2_FailedReport_DeclaredTypesRetained)
{
  const std::string seq =
    "declare(X,Y,Z scalar); report(packed = frame{5})";
  Sess s;
  s.declare("X", kScalar);
  s.declare("Y", kScalar);
  s.declare("Z", kScalar);
  auto m = s.wrap(frame({5}));
  EXPECT_THROW(dpc.update(*m), ceph::buffer::error)
    << ctx(seq, "no buffer::error was thrown", "buffer::error from the short decode");
  EXPECT_EQ(s.session->declared_types,
	    (std::set<std::string>{"X", "Y", "Z"}))
    << ctx(seq, "session declared_types=" + describe(s.session->declared_types),
	   "{ X Y Z }: the client has already recorded the declares, so the "
	   "server must keep them");
}

TEST_F(PerfCountersOrdering, OrderingBug_P3_ReopenThenLateOldSession_RecreatesOnlyItsOwnPaths)
{
  const std::string seq =
    "A.declare(X,Y); A.report(1); clear(); B.declare(X); B.report(2); A.report(3)";
  Sess a, b;
  a.declare("X", kScalar);
  a.declare("Y", kScalar);
  ASSERT_NO_THROW(dpc.update(*a.report(1)))
    << ctx(seq, "update() threw", "update() accepts a well-formed report");
  dpc.clear();
  b.declare("X", kScalar);
  ASSERT_NO_THROW(dpc.update(*b.report(2)))
    << ctx(seq, "update() threw", "update() accepts a well-formed report");
  ASSERT_NO_THROW(dpc.update(*a.report(3)))
    << ctx(seq, "late report from A threw",
	   "late report accepted: instances are recreated lazily");
  ASSERT_EQ(dpc.instances.count("Y"), 1u)
    << ctx(seq, state(), "Y recreated from A's own declared set");
  EXPECT_EQ(dpc.instances.at("Y").get_data().size(), 1u)
    << ctx(seq, state(), "Y holds only A's post-clear point");
  EXPECT_EQ(dpc.instances.at("X").get_data().size(), 2u)
    << ctx(seq, state(), "X holds B's point and A's late point");
}

TEST_F(PerfCountersOrdering, OrderingBug_P3_ReopenThenNewSession_OnlyNewSessionVisible)
{
  const std::string seq =
    "A.declare(X,Y); A.report(1); clear(); B.declare(X); B.report(2)";
  Sess a, b;
  a.declare("X", kScalar);
  a.declare("Y", kScalar);
  dpc.update(*a.report(1));
  dpc.clear();
  b.declare("X", kScalar);
  dpc.update(*b.report(2));
  EXPECT_EQ(dpc.instances.size(), 1u) << ctx(seq, state(), "only X");
  ASSERT_EQ(dpc.instances.count("X"), 1u) << ctx(seq, state(), "X present");
  ASSERT_EQ(dpc.instances.at("X").get_data().size(), 1u)
    << ctx(seq, state(), "X holds one point");
  EXPECT_EQ(dpc.instances.at("X").get_latest_data().v, 2u)
    << ctx(seq, state(), "X holds B's value 2, not A's 1");
}

TEST_F(PerfCountersOrdering, OrderingBug_P3_ClearBetweenReports_LazyRecreateNoThrow)
{
  const std::string seq = "A.declare(X); A.report(1); clear(); A.report(2)";
  Sess a;
  a.declare("X", kScalar);
  dpc.update(*a.report(1));
  dpc.clear();
  EXPECT_NO_THROW(dpc.update(*a.report(2)))
    << ctx(seq, "update threw after clear", "instance recreated lazily");
  ASSERT_EQ(dpc.instances.count("X"), 1u) << ctx(seq, state(), "X recreated");
  EXPECT_EQ(dpc.instances.at("X").get_data().size(), 1u)
    << ctx(seq, state(), "X holds only the point pushed after clear");
}

TEST_F(PerfCountersOrdering, OrderingBug_P3_ClearDoesNotTouchSessionDeclaredTypes)
{
  const std::string seq = "A.declare(X,Y); A.report(1); clear()";
  Sess a;
  a.declare("X", kScalar);
  a.declare("Y", kScalar);
  dpc.update(*a.report(1));
  dpc.clear();
  EXPECT_EQ(a.session->declared_types, (std::set<std::string>{"X", "Y"}))
    << ctx(seq, describe(a.session->declared_types), "{ X Y } unchanged");
  EXPECT_TRUE(dpc.instances.empty()) << ctx(seq, state(), "no instances");
}

TEST_F(PerfCountersOrdering, OrderingBug_P4_Undeclare_NoNewPoint)
{
  const std::string seq =
    "declare(X,Y); report(1); undeclare(X); report(2)";
  Sess s;
  s.declare("X", kScalar);
  s.declare("Y", kScalar);
  dpc.update(*s.report(1));
  s.undeclare("X");
  dpc.update(*s.report(2));
  EXPECT_EQ(dpc.instances.at("X").get_data().size(), 1u)
    << ctx(seq, state(), "X keeps its single old point");
  EXPECT_EQ(dpc.instances.at("Y").get_data().size(), 2u)
    << ctx(seq, state(), "Y has two points");
}

TEST_F(PerfCountersOrdering, OrderingBug_P4_Undeclare_InstanceRetainedWithOldValue)
{
  const std::string seq = "declare(X); report(41); undeclare(X); report()";
  Sess s;
  s.declare("X", kScalar);
  dpc.update(*s.report(41));
  s.undeclare("X");
  dpc.update(*s.report(0));
  ASSERT_EQ(dpc.instances.count("X"), 1u)
    << ctx(seq, state(), "instance retained after undeclare");
  EXPECT_EQ(dpc.instances.at("X").get_latest_data().v, 41u)
    << ctx(seq, state(), "old value 41 retained");
}

TEST_F(PerfCountersOrdering, OrderingBug_P4_DeclaredPathAlwaysInTypes)
{
  const std::string seq = "declare(X); undeclare(X); declare(X); report";
  Sess s;
  s.declare("X", kScalar);
  dpc.update(*s.report(1));
  s.undeclare("X");
  dpc.update(*s.report(1));
  s.declare("X", kScalar);
  dpc.update(*s.report(1));
  for (const auto& p : s.session->declared_types) {
    EXPECT_EQ(types.count(p), 1u)
      << ctx(seq, "declared " + p + " missing from " + describe(types),
	     "every declared path has a type");
  }
  EXPECT_EQ(types.count("X"), 1u)
    << ctx(seq, describe(types), "types keeps X after undeclare");
}

TEST_F(PerfCountersOrdering, OrderingBug_P4_UndeclareUnknownPath_NoOp)
{
  const std::string seq = "declare(X); report(1); undeclare(Q); report(2)";
  Sess s;
  s.declare("X", kScalar);
  dpc.update(*s.report(1));
  s.pending_undeclare.push_back("Q");
  EXPECT_NO_THROW(dpc.update(*s.report(2)))
    << ctx(seq, "update() threw", "update() accepts a well-formed report");
  EXPECT_EQ(s.session->declared_types, (std::set<std::string>{"X"}))
    << ctx(seq, describe(s.session->declared_types), "{ X }");
  EXPECT_EQ(dpc.instances.at("X").get_data().size(), 2u)
    << ctx(seq, state(), "X has two points");
}

TEST_F(PerfCountersOrdering, OrderingBug_P4_DeclareAndUndeclareInOneReport_EndsUndeclared)
{
  const std::string seq = "one report: declare(X) and undeclare(X)";
  Sess s;
  PerfCounterType t;
  t.path = "X";
  t.type = kScalar;
  s.pending_declare.push_back(t);
  s.pending_undeclare.push_back("X");
  EXPECT_NO_THROW(dpc.update(*s.wrap(frame({}))))
    << ctx(seq, "update() threw", "update() accepts a well-formed report");
  EXPECT_TRUE(s.session->declared_types.empty())
    << ctx(seq, describe(s.session->declared_types), "{ }");
  EXPECT_TRUE(dpc.instances.empty()) << ctx(seq, state(), "no instance");
}

TEST_F(PerfCountersOrdering, OrderingBug_P4_UndeclareThenRedeclareOtherKind_UsesNewDecode)
{
  const std::string seq =
    "declare(X,scalar); report(1); undeclare(X); declare(X,avg); report(5,2)";
  Sess s;
  s.declare("X", kScalar);
  dpc.update(*s.report(1));
  s.undeclare("X");
  s.declare("X", kAvg);
  EXPECT_NO_THROW(dpc.update(*s.report(5, 2)))
    << ctx(seq, "update threw: the stored type for X is still scalar, "
		"so the avg payload was decoded as scalar and DECODE_FINISH "
		"found trailing bytes",
	   "X decoded as avg after the client redeclared it as avg");
  auto it = dpc.instances.find("X");
  ASSERT_NE(it, dpc.instances.end()) << ctx(seq, state(), "X present");
  EXPECT_EQ(it->second.get_data_avg().size(), 1u)
    << ctx(seq, state(), "one avg point (5/2)");
}

TEST_F(PerfCountersOrdering, OrderingBug_P5_DeclareTwice_OnceInSetInstanceKept)
{
  const std::string seq = "declare(X); report(1); declare(X); report(2)";
  Sess s;
  s.declare("X", kScalar);
  dpc.update(*s.report(1));
  s.declare("X", kScalar);
  dpc.update(*s.report(2));
  EXPECT_EQ(s.session->declared_types.size(), 1u)
    << ctx(seq, describe(s.session->declared_types), "X once");
  EXPECT_EQ(dpc.instances.size(), 1u) << ctx(seq, state(), "one instance");
  EXPECT_EQ(dpc.instances.at("X").get_data().size(), 2u)
    << ctx(seq, state(), "instance not reset: two points");
}

TEST_F(PerfCountersOrdering, OrderingBug_P5_DeclareTwiceInOneReport_OnceInSet)
{
  const std::string seq = "one report: declare(X) twice";
  Sess s;
  s.declared["X"] = kScalar;
  PerfCounterType t;
  t.path = "X";
  t.type = kScalar;
  s.pending_declare = {t, t};
  dpc.update(*s.wrap(s.pack(4, 0)));
  EXPECT_EQ(s.session->declared_types.size(), 1u)
    << ctx(seq, describe(s.session->declared_types), "X once");
  EXPECT_EQ(dpc.instances.at("X").get_data().size(), 1u)
    << ctx(seq, state(), "one point");
}

TEST_F(PerfCounterInstanceOrdering, OrderingBug_P6_Scalar_KeepsNewest20InOrder)
{
  PerfCounterInstance inst(kScalar);
  for (uint64_t i = 1; i <= 25; ++i) {
    inst.push(utime_t(i, 0), i);
  }
  ASSERT_EQ(inst.get_data().size(), 20u)
    << ctx("push 1..25", "size=" + std::to_string(inst.get_data().size()),
	   "20");
  for (size_t k = 0; k < 20; ++k) {
    EXPECT_EQ(inst.get_data()[k].v, 6 + k)
      << ctx("push 1..25", "index " + std::to_string(k) + " = " +
				std::to_string(inst.get_data()[k].v),
	     "oldest first: " + std::to_string(6 + k));
  }
  EXPECT_EQ(inst.get_latest_data().v, 25u)
    << ctx("push 1..25", std::to_string(inst.get_latest_data().v), "25");
}

TEST_F(PerfCounterInstanceOrdering, OrderingBug_P6_Avg_KeepsNewest20InOrder)
{
  PerfCounterInstance inst(kAvg);
  for (uint64_t i = 1; i <= 25; ++i) {
    inst.push_avg(utime_t(i, 0), i, 100 + i);
  }
  ASSERT_EQ(inst.get_data_avg().size(), 20u)
    << ctx("push_avg 1..25",
	   "size=" + std::to_string(inst.get_data_avg().size()), "20");
  for (size_t k = 0; k < 20; ++k) {
    EXPECT_EQ(inst.get_data_avg()[k].s, 6 + k)
      << ctx("push_avg 1..25", "sum at " + std::to_string(k),
	     "oldest first: " + std::to_string(6 + k));
    EXPECT_EQ(inst.get_data_avg()[k].c, 106 + k)
      << ctx("push_avg 1..25", "count at " + std::to_string(k),
	     std::to_string(106 + k));
  }
  EXPECT_EQ(inst.get_latest_data_avg().s, 25u)
    << ctx("push_avg 1..25", "latest sum", "25");
}

TEST_F(PerfCounterInstanceOrdering, OrderingBug_P6_ViaUpdate_WraparoundKeepsLatest)
{
  PerfCounterTypes t;
  DaemonPerfCounters d(t);
  Sess s;
  s.declare("X", kScalar);
  for (uint64_t i = 1; i <= 25; ++i) {
    d.update(*s.report(i));
  }
  ASSERT_EQ(d.instances.at("X").get_data().size(), 20u)
    << ctx("25 reports", describe(d), "20 points");
  EXPECT_EQ(d.instances.at("X").get_latest_data().v, 25u)
    << ctx("25 reports", describe(d), "latest 25");
  EXPECT_EQ(d.instances.at("X").get_data().front().v, 6u)
    << ctx("25 reports", describe(d), "oldest 6");
}

TEST_F(PerfCounterInstanceOrdering, OrderingBug_P6_EmptyScalar_GetLatestDataRejected)
{
  PerfCounterInstance inst(kScalar);
  EXPECT_DEATH(inst.get_latest_data(), "")
    << ctx("PerfCounterInstance(scalar).get_latest_data() on a fresh instance",
	   "back() on an empty circular_buffer returned a reference without "
	   "aborting (BOOST_CB_ASSERT is compiled out)",
	   "the call is rejected (abort or exception), not undefined behaviour");
}

TEST_F(PerfCounterInstanceOrdering, OrderingBug_P6_EmptyAvg_GetLatestDataAvgRejected)
{
  PerfCounterInstance inst(kAvg);
  EXPECT_DEATH(inst.get_latest_data_avg(), "")
    << ctx("PerfCounterInstance(avg).get_latest_data_avg() on a fresh instance",
	   "back() on an empty circular_buffer returned a reference without "
	   "aborting (BOOST_CB_ASSERT is compiled out)",
	   "the call is rejected (abort or exception), not undefined behaviour");
}

TEST_F(PerfCounterInstanceOrdering, OrderingBug_P6_ScalarInstance_GetLatestDataAvgRejected)
{
  PerfCounterInstance inst(kScalar);
  inst.push(utime_t(1, 0), 1);
  EXPECT_DEATH(inst.get_latest_data_avg(), "")
    << ctx("scalar instance holding one point, then get_latest_data_avg()",
	   "back() on the zero-capacity avg_buffer returned a reference "
	   "without aborting",
	   "the call is rejected (abort or exception), not undefined behaviour");
}

TEST_F(PerfCountersOrdering, OrderingBug_B_HeaderOnlyPacked_NoDeclaredTypes_NoOp)
{
  const std::string seq = "report(no declares, packed = empty frame)";
  Sess s;
  EXPECT_NO_THROW(dpc.update(*s.wrap(frame({}))))
    << ctx(seq, "update() threw", "update() accepts a well-formed report");
  EXPECT_TRUE(dpc.instances.empty()) << ctx(seq, state(), "no instances");
}

TEST_F(PerfCountersOrdering, OrderingBug_B_EmptyPacked_ThrowsBufferError)
{
  const std::string seq = "report(no declares, packed = empty buffer)";
  Sess s;
  EXPECT_THROW(dpc.update(*s.wrap(ceph::buffer::list())), ceph::buffer::error)
    << ctx(seq, "no buffer::error", "buffer::error");
}

TEST_F(PerfCountersOrdering, OrderingBug_B_DeclareInReportWithTrailingBytes_StillDecodes)
{
  const std::string seq = "declare(X); report(frame{9, extra})";
  Sess s;
  s.declare("X", kScalar);
  EXPECT_NO_THROW(dpc.update(*s.wrap(frame({9, 10}))))
    << ctx(seq, "threw on trailing bytes inside the struct",
	   "DECODE_FINISH skips the unread tail of a struct");
  EXPECT_EQ(dpc.instances.at("X").get_latest_data().v, 9u)
    << ctx(seq, state(), "9");
}

TEST_F(PerfCountersOrdering, OrderingBug_B_TwoDaemonsShareTypes_InstancesIndependent)
{
  const std::string seq = "A.declare(X); B.report(X) via A's type; A.clear()";
  DaemonPerfCounters dpc_b(types);
  Sess a, b;
  a.declare("X", kScalar);
  b.declare("X", kScalar);
  dpc.update(*a.report(1));
  dpc_b.update(*b.report(2));
  dpc.clear();
  EXPECT_TRUE(dpc.instances.empty()) << ctx(seq, state(), "A cleared");
  ASSERT_EQ(dpc_b.instances.count("X"), 1u)
    << ctx(seq, describe(dpc_b), "B untouched");
  EXPECT_EQ(dpc_b.instances.at("X").get_latest_data().v, 2u)
    << ctx(seq, describe(dpc_b), "2");
}

TEST_F(PerfCountersOrdering, OrderingBug_B_ManyPathsSortedDecodeOrder)
{
  const std::string seq = "declare(c,a,b) out of order; report(10)";
  Sess s;
  s.declare("c", kScalar);
  s.declare("a", kScalar);
  s.declare("b", kScalar);
  dpc.update(*s.report(10));
  EXPECT_EQ(dpc.instances.at("a").get_latest_data().v, 10u)
    << ctx(seq, state(), "a=10");
  EXPECT_EQ(dpc.instances.at("b").get_latest_data().v, 11u)
    << ctx(seq, state(), "b=11");
  EXPECT_EQ(dpc.instances.at("c").get_latest_data().v, 12u)
    << ctx(seq, state(), "c=12");
}

namespace {

enum class WalkOp { Declare, Undeclare, Report, Reopen, StaleReport };

struct Step {
  WalkOp op;
  int path;
  perfcounter_type_d kind;
};

const char* const kWalkPaths[] = {"p0", "p1", "p2"};
const perfcounter_type_d kFixedKind[] = {kScalar, kAvg, kScalar};

std::string show(const Step& s)
{
  std::string p = kWalkPaths[s.path];
  switch (s.op) {
  case WalkOp::Declare:
    return "declare(" + p + "," + kind_name(s.kind) + ") on newest session";
  case WalkOp::Undeclare:
    return "undeclare(" + p + ") on newest session";
  case WalkOp::Report:
    return "report from newest session";
  case WalkOp::Reopen:
    return "reopen: clear() and new session";
  case WalkOp::StaleReport:
    return "report from first session (stale after a reopen)";
  }
  return "";
}

std::string show(const std::vector<Step>& steps)
{
  std::ostringstream o;
  for (size_t i = 0; i < steps.size(); ++i) {
    o << "\n    [" << i << "] " << show(steps[i]);
  }
  return o.str();
}

// MgrClient::_send_report declares only paths it has not declared and
// undeclares only declared ones, and never both for one path in one report.
bool client_can_emit(const Sess& s, const std::string& path, bool declare)
{
  for (const auto& t : s.pending_declare) {
    if (t.path == path) {
      return false;
    }
  }
  for (const auto& u : s.pending_undeclare) {
    if (u == path) {
      return false;
    }
  }
  return declare ? s.declared.count(path) == 0 : s.declared.count(path) == 1;
}

// Empty result means every invariant held after every step.
std::string replay(const std::vector<Step>& steps)
{
  PerfCounterTypes t;
  DaemonPerfCounters d(t);
  std::vector<std::unique_ptr<Sess>> sessions;
  sessions.push_back(std::make_unique<Sess>());
  std::map<std::string, size_t> expected;
  uint64_t seqno = 0;

  auto report_from = [&](Sess& s) -> std::string {
    ++seqno;
    for (const auto& entry : s.declared) {
      size_t& n = expected[entry.first];
      n = std::min<size_t>(n + 1, 20);
    }
    try {
      d.update(*s.report(seqno, seqno));
    } catch (const std::exception& e) {
      return std::string("update threw: ") + e.what();
    }
    return "";
  };

  auto check = [&]() -> std::string {
    for (const auto& [path, inst] : d.instances) {
      auto tit = t.find(path);
      if (tit == t.end()) {
	return "instance " + path + " has no entry in types";
      }
      bool avg = inst.get_data_avg().capacity() != 0;
      if (avg != is_avg(tit->second.type)) {
	return "instance " + path + " is " + (avg ? "avg" : "scalar") +
	       " but types says " + kind_name(tit->second.type);
      }
      auto e = expected.find(path);
      size_t want = e == expected.end() ? 0 : e->second;
      if (points(inst) != want) {
	return "instance " + path + " holds " + std::to_string(points(inst)) +
	       " points but sessions that declared it pushed " +
	       std::to_string(want) + " since the last clear";
      }
    }
    for (const auto& [path, n] : expected) {
      if (n != 0 && d.instances.count(path) == 0) {
	return "path " + path + " was pushed " + std::to_string(n) +
	       " time(s) but has no instance";
      }
    }
    for (const auto& s : sessions) {
      for (const auto& p : s->session->declared_types) {
	if (t.count(p) == 0) {
	  return "declared path " + p + " has no entry in types";
	}
      }
    }
    Sess& cur = *sessions.back();
    for (const auto& p : cur.session->declared_types) {
      auto declared = cur.declared.find(p);
      auto inst = d.instances.find(p);
      if (declared == cur.declared.end() || inst == d.instances.end()) {
	continue;
      }
      if ((inst->second.get_data_avg().capacity() != 0) !=
	  is_avg(declared->second)) {
	return "instance " + p + " is " +
	       (inst->second.get_data_avg().capacity() ? "avg" : "scalar") +
	       " but the current session declared " + kind_name(declared->second);
      }
    }
    return "";
  };

  for (size_t i = 0; i < steps.size(); ++i) {
    const Step& s = steps[i];
    std::string why;
    switch (s.op) {
    case WalkOp::Declare:
      if (client_can_emit(*sessions.back(), kWalkPaths[s.path], true)) {
	sessions.back()->declare(kWalkPaths[s.path], s.kind);
      }
      break;
    case WalkOp::Undeclare:
      if (client_can_emit(*sessions.back(), kWalkPaths[s.path], false)) {
	sessions.back()->undeclare(kWalkPaths[s.path]);
      }
      break;
    case WalkOp::Report:
      why = report_from(*sessions.back());
      break;
    case WalkOp::Reopen:
      d.clear();
      expected.clear();
      sessions.push_back(std::make_unique<Sess>());
      break;
    case WalkOp::StaleReport:
      if (sessions.size() > 1) {
	why = report_from(*sessions.front());
      }
      break;
    }
    if (why.empty()) {
      why = check();
    }
    if (!why.empty()) {
      return "after step " + std::to_string(i) + " (" + show(s) + "): " + why +
	     "\n  Current: " + describe(d) + " " + describe(t);
    }
  }
  return "";
}

std::vector<Step> shrink(std::vector<Step> steps)
{
  for (bool progress = true; progress;) {
    progress = false;
    for (size_t i = 0; i < steps.size();) {
      auto candidate = steps;
      candidate.erase(candidate.begin() + i);
      if (!replay(candidate).empty()) {
	steps = std::move(candidate);
	progress = true;
      } else {
	++i;
      }
    }
  }
  return steps;
}

void run_walk(bool kind_fixed_per_path, unsigned seed)
{
  static const WalkOp kWeights[] = {
    WalkOp::Declare, WalkOp::Declare, WalkOp::Undeclare, WalkOp::Report,
    WalkOp::Report, WalkOp::Reopen, WalkOp::StaleReport};
  std::mt19937 rng(seed);
  const int kSequences = 100000;
  const int kLen = 10;

  for (int n = 0; n < kSequences; ++n) {
    std::vector<Step> steps;
    for (int k = 0; k < kLen; ++k) {
      Step s;
      s.op = kWeights[rng() % 7];
      s.path = rng() % 3;
      s.kind = kind_fixed_per_path ? kFixedKind[s.path]
				   : ((rng() & 1) ? kAvg : kScalar);
      steps.push_back(s);
    }
    std::string why = replay(steps);
    if (!why.empty()) {
      auto minimal = shrink(steps);
      ADD_FAILURE()
	<< "Randomized invariant violation"
	<< "\n  Seed: 0x" << std::hex << seed << std::dec
	<< "\n  Sequence index: " << n << " of " << kSequences
	<< "\n  Minimal failing sequence:" << show(minimal)
	<< "\n  Current: " << replay(minimal)
	<< "\n  Expected: every instance path in types, its kind equal to the "
	   "type table and to the kind the current session declared, and its "
	   "point count equal to the pushes made by sessions that declared the "
	   "path since the last clear";
      return;
    }
  }
}

} // namespace

TEST_F(PerfCountersOrdering, OrderingBug_W_RandomizedWalk_AllInvariants)
{
  run_walk(false, 0x5eed0c01u);
}

TEST_F(PerfCountersOrdering, OrderingBug_W_RandomizedWalk_KindFixedPerPath_AllInvariants)
{
  run_walk(true, 0x5eed0c02u);
}

TEST_F(PerfCountersOrdering, OrderingBug_B_NewerStructCompat_ThrowsAndLeavesInstances)
{
  const std::string seq =
    "declare(X); report(1); report(packed = struct v2 compat 2)";
  Sess s;
  s.declare("X", kScalar);
  ASSERT_NO_THROW(dpc.update(*s.report(1)))
    << ctx(seq, "update() threw", "update() accepts a well-formed report");
  const std::string before = describe(dpc);
  ceph::buffer::list bl;
  ENCODE_START(2, 2, bl);
  encode(static_cast<uint64_t>(9), bl);
  ENCODE_FINISH(bl);
  EXPECT_THROW(dpc.update(*s.wrap(bl)), ceph::buffer::malformed_input)
    << ctx(seq, "no malformed_input", "malformed_input for compat 2 > 1");
  EXPECT_EQ(describe(dpc), before)
    << ctx(seq, describe(dpc), "unchanged: " + before);
}

TEST_F(PerfCountersOrdering, OrderingBug_B_ExactlyCapacityThenOneMore_ViaUpdate)
{
  const std::string seq = "declare(X,avg); 20 reports; 1 more report";
  Sess s;
  s.declare("X", kAvg);
  for (uint64_t i = 1; i <= 20; ++i) {
    dpc.update(*s.report(i, i + 100));
  }
  ASSERT_EQ(dpc.instances.at("X").get_data_avg().size(), 20u)
    << ctx(seq, describe(dpc), "20 points");
  EXPECT_EQ(dpc.instances.at("X").get_data_avg().front().s, 1u)
    << ctx(seq, describe(dpc), "oldest is 1 at capacity");
  dpc.update(*s.report(21, 121));
  EXPECT_EQ(dpc.instances.at("X").get_data_avg().size(), 20u)
    << ctx(seq, describe(dpc), "still 20 points");
  EXPECT_EQ(dpc.instances.at("X").get_data_avg().front().s, 2u)
    << ctx(seq, describe(dpc), "oldest is 2 after one more");
  EXPECT_EQ(dpc.instances.at("X").get_latest_data_avg().c, 121u)
    << ctx(seq, describe(dpc), "latest count 121");
}

TEST_F(PerfCountersOrdering, OrderingBug_B_AvgZeroCount_StoredFaithfully)
{
  const std::string seq = "declare(X,avg); report(val=0,count=0)";
  Sess s;
  s.declare("X", kAvg);
  ASSERT_NO_THROW(dpc.update(*s.report(0, 0)))
    << ctx(seq, "update() threw", "update() accepts a well-formed report");
  ASSERT_EQ(dpc.instances.at("X").get_data_avg().size(), 1u)
    << ctx(seq, describe(dpc), "one point");
  EXPECT_EQ(dpc.instances.at("X").get_latest_data_avg().c, 0u)
    << ctx(seq, describe(dpc), "count 0");
}

TEST_F(PerfCountersOrdering, OrderingBug_B_TimeAndNoneKinds_DecodedAsScalar)
{
  const std::string seq =
    "declare(t,TIME|U64), declare(n,NONE); report(val=3)";
  Sess s;
  s.declare("n", PERFCOUNTER_NONE);
  s.declare("t", static_cast<perfcounter_type_d>(PERFCOUNTER_TIME | PERFCOUNTER_U64));
  ASSERT_NO_THROW(dpc.update(*s.report(3)))
    << ctx(seq, "update() threw", "update() accepts a well-formed report");
  EXPECT_EQ(dpc.instances.at("n").get_latest_data().v, 3u)
    << ctx(seq, state(), "n=3");
  EXPECT_EQ(dpc.instances.at("t").get_latest_data().v, 4u)
    << ctx(seq, state(), "t=4");
}

TEST_F(PerfCountersOrdering, OrderingBug_B_TimeLongRunAvgKind_DecodedAsAvg)
{
  const std::string seq = "declare(t,TIME|LONGRUNAVG|COUNTER); report(val=6,count=2)";
  Sess s;
  s.declare("t", static_cast<perfcounter_type_d>(
		   PERFCOUNTER_TIME | PERFCOUNTER_LONGRUNAVG | PERFCOUNTER_COUNTER));
  ASSERT_NO_THROW(dpc.update(*s.report(6, 2)))
    << ctx(seq, "update() threw", "update() accepts a well-formed report");
  ASSERT_EQ(dpc.instances.at("t").get_data_avg().size(), 1u)
    << ctx(seq, state(), "one avg point");
  EXPECT_EQ(dpc.instances.at("t").get_latest_data_avg().s, 6u)
    << ctx(seq, state(), "sum 6");
  EXPECT_EQ(dpc.instances.at("t").get_latest_data_avg().c, 2u)
    << ctx(seq, state(), "count 2");
}

TEST_F(PerfCountersOrdering, OrderingBug_B_HistogramKind_DecodedAsSingleScalar)
{
  const std::string seq = "declare(h,HISTOGRAM|U64), declare(z,U64); report(val=5)";
  Sess s;
  s.declare("h", static_cast<perfcounter_type_d>(PERFCOUNTER_HISTOGRAM | PERFCOUNTER_U64));
  s.declare("z", kScalar);
  ASSERT_NO_THROW(dpc.update(*s.report(5)))
    << ctx(seq, "update() threw", "update() accepts a well-formed report");
  EXPECT_EQ(dpc.instances.at("h").get_latest_data().v, 5u)
    << ctx(seq, state(), "h=5: MgrClient encodes one u64 for a histogram");
  EXPECT_EQ(dpc.instances.at("z").get_latest_data().v, 6u)
    << ctx(seq, state(), "z=6, not shifted by the histogram");
}

TEST_F(PerfCountersOrdering, OrderingBug_B_AvgThenScalarSamePacked_NeighbourInSync)
{
  const std::string seq =
    "declare(a,avg), declare(b,scalar); report(val=10,count=20,avgcount2=7777 then 11)";
  Sess s;
  s.declare("a", kAvg);
  s.declare("b", kScalar);
  ASSERT_NO_THROW(dpc.update(*s.report(10, 20)))
    << ctx(seq, "update() threw", "update() accepts a well-formed report");
  EXPECT_EQ(dpc.instances.at("a").get_latest_data_avg().s, 10u)
    << ctx(seq, state(), "a.sum=10");
  EXPECT_EQ(dpc.instances.at("a").get_latest_data_avg().c, 20u)
    << ctx(seq, state(), "a.count=20");
  EXPECT_EQ(dpc.instances.at("b").get_latest_data().v, 11u)
    << ctx(seq, state(), "b=11: the avgcount2 word of a was consumed");
}

TEST_F(PerfCountersOrdering, OrderingBug_P4_UndeclareSamePathTwiceInOneReport_NoOp)
{
  const std::string seq = "declare(X,Y); report(1); undeclare(X) twice in one report";
  Sess s;
  s.declare("X", kScalar);
  s.declare("Y", kScalar);
  dpc.update(*s.report(1));
  s.declared.erase("X");
  s.pending_undeclare = {"X", "X"};
  ASSERT_NO_THROW(dpc.update(*s.report(2)))
    << ctx(seq, "update() threw", "update() accepts a well-formed report");
  EXPECT_EQ(s.session->declared_types, (std::set<std::string>{"Y"}))
    << ctx(seq, describe(s.session->declared_types), "{ Y }");
  EXPECT_EQ(dpc.instances.at("Y").get_data().size(), 2u)
    << ctx(seq, state(), "Y has two points");
}

TEST_F(PerfCountersOrdering, OrderingBug_P4_UndeclareThenRedeclareSameKind_ResumesSameInstance)
{
  const std::string seq =
    "declare(X); report(1); undeclare(X); report(2); declare(X); report(3)";
  Sess s;
  s.declare("X", kScalar);
  dpc.update(*s.report(1));
  s.undeclare("X");
  dpc.update(*s.report(2));
  s.declare("X", kScalar);
  ASSERT_NO_THROW(dpc.update(*s.report(3)))
    << ctx(seq, "update() threw", "update() accepts a well-formed report");
  ASSERT_EQ(dpc.instances.at("X").get_data().size(), 2u)
    << ctx(seq, state(), "points 1 and 3; none for the undeclared report");
  EXPECT_EQ(dpc.instances.at("X").get_data()[0].v, 1u)
    << ctx(seq, state(), "oldest point 1");
  EXPECT_EQ(dpc.instances.at("X").get_data()[1].v, 3u)
    << ctx(seq, state(), "newest point 3");
}

TEST_F(PerfCountersOrdering, OrderingBug_B_LabeledPathWithEmbeddedNul_RoundTrips)
{
  const std::string path = std::string("osd_scrub_sh_repl\0level\0shallow\0.successful", 41);
  const std::string seq = "declare(labeled path with NUL separators); report(12)";
  Sess s;
  s.declare(path, kScalar);
  s.declare("plain", kScalar);
  ASSERT_NO_THROW(dpc.update(*s.report(12)))
    << ctx(seq, "update() threw", "update() accepts a well-formed report");
  ASSERT_EQ(dpc.instances.count(path), 1u)
    << ctx(seq, state(), "instance keyed by the full path including NULs");
  EXPECT_EQ(dpc.instances.at(path).get_latest_data().v, 12u)
    << ctx(seq, state(), "12");
  EXPECT_EQ(dpc.instances.at("plain").get_latest_data().v, 13u)
    << ctx(seq, state(), "plain=13");
}

TEST_F(PerfCountersOrdering, OrderingBug_B_EmptyPathDeclared_RoundTrips)
{
  const std::string seq = "declare(\"\",scalar); report(8)";
  Sess s;
  s.declare("", kScalar);
  ASSERT_NO_THROW(dpc.update(*s.report(8)))
    << ctx(seq, "update() threw", "update() accepts a well-formed report");
  ASSERT_EQ(dpc.instances.count(""), 1u) << ctx(seq, state(), "instance \"\"");
  EXPECT_EQ(dpc.instances.at("").get_latest_data().v, 8u)
    << ctx(seq, state(), "8");
}

TEST_F(PerfCountersOrdering, OrderingBug_B_NewerStructVersionCompat1_StillDecodes)
{
  const std::string seq = "declare(X); report(packed = struct v2 compat 1)";
  Sess s;
  s.declare("X", kScalar);
  ceph::buffer::list bl;
  ENCODE_START(2, 1, bl);
  encode(static_cast<uint64_t>(9), bl);
  encode(static_cast<uint64_t>(10), bl);
  ENCODE_FINISH(bl);
  EXPECT_NO_THROW(dpc.update(*s.wrap(bl)))
    << ctx(seq, "threw on a v2 struct with compat 1",
	   "DECODE_START(1) accepts struct_compat <= 1");
  EXPECT_EQ(dpc.instances.at("X").get_latest_data().v, 9u)
    << ctx(seq, state(), "9");
}

TEST_F(PerfCountersOrdering, OrderingBug_B_MaxUint64Values_StoredUnchanged)
{
  const std::string seq = "declare(S,scalar), declare(V,avg); report(UINT64_MAX)";
  Sess s;
  s.declare("S", kScalar);
  s.declare("V", kAvg);
  const uint64_t max = std::numeric_limits<uint64_t>::max();
  ceph::buffer::list bl;
  ENCODE_START(1, 1, bl);
  encode(max, bl);
  encode(max, bl);
  encode(max, bl);
  encode(static_cast<uint64_t>(0), bl);
  ENCODE_FINISH(bl);
  ASSERT_NO_THROW(dpc.update(*s.wrap(bl)))
    << ctx(seq, "update() threw", "update() accepts a well-formed report");
  EXPECT_EQ(dpc.instances.at("S").get_latest_data().v, max)
    << ctx(seq, state(), "S = UINT64_MAX");
  EXPECT_EQ(dpc.instances.at("V").get_latest_data_avg().s, max)
    << ctx(seq, state(), "V.sum = UINT64_MAX");
  EXPECT_EQ(dpc.instances.at("V").get_latest_data_avg().c, max)
    << ctx(seq, state(), "V.count = UINT64_MAX");
}

TEST_F(PerfCountersOrdering, OrderingBug_P4_UndeclareAllThenReport_NoPointsAdded)
{
  const std::string seq = "declare(X,Y); report(1); undeclare(X,Y); report()";
  Sess s;
  s.declare("X", kScalar);
  s.declare("Y", kAvg);
  dpc.update(*s.report(1, 1));
  s.undeclare("X");
  s.undeclare("Y");
  ASSERT_NO_THROW(dpc.update(*s.report(2, 2)))
    << ctx(seq, "update() threw", "update() accepts a well-formed report");
  EXPECT_TRUE(s.session->declared_types.empty())
    << ctx(seq, describe(s.session->declared_types), "{ }");
  EXPECT_EQ(points(dpc.instances.at("X")), 1u) << ctx(seq, state(), "X: 1 point");
  EXPECT_EQ(points(dpc.instances.at("Y")), 1u) << ctx(seq, state(), "Y: 1 point");
}

TEST_F(PerfCountersOrdering, OrderingBug_P3_TwoReopensInARow_OnlyLastSessionData)
{
  const std::string seq =
    "A.declare(X); A.report(1); clear(); B.declare(Y); clear(); C.declare(Z); C.report(3)";
  Sess a, b, c;
  a.declare("X", kScalar);
  dpc.update(*a.report(1));
  dpc.clear();
  b.declare("Y", kScalar);
  dpc.clear();
  c.declare("Z", kScalar);
  dpc.update(*c.report(3));
  EXPECT_EQ(dpc.instances.size(), 1u) << ctx(seq, state(), "only Z");
  ASSERT_EQ(dpc.instances.count("Z"), 1u) << ctx(seq, state(), "Z present");
  EXPECT_EQ(dpc.instances.at("Z").get_latest_data().v, 3u)
    << ctx(seq, state(), "3");
}

TEST_F(PerfCountersOrdering, OrderingBug_C_UpdateVsClearVsLateSession_LockedInterleavings)
{
  const std::string seq =
    "thread1: A.report x3; thread2: clear, B.declare(X), B.report, A.report (stale), all under the daemon lock";
  const int kIterations = 10000;
  std::string first_failure;

  for (int it = 0; it < kIterations && first_failure.empty(); ++it) {
    PerfCounterTypes t;
    DaemonPerfCounters d(t);
    Sess a, b;
    a.declare("X", kScalar);
    a.declare("Y", kAvg);
    b.declare("X", kScalar);
    std::mutex daemon_lock;
    std::mutex fm;
    std::atomic<bool> go{false};
    auto record = [&](const std::string& why) {
      std::lock_guard g(fm);
      if (first_failure.empty()) {
	first_failure = why;
      }
    };

    std::thread t1([&] {
      while (!go) {}
      for (int i = 0; i < 3; ++i) {
	auto m = a.report(i + 1, i + 1);
	std::lock_guard l(daemon_lock);
	try {
	  d.update(*m);
	} catch (const std::exception& e) {
	  record(std::string("A.report threw: ") + e.what());
	}
      }
    });
    std::thread t2([&] {
      while (!go) {}
      {
	std::lock_guard l(daemon_lock);
	d.clear();
      }
      auto m = b.report(50, 0);
      std::lock_guard l(daemon_lock);
      try {
	d.update(*m);
      } catch (const std::exception& e) {
	record(std::string("B.report threw: ") + e.what());
      }
    });
    go = true;
    t1.join();
    t2.join();

    for (const auto& [path, inst] : d.instances) {
      if (t.count(path) == 0 || points(inst) > 20u ||
	  (inst.get_data_avg().capacity() != 0) != is_avg(t.at(path).type)) {
	record("instance " + path + " inconsistent: " + describe(d) + " " +
	       describe(t));
      }
    }
  }
  EXPECT_TRUE(first_failure.empty())
    << ctx(seq, first_failure,
	   "no throw; every instance in types with the matching kind and at "
	   "most 20 points");
}

TEST_F(PerfCountersOrdering, OrderingBug_C_DeclareVsTypesLookup_NoThrow)
{
  const std::string seq =
    "thread1: other daemon update() declares new paths under its daemon lock; "
    "thread2: types.at(X) for pre-declared X under a different daemon's lock";
  const int kIterations = 10000;
  const int kInserts = 64;
  const int kLookups = 512;
  std::atomic<int> failures{0};
  std::string first_failure;
  std::mutex fm;

  for (int it = 0; it < kIterations && failures == 0; ++it) {
    PerfCounterTypes t;
    DaemonPerfCounters a(t), b(t);
    Sess sa, sb;
    sa.declare("X", kScalar);
    a.update(*sa.report(1));
    std::mutex lock_a, lock_b;
    std::atomic<bool> go{false};

    std::thread writer([&] {
      while (!go) {}
      for (int i = 0; i < kInserts; ++i) {
	sb.declare("w" + std::to_string(i), kScalar);
	auto m = sb.report(i);
	std::lock_guard l(lock_b);
	b.update(*m);
      }
    });
    std::thread reader([&] {
      while (!go) {}
      for (int i = 0; i < kLookups; ++i) {
	std::lock_guard l(lock_a);
	try {
	  if (t.at("X").type != kScalar) {
	    failures++;
	  }
	} catch (const std::exception& e) {
	  std::lock_guard g(fm);
	  first_failure = e.what();
	  failures++;
	}
      }
    });
    go = true;
    writer.join();
    reader.join();
  }
  EXPECT_EQ(failures.load(), 0)
    << ctx(seq, std::to_string(failures.load()) + " failure(s): " +
		first_failure,
	   "the shared types map is never read while another daemon's "
	   "update() inserts into it");
}

int main(int argc, char** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  std::vector<const char*> args(argv, argv + argc);
  auto cct = global_init(nullptr, args, CEPH_ENTITY_TYPE_CLIENT,
			 CODE_ENVIRONMENT_UTILITY,
			 CINIT_FLAG_NO_DEFAULT_CONFIG_FILE);
  common_init_finish(g_ceph_context);
  return RUN_ALL_TESTS();
}
