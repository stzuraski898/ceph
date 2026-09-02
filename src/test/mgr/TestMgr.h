// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <Python.h>

#include <cassert>

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
#include "mgr/MgrSession.h"
#include "mgr/PyModule.h"
#include "mgr/PyModuleRegistry.h"
#include "mgr/DaemonServer.h"
#include "messages/MMgrReport.h"
#include "mgr/PerfCounterInstance.h"
#include "mgr/mgr_perf_counters.h"

#include "global/global_init.h"
#include "common/common_init.h"
#include "common/config.h"

#define dout_subsys ceph_subsys_client

namespace bs = boost::system;
namespace ca = ceph::async;

class ClusterStateTestHelper : public ClusterState {
public:
  ClusterStateTestHelper(
      MonClient* mc_,
      Objecter* objecter_,
      const MgrMap& mgrmap_) :
    ClusterState(mc_, objecter_, mgrmap_)
  {}

  const PGMap::Incremental&
  test_get_pending_inc() const
  {
    return pending_inc;
  }

  const std::map<int64_t, unsigned>&
  test_get_existing_pools() const
  {
    return existing_pools;
  }

  const PGMap&
  test_get_pg_map() const
  {
    return pg_map;
  }

  Objecter*
  test_get_objecter() const
  {
    return objecter;
  }
};

class TestMgr : public ::testing::Test {
public:
  static void
  SetUpTestSuite()
  {
    if (g_ceph_context) {
      cct = g_ceph_context;
    }
    else
    {
      std::vector<const char*> args = {"unittest_mgr"};
      cct = global_init(
          nullptr, args, CEPH_ENTITY_TYPE_CLIENT, CODE_ENVIRONMENT_UTILITY,
          CINIT_FLAG_NO_DEFAULT_CONFIG_FILE);

      cct->_conf.set_val("auth_client_required", "none");
      cct->_conf.set_val("auth_cluster_required", "none");
      cct->_conf.set_val("auth_service_required", "none");
      cct->_conf.apply_changes(nullptr);

      common_init_finish(cct.get());
    }
  }

  void
  SetUp() override
  {
    icp = std::make_unique<ca::io_context_pool>(1);
    mc = std::make_unique<MonClient>(cct.get(), *icp);
    messenger.reset(
        Messenger::create_client_messenger(cct.get(), "unittest_mgr"));
    objecter =
        std::make_unique<Objecter>(cct.get(), messenger.get(), mc.get(), *icp);

    ceph_assert(objecter != nullptr);
    objecter->set_client_incarnation(0);
    objecter->init();

    cs = std::make_unique<ClusterStateTestHelper>(
        mc.get(), objecter.get(), mgr_map);
  }

  void
  TearDown() override
  {
    ceph_assert(objecter != nullptr);
    ceph_assert(mc != nullptr);
    objecter->shutdown();
    mc->shutdown();
    messenger->shutdown();
    messenger->wait();

    cs.reset();
    objecter.reset();
    mc.reset();
    messenger.reset();
    icp.reset();
  }

protected:
  static inline boost::intrusive_ptr<CephContext> cct;
  std::unique_ptr<ClusterStateTestHelper> cs;
  std::unique_ptr<ca::io_context_pool> icp;
  std::unique_ptr<Messenger> messenger;
  std::unique_ptr<Objecter> objecter;
  std::unique_ptr<MonClient> mc;
  MgrMap mgr_map;
  OSDMap osd_map;
};

class ActivePyModulesTestHelper : public ActivePyModules {
public:
  ActivePyModulesTestHelper(
      PyModuleConfig &module_config_,
      std::map<std::string, std::string> store_data,
      bool mon_provides_kv_sub,
      DaemonStateIndex &ds, ClusterState &cs,
      MonClient &mc, LogChannelRef clog_, LogChannelRef audit_clog_,
      Objecter &objecter_, Finisher &f, DaemonServer &server,
      PyModuleRegistry &pmr)
    : ActivePyModules(module_config_, store_data, mon_provides_kv_sub,
                      ds, cs, mc, clog_, audit_clog_, objecter_, f, server, pmr)
  {}

  // Expose modules map for testing
  void test_insert_module(const std::string& name, std::shared_ptr<ActivePyModule> module) {
    modules[name] = module;
  }

  void test_erase_module(const std::string& name) {
    // Erase module - caller must ensure GIL is NOT held
    // because ActivePyModule destructor will try to acquire it
    modules.erase(name);
  }
  
