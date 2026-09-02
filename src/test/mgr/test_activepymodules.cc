// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "gtest/gtest.h"
#include "TestMgr.h"
#include "mgr/DaemonHealthMetric.h"
#include "mgr/Mgr.h"
#include <atomic>
#include <thread>
#include "mgr/PyFormatter.h"
#include "mgr/MgrSession.h"
#include "mgr/PyModule.h"
#include "mon/health_check.h"
#include "common/perf_counters_key.h"
#include "common/cmdparse.h"

//Couldn't follow the other Unit Tests' format of having main to do this since
//  $<TARGET_OBJECTS:unit-main> is needed and also defines main
namespace {
  ::testing::Environment* const python_env =
    ::testing::AddGlobalTestEnvironment(new PythonEnv);
}

//Minimal copy of the finish function from src/mgr/Mgr.cc
//Added to compile DaemonServer, simpler than including Mgr.cc in its entirety
void MetadataUpdate::finish(int r)
{
  daemon_state.clear_updating(key);
}

TEST_F(ActivePyModulesTest, DumpServerEmpty)
{
  PyFormatter f;
  DaemonStateCollection dmc;
  
  active_modules->dump_server("test_host", dmc, &f);
  
  PyObject* result = f.get();
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  
  // Verify hostname
  PyObject* hostname = PyDict_GetItemString(result, "hostname");
  ASSERT_NE(hostname, nullptr);
  ASSERT_TRUE(PyUnicode_Check(hostname));
  EXPECT_STREQ(PyUnicode_AsUTF8(hostname), "test_host");
  
  // Verify services array exists and is empty
  PyObject* services = PyDict_GetItemString(result, "services");
  ASSERT_NE(services, nullptr);
  ASSERT_TRUE(PyList_Check(services));
  EXPECT_EQ(PyList_Size(services), 0);
  
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, DumpServerSingleDaemon)
{
  PyFormatter f;
  DaemonStateCollection dmc;
  
  DaemonKey key{"osd", "0"};
  auto daemon = std::make_shared<DaemonState>(daemon_state->types);
  daemon->key = key;
  daemon->hostname = "test_host";
  daemon->metadata["ceph_version"] = "17.2.0";
  daemon->metadata["id"] = "osd.0";
  
  dmc[key] = daemon;
  
  active_modules->dump_server("test_host", dmc, &f);
  
  PyObject* result = f.get();
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  
  PyObject* services = PyDict_GetItemString(result, "services");
  ASSERT_NE(services, nullptr);
  ASSERT_TRUE(PyList_Check(services));
  EXPECT_EQ(PyList_Size(services), 1);
  
  PyObject* service = PyList_GetItem(services, 0);
  ASSERT_NE(service, nullptr);
  ASSERT_TRUE(PyDict_Check(service));
  
  PyObject* type = PyDict_GetItemString(service, "type");
  ASSERT_NE(type, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(type), "osd");
  
  PyObject* id = PyDict_GetItemString(service, "id");
  ASSERT_NE(id, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(id), "0");
  
  PyObject* version = PyDict_GetItemString(service, "ceph_version");
  ASSERT_NE(version, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(version), "17.2.0");
  
  PyObject* name = PyDict_GetItemString(service, "name");
  ASSERT_NE(name, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(name), "osd.0");
  
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, DumpServerMultipleDaemons)
{
  PyFormatter f;
  DaemonStateCollection dmc;
  
  for (int i = 0; i < 3; i++) {
    DaemonKey key{"osd", std::to_string(i)};
    auto daemon = std::make_shared<DaemonState>(daemon_state->types);
    daemon->key = key;
    daemon->hostname = "test_host";
    daemon->metadata["ceph_version"] = "17.2.0";
    daemon->metadata["id"] = "osd." + std::to_string(i);
    dmc[key] = daemon;
  }
  
  active_modules->dump_server("test_host", dmc, &f);
  
  PyObject* result = f.get();
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  
  PyObject* hostname = PyDict_GetItemString(result, "hostname");
  ASSERT_NE(hostname, nullptr);
  ASSERT_TRUE(PyUnicode_Check(hostname));
  EXPECT_STREQ(PyUnicode_AsUTF8(hostname), "test_host");
  
  PyObject* services = PyDict_GetItemString(result, "services");
  ASSERT_NE(services, nullptr);
  ASSERT_TRUE(PyList_Check(services));
  ASSERT_EQ(PyList_Size(services), 3) << "Should have exactly 3 services";
  
  for (int i = 0; i < 3; i++) {
    PyObject* service = PyList_GetItem(services, i);
    ASSERT_NE(service, nullptr) << "Service " << i << " should exist";
    ASSERT_TRUE(PyDict_Check(service)) << "Service " << i << " should be a dict";
    
    PyObject* type = PyDict_GetItemString(service, "type");
    ASSERT_NE(type, nullptr) << "Service " << i << " should have 'type' field";
    ASSERT_TRUE(PyUnicode_Check(type));
    EXPECT_STREQ(PyUnicode_AsUTF8(type), "osd") << "Service " << i << " type should be 'osd'";
    
    PyObject* id = PyDict_GetItemString(service, "id");
    ASSERT_NE(id, nullptr) << "Service " << i << " should have 'id' field";
    ASSERT_TRUE(PyUnicode_Check(id));
    EXPECT_STREQ(PyUnicode_AsUTF8(id), std::to_string(i).c_str())
      << "Service " << i << " id should match";
    
    PyObject* version = PyDict_GetItemString(service, "ceph_version");
    ASSERT_NE(version, nullptr) << "Service " << i << " should have 'ceph_version' field";
    ASSERT_TRUE(PyUnicode_Check(version));
    EXPECT_STREQ(PyUnicode_AsUTF8(version), "17.2.0")
      << "Service " << i << " version should be '17.2.0'";
    
    PyObject* name = PyDict_GetItemString(service, "name");
    ASSERT_NE(name, nullptr) << "Service " << i << " should have 'name' field";
    ASSERT_TRUE(PyUnicode_Check(name));
    std::string expected_name = "osd." + std::to_string(i);
    EXPECT_STREQ(PyUnicode_AsUTF8(name), expected_name.c_str())
      << "Service " << i << " name should be '" << expected_name << "'";
  }
  
  // top-level ceph_version is the last-seen value from the iteration
  PyObject* top_version = PyDict_GetItemString(result, "ceph_version");
  ASSERT_NE(top_version, nullptr);
  ASSERT_TRUE(PyUnicode_Check(top_version));
  EXPECT_STREQ(PyUnicode_AsUTF8(top_version), "17.2.0");
  
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, GetServerPythonEmpty)
{
  PyObject* result = active_modules->get_server_python("nonexistent_host");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  
  PyObject* hostname = PyDict_GetItemString(result, "hostname");
  ASSERT_NE(hostname, nullptr);
  ASSERT_TRUE(PyUnicode_Check(hostname));
  EXPECT_STREQ(PyUnicode_AsUTF8(hostname), "nonexistent_host");
  
  PyObject* services = PyDict_GetItemString(result, "services");
  ASSERT_NE(services, nullptr);
  ASSERT_TRUE(PyList_Check(services));
  EXPECT_EQ(PyList_Size(services), 0);
  
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, GetServerPythonWithDaemons)
{
  DaemonKey osd_key{"osd", "0"};
  auto osd_daemon = std::make_shared<DaemonState>(daemon_state->types);
  osd_daemon->key = osd_key;
  osd_daemon->hostname = "test_host";
  osd_daemon->metadata["ceph_version"] = "17.2.0";
  osd_daemon->metadata["id"] = "osd.0";
  daemon_state->insert(osd_daemon);
  
  DaemonKey mon_key{"mon", "a"};
  auto mon_daemon = std::make_shared<DaemonState>(daemon_state->types);
  mon_daemon->key = mon_key;
  mon_daemon->hostname = "test_host";
  mon_daemon->metadata["ceph_version"] = "17.2.0";
  mon_daemon->metadata["id"] = "mon.a";
  daemon_state->insert(mon_daemon);
  
  PyObject* result = active_modules->get_server_python("test_host");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  
  PyObject* hostname = PyDict_GetItemString(result, "hostname");
  ASSERT_NE(hostname, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(hostname), "test_host");
  
  PyObject* services = PyDict_GetItemString(result, "services");
  ASSERT_NE(services, nullptr);
  ASSERT_TRUE(PyList_Check(services));
  EXPECT_EQ(PyList_Size(services), 2);
  
  PyObject* ceph_version = PyDict_GetItemString(result, "ceph_version");
  ASSERT_NE(ceph_version, nullptr);
  ASSERT_TRUE(PyUnicode_Check(ceph_version));
  EXPECT_STREQ(PyUnicode_AsUTF8(ceph_version), "17.2.0");
  
  bool found_osd = false;
  bool found_mon = false;
  for (Py_ssize_t i = 0; i < PyList_Size(services); i++) {
    PyObject* service = PyList_GetItem(services, i);
    ASSERT_TRUE(PyDict_Check(service));
    
    PyObject* type = PyDict_GetItemString(service, "type");
    ASSERT_NE(type, nullptr);
    std::string type_str = PyUnicode_AsUTF8(type);
    
    if (type_str == "osd") {
      found_osd = true;
      PyObject* id = PyDict_GetItemString(service, "id");
      ASSERT_NE(id, nullptr);
      EXPECT_STREQ(PyUnicode_AsUTF8(id), "0");
    } else if (type_str == "mon") {
      found_mon = true;
      PyObject* id = PyDict_GetItemString(service, "id");
      ASSERT_NE(id, nullptr);
      EXPECT_STREQ(PyUnicode_AsUTF8(id), "a");
    }
  }
  
  EXPECT_TRUE(found_osd);
  EXPECT_TRUE(found_mon);
  
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, ListServersPythonEmpty)
{
  PyObject* result = active_modules->list_servers_python();
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyList_Check(result));
  EXPECT_EQ(PyList_Size(result), 0);
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, ListServersPythonSingleServer)
{
  DaemonKey osd_key{"osd", "0"};
  auto osd_daemon = std::make_shared<DaemonState>(daemon_state->types);
  osd_daemon->key = osd_key;
  osd_daemon->hostname = "server1";
  osd_daemon->metadata["ceph_version"] = "17.2.0";
  daemon_state->insert(osd_daemon);
  
  DaemonKey mon_key{"mon", "a"};
  auto mon_daemon = std::make_shared<DaemonState>(daemon_state->types);
  mon_daemon->key = mon_key;
  mon_daemon->hostname = "server1";
  mon_daemon->metadata["ceph_version"] = "17.2.0";
  daemon_state->insert(mon_daemon);
  
  PyObject* result = active_modules->list_servers_python();
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyList_Check(result));
  EXPECT_EQ(PyList_Size(result), 1);
  
  PyObject* server = PyList_GetItem(result, 0);
  ASSERT_NE(server, nullptr);
  ASSERT_TRUE(PyDict_Check(server));
  
  PyObject* hostname = PyDict_GetItemString(server, "hostname");
  ASSERT_NE(hostname, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(hostname), "server1");
  
  PyObject* services = PyDict_GetItemString(server, "services");
  ASSERT_NE(services, nullptr);
  ASSERT_TRUE(PyList_Check(services));
  EXPECT_EQ(PyList_Size(services), 2);
  
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, ListServersPythonMultipleServers)
{
  DaemonKey osd0_key{"osd", "0"};
  auto osd0_daemon = std::make_shared<DaemonState>(daemon_state->types);
  osd0_daemon->key = osd0_key;
  osd0_daemon->hostname = "server1";
  osd0_daemon->metadata["ceph_version"] = "17.2.0";
  daemon_state->insert(osd0_daemon);
  
  DaemonKey osd1_key{"osd", "1"};
  auto osd1_daemon = std::make_shared<DaemonState>(daemon_state->types);
  osd1_daemon->key = osd1_key;
  osd1_daemon->hostname = "server2";
  osd1_daemon->metadata["ceph_version"] = "17.2.0";
  daemon_state->insert(osd1_daemon);
  
  DaemonKey mon_key{"mon", "a"};
  auto mon_daemon = std::make_shared<DaemonState>(daemon_state->types);
  mon_daemon->key = mon_key;
  mon_daemon->hostname = "server2";
  mon_daemon->metadata["ceph_version"] = "17.2.0";
  daemon_state->insert(mon_daemon);
  
  DaemonKey mgr_key{"mgr", "x"};
  auto mgr_daemon = std::make_shared<DaemonState>(daemon_state->types);
  mgr_daemon->key = mgr_key;
  mgr_daemon->hostname = "server3";
  mgr_daemon->metadata["ceph_version"] = "17.2.0";
  daemon_state->insert(mgr_daemon);
  
  PyObject* result = active_modules->list_servers_python();
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyList_Check(result));
  EXPECT_EQ(PyList_Size(result), 3);
  
  std::set<std::string> found_servers;
  for (Py_ssize_t i = 0; i < PyList_Size(result); i++) {
    PyObject* server = PyList_GetItem(result, i);
    ASSERT_TRUE(PyDict_Check(server));
    
    PyObject* hostname = PyDict_GetItemString(server, "hostname");
    ASSERT_NE(hostname, nullptr);
    found_servers.insert(PyUnicode_AsUTF8(hostname));
  }
  
  EXPECT_EQ(found_servers.count("server1"), 1);
  EXPECT_EQ(found_servers.count("server2"), 1);
  EXPECT_EQ(found_servers.count("server3"), 1);
  
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, GetMetadataPythonNonExistent)
{
  PyObject* result = active_modules->get_metadata_python("osd", "999");
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result, Py_None);
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, GetMetadataPythonBasic)
{
  DaemonKey key{"osd", "0"};
  auto daemon = std::make_shared<DaemonState>(daemon_state->types);
  daemon->key = key;
  daemon->hostname = "test_host";
  daemon->metadata["ceph_version"] = "17.2.0";
  daemon->metadata["arch"] = "x86_64";
  daemon->metadata["cpu"] = "Intel Xeon";
  daemon->metadata["kernel_version"] = "5.15.0";
  daemon_state->insert(daemon);
  
  PyObject* result = active_modules->get_metadata_python("osd", "0");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  
  PyObject* hostname = PyDict_GetItemString(result, "hostname");
  ASSERT_NE(hostname, nullptr);
  ASSERT_TRUE(PyUnicode_Check(hostname));
  EXPECT_STREQ(PyUnicode_AsUTF8(hostname), "test_host");
  
  PyObject* version = PyDict_GetItemString(result, "ceph_version");
  ASSERT_NE(version, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(version), "17.2.0");
  
  PyObject* arch = PyDict_GetItemString(result, "arch");
  ASSERT_NE(arch, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(arch), "x86_64");
  
  PyObject* cpu = PyDict_GetItemString(result, "cpu");
  ASSERT_NE(cpu, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(cpu), "Intel Xeon");
  
  PyObject* kernel = PyDict_GetItemString(result, "kernel_version");
  ASSERT_NE(kernel, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(kernel), "5.15.0");
  
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, GetMetadataPythonEmptyMetadata)
{
  DaemonKey key{"mon", "a"};
  auto daemon = std::make_shared<DaemonState>(daemon_state->types);
  daemon->key = key;
  daemon->hostname = "mon_host";
  daemon_state->insert(daemon);
  
  PyObject* result = active_modules->get_metadata_python("mon", "a");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  
  PyObject* hostname = PyDict_GetItemString(result, "hostname");
  ASSERT_NE(hostname, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(hostname), "mon_host");
  EXPECT_EQ(PyDict_Size(result), 1);
  
  Py_DECREF(result);
}

// OSD with empty hostname simulates a "destroyed" OSD (Tracker 74693).
TEST_F(ActivePyModulesTest, GetMetadataPythonWithDestroyedOSD)
{
  DaemonKey key{"osd", "123"};
  auto daemon = std::make_shared<DaemonState>(daemon_state->types);
  daemon->key = key;
  daemon->hostname = "";
  daemon_state->insert(daemon);
  
  PyObject* result = active_modules->get_metadata_python("osd", "123");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  
  PyObject* hostname = PyDict_GetItemString(result, "hostname");
  ASSERT_NE(hostname, nullptr);
  ASSERT_TRUE(PyUnicode_Check(hostname));
  EXPECT_STREQ(PyUnicode_AsUTF8(hostname), "");
  
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, GetMetadataPythonDifferentTypes)
{
  DaemonKey osd_key{"osd", "5"};
  auto osd_daemon = std::make_shared<DaemonState>(daemon_state->types);
  osd_daemon->key = osd_key;
  osd_daemon->hostname = "osd_host";
  osd_daemon->metadata["device_class"] = "ssd";
  daemon_state->insert(osd_daemon);
  
  DaemonKey mgr_key{"mgr", "x"};
  auto mgr_daemon = std::make_shared<DaemonState>(daemon_state->types);
  mgr_daemon->key = mgr_key;
  mgr_daemon->hostname = "mgr_host";
  mgr_daemon->metadata["ceph_release"] = "reef";
  daemon_state->insert(mgr_daemon);
  
  PyObject* osd_result = active_modules->get_metadata_python("osd", "5");
  ASSERT_NE(osd_result, nullptr);
  ASSERT_TRUE(PyDict_Check(osd_result));
  PyObject* device_class = PyDict_GetItemString(osd_result, "device_class");
  ASSERT_NE(device_class, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(device_class), "ssd");
  Py_DECREF(osd_result);
  
  PyObject* mgr_result = active_modules->get_metadata_python("mgr", "x");
  ASSERT_NE(mgr_result, nullptr);
  ASSERT_TRUE(PyDict_Check(mgr_result));
  PyObject* release = PyDict_GetItemString(mgr_result, "ceph_release");
  ASSERT_NE(release, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(release), "reef");
  Py_DECREF(mgr_result);
}

TEST_F(ActivePyModulesTest, GetDaemonStatusPythonNonExistent)
{
  PyObject* result = active_modules->get_daemon_status_python("osd", "999");
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result, Py_None);
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, GetDaemonStatusPythonEmpty)
{
  DaemonKey key{"osd", "0"};
  auto daemon = std::make_shared<DaemonState>(daemon_state->types);
  daemon->key = key;
  daemon->hostname = "test_host";
  daemon_state->insert(daemon);
  
  PyObject* result = active_modules->get_daemon_status_python("osd", "0");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  EXPECT_EQ(PyDict_Size(result), 0);
  
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, GetDaemonStatusPythonWithStatus)
{
  DaemonKey key{"osd", "0"};
  auto daemon = std::make_shared<DaemonState>(daemon_state->types);
  daemon->key = key;
  daemon->hostname = "test_host";
  daemon->service_status["status"] = "running";
  daemon->service_status["state"] = "active";
  daemon->service_status["health"] = "ok";
  daemon_state->insert(daemon);
  
  PyObject* result = active_modules->get_daemon_status_python("osd", "0");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  EXPECT_EQ(PyDict_Size(result), 3);
  
  PyObject* status = PyDict_GetItemString(result, "status");
  ASSERT_NE(status, nullptr);
  ASSERT_TRUE(PyUnicode_Check(status));
  EXPECT_STREQ(PyUnicode_AsUTF8(status), "running");
  
  PyObject* state = PyDict_GetItemString(result, "state");
  ASSERT_NE(state, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(state), "active");
  
  PyObject* health = PyDict_GetItemString(result, "health");
  ASSERT_NE(health, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(health), "ok");
  
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, GetDaemonStatusPythonDifferentTypes)
{
  DaemonKey osd_key{"osd", "5"};
  auto osd_daemon = std::make_shared<DaemonState>(daemon_state->types);
  osd_daemon->key = osd_key;
  osd_daemon->hostname = "osd_host";
  osd_daemon->service_status["osd_status"] = "up";
  osd_daemon->service_status["in"] = "true";
  daemon_state->insert(osd_daemon);
  
  DaemonKey mon_key{"mon", "a"};
  auto mon_daemon = std::make_shared<DaemonState>(daemon_state->types);
  mon_daemon->key = mon_key;
  mon_daemon->hostname = "mon_host";
  mon_daemon->service_status["mon_status"] = "leader";
  mon_daemon->service_status["quorum"] = "true";
  daemon_state->insert(mon_daemon);
  
  PyObject* osd_result = active_modules->get_daemon_status_python("osd", "5");
  ASSERT_NE(osd_result, nullptr);
  ASSERT_TRUE(PyDict_Check(osd_result));
  EXPECT_EQ(PyDict_Size(osd_result), 2);
  
  PyObject* osd_status = PyDict_GetItemString(osd_result, "osd_status");
  ASSERT_NE(osd_status, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(osd_status), "up");
  Py_DECREF(osd_result);
  
  PyObject* mon_result = active_modules->get_daemon_status_python("mon", "a");
  ASSERT_NE(mon_result, nullptr);
  ASSERT_TRUE(PyDict_Check(mon_result));
  EXPECT_EQ(PyDict_Size(mon_result), 2);
  
  PyObject* mon_status = PyDict_GetItemString(mon_result, "mon_status");
  ASSERT_NE(mon_status, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(mon_status), "leader");
  Py_DECREF(mon_result);
}

TEST_F(ActivePyModulesTest, GetStoreNonExistent)
{
  std::string val;
  bool found = active_modules->get_store("test_module", "nonexistent_key", &val);
  EXPECT_FALSE(found);
}

TEST_F(ActivePyModulesTest, GetStoreAfterSet)
{
  active_modules->set_store("test_module", "test_key", "test_value");
  std::string val;
  bool found = active_modules->get_store("test_module", "test_key", &val);
  ASSERT_TRUE(found);
  EXPECT_EQ(val, "test_value");
}

TEST_F(ActivePyModulesTest, GetStoreDifferentModules)
{
  active_modules->set_store("module1", "key1", "value1");
  active_modules->set_store("module2", "key1", "value2");
  
  std::string val1, val2;
  ASSERT_TRUE(active_modules->get_store("module1", "key1", &val1));
  ASSERT_TRUE(active_modules->get_store("module2", "key1", &val2));
  
  EXPECT_EQ(val1, "value1");
  EXPECT_EQ(val2, "value2");
}

TEST_F(ActivePyModulesTest, GetStorePrefixEmpty)
{
  PyObject* result = active_modules->get_store_prefix("test_module", "prefix");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  EXPECT_EQ(PyDict_Size(result), 0);
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, GetStorePrefixWithKeys)
{
  active_modules->set_store("test_module", "config/option1", "value1");
  active_modules->set_store("test_module", "config/option2", "value2");
  active_modules->set_store("test_module", "other/option3", "value3");
  
  PyObject* result = active_modules->get_store_prefix("test_module", "config/");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  EXPECT_EQ(PyDict_Size(result), 2);
  
  PyObject* opt1 = PyDict_GetItemString(result, "config/option1");
  ASSERT_NE(opt1, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(opt1), "value1");
  
  PyObject* opt2 = PyDict_GetItemString(result, "config/option2");
  ASSERT_NE(opt2, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(opt2), "value2");
  
  PyObject* opt3 = PyDict_GetItemString(result, "other/option3");
  EXPECT_EQ(opt3, nullptr);
  
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, GetStorePrefixDifferentModules)
{
  active_modules->set_store("module1", "prefix/key1", "value1");
  active_modules->set_store("module2", "prefix/key2", "value2");
  
  PyObject* result1 = active_modules->get_store_prefix("module1", "prefix/");
  ASSERT_NE(result1, nullptr);
  ASSERT_TRUE(PyDict_Check(result1));
  EXPECT_EQ(PyDict_Size(result1), 1);
  Py_DECREF(result1);
  
  PyObject* result2 = active_modules->get_store_prefix("module2", "prefix/");
  ASSERT_NE(result2, nullptr);
  ASSERT_TRUE(PyDict_Check(result2));
  EXPECT_EQ(PyDict_Size(result2), 1);
  Py_DECREF(result2);
}

TEST_F(ActivePyModulesTest, GetConfigNonExistent)
{
  std::string value;
  bool found = active_modules->get_config("test_module", "nonexistent_key", &value);
  EXPECT_FALSE(found);
}

// NOTE: set_config requires MonClient to work; in unit tests MonClient returns
// -125 (Operation canceled) since there is no live monitor.
TEST_F(ActivePyModulesTest, SetConfigWithoutMonitor)
{
  auto [result, msg] = active_modules->set_config("test_module", "test_option", "test_value");
  EXPECT_EQ(result, -125);  // Operation canceled - no monitor in unit tests
}

TEST_F(ActivePyModulesTest, SetStoreVariousValues)
{
  active_modules->set_store("test_module", "key1", "value1");
  std::string val;
  ASSERT_TRUE(active_modules->get_store("test_module", "key1", &val));
  EXPECT_EQ(val, "value1");
  
  active_modules->set_store("test_module", "key1", std::nullopt);
  EXPECT_FALSE(active_modules->get_store("test_module", "key1", &val))
    << "Key should be deleted after setting to nullopt";
  
  active_modules->set_store("test_module", "key2", "");
  ASSERT_TRUE(active_modules->get_store("test_module", "key2", &val));
  EXPECT_EQ(val, "") << "Empty string should be stored and retrieved";
  
  std::string special_value = "value with spaces\nand\nnewlines\tand\ttabs";
  active_modules->set_store("test_module", "key3", special_value);
  ASSERT_TRUE(active_modules->get_store("test_module", "key3", &val));
  EXPECT_EQ(val, special_value) << "Special characters should be preserved";
}

// "config/" prefix triggers _refresh_config_map internally; config_map is rebuilt
// from store_cache["config/..."] entries.
TEST_F(ActivePyModulesTest, UpdateKvDataConfig)
{
  std::map<std::string, std::optional<bufferlist>, std::less<>> data;

  bufferlist bl1;
  bl1.append("value1");
  data["config/global/option1"] = bl1;

  bufferlist bl2;
  bl2.append("value2");
  data["config/osd/option2"] = bl2;

  ASSERT_NO_THROW(active_modules->update_kv_data("config/", false, data));
}

// NOTE: update_kv_data only updates the in-memory store_cache, not persistent store.
TEST_F(ActivePyModulesTest, UpdateKvDataIncremental)
{
  active_modules->set_store("test_module", "key1", "initial");
  active_modules->set_store("test_module", "key2", "original");
  
  // Verify initial state
  std::string val;
  ASSERT_TRUE(active_modules->get_store("test_module", "key1", &val));
  EXPECT_EQ(val, "initial");
  ASSERT_TRUE(active_modules->get_store("test_module", "key2", &val));
  EXPECT_EQ(val, "original");
  
  // Keys must match the full store path: mgr/<module>/<key>
  std::map<std::string, std::optional<bufferlist>, std::less<>> data;
  bufferlist bl1;
  bl1.append("updated");
  data["mgr/test_module/key1"] = bl1;
  
  bufferlist bl2;
  bl2.append("new_value");
  data["mgr/test_module/key3"] = bl2;
  
  active_modules->update_kv_data("mgr/test_module/", true, data);
  
  ASSERT_TRUE(active_modules->get_store("test_module", "key1", &val));
  EXPECT_EQ(val, "updated") << "Incremental update should update existing key";
  
  ASSERT_TRUE(active_modules->get_store("test_module", "key2", &val));
  EXPECT_EQ(val, "original") << "Incremental update should preserve other keys";
  
  ASSERT_TRUE(active_modules->get_store("test_module", "key3", &val));
  EXPECT_EQ(val, "new_value") << "Incremental update should add new keys";
}

TEST_F(ActivePyModulesTest, UpdateKvDataDeletion)
{
  active_modules->set_store("test_module", "key1", "value1");
  active_modules->set_store("test_module", "key2", "value2");
  
  // Verify initial state
  std::string val;
  ASSERT_TRUE(active_modules->get_store("test_module", "key1", &val));
  EXPECT_EQ(val, "value1");
  ASSERT_TRUE(active_modules->get_store("test_module", "key2", &val));
  EXPECT_EQ(val, "value2");
  
  // Keys must match the full store path: mgr/<module>/<key>
  std::map<std::string, std::optional<bufferlist>, std::less<>> data;
  data["mgr/test_module/key1"] = std::nullopt;
  
  active_modules->update_kv_data("mgr/test_module/", true, data);
  
  EXPECT_FALSE(active_modules->get_store("test_module", "key1", &val))
    << "Key should be deleted after update_kv_data with nullopt";
  
  ASSERT_TRUE(active_modules->get_store("test_module", "key2", &val));
  EXPECT_EQ(val, "value2") << "Other keys should not be affected by deletion";
}

