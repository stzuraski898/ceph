// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "TestMgr.h"
#include "mgr/StandbyPyModules.h"
#include "common/Finisher.h"

// Test basic setup and initialization
TEST_F(StandbyPyModulesTestHelper, BasicSetup) {
  ASSERT_NE(py_registry, nullptr);
  ASSERT_NE(mc, nullptr);
  ASSERT_NE(cct, nullptr);
  ASSERT_NE(clog, nullptr);
}

// Test StandbyPyModules construction
TEST_F(StandbyPyModulesTestHelper, Construction) {
  MgrMap test_mgr_map;
  PyModuleConfig module_config;
  Finisher finisher(cct.get());
  finisher.start();
  
  // Create StandbyPyModules instance
  StandbyPyModules standby_modules(
      test_mgr_map,
      module_config,
      clog,
      *mc,
      finisher);
  
  // If we get here without crashing, construction succeeded
  SUCCEED();
  
  finisher.stop();
}

// Made with Bob