  // Test-specific method_exists that assumes GIL is already held
  // (called from test code that has TestGILGuard)
  bool test_method_exists(const std::string& module_name, const std::string& method_name) const {
    auto module_iter = modules.find(module_name);
    if (module_iter == modules.end()) {
      return false;
    }
    
    auto& module = module_iter->second;
    
    // Check if method exists (GIL already held by test)
    auto boundMethod = PyObject_GetAttrString(module->pClassInstance, method_name.c_str());
    if (boundMethod == nullptr) {
      PyErr_Clear();
      return false;
    }
    
    Py_DECREF(boundMethod);
    return true;
  }
  
  // Expose have_local_config_map for tests that exercise the config_map path
  void test_set_have_local_config_map(bool v) {
    have_local_config_map = v;
  }

  // Helper methods for testing start_one()
  bool test_is_pending(const std::string& name) const {
    std::lock_guard l(lock);
    return pending_modules.count(name) > 0;
  }
  
  size_t test_pending_count() const {
    std::lock_guard l(lock);
    return pending_modules.size();
  }
  
  const std::set<std::string, std::less<>>& test_get_pending_modules() const {
    return pending_modules;
  }
  
  // Helper methods for testing check_all_modules_started()
  void test_add_pending_module(const std::string& name) {
    std::lock_guard l(lock);
    pending_modules.insert(name);
  }
  
  void test_clear_pending_modules() {
    std::lock_guard l(lock);
    pending_modules.clear();
  }
  
  Context* test_get_recheck_context() const {
    std::lock_guard l(lock);
    return recheck_modules_start;
  }
  
  void test_set_recheck_context(Context* ctx) {
    std::lock_guard l(lock);
    recheck_modules_start = ctx;
  }
  
  // Helper methods for testing notify_all() race conditions
  std::shared_ptr<ActivePyModule> test_get_module(const std::string& name) {
    std::lock_guard l(lock);
    auto it = modules.find(name);
    return (it != modules.end()) ? it->second : nullptr;
  }
  
  void test_remove_module(const std::string& name) {
    std::lock_guard l(lock);
    modules.erase(name);
  }
  
  size_t test_module_count() const {
    std::lock_guard l(lock);
    return modules.size();
  }
  
  // Helper to manually add a module to the modules map for testing
  void test_add_module(const std::string& name, std::shared_ptr<ActivePyModule> module) {
    std::lock_guard l(lock);
    modules[name] = module;
  }

  // Mirrors the private get_config() logic; module_config is protected.
  // Returns true and sets *val when "mgr/<module>/<key>" exists in config.
  bool test_get_config(const std::string& module_name,
                       const std::string& key,
                       std::string* val) const {
    const std::string global_key = "mgr/" + module_name + "/" + key;
    std::lock_guard lk(module_config.lock);
    auto i = module_config.config.find(global_key);
    if (i != module_config.config.end()) {
      *val = i->second;
      return true;
    }
    return false;
  }
};

// Helper class to access protected members of ActivePyModule for testing
class ActivePyModuleTestHelper : public ActivePyModule {
public:
  ActivePyModuleTestHelper(const PyModuleRef &py_module_, LogChannelRef clog_)
    : ActivePyModule(py_module_, clog_) {}
  
  void set_class_instance(PyObject* instance) {
    pClassInstance = instance;
  }
};

class ActivePyModulesTest : public TestMgr {
public:
  // Helper methods for Python object validation
  static std::string dict_keys_to_string(PyObject* dict)
  {
    if (dict == nullptr || !PyDict_Check(dict)) {
      return "<not-a-dict>";
    }

    std::string keys;
    PyObject* key = nullptr;
    PyObject* value = nullptr;
    Py_ssize_t pos = 0;
    while (PyDict_Next(dict, &pos, &key, &value)) {
      const char* key_str = PyUnicode_Check(key) ? PyUnicode_AsUTF8(key) : "<non-unicode>";
      if (!keys.empty()) {
        keys += ", ";
      }
      keys += key_str ? key_str : "<null>";
    }

    return keys.empty() ? "<empty>" : keys;
  }

  static PyObject* lookup_dict_item(PyObject* dict, const char* key)
  {
    if (dict == nullptr || !PyDict_Check(dict)) {
      ADD_FAILURE() << "Expected a Python dict while looking up key '" << key << "'";
      return nullptr;
    }

    PyObject* item = PyDict_GetItemString(dict, key);
    if (item == nullptr) {
      ADD_FAILURE() << "Missing key '" << key << "'; available keys: ["
                    << dict_keys_to_string(dict) << "]";
    }
    return item;
  }

  static void expect_counter_metadata(PyObject* counters,
                                     const char* counter_name,
                                     const char* expected_description,
                                     const char* expected_nick,
                                     unsigned long expected_type)
  {
    PyObject* counter = lookup_dict_item(counters, counter_name);
    ASSERT_NE(counter, nullptr);
    ASSERT_TRUE(PyDict_Check(counter));

    PyObject* description = lookup_dict_item(counter, "description");
    ASSERT_NE(description, nullptr);
    ASSERT_TRUE(PyUnicode_Check(description));
    EXPECT_STREQ(PyUnicode_AsUTF8(description), expected_description);

    if (expected_nick != nullptr) {
      PyObject* nick = lookup_dict_item(counter, "nick");
      ASSERT_NE(nick, nullptr);
      ASSERT_TRUE(PyUnicode_Check(nick));
      EXPECT_STREQ(PyUnicode_AsUTF8(nick), expected_nick);
    }

    PyObject* type = lookup_dict_item(counter, "type");
    ASSERT_NE(type, nullptr);
    ASSERT_TRUE(PyLong_Check(type));
    EXPECT_EQ(PyLong_AsUnsignedLong(type), expected_type);

    ASSERT_NE(lookup_dict_item(counter, "priority"), nullptr);
    ASSERT_NE(lookup_dict_item(counter, "units"), nullptr);
  }

  void
  SetUp() override
  {
    TestMgr::SetUp();

    // Initialize perfcounters for cache metrics
    mgr_perf_start(cct.get());

    clog = std::make_shared<LogChannel>(cct.get(), nullptr, "cluster");
    audit_clog = std::make_shared<LogChannel>(cct.get(), nullptr, "audit");
    daemon_state = std::make_unique<DaemonStateIndex>();
    py_config = std::make_unique<PyModuleConfig>();
    py_module_registry = std::make_unique<PyModuleRegistry>(clog);
    finisher = std::make_unique<Finisher>(cct.get(), "test_finisher", "test_fin");
    finisher->start();
    daemon_server = std::make_unique<DaemonServer>(mc.get(), *finisher, *daemon_state, *cs, *py_module_registry, clog, audit_clog);
    std::map<std::string, std::string> store_data;
    active_modules = std::make_unique<ActivePyModulesTestHelper>(
      *py_config, store_data, false, *daemon_state, *cs, *mc,
      clog, audit_clog, *objecter, *finisher, *daemon_server, *py_module_registry);
  }

  void add_daemon_perf_counters(
    const std::string& svc_type,
    const std::string& svc_id,
    const std::map<std::string, PerfCounterType>& counter_types)
  {
    DaemonKey key(svc_type, svc_id);
    auto daemon = daemon_state->get(key);
    if(!daemon) {
      daemon = std::make_shared<DaemonState>(daemon_state->types);
      daemon->key = key;
      daemon_state->insert(daemon);
    }
    //Add counter types
    for(const auto& [name, type] : counter_types) {
      auto type_copy = type;

      if(type_copy.path.empty()) {
        type_copy.path = name;
      }
      daemon_state->types[name] = std::move(type_copy);
    }  

    //Init counter instances
    for(const auto& [name, type] : counter_types) {
      daemon->perf_counters.instances.emplace(name, 
          PerfCounterInstance(type.type));
    }
  }

  // Helper: Add perf counter data to an existing daemon
  void add_perf_counter_data(
    const std::string& svc_type,
    const std::string& svc_id,
    const std::string& counter_name,
    const std::vector<std::pair<utime_t, uint64_t>>& data_points)
  {
    DaemonKey key{svc_type, svc_id};
    auto daemon = daemon_state->get(key);
    ASSERT_NE(daemon, nullptr) << "Daemon " << svc_type << "." << svc_id << " not found";
    
    auto it = daemon->perf_counters.instances.find(counter_name);
    ASSERT_NE(it, daemon->perf_counters.instances.end())
      << "Counter " << counter_name << " not found";
    
    for (const auto& [time, value] : data_points) {
      it->second.push(time, value);
    }
  }

  // Helper: Add average perf counter data to an existing daemon
  void add_perf_counter_avg_data(
    const std::string& svc_type,
    const std::string& svc_id,
    const std::string& counter_name,
    const std::vector<std::tuple<utime_t, uint64_t, uint64_t>>& data_points)
  {
    DaemonKey key{svc_type, svc_id};
    auto daemon = daemon_state->get(key);
    ASSERT_NE(daemon, nullptr) << "Daemon " << svc_type << "." << svc_id << " not found";
    
    auto it = daemon->perf_counters.instances.find(counter_name);
    ASSERT_NE(it, daemon->perf_counters.instances.end())
      << "Counter " << counter_name << " not found";
    
    for (const auto& [time, sum, count] : data_points) {
      it->second.push_avg(time, sum, count);
    }
  }

  // Helper: Verify progress event exists with expected values
  void verify_progress_event(
    const std::string& event_id,
    const std::string& expected_message,
    float expected_progress,
    bool expected_add_to_ceph_s)
  {
    std::map<std::string, ProgressEvent> events;
    active_modules->get_progress_events(&events);
    
    auto it = events.find(event_id);
    ASSERT_NE(it, events.end()) << "Progress event '" << event_id << "' not found";
    EXPECT_EQ(it->second.message, expected_message)
      << "Event '" << event_id << "' has wrong message";
    EXPECT_FLOAT_EQ(it->second.progress, expected_progress)
      << "Event '" << event_id << "' has wrong progress";
    EXPECT_EQ(it->second.add_to_ceph_s, expected_add_to_ceph_s)
      << "Event '" << event_id << "' has wrong add_to_ceph_s flag";
  }

  // Helper: Verify progress event does not exist
  void verify_progress_event_absent(const std::string& event_id)
  {
    std::map<std::string, ProgressEvent> events;
    active_modules->get_progress_events(&events);
    
    EXPECT_EQ(events.find(event_id), events.end())
      << "Progress event '" << event_id << "' should not exist";
  }

  // Helper: Verify Python dict has expected key with value
  static void verify_py_dict_string(PyObject* dict, const char* key, const char* expected_value)
  {
    ASSERT_NE(dict, nullptr);
    ASSERT_TRUE(PyDict_Check(dict)) << "Expected Python dict";
    
    PyObject* item = PyDict_GetItemString(dict, key);
    ASSERT_NE(item, nullptr) << "Key '" << key << "' not found in dict";
    ASSERT_TRUE(PyUnicode_Check(item)) << "Value for key '" << key << "' is not a string";
    
    const char* actual = PyUnicode_AsUTF8(item);
    ASSERT_NE(actual, nullptr);
    EXPECT_STREQ(actual, expected_value) << "Value mismatch for key '" << key << "'";
  }

  // Helper: Verify Python dict has expected key with long value
  static void verify_py_dict_long(PyObject* dict, const char* key, long expected_value)
  {
    ASSERT_NE(dict, nullptr);
    ASSERT_TRUE(PyDict_Check(dict)) << "Expected Python dict";
    
    PyObject* item = PyDict_GetItemString(dict, key);
    ASSERT_NE(item, nullptr) << "Key '" << key << "' not found in dict";
    ASSERT_TRUE(PyLong_Check(item)) << "Value for key '" << key << "' is not a long";
    
    long actual = PyLong_AsLong(item);
    EXPECT_EQ(actual, expected_value) << "Value mismatch for key '" << key << "'";
  }

  // Helper: Verify Python dict contains key
  static void verify_py_dict_has_key(PyObject* dict, const char* key)
  {
    ASSERT_NE(dict, nullptr);
    ASSERT_TRUE(PyDict_Check(dict)) << "Expected Python dict";
    
    PyObject* py_key = PyUnicode_FromString(key);
    ASSERT_NE(py_key, nullptr);
    
    bool has_key = PyDict_Contains(dict, py_key) == 1;
    Py_DECREF(py_key);
    
    EXPECT_TRUE(has_key) << "Key '" << key << "' not found in dict";
  }

  // Helper: Verify Python dict does not contain key
  static void verify_py_dict_lacks_key(PyObject* dict, const char* key)
  {
    ASSERT_NE(dict, nullptr);
    ASSERT_TRUE(PyDict_Check(dict)) << "Expected Python dict";
    
    PyObject* py_key = PyUnicode_FromString(key);
    ASSERT_NE(py_key, nullptr);
    
    bool has_key = PyDict_Contains(dict, py_key) == 1;
    Py_DECREF(py_key);
    
    EXPECT_FALSE(has_key) << "Key '" << key << "' should not be in dict";
  }

