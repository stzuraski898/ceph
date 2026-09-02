// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

/*
 * GIL-Safe Extension of TestMgr.h
 * 
 * This header provides a test fixture that:
 * 1. Reuses TestMgr's infrastructure (MonClient, DaemonState, ClusterState, etc.)
 * 2. Manages GIL carefully to avoid deadlocks with GIL-sensitive functions
 * 3. Provides helpers for testing ActivePyModules functions that use without_gil_t
 * 
 * The key difference from TestMgr.h is that we explicitly release the GIL
 * before calling functions that acquire it internally, preventing deadlocks.
 */

#pragma once

#include <Python.h>
#include <cassert>
#include <array>
#include <functional>

#include "common/async/context_pool.h"
#include "global/global_context.h"
#include "gtest/gtest.h"
#include "messages/MPGStats.h"
#include "mgr/ClusterState.h"
#include "mgr/DaemonState.h"
#include "mon/MgrMap.h"
#include "mon/MonClient.h"
#include "msg/Messenger.h"
#include "osdc/Objecter.h"
#include "common/LogClient.h"
#include "common/Finisher.h"
#include "mgr/ActivePyModules.h"
#include "mgr/PyModule.h"
#include "mgr/PyModuleRegistry.h"
#include "mgr/DaemonServer.h"
#include "messages/MMgrReport.h"
#include "mgr/PerfCounterInstance.h"
#include "mgr/mgr_perf_counters.h"

#include "global/global_init.h"
#include "common/common_init.h"
#include "common/config.h"

#define dout_subsys ceph_subsys_mgr

namespace bs = boost::system;
namespace ca = ceph::async;

/**
 * GIL-Safe Test Fixture for ActivePyModules
 * 
 * This fixture provides the same infrastructure as TestMgr but with
 * explicit GIL management to avoid deadlocks when testing functions
 * that use without_gil_t pattern.
 * 
 * Usage:
 *   TEST_F(TestMgrNoGil, FunctionName) {
 *     // For functions that DON'T use without_gil:
 *     PyObject* result = active_modules->get_rocksdb_version();
 *     
 *     // For functions that DO use without_gil:
 *     PyObject* result = call_with_gil_release([this]() {
 *       return active_modules->list_servers_python();
 *     });
 *   }
 */
class TestMgrNoGil : public ::testing::Test {
public:
  /**
   * Initialize Ceph context once for all tests
   */
  static void SetUpTestSuite() {
    cct = g_ceph_context;
    if (!cct) {
      std::vector<const char*> args = {"unittest_mgr_nogil"};
      cct = global_init(
          nullptr, args, CEPH_ENTITY_TYPE_CLIENT, CODE_ENVIRONMENT_UTILITY,
          CINIT_FLAG_NO_DEFAULT_CONFIG_FILE);

      for (auto key : {"auth_client_required",
                       "auth_cluster_required",
                       "auth_service_required"}) {
        cct->_conf.set_val(key, "none");
      }
      cct->_conf.apply_changes(nullptr);

      common_init_finish(cct.get());
    }

    // Initialize Python once
    if (!Py_IsInitialized()) {
      Py_Initialize();
    }
  }

