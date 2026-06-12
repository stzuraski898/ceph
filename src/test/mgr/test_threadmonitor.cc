// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "TestMgr.h"
#include "mgr/ThreadMonitor.h"

// Test basic creation
TEST_F(ThreadMonitorTestHelper, BasicCreation) {
  ASSERT_NE(thread_monitor, nullptr);
  ASSERT_NE(cct, nullptr);
}

// Test construction
TEST_F(ThreadMonitorTestHelper, Construction) {
  ThreadMonitor tm(cct.get());
  // Should not crash
}

// Made with Bob