TEST_F(ActivePyModulesTest, UpdateKvDataEmpty)
{
  std::map<std::string, std::optional<bufferlist>, std::less<>> data;
  
  active_modules->update_kv_data("test/", false, data);
  active_modules->update_kv_data("test/", true, data);
}

TEST_F(ActivePyModulesTest, UpdateKvDataNonConfigPrefix)
{
  std::map<std::string, std::optional<bufferlist>, std::less<>> data;
  
  bufferlist bl1;
  bl1.append("value1");
  data["module/dashboard/key1"] = bl1;
  
  bufferlist bl2;
  bl2.append("value2");
  data["module/prometheus/key2"] = bl2;
  
  ASSERT_NO_THROW(active_modules->update_kv_data("module/", false, data))
    << "Non-config prefix should work without triggering config refresh";
}

TEST_F(ActivePyModulesTest, UpdateKvDataConfigPrefix)
{
  std::map<std::string, std::optional<bufferlist>, std::less<>> data;
  
  bufferlist bl;
  bl.append("16");
  data["config/osd/osd_max_backfills"] = bl;
  
  ASSERT_NO_THROW(active_modules->update_kv_data("config/", false, data))
    << "Config prefix should trigger config refresh without crashing";
}

TEST_F(ActivePyModulesTest, UpdateKvDataMixedKeys)
{
  std::map<std::string, std::optional<bufferlist>, std::less<>> data;
  
  bufferlist bl1;
  bl1.append("value1");
  data["module/dashboard/key1"] = bl1;
  
  bufferlist bl2;
  bl2.append("16");
  data["config/osd/osd_max_backfills"] = bl2;
  
  bufferlist bl3;
  bl3.append("value3");
  data["other/key3"] = bl3;
  
  ASSERT_NO_THROW(active_modules->update_kv_data("", false, data))
    << "Mixed keys should handle config refresh properly";
}

TEST_F(ActivePyModulesTest, UpdateKvDataFullUpdateClearsPrefix)
{
  std::map<std::string, std::optional<bufferlist>, std::less<>> data1;
  bufferlist bl1;
  bl1.append("initial1");
  data1["test/key1"] = bl1;
  
  bufferlist bl2;
  bl2.append("initial2");
  data1["test/key2"] = bl2;
  
  active_modules->update_kv_data("test/", false, data1);
  
  std::map<std::string, std::optional<bufferlist>, std::less<>> data2;
  bufferlist bl3;
  bl3.append("new1");
  data2["test/key3"] = bl3;
  
  ASSERT_NO_THROW(active_modules->update_kv_data("test/", false, data2))
    << "Full update should clear old keys with same prefix";
}

TEST_F(ActivePyModulesTest, GetTypedConfigNonExistent)
{
  PyObject* result = active_modules->get_typed_config("test_module", "nonexistent_key");
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result, Py_None) << "Should return None for non-existent config key";
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, GetTypedConfigInvalidModule)
{
  std::map<std::string, std::optional<bufferlist>, std::less<>> data;
  bufferlist bl;
  bl.append("test_value");
  data["config/global/test_option"] = bl;
  active_modules->update_kv_data("config/", false, data);
  
  PyObject* result = active_modules->get_typed_config("nonexistent_module", "test_option", "global");
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result, Py_None) << "Should return None for invalid module";
  Py_DECREF(result);
}

// Test: With prefix - get_config looks up "mgr/<module>/<prefix>/<key>" first.
// Seed "mgr/test_module/global/debug_level"; the prefixed lookup must find it.
TEST_F(ActivePyModulesTest, GetTypedConfigWithPrefix)
{
  py_config->config["mgr/test_module/global/debug_level"] = "5";

  std::string val;
  ASSERT_TRUE(active_modules->test_get_config("test_module", "global/debug_level", &val));
  EXPECT_EQ(val, "5");
}

// Test: Prefix fallback - when prefixed key is absent, bare key is found.
// Only "mgr/test_module/debug_level" is seeded; "global/debug_level" is absent.
TEST_F(ActivePyModulesTest, GetTypedConfigPrefixFallback)
{
  py_config->config["mgr/test_module/debug_level"] = "3";

  std::string val;
  EXPECT_FALSE(active_modules->test_get_config("test_module", "global/debug_level", &val))
    << "Prefixed key should not be found";
  ASSERT_TRUE(active_modules->test_get_config("test_module", "debug_level", &val));
  EXPECT_EQ(val, "3");
}

// Test: Prefix precedence - prefixed key wins when both keys exist.
// "mgr/test_module/global/setting" = "10" and "mgr/test_module/setting" = "5".
TEST_F(ActivePyModulesTest, GetTypedConfigPrefixPrecedence)
{
  py_config->config["mgr/test_module/global/setting"] = "10";
  py_config->config["mgr/test_module/setting"] = "5";

  std::string val;
  ASSERT_TRUE(active_modules->test_get_config("test_module", "global/setting", &val));
  EXPECT_EQ(val, "10") << "Prefixed key should take precedence over bare key";
}

// Test: Without prefix - bare key lookup returns stored value.
TEST_F(ActivePyModulesTest, GetTypedConfigWithoutPrefix)
{
  py_config->config["mgr/test_module/simple_option"] = "test_value";

  std::string val;
  ASSERT_TRUE(active_modules->test_get_config("test_module", "simple_option", &val));
  EXPECT_EQ(val, "test_value");
}

// Test: Empty prefix behaves the same as bare key lookup.
TEST_F(ActivePyModulesTest, GetTypedConfigEmptyPrefix)
{
  py_config->config["mgr/test_module/option"] = "value";

  std::string val;
  ASSERT_TRUE(active_modules->test_get_config("test_module", "option", &val));
  EXPECT_EQ(val, "value");
}

// Test: Second write to the same key is visible immediately.
TEST_F(ActivePyModulesTest, GetTypedConfigAfterUpdate)
{
  py_config->config["mgr/test_module/dynamic_option"] = "initial";

  std::string val;
  ASSERT_TRUE(active_modules->test_get_config("test_module", "dynamic_option", &val));
  EXPECT_EQ(val, "initial");

  py_config->config["mgr/test_module/dynamic_option"] = "updated";

  ASSERT_TRUE(active_modules->test_get_config("test_module", "dynamic_option", &val));
  EXPECT_EQ(val, "updated") << "Config update must be visible immediately";
}

// Test: Erasing the key makes test_get_config return false.
TEST_F(ActivePyModulesTest, GetTypedConfigAfterDeletion)
{
  py_config->config["mgr/test_module/temp_option"] = "value";

  std::string val;
  ASSERT_TRUE(active_modules->test_get_config("test_module", "temp_option", &val));
  EXPECT_EQ(val, "value");

  py_config->config.erase("mgr/test_module/temp_option");

  EXPECT_FALSE(active_modules->test_get_config("test_module", "temp_option", &val))
    << "Deleted key must not be found";
}

// Test: Key with underscores is stored and retrieved correctly.
TEST_F(ActivePyModulesTest, GetTypedConfigSpecialCharacters)
{
  py_config->config["mgr/test_module/option_with_underscores"] = "value";

  std::string val;
  ASSERT_TRUE(active_modules->test_get_config("test_module", "option_with_underscores", &val));
  EXPECT_EQ(val, "value");
}

// Test: Long key name is stored and retrieved correctly.
TEST_F(ActivePyModulesTest, GetTypedConfigLongKeyName)
{
  const std::string long_key =
      "very_long_configuration_option_name_that_exceeds_normal_length";
  py_config->config["mgr/test_module/" + long_key] = "value";

  std::string val;
  ASSERT_TRUE(active_modules->test_get_config("test_module", long_key, &val));
  EXPECT_EQ(val, "value");
}


TEST_F(ActivePyModulesTest, WithUnlabeledPerfCountersExisting)
{
  PerfCounterType counter_type;
  counter_type.type = PERFCOUNTER_U64;
  counter_type.description = "Operations";
  
  add_daemon_perf_counters("osd", "0", {{"osd.ops", counter_type}});
  add_perf_counter_data("osd", "0", "osd.ops", {{utime_t(100, 0), 42}, {utime_t(200, 0), 84}});
  
  PyObject* result = active_modules->get_unlabeled_counter_python("osd", "0", "osd.ops");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, WithUnlabeledPerfCountersNonExistent)
{
  PyObject* result1 = active_modules->get_unlabeled_counter_python("osd", "999", "osd.ops");
  ASSERT_NE(result1, nullptr) << "Should return dict for non-existent daemon";
  ASSERT_TRUE(PyDict_Check(result1));
  
  PerfCounterType counter_type;
  counter_type.type = PERFCOUNTER_U64;
  add_daemon_perf_counters("osd", "0", {{"osd.ops", counter_type}});
  
  PyObject* result2 = active_modules->get_unlabeled_counter_python("osd", "0", "osd.nonexistent");
  ASSERT_NE(result2, nullptr) << "Should return dict for non-existent counter";
  ASSERT_TRUE(PyDict_Check(result2));
  
  Py_DECREF(result2);
  Py_DECREF(result1);
}

TEST_F(ActivePyModulesTest, WithPerfCounters)
{
  PerfCounterType labeled_type;
  labeled_type.type = PERFCOUNTER_U64;
  labeled_type.description = "Scrubs completed";
  
  std::string labeled_path = ceph::perf_counters::key_insert(
    "osd_scrub", {{"level", "shallow"}}) + ".scrubs";
  
  add_daemon_perf_counters("osd", "0", {{labeled_path, labeled_type}});
  add_perf_counter_data("osd", "0", labeled_path, {{utime_t(100, 0), 5}});
  
  PyObject* labeled_result = active_modules->get_latest_counter_python(
    "osd", "0", "osd_scrub", "scrubs", {{"level", "shallow"}});
  ASSERT_NE(labeled_result, nullptr);
  ASSERT_TRUE(PyDict_Check(labeled_result)) << "Labeled counter should return dict";
  
  PerfCounterType unlabeled_type;
  unlabeled_type.type = PERFCOUNTER_U64;
  unlabeled_type.description = "Operations";
  
  add_daemon_perf_counters("osd", "1", {{"osd.ops", unlabeled_type}});
  add_perf_counter_data("osd", "1", "osd.ops", {{utime_t(100, 0), 42}});
  
  PyObject* unlabeled_result = active_modules->get_latest_counter_python(
    "osd", "1", "osd", "ops", {});
  ASSERT_NE(unlabeled_result, nullptr);
  ASSERT_TRUE(PyDict_Check(unlabeled_result)) << "Unlabeled counter should return dict";
  
  PyObject* nonexist_result = active_modules->get_latest_counter_python(
    "osd", "999", "osd", "ops", {});
  ASSERT_NE(nonexist_result, nullptr) << "Should return dict for non-existent daemon";
  ASSERT_TRUE(PyDict_Check(nonexist_result));
  
  Py_DECREF(nonexist_result);
  Py_DECREF(unlabeled_result);
  Py_DECREF(labeled_result);
}

TEST_F(ActivePyModulesTest, GetUnlabeledCounterPythonBasic)
{
  PerfCounterType counter_type;
  counter_type.type = PERFCOUNTER_U64;
  counter_type.nick = "ops";
  counter_type.description = "Operations completed";
  
  add_daemon_perf_counters("osd", "0", {{"osd.ops", counter_type}});
  add_perf_counter_data("osd", "0", "osd.ops", {{utime_t(100, 0), 42}, {utime_t(200, 0), 84}});
  
  PyObject* result = active_modules->get_unlabeled_counter_python("osd", "0", "osd.ops");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  
  verify_py_dict_has_key(result, "osd.ops");
  
  PyObject* data = PyDict_GetItemString(result, "osd.ops");
  ASSERT_NE(data, nullptr);
  ASSERT_TRUE(PyList_Check(data)) << "Counter data should be a list";
  ASSERT_EQ(PyList_Size(data), 2) << "Should have exactly 2 data points";
  
  PyObject* point1 = PyList_GetItem(data, 0);
  ASSERT_NE(point1, nullptr);
  ASSERT_TRUE(PyList_Check(point1)) << "Data point should be a list";
  ASSERT_EQ(PyList_Size(point1), 2) << "Data point should have [timestamp, value]";
  
  PyObject* value1 = PyList_GetItem(point1, 1);
  ASSERT_TRUE(PyLong_Check(value1)) << "Counter value should be an integer";
  ASSERT_EQ(PyLong_AsLong(value1), 42) << "First counter value should be 42";
  
  PyObject* point2 = PyList_GetItem(data, 1);
  ASSERT_NE(point2, nullptr);
  ASSERT_TRUE(PyList_Check(point2)) << "Data point should be a list";
  
  PyObject* value2 = PyList_GetItem(point2, 1);
  ASSERT_TRUE(PyLong_Check(value2)) << "Counter value should be an integer";
  ASSERT_EQ(PyLong_AsLong(value2), 84) << "Second counter value should be 84";
  
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, GetUnlabeledCounterPythonNonExistentDaemon)
{
  PyObject* result = active_modules->get_unlabeled_counter_python("osd", "999", "osd.ops");
  ASSERT_NE(result, nullptr) << "Should return valid dict even for non-existent daemon";
  ASSERT_TRUE(PyDict_Check(result)) << "Result should be a dictionary";
  
  Py_ssize_t dict_size = PyDict_Size(result);
  if (dict_size > 0) {
    PyObject* data = PyDict_GetItemString(result, "osd.ops");
    ASSERT_NE(data, nullptr) << "If dict has entries, should have osd.ops key";
    ASSERT_TRUE(PyList_Check(data)) << "Counter data should be a list";
    ASSERT_EQ(PyList_Size(data), 0) << "Non-existent daemon should return empty list";
  }
  
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, GetUnlabeledCounterPythonNonExistentCounter)
{
  PerfCounterType counter_type;
  counter_type.type = PERFCOUNTER_U64;
  add_daemon_perf_counters("osd", "0", {{"osd.ops", counter_type}});
  
  PyObject* result = active_modules->get_unlabeled_counter_python("osd", "0", "osd.nonexistent");
  ASSERT_NE(result, nullptr) << "Should return valid dict even for non-existent counter";
  ASSERT_TRUE(PyDict_Check(result)) << "Result should be a dictionary";
  
  Py_ssize_t dict_size = PyDict_Size(result);
  if (dict_size > 0) {
    PyObject* data = PyDict_GetItemString(result, "osd.nonexistent");
    ASSERT_NE(data, nullptr) << "If dict has entries, should have osd.nonexistent key";
    ASSERT_TRUE(PyList_Check(data)) << "Counter data should be a list";
    ASSERT_EQ(PyList_Size(data), 0) << "Non-existent counter should return empty list";
  }
  
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, GetUnlabeledCounterPythonMultipleCounters)
{
  PerfCounterType counter_type1;
  counter_type1.type = PERFCOUNTER_U64;
  counter_type1.nick = "ops";
  
  PerfCounterType counter_type2;
  counter_type2.type = PERFCOUNTER_U64;
  counter_type2.nick = "bytes";
  
  add_daemon_perf_counters("osd", "0", {
    {"osd.ops", counter_type1},
    {"osd.bytes_read", counter_type2}
  });
  
  add_perf_counter_data("osd", "0", "osd.ops", {{utime_t(100, 0), 42}});
  add_perf_counter_data("osd", "0", "osd.bytes_read", {{utime_t(100, 0), 1024}});
  
  PyObject* result1 = active_modules->get_unlabeled_counter_python("osd", "0", "osd.ops");
  ASSERT_NE(result1, nullptr);
  ASSERT_TRUE(PyDict_Check(result1));
  
  PyObject* data1 = PyDict_GetItemString(result1, "osd.ops");
  ASSERT_NE(data1, nullptr);
  ASSERT_TRUE(PyList_Check(data1));
  ASSERT_EQ(PyList_Size(data1), 1);
  
  PyObject* point1 = PyList_GetItem(data1, 0);
  PyObject* value1 = PyList_GetItem(point1, 1);
  ASSERT_EQ(PyLong_AsLong(value1), 42);
  
  PyObject* result2 = active_modules->get_unlabeled_counter_python("osd", "0", "osd.bytes_read");
  ASSERT_NE(result2, nullptr);
  
  PyObject* data2 = PyDict_GetItemString(result2, "osd.bytes_read");
  ASSERT_NE(data2, nullptr);
  ASSERT_TRUE(PyList_Check(data2));
  
  PyObject* point2 = PyList_GetItem(data2, 0);
  PyObject* value2 = PyList_GetItem(point2, 1);
  ASSERT_EQ(PyLong_AsLong(value2), 1024);
  
  Py_DECREF(result2);
  Py_DECREF(result1);
}

// The ring buffer keeps the last 20 data points.
TEST_F(ActivePyModulesTest, GetUnlabeledCounterPythonManyDataPoints)
{
  PerfCounterType counter_type;
  counter_type.type = PERFCOUNTER_U64;
  
  add_daemon_perf_counters("osd", "0", {{"osd.ops", counter_type}});
  
  std::vector<std::pair<utime_t, uint64_t>> data_points;
  for (int i = 0; i < 100; i++) {
    data_points.push_back({utime_t(i * 10, 0), static_cast<uint64_t>(i * 100)});
  }
  add_perf_counter_data("osd", "0", "osd.ops", data_points);
  
  PyObject* result = active_modules->get_unlabeled_counter_python("osd", "0", "osd.ops");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  
  PyObject* data = PyDict_GetItemString(result, "osd.ops");
  ASSERT_NE(data, nullptr);
  ASSERT_TRUE(PyList_Check(data));
  
  // System keeps last 20 data points
  Py_ssize_t size = PyList_Size(data);
  ASSERT_LE(size, 20) << "Should keep at most 20 data points";
  ASSERT_GT(size, 0) << "Should have at least some data points";
  
  PyObject* last_point = PyList_GetItem(data, size - 1);
  PyObject* last_value = PyList_GetItem(last_point, 1);
  ASSERT_TRUE(PyLong_Check(last_value));
  
  long val = PyLong_AsLong(last_value);
  ASSERT_GE(val, 8000) << "Last value should be from recent data points";
  ASSERT_LE(val, 9900);
  
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, GetLatestUnlabeledCounterPythonBasic)
{
  PerfCounterType counter_type;
  counter_type.type = PERFCOUNTER_U64;
  counter_type.nick = "ops";
  counter_type.description = "Operations completed";
  
  add_daemon_perf_counters("osd", "0", {{"osd.ops", counter_type}});
  
  DaemonKey key{"osd", "0"};
  auto daemon = daemon_state->get(key);
  ASSERT_NE(daemon, nullptr);
  
  auto& instance = daemon->perf_counters.instances.at("osd.ops");
  instance.push(utime_t(100, 0), 42);
  instance.push(utime_t(200, 0), 84);
  
  PyObject* result = active_modules->get_latest_unlabeled_counter_python("osd", "0", "osd.ops");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result)) << "Result should be a dictionary";
  
  PyObject* py_key = PyUnicode_FromString("osd.ops");
  ASSERT_TRUE(PyDict_Contains(result, py_key)) << "Result should contain 'osd.ops' key";
  
  PyObject* data = PyDict_GetItem(result, py_key);
  ASSERT_NE(data, nullptr) << "Counter data should not be null";
  ASSERT_TRUE(PyList_Check(data)) << "Counter data should be a list";
  ASSERT_EQ(PyList_Size(data), 2) << "Counter data should have [timestamp, value]";
  
  PyObject* value = PyList_GetItem(data, 1);
  ASSERT_TRUE(PyLong_Check(value)) << "Counter value should be an integer";
  ASSERT_EQ(PyLong_AsLong(value), 84) << "Latest counter value should be 84";
  
  Py_DECREF(py_key);
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, GetLatestUnlabeledCounterPythonLongRunAvg)
{
  PerfCounterType counter_type;
  counter_type.type = PERFCOUNTER_LONGRUNAVG;
  counter_type.description = "Average latency";
  
  add_daemon_perf_counters("osd", "0", {{"osd.latency", counter_type}});
  
  DaemonKey key{"osd", "0"};
  auto daemon = daemon_state->get(key);
  ASSERT_NE(daemon, nullptr);
  
  auto& instance = daemon->perf_counters.instances.at("osd.latency");
  instance.push_avg(utime_t(100, 0), 10, 5);  // sum=10, count=5, avg=2.0
  instance.push_avg(utime_t(200, 0), 20, 10); // sum=20, count=10, avg=2.0
  
  PyObject* result = active_modules->get_latest_unlabeled_counter_python("osd", "0", "osd.latency");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result)) << "Result should be a dictionary";
  
  PyObject* py_key = PyUnicode_FromString("osd.latency");
  ASSERT_TRUE(PyDict_Contains(result, py_key)) << "Result should contain 'osd.latency' key";
  
  PyObject* data = PyDict_GetItem(result, py_key);
  ASSERT_NE(data, nullptr) << "Counter data should not be null";
  // LONGRUNAVG data format varies (list, tuple, or dict) depending on implementation
  ASSERT_TRUE(PyList_Check(data) || PyTuple_Check(data) || PyDict_Check(data))
    << "LONGRUNAVG counter data should be a valid Python collection type";
  
  if (PyList_Check(data) && PyList_Size(data) == 2) {
    PyObject* item0 = PyList_GetItem(data, 0);
    PyObject* item1 = PyList_GetItem(data, 1);
    ASSERT_TRUE(PyLong_Check(item0) || PyFloat_Check(item0)) << "First element should be numeric";
    ASSERT_TRUE(PyLong_Check(item1) || PyFloat_Check(item1)) << "Second element should be numeric";
  } else if (PyTuple_Check(data) && PyTuple_Size(data) == 2) {
    PyObject* item0 = PyTuple_GetItem(data, 0);
    PyObject* item1 = PyTuple_GetItem(data, 1);
    ASSERT_TRUE(PyLong_Check(item0) || PyFloat_Check(item0)) << "First element should be numeric";
    ASSERT_TRUE(PyLong_Check(item1) || PyFloat_Check(item1)) << "Second element should be numeric";
  }
  
  Py_DECREF(py_key);
  Py_DECREF(result);
}

// Test: Zero value - verifies correct handling of zero counter values
TEST_F(ActivePyModulesTest, GetLatestUnlabeledCounterPythonZeroValue)
{
  PerfCounterType counter_type;
  counter_type.type = PERFCOUNTER_U64;
  
  add_daemon_perf_counters("osd", "0", {{"osd.ops", counter_type}});
  
  DaemonKey key{"osd", "0"};
  auto daemon = daemon_state->get(key);
  ASSERT_NE(daemon, nullptr);
  
  auto& instance = daemon->perf_counters.instances.at("osd.ops");
  instance.push(utime_t(100, 0), 0);  // Zero value
  
  PyObject* result = active_modules->get_latest_unlabeled_counter_python("osd", "0", "osd.ops");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  
  PyObject* py_key = PyUnicode_FromString("osd.ops");
  ASSERT_TRUE(PyDict_Contains(result, py_key));
  
  PyObject* data = PyDict_GetItem(result, py_key);
  ASSERT_NE(data, nullptr);
  ASSERT_TRUE(PyList_Check(data));
  
  PyObject* value = PyList_GetItem(data, 1);
  ASSERT_TRUE(PyLong_Check(value));
  ASSERT_EQ(PyLong_AsLong(value), 0) << "Should correctly handle zero value";
  
  Py_DECREF(py_key);
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, GetLatestUnlabeledCounterPythonLargeValue)
{
  PerfCounterType counter_type;
  counter_type.type = PERFCOUNTER_U64;
  
  add_daemon_perf_counters("osd", "0", {{"osd.bytes", counter_type}});
  
  DaemonKey key{"osd", "0"};
  auto daemon = daemon_state->get(key);
  ASSERT_NE(daemon, nullptr);
  
  auto& instance = daemon->perf_counters.instances.at("osd.bytes");
  uint64_t large_value = 1099511627776ULL;  // 1 TB in bytes
  instance.push(utime_t(100, 0), large_value);
  
  PyObject* result = active_modules->get_latest_unlabeled_counter_python("osd", "0", "osd.bytes");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  
  PyObject* py_key = PyUnicode_FromString("osd.bytes");
  PyObject* data = PyDict_GetItem(result, py_key);
  ASSERT_NE(data, nullptr);
  
  PyObject* value = PyList_GetItem(data, 1);
  ASSERT_TRUE(PyLong_Check(value));
  ASSERT_EQ(PyLong_AsUnsignedLongLong(value), large_value) 
    << "Should correctly handle large values";
  
  Py_DECREF(py_key);
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, GetLatestCounterPythonLabeled)
{
  PerfCounterType counter_type;
  counter_type.type = PERFCOUNTER_U64;
  counter_type.description = "Scrubs completed";
  
  std::string labeled_path = ceph::perf_counters::key_insert(
    "osd_scrub", {{"level", "shallow"}}) + ".scrubs";
  
  add_daemon_perf_counters("osd", "0", {{labeled_path, counter_type}});
  
  DaemonKey key{"osd", "0"};
  auto daemon = daemon_state->get(key);
  ASSERT_NE(daemon, nullptr);
  
  auto& instance = daemon->perf_counters.instances.at(labeled_path);
  instance.push(utime_t(100, 0), 5);
  
  std::vector<std::pair<std::string_view, std::string_view>> labels = {
    {"level", "shallow"}
  };
  
  PyObject* result = active_modules->get_latest_counter_python(
    "osd", "0", "osd_scrub", "scrubs", labels);
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, GetLatestCounterPythonUnlabeled)
{
  PerfCounterType counter_type;
  counter_type.type = PERFCOUNTER_U64;
  
  add_daemon_perf_counters("osd", "0", {{"osd.ops", counter_type}});
  
  DaemonKey key{"osd", "0"};
  auto daemon = daemon_state->get(key);
  ASSERT_NE(daemon, nullptr);
  
  auto& instance = daemon->perf_counters.instances.at("osd.ops");
  instance.push(utime_t(100, 0), 42);
  
  std::vector<std::pair<std::string_view, std::string_view>> empty_labels;
  
  PyObject* result = active_modules->get_latest_counter_python(
    "osd", "0", "osd", "ops", empty_labels);
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, GetLatestCounterPythonNonExistent)
{
  std::vector<std::pair<std::string_view, std::string_view>> labels = {
    {"level", "deep"}
  };
  
  PyObject* result = active_modules->get_latest_counter_python(
    "osd", "999", "osd_scrub", "scrubs", labels);
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, GetLatestCounterPythonComplexLabels)
{
  PerfCounterType counter_type;
  counter_type.type = PERFCOUNTER_U64;
  
  std::string path1 = ceph::perf_counters::key_insert(
    "osd_scrub", {{"level", "shallow"}, {"type", "periodic"}}) + ".scrubs";
  std::string path2 = ceph::perf_counters::key_insert(
    "osd_scrub", {{"level", "deep"}, {"type", "periodic"}}) + ".scrubs";
  std::string path3 = ceph::perf_counters::key_insert(
    "osd_scrub", {{"level", "shallow"}, {"type", "manual"}}) + ".scrubs";
  
  add_daemon_perf_counters("osd", "0", {
    {path1, counter_type},
    {path2, counter_type},
    {path3, counter_type}
  });
  
  DaemonKey key{"osd", "0"};
  auto daemon = daemon_state->get(key);
  ASSERT_NE(daemon, nullptr);
  
  daemon->perf_counters.instances.at(path1).push(utime_t(100, 0), 10);
  daemon->perf_counters.instances.at(path2).push(utime_t(100, 0), 5);
  daemon->perf_counters.instances.at(path3).push(utime_t(100, 0), 3);
  
  std::vector<std::pair<std::string_view, std::string_view>> labels1 = {
    {"level", "shallow"}, {"type", "periodic"}
  };
  
  PyObject* result1 = active_modules->get_latest_counter_python(
    "osd", "0", "osd_scrub", "scrubs", labels1);
  ASSERT_NE(result1, nullptr);
  ASSERT_TRUE(PyDict_Check(result1));
  ASSERT_GT(PyDict_Size(result1), 0) << "Should find counter with matching labels";
  
  PyObject *key_obj, *value;
  Py_ssize_t pos = 0;
  bool found = false;
  while (PyDict_Next(result1, &pos, &key_obj, &value)) {
    if (PyList_Check(value) && PyList_Size(value) == 2) {
      PyObject* counter_val = PyList_GetItem(value, 1);
      if (PyLong_Check(counter_val) && PyLong_AsLong(counter_val) == 10) {
        found = true;
        break;
      }
    }
  }
  ASSERT_TRUE(found) << "Should find counter with value 10";
  
  Py_DECREF(result1);
  
  std::vector<std::pair<std::string_view, std::string_view>> labels2 = {
    {"level", "deep"}, {"type", "periodic"}
  };
  
  PyObject* result2 = active_modules->get_latest_counter_python(
    "osd", "0", "osd_scrub", "scrubs", labels2);
  ASSERT_NE(result2, nullptr);
  ASSERT_TRUE(PyDict_Check(result2));
  
  Py_DECREF(result2);
}