  /**
   * Set up test infrastructure
   * 
   * Creates all the components needed by ActivePyModules:
   * - MonClient, Objecter, Messenger
   * - ClusterState, DaemonStateIndex
   * - PyModuleRegistry, DaemonServer
   * - ActivePyModules instance
   */
  void SetUp() override {
    // Create async I/O context pool
    icp = std::make_unique<ca::io_context_pool>(1);
    
    // Create MonClient
    mc = std::make_unique<MonClient>(cct.get(), *icp);
    
    // Create Messenger
    messenger.reset(
        Messenger::create_client_messenger(cct.get(), "unittest_mgr_nogil"));
    
    // Create Objecter
    objecter = std::make_unique<Objecter>(
        cct.get(), messenger.get(), mc.get(), *icp);
    ceph_assert(objecter != nullptr);
    objecter->set_client_incarnation(0);
    objecter->init();

    // Create ClusterState
    cs = std::make_unique<ClusterState>(
        mc.get(), objecter.get(), mgr_map);

    // Initialize perfcounters for cache metrics
    mgr_perf_start(cct.get());

    // Create log channels
    clog = std::make_shared<LogChannel>(cct.get(), nullptr, "cluster");
    audit_clog = std::make_shared<LogChannel>(cct.get(), nullptr, "audit");
    
    // Create DaemonStateIndex
    daemon_state = std::make_unique<DaemonStateIndex>();
    
    // Create PyModuleConfig and PyModuleRegistry
    py_config = std::make_unique<PyModuleConfig>();
    py_module_registry = std::make_unique<PyModuleRegistry>(clog);
    
    // Create and start Finisher
    finisher = std::make_unique<Finisher>(cct.get(), "test_finisher", "test_fin");
    finisher->start();
    
    // Create DaemonServer
    daemon_server = std::make_unique<DaemonServer>(
        mc.get(), *finisher, *daemon_state, *cs, *py_module_registry, 
        clog, audit_clog);
    
    // Create ActivePyModules
    std::map<std::string, std::string> store_data;
    active_modules = std::make_unique<ActivePyModules>(
        *py_config, store_data, false, *daemon_state, *cs, *mc,
        clog, audit_clog, *objecter, *finisher, *daemon_server, *py_module_registry);
  }

  /**
   * Clean up test infrastructure
   * 
   * Tears down all components in reverse order of creation
   */
  void TearDown() override {
    // Clean up in reverse order
    active_modules.reset();
    daemon_server.reset();
    
    if (finisher) {
      finisher->stop();
      finisher.reset();
    }
    
    py_module_registry.reset();
    py_config.reset();
    daemon_state.reset();
    cs.reset();

    // Shutdown network components
    if (objecter) {
      objecter->shutdown();
      objecter.reset();
    }
    
    if (mc) {
      mc->shutdown();
      mc.reset();
    }
    
    if (messenger) {
      messenger->shutdown();
      messenger->wait();
      messenger.reset();
    }
    
    icp.reset();
    
    // Stop perfcounters
    mgr_perf_stop(cct.get());
  }

  /**
   * Helper: Call a function with GIL released
   * 
   * Use this for ActivePyModules functions that use without_gil_t pattern.
   * The function will:
   * 1. Release the GIL
   * 2. Call your function (which acquires GIL internally)
   * 3. Re-acquire the GIL
   * 4. Return the result
   * 
   * Example:
   *   PyObject* result = call_with_gil_release([this]() {
   *     return active_modules->list_servers_python();
   *   });
   */
  template<typename Func>
  auto call_with_gil_release(Func&& func) -> decltype(func()) {
    PyThreadState* save = PyEval_SaveThread();  // Release GIL
    auto result = func();                        // Call function (acquires GIL internally)
    PyEval_RestoreThread(save);                  // Re-acquire GIL
    return result;
  }

  /**
   * Helper: Call a void function with GIL released
   * 
   * Same as call_with_gil_release but for functions that return void.
   */
  template<typename Func>
  void call_void_with_gil_release(Func&& func) {
    PyThreadState* save = PyEval_SaveThread();  // Release GIL
    func();                                      // Call function
    PyEval_RestoreThread(save);                  // Re-acquire GIL
  }

  /**
   * Helper: Add a daemon to DaemonStateIndex
   * 
   * Useful for testing functions that need daemon data.
   * 
   * Example:
   *   add_test_daemon("osd", "1", {{"hostname", "test-host"}});
   */
  void add_test_daemon(
      const std::string& svc_type,
      const std::string& svc_id,
      const std::map<std::string, std::string>& metadata = {}) {
    DaemonKey key(svc_type, svc_id);
    auto daemon = std::make_shared<DaemonState>(daemon_state->types);
    daemon->key = key;
    daemon->metadata = metadata;
    daemon_state->insert(daemon);
  }