  // Helper: Verify Python dict size
  static void verify_py_dict_size(PyObject* dict, Py_ssize_t expected_size)
  {
    ASSERT_NE(dict, nullptr);
    ASSERT_TRUE(PyDict_Check(dict)) << "Expected Python dict";
    
    Py_ssize_t actual_size = PyDict_Size(dict);
    EXPECT_EQ(actual_size, expected_size) << "Dict size mismatch";
  }

  // Helper: Verify Python list size
  static void verify_py_list_size(PyObject* list, Py_ssize_t expected_size)
  {
    ASSERT_NE(list, nullptr);
    ASSERT_TRUE(PyList_Check(list)) << "Expected Python list";
    
    Py_ssize_t actual_size = PyList_Size(list);
    EXPECT_EQ(actual_size, expected_size) << "List size mismatch";
  }

  // Helper to create and register a mock Python module for testing
  std::shared_ptr<ActivePyModule> create_test_module(const std::string& module_name, const char* module_code)
  {
    PyGILState_STATE gstate = PyGILState_Ensure();
    // Create a Python module
    PyObject* test_module = PyModule_New(module_name.c_str());
    if (!test_module) {
      PyGILState_Release(gstate);
      return nullptr;
    }
    
    PyObject* module_dict = PyModule_GetDict(test_module);
    if (!module_dict) {
      Py_DECREF(test_module);
      PyGILState_Release(gstate);
      return nullptr;
    }
    
    // Execute the module code to define the Module class
    PyObject* result = PyRun_String(module_code, Py_file_input, module_dict, module_dict);
    if (!result) {
      Py_DECREF(test_module);
      PyGILState_Release(gstate);
      return nullptr;
    }
    Py_DECREF(result);
    
    // Get the Module class
    PyObject* module_class = PyDict_GetItemString(module_dict, "Module");
    if (!module_class) {
      Py_DECREF(test_module);
      PyGILState_Release(gstate);
      return nullptr;
    }
    
    // Create a PyModule wrapper with custom deleter so PyModule::~PyModule won't do unsafe Gil/EndInterpreter
    auto py_module_ref = std::shared_ptr<PyModule>(
      new PyModule(module_name),
      [](PyModule* m) {
        if (m) {
          PyGILState_STATE gs = PyGILState_Ensure();
          Py_XDECREF(m->pClass);
          m->pClass = nullptr;
          Py_XDECREF(m->pStandbyClass);
          m->pStandbyClass = nullptr;
          Py_XDECREF(m->pPickleModule);
          m->pPickleModule = nullptr;
          m->pMyThreadState.ts = nullptr;
          PyGILState_Release(gs);
          delete m;
        }
      });
    py_module_ref->pClass = module_class;
    Py_INCREF(module_class);
    py_module_ref->pPickleModule = PyImport_ImportModule("pickle");
    py_module_ref->pMyThreadState.set(PyThreadState_Get());
    py_module_ref->use_main_interpreter = false;
    
    // Create an ActivePyModule with custom deleter that cleans up pClassInstance under GIL
    auto active_module = std::shared_ptr<ActivePyModule>(
      new ActivePyModule(py_module_ref, clog),
      [](ActivePyModule* m) {
        if (m) {
          if (m->pClassInstance) {
            PyGILState_STATE gs = PyGILState_Ensure();
            Py_DECREF(m->pClassInstance);
            m->pClassInstance = nullptr;
            PyGILState_Release(gs);
          }
          delete m;
        }
      });
    
    // Instantiate the Python class
    active_module->pClassInstance = PyObject_CallObject(module_class, nullptr);
    if (!active_module->pClassInstance) {
      Py_DECREF(test_module);
      PyGILState_Release(gstate);
      return nullptr;
    }
    
    Py_DECREF(test_module);
    PyGILState_Release(gstate);
    return active_module;
  }

  // Helper: Create a test ModuleCommand
  static ModuleCommand create_test_command(const std::string& module_name) {
    ModuleCommand cmd;
    cmd.module_name = module_name;
    cmd.cmdstring = "test command";
    cmd.helpstring = "test help";
    cmd.perm = "rw";
    cmd.polling = false;
    return cmd;
  }

  // Helper: Create a test MgrSession
  static MgrSessionRef create_test_session(CephContext* cct) {
    auto session = ceph::make_ref<MgrSession>(cct);
    session->global_id = 12345;
    session->entity_name.set_type(CEPH_ENTITY_TYPE_CLIENT);
    return session;
  }

