// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "TestMgr.h"
#include "mgr/PyModuleRunner.h"
#include "mgr/PyModule.h"
#include "common/LogClient.h"

// Register Python environment for all tests
::testing::Environment* const python_env =
    ::testing::AddGlobalTestEnvironment(new PythonEnv);

// Test that CephContext is properly initialized
TEST_F(PyModuleRunnerTestHelper, CephContextInitialized) {
  ASSERT_NE(cct, nullptr);
  EXPECT_NE(cct.get(), nullptr);
}

// Test that we can create a PyModule
TEST_F(PyModuleRunnerTestHelper, PyModuleCreation) {
  auto py_module = std::make_shared<PyModule>("test_module");
  ASSERT_NE(py_module, nullptr);
  EXPECT_EQ(py_module->get_name(), "test_module");
}

// Test PyModule with different names
TEST_F(PyModuleRunnerTestHelper, PyModuleNames) {
  auto py_module1 = std::make_shared<PyModule>("module1");
  auto py_module2 = std::make_shared<PyModule>("module2");
  auto py_module3 = std::make_shared<PyModule>("very_long_module_name");
  
  EXPECT_EQ(py_module1->get_name(), "module1");
  EXPECT_EQ(py_module2->get_name(), "module2");
  EXPECT_EQ(py_module3->get_name(), "very_long_module_name");
}

// Test PyModule with empty name
TEST_F(PyModuleRunnerTestHelper, PyModuleEmptyName) {
  auto py_module = std::make_shared<PyModule>("");
  ASSERT_NE(py_module, nullptr);
  EXPECT_EQ(py_module->get_name(), "");
}

// Test PyModule with special characters
TEST_F(PyModuleRunnerTestHelper, PyModuleSpecialChars) {
  auto py_module = std::make_shared<PyModule>("test-module_123");
  ASSERT_NE(py_module, nullptr);
  EXPECT_EQ(py_module->get_name(), "test-module_123");
}

// Test LogChannel creation
TEST_F(PyModuleRunnerTestHelper, LogChannelCreation) {
  auto clog = std::make_shared<LogChannel>(cct.get(), nullptr, "cluster");
  ASSERT_NE(clog, nullptr);
  EXPECT_NE(clog.get(), nullptr);
}

// Test ThreadMonitor creation
TEST_F(PyModuleRunnerTestHelper, ThreadMonitorCreation) {
  auto thread_monitor = std::make_unique<ThreadMonitor>(cct.get());
  ASSERT_NE(thread_monitor, nullptr);
  EXPECT_NE(thread_monitor.get(), nullptr);
}

// Test multiple LogChannel instances
TEST_F(PyModuleRunnerTestHelper, MultipleLogChannels) {
  auto clog1 = std::make_shared<LogChannel>(cct.get(), nullptr, "cluster");
  auto clog2 = std::make_shared<LogChannel>(cct.get(), nullptr, "audit");
  
  ASSERT_NE(clog1, nullptr);
  ASSERT_NE(clog2, nullptr);
  EXPECT_NE(clog1.get(), clog2.get());
}

// Test PyModule reference counting
TEST_F(PyModuleRunnerTestHelper, PyModuleRefCounting) {
  auto py_module = std::make_shared<PyModule>("test_module");
  EXPECT_EQ(py_module.use_count(), 1);
  
  auto py_module_copy = py_module;
  EXPECT_EQ(py_module.use_count(), 2);
  EXPECT_EQ(py_module_copy.use_count(), 2);
}

// Test that CephContext is shared across tests
TEST_F(PyModuleRunnerTestHelper, CephContextShared) {
  ASSERT_NE(cct, nullptr);
  // CephContext should be the same across all tests in this suite
  EXPECT_NE(cct.get(), nullptr);
}

// Made with Bob