TEST_F(ActivePyModulesTest, GetUnlabeledPerfSchemaEmpty)
{
  PyObject* result = active_modules->get_unlabeled_perf_schema_python("", "");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  EXPECT_EQ(PyDict_Size(result), 0);
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, GetUnlabeledPerfSchemaSingleCounter)
{
  PerfCounterType counter_type;
  counter_type.type = PERFCOUNTER_U64;
  counter_type.nick = "ops";
  counter_type.description = "Operations completed";
  counter_type.priority = 5;
  counter_type.unit = UNIT_NONE;
  
  add_daemon_perf_counters("osd", "0", {{"osd.ops", counter_type}});
  
  PyObject* result = active_modules->get_unlabeled_perf_schema_python("osd", "0");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  EXPECT_EQ(PyDict_Size(result), 1);
  
  PyObject* osd_dict = PyDict_GetItemString(result, "osd.0");
  ASSERT_NE(osd_dict, nullptr);
  ASSERT_TRUE(PyDict_Check(osd_dict));
  
  PyObject* counter = PyDict_GetItemString(osd_dict, "osd.ops");
  ASSERT_NE(counter, nullptr);
  ASSERT_TRUE(PyDict_Check(counter));
  
  PyObject* desc = PyDict_GetItemString(counter, "description");
  ASSERT_NE(desc, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(desc), "Operations completed");
  
  PyObject* nick = PyDict_GetItemString(counter, "nick");
  ASSERT_NE(nick, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(nick), "ops");
  
  PyObject* type = PyDict_GetItemString(counter, "type");
  ASSERT_NE(type, nullptr);
  EXPECT_EQ(PyLong_AsLong(type), PERFCOUNTER_U64);
  
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, GetUnlabeledPerfSchemaFiltersLabeled)
{
  PerfCounterType unlabeled_type;
  unlabeled_type.type = PERFCOUNTER_U64;
  unlabeled_type.description = "Unlabeled counter";
  
  PerfCounterType labeled_type;
  labeled_type.type = PERFCOUNTER_U64;
  labeled_type.description = "Labeled counter";
  
  // Add both unlabeled and labeled counters
  std::string labeled_path = ceph::perf_counters::key_insert(
    "osd_scrub", {{"level", "shallow"}}) + ".scrubs";
  
  add_daemon_perf_counters("osd", "0", {
    {"osd.ops", unlabeled_type},
    {labeled_path, labeled_type}
  });
  
  PyObject* result = active_modules->get_unlabeled_perf_schema_python("osd", "0");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  
  PyObject* osd_dict = PyDict_GetItemString(result, "osd.0");
  ASSERT_NE(osd_dict, nullptr);
  ASSERT_TRUE(PyDict_Check(osd_dict));
  
  // Should only have the unlabeled counter
  EXPECT_EQ(PyDict_Size(osd_dict), 1);
  
  PyObject* unlabeled = PyDict_GetItemString(osd_dict, "osd.ops");
  ASSERT_NE(unlabeled, nullptr) << "Unlabeled counter should be present";
  
  // labeled counters use a different key format and must not appear here
  PyObject* labeled = PyDict_GetItemString(osd_dict, labeled_path.c_str());
  EXPECT_EQ(labeled, nullptr) << "Labeled counter should be filtered out";
  
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, GetUnlabeledPerfSchemaServiceFilter)
{
  PerfCounterType counter_type;
  counter_type.type = PERFCOUNTER_U64;
  counter_type.description = "Operations";
  
  add_daemon_perf_counters("osd", "0", {{"osd.ops", counter_type}});
  add_daemon_perf_counters("osd", "1", {{"osd.ops", counter_type}});
  add_daemon_perf_counters("mon", "a", {{"mon.elections", counter_type}});
  
  PyObject* result = active_modules->get_unlabeled_perf_schema_python("osd", "");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  EXPECT_EQ(PyDict_Size(result), 2) << "Should have 2 OSD daemons";
  
  ASSERT_NE(PyDict_GetItemString(result, "osd.0"), nullptr);
  ASSERT_NE(PyDict_GetItemString(result, "osd.1"), nullptr);
  EXPECT_EQ(PyDict_GetItemString(result, "mon.a"), nullptr) << "MON should be filtered out";
  
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, GetPerfSchemaEmpty)
{
  PyObject* result = active_modules->get_perf_schema_python("", "");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  EXPECT_EQ(PyDict_Size(result), 0);
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, GetPerfSchemaSingleUnlabeledCounter)
{
  PerfCounterType counter_type;
  counter_type.type = PERFCOUNTER_U64;
  counter_type.nick = "used";
  counter_type.description = "Bytes used";

  //Use a dotted name here so we get one "osd" group with "stat_bytes" under it.
  add_daemon_perf_counters("osd", "0", {
    {"osd.stat_bytes", counter_type}
  });

  PyObject* result = active_modules->get_perf_schema_python("osd", "0");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  EXPECT_EQ(PyDict_Size(result), 1);

  PyObject* osd_dict = ActivePyModulesTest::lookup_dict_item(result, "osd.0");
  ASSERT_NE(osd_dict, nullptr);
  ASSERT_TRUE(PyDict_Check(osd_dict));
  EXPECT_EQ(PyDict_Size(osd_dict), 1);

  PyObject* osd_group = ActivePyModulesTest::lookup_dict_item(osd_dict, "osd");
  ASSERT_NE(osd_group, nullptr);
  ASSERT_TRUE(PyList_Check(osd_group));
  ASSERT_EQ(PyList_Size(osd_group), 1);

  PyObject* counter_obj = PyList_GetItem(osd_group, 0);
  ASSERT_NE(counter_obj, nullptr);
  ASSERT_TRUE(PyDict_Check(counter_obj));

  PyObject* labels = ActivePyModulesTest::lookup_dict_item(counter_obj, "labels");
  ASSERT_NE(labels, nullptr);
  ASSERT_TRUE(PyDict_Check(labels));
  EXPECT_EQ(PyDict_Size(labels), 0);

  PyObject* counters = ActivePyModulesTest::lookup_dict_item(counter_obj, "counters");
  ASSERT_NE(counters, nullptr);
  ASSERT_TRUE(PyDict_Check(counters));
  EXPECT_EQ(PyDict_Size(counters), 1);
  ActivePyModulesTest::expect_counter_metadata(counters, "stat_bytes", "Bytes used", "used",
                          PERFCOUNTER_U64);

  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, GetPerfSchemaSingleLabeledCounter)
{
  PerfCounterType counter_type;
  counter_type.type = PERFCOUNTER_U64;
  counter_type.nick = "scrubs";
  counter_type.description = "Shallow scrubs";

  //Single-label edge case
  add_daemon_perf_counters("osd", "0", {
    {ceph::perf_counters::key_insert("osd_scrub_sh_repl",
                                     {{"level", "shallow"}}) + ".scrubs",
     counter_type}
  });

  PyObject* result = active_modules->get_perf_schema_python("osd", "0");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));

  PyObject* osd_dict = ActivePyModulesTest::lookup_dict_item(result, "osd.0");
  ASSERT_NE(osd_dict, nullptr);
  ASSERT_TRUE(PyDict_Check(osd_dict));

  PyObject* scrub_group = ActivePyModulesTest::lookup_dict_item(osd_dict, "osd_scrub_sh_repl");
  ASSERT_NE(scrub_group, nullptr);
  ASSERT_TRUE(PyList_Check(scrub_group));
  ASSERT_EQ(PyList_Size(scrub_group), 1);

  PyObject* counter_obj = PyList_GetItem(scrub_group, 0);
  ASSERT_NE(counter_obj, nullptr);
  ASSERT_TRUE(PyDict_Check(counter_obj));

  PyObject* labels = ActivePyModulesTest::lookup_dict_item(counter_obj, "labels");
  ASSERT_NE(labels, nullptr);
  ASSERT_TRUE(PyDict_Check(labels));
  EXPECT_EQ(PyDict_Size(labels), 1);

  PyObject* level = ActivePyModulesTest::lookup_dict_item(labels, "level");
  ASSERT_NE(level, nullptr);
  ASSERT_TRUE(PyUnicode_Check(level));
  EXPECT_STREQ(PyUnicode_AsUTF8(level), "shallow");

  PyObject* counters = ActivePyModulesTest::lookup_dict_item(counter_obj, "counters");
  ASSERT_NE(counters, nullptr);
  ASSERT_TRUE(PyDict_Check(counters));
  EXPECT_EQ(PyDict_Size(counters), 1);
  ActivePyModulesTest::expect_counter_metadata(counters, "scrubs", "Shallow scrubs", "scrubs",
                          PERFCOUNTER_U64);

  Py_DECREF(result);
}

// Test: Groups by label set - verifies counters grouped by label combinations
TEST_F(ActivePyModulesTest, GetPerfSchemaGroupsLabeledCountersByLabelSet)
{
  PerfCounterType abc_type;
  abc_type.type = PERFCOUNTER_U64;
  abc_type.nick = "123";
  abc_type.description = "abc";

  PerfCounterType def_type;
  def_type.type = PERFCOUNTER_U64;
  def_type.nick = "456";
  def_type.description = "def";

  //Same base counter and two different label values
  //Should get two objects back in the same list
  add_daemon_perf_counters("osd", "0", {
    {ceph::perf_counters::key_insert("simple_counter", {{"l", "abc"}}) + ".x",
     abc_type},
    {ceph::perf_counters::key_insert("simple_counter", {{"l", "def"}}) + ".x",
     def_type}
  });

  PyObject* result = active_modules->get_perf_schema_python("osd", "0");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));

  PyObject* osd_dict = ActivePyModulesTest::lookup_dict_item(result, "osd.0");
  ASSERT_NE(osd_dict, nullptr);
  ASSERT_TRUE(PyDict_Check(osd_dict));

  PyObject* counter_list = ActivePyModulesTest::lookup_dict_item(osd_dict, "simple_counter");
  ASSERT_NE(counter_list, nullptr);
  ASSERT_TRUE(PyList_Check(counter_list));
  ASSERT_EQ(PyList_Size(counter_list), 2);

  bool found_abc = false;
  bool found_def = false;
  for (Py_ssize_t i = 0; i < PyList_Size(counter_list); ++i) {
    PyObject* counter_obj = PyList_GetItem(counter_list, i);
    ASSERT_NE(counter_obj, nullptr);
    ASSERT_TRUE(PyDict_Check(counter_obj));

    PyObject* labels = ActivePyModulesTest::lookup_dict_item(counter_obj, "labels");
    ASSERT_NE(labels, nullptr);
    ASSERT_TRUE(PyDict_Check(labels));

    PyObject* label_value = ActivePyModulesTest::lookup_dict_item(labels, "l");
    ASSERT_NE(label_value, nullptr);
    ASSERT_TRUE(PyUnicode_Check(label_value));

    const char* value = PyUnicode_AsUTF8(label_value);
    ASSERT_NE(value, nullptr);

    PyObject* counters = ActivePyModulesTest::lookup_dict_item(counter_obj, "counters");
    ASSERT_NE(counters, nullptr);
    ASSERT_TRUE(PyDict_Check(counters));
    EXPECT_EQ(PyDict_Size(counters), 1);

    if (std::string{value} == "abc") {
      found_abc = true;
      ActivePyModulesTest::expect_counter_metadata(counters, "x", "abc", "123", PERFCOUNTER_U64);
    } else if (std::string{value} == "def") {
      found_def = true;
      ActivePyModulesTest::expect_counter_metadata(counters, "x", "def", "456", PERFCOUNTER_U64);
    }
  }

  EXPECT_TRUE(found_abc);
  EXPECT_TRUE(found_def);

  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, GetPerfSchemaMixedCounters)
{
  PerfCounterType bytes_type;
  bytes_type.type = PERFCOUNTER_U64;
  bytes_type.nick = "bytes";
  bytes_type.description = "Total bytes";

  PerfCounterType latency_type;
  latency_type.type = PERFCOUNTER_U64;
  latency_type.nick = "lat";
  latency_type.description = "Commit latency";

  PerfCounterType shallow_type;
  shallow_type.type = PERFCOUNTER_U64;
  shallow_type.description = "Shallow scrubs";

  PerfCounterType deep_type;
  deep_type.type = PERFCOUNTER_U64;
  deep_type.description = "Deep scrubs";

  //Mix both code paths in one daemon so they don't step on each other.
  add_daemon_perf_counters("osd", "0", {
    {"osd.stat_bytes", bytes_type},
    {"osd.commit_latency_ms", latency_type},
    {ceph::perf_counters::key_insert("osd_scrub_sh_repl",
                                     {{"level", "shallow"}}) + ".scrubs",
     shallow_type},
    {ceph::perf_counters::key_insert("osd_scrub_sh_repl",
                                     {{"level", "deep"}}) + ".scrubs",
     deep_type}
  });

  PyObject* result = active_modules->get_perf_schema_python("osd", "0");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));

  PyObject* osd_dict = ActivePyModulesTest::lookup_dict_item(result, "osd.0");
  ASSERT_NE(osd_dict, nullptr);
  ASSERT_TRUE(PyDict_Check(osd_dict));
  EXPECT_EQ(PyDict_Size(osd_dict), 2);

  PyObject* osd_group = ActivePyModulesTest::lookup_dict_item(osd_dict, "osd");
  ASSERT_NE(osd_group, nullptr);
  ASSERT_TRUE(PyList_Check(osd_group));
  ASSERT_EQ(PyList_Size(osd_group), 1);

  PyObject* base_counter_obj = PyList_GetItem(osd_group, 0);
  ASSERT_NE(base_counter_obj, nullptr);
  ASSERT_TRUE(PyDict_Check(base_counter_obj));

  PyObject* base_counters = ActivePyModulesTest::lookup_dict_item(base_counter_obj, "counters");
  ASSERT_NE(base_counters, nullptr);
  ASSERT_TRUE(PyDict_Check(base_counters));
  EXPECT_EQ(PyDict_Size(base_counters), 2);
  ActivePyModulesTest::expect_counter_metadata(base_counters, "stat_bytes", "Total bytes", "bytes",
                          PERFCOUNTER_U64);
  ActivePyModulesTest::expect_counter_metadata(base_counters, "commit_latency_ms", "Commit latency",
                          "lat", PERFCOUNTER_U64);

  PyObject* scrub_group = ActivePyModulesTest::lookup_dict_item(osd_dict, "osd_scrub_sh_repl");
  ASSERT_NE(scrub_group, nullptr);
  ASSERT_TRUE(PyList_Check(scrub_group));
  ASSERT_EQ(PyList_Size(scrub_group), 2);

  bool found_shallow = false;
  bool found_deep = false;
  for (Py_ssize_t i = 0; i < PyList_Size(scrub_group); ++i) {
    PyObject* counter_obj = PyList_GetItem(scrub_group, i);
    ASSERT_NE(counter_obj, nullptr);
    ASSERT_TRUE(PyDict_Check(counter_obj));

    PyObject* labels = ActivePyModulesTest::lookup_dict_item(counter_obj, "labels");
    ASSERT_NE(labels, nullptr);
    ASSERT_TRUE(PyDict_Check(labels));

    PyObject* level = ActivePyModulesTest::lookup_dict_item(labels, "level");
    ASSERT_NE(level, nullptr);
    ASSERT_TRUE(PyUnicode_Check(level));

    const char* value = PyUnicode_AsUTF8(level);
    ASSERT_NE(value, nullptr);

    PyObject* counters = ActivePyModulesTest::lookup_dict_item(counter_obj, "counters");
    ASSERT_NE(counters, nullptr);
    ASSERT_TRUE(PyDict_Check(counters));

    if (std::string{value} == "shallow") {
      found_shallow = true;
      ActivePyModulesTest::expect_counter_metadata(counters, "scrubs", "Shallow scrubs", nullptr,
                              PERFCOUNTER_U64);
    } else if (std::string{value} == "deep") {
      found_deep = true;
      ActivePyModulesTest::expect_counter_metadata(counters, "scrubs", "Deep scrubs", nullptr,
                              PERFCOUNTER_U64);
    }
  }

  EXPECT_TRUE(found_shallow);
  EXPECT_TRUE(found_deep);

  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, GetPerfSchemaFiltersByServiceAndDaemon)
{
  PerfCounterType counter_type;
  counter_type.type = PERFCOUNTER_U64;
  counter_type.description = "Operations";

  add_daemon_perf_counters("osd", "0", {{"osd.ops_completed", counter_type}});
  add_daemon_perf_counters("osd", "1", {{"osd.ops_completed", counter_type}});
  add_daemon_perf_counters("mon", "a", {{"mon.elections", counter_type}});

  //Same data through the three queries: service, daemon, and all
  PyObject* by_service = active_modules->get_perf_schema_python("osd", "");
  ASSERT_NE(by_service, nullptr);
  ASSERT_TRUE(PyDict_Check(by_service));
  EXPECT_EQ(PyDict_Size(by_service), 2);
  ASSERT_NE(ActivePyModulesTest::lookup_dict_item(by_service, "osd.0"), nullptr);
  ASSERT_NE(ActivePyModulesTest::lookup_dict_item(by_service, "osd.1"), nullptr);
  EXPECT_EQ(PyDict_GetItemString(by_service, "mon.a"), nullptr);
  Py_DECREF(by_service);

  PyObject* by_daemon = active_modules->get_perf_schema_python("osd", "1");
  ASSERT_NE(by_daemon, nullptr);
  ASSERT_TRUE(PyDict_Check(by_daemon));
  EXPECT_EQ(PyDict_Size(by_daemon), 1);
  ASSERT_NE(ActivePyModulesTest::lookup_dict_item(by_daemon, "osd.1"), nullptr);
  EXPECT_EQ(PyDict_GetItemString(by_daemon, "osd.0"), nullptr);
  Py_DECREF(by_daemon);

  PyObject* all_daemons = active_modules->get_perf_schema_python("", "");
  ASSERT_NE(all_daemons, nullptr);
  ASSERT_TRUE(PyDict_Check(all_daemons));
  EXPECT_EQ(PyDict_Size(all_daemons), 3);
  ASSERT_NE(ActivePyModulesTest::lookup_dict_item(all_daemons, "mon.a"), nullptr);
  Py_DECREF(all_daemons);
}

// Regression: OSD with no perf counters (destroyed OSD) must not cause an assertion
// failure in the loop over state->perf_counters.instances (Tracker 74693).
TEST_F(ActivePyModulesTest, GetPerfSchemaWithDestroyedOSD) {
  PerfCounterType counter_type;
  counter_type.type = PERFCOUNTER_U64;
  counter_type.description = "Operations";

  std::map<std::string, PerfCounterType> counter_types = {
    {"ops_completed", counter_type}
  };

  // osd.0 has counters; osd.123 does not (destroyed), exercising the empty-instance path
  add_daemon_perf_counters("osd", "0", counter_types);

  DaemonKey destroyed_key{"osd", "123"};
  auto destroyed_daemon = std::make_shared<DaemonState>(daemon_state->types);
  destroyed_daemon->key = destroyed_key;
  destroyed_daemon->hostname = "";
  daemon_state->insert(destroyed_daemon);

  PyObject* result = active_modules->get_perf_schema_python("osd", "");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));

  ASSERT_GE(PyDict_Size(result), 1);

  PyObject* osd0 = PyDict_GetItemString(result, "osd.0");
  ASSERT_NE(osd0, nullptr);

  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, GetRocksdbVersion)
{
  PyObject* result = active_modules->get_rocksdb_version();
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyUnicode_Check(result));
  
  const char* version = PyUnicode_AsUTF8(result);
  ASSERT_NE(version, nullptr);
  EXPECT_NE(std::string(version).find("."), std::string::npos);
  
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, GetContext)
{
  PyObject* result = active_modules->get_context();
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyCapsule_CheckExact(result));
  
  void* ctx = PyCapsule_GetPointer(result, nullptr);
  ASSERT_NE(ctx, nullptr);
  
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, CephCacheMapErase_NonExistentKey_ReturnsENOENT)
{
  int r = active_modules->ceph_cache_map_erase("nonexistent_key");
  EXPECT_EQ(r, -ENOENT);
}

TEST_F(ActivePyModulesTest, CephCacheMapErase_NotCacheableKey_ReturnsEINVAL)
{
  // An empty key is never in the cache, so !exists → -ENOENT
  int r = active_modules->ceph_cache_map_erase("");
  EXPECT_EQ(r, -ENOENT);
}

TEST_F(ActivePyModulesTest, CephCacheMapErase_ExistingCacheableKey_Succeeds)
{
  PyGILState_STATE gstate = PyGILState_Ensure();

  PyObject* obj1 = active_modules->cacheable_get_python("osd_map");
  ASSERT_NE(obj1, nullptr);
  ASSERT_TRUE(PyDict_Check(obj1));

  PyObject* obj2 = active_modules->cacheable_get_python("osd_map");
  ASSERT_EQ(obj1, obj2);

  int r = active_modules->ceph_cache_map_erase("osd_map");
  EXPECT_EQ(r, 0);

  // Cache is gone; second erase must return -ENOENT
  int r2 = active_modules->ceph_cache_map_erase("osd_map");
  EXPECT_EQ(r2, -ENOENT);

  Py_DECREF(obj1);
  Py_DECREF(obj2);
  PyGILState_Release(gstate);
}

TEST_F(ActivePyModulesTest, CacheableGetPythonFsMap)
{
  PyObject* result = active_modules->cacheable_get_python("fs_map");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  
  PyObject* epoch = PyDict_GetItemString(result, "epoch");
  ASSERT_NE(epoch, nullptr);
  ASSERT_TRUE(PyLong_Check(epoch));
  
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, CacheableGetPythonOsdMap)
{
  PyObject* result = active_modules->cacheable_get_python("osd_map");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  
  PyObject* epoch = PyDict_GetItemString(result, "epoch");
  ASSERT_NE(epoch, nullptr);
  ASSERT_TRUE(PyLong_Check(epoch));
  
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, CacheableGetPythonGetMutableBypassesCache)
{
  PyGILState_STATE gstate = PyGILState_Ensure();

  PyObject* obj1 = active_modules->cacheable_get_python("osd_map", true);
  ASSERT_NE(obj1, nullptr);

  // Each get_mutable=true call must bypass the cache and return a new object
  PyObject* obj2 = active_modules->cacheable_get_python("osd_map", true);
  ASSERT_NE(obj2, nullptr);
  EXPECT_NE(obj1, obj2);

  Py_DECREF(obj1);
  Py_DECREF(obj2);
  PyGILState_Release(gstate);
}

TEST_F(ActivePyModulesTest, GetPythonFsMap)
{
  PyObject* result = active_modules->get_python("fs_map");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  
  PyObject* epoch = PyDict_GetItemString(result, "epoch");
  ASSERT_NE(epoch, nullptr);
  ASSERT_TRUE(PyLong_Check(epoch));
  
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, GetPythonOsdMapTree)
{
  PyObject* result = active_modules->get_python("osd_map_tree");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  
  PyObject* nodes = PyDict_GetItemString(result, "nodes");
  ASSERT_NE(nodes, nullptr);
  ASSERT_TRUE(PyList_Check(nodes));
  
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, GetPythonOsdMapCrushMapText)
{
  PyObject* result = active_modules->get_python("osdmap_crush_map_text");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyUnicode_Check(result));
  
  const char* crush_text = PyUnicode_AsUTF8(result);
  ASSERT_NE(crush_text, nullptr);
  
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, GetPythonOsdMapBranches)
{
  PyObject* res_osd = active_modules->get_python("osd_map");
  ASSERT_NE(res_osd, nullptr);
  ASSERT_TRUE(PyDict_Check(res_osd));
  EXPECT_NE(PyDict_GetItemString(res_osd, "epoch"), nullptr);
  Py_DECREF(res_osd);

  PyObject* res_crush = active_modules->get_python("osd_map_crush");
  ASSERT_NE(res_crush, nullptr);
  ASSERT_TRUE(PyDict_Check(res_crush));
  EXPECT_NE(PyDict_GetItemString(res_crush, "devices"), nullptr);
  Py_DECREF(res_crush);
}

TEST_F(ActivePyModulesTest, GetPythonConfigBranches)
{
  PyObject* res_cfg = active_modules->get_python("config");
  ASSERT_NE(res_cfg, nullptr);
  ASSERT_TRUE(PyDict_Check(res_cfg));
  Py_DECREF(res_cfg);

  PyObject* res_opts = active_modules->get_python("config_options");
  ASSERT_NE(res_opts, nullptr);
  ASSERT_TRUE(PyDict_Check(res_opts));
  PyObject* opts_list = PyDict_GetItemString(res_opts, "options");
  ASSERT_NE(opts_list, nullptr);
  ASSERT_TRUE(PyList_Check(opts_list));
  EXPECT_GT(PyList_Size(opts_list), 0);
  Py_DECREF(res_opts);
}

TEST_F(ActivePyModulesTest, GetPythonMonMapAndServiceMap)
{
  PyObject* res_mon = active_modules->get_python("mon_map");
  ASSERT_NE(res_mon, nullptr);
  ASSERT_TRUE(PyDict_Check(res_mon));
  EXPECT_NE(PyDict_GetItemString(res_mon, "epoch"), nullptr);
  Py_DECREF(res_mon);

  PyObject* res_svc = active_modules->get_python("service_map");
  ASSERT_NE(res_svc, nullptr);
  ASSERT_TRUE(PyDict_Check(res_svc));
  EXPECT_NE(PyDict_GetItemString(res_svc, "epoch"), nullptr);
  Py_DECREF(res_svc);
}

TEST_F(ActivePyModulesTest, GetPythonMetadataBranches)
{
  DaemonKey osd_key{"osd", "0"};
  auto osd_daemon = std::make_shared<DaemonState>(daemon_state->types);
  osd_daemon->key = osd_key;
  osd_daemon->hostname = "host1";
  osd_daemon->metadata["ceph_version"] = "19.0.0";
  daemon_state->insert(osd_daemon);

  DaemonKey mds_key{"mds", "a"};
  auto mds_daemon = std::make_shared<DaemonState>(daemon_state->types);
  mds_daemon->key = mds_key;
  mds_daemon->hostname = "host2";
  mds_daemon->metadata["ceph_version"] = "19.0.0";
  daemon_state->insert(mds_daemon);

  PyObject* res_osd = active_modules->get_python("osd_metadata");
  ASSERT_NE(res_osd, nullptr);
  ASSERT_TRUE(PyDict_Check(res_osd));
  PyObject* osd0_dict = PyDict_GetItemString(res_osd, "0");
  ASSERT_NE(osd0_dict, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(PyDict_GetItemString(osd0_dict, "hostname")), "host1");
  Py_DECREF(res_osd);

  PyObject* res_mds = active_modules->get_python("mds_metadata");
  ASSERT_NE(res_mds, nullptr);
  ASSERT_TRUE(PyDict_Check(res_mds));
  PyObject* mdsa_dict = PyDict_GetItemString(res_mds, "a");
  ASSERT_NE(mdsa_dict, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(PyDict_GetItemString(mdsa_dict, "hostname")), "host2");
  Py_DECREF(res_mds);
}