  void
  TearDown() override
  {
    // Destroy in reverse order of construction to ensure proper cleanup
    // DaemonServer must be destroyed before finisher since it depends on it
    if(active_modules)
    {
      // ~ActivePyModules() calls cmd_finisher.stop() — do not call it here
      // too, as double-stopping a Finisher whose thread_id is 0 aborts.
      active_modules.reset();
    }
    
    // Destroy daemon_server before finisher - this ensures the timer thread
    // (OpHistorySvc) stops before we destroy the finisher it may depend on
    daemon_server.reset();
    
    py_module_registry.reset();
    
    if(finisher)
    {
      finisher->stop();
      finisher.reset();
    }
    
    py_config.reset();
    daemon_state.reset();
    audit_clog.reset();
    clog.reset();

    // Clean up perfcounters
    mgr_perf_stop(cct.get());

    TestMgr::TearDown();
  }

protected:
  LogChannelRef clog;
  LogChannelRef audit_clog;
  std::unique_ptr<DaemonStateIndex> daemon_state;
  std::unique_ptr<PyModuleConfig> py_config;
  std::unique_ptr<PyModuleRegistry> py_module_registry;
  std::unique_ptr<DaemonServer> daemon_server;
  std::unique_ptr<Finisher> finisher;
  
  // Helper to create a secondary ActivePyModules instance for testing destruction
  // This allows us to test the race condition by destroying the instance while
  // callbacks are pending in the finisher queue
  std::unique_ptr<ActivePyModulesTestHelper> create_secondary_active_modules() {
    return std::make_unique<ActivePyModulesTestHelper>(
      *py_config, std::map<std::string, std::string>{}, false,
      *daemon_state, *cs, *mc, clog, audit_clog, *objecter,
      *finisher, *daemon_server, *py_module_registry);
  }
  std::unique_ptr<ActivePyModulesTestHelper> active_modules;
};

class ClusterStateTest : public TestMgr {
public:
  ceph::ref_t<DeviceState> device;

  void
  SetUp() override
  {
    TestMgr::SetUp();

    //Setup pools and notify osdmap
    pool.set_pg_num(2);
    osd_inc.new_pool_max = 1;
    osd_inc.new_pools[1] = pool;
    osd_map.apply_incremental(osd_inc);

    cs->with_osdmap_and_pgmap([&](const OSDMap& old_map, const PGMap& pg_map) {
      cs->notify_osdmap(osd_map);
    });

    stats->pool_stat[1] = store_statfs_t{};
    stats->set_src(entity_name_t::OSD(0));
    stats->osd_stat.seq = 1;
    pgstat.state = PG_STATE_ACTIVE;
    pgstat.reported_epoch = 1;
    pgstat.reported_seq = 2;
  }

  void
  ingest_and_pginc()
  {
    cs->ingest_pgstats(stats);
    p_inc = cs->test_get_pending_inc();
  }

protected:
  const mempool::osdmap::map<int64_t, pg_pool_t>& pools = osd_map.get_pools();
  OSDMap::Incremental osd_inc = OSDMap::Incremental(osd_map.get_epoch() + 1);
  ceph::ref_t<MPGStats> stats = ceph::make_ref<MPGStats>();
  PGMap::Incremental p_inc;
  pg_stat_t pgstat;
  pg_pool_t pool;
};

class DeviceStateTest : public ::testing::Test {
public:
  ceph::ref_t<DeviceState> device;

  void
  SetUp() override
  {
    device = ceph::make_ref<DeviceState>("test_device_111");
  }
};

struct PythonEnv : public ::testing::Environment {
  void
  SetUp() override
  {
    Py_Initialize();
    // Explicitly initialize threading to help helgrind understand the GIL
    // In Python 3.7+, PyEval_InitThreads() is deprecated and does nothing,
    // but calling it helps helgrind's analysis by making thread initialization explicit
#if PY_VERSION_HEX < 0x03090000
    // For Python < 3.9, explicitly call PyEval_InitThreads
    if (!PyEval_ThreadsInitialized()) {
      PyEval_InitThreads();
    }
#endif
    // Ensure we have a thread state for helgrind to track
    PyThreadState* tstate = PyThreadState_Get();
    if (tstate) {
      // Release and re-acquire GIL to establish clear ownership for helgrind
      PyEval_SaveThread();
      PyEval_RestoreThread(tstate);
    }
  }

  void
  TearDown() override
  {
    Py_Finalize();
  }
};

// Made with Bob
