// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <chrono>
#include <thread>

#include "TestMgr.h"
#include "mgr/ThreadMonitor.h"

TEST_F(ThreadMonitorTestHelper, BasicCreation) {
  ASSERT_NE(thread_monitor, nullptr);
  ASSERT_NE(cct, nullptr);
}

TEST_F(ThreadMonitorTestHelper, Construction) {
  ThreadMonitor tm(cct.get());
  // Should not crash
}

// Regression test for https://tracker.ceph.com/issues/81336
//
// When sysconf(_SC_CLK_TCK)/sysconf(_SC_PAGESIZE) fail, monitoring_loop()
// sets running=false from inside the monitoring thread and returns without
// joining itself. If stop_monitoring() (called from the destructor) gates its
// join() on the `running` flag, it short-circuits on !running and never joins.
// The std::thread then stays joinable, and ~std::thread() on a joinable thread
// calls std::terminate(), crashing the process at shutdown.
//
// The fix makes stop_monitoring() join whenever the thread is joinable,
// regardless of the `running` flag, so the destructor completes normally.
TEST_F(ThreadMonitorTestHelper, SysconfFailureDestructorJoinsThread) {
  // Inject a sysconf failure for the lifetime of this test only, restoring the
  // real sysconf afterwards so other tests are unaffected.
  auto* const saved_sysconf = ThreadMonitor::sysconf_fn;
  ThreadMonitor::sysconf_fn = [](int) -> long { return -1; };

  {
    ThreadMonitor tm(cct.get());
    tm.start_monitoring();
    // Let the monitoring thread reach the sysconf guard, set running=false, and
    // return. The std::thread stays joinable until it is joined, so destroying
    // `tm` at the end of this scope exercises the destructor's join path.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    // ~ThreadMonitor() runs here. On the unfixed code this reaches
    // std::terminate() and aborts the whole test binary before we return.
  }

  ThreadMonitor::sysconf_fn = saved_sysconf;

  SUCCEED()
      << "Current: std::terminate() called because stop_monitoring() skips "
         "join when running==false; "
         "Expected: destructor joins the thread and returns normally.";
}