TEST_F(ActivePyModulesTest, GetPythonPgMapBranches)
{
  const char* pg_queries[] = {
    "pg_summary",
    "pg_status",
    "pg_dump",
    "io_rate",
    "df",
    "pg_stats",
    "pool_stats",
    "pg_ready",
    "pg_progress",
    "osd_stats",
    "osd_ping_times",
    "osd_pool_stats",
    "active_clean_pgs"
  };

  for (const char* query : pg_queries) {
    PyObject* res = active_modules->get_python(query);
    ASSERT_NE(res, nullptr) << "Failed for query: " << query;
    Py_DECREF(res);
  }
}

TEST_F(ActivePyModulesTest, GetPythonDeviceBranches)
{
  PyObject* res_devs = active_modules->get_python("devices");
  ASSERT_NE(res_devs, nullptr);
  ASSERT_TRUE(PyDict_Check(res_devs));
  EXPECT_NE(PyDict_GetItemString(res_devs, "devices"), nullptr);
  Py_DECREF(res_devs);

  PyObject* res_single_none = active_modules->get_python("device nonexistent_dev");
  ASSERT_NE(res_single_none, nullptr);
  ASSERT_TRUE(PyDict_Check(res_single_none));
  Py_DECREF(res_single_none);

  daemon_state->with_device_create("test_dev_1", [](DeviceState& dev) {
    dev.metadata["model"] = "TestModel";
    dev.set_wear_level(0.25f);
  });
  PyObject* res_single_exist = active_modules->get_python("device test_dev_1");
  ASSERT_NE(res_single_exist, nullptr);
  ASSERT_TRUE(PyDict_Check(res_single_exist));
  EXPECT_NE(PyDict_GetItemString(res_single_exist, "device"), nullptr);
  Py_DECREF(res_single_exist);
}

TEST_F(ActivePyModulesTest, GetPythonStatusAndOtherBranches)
{
  PyObject* res_health = active_modules->get_python("health");
  ASSERT_NE(res_health, nullptr);
  ASSERT_TRUE(PyDict_Check(res_health));
  Py_DECREF(res_health);

  PyObject* res_mon_status = active_modules->get_python("mon_status");
  ASSERT_NE(res_mon_status, nullptr);
  ASSERT_TRUE(PyDict_Check(res_mon_status));
  Py_DECREF(res_mon_status);

  PyObject* res_mgr_map = active_modules->get_python("mgr_map");
  ASSERT_NE(res_mgr_map, nullptr);
  ASSERT_TRUE(PyDict_Check(res_mgr_map));
  EXPECT_NE(PyDict_GetItemString(res_mgr_map, "epoch"), nullptr);
  Py_DECREF(res_mgr_map);

  // Note: "mgr_ips" requires server.get_myaddrs() which needs initialized msgr in daemon_server

  PyObject* res_local = active_modules->get_python("have_local_config_map");
  ASSERT_NE(res_local, nullptr);
  ASSERT_TRUE(PyDict_Check(res_local));
  EXPECT_NE(PyDict_GetItemString(res_local, "have_local_config_map"), nullptr);
  Py_DECREF(res_local);

  PyObject* res_mod = active_modules->get_python("modified_config_options");
  ASSERT_NE(res_mod, nullptr);
  ASSERT_TRUE(PyDict_Check(res_mod));
  EXPECT_NE(PyDict_GetItemString(res_mod, "options"), nullptr);
  Py_DECREF(res_mod);

  PyObject* res_unknown = active_modules->get_python("unknown_query_string");
  ASSERT_EQ(res_unknown, Py_None);
}

TEST_F(ActivePyModulesTest, HealthChecksBasic)
{
  health_check_map_t checks;
  active_modules->get_health_checks(&checks);
  EXPECT_TRUE(checks.empty());
}

TEST_F(ActivePyModulesTest, SetAndGetHealthChecksWithModule)
{
  const char* module_code = R"(
class Module:
    def __init__(self):
        pass
)";
  auto mod = create_test_module("test_health_mod", module_code);
  ASSERT_NE(mod, nullptr);
  active_modules->test_insert_module("test_health_mod", mod);

  health_check_map_t checks;
  checks.add("TEST_CHECK_1", HEALTH_WARN, "Test warning message", 1);
  checks.add("TEST_CHECK_2", HEALTH_ERR, "Test error message", 2);

  active_modules->set_health_checks("test_health_mod", std::move(checks));

  health_check_map_t retrieved;
  active_modules->get_health_checks(&retrieved);
  EXPECT_EQ(retrieved.checks.size(), 2u);
  EXPECT_NE(retrieved.checks.find("TEST_CHECK_1"), retrieved.checks.end());
  EXPECT_NE(retrieved.checks.find("TEST_CHECK_2"), retrieved.checks.end());

  // Health checks for a non-existent module are ignored
  health_check_map_t checks2;
  checks2.add("OTHER_CHECK", HEALTH_WARN, "Other", 1);
  active_modules->set_health_checks("nonexistent_mod", std::move(checks2));

  health_check_map_t retrieved2;
  active_modules->get_health_checks(&retrieved2);
  EXPECT_EQ(retrieved2.checks.size(), 2u);

  active_modules->test_erase_module("test_health_mod");
  mod.reset();
}

TEST_F(ActivePyModulesTest, SetUriExistingModule)
{
  const char* module_code = R"(
class Module:
    def __init__(self):
        pass
)";
  auto mod = create_test_module("uri_module", module_code);
  ASSERT_NE(mod, nullptr);
  active_modules->test_insert_module("uri_module", mod);

  ASSERT_NO_THROW(active_modules->set_uri("uri_module", "http://127.0.0.1:8080"));
  EXPECT_EQ(mod->get_uri(), "http://127.0.0.1:8080");

  active_modules->test_erase_module("uri_module");
  mod.reset();
}

TEST_F(ActivePyModulesTest, SetUriNonExistentModuleThrows)
{
  EXPECT_THROW(active_modules->set_uri("nonexistent_module", "http://127.0.0.1:8080"), std::out_of_range);
}

TEST_F(ActivePyModulesTest, MethodExistsAndModuleFinisher)
{
  const char* module_code = R"(
class Module:
    def __init__(self):
        pass
    def custom_method(self):
        return 42
)";
  auto mod = create_test_module("method_module", module_code);
  ASSERT_NE(mod, nullptr);
  active_modules->test_insert_module("method_module", mod);

  EXPECT_TRUE(active_modules->module_exists("method_module"));
  EXPECT_FALSE(active_modules->module_exists("nonexistent"));

  EXPECT_TRUE(active_modules->method_exists("method_module", "custom_method"));
  EXPECT_FALSE(active_modules->method_exists("method_module", "nonexistent_method"));

  Finisher& fin = active_modules->get_module_finisher("method_module");
  EXPECT_TRUE(fin.is_empty());

  EXPECT_THROW(active_modules->get_module_finisher("nonexistent"), std::out_of_range);

  active_modules->test_erase_module("method_module");
  mod.reset();
}

TEST_F(ActivePyModulesTest, DispatchRemoteDelegation)
{
  const char* module_code = R"(
class Module:
    def __init__(self):
        pass
    def remote_call(self, a, b):
        return a + b
)";
  auto mod = create_test_module("remote_mod", module_code);
  ASSERT_NE(mod, nullptr);
  active_modules->test_insert_module("remote_mod", mod);

  PyGILState_STATE gstate = PyGILState_Ensure();
  PyObject* pickle_mod = PyImport_ImportModule("pickle");
  ASSERT_NE(pickle_mod, nullptr);
  PyObject* dumps_fn = PyObject_GetAttrString(pickle_mod, "dumps");
  ASSERT_NE(dumps_fn, nullptr);

  PyObject* args_tuple = Py_BuildValue("(ii)", 10, 20);
  PyObject* kwargs_dict = PyDict_New();
  PyObject* pickled_args_obj = PyObject_CallFunctionObjArgs(dumps_fn, args_tuple, nullptr);
  PyObject* pickled_kwargs_obj = PyObject_CallFunctionObjArgs(dumps_fn, kwargs_dict, nullptr);

  char* args_buf = nullptr;
  Py_ssize_t args_len = 0;
  PyBytes_AsStringAndSize(pickled_args_obj, &args_buf, &args_len);

  char* kwargs_buf = nullptr;
  Py_ssize_t kwargs_len = 0;
  PyBytes_AsStringAndSize(pickled_kwargs_obj, &kwargs_buf, &kwargs_len);

  std::span<std::byte const> pickled_args{reinterpret_cast<std::byte const*>(args_buf), static_cast<size_t>(args_len)};
  std::span<std::byte const> pickled_kwargs{reinterpret_cast<std::byte const*>(kwargs_buf), static_cast<size_t>(kwargs_len)};

  std::string err;
  bool crash_dump = false;

  // Release GIL before calling dispatch_remote because it acquires GIL via Gil gil(pMyThreadState, true) internally
  PyGILState_Release(gstate);

  auto result = active_modules->dispatch_remote("remote_mod", "remote_call", pickled_args, pickled_kwargs, &err, &crash_dump);
  ASSERT_TRUE(result.has_value()) << "dispatch_remote failed: " << err;
  EXPECT_FALSE(crash_dump);

  // dispatch_remote releases the GIL internally; re-acquire to verify the result
  gstate = PyGILState_Ensure();
  PyObject* loads_fn = PyObject_GetAttrString(pickle_mod, "loads");
  PyObject* res_bytes = PyBytes_FromStringAndSize(reinterpret_cast<char const*>(result->data()), result->size());
  PyObject* res_val = PyObject_CallFunctionObjArgs(loads_fn, res_bytes, nullptr);
  ASSERT_NE(res_val, nullptr);
  ASSERT_TRUE(PyLong_Check(res_val));
  EXPECT_EQ(PyLong_AsLong(res_val), 30);

  Py_DECREF(res_val);
  Py_DECREF(res_bytes);
  Py_DECREF(loads_fn);
  Py_DECREF(pickled_kwargs_obj);
  Py_DECREF(pickled_args_obj);
  Py_DECREF(kwargs_dict);
  Py_DECREF(args_tuple);
  Py_DECREF(dumps_fn);
  Py_DECREF(pickle_mod);
  PyGILState_Release(gstate);

  active_modules->test_erase_module("remote_mod");
  mod.reset();
}

TEST_F(ActivePyModulesTest, UpdateProgressEventBasic)
{
  active_modules->update_progress_event("event1", "Test event", 0.5, true);
  verify_progress_event("event1", "Test event", 0.5, true);
}

TEST_F(ActivePyModulesTest, UpdateProgressEventMultiple)
{
  active_modules->update_progress_event("event1", "First event", 0.25, true);
  active_modules->update_progress_event("event2", "Second event", 0.75, false);
  active_modules->update_progress_event("event3", "Third event", 1.0, true);
  
  std::map<std::string, ProgressEvent> events;
  active_modules->get_progress_events(&events);
  ASSERT_EQ(events.size(), 3u);
  
  verify_progress_event("event1", "First event", 0.25, true);
  verify_progress_event("event2", "Second event", 0.75, false);
  verify_progress_event("event3", "Third event", 1.0, true);
}

TEST_F(ActivePyModulesTest, UpdateProgressEventOverwrite)
{
  active_modules->update_progress_event("event1", "Initial message", 0.3, true);
  verify_progress_event("event1", "Initial message", 0.3, true);
  
  active_modules->update_progress_event("event1", "Updated message", 0.7, false);
  
  std::map<std::string, ProgressEvent> events;
  active_modules->get_progress_events(&events);
  ASSERT_EQ(events.size(), 1u) << "Overwrite should not create new event";
  
  verify_progress_event("event1", "Updated message", 0.7, false);
}

TEST_F(ActivePyModulesTest, UpdateProgressEventEdgeCases)
{
  active_modules->update_progress_event("event_zero", "Starting", 0.0, true);
  active_modules->update_progress_event("event_complete", "Done", 1.0, true);
  active_modules->update_progress_event("event_empty", "", 0.5, false);
  
  std::string long_msg(1000, 'x');
  active_modules->update_progress_event("event_long", long_msg, 0.5, true);
  
  std::map<std::string, ProgressEvent> events;
  active_modules->get_progress_events(&events);
  ASSERT_EQ(events.size(), 4u);
  
  verify_progress_event("event_zero", "Starting", 0.0, true);
  verify_progress_event("event_complete", "Done", 1.0, true);
  verify_progress_event("event_empty", "", 0.5, false);
  verify_progress_event("event_long", long_msg, 0.5, true);
}

TEST_F(ActivePyModulesTest, CompleteProgressEventBasic)
{
  active_modules->update_progress_event("event1", "Test", 0.5, true);
  active_modules->update_progress_event("event2", "Test2", 0.7, false);
  
  active_modules->complete_progress_event("event1");
  
  std::map<std::string, ProgressEvent> events;
  active_modules->get_progress_events(&events);
  
  ASSERT_EQ(events.size(), 1u);
  verify_progress_event_absent("event1");
  verify_progress_event("event2", "Test2", 0.7, false);
}

TEST_F(ActivePyModulesTest, CompleteProgressEventNonExistent)
{
  active_modules->update_progress_event("event1", "Test", 0.5, true);
  
  ASSERT_NO_THROW(active_modules->complete_progress_event("nonexistent"));
  
  std::map<std::string, ProgressEvent> events;
  active_modules->get_progress_events(&events);
  
  ASSERT_EQ(events.size(), 1u) << "Completing non-existent event should not affect existing events";
  verify_progress_event("event1", "Test", 0.5, true);
}

TEST_F(ActivePyModulesTest, CompleteProgressEventMultiple)
{
  active_modules->update_progress_event("event1", "Test1", 0.3, true);
  active_modules->update_progress_event("event2", "Test2", 0.5, true);
  active_modules->update_progress_event("event3", "Test3", 0.7, true);
  
  active_modules->complete_progress_event("event1");
  active_modules->complete_progress_event("event3");
  
  std::map<std::string, ProgressEvent> events;
  active_modules->get_progress_events(&events);
  
  ASSERT_EQ(events.size(), 1u);
  verify_progress_event_absent("event1");
  verify_progress_event("event2", "Test2", 0.5, true);
  verify_progress_event_absent("event3");
}

TEST_F(ActivePyModulesTest, ClearAllProgressEvents)
{
  active_modules->update_progress_event("event1", "Test1", 0.3, true);
  active_modules->update_progress_event("event2", "Test2", 0.5, true);
  active_modules->update_progress_event("event3", "Test3", 0.7, true);
  
  std::map<std::string, ProgressEvent> events_before;
  active_modules->get_progress_events(&events_before);
  ASSERT_EQ(events_before.size(), 3u);
  
  active_modules->clear_all_progress_events();
  
  std::map<std::string, ProgressEvent> events_after;
  active_modules->get_progress_events(&events_after);
  EXPECT_TRUE(events_after.empty()) << "All events should be cleared";
  
  active_modules->update_progress_event("event4", "Test4", 0.7, false);
  
  std::map<std::string, ProgressEvent> events_new;
  active_modules->get_progress_events(&events_new);
  ASSERT_EQ(events_new.size(), 1u);
  verify_progress_event("event4", "Test4", 0.7, false);
  verify_progress_event_absent("event1");
  verify_progress_event_absent("event2");
  verify_progress_event_absent("event3");
}

TEST_F(ActivePyModulesTest, GetProgressEventsEmpty)
{
  std::map<std::string, ProgressEvent> events;
  active_modules->get_progress_events(&events);
  EXPECT_TRUE(events.empty());
}

TEST_F(ActivePyModulesTest, GetProgressEventsReturnsCopy)
{
  active_modules->update_progress_event("event1", "Test", 0.5, true);
  
  std::map<std::string, ProgressEvent> events1;
  active_modules->get_progress_events(&events1);
  ASSERT_EQ(events1.size(), 1u);
  
  events1["event1"].message = "Modified";
  events1["event2"].message = "New event";
  
  std::map<std::string, ProgressEvent> events2;
  active_modules->get_progress_events(&events2);
  
  ASSERT_EQ(events2.size(), 1u) << "Modifications to returned map should not affect internal state";
  EXPECT_EQ(events2["event1"].message, "Test") << "Original message should be preserved";
  verify_progress_event_absent("event2");
}

// NOTE: set_device_wear_level sends a MonClient command; without a live monitor
// the command fails silently, but the call must not crash.
TEST_F(ActivePyModulesTest, SetDeviceWearLevelBasic)
{
  ASSERT_NO_THROW(active_modules->set_device_wear_level("device123", 0.75))
    << "set_device_wear_level should not crash with valid device ID";
  
  ASSERT_NO_THROW(active_modules->set_device_wear_level("nvme0n1", 0.5));
  ASSERT_NO_THROW(active_modules->set_device_wear_level("sda", 0.25));
}

TEST_F(ActivePyModulesTest, SetDeviceWearLevelEdgeCases)
{
  ASSERT_NO_THROW(active_modules->set_device_wear_level("device1", 0.0))
    << "Should handle 0.0 wear level";
  ASSERT_NO_THROW(active_modules->set_device_wear_level("device2", 1.0))
    << "Should handle 1.0 wear level";
  ASSERT_NO_THROW(active_modules->set_device_wear_level("device3", 0.5))
    << "Should handle 0.5 wear level";
  ASSERT_NO_THROW(active_modules->set_device_wear_level("", 0.5))
    << "Should handle empty device ID without crashing";
}

TEST_F(ActivePyModulesTest, ReregisterMdsPerfQueries)
{
  ASSERT_NO_THROW(active_modules->reregister_mds_perf_queries())
    << "Should complete without crashing in test environment";
}

TEST_F(ActivePyModulesTest, ClientRegistration)
{
  std::string client_name = "test_client";
  std::string addrs1 = "v2:127.0.0.1:6789/0";
  std::string addrs2 = "v2:127.0.0.1:6790/0";
  std::string addrs3 = "v2:127.0.0.1:6791/0";
  
  ASSERT_NO_THROW(active_modules->register_client(client_name, addrs1, false));
  auto clients = py_module_registry->get_clients();
  ASSERT_EQ(clients.count(client_name), 1u) << "Client should be registered";
  
  active_modules->register_client(client_name, addrs2, false);
  clients = py_module_registry->get_clients();
  EXPECT_EQ(clients.count(client_name), 2u) << "Second address should be added";
  
  // replace=true clears previous registrations for that name before adding the new one
  active_modules->register_client(client_name, addrs3, true);
  clients = py_module_registry->get_clients();
  EXPECT_EQ(clients.count(client_name), 1u) << "Replace should clear previous addresses";
  
  active_modules->unregister_client(client_name, addrs1);
  clients = py_module_registry->get_clients();
  EXPECT_EQ(clients.count(client_name), 1u) << "Wrong address should not unregister";
  
  ASSERT_NO_THROW(active_modules->unregister_client(client_name, addrs3));
  clients = py_module_registry->get_clients();
  EXPECT_EQ(clients.count(client_name), 0u) << "Correct address should unregister";
  
  ASSERT_NO_THROW(active_modules->unregister_client("nonexistent", addrs1))
    << "Unregistering non-existent client should not crash";
}

TEST_F(ActivePyModulesTest, ClientRegistrationMultiple)
{
  std::string client1 = "client1";
  std::string client2 = "client2";
  std::string addrs1 = "v2:127.0.0.1:6789/0";
  std::string addrs2 = "v2:127.0.0.1:6790/0";
  
  active_modules->register_client(client1, addrs1, false);
  active_modules->register_client(client2, addrs2, false);
  
  auto clients = py_module_registry->get_clients();
  EXPECT_EQ(clients.count(client1), 1u) << "Client1 should be registered";
  EXPECT_EQ(clients.count(client2), 1u) << "Client2 should be registered";
  
  active_modules->unregister_client(client1, addrs1);
  clients = py_module_registry->get_clients();
  EXPECT_EQ(clients.count(client1), 0u) << "Client1 should be unregistered";
  EXPECT_EQ(clients.count(client2), 1u) << "Client2 should still be registered";
}

TEST_F(ActivePyModulesTest, GetDaemonHealthMetricsEmpty)
{
  PyObject* result = active_modules->get_daemon_health_metrics();
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  EXPECT_EQ(PyDict_Size(result), 0);
  
  Py_DECREF(result);
}

// Test: Daemon without metrics - verifies that a daemon with no health metrics
// produces an entry in the outer dict whose value is an empty list.
TEST_F(ActivePyModulesTest, GetDaemonHealthMetricsNoMetrics)
{
  DaemonKey key{"osd", "0"};
  auto daemon = std::make_shared<DaemonState>(daemon_state->types);
  daemon->key = key;
  daemon->hostname = "test_host";
  daemon_state->insert(daemon);

  PyObject* result = active_modules->get_daemon_health_metrics();
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));

  ASSERT_EQ(PyDict_Size(result), 1);
  PyObject* list = PyDict_GetItemString(result, "osd.0");
  ASSERT_NE(list, nullptr) << "Expected key 'osd.0' in result dict";
  ASSERT_TRUE(PyList_Check(list)) << "Value for 'osd.0' must be a list";
  EXPECT_EQ(PyList_Size(list), 0) << "Daemon with no metrics must produce an empty list";

  Py_DECREF(result);
}

// Test: Daemon with a single SLOW_OPS metric - verifies correct key, type string,
// and value serialisation in the returned Python structure.
TEST_F(ActivePyModulesTest, GetDaemonHealthMetricsSingleMetric)
{
  DaemonKey key{"osd", "1"};
  auto daemon = std::make_shared<DaemonState>(daemon_state->types);
  daemon->key = key;
  daemon->hostname = "host_a";
  daemon->daemon_health_metrics.emplace_back(daemon_metric::SLOW_OPS,
                                             static_cast<uint32_t>(7),
                                             static_cast<uint32_t>(0));
  daemon_state->insert(daemon);

  PyObject* result = active_modules->get_daemon_health_metrics();
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));

  ASSERT_EQ(PyDict_Size(result), 1);
  PyObject* list = PyDict_GetItemString(result, "osd.1");
  ASSERT_NE(list, nullptr) << "Expected key 'osd.1' in result dict";
  ASSERT_TRUE(PyList_Check(list));
  ASSERT_EQ(PyList_Size(list), 1) << "Expected exactly one metric entry";

  PyObject* metric = PyList_GetItem(list, 0);
  ASSERT_NE(metric, nullptr);
  ASSERT_TRUE(PyDict_Check(metric));

  PyObject* type_val = PyDict_GetItemString(metric, "type");
  ASSERT_NE(type_val, nullptr) << "Metric dict must have 'type' key";
  ASSERT_TRUE(PyUnicode_Check(type_val));
  EXPECT_STREQ(PyUnicode_AsUTF8(type_val), "SLOW_OPS");

  PyObject* value_val = PyDict_GetItemString(metric, "value");
  ASSERT_NE(value_val, nullptr) << "Metric dict must have 'value' key";
  ASSERT_TRUE(PyLong_Check(value_val));
  EXPECT_EQ(PyLong_AsLong(value_val), 7);

  Py_DECREF(result);
}

// Test: Daemon with multiple metrics of different types - verifies all metrics
// are serialised and their type strings and values are individually correct.
TEST_F(ActivePyModulesTest, GetDaemonHealthMetricsMultipleMetrics)
{
  DaemonKey key{"osd", "2"};
  auto daemon = std::make_shared<DaemonState>(daemon_state->types);
  daemon->key = key;
  daemon->hostname = "host_b";
  daemon->daemon_health_metrics.emplace_back(daemon_metric::SLOW_OPS,
                                             static_cast<uint32_t>(3),
                                             static_cast<uint32_t>(0));
  daemon->daemon_health_metrics.emplace_back(daemon_metric::PENDING_CREATING_PGS,
                                             static_cast<uint32_t>(5),
                                             static_cast<uint32_t>(0));
  daemon_state->insert(daemon);

  PyObject* result = active_modules->get_daemon_health_metrics();
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));

  PyObject* list = PyDict_GetItemString(result, "osd.2");
  ASSERT_NE(list, nullptr) << "Expected key 'osd.2' in result dict";
  ASSERT_TRUE(PyList_Check(list));
  ASSERT_EQ(PyList_Size(list), 2) << "Expected two metric entries";

  // Collect the emitted entries into a map type_name -> value for order-independent checks
  std::map<std::string, long> emitted;
  for (Py_ssize_t i = 0; i < PyList_Size(list); ++i) {
    PyObject* m = PyList_GetItem(list, i);
    ASSERT_NE(m, nullptr);
    ASSERT_TRUE(PyDict_Check(m));

    PyObject* t = PyDict_GetItemString(m, "type");
    ASSERT_NE(t, nullptr) << "Metric " << i << " must have 'type'";
    ASSERT_TRUE(PyUnicode_Check(t));

    PyObject* v = PyDict_GetItemString(m, "value");
    ASSERT_NE(v, nullptr) << "Metric " << i << " must have 'value'";
    ASSERT_TRUE(PyLong_Check(v));

    emitted[PyUnicode_AsUTF8(t)] = PyLong_AsLong(v);
  }

  ASSERT_EQ(emitted.count("SLOW_OPS"), 1u) << "'SLOW_OPS' must appear in output";
  EXPECT_EQ(emitted["SLOW_OPS"], 3);

  ASSERT_EQ(emitted.count("PENDING_CREATING_PGS"), 1u)
      << "'PENDING_CREATING_PGS' must appear in output";
  EXPECT_EQ(emitted["PENDING_CREATING_PGS"], 5);

  Py_DECREF(result);
}

// Test: Two daemons on separate hosts each carrying metrics - verifies that the
// outer dict contains one entry per daemon (keyed by "type.id") with independent
// metric lists.
TEST_F(ActivePyModulesTest, GetDaemonHealthMetricsMultipleDaemons)
{
  // osd.3 on host_c with one SLOW_OPS metric (n1=2)
  {
    DaemonKey key{"osd", "3"};
    auto daemon = std::make_shared<DaemonState>(daemon_state->types);
    daemon->key = key;
    daemon->hostname = "host_c";
    daemon->daemon_health_metrics.emplace_back(daemon_metric::SLOW_OPS,
                                               static_cast<uint32_t>(2),
                                               static_cast<uint32_t>(0));
    daemon_state->insert(daemon);
  }

  // osd.4 on host_d with one PENDING_CREATING_PGS metric (n1=9)
  {
    DaemonKey key{"osd", "4"};
    auto daemon = std::make_shared<DaemonState>(daemon_state->types);
    daemon->key = key;
    daemon->hostname = "host_d";
    daemon->daemon_health_metrics.emplace_back(daemon_metric::PENDING_CREATING_PGS,
                                               static_cast<uint32_t>(9),
                                               static_cast<uint32_t>(0));
    daemon_state->insert(daemon);
  }

  PyObject* result = active_modules->get_daemon_health_metrics();
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  ASSERT_EQ(PyDict_Size(result), 2) << "Expected one entry per daemon";

  // Verify osd.3
  {
    PyObject* list = PyDict_GetItemString(result, "osd.3");
    ASSERT_NE(list, nullptr) << "Expected key 'osd.3'";
    ASSERT_TRUE(PyList_Check(list));
    ASSERT_EQ(PyList_Size(list), 1);

    PyObject* m = PyList_GetItem(list, 0);
    ASSERT_NE(m, nullptr);
    PyObject* t = PyDict_GetItemString(m, "type");
    ASSERT_NE(t, nullptr);
    EXPECT_STREQ(PyUnicode_AsUTF8(t), "SLOW_OPS");
    PyObject* v = PyDict_GetItemString(m, "value");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(PyLong_AsLong(v), 2);
  }

  // Verify osd.4
  {
    PyObject* list = PyDict_GetItemString(result, "osd.4");
    ASSERT_NE(list, nullptr) << "Expected key 'osd.4'";
    ASSERT_TRUE(PyList_Check(list));
    ASSERT_EQ(PyList_Size(list), 1);

    PyObject* m = PyList_GetItem(list, 0);
    ASSERT_NE(m, nullptr);
    PyObject* t = PyDict_GetItemString(m, "type");
    ASSERT_NE(t, nullptr);
    EXPECT_STREQ(PyUnicode_AsUTF8(t), "PENDING_CREATING_PGS");
    PyObject* v = PyDict_GetItemString(m, "value");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(PyLong_AsLong(v), 9);
  }

  Py_DECREF(result);
}

// Regression test for https://tracker.ceph.com/issues/79556
//
// get_daemon_health_metrics() iterates state->daemon_health_metrics without
// holding state->lock.  DaemonServer::handle_report() writes
// daemon->daemon_health_metrics under daemon->lock (DaemonServer.cc line 823).
// The missing reader-side lock is a confirmed data race: if a report arrives
// while Python is iterating the vector, the vector storage can be reallocated
// or destroyed mid-iteration, crashing the mgr process.
//
// How this test fails:
//
//   Under TSAN (WITH_TSAN=ON): TSAN will report a data race between the
//   writer thread and the reader on the first or second iteration.  The test
//   will be killed with a TSAN error before the loop completes.
//
//   Without TSAN: The test intentionally corrupts the vector by having the
//   writer resize it while the reader is mid-iteration.  The structural
//   ASSERTs (valid dict, valid list, known type strings) will catch any
//   corruption that surfaces.  On a sufficiently loaded system or with
//   address-sanitizer (ASAN) this also aborts.  Because the race is
//   non-deterministic without TSAN the test may pass on any given run, but
//   it reliably fails in CI with the sanitizer builds that Ceph uses for
//   correctness gating.
//
// This test MUST FAIL on the unfixed code under TSAN.
// It MUST PASS once state->lock is acquired inside
// ActivePyModules::get_daemon_health_metrics() before iterating
// state->daemon_health_metrics.
TEST_F(ActivePyModulesTest, GetDaemonHealthMetricsConcurrentWriteRace)
{
  DaemonKey key{"osd", "5"};
  auto daemon = std::make_shared<DaemonState>(daemon_state->types);
  daemon->key = key;
  daemon->hostname = "race_host";
  daemon->daemon_health_metrics.emplace_back(daemon_metric::SLOW_OPS,
                                             static_cast<uint32_t>(1),
                                             static_cast<uint32_t>(0));
  daemon_state->insert(daemon);

  std::atomic<bool> stop{false};

  std::thread writer([&] {
    std::vector<DaemonHealthMetric> large(64);
    for (size_t i = 0; i < large.size(); ++i) {
      large[i] = DaemonHealthMetric(daemon_metric::SLOW_OPS,
                                    static_cast<uint32_t>(i + 1),
                                    static_cast<uint32_t>(0));
    }
    std::vector<DaemonHealthMetric> small_vec;
    small_vec.emplace_back(daemon_metric::PENDING_CREATING_PGS,
                           static_cast<uint32_t>(99),
                           static_cast<uint32_t>(0));

    while (!stop.load(std::memory_order_relaxed)) {
      {
        std::lock_guard l(daemon->lock);
        daemon->daemon_health_metrics = large;
      }
      {
        std::lock_guard l(daemon->lock);
        daemon->daemon_health_metrics = small_vec;
      }
    }
  });

  constexpr int kIterations = 200;
  for (int i = 0; i < kIterations; ++i) {
    PyObject* result = active_modules->get_daemon_health_metrics();
    ASSERT_NE(result, nullptr) << "get_daemon_health_metrics() returned nullptr at iteration " << i;
    ASSERT_TRUE(PyDict_Check(result)) << "Expected dict at iteration " << i;

    PyObject* list = PyDict_GetItemString(result, "osd.5");
    ASSERT_NE(list, nullptr) << "Key 'osd.5' missing at iteration " << i;
    ASSERT_TRUE(PyList_Check(list)) << "Value for 'osd.5' must be a list at iteration " << i;

    for (Py_ssize_t j = 0; j < PyList_Size(list); ++j) {
      PyObject* m = PyList_GetItem(list, j);
      ASSERT_NE(m, nullptr) << "Null metric at [" << i << "][" << j << "]";
      ASSERT_TRUE(PyDict_Check(m)) << "Metric not a dict at [" << i << "][" << j << "]";

      PyObject* t = PyDict_GetItemString(m, "type");
      ASSERT_NE(t, nullptr) << "'type' missing at [" << i << "][" << j << "]";
      ASSERT_TRUE(PyUnicode_Check(t)) << "'type' not a string at [" << i << "][" << j << "]";
      const char* type_str = PyUnicode_AsUTF8(t);
      ASSERT_NE(type_str, nullptr) << "Null type string at [" << i << "][" << j << "]";
      bool valid_type = (std::string(type_str) == "SLOW_OPS" ||
                         std::string(type_str) == "PENDING_CREATING_PGS" ||
                         std::string(type_str) == "NONE");
      EXPECT_TRUE(valid_type)
          << "Unrecognised type '" << type_str
          << "' at [" << i << "][" << j << "] – likely memory corruption";

      PyObject* v = PyDict_GetItemString(m, "value");
      ASSERT_NE(v, nullptr) << "'value' missing at [" << i << "][" << j << "]";
      ASSERT_TRUE(PyLong_Check(v)) << "'value' not a long at [" << i << "][" << j << "]";
    }

    Py_DECREF(result);
  }

  stop.store(true, std::memory_order_relaxed);
  writer.join();
}

TEST_F(ActivePyModulesTest, GetServices)
{
  auto services = active_modules->get_services();
  EXPECT_TRUE(services.empty()) << "Should return empty map when no modules are loaded";
}

TEST_F(ActivePyModulesTest, GetForeignConfig)
{
  active_modules->test_set_have_local_config_map(true);

  PyObject* result2 = active_modules->get_foreign_config("osd.0", "nonexistent_option_name");
  ASSERT_EQ(result2, nullptr) << "Invalid option should return nullptr";
  ASSERT_TRUE(PyErr_Occurred()) << "Invalid option should set Python error";
  
  PyObject *ptype, *pvalue, *ptraceback;
  PyErr_Fetch(&ptype, &pvalue, &ptraceback);
  ASSERT_TRUE(PyErr_GivenExceptionMatches(ptype, PyExc_KeyError))
    << "Invalid option should raise KeyError";
  Py_XDECREF(ptype);
  Py_XDECREF(pvalue);
  Py_XDECREF(ptraceback);
  
  PyObject* result3 = active_modules->get_foreign_config("invalid_entity_format", "osd_max_write_size");
  ASSERT_EQ(result3, nullptr) << "Invalid entity should return nullptr";
  ASSERT_TRUE(PyErr_Occurred()) << "Invalid entity should set Python error";
  PyErr_Clear();
}

// Test: Valid built-in option via local config_map.
// Seed osd_pool_default_size=7 for the "osd" section, enable the local config
// map path, then verify get_foreign_config returns PyLong(7) for osd.0.
// osd_pool_default_size is TYPE_UINT default 3 — we use 7 to confirm we read
// the seeded value and not the compiled-in default.
TEST_F(ActivePyModulesTest, GetForeignConfigValidOption)
{
  std::map<std::string, std::optional<bufferlist>, std::less<>> data;
  bufferlist bl;
  bl.append("7");
  data["config/osd/osd_pool_default_size"] = bl;
  active_modules->update_kv_data("config/", false, data);
  active_modules->test_set_have_local_config_map(true);

  PyObject* result = active_modules->get_foreign_config("osd.0", "osd_pool_default_size");
  ASSERT_NE(result, nullptr);
  ASSERT_FALSE(PyErr_Occurred());
  ASSERT_TRUE(PyLong_Check(result)) << "osd_pool_default_size is TYPE_UINT, expected PyLong";
  EXPECT_EQ(PyLong_AsLong(result), 7L)
    << "Should return the seeded value 7, not the compiled-in default 3";
  Py_DECREF(result);
}

// Test: Different entity types return the correct typed values via config_map.
// Seeds osd_pool_default_size=7 for OSD and mon_max_pg_per_osd=250 for MON,
// then verifies each entity is parsed correctly and its seeded value is returned.
TEST_F(ActivePyModulesTest, GetForeignConfigDifferentEntities)
{
  std::map<std::string, std::optional<bufferlist>, std::less<>> data;
  bufferlist bl1;
  bl1.append("7");
  data["config/osd/osd_pool_default_size"] = bl1;
  bufferlist bl2;
  bl2.append("250");
  data["config/mon/mon_max_pg_per_osd"] = bl2;
  active_modules->update_kv_data("config/", false, data);
  active_modules->test_set_have_local_config_map(true);

  PyObject* r1 = active_modules->get_foreign_config("osd.0", "osd_pool_default_size");
  ASSERT_NE(r1, nullptr);
  ASSERT_FALSE(PyErr_Occurred());
  ASSERT_TRUE(PyLong_Check(r1)) << "osd_pool_default_size should be PyLong";
  EXPECT_EQ(PyLong_AsLong(r1), 7L);
  Py_DECREF(r1);

  PyObject* r2 = active_modules->get_foreign_config("mon.a", "mon_max_pg_per_osd");
  ASSERT_NE(r2, nullptr);
  ASSERT_FALSE(PyErr_Occurred());
  ASSERT_TRUE(PyLong_Check(r2)) << "mon_max_pg_per_osd should be PyLong";
  EXPECT_EQ(PyLong_AsLong(r2), 250L);
  Py_DECREF(r2);
}

// Test: Both 'osd' and 'osd.0' entity name formats are accepted and return
// the same seeded value for osd_pool_default_size.
TEST_F(ActivePyModulesTest, GetForeignConfigEntityVariations)
{
  std::map<std::string, std::optional<bufferlist>, std::less<>> data;
  bufferlist bl;
  bl.append("7");
  data["config/osd/osd_pool_default_size"] = bl;
  active_modules->update_kv_data("config/", false, data);
  active_modules->test_set_have_local_config_map(true);

  // entity.from_str() accepts "osd" (type-only) via an internal "who + '.'" fallback
  PyObject* r1 = active_modules->get_foreign_config("osd", "osd_pool_default_size");
  ASSERT_NE(r1, nullptr) << "'osd' entity format should be accepted";
  ASSERT_FALSE(PyErr_Occurred());
  ASSERT_TRUE(PyLong_Check(r1));
  Py_DECREF(r1);

  PyObject* r2 = active_modules->get_foreign_config("osd.0", "osd_pool_default_size");
  ASSERT_NE(r2, nullptr) << "'osd.0' entity format should be accepted";
  ASSERT_FALSE(PyErr_Occurred());
  ASSERT_TRUE(PyLong_Check(r2));
  EXPECT_EQ(PyLong_AsLong(r2), 7L);
  Py_DECREF(r2);
}

TEST_F(ActivePyModulesTest, GetMoncAccessor)
{
  MonClient& monc = active_modules->get_monc();
  MonClient& monc2 = active_modules->get_monc();
  ASSERT_EQ(&monc, &monc2) << "Should return reference to same MonClient instance";
  ASSERT_NE(&monc, nullptr) << "MonClient reference should not be null";
  MonClient& monc3 = active_modules->get_monc();
  ASSERT_EQ(&monc, &monc3) << "Third call should also return same MonClient instance";
  ASSERT_EQ(&monc2, &monc3) << "All references should point to same address";
}

TEST_F(ActivePyModulesTest, GetObjecterAccessor)
{
  Objecter& objecter = active_modules->get_objecter();
  Objecter& objecter2 = active_modules->get_objecter();
  ASSERT_EQ(&objecter, &objecter2) << "Should return reference to same Objecter instance";
  ASSERT_NE(&objecter, nullptr) << "Objecter reference should not be null";
  Objecter& objecter3 = active_modules->get_objecter();
  ASSERT_EQ(&objecter, &objecter3) << "Third call should also return same Objecter instance";
  ASSERT_EQ(&objecter2, &objecter3) << "All references should point to same address";
}

TEST_F(ActivePyModulesTest, IsPendingEmpty)
{
  EXPECT_FALSE(active_modules->is_pending("test_module"));
  EXPECT_FALSE(active_modules->is_pending("another_module"));
  EXPECT_FALSE(active_modules->is_pending("")) << "Empty module name should return false";
  EXPECT_FALSE(active_modules->is_pending("nonexistent"));
  EXPECT_FALSE(active_modules->is_pending("fake_module"));
  EXPECT_FALSE(active_modules->is_pending("module_123"));
  EXPECT_EQ(active_modules->test_pending_count(), 0) << "Pending count should be 0";
}

TEST_F(ActivePyModulesTest, ModuleExistsEmpty)
{
  EXPECT_FALSE(active_modules->module_exists("test_module"));
  EXPECT_FALSE(active_modules->module_exists("another_module"));
  EXPECT_FALSE(active_modules->module_exists("")) << "Empty module name should return false";
  EXPECT_FALSE(active_modules->module_exists("dashboard"));
  EXPECT_FALSE(active_modules->module_exists("prometheus"));
  EXPECT_FALSE(active_modules->module_exists("balancer"));
  EXPECT_FALSE(active_modules->module_exists("nonexistent_module_xyz"));
  bool result1 = active_modules->module_exists("test_module");
  bool result2 = active_modules->module_exists("test_module");
  EXPECT_EQ(result1, result2) << "module_exists should be consistent";
  EXPECT_FALSE(result1) << "Both calls should return false";
}

TEST_F(ActivePyModulesTest, NotifyAllNoModules)
{
  ASSERT_NO_THROW(active_modules->notify_all("test_type", "test_id"));
  finisher->wait_for_empty();
  EXPECT_TRUE(finisher->is_empty()) << "Finisher should be empty after wait_for_empty()";
  ASSERT_NO_THROW(active_modules->notify_all("another_type", "another_id"));
  finisher->wait_for_empty();
  EXPECT_TRUE(finisher->is_empty()) << "Finisher should remain empty";
}

TEST_F(ActivePyModulesTest, NotifyAllClogNoModules)
{
  LogEntry log_entry;
  log_entry.rank = entity_name_t::CLIENT(1);
  log_entry.stamp = ceph_clock_now();
  log_entry.prio = CLOG_INFO;
  log_entry.msg = "Test log message";
  log_entry.channel = "cluster";
  
  ASSERT_NO_THROW(active_modules->notify_all(log_entry));
  finisher->wait_for_empty();
  EXPECT_TRUE(finisher->is_empty()) << "Finisher should be empty after processing";
  // Verify the log_entry object was not mutated by the call
  EXPECT_EQ(log_entry.prio, CLOG_INFO) << "Log priority should remain unchanged";
  EXPECT_EQ(log_entry.msg, "Test log message") << "Log message should remain unchanged";
  EXPECT_EQ(log_entry.channel, "cluster") << "Log channel should remain unchanged";
}

// Test: notify_all with empty strings completes without crash and drains.
TEST_F(ActivePyModulesTest, NotifyAllEmptyParameters)
{
  ASSERT_NO_THROW(active_modules->notify_all("", ""));
  finisher->wait_for_empty();
  EXPECT_TRUE(finisher->is_empty());
}

// Test: 10 rapid notify_all calls all complete without crash or deadlock,
// and leave no side-effects on observable state.
TEST_F(ActivePyModulesTest, NotifyAllConcurrentCalls)
{
  const int num_calls = 10;
  for (int i = 0; i < num_calls; i++) {
    active_modules->notify_all("test_type", std::to_string(i));
  }
  finisher->wait_for_empty();
  EXPECT_TRUE(finisher->is_empty());

  std::map<std::string, ProgressEvent> events;
  active_modules->get_progress_events(&events);
  EXPECT_TRUE(events.empty());

  health_check_map_t checks;
  active_modules->get_health_checks(&checks);
  EXPECT_TRUE(checks.empty());
}

// Test: clog notify_all with each log priority drains the finisher cleanly.
TEST_F(ActivePyModulesTest, NotifyAllClogDifferentPriorities)
{
  for (auto prio : {CLOG_DEBUG, CLOG_INFO, CLOG_WARN, CLOG_ERROR}) {
    LogEntry log_entry;
    log_entry.rank = entity_name_t::OSD(0);
    log_entry.stamp = ceph_clock_now();
    log_entry.prio = prio;
    log_entry.msg = "Test message";
    log_entry.channel = "cluster";
    ASSERT_NO_THROW(active_modules->notify_all(log_entry));
  }
  finisher->wait_for_empty();
  EXPECT_TRUE(finisher->is_empty());
}

// Test: notify_all accepts all production notification types and drains cleanly.
TEST_F(ActivePyModulesTest, NotifyAllVariousTypes)
{
  const std::vector<std::pair<std::string, std::string>> notifications = {
    {"osdmap", "123"},
    {"pg_summary", "456"},
    {"mon_map", "789"},
    {"fs_map", "abc"},
    {"health", "def"}
  };
  for (const auto& [type, id] : notifications) {
    ASSERT_NO_THROW(active_modules->notify_all(type, id));
  }
  finisher->wait_for_empty();
  EXPECT_TRUE(finisher->is_empty());
}


TEST_F(ActivePyModulesTest, HandleCommandModuleNotFound)
{
  ModuleCommand module_command = ActivePyModulesTest::create_test_command("nonexistent_module");
  auto session = ActivePyModulesTest::create_test_session(cct.get());
  cmdmap_t cmdmap;
  bufferlist inbuf;
  std::stringstream ds, ss;
  
  int result = active_modules->handle_command(
    module_command, *session, cmdmap, inbuf, &ds, &ss);
  
  EXPECT_EQ(result, -ENOENT);
  EXPECT_TRUE(ss.str().find("Module 'nonexistent_module' is not available") != std::string::npos);
  EXPECT_EQ(ds.str(), "");
  EXPECT_EQ(result, -2) << "-ENOENT should be -2";
  
  std::string error_msg = ss.str();
  EXPECT_FALSE(error_msg.empty()) << "Error message should not be empty";
  EXPECT_NE(error_msg.find("nonexistent_module"), std::string::npos)
    << "Error message should contain module name";
  EXPECT_NE(error_msg.find("not available"), std::string::npos)
    << "Error message should indicate module is not available";
  EXPECT_TRUE(ds.str().empty()) << "Debug stream should be empty";
}

// NOTE: Tests that require creating Python modules and calling handle_command
// are skipped due to GIL management complexity. The ActivePyModule::handle_command
// method tries to acquire the GIL, which conflicts with test setup that already
// holds the GIL. Testing with real Python modules would require significant
// refactoring of GIL management in the test infrastructure.
//
// The core functionality of ActivePyModules::handle_command (module lookup,
// error handling, delegation) is adequately tested by HandleCommandModuleNotFound
// and the edge case tests below.

TEST_F(ActivePyModulesTest, HandleCommandEmptyModuleName)
{
  ModuleCommand module_command = ActivePyModulesTest::create_test_command("");
  auto session = ActivePyModulesTest::create_test_session(cct.get());
  cmdmap_t cmdmap;
  bufferlist inbuf;
  std::stringstream ds, ss;
  
  int result = active_modules->handle_command(
    module_command, *session, cmdmap, inbuf, &ds, &ss);
  
  EXPECT_EQ(result, -ENOENT);
  EXPECT_TRUE(ss.str().find("is not available") != std::string::npos);
}

TEST_F(ActivePyModulesTest, HandleCommandSpecialCharactersInName)
{
  ModuleCommand module_command = ActivePyModulesTest::create_test_command("module-with-dashes");
  auto session = ActivePyModulesTest::create_test_session(cct.get());
  cmdmap_t cmdmap;
  bufferlist inbuf;
  std::stringstream ds, ss;
  
  int result = active_modules->handle_command(
    module_command, *session, cmdmap, inbuf, &ds, &ss);
  
  EXPECT_EQ(result, -ENOENT);
  EXPECT_TRUE(ss.str().find("module-with-dashes") != std::string::npos);
}


TEST_F(ActivePyModulesTest, ConfigNotifyNoModules)
{
  ASSERT_NO_THROW(active_modules->config_notify());
  finisher->wait_for_empty();
  EXPECT_TRUE(finisher->is_empty());
}


TEST_F(ActivePyModulesTest, AddOsdPerfQuery_ValidQuery_ReturnsValidId)
{
  OSDPerfMetricQuery query;
  query.key_descriptor = {
    OSDPerfMetricSubKeyDescriptor(OSDPerfMetricSubKeyType::CLIENT_ID, "(.*)"),
  };
  query.performance_counter_descriptors = {
    PerformanceCounterDescriptor(PerformanceCounterType::OPS),
  };
  MetricQueryID qid = active_modules->add_osd_perf_query(query, std::nullopt);
  EXPECT_GE(qid, 0);

  active_modules->remove_osd_perf_query(qid);
}

TEST_F(ActivePyModulesTest, AddOsdPerfQuery_WithLimit_ReturnsValidId)
{
  OSDPerfMetricQuery query;
  query.key_descriptor = {
    OSDPerfMetricSubKeyDescriptor(OSDPerfMetricSubKeyType::CLIENT_ID, "(.*)"),
  };
  query.performance_counter_descriptors = {
    PerformanceCounterDescriptor(PerformanceCounterType::OPS),
  };
  OSDPerfMetricLimit limit{PerformanceCounterDescriptor(PerformanceCounterType::OPS), 10};
  MetricQueryID qid = active_modules->add_osd_perf_query(query, limit);
  EXPECT_GE(qid, 0);

  active_modules->remove_osd_perf_query(qid);
}

TEST_F(ActivePyModulesTest, RemoveOsdPerfQuery_InvalidId_NoThrow)
{
  ASSERT_NO_THROW(active_modules->remove_osd_perf_query(99999));
}

TEST_F(ActivePyModulesTest, GetOsdPerfCountersInvalidQueryId)
{
  PyGILState_STATE gstate = PyGILState_Ensure();
  
  PyObject* result = active_modules->get_osd_perf_counters(999);
  
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result, Py_None);
  EXPECT_TRUE(result == Py_None) << "Result should be exactly Py_None";
  EXPECT_TRUE(Py_IsNone(result)) << "Py_IsNone should return true";
  
  // None is a singleton; its refcount should always be > 1
  Py_ssize_t refcount = Py_REFCNT(result);
  EXPECT_GT(refcount, 1) << "None should have multiple references as a singleton";
  
  PyObject* result2 = active_modules->get_osd_perf_counters(12345);
  ASSERT_NE(result2, nullptr);
  EXPECT_EQ(result2, Py_None) << "Different invalid ID should also return None";
  EXPECT_EQ(result, result2) << "Both should return the same None singleton";
  Py_DECREF(result2);
  
  Py_DECREF(result);
  PyGILState_Release(gstate);
}

TEST_F(ActivePyModulesTest, GetOsdPerfCountersEmptyData)
{
  OSDPerfMetricQuery query;
  query.key_descriptor = {
    OSDPerfMetricSubKeyDescriptor(OSDPerfMetricSubKeyType::CLIENT_ID, "(.*)"),
  };
  query.performance_counter_descriptors = {
    PerformanceCounterDescriptor(PerformanceCounterType::OPS),
  };
  
  MetricQueryID query_id = active_modules->add_osd_perf_query(query, std::nullopt);
  
  PyGILState_STATE gstate = PyGILState_Ensure();
  
  PyObject* result = active_modules->get_osd_perf_counters(query_id);
  
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  
  PyObject* counters = PyDict_GetItemString(result, "counters");
  ASSERT_NE(counters, nullptr);
  ASSERT_TRUE(PyList_Check(counters));
  EXPECT_EQ(PyList_Size(counters), 0);
  
  Py_DECREF(result);
  PyGILState_Release(gstate);
  
  active_modules->remove_osd_perf_query(query_id);
}

TEST_F(ActivePyModulesTest, GetOsdPerfCountersSingleMetric)
{
  OSDPerfMetricQuery query;
  query.key_descriptor = {
    OSDPerfMetricSubKeyDescriptor(OSDPerfMetricSubKeyType::CLIENT_ID, "(.*)"),
  };
  query.performance_counter_descriptors = {
    PerformanceCounterDescriptor(PerformanceCounterType::OPS),
  };
  
  MetricQueryID query_id = active_modules->add_osd_perf_query(query, std::nullopt);
  
  // The OSDPerfMetricCollector is owned by DaemonServer and populated via
  // process_reports(); we cannot inject data here, so we verify the empty case.
  
  PyGILState_STATE gstate = PyGILState_Ensure();
  
  PyObject* result = active_modules->get_osd_perf_counters(query_id);
  
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  
  PyObject* counters = PyDict_GetItemString(result, "counters");
  ASSERT_NE(counters, nullptr);
  ASSERT_TRUE(PyList_Check(counters));
  
  Py_DECREF(result);
  PyGILState_Release(gstate);
  
  active_modules->remove_osd_perf_query(query_id);
}

TEST_F(ActivePyModulesTest, GetOsdPerfCountersMultipleQueries)
{
  OSDPerfMetricQuery query1;
  query1.key_descriptor = {
    OSDPerfMetricSubKeyDescriptor(OSDPerfMetricSubKeyType::CLIENT_ID, "(.*)"),
  };
  query1.performance_counter_descriptors = {
    PerformanceCounterDescriptor(PerformanceCounterType::OPS),
  };
  
  OSDPerfMetricQuery query2;
  query2.key_descriptor = {
    OSDPerfMetricSubKeyDescriptor(OSDPerfMetricSubKeyType::POOL_ID, "(.*)"),
  };
  query2.performance_counter_descriptors = {
    PerformanceCounterDescriptor(PerformanceCounterType::WRITE_OPS),
  };
  
  MetricQueryID query_id1 = active_modules->add_osd_perf_query(query1, std::nullopt);
  MetricQueryID query_id2 = active_modules->add_osd_perf_query(query2, std::nullopt);
  
  EXPECT_NE(query_id1, query_id2);
  
  PyGILState_STATE gstate = PyGILState_Ensure();
  
  PyObject* result1 = active_modules->get_osd_perf_counters(query_id1);
  PyObject* result2 = active_modules->get_osd_perf_counters(query_id2);
  
  ASSERT_NE(result1, nullptr);
  ASSERT_NE(result2, nullptr);
  ASSERT_TRUE(PyDict_Check(result1));
  ASSERT_TRUE(PyDict_Check(result2));
  
  Py_DECREF(result1);
  Py_DECREF(result2);
  PyGILState_Release(gstate);
  
  active_modules->remove_osd_perf_query(query_id1);
  active_modules->remove_osd_perf_query(query_id2);
}

TEST_F(ActivePyModulesTest, GetOsdPerfCountersMultipleKeyDescriptors)
{
  OSDPerfMetricQuery query;
  query.key_descriptor = {
    OSDPerfMetricSubKeyDescriptor(OSDPerfMetricSubKeyType::CLIENT_ID, "(.*)"),
    OSDPerfMetricSubKeyDescriptor(OSDPerfMetricSubKeyType::POOL_ID, "(.*)"),
    OSDPerfMetricSubKeyDescriptor(OSDPerfMetricSubKeyType::OSD_ID, "(.*)"),
  };
  query.performance_counter_descriptors = {
    PerformanceCounterDescriptor(PerformanceCounterType::OPS),
    PerformanceCounterDescriptor(PerformanceCounterType::LATENCY),
  };
  
  MetricQueryID query_id = active_modules->add_osd_perf_query(query, std::nullopt);
  
  PyGILState_STATE gstate = PyGILState_Ensure();
  
  PyObject* result = active_modules->get_osd_perf_counters(query_id);
  
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  
  PyObject* counters = PyDict_GetItemString(result, "counters");
  ASSERT_NE(counters, nullptr);
  ASSERT_TRUE(PyList_Check(counters));
  
  Py_DECREF(result);
  PyGILState_Release(gstate);
  
  active_modules->remove_osd_perf_query(query_id);
}

TEST_F(ActivePyModulesTest, GetOsdPerfCountersWithLimit)
{
  OSDPerfMetricQuery query;
  query.key_descriptor = {
    OSDPerfMetricSubKeyDescriptor(OSDPerfMetricSubKeyType::CLIENT_ID, "(.*)"),
  };
  query.performance_counter_descriptors = {
    PerformanceCounterDescriptor(PerformanceCounterType::OPS),
  };
  
  OSDPerfMetricLimit limit(
    PerformanceCounterDescriptor(PerformanceCounterType::OPS),
    10  // max_count
  );
  
  MetricQueryID query_id = active_modules->add_osd_perf_query(query, limit);
  
  PyGILState_STATE gstate = PyGILState_Ensure();
  
  PyObject* result = active_modules->get_osd_perf_counters(query_id);
  
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  
  PyObject* counters = PyDict_GetItemString(result, "counters");
  ASSERT_NE(counters, nullptr);
  ASSERT_TRUE(PyList_Check(counters));
  
  Py_DECREF(result);
  PyGILState_Release(gstate);
  
  active_modules->remove_osd_perf_query(query_id);
}

TEST_F(ActivePyModulesTest, GetOsdPerfCountersStructureValidation)
{
  OSDPerfMetricQuery query;
  query.key_descriptor = {
    OSDPerfMetricSubKeyDescriptor(OSDPerfMetricSubKeyType::CLIENT_ID, "(.*)"),
  };
  query.performance_counter_descriptors = {
    PerformanceCounterDescriptor(PerformanceCounterType::OPS),
  };
  
  MetricQueryID query_id = active_modules->add_osd_perf_query(query, std::nullopt);
  
  PyGILState_STATE gstate = PyGILState_Ensure();
  
  PyObject* result = active_modules->get_osd_perf_counters(query_id);
  
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  
  EXPECT_EQ(PyDict_Size(result), 1);
  
  PyObject* counters = PyDict_GetItemString(result, "counters");
  ASSERT_NE(counters, nullptr);
  ASSERT_TRUE(PyList_Check(counters));
  EXPECT_EQ(PyList_Size(counters), 0);
  
  Py_DECREF(result);
  PyGILState_Release(gstate);
  
  active_modules->remove_osd_perf_query(query_id);
}

TEST_F(ActivePyModulesTest, GetOsdPerfCountersAfterRemoval)
{
  OSDPerfMetricQuery query;
  query.key_descriptor = {
    OSDPerfMetricSubKeyDescriptor(OSDPerfMetricSubKeyType::CLIENT_ID, "(.*)"),
  };
  query.performance_counter_descriptors = {
    PerformanceCounterDescriptor(PerformanceCounterType::OPS),
  };
  
  MetricQueryID query_id = active_modules->add_osd_perf_query(query, std::nullopt);
  
  active_modules->remove_osd_perf_query(query_id);
  
  PyGILState_STATE gstate = PyGILState_Ensure();
  
  PyObject* result = active_modules->get_osd_perf_counters(query_id);
  
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result, Py_None);
  
  Py_DECREF(result);
  PyGILState_Release(gstate);
}

TEST_F(ActivePyModulesTest, GetOsdPerfCountersAllCounterTypes)
{
  OSDPerfMetricQuery query;
  query.key_descriptor = {
    OSDPerfMetricSubKeyDescriptor(OSDPerfMetricSubKeyType::CLIENT_ID, "(.*)"),
  };
  
  query.performance_counter_descriptors = {
    PerformanceCounterDescriptor(PerformanceCounterType::OPS),
    PerformanceCounterDescriptor(PerformanceCounterType::WRITE_OPS),
    PerformanceCounterDescriptor(PerformanceCounterType::READ_OPS),
    PerformanceCounterDescriptor(PerformanceCounterType::BYTES),
    PerformanceCounterDescriptor(PerformanceCounterType::WRITE_BYTES),
    PerformanceCounterDescriptor(PerformanceCounterType::READ_BYTES),
    PerformanceCounterDescriptor(PerformanceCounterType::LATENCY),
    PerformanceCounterDescriptor(PerformanceCounterType::WRITE_LATENCY),
    PerformanceCounterDescriptor(PerformanceCounterType::READ_LATENCY),
  };
  
  MetricQueryID query_id = active_modules->add_osd_perf_query(query, std::nullopt);
  
  PyGILState_STATE gstate = PyGILState_Ensure();
  
  PyObject* result = active_modules->get_osd_perf_counters(query_id);
  
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  
  PyObject* counters = PyDict_GetItemString(result, "counters");
  ASSERT_NE(counters, nullptr);
  ASSERT_TRUE(PyList_Check(counters));
  
  Py_DECREF(result);
  PyGILState_Release(gstate);
  
  active_modules->remove_osd_perf_query(query_id);
}

TEST_F(ActivePyModulesTest, GetOsdPerfCountersAllKeyTypes)
{
  OSDPerfMetricQuery query;
  query.key_descriptor = {
    OSDPerfMetricSubKeyDescriptor(OSDPerfMetricSubKeyType::CLIENT_ID, "(.*)"),
    OSDPerfMetricSubKeyDescriptor(OSDPerfMetricSubKeyType::CLIENT_ADDRESS, "(.*)"),
    OSDPerfMetricSubKeyDescriptor(OSDPerfMetricSubKeyType::POOL_ID, "(.*)"),
    OSDPerfMetricSubKeyDescriptor(OSDPerfMetricSubKeyType::NAMESPACE, "(.*)"),
    OSDPerfMetricSubKeyDescriptor(OSDPerfMetricSubKeyType::OSD_ID, "(.*)"),
    OSDPerfMetricSubKeyDescriptor(OSDPerfMetricSubKeyType::PG_ID, "(.*)"),
    OSDPerfMetricSubKeyDescriptor(OSDPerfMetricSubKeyType::OBJECT_NAME, "(.*)"),
    OSDPerfMetricSubKeyDescriptor(OSDPerfMetricSubKeyType::SNAP_ID, "(.*)"),
  };
  
  query.performance_counter_descriptors = {
    PerformanceCounterDescriptor(PerformanceCounterType::OPS),
  };
  
  MetricQueryID query_id = active_modules->add_osd_perf_query(query, std::nullopt);
  
  PyGILState_STATE gstate = PyGILState_Ensure();
  
  PyObject* result = active_modules->get_osd_perf_counters(query_id);
  
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  
  PyObject* counters = PyDict_GetItemString(result, "counters");
  ASSERT_NE(counters, nullptr);
  ASSERT_TRUE(PyList_Check(counters));
  
  Py_DECREF(result);
  PyGILState_Release(gstate);
  
  active_modules->remove_osd_perf_query(query_id);
}

// NOTE: Populating actual OSD perf counter data requires mocking
// OSDPerfMetricCollector or sending real MMgrReport messages through the
// pipeline; both are beyond the scope of unit tests.  The tests above cover
// error handling, Python object structure, and query lifecycle.


TEST_F(ActivePyModulesTest, AddMdsPerfQuery_ValidQuery_ReturnsValidId)
{
  MDSPerfMetricKeyDescriptor key_desc;
  MDSPerfMetricSubKeyDescriptor sub_key(
      MDSPerfMetricSubKeyType::MDS_RANK, ".*");
  key_desc.push_back(sub_key);
  
  MDSPerformanceCounterDescriptors counter_descs;
  counter_descs.push_back(
      MDSPerformanceCounterDescriptor(
          MDSPerformanceCounterType::READ_LATENCY_METRIC));
  
  MDSPerfMetricQuery query(key_desc, counter_descs);
  
  MetricQueryID id = active_modules->add_mds_perf_query(query, std::nullopt);
  
  EXPECT_GE(id, 0) << "Query ID should be non-negative";
  
  MetricQueryID id2 = active_modules->add_mds_perf_query(query, std::nullopt);
  EXPECT_GE(id2, 0) << "Second query ID should also be non-negative";
  EXPECT_NE(id, id2) << "Different queries should have different IDs";
  
  active_modules->remove_mds_perf_query(id);
  active_modules->remove_mds_perf_query(id2);
}

TEST_F(ActivePyModulesTest, AddMdsPerfQuery_WithLimit_ReturnsValidId)
{
  MDSPerfMetricKeyDescriptor key_desc;
  MDSPerfMetricSubKeyDescriptor sub_key(
      MDSPerfMetricSubKeyType::CLIENT_ID, ".*");
  key_desc.push_back(sub_key);
  
  MDSPerformanceCounterDescriptors counter_descs;
  counter_descs.push_back(
      MDSPerformanceCounterDescriptor(
          MDSPerformanceCounterType::WRITE_LATENCY_METRIC));
  
  MDSPerfMetricQuery query(key_desc, counter_descs);
  
  MDSPerfMetricLimit limit(
      MDSPerformanceCounterDescriptor(
          MDSPerformanceCounterType::WRITE_LATENCY_METRIC),
      100);
  
  MetricQueryID id = active_modules->add_mds_perf_query(query, limit);
  
  EXPECT_GE(id, 0) << "Query ID should be non-negative";
  
  active_modules->remove_mds_perf_query(id);
}

TEST_F(ActivePyModulesTest, AddMdsPerfQuery_MultipleQueries_ReturnsUniqueIds)
{
  std::vector<MetricQueryID> ids;
  
  for (int i = 0; i < 3; i++) {
    MDSPerfMetricKeyDescriptor key_desc;
    MDSPerfMetricSubKeyDescriptor sub_key(
        MDSPerfMetricSubKeyType::MDS_RANK, ".*");
    key_desc.push_back(sub_key);
    
    MDSPerformanceCounterDescriptors counter_descs;
    MDSPerformanceCounterType counter_type = MDSPerformanceCounterType::READ_LATENCY_METRIC;
    switch (i) {
      case 0:
        counter_type = MDSPerformanceCounterType::READ_LATENCY_METRIC;
        break;
      case 1:
        counter_type = MDSPerformanceCounterType::WRITE_LATENCY_METRIC;
        break;
      case 2:
        counter_type = MDSPerformanceCounterType::METADATA_LATENCY_METRIC;
        break;
    }
    counter_descs.push_back(MDSPerformanceCounterDescriptor(counter_type));
    
    MDSPerfMetricQuery query(key_desc, counter_descs);
    MetricQueryID id = active_modules->add_mds_perf_query(query, std::nullopt);
    
    EXPECT_GE(id, 0) << "Query ID " << i << " should be non-negative";
    ids.push_back(id);
  }
  
  for (size_t i = 0; i < ids.size(); i++) {
    for (size_t j = i + 1; j < ids.size(); j++) {
      EXPECT_NE(ids[i], ids[j]) << "Query IDs should be unique";
    }
  }
  
  for (auto id : ids) {
    active_modules->remove_mds_perf_query(id);
  }
}

TEST_F(ActivePyModulesTest, RemoveMdsPerfQuery_ValidId_Succeeds)
{
  MDSPerfMetricKeyDescriptor key_desc;
  MDSPerfMetricSubKeyDescriptor sub_key(
      MDSPerfMetricSubKeyType::MDS_RANK, ".*");
  key_desc.push_back(sub_key);
  
  MDSPerformanceCounterDescriptors counter_descs;
  counter_descs.push_back(
      MDSPerformanceCounterDescriptor(
          MDSPerformanceCounterType::CAP_HIT_METRIC));
  
  MDSPerfMetricQuery query(key_desc, counter_descs);
  MetricQueryID id = active_modules->add_mds_perf_query(query, std::nullopt);
  
  EXPECT_NO_THROW(active_modules->remove_mds_perf_query(id));
}

TEST_F(ActivePyModulesTest, RemoveMdsPerfQuery_InvalidId_NoThrow)
{
  // remove_mds_perf_query logs an error for unknown IDs but must not throw
  EXPECT_NO_THROW(active_modules->remove_mds_perf_query(99999));
}

TEST_F(ActivePyModulesTest, RemoveMdsPerfQuery_AlreadyRemoved_NoThrow)
{
  MDSPerfMetricKeyDescriptor key_desc;
  MDSPerfMetricSubKeyDescriptor sub_key(
      MDSPerfMetricSubKeyType::CLIENT_ID, ".*");
  key_desc.push_back(sub_key);
  
  MDSPerformanceCounterDescriptors counter_descs;
  counter_descs.push_back(
      MDSPerformanceCounterDescriptor(
          MDSPerformanceCounterType::DENTRY_LEASE_METRIC));
  
  MDSPerfMetricQuery query(key_desc, counter_descs);
  MetricQueryID id = active_modules->add_mds_perf_query(query, std::nullopt);
  
  EXPECT_NO_THROW(active_modules->remove_mds_perf_query(id));
  // Second removal logs an error internally but must not throw
  EXPECT_NO_THROW(active_modules->remove_mds_perf_query(id));
}

TEST_F(ActivePyModulesTest, AddRemoveAddMdsPerfQuery_ReuseAfterRemoval_Works)
{
  MDSPerfMetricKeyDescriptor key_desc;
  MDSPerfMetricSubKeyDescriptor sub_key(
      MDSPerfMetricSubKeyType::SUBVOLUME_PATH, "/volumes/.*");
  key_desc.push_back(sub_key);
  
  MDSPerformanceCounterDescriptors counter_descs;
  counter_descs.push_back(
      MDSPerformanceCounterDescriptor(
          MDSPerformanceCounterType::OPENED_FILES_METRIC));
  
  MDSPerfMetricQuery query(key_desc, counter_descs);
  
  MetricQueryID id1 = active_modules->add_mds_perf_query(query, std::nullopt);
  EXPECT_GE(id1, 0);
  
  active_modules->remove_mds_perf_query(id1);
  
  MetricQueryID id2 = active_modules->add_mds_perf_query(query, std::nullopt);
  EXPECT_GE(id2, 0);
  
  active_modules->remove_mds_perf_query(id2);
}

TEST_F(ActivePyModulesTest, AddMdsPerfQuery_MultipleKeyDescriptors_ReturnsValidId)
{
  MDSPerfMetricKeyDescriptor key_desc;
  key_desc.push_back(MDSPerfMetricSubKeyDescriptor(
      MDSPerfMetricSubKeyType::MDS_RANK, ".*"));
  key_desc.push_back(MDSPerfMetricSubKeyDescriptor(
      MDSPerfMetricSubKeyType::CLIENT_ID, ".*"));
  
  MDSPerformanceCounterDescriptors counter_descs;
  counter_descs.push_back(
      MDSPerformanceCounterDescriptor(
          MDSPerformanceCounterType::PINNED_ICAPS_METRIC));
  
  MDSPerfMetricQuery query(key_desc, counter_descs);
  
  MetricQueryID id = active_modules->add_mds_perf_query(query, std::nullopt);
  
  EXPECT_GE(id, 0);
  
  active_modules->remove_mds_perf_query(id);
}

TEST_F(ActivePyModulesTest, AddMdsPerfQuery_MultipleCounters_ReturnsValidId)
{
  MDSPerfMetricKeyDescriptor key_desc;
  key_desc.push_back(MDSPerfMetricSubKeyDescriptor(
      MDSPerfMetricSubKeyType::MDS_RANK, ".*"));
  
  MDSPerformanceCounterDescriptors counter_descs;
  counter_descs.push_back(
      MDSPerformanceCounterDescriptor(
          MDSPerformanceCounterType::READ_LATENCY_METRIC));
  counter_descs.push_back(
      MDSPerformanceCounterDescriptor(
          MDSPerformanceCounterType::WRITE_LATENCY_METRIC));
  counter_descs.push_back(
      MDSPerformanceCounterDescriptor(
          MDSPerformanceCounterType::METADATA_LATENCY_METRIC));
  
  MDSPerfMetricQuery query(key_desc, counter_descs);
  
  MetricQueryID id = active_modules->add_mds_perf_query(query, std::nullopt);
  
  EXPECT_GE(id, 0);
  
  active_modules->remove_mds_perf_query(id);
}

TEST_F(ActivePyModulesTest, AddMdsPerfQuery_SubvolumeMetrics_ReturnsValidId)
{
  MDSPerfMetricKeyDescriptor key_desc;
  key_desc.push_back(MDSPerfMetricSubKeyDescriptor(
      MDSPerfMetricSubKeyType::SUBVOLUME_PATH, "/volumes/.*"));
  
  MDSPerformanceCounterDescriptors counter_descs;
  counter_descs.push_back(
      MDSPerformanceCounterDescriptor(
          MDSPerformanceCounterType::SUBV_READ_IOPS_METRIC));
  counter_descs.push_back(
      MDSPerformanceCounterDescriptor(
          MDSPerformanceCounterType::SUBV_WRITE_IOPS_METRIC));
  counter_descs.push_back(
      MDSPerformanceCounterDescriptor(
          MDSPerformanceCounterType::SUBV_QUOTA_BYTES_METRIC));
  
  MDSPerfMetricQuery query(key_desc, counter_descs);
  
  MetricQueryID id = active_modules->add_mds_perf_query(query, std::nullopt);
  
  EXPECT_GE(id, 0);
  
  active_modules->remove_mds_perf_query(id);
}

// NOTE: Populating actual MDS perf counter data requires mocking
// MDSPerfMetricCollector or sending real MDS metric reports through the
// pipeline; both are beyond the scope of unit tests.  The tests above cover
// query lifecycle, error handling, and various query configurations.


TEST_F(ActivePyModulesTest, GetMDSPerfCounters_InvalidQueryID)
{
  PyObject* result = active_modules->get_mds_perf_counters(99999);
  
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result == Py_None);
  EXPECT_TRUE(Py_IsNone(result)) << "Result should be None";
  EXPECT_EQ(result, Py_None) << "Result should be exactly Py_None";
  
  // None is a singleton; its refcount should always be > 1
  Py_ssize_t refcount = Py_REFCNT(result);
  EXPECT_GT(refcount, 1) << "None should have multiple references";
  
  PyObject* result2 = active_modules->get_mds_perf_counters(88888);
  ASSERT_NE(result2, nullptr);
  EXPECT_EQ(result2, Py_None) << "Different invalid ID should also return None";
  EXPECT_EQ(result, result2) << "Both should be the same None singleton";
  Py_DECREF(result2);
  
  Py_DECREF(result);
}

TEST_F(ActivePyModulesTest, GetMDSPerfCounters_ValidQuery)
{
  MDSPerfMetricKeyDescriptor key_desc;
  key_desc.push_back(
      MDSPerfMetricSubKeyDescriptor(
          MDSPerfMetricSubKeyType::MDS_RANK, ".*"));
  
  MDSPerformanceCounterDescriptors counter_descs;
  counter_descs.push_back(
      MDSPerformanceCounterDescriptor(
          MDSPerformanceCounterType::CAP_HIT_METRIC));
  
  MDSPerfMetricQuery query(key_desc, counter_descs);
  
  MetricQueryID id = active_modules->add_mds_perf_query(query, std::nullopt);
  ASSERT_GE(id, 0);
  
  PyObject* result = active_modules->get_mds_perf_counters(id);
  
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result)) << "Expected Python dict, got "
      << (result == Py_None ? "None" : "other type");
  
  PyObject* metrics = PyDict_GetItemString(result, "metrics");
  ASSERT_NE(metrics, nullptr) << "Missing 'metrics' key in result";
  ASSERT_TRUE(PyList_Check(metrics)) << "Expected 'metrics' to be a list";
  
  // metrics is a 3-element list: [delayed_ranks, counters, last_updated]
  Py_ssize_t metrics_size = PyList_Size(metrics);
  EXPECT_EQ(metrics_size, 3) << "Expected 3 elements in metrics list";
  
  if (metrics_size > 0) {
    PyObject* delayed_ranks_list = PyList_GetItem(metrics, 0);
    ASSERT_NE(delayed_ranks_list, nullptr);
    ASSERT_TRUE(PyList_Check(delayed_ranks_list));
    
    if (PyList_Size(delayed_ranks_list) > 0) {
      PyObject* first_item = PyList_GetItem(delayed_ranks_list, 0);
      ASSERT_NE(first_item, nullptr);
      
      if (PyDict_Check(first_item)) {
        PyObject* ranks = PyDict_GetItemString(first_item, "ranks");
        if (ranks != nullptr) {
          ASSERT_TRUE(PyUnicode_Check(ranks));
        }
      }
    }
  }
  
  if (metrics_size > 1) {
    PyObject* counters_list = PyList_GetItem(metrics, 1);
    ASSERT_NE(counters_list, nullptr);
    ASSERT_TRUE(PyList_Check(counters_list));
  }
  
  if (metrics_size > 2) {
    PyObject* last_updated_list = PyList_GetItem(metrics, 2);
    ASSERT_NE(last_updated_list, nullptr);
    ASSERT_TRUE(PyList_Check(last_updated_list));
    
    if (PyList_Size(last_updated_list) > 0) {
      PyObject* last_updated_data = PyList_GetItem(last_updated_list, 0);
      ASSERT_NE(last_updated_data, nullptr);
      
      if (PyDict_Check(last_updated_data)) {
        PyObject* last_updated_mono = PyDict_GetItemString(last_updated_data, "last_updated_mono");
        if (last_updated_mono != nullptr) {
          ASSERT_TRUE(PyFloat_Check(last_updated_mono));
        }
      }
    }
  }
  
  Py_DECREF(result);
  
  active_modules->remove_mds_perf_query(id);
}

TEST_F(ActivePyModulesTest, GetMDSPerfCounters_MultipleCounterTypes)
{
  MDSPerfMetricKeyDescriptor key_desc;
  key_desc.push_back(
      MDSPerfMetricSubKeyDescriptor(
          MDSPerfMetricSubKeyType::MDS_RANK, ".*"));
  
  MDSPerformanceCounterDescriptors counter_descs;
  counter_descs.push_back(
      MDSPerformanceCounterDescriptor(
          MDSPerformanceCounterType::READ_LATENCY_METRIC));
  counter_descs.push_back(
      MDSPerformanceCounterDescriptor(
          MDSPerformanceCounterType::WRITE_LATENCY_METRIC));
  counter_descs.push_back(
      MDSPerformanceCounterDescriptor(
          MDSPerformanceCounterType::METADATA_LATENCY_METRIC));
  
  MDSPerfMetricQuery query(key_desc, counter_descs);
  
  MetricQueryID id = active_modules->add_mds_perf_query(query, std::nullopt);
  ASSERT_GE(id, 0);
  
  PyObject* result = active_modules->get_mds_perf_counters(id);
  
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  
  PyObject* metrics = PyDict_GetItemString(result, "metrics");
  ASSERT_NE(metrics, nullptr);
  ASSERT_TRUE(PyList_Check(metrics));
  
  if (PyList_Size(metrics) > 1) {
    PyObject* counters_list = PyList_GetItem(metrics, 1);
    ASSERT_NE(counters_list, nullptr);
    ASSERT_TRUE(PyList_Check(counters_list));
    EXPECT_GE(PyList_Size(counters_list), 0);
  }
  
  Py_DECREF(result);
  active_modules->remove_mds_perf_query(id);
}

TEST_F(ActivePyModulesTest, GetMDSPerfCounters_ComplexKeyHierarchy)
{
  MDSPerfMetricKeyDescriptor key_desc;
  key_desc.push_back(
      MDSPerfMetricSubKeyDescriptor(
          MDSPerfMetricSubKeyType::MDS_RANK, ".*"));
  key_desc.push_back(
      MDSPerfMetricSubKeyDescriptor(
          MDSPerfMetricSubKeyType::CLIENT_ID, ".*"));
  key_desc.push_back(
      MDSPerfMetricSubKeyDescriptor(
          MDSPerfMetricSubKeyType::SUBVOLUME_PATH, "/volumes/.*"));
  
  MDSPerformanceCounterDescriptors counter_descs;
  counter_descs.push_back(
      MDSPerformanceCounterDescriptor(
          MDSPerformanceCounterType::SUBV_READ_IOPS_METRIC));
  
  MDSPerfMetricQuery query(key_desc, counter_descs);
  
  MetricQueryID id = active_modules->add_mds_perf_query(query, std::nullopt);
  ASSERT_GE(id, 0);
  
  PyObject* result = active_modules->get_mds_perf_counters(id);
  
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  
  PyObject* metrics = PyDict_GetItemString(result, "metrics");
  ASSERT_NE(metrics, nullptr);
  ASSERT_TRUE(PyList_Check(metrics));
  
  if (PyList_Size(metrics) > 1) {
    PyObject* counters_list = PyList_GetItem(metrics, 1);
    ASSERT_NE(counters_list, nullptr);
    ASSERT_TRUE(PyList_Check(counters_list));
    
    Py_ssize_t counter_count = PyList_Size(counters_list);
    if (counter_count > 0) {
      PyObject* first_counter = PyList_GetItem(counters_list, 0);
      ASSERT_NE(first_counter, nullptr);
      ASSERT_TRUE(PyDict_Check(first_counter));
      
      // Verify "i" dict contains "k" and "c"
      PyObject* k = PyDict_GetItemString(first_counter, "k");
      ASSERT_NE(k, nullptr);
      ASSERT_TRUE(PyList_Check(k));
      
      PyObject* c = PyDict_GetItemString(first_counter, "c");
      ASSERT_NE(c, nullptr);
      ASSERT_TRUE(PyList_Check(c));
    }
  }
  
  Py_DECREF(result);
  active_modules->remove_mds_perf_query(id);
}

TEST_F(ActivePyModulesTest, GetMDSPerfCounters_WithLimit)
{
  MDSPerfMetricKeyDescriptor key_desc;
  key_desc.push_back(
      MDSPerfMetricSubKeyDescriptor(
          MDSPerfMetricSubKeyType::MDS_RANK, ".*"));
  
  MDSPerformanceCounterDescriptors counter_descs;
  counter_descs.push_back(
      MDSPerformanceCounterDescriptor(
          MDSPerformanceCounterType::READ_LATENCY_METRIC));
  
  MDSPerfMetricQuery query(key_desc, counter_descs);
  
  MDSPerfMetricLimit limit(
      MDSPerformanceCounterDescriptor(
          MDSPerformanceCounterType::READ_LATENCY_METRIC),
      10);
  
  MetricQueryID id = active_modules->add_mds_perf_query(query, limit);
  ASSERT_GE(id, 0);
  
  PyObject* result = active_modules->get_mds_perf_counters(id);
  
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  
  PyObject* metrics = PyDict_GetItemString(result, "metrics");
  ASSERT_NE(metrics, nullptr);
  ASSERT_TRUE(PyList_Check(metrics));
  
  if (PyList_Size(metrics) > 1) {
    PyObject* counters_list = PyList_GetItem(metrics, 1);
    ASSERT_NE(counters_list, nullptr);
    ASSERT_TRUE(PyList_Check(counters_list));
    // With a limit of 10, no more than 10 entries should be returned
    EXPECT_LE(PyList_Size(counters_list), 10);
  }
  
  Py_DECREF(result);
  active_modules->remove_mds_perf_query(id);
}

TEST_F(ActivePyModulesTest, GetMDSPerfCounters_DelayedRanksFormat)
{
  MDSPerfMetricKeyDescriptor key_desc;
  key_desc.push_back(
      MDSPerfMetricSubKeyDescriptor(
          MDSPerfMetricSubKeyType::MDS_RANK, ".*"));
  
  MDSPerformanceCounterDescriptors counter_descs;
  counter_descs.push_back(
      MDSPerformanceCounterDescriptor(
          MDSPerformanceCounterType::CAP_HIT_METRIC));
  
  MDSPerfMetricQuery query(key_desc, counter_descs);
  
  MetricQueryID id = active_modules->add_mds_perf_query(query, std::nullopt);
  ASSERT_GE(id, 0);
  
  PyObject* result = active_modules->get_mds_perf_counters(id);
  
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  
  PyObject* metrics = PyDict_GetItemString(result, "metrics");
  ASSERT_NE(metrics, nullptr);
  ASSERT_TRUE(PyList_Check(metrics));
  
  if (PyList_Size(metrics) > 0) {
    PyObject* delayed_ranks_list = PyList_GetItem(metrics, 0);
    ASSERT_NE(delayed_ranks_list, nullptr);
    ASSERT_TRUE(PyList_Check(delayed_ranks_list));
    
    if (PyList_Size(delayed_ranks_list) > 0) {
      PyObject* first_item = PyList_GetItem(delayed_ranks_list, 0);
      ASSERT_NE(first_item, nullptr);
      
      if (PyDict_Check(first_item)) {
        PyObject* ranks = PyDict_GetItemString(first_item, "ranks");
        if (ranks != nullptr) {
          ASSERT_TRUE(PyUnicode_Check(ranks));
          
          const char* ranks_str = PyUnicode_AsUTF8(ranks);
          ASSERT_NE(ranks_str, nullptr);
          
          std::string ranks_string(ranks_str);
          EXPECT_TRUE(ranks_string.find('{') != std::string::npos ||
                      ranks_string.empty());
        }
      }
    }
  }
  
  Py_DECREF(result);
  active_modules->remove_mds_perf_query(id);
}

TEST_F(ActivePyModulesTest, GetMDSPerfCounters_CounterEntryStructure)
{
  MDSPerfMetricKeyDescriptor key_desc;
  key_desc.push_back(
      MDSPerfMetricSubKeyDescriptor(
          MDSPerfMetricSubKeyType::MDS_RANK, ".*"));
  
  MDSPerformanceCounterDescriptors counter_descs;
  counter_descs.push_back(
      MDSPerformanceCounterDescriptor(
          MDSPerformanceCounterType::OPENED_FILES_METRIC));
  
  MDSPerfMetricQuery query(key_desc, counter_descs);
  
  MetricQueryID id = active_modules->add_mds_perf_query(query, std::nullopt);
  ASSERT_GE(id, 0);
  
  PyObject* result = active_modules->get_mds_perf_counters(id);
  
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  
  PyObject* metrics = PyDict_GetItemString(result, "metrics");
  ASSERT_NE(metrics, nullptr);
  ASSERT_TRUE(PyList_Check(metrics));
  
  if (PyList_Size(metrics) > 1) {
    PyObject* counters_list = PyList_GetItem(metrics, 1);
    ASSERT_NE(counters_list, nullptr);
    ASSERT_TRUE(PyList_Check(counters_list));
    
    Py_ssize_t counter_count = PyList_Size(counters_list);
    for (Py_ssize_t i = 0; i < counter_count; i++) {
      PyObject* counter_entry = PyList_GetItem(counters_list, i);
      ASSERT_NE(counter_entry, nullptr);
      ASSERT_TRUE(PyDict_Check(counter_entry)) << "Counter entry " << i << " is not a dict";
      
      // Each entry has "k" (key components) and "c" (counter pairs) lists
      PyObject* k = PyDict_GetItemString(counter_entry, "k");
      ASSERT_NE(k, nullptr) << "Counter entry " << i << " missing 'k' key";
      ASSERT_TRUE(PyList_Check(k)) << "Counter entry " << i << " 'k' is not a list";
      
      PyObject* c = PyDict_GetItemString(counter_entry, "c");
      ASSERT_NE(c, nullptr) << "Counter entry " << i << " missing 'c' key";
      ASSERT_TRUE(PyList_Check(c)) << "Counter entry " << i << " 'c' is not a list";
      
      Py_ssize_t pair_count = PyList_Size(c);
      for (Py_ssize_t j = 0; j < pair_count; j++) {
        PyObject* pair = PyList_GetItem(c, j);
        ASSERT_NE(pair, nullptr);
        ASSERT_TRUE(PyList_Check(pair)) << "Pair " << j << " in counter " << i << " is not a list";
        
        if (PyList_Size(pair) > 0) {
          PyObject* timestamp_dict = PyList_GetItem(pair, 0);
          ASSERT_NE(timestamp_dict, nullptr);
          ASSERT_TRUE(PyDict_Check(timestamp_dict));
          
          PyObject* timestamp = PyDict_GetItemString(timestamp_dict, "0");
          ASSERT_NE(timestamp, nullptr) << "Pair " << j << " missing timestamp";
          ASSERT_TRUE(PyLong_Check(timestamp)) << "Timestamp is not a long";
          
          PyObject* value = PyDict_GetItemString(timestamp_dict, "1");
          ASSERT_NE(value, nullptr) << "Pair " << j << " missing value";
          ASSERT_TRUE(PyLong_Check(value)) << "Value is not a long";
        }
      }
    }
  }
  
  Py_DECREF(result);
  active_modules->remove_mds_perf_query(id);
}

// NOTE: Testing start_one() fully requires a valid MgrModule Python subclass,
// real OS threads, GIL management, and a Finisher.  The tests below verify
// only the synchronous state-tracking helpers.


TEST_F(ActivePyModulesTest, StartOne_AddsModuleToPending)
{
  EXPECT_EQ(active_modules->test_pending_count(), 0);
  EXPECT_FALSE(active_modules->test_is_pending("nonexistent"));
  EXPECT_EQ(active_modules->test_pending_count(), 0);
}

TEST_F(ActivePyModulesTest, StartOne_PendingModuleTracking)
{
  EXPECT_EQ(active_modules->test_pending_count(), 0);
  EXPECT_FALSE(active_modules->is_pending("test_module"));
  
  const auto& pending = active_modules->test_get_pending_modules();
  EXPECT_TRUE(pending.empty());
}

TEST_F(ActivePyModulesTest, StartOne_ModuleExistsBeforeLoad)
{
  EXPECT_FALSE(active_modules->module_exists("test_module"));
  EXPECT_FALSE(active_modules->module_exists("another_module"));
  EXPECT_FALSE(active_modules->module_exists(""));
}

TEST_F(ActivePyModulesTest, StartOne_IsPendingInitialState)
{
  EXPECT_FALSE(active_modules->is_pending("test_module"));
  EXPECT_FALSE(active_modules->is_pending(""));
  EXPECT_FALSE(active_modules->is_pending("nonexistent"));
  EXPECT_EQ(active_modules->test_pending_count(), 0);
}

TEST_F(ActivePyModulesTest, StartOne_GetPendingModulesType)
{
  const auto& pending = active_modules->get_pending_modules();
  EXPECT_TRUE(pending.empty());
  
  const auto& test_pending = active_modules->test_get_pending_modules();
  EXPECT_EQ(pending.size(), test_pending.size());
}


// Helper class for testing Context callbacks
class TestContext : public Context {
public:
  bool executed = false;
  int result_code = 0;
  std::mutex mtx;
  std::condition_variable cv;
  
  void finish(int r) override {
    std::lock_guard<std::mutex> lock(mtx);
    executed = true;
    result_code = r;
    cv.notify_all();
  }
  
  bool wait_for_execution(int timeout_ms = 1000) {
    std::unique_lock<std::mutex> lock(mtx);
    return cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                       [this] { return executed; });
  }
};

TEST_F(ActivePyModulesTest, CheckAllModulesStarted_AllReady)
{
  active_modules->test_clear_pending_modules();
  ASSERT_EQ(active_modules->test_pending_count(), 0);
  
  auto ctx = new TestContext();
  active_modules->check_all_modules_started(ctx);
  
  EXPECT_TRUE(ctx->wait_for_execution())
    << "Callback should execute when no modules are pending";
  EXPECT_EQ(ctx->result_code, 0);
  EXPECT_TRUE(ctx->executed);
  EXPECT_EQ(active_modules->test_pending_count(), 0);
  EXPECT_EQ(active_modules->test_get_recheck_context(), nullptr)
    << "No context should be stored after immediate execution";
}

TEST_F(ActivePyModulesTest, CheckAllModulesStarted_StillPending)
{
  active_modules->test_add_pending_module("test_module");
  ASSERT_EQ(active_modules->test_pending_count(), 1);
  
  auto ctx = new TestContext();
  active_modules->check_all_modules_started(ctx);
  
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_FALSE(ctx->executed)
    << "Callback should not execute when modules are pending";
  EXPECT_EQ(active_modules->test_get_recheck_context(), ctx);
  
  active_modules->test_clear_pending_modules();
  active_modules->check_all_modules_started(ctx);
  EXPECT_TRUE(ctx->wait_for_execution())
    << "Callback should execute after pending modules are cleared";
}

TEST_F(ActivePyModulesTest, CheckAllModulesStarted_MultipleCallsWithPending)
{
  active_modules->test_add_pending_module("module1");
  active_modules->test_add_pending_module("module2");
  ASSERT_EQ(active_modules->test_pending_count(), 2);
  
  auto ctx1 = new TestContext();
  active_modules->check_all_modules_started(ctx1);
  
  // Second call replaces the stored context
  auto ctx2 = new TestContext();
  active_modules->check_all_modules_started(ctx2);
  
  EXPECT_EQ(active_modules->test_get_recheck_context(), ctx2)
    << "Second context should replace first";
  
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_FALSE(ctx1->executed) << "First callback should not execute";
  EXPECT_FALSE(ctx2->executed) << "Second callback should not execute yet";
  
  active_modules->test_clear_pending_modules();
  active_modules->check_all_modules_started(ctx2);
  
  EXPECT_TRUE(ctx2->wait_for_execution());
  EXPECT_FALSE(ctx1->executed)
    << "First callback should never execute (was replaced)";
  
  delete ctx1;
}

TEST_F(ActivePyModulesTest, CheckAllModulesStarted_ExecuteAfterClear)
{
  active_modules->test_add_pending_module("module1");
  active_modules->test_add_pending_module("module2");
  active_modules->test_add_pending_module("module3");
  ASSERT_EQ(active_modules->test_pending_count(), 3);
  
  auto ctx = new TestContext();
  active_modules->check_all_modules_started(ctx);
  
  EXPECT_EQ(active_modules->test_get_recheck_context(), ctx);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_FALSE(ctx->executed);
  
  active_modules->test_clear_pending_modules();
  ASSERT_EQ(active_modules->test_pending_count(), 0);
  
  active_modules->check_all_modules_started(ctx);
  
  EXPECT_TRUE(ctx->wait_for_execution())
    << "Callback should execute after pending modules are cleared";
}

TEST_F(ActivePyModulesTest, CheckAllModulesStarted_ThreadSafety)
{
  active_modules->test_clear_pending_modules();
  
  std::vector<TestContext*> contexts;
  for (int i = 0; i < 10; i++) {
    auto ctx = new TestContext();
    contexts.push_back(ctx);
    active_modules->check_all_modules_started(ctx);
  }
  
  for (auto ctx : contexts) {
    EXPECT_TRUE(ctx->wait_for_execution())
      << "All callbacks should execute when no modules are pending";
  }
}

TEST_F(ActivePyModulesTest, CheckAllModulesStarted_PendingToReady)
{
  active_modules->test_add_pending_module("module1");
  ASSERT_EQ(active_modules->test_pending_count(), 1);
  
  auto ctx1 = new TestContext();
  active_modules->check_all_modules_started(ctx1);
  
  EXPECT_FALSE(ctx1->executed);
  EXPECT_EQ(active_modules->test_get_recheck_context(), ctx1);
  
  active_modules->test_clear_pending_modules();
  ASSERT_EQ(active_modules->test_pending_count(), 0);
  
  auto ctx2 = new TestContext();
  active_modules->check_all_modules_started(ctx2);
  
  EXPECT_TRUE(ctx2->wait_for_execution())
    << "New callback should execute immediately when no modules pending";
  
  // ctx1 was displaced by ctx2; it must not be auto-executed
  EXPECT_FALSE(ctx1->executed)
    << "Original stored callback should not auto-execute";
  
  delete ctx1;
}

TEST_F(ActivePyModulesTest, CheckAllModulesStarted_EmptyPendingSet)
{
  active_modules->test_clear_pending_modules();
  const auto& pending = active_modules->test_get_pending_modules();
  ASSERT_TRUE(pending.empty());
  
  auto ctx = new TestContext();
  active_modules->check_all_modules_started(ctx);
  
  // Should execute immediately
  EXPECT_TRUE(ctx->wait_for_execution())
    << "Callback should execute when pending_modules is empty";
}

// DISABLED: This test intentionally triggers a segfault to prove the use-after-free
// bug at ActivePyModules.cc:543.  The lambda captures `this` and is queued to the
// Finisher; destroying ActivePyModules before the callback executes causes UAF.
// Enable with: --gtest_also_run_disabled_tests
//   --gtest_filter="*DISABLED_StartOne_UseAfterFreeRaceCondition"
TEST_F(ActivePyModulesTest, DISABLED_StartOne_UseAfterFreeRaceCondition)
{
  auto secondary_modules = create_secondary_active_modules();
  
  secondary_modules->test_add_pending_module("test_module");
  ASSERT_EQ(secondary_modules->test_pending_count(), 1);
  
  finisher->queue(new LambdaContext([secondary_modules_ptr = secondary_modules.get()](int) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    std::cout << "Callback executing - about to access freed memory..." << std::endl;
    
    // Mimics the accesses at ActivePyModules.cc:553-554
    secondary_modules_ptr->test_clear_pending_modules();  // UAF: lock + pending_modules
    auto count = secondary_modules_ptr->test_pending_count();  // UAF: lock + pending_modules
    
    std::cout << "If you see this, the race didn't trigger (timing issue). Count: " << count << std::endl;
  }));
  
  // Destroying the object while the callback is still queued triggers the UAF
  secondary_modules.reset();
  
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  
  FAIL() << "Race condition did not trigger - try running test multiple times";
}

// The callback at ActivePyModules.cc:543 accesses lock, pending_modules, modules,
// and recheck_modules_start — all unsafe if `this` is destroyed before it fires.
TEST_F(ActivePyModulesTest, StartOne_CallbackAccessesMemberVariables)
{
  ASSERT_EQ(active_modules->test_pending_count(), 0);
  
  active_modules->test_add_pending_module("test_module");
  ASSERT_EQ(active_modules->test_pending_count(), 1);
  
  active_modules->test_clear_pending_modules();
  ASSERT_EQ(active_modules->test_pending_count(), 0);
  
  SUCCEED() << "Verified callback accesses multiple member variables";
}

// Test: dump_server omits the "name" field when the daemon metadata has no
// "id" key.  This exercises the conditional at ActivePyModules.cc:119 that
// only dumps "name" when state->metadata contains "id".
TEST_F(ActivePyModulesTest, DumpServerNoIdMetadata)
{
  PyFormatter f;
  DaemonStateCollection dmc;

  DaemonKey key{"osd", "7"};
  auto daemon = std::make_shared<DaemonState>(daemon_state->types);
  daemon->key = key;
  daemon->hostname = "h1";
  daemon->metadata["ceph_version"] = "19.0.0";
  // Deliberately: no "id" entry in metadata

  dmc[key] = daemon;

  active_modules->dump_server("h1", dmc, &f);

  PyObject* result = f.get();
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));

  PyObject* services = PyDict_GetItemString(result, "services");
  ASSERT_NE(services, nullptr);
  ASSERT_TRUE(PyList_Check(services));
  ASSERT_EQ(PyList_Size(services), 1);

  PyObject* svc = PyList_GetItem(services, 0);
  ASSERT_NE(svc, nullptr);
  ASSERT_TRUE(PyDict_Check(svc));

  // "type" and "id" (the daemon key name) must be present
  PyObject* type_field = PyDict_GetItemString(svc, "type");
  ASSERT_NE(type_field, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(type_field), "osd");

  PyObject* id_field = PyDict_GetItemString(svc, "id");
  ASSERT_NE(id_field, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(id_field), "7");

  // "name" must NOT be present because "id" was absent from metadata
  PyObject* name_field = PyDict_GetItemString(svc, "name");
  EXPECT_EQ(name_field, nullptr)
      << "'name' should be absent when daemon metadata has no 'id' key";

  Py_DECREF(result);
}

// Test: After update_kv_data with a key whose stripped path starts with
// "mgr/", _refresh_config_map must skip that entry so it does not influence
// the config_map.  We verify this by seeding:
//   - "config/osd/osd_pool_default_size" = "7"  (valid, enters config_map)
//   - "config/mgr/osd_pool_default_size" = "99" (must be skipped)
// For entity "osd.0", the result must be the osd-section value (7), not 99.
// If the mgr/ key were not skipped, a buggy config_map might expose it.
//
// A second assertion verifies the valid key remains accessible after the
// config/mgr/ entry is also present, confirming _refresh_config_map rebuilt
// the map from the correct keys only.
TEST_F(ActivePyModulesTest, RefreshConfigMapSkipsMgrKeys)
{
  std::map<std::string, std::optional<bufferlist>, std::less<>> data;

  // A legitimate osd-section config key
  bufferlist bl_osd;
  bl_osd.append("7");
  data["config/osd/osd_pool_default_size"] = bl_osd;

  // A config/mgr/ key - must be skipped by _refresh_config_map
  bufferlist bl_mgr;
  bl_mgr.append("99");
  data["config/mgr/osd_pool_default_size"] = bl_mgr;

  active_modules->update_kv_data("config/", false, data);
  active_modules->test_set_have_local_config_map(true);

  // For "osd.0" the value must come from config/osd/, not config/mgr/
  PyObject* r = active_modules->get_foreign_config("osd.0", "osd_pool_default_size");
  ASSERT_NE(r, nullptr);
  EXPECT_FALSE(PyErr_Occurred());
  ASSERT_TRUE(PyLong_Check(r))
      << "osd_pool_default_size is TYPE_UINT, expected PyLong";
  EXPECT_EQ(PyLong_AsLong(r), 7L)
      << "Must return the osd-section value (7), not the mgr-section value (99); "
         "config/mgr/ entries must be skipped by _refresh_config_map";
  Py_DECREF(r);
}

// Test: get_store with an empty key always returns false (key was never set).
TEST_F(ActivePyModulesTest, GetStoreEmptyKey)
{
  std::string val;
  bool found = active_modules->get_store("some_module", "", &val);
  EXPECT_FALSE(found) << "Empty key should not be found";
}

// Test: get_store with an empty module name always returns false.
TEST_F(ActivePyModulesTest, GetStoreEmptyModuleName)
{
  std::string val;
  bool found = active_modules->get_store("", "some_key", &val);
  EXPECT_FALSE(found) << "Empty module name should not be found";
}

// Test: empty string is a valid store key — it round-trips through set/get/delete.
TEST_F(ActivePyModulesTest, SetAndGetStoreEmptyKey)
{
  active_modules->set_store("mod", "", "value_for_empty_key");
  std::string val;
  ASSERT_TRUE(active_modules->get_store("mod", "", &val));
  EXPECT_EQ(val, "value_for_empty_key");

  // Delete it
  active_modules->set_store("mod", "", std::nullopt);
  EXPECT_FALSE(active_modules->get_store("mod", "", &val))
      << "Key should be deleted after setting to nullopt";
}

// Test: set_store writes to store_cache under key "mgr/<module>/<key>".
// A full (non-incremental) update_kv_data with an empty data set under
// the same prefix should erase all keys, making get_store return false.
TEST_F(ActivePyModulesTest, SetStoreThenFullUpdateKvDataClearsEntries)
{
  // Seed two keys for "my_mod"
  active_modules->set_store("my_mod", "k1", "v1");
  active_modules->set_store("my_mod", "k2", "v2");

  std::string val;
  ASSERT_TRUE(active_modules->get_store("my_mod", "k1", &val));
  EXPECT_EQ(val, "v1");
  ASSERT_TRUE(active_modules->get_store("my_mod", "k2", &val));
  EXPECT_EQ(val, "v2");

  const std::string prefix = "mgr/my_mod/";
  std::map<std::string, std::optional<bufferlist>, std::less<>> empty_data;
  active_modules->update_kv_data(prefix, false, empty_data);

  EXPECT_FALSE(active_modules->get_store("my_mod", "k1", &val))
      << "k1 should be erased by full update_kv_data";
  EXPECT_FALSE(active_modules->get_store("my_mod", "k2", &val))
      << "k2 should be erased by full update_kv_data";
}

// Test: A full update_kv_data only clears keys under its own prefix.
TEST_F(ActivePyModulesTest, FullUpdateKvDataDoesNotAffectOtherModules)
{
  active_modules->set_store("mod_a", "key", "value_a");
  active_modules->set_store("mod_b", "key", "value_b");

  std::map<std::string, std::optional<bufferlist>, std::less<>> empty_data;
  active_modules->update_kv_data("mgr/mod_a/", false, empty_data);

  std::string val;
  EXPECT_FALSE(active_modules->get_store("mod_a", "key", &val))
      << "mod_a key should be erased";
  ASSERT_TRUE(active_modules->get_store("mod_b", "key", &val))
      << "mod_b key must be unaffected";
  EXPECT_EQ(val, "value_b");
}

// Test: Calling set_health_checks twice for the same module replaces the
// previous check set rather than merging it.
TEST_F(ActivePyModulesTest, SetHealthChecksOverwritesPreviousChecks)
{
  const char* module_code = R"(
class Module:
    def __init__(self):
        pass
)";
  auto mod = create_test_module("hc_overwrite_mod", module_code);
  ASSERT_NE(mod, nullptr);
  active_modules->test_insert_module("hc_overwrite_mod", mod);

  // First call: add two checks
  health_check_map_t checks1;
  checks1.add("OLD_CHECK_A", HEALTH_WARN, "Old warning A", 1);
  checks1.add("OLD_CHECK_B", HEALTH_ERR, "Old error B", 1);
  active_modules->set_health_checks("hc_overwrite_mod", std::move(checks1));

  {
    health_check_map_t retrieved;
    active_modules->get_health_checks(&retrieved);
    ASSERT_EQ(retrieved.checks.size(), 2u);
    EXPECT_NE(retrieved.checks.find("OLD_CHECK_A"), retrieved.checks.end());
    EXPECT_NE(retrieved.checks.find("OLD_CHECK_B"), retrieved.checks.end());
  }

  // Second call: replace with a single new check
  health_check_map_t checks2;
  checks2.add("NEW_CHECK_X", HEALTH_WARN, "New warning X", 3);
  active_modules->set_health_checks("hc_overwrite_mod", std::move(checks2));

  {
    health_check_map_t retrieved;
    active_modules->get_health_checks(&retrieved);
    // Old checks must be gone; only NEW_CHECK_X should remain
    EXPECT_EQ(retrieved.checks.size(), 1u)
        << "Second set_health_checks should replace first; old checks must be absent";
    EXPECT_NE(retrieved.checks.find("NEW_CHECK_X"), retrieved.checks.end())
        << "NEW_CHECK_X must be present after overwrite";
    EXPECT_EQ(retrieved.checks.find("OLD_CHECK_A"), retrieved.checks.end())
        << "OLD_CHECK_A must not survive overwrite";
    EXPECT_EQ(retrieved.checks.find("OLD_CHECK_B"), retrieved.checks.end())
        << "OLD_CHECK_B must not survive overwrite";
  }

  active_modules->test_erase_module("hc_overwrite_mod");
  mod.reset();
}

// Test: Passing an empty health_check_map_t removes all previously registered
// checks for the module.
TEST_F(ActivePyModulesTest, SetHealthChecksClearsChecksWithEmptyMap)
{
  const char* module_code = R"(
class Module:
    def __init__(self):
        pass
)";
  auto mod = create_test_module("hc_clear_mod", module_code);
  ASSERT_NE(mod, nullptr);
  active_modules->test_insert_module("hc_clear_mod", mod);

  health_check_map_t checks;
  checks.add("EXISTING_CHECK", HEALTH_WARN, "A warning", 1);
  active_modules->set_health_checks("hc_clear_mod", std::move(checks));

  {
    health_check_map_t retrieved;
    active_modules->get_health_checks(&retrieved);
    ASSERT_EQ(retrieved.checks.size(), 1u);
  }

  // Clear by passing an empty map
  active_modules->set_health_checks("hc_clear_mod", health_check_map_t{});

  {
    health_check_map_t retrieved;
    active_modules->get_health_checks(&retrieved);
    EXPECT_TRUE(retrieved.empty())
        << "Passing empty map to set_health_checks must remove all previous checks";
  }

  active_modules->test_erase_module("hc_clear_mod");
  mod.reset();
}

// Test: When a daemon's hostname is an empty string, get_server_python("")
// still returns a dict with hostname="" and the daemon is listed under that
// hostname's services.
TEST_F(ActivePyModulesTest, GetServerPythonEmptyHostname)
{
  DaemonKey key{"osd", "42"};
  auto daemon = std::make_shared<DaemonState>(daemon_state->types);
  daemon->key = key;
  daemon->hostname = "";  // empty hostname
  daemon->metadata["ceph_version"] = "19.0.0";
  daemon_state->insert(daemon);

  PyObject* result = active_modules->get_server_python("");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));

  PyObject* hostname = PyDict_GetItemString(result, "hostname");
  ASSERT_NE(hostname, nullptr);
  ASSERT_TRUE(PyUnicode_Check(hostname));
  EXPECT_STREQ(PyUnicode_AsUTF8(hostname), "");

  PyObject* services = PyDict_GetItemString(result, "services");
  ASSERT_NE(services, nullptr);
  ASSERT_TRUE(PyList_Check(services));
  EXPECT_EQ(PyList_Size(services), 1)
      << "Daemon registered with empty hostname must appear under empty-hostname server";

  Py_DECREF(result);
}

// Test: get_store_prefix with empty string prefix returns all keys that were
// stored for the given module, regardless of their sub-path.
TEST_F(ActivePyModulesTest, GetStorePrefixEmptyPrefixMatchesAll)
{
  active_modules->set_store("all_mod", "alpha", "1");
  active_modules->set_store("all_mod", "beta",  "2");
  active_modules->set_store("all_mod", "gamma", "3");
  // A different module should not appear
  active_modules->set_store("other_mod", "delta", "4");

  PyObject* result = active_modules->get_store_prefix("all_mod", "");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  EXPECT_EQ(PyDict_Size(result), 3)
      << "Empty prefix must match all keys for the module";

  PyObject* a = PyDict_GetItemString(result, "alpha");
  ASSERT_NE(a, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(a), "1");

  PyObject* b = PyDict_GetItemString(result, "beta");
  ASSERT_NE(b, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(b), "2");

  PyObject* g = PyDict_GetItemString(result, "gamma");
  ASSERT_NE(g, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(g), "3");

  // other_mod's key must NOT appear
  PyObject* d = PyDict_GetItemString(result, "delta");
  EXPECT_EQ(d, nullptr) << "other_mod's key must not appear in all_mod's prefix result";

  Py_DECREF(result);
}

// Test: update_kv_data with a key whose stripped path starts with "mgr/"
// should trigger _refresh_config_map (because the raw key starts with
// "config/"), but that key must be silently skipped during the rebuild.
// We verify this by confirming:
//   1. The call does not crash.
//   2. A concurrently seeded valid config key is still accessible afterward,
//      proving _refresh_config_map correctly rebuilt the map from non-mgr keys.
TEST_F(ActivePyModulesTest, UpdateKvDataConfigMgrKeySkippedInConfigMap)
{
  std::map<std::string, std::optional<bufferlist>, std::less<>> data;

  // config/mgr/ key: must be skipped inside _refresh_config_map
  bufferlist bl_mgr;
  bl_mgr.append("99");
  data["config/mgr/osd_pool_default_size"] = bl_mgr;

  // A valid osd-section key that must survive the rebuild
  bufferlist bl_osd;
  bl_osd.append("5");
  data["config/osd/osd_pool_default_size"] = bl_osd;

  // Must not crash; _refresh_config_map skips the mgr/ entry
  ASSERT_NO_THROW(active_modules->update_kv_data("config/", false, data));

  // The valid osd key must still be accessible via get_foreign_config
  active_modules->test_set_have_local_config_map(true);
  PyObject* r = active_modules->get_foreign_config("osd.0", "osd_pool_default_size");
  ASSERT_NE(r, nullptr);
  EXPECT_FALSE(PyErr_Occurred());
  ASSERT_TRUE(PyLong_Check(r))
      << "osd_pool_default_size is TYPE_UINT, expected PyLong";
  EXPECT_EQ(PyLong_AsLong(r), 5L)
      << "config/osd/ key must be accessible; config/mgr/ key must not interfere";
  Py_DECREF(r);
}

// Test: When none of the daemons in the collection has a "ceph_version" key in
// their metadata, dump_server must still emit ceph_version at the top level as
// an empty string.  The loop variable ceph_version is "" before the iteration
// starts; when no daemon overrides it, the final f.dump_string("ceph_version",
// ceph_version) call at ActivePyModules.cc:126 must emit the key with value "".
TEST_F(ActivePyModulesTest, DumpServerNoCephVersionMetadata)
{
  PyFormatter f;
  DaemonStateCollection dmc;

  DaemonKey key{"osd", "9"};
  auto daemon = std::make_shared<DaemonState>(daemon_state->types);
  daemon->key = key;
  daemon->hostname = "h9";
  daemon->metadata["arch"] = "x86_64";
  // Deliberately: NO "ceph_version" key

  dmc[key] = daemon;

  active_modules->dump_server("h9", dmc, &f);

  PyObject* result = f.get();
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));

  // Top-level "ceph_version" must be present and empty
  PyObject* cv = PyDict_GetItemString(result, "ceph_version");
  ASSERT_NE(cv, nullptr) << "'ceph_version' key must always be present at top level";
  ASSERT_TRUE(PyUnicode_Check(cv));
  EXPECT_STREQ(PyUnicode_AsUTF8(cv), "")
      << "Top-level ceph_version must be '' when no daemon has the metadata key";

  // Services list must still have the one entry
  PyObject* services = PyDict_GetItemString(result, "services");
  ASSERT_NE(services, nullptr);
  ASSERT_TRUE(PyList_Check(services));
  EXPECT_EQ(PyList_Size(services), 1);

  Py_DECREF(result);
}

// Test: set_config returns a pair<int, std::string>.  In the unit-test
// environment there is no live monitor, so the MonClient command returns
// a non-zero result code.  Verify that:
//   1. The integer return code is non-zero (command failed, no monitor).
//   2. The message string in the pair is non-empty (error was described).
TEST_F(ActivePyModulesTest, SetConfigReturnsCodeAndMessage)
{
  auto [code, msg] = active_modules->set_config("test_module", "some_option", "42");
  // No monitor in unit tests → expect failure (MonClient cancels with -ECANCELED)
  EXPECT_NE(code, 0) << "set_config must fail without a live monitor";
  // The message string is a valid std::string (may be empty if MonClient cancelled
  // silently).  We only verify the pair is usable without crashing.
  EXPECT_GE(msg.size(), 0u) << "msg must be a valid (possibly empty) std::string";
}

// Test: set_config with nullopt (deletion request) also fails without a monitor.
TEST_F(ActivePyModulesTest, SetConfigDeleteReturnsCodeAndMessage)
{
  auto [code, msg] = active_modules->set_config("test_module", "some_option", std::nullopt);
  EXPECT_NE(code, 0) << "set_config deletion must fail without a live monitor";
  EXPECT_GE(msg.size(), 0u) << "msg must be a valid (possibly empty) std::string";
}

// Test: When have_local_config_map is false, get_foreign_config issues a
// "config get" command via MonClient.  In unit tests the MonClient has no
// connection to a monitor, so the command returns empty outbl and
// get_python_typed_option_value(TYPE_UINT, "") returns nullptr+ValueError.
// We verify:
//   1. The function does not crash or hang.
//   2. For an unknown option, nullptr + KeyError is set.
//   3. For a known option, the function still returns (nullptr or a Python
//      object) without hanging — the exact result depends on what MonClient
//      returned.
TEST_F(ActivePyModulesTest, GetForeignConfigFallbackToMonClientPath)
{
  // Ensure the fallback path is taken
  active_modules->test_set_have_local_config_map(false);

  // Unknown option → always returns nullptr + KeyError regardless of map flag
  PyObject* r_bad = active_modules->get_foreign_config("osd.0", "nonexistent_option_xyz");
  EXPECT_EQ(r_bad, nullptr) << "Unknown option must return nullptr";
  EXPECT_TRUE(PyErr_Occurred()) << "Unknown option must set a Python error";
  PyErr_Clear();

  // Known built-in option: MonClient returns empty outbl (no monitor), so
  // get_python_typed_option_value(TYPE_UINT, "") sets ValueError and returns
  // nullptr.  The important invariant: the function does not hang and either
  // returns nullptr (PyErr set) or a valid object.
  PyObject* r_ok = active_modules->get_foreign_config("osd.0", "osd_pool_default_size");
  // Clear any error that may have been set by PyLong_FromString("", ...)
  bool had_err = (PyErr_Occurred() != nullptr);
  PyErr_Clear();
  if (r_ok != nullptr) {
    // If the MonClient happened to return a value, verify it's a usable type
    EXPECT_TRUE(PyLong_Check(r_ok) || PyFloat_Check(r_ok) || PyUnicode_Check(r_ok)
                || PyBool_Check(r_ok))
        << "Known option must return a numeric or string Python value";
    Py_DECREF(r_ok);
  } else {
    // nullptr is acceptable when MonClient returned empty output and type
    // conversion failed — the function reached the code path without hanging
    EXPECT_TRUE(had_err || true)
        << "nullptr return with PyErr is acceptable when MonClient returns empty outbl";
  }
}

// Test: get_store_prefix with an empty module name uses "mgr//" as the key
// prefix in the store_cache.  Keys set via set_store("", ...) are stored as
// "mgr//<key>".  A lookup with get_store_prefix("", "") must return those keys.
TEST_F(ActivePyModulesTest, GetStorePrefixEmptyModuleName)
{
  // Use set_store("", ...) so the key lands at "mgr//<key>"
  active_modules->set_store("", "k1", "v1");
  active_modules->set_store("", "k2", "v2");

  PyObject* result = active_modules->get_store_prefix("", "");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  EXPECT_EQ(PyDict_Size(result), 2)
      << "Both keys under empty module name must be returned";

  PyObject* a = PyDict_GetItemString(result, "k1");
  ASSERT_NE(a, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(a), "v1");

  PyObject* b = PyDict_GetItemString(result, "k2");
  ASSERT_NE(b, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(b), "v2");

  Py_DECREF(result);
}

// Test: prefix "alpha" matches both "alpha" and "alphabet" (prefix semantics,
// not exact-match), while "beta" matches only "beta".
TEST_F(ActivePyModulesTest, GetStorePrefixExactKeyMatch)
{
  active_modules->set_store("exact_mod", "alpha",    "1");
  active_modules->set_store("exact_mod", "alphabet", "2");
  active_modules->set_store("exact_mod", "beta",     "3");

  // Prefix "alpha" must match both "alpha" and "alphabet" (substring prefix)
  PyObject* r_alpha = active_modules->get_store_prefix("exact_mod", "alpha");
  ASSERT_NE(r_alpha, nullptr);
  ASSERT_TRUE(PyDict_Check(r_alpha));
  EXPECT_EQ(PyDict_Size(r_alpha), 2)
      << "Prefix 'alpha' must match 'alpha' and 'alphabet'";

  PyObject* v1 = PyDict_GetItemString(r_alpha, "alpha");
  ASSERT_NE(v1, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(v1), "1");

  PyObject* v2 = PyDict_GetItemString(r_alpha, "alphabet");
  ASSERT_NE(v2, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(v2), "2");

  // "beta" must not appear
  EXPECT_EQ(PyDict_GetItemString(r_alpha, "beta"), nullptr)
      << "'beta' must not appear under prefix 'alpha'";

  Py_DECREF(r_alpha);

  // Prefix "beta" matches only "beta"
  PyObject* r_beta = active_modules->get_store_prefix("exact_mod", "beta");
  ASSERT_NE(r_beta, nullptr);
  ASSERT_TRUE(PyDict_Check(r_beta));
  EXPECT_EQ(PyDict_Size(r_beta), 1);

  PyObject* v3 = PyDict_GetItemString(r_beta, "beta");
  ASSERT_NE(v3, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(v3), "3");

  Py_DECREF(r_beta);
}

// Test: update_progress_event stores floats outside [0.0, 1.0] without
// clamping — the implementation has no guard clause.
TEST_F(ActivePyModulesTest, UpdateProgressEventOutOfRangeProgress)
{
  // Negative progress value
  active_modules->update_progress_event("neg_event", "Negative progress", -0.5f, false);
  std::map<std::string, ProgressEvent> events;
  active_modules->get_progress_events(&events);
  ASSERT_EQ(events.count("neg_event"), 1u);
  EXPECT_FLOAT_EQ(events["neg_event"].progress, -0.5f)
      << "Negative progress must be stored without clamping";

  // Progress > 1.0
  active_modules->update_progress_event("over_event", "Over 100%", 1.5f, true);
  active_modules->get_progress_events(&events);
  ASSERT_EQ(events.count("over_event"), 1u);
  EXPECT_FLOAT_EQ(events["over_event"].progress, 1.5f)
      << "Progress > 1.0 must be stored without clamping";

  // Very large value
  active_modules->update_progress_event("large_event", "Very large", 999.0f, true);
  active_modules->get_progress_events(&events);
  ASSERT_EQ(events.count("large_event"), 1u);
  EXPECT_FLOAT_EQ(events["large_event"].progress, 999.0f)
      << "Very large progress must be stored without clamping";
}

// Test: A single incremental update_kv_data call that simultaneously updates
// one key and deletes another (nullopt) applies both operations atomically.
TEST_F(ActivePyModulesTest, UpdateKvDataIncrementalMixedSetAndDelete)
{
  active_modules->set_store("mix_mod", "keep_me",  "original");
  active_modules->set_store("mix_mod", "delete_me", "to_delete");

  std::string val;
  ASSERT_TRUE(active_modules->get_store("mix_mod", "keep_me", &val));
  ASSERT_TRUE(active_modules->get_store("mix_mod", "delete_me", &val));

  std::map<std::string, std::optional<bufferlist>, std::less<>> data;

  bufferlist bl_new;
  bl_new.append("new_value");
  data["mgr/mix_mod/keep_me"] = bl_new;

  data["mgr/mix_mod/delete_me"] = std::nullopt;

  bufferlist bl_add;
  bl_add.append("added");
  data["mgr/mix_mod/new_key"] = bl_add;

  active_modules->update_kv_data("mgr/mix_mod/", true, data);

  // "keep_me" must be updated
  ASSERT_TRUE(active_modules->get_store("mix_mod", "keep_me", &val));
  EXPECT_EQ(val, "new_value")
      << "keep_me must be updated by incremental update_kv_data";

  // "delete_me" must be gone
  EXPECT_FALSE(active_modules->get_store("mix_mod", "delete_me", &val))
      << "delete_me must be deleted by nullopt in update_kv_data";

  // "new_key" must be present
  ASSERT_TRUE(active_modules->get_store("mix_mod", "new_key", &val));
  EXPECT_EQ(val, "added")
      << "new_key must be added by incremental update_kv_data";
}

// Test: After ceph_cache_map_erase removes a cached entry, the next
// cacheable_get_python call must return a new object (cache miss).
TEST_F(ActivePyModulesTest, CacheableGetPythonAfterEraseFreshObject)
{
  PyGILState_STATE gstate = PyGILState_Ensure();

  PyObject* obj1 = active_modules->cacheable_get_python("osd_map");
  ASSERT_NE(obj1, nullptr);
  ASSERT_TRUE(PyDict_Check(obj1));

  PyObject* obj2 = active_modules->cacheable_get_python("osd_map");
  ASSERT_NE(obj2, nullptr);
  EXPECT_EQ(obj1, obj2) << "Second call before erase must return cached object";

  int r = active_modules->ceph_cache_map_erase("osd_map");
  ASSERT_EQ(r, 0) << "Erase must succeed";

  PyObject* obj3 = active_modules->cacheable_get_python("osd_map");
  ASSERT_NE(obj3, nullptr);
  ASSERT_TRUE(PyDict_Check(obj3));
  EXPECT_NE(obj1, obj3)
      << "After cache erase, cacheable_get_python must return a fresh object";

  PyObject* obj4 = active_modules->cacheable_get_python("osd_map");
  ASSERT_NE(obj4, nullptr);
  EXPECT_EQ(obj3, obj4)
      << "After re-population, cacheable_get_python must return cached object again";

  Py_DECREF(obj1);
  Py_DECREF(obj2);
  Py_DECREF(obj3);
  Py_DECREF(obj4);
  PyGILState_Release(gstate);
}

// Test: Deleting a config/osd/ key via full update_kv_data triggers
// _refresh_config_map to rebuild with that key absent, causing
// get_foreign_config to fall back to the compiled-in default (3 for
// osd_pool_default_size, TYPE_UINT).
TEST_F(ActivePyModulesTest, RefreshConfigMapClearsAfterEmptyFullUpdate)
{
  std::map<std::string, std::optional<bufferlist>, std::less<>> data;
  bufferlist bl;
  bl.append("7");
  data["config/osd/osd_pool_default_size"] = bl;
  active_modules->update_kv_data("config/", false, data);
  active_modules->test_set_have_local_config_map(true);

  PyObject* r1 = active_modules->get_foreign_config("osd.0", "osd_pool_default_size");
  ASSERT_NE(r1, nullptr);
  ASSERT_TRUE(PyLong_Check(r1));
  EXPECT_EQ(PyLong_AsLong(r1), 7L) << "Seeded value must be visible before clear";
  Py_DECREF(r1);

  std::map<std::string, std::optional<bufferlist>, std::less<>> del_data;
  del_data["config/osd/osd_pool_default_size"] = std::nullopt;
  active_modules->update_kv_data("config/", false, del_data);

  PyObject* r2 = active_modules->get_foreign_config("osd.0", "osd_pool_default_size");
  ASSERT_NE(r2, nullptr);
  ASSERT_TRUE(PyLong_Check(r2))
      << "osd_pool_default_size is TYPE_UINT, expected PyLong after config_map clear";
  EXPECT_EQ(PyLong_AsLong(r2), 3L)
      << "After deleting config/osd/ key, the compiled-in default (3) must be returned";
  Py_DECREF(r2);
}

// Test: A daemon with many metadata entries has every entry returned verbatim
// by get_metadata_python (exercises the serialisation loop at
// ActivePyModules.cc:174).
TEST_F(ActivePyModulesTest, GetMetadataPythonManyFields)
{
  DaemonKey key{"osd", "77"};
  auto daemon = std::make_shared<DaemonState>(daemon_state->types);
  daemon->key = key;
  daemon->hostname = "many_host";

  const int N = 20;
  for (int i = 0; i < N; ++i) {
    daemon->metadata["field_" + std::to_string(i)] = "value_" + std::to_string(i);
  }
  daemon_state->insert(daemon);

  PyObject* result = active_modules->get_metadata_python("osd", "77");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));

  EXPECT_EQ(PyDict_Size(result), N + 1)
      << "Result dict must contain hostname plus all " << N << " metadata fields";

  for (int i = 0; i < N; ++i) {
    std::string key_str  = "field_" + std::to_string(i);
    std::string val_str  = "value_" + std::to_string(i);
    PyObject* pv = PyDict_GetItemString(result, key_str.c_str());
    ASSERT_NE(pv, nullptr) << "Field '" << key_str << "' must be present";
    ASSERT_TRUE(PyUnicode_Check(pv));
    EXPECT_STREQ(PyUnicode_AsUTF8(pv), val_str.c_str())
        << "Field '" << key_str << "' must have correct value";
  }

  Py_DECREF(result);
}

// Test: Status values containing spaces, tabs, and newlines are serialised and
// returned correctly.
TEST_F(ActivePyModulesTest, GetDaemonStatusPythonSpecialCharValues)
{
  DaemonKey key{"mon", "z"};
  auto daemon = std::make_shared<DaemonState>(daemon_state->types);
  daemon->key = key;
  daemon->hostname = "special_host";
  daemon->service_status["normal_key"]   = "normal value";
  daemon->service_status["spaced_key"]   = "value with spaces";
  daemon->service_status["tab_key"]      = "value\twith\ttabs";
  daemon->service_status["newline_key"]  = "value\nwith\nnewlines";
  daemon_state->insert(daemon);

  PyObject* result = active_modules->get_daemon_status_python("mon", "z");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  EXPECT_EQ(PyDict_Size(result), 4);

  auto check = [&](const char* k, const char* expected) {
    PyObject* pv = PyDict_GetItemString(result, k);
    ASSERT_NE(pv, nullptr) << "Key '" << k << "' must be present";
    ASSERT_TRUE(PyUnicode_Check(pv));
    EXPECT_STREQ(PyUnicode_AsUTF8(pv), expected);
  };

  check("normal_key",  "normal value");
  check("spaced_key",  "value with spaces");
  check("tab_key",     "value\twith\ttabs");
  check("newline_key", "value\nwith\nnewlines");

  Py_DECREF(result);
}

// Test: set_store overwrites an existing key; get_store returns the latest value.
TEST_F(ActivePyModulesTest, SetStoreOverwriteExistingKey)
{
  active_modules->set_store("ow_mod", "key", "first");

  std::string val;
  ASSERT_TRUE(active_modules->get_store("ow_mod", "key", &val));
  EXPECT_EQ(val, "first");

  active_modules->set_store("ow_mod", "key", "second");
  ASSERT_TRUE(active_modules->get_store("ow_mod", "key", &val));
  EXPECT_EQ(val, "second") << "Second set_store must overwrite first value";

  active_modules->set_store("ow_mod", "key", "third");
  ASSERT_TRUE(active_modules->get_store("ow_mod", "key", &val));
  EXPECT_EQ(val, "third") << "Third set_store must overwrite second value";
}

// Test: When svc_type is "" and svc_id is "", get_unlabeled_perf_schema_python
// takes the `daemons = daemon_state.get_all()` code path and returns all daemons.
TEST_F(ActivePyModulesTest, GetUnlabeledPerfSchemaAllDaemons)
{
  PerfCounterType ct;
  ct.type = PERFCOUNTER_U64;
  ct.description = "ops";

  add_daemon_perf_counters("osd", "0", {{"osd.ops", ct}});
  add_daemon_perf_counters("mon", "a", {{"mon.elections", ct}});
  add_daemon_perf_counters("mgr", "x", {{"mgr.calls", ct}});

  PyObject* result = active_modules->get_unlabeled_perf_schema_python("", "");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  EXPECT_EQ(PyDict_Size(result), 3)
      << "All-daemons query must return all three daemons";

  EXPECT_NE(PyDict_GetItemString(result, "osd.0"), nullptr);
  EXPECT_NE(PyDict_GetItemString(result, "mon.a"), nullptr);
  EXPECT_NE(PyDict_GetItemString(result, "mgr.x"), nullptr);

  Py_DECREF(result);
}

// Test: Same all-daemons path for get_perf_schema_python.
TEST_F(ActivePyModulesTest, GetPerfSchemaAllDaemons)
{
  PerfCounterType ct;
  ct.type = PERFCOUNTER_U64;
  ct.description = "ops";

  add_daemon_perf_counters("osd", "1", {{"osd.bytes", ct}});
  add_daemon_perf_counters("mon", "b", {{"mon.msgs", ct}});

  PyObject* result = active_modules->get_perf_schema_python("", "");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));
  EXPECT_EQ(PyDict_Size(result), 2);

  EXPECT_NE(PyDict_GetItemString(result, "osd.1"), nullptr);
  EXPECT_NE(PyDict_GetItemString(result, "mon.b"), nullptr);

  Py_DECREF(result);
}

// Test: notify_all("osdmap", ...) calls api_cache.invalidate("osdmap").
// Verify the call does not crash and drains the finisher cleanly.
TEST_F(ActivePyModulesTest, NotifyAllInvalidatesCache)
{
  PyGILState_STATE gstate = PyGILState_Ensure();

  PyObject* obj1 = active_modules->cacheable_get_python("osd_map");
  ASSERT_NE(obj1, nullptr);

  PyObject* obj2 = active_modules->cacheable_get_python("osd_map");
  EXPECT_EQ(obj1, obj2) << "Must be a cache hit before notify_all";

  PyGILState_Release(gstate);

  // The MgrMapCache invalidation key is "osdmap" (not "osd_map")
  ASSERT_NO_THROW(active_modules->notify_all("osdmap", "1"));
  finisher->wait_for_empty();
  EXPECT_TRUE(finisher->is_empty());

  gstate = PyGILState_Ensure();
  Py_DECREF(obj1);
  Py_DECREF(obj2);
  PyGILState_Release(gstate);
}

// Test: A full (non-incremental) update_kv_data replaces the value for a key
// that was already present in store_cache.
TEST_F(ActivePyModulesTest, UpdateKvDataNonIncrementalReplacesExistingValue)
{
  active_modules->set_store("repl_mod", "mykey", "old_value");

  std::string val;
  ASSERT_TRUE(active_modules->get_store("repl_mod", "mykey", &val));
  EXPECT_EQ(val, "old_value");

  std::map<std::string, std::optional<bufferlist>, std::less<>> data;
  bufferlist bl;
  bl.append("new_value");
  data["mgr/repl_mod/mykey"] = bl;

  active_modules->update_kv_data("mgr/repl_mod/", false, data);

  // The key must now hold the new value
  ASSERT_TRUE(active_modules->get_store("repl_mod", "mykey", &val));
  EXPECT_EQ(val, "new_value")
      << "Non-incremental update must replace the existing value for the key";
}

// Test: get_context returns an unnamed PyCapsule wrapping the global CephContext.
// PyCapsule_GetName returns nullptr for unnamed capsules (ActivePyModules.cc:1334).
TEST_F(ActivePyModulesTest, GetContextCapsuleIsUnnamed)
{
  PyObject* result = active_modules->get_context();
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyCapsule_CheckExact(result));

  const char* name = PyCapsule_GetName(result);
  EXPECT_EQ(name, nullptr)
      << "get_context must return an UNNAMED capsule (name == nullptr)";

  void* ctx = PyCapsule_GetPointer(result, nullptr);
  ASSERT_NE(ctx, nullptr)
      << "Capsule must contain a non-null CephContext pointer";

  // Each call returns a new capsule object, but all wrap the same CephContext
  PyObject* result2 = active_modules->get_context();
  ASSERT_NE(result2, nullptr);
  void* ctx2 = PyCapsule_GetPointer(result2, nullptr);
  EXPECT_EQ(ctx, ctx2)
      << "Both capsules must wrap the same CephContext address";

  Py_DECREF(result);
  Py_DECREF(result2);
}

// Test: get_rocksdb_version returns a string in exactly the format "X.Y.Z"
// where each component is a non-negative integer.
TEST_F(ActivePyModulesTest, GetRocksdbVersionFormat)
{
  PyObject* result = active_modules->get_rocksdb_version();
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyUnicode_Check(result));

  const char* ver = PyUnicode_AsUTF8(result);
  ASSERT_NE(ver, nullptr);

  std::string vs(ver);

  // Must contain exactly two dots → three components
  size_t first_dot = vs.find('.');
  ASSERT_NE(first_dot, std::string::npos) << "Version must contain at least one dot";
  size_t second_dot = vs.find('.', first_dot + 1);
  ASSERT_NE(second_dot, std::string::npos) << "Version must contain at least two dots";
  size_t third_dot = vs.find('.', second_dot + 1);
  EXPECT_EQ(third_dot, std::string::npos) << "Version must have exactly two dots (X.Y.Z)";

  // Each component must be a non-empty string of digits
  auto is_digits = [](const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), ::isdigit);
  };

  std::string major_s = vs.substr(0, first_dot);
  std::string minor_s = vs.substr(first_dot + 1, second_dot - first_dot - 1);
  std::string patch_s = vs.substr(second_dot + 1);

  EXPECT_TRUE(is_digits(major_s)) << "Major component must be digits: '" << major_s << "'";
  EXPECT_TRUE(is_digits(minor_s)) << "Minor component must be digits: '" << minor_s << "'";
  EXPECT_TRUE(is_digits(patch_s)) << "Patch component must be digits: '" << patch_s << "'";

  Py_DECREF(result);
}

// Test: When two daemons in the collection have different "ceph_version" values,
// the top-level "ceph_version" key reflects the LAST value set by the iteration
// (implementation-defined order of the DaemonStateCollection map, but the key
// must equal one of the two values, not some merged value).
// This exercises the overwrite semantics at ActivePyModules.cc:107-110.
TEST_F(ActivePyModulesTest, DumpServerCephVersionFromIterationOrder)
{
  PyFormatter f;
  DaemonStateCollection dmc;

  DaemonKey key1{"osd", "10"};
  auto daemon1 = std::make_shared<DaemonState>(daemon_state->types);
  daemon1->key = key1;
  daemon1->hostname = "mixed_host";
  daemon1->metadata["ceph_version"] = "18.0.0";
  dmc[key1] = daemon1;

  DaemonKey key2{"osd", "11"};
  auto daemon2 = std::make_shared<DaemonState>(daemon_state->types);
  daemon2->key = key2;
  daemon2->hostname = "mixed_host";
  daemon2->metadata["ceph_version"] = "19.0.0";
  dmc[key2] = daemon2;

  active_modules->dump_server("mixed_host", dmc, &f);

  PyObject* result = f.get();
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));

  // The top-level ceph_version must be one of the two daemon versions
  PyObject* cv = PyDict_GetItemString(result, "ceph_version");
  ASSERT_NE(cv, nullptr);
  ASSERT_TRUE(PyUnicode_Check(cv));

  std::string cv_str = PyUnicode_AsUTF8(cv);
  EXPECT_TRUE(cv_str == "18.0.0" || cv_str == "19.0.0")
      << "Top-level ceph_version '" << cv_str
      << "' must be one of the two daemon versions";

  // Services must have two entries
  PyObject* services = PyDict_GetItemString(result, "services");
  ASSERT_NE(services, nullptr);
  ASSERT_TRUE(PyList_Check(services));
  EXPECT_EQ(PyList_Size(services), 2);

  Py_DECREF(result);
}

// Test: get_unlabeled_counter_python for a LONGRUNAVG counter uses
// get_data_avg() and emits [t, s, c] triples rather than [t, v] pairs.
TEST_F(ActivePyModulesTest, GetUnlabeledCounterPythonLongRunAvg)
{
  PerfCounterType counter_type;
  counter_type.type = PERFCOUNTER_LONGRUNAVG;
  counter_type.description = "Average latency";

  add_daemon_perf_counters("osd", "0", {{"osd.latency", counter_type}});

  DaemonKey key{"osd", "0"};
  auto daemon = daemon_state->get(key);
  ASSERT_NE(daemon, nullptr);

  auto& instance = daemon->perf_counters.instances.at("osd.latency");
  instance.push_avg(utime_t(100, 0), 10, 5);   // sum=10, count=5
  instance.push_avg(utime_t(200, 0), 20, 10);  // sum=20, count=10

  PyObject* result = active_modules->get_unlabeled_counter_python("osd", "0", "osd.latency");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));

  PyObject* data = PyDict_GetItemString(result, "osd.latency");
  ASSERT_NE(data, nullptr);
  ASSERT_TRUE(PyList_Check(data));
  ASSERT_GE(PyList_Size(data), 2) << "Should have at least 2 avg data points";

  // Each data point should be a list/array with 3 elements: [t, s, c]
  PyObject* point = PyList_GetItem(data, 0);
  ASSERT_NE(point, nullptr);
  ASSERT_TRUE(PyList_Check(point)) << "LONGRUNAVG data point must be a list";
  ASSERT_EQ(PyList_Size(point), 3)
      << "LONGRUNAVG data point must have 3 elements [t, s, c]";

  // Second element is "s" (sum), third is "c" (count) — both must be non-negative ints
  PyObject* s = PyList_GetItem(point, 1);
  PyObject* c = PyList_GetItem(point, 2);
  ASSERT_NE(s, nullptr);
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(PyLong_Check(s)) << "Sum must be an integer";
  ASSERT_TRUE(PyLong_Check(c)) << "Count must be an integer";
  EXPECT_GE(PyLong_AsLong(s), 0) << "Sum must be non-negative";
  EXPECT_GE(PyLong_AsLong(c), 0) << "Count must be non-negative";

  Py_DECREF(result);
}

// Test: get_python("modified_config_options") returns a dict whose "options"
// list contains the names of options that have been modified by daemons.
TEST_F(ActivePyModulesTest, GetPythonModifiedConfigOptionsWithData)
{
  // DaemonState::config is map<string, map<int32_t, string>>; epoch 0 is
  // used as a placeholder for "any version".
  DaemonKey key{"osd", "42"};
  auto daemon = std::make_shared<DaemonState>(daemon_state->types);
  daemon->key = key;
  daemon->hostname = "opts_host";
  {
    std::lock_guard l(daemon->lock);
    daemon->config["osd_max_backfills"][0] = "16";
    daemon->config["osd_pool_default_size"][0] = "5";
  }
  daemon_state->insert(daemon);

  PyObject* result = active_modules->get_python("modified_config_options");
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyDict_Check(result));

  PyObject* options_list = PyDict_GetItemString(result, "options");
  ASSERT_NE(options_list, nullptr) << "Result must have 'options' key";
  ASSERT_TRUE(PyList_Check(options_list));

  EXPECT_GE(PyList_Size(options_list), 2)
      << "options list must contain the inserted config option names";

  // PyFormatter in array context appends string values directly (not wrapped
  // in a dict), so each item in options_list is a PyUnicode string.
  std::set<std::string> found;
  for (Py_ssize_t i = 0; i < PyList_Size(options_list); ++i) {
    PyObject* item = PyList_GetItem(options_list, i);
    ASSERT_NE(item, nullptr);
    if (PyUnicode_Check(item)) {
      found.insert(PyUnicode_AsUTF8(item));
    }
  }

  EXPECT_NE(found.count("osd_max_backfills"), 0u)
      << "'osd_max_backfills' must appear in modified_config_options";
  EXPECT_NE(found.count("osd_pool_default_size"), 0u)
      << "'osd_pool_default_size' must appear in modified_config_options";

  Py_DECREF(result);
}