  /**
   * Helper: Verify Python object is a string with expected value
   */
  static void expect_py_string(PyObject* obj, const char* expected) {
    ASSERT_NE(obj, nullptr) << "Python object is NULL";
    ASSERT_TRUE(PyUnicode_Check(obj)) << "Python object is not a string";
    EXPECT_STREQ(PyUnicode_AsUTF8(obj), expected);
  }

  /**
   * Helper: Verify Python object is a dict with expected key
   */
  static void expect_py_dict_has_key(PyObject* dict, const char* key) {
    ASSERT_NE(dict, nullptr) << "Python dict is NULL";
    ASSERT_TRUE(PyDict_Check(dict)) << "Python object is not a dict";
    PyObject* item = PyDict_GetItemString(dict, key);
    ASSERT_NE(item, nullptr) << "Dict missing key: " << key;
  }

  /**
   * Helper: Verify Python object is a list with expected size
   */
  static void expect_py_list_size(PyObject* list, Py_ssize_t expected_size) {
    ASSERT_NE(list, nullptr) << "Python list is NULL";
    ASSERT_TRUE(PyList_Check(list)) << "Python object is not a list";
    EXPECT_EQ(PyList_Size(list), expected_size);
  }

protected:
  // Static members (shared across all tests)
  static inline boost::intrusive_ptr<CephContext> cct;

  // Per-test members (recreated for each test)
  std::unique_ptr<ca::io_context_pool> icp;
  std::unique_ptr<Messenger> messenger;
  std::unique_ptr<Objecter> objecter;
  std::unique_ptr<MonClient> mc;
  std::unique_ptr<ClusterState> cs;
  std::unique_ptr<DaemonStateIndex> daemon_state;
  std::unique_ptr<PyModuleConfig> py_config;
  std::unique_ptr<PyModuleRegistry> py_module_registry;
  std::unique_ptr<Finisher> finisher;
  std::unique_ptr<DaemonServer> daemon_server;
  std::unique_ptr<ActivePyModules> active_modules;
  
  LogChannelRef clog;
  LogChannelRef audit_clog;
  MgrMap mgr_map;
};

/**
 * Helper class to access protected members of ActivePyModule for testing
 * 
 * This class exposes protected members of ActivePyModule so tests can
 * directly set and get the Python class instance for testing purposes.
 */
class ActivePyModuleTestHelper : public ActivePyModule {
public:
  ActivePyModuleTestHelper(const PyModuleRef &py_module_, LogChannelRef clog_)
    : ActivePyModule(py_module_, clog_) {}
  
  void set_class_instance(PyObject* instance) {
    pClassInstance = instance;
  }
  
  PyObject* get_class_instance() const {
    return pClassInstance;
  }
};

/**
 * Test fixture for GIL-sensitive ActivePyModule function tests
 * 
 * This fixture provides minimal infrastructure for testing individual
 * ActivePyModule methods (notify, notify_clog) that are GIL-sensitive.
 * Unlike TestMgrNoGil which provides full Ceph infrastructure, this
 * fixture focuses on testing module-level behavior with explicit GIL control.
 * 
 * Key differences from TestMgrNoGil:
 * - Minimal setup (no MonClient, Objecter, DaemonServer)
 * - Tests individual ActivePyModule methods
 * - Intentionally leaks objects to avoid destructor GIL issues
 * - Manual GIL management with PyEval_SaveThread/RestoreThread
 * 
 * Usage:
 *   TEST_F(ActivePyModulesNoGilTest, Notify_ValidNotification) {
 *     auto module = create_test_module("test_module", module_code);
 *     PyThreadState* save = PyEval_SaveThread();  // Release GIL
 *     module->notify("osd_map", "12345");
 *     PyEval_RestoreThread(save);  // Re-acquire GIL
 *   }
 */
class ActivePyModulesNoGilTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    // Initialize Python interpreter once for all tests
    if (!Py_IsInitialized()) {
      Py_Initialize();
      // PyEval_InitThreads() is deprecated in Python 3.9+ and no longer needed
      // The GIL is automatically initialized with Py_Initialize()
    }
    
    // Initialize Ceph context if not already done
    if (!g_ceph_context) {
      std::vector<const char*> args = {"test_activepymodules_nogil"};
      cct = global_init(
          nullptr, args, CEPH_ENTITY_TYPE_CLIENT, CODE_ENVIRONMENT_UTILITY,
          CINIT_FLAG_NO_DEFAULT_CONFIG_FILE);
      
      cct->_conf.set_val("auth_client_required", "none");
      cct->_conf.set_val("auth_cluster_required", "none");
      cct->_conf.set_val("auth_service_required", "none");
      cct->_conf.apply_changes(nullptr);
      
      common_init_finish(cct.get());
    } else {
      cct = g_ceph_context;
    }
  }
  
  static void TearDownTestSuite() {
    // Note: We don't call Py_Finalize() here as it can cause issues
    // with subsequent tests. The Python interpreter will be cleaned up
    // when the process exits.
  }
  
  void SetUp() override {
    // GIL is already held by Python environment from SetUpTestSuite
    // No need to acquire it again
  }
  
  void TearDown() override {
    // GIL will be released by Python environment in TearDownTestSuite
    // No need to release it here
  }
  
  /**
   * Helper: Create a simple Python module with a test method
   * 
   * Returns raw pointer - INTENTIONALLY LEAKED to avoid destructor GIL issues.
   * This is acceptable in tests as the objects are small and tests are short-lived.
   * 
   * @param module_name Name of the Python module to create
   * @param module_code Python code defining the Module class
   * @return Raw pointer to ActivePyModuleTestHelper (intentionally leaked)
   */
  ActivePyModuleTestHelper* create_test_module(
      const std::string& module_name,
      const char* module_code) {
    
    // Create a Python module
    PyObject* test_module = PyModule_New(module_name.c_str());
    if (!test_module) {
      PyErr_Print();
      return nullptr;
    }
    
    PyObject* module_dict = PyModule_GetDict(test_module);
    if (!module_dict) {
      Py_DECREF(test_module);
      return nullptr;
    }
    
    // Execute the module code to define the Module class
    PyObject* result = PyRun_String(module_code, Py_file_input, module_dict, module_dict);
    if (!result) {
      PyErr_Print();
      Py_DECREF(test_module);
      return nullptr;
    }
    Py_DECREF(result);
    
    // Get the Module class
    PyObject* module_class = PyDict_GetItemString(module_dict, "Module");
    if (!module_class) {
      Py_DECREF(test_module);
      return nullptr;
    }
    
    // Create a PyModule wrapper - use raw pointer to avoid shared_ptr destructor
    auto py_module_ref = std::make_shared<PyModule>(module_name);
    py_module_ref->pClass = module_class;
    Py_INCREF(module_class);
    
    // Initialize pickle module for dispatch_remote support
    py_module_ref->pPickleModule = PyImport_ImportModule("pickle");
    if (!py_module_ref->pPickleModule) {
      PyErr_Print();
      Py_DECREF(test_module);
      return nullptr;
    }
    
    // Set thread state while GIL is held
    py_module_ref->pMyThreadState.set(PyThreadState_Get());
    
    // Create log channel
    auto clog = std::make_shared<LogChannel>(cct.get(), nullptr, "cluster");
    
    // Create an ActivePyModuleTestHelper - RAW POINTER, intentionally leaked
    auto active_module = new ActivePyModuleTestHelper(py_module_ref, clog);
    
    // Instantiate the Python class
    PyObject* instance = PyObject_CallObject(module_class, nullptr);
    if (!instance) {
      PyErr_Print();
      Py_DECREF(test_module);
      delete active_module;
      return nullptr;
    }
    
    // Set the instance using our helper's public method
    active_module->set_class_instance(instance);
    
    Py_DECREF(test_module);
    // Return raw pointer - will be intentionally leaked
    return active_module;
  }
  
protected:
  static boost::intrusive_ptr<CephContext> cct;
};

// Made with Bob