// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

/*
 * Independent unit tests for GIL-sensitive ActivePyModule functions
 *
 * This test file tests individual ActivePyModule methods (notify, notify_clog)
 * that are GIL-sensitive. The test fixture ActivePyModulesNoGilTest is defined
 * in TestMgr_nogil.h along with other GIL-safe test helpers.
 *
 * These tests use minimal infrastructure and explicit GIL management to test
 * module-level behavior without the full Ceph stack.
 */

#include "TestMgr_nogil.h"
#include "mgr/Mgr.h"
#include "common/LogEntry.h"

#define dout_subsys ceph_subsys_mgr

// Minimal copy of the finish function from src/mgr/Mgr.cc
// Added to compile DaemonServer, simpler than including Mgr.cc in its entirety
void MetadataUpdate::finish(int r)
{
  daemon_state.clear_updating(key);
}

// Static member initialization
boost::intrusive_ptr<CephContext> ActivePyModulesNoGilTest::cct;

// ============================================================================
// NOTIFY TESTS
// ============================================================================

// notify() forwards notify_type and notify_id to the Python module's notify()
// method unchanged. Covers typical, empty, and special/escaped strings in one
// pass over a single code path: dropping either argument assignment (or a
// string-conversion bug in notify()) fails this for at least one input.
TEST_F(ActivePyModulesNoGilTest, Notify_RoundTripsArguments) {
  const char* module_code = R"(
class Module:
    def __init__(self):
        self.last_notify_type = None
        self.last_notify_id = None

    def notify(self, notify_type, notify_id):
        self.last_notify_type = notify_type
        self.last_notify_id = notify_id
        return True
)";

  auto module = create_test_module("test_module", module_code);
  ASSERT_NE(module, nullptr) << "Failed to create test module";

  const std::vector<std::pair<std::string, std::string>> cases = {
    {"osd_map", "12345"},                                       // typical
    {"", ""},                                                   // empty strings
    {"type_with_underscore", "id-with-dash/slash\\backslash"},  // special chars
  };

  for (const auto& [notify_type, notify_id] : cases) {
    // Release GIL before calling notify (it acquires it internally)
    PyThreadState* save = PyEval_SaveThread();
    module->notify(notify_type, notify_id);
    PyEval_RestoreThread(save);

    PyObject* instance = module->get_class_instance();
    ASSERT_NE(instance, nullptr);

    PyObject* last_type = PyObject_GetAttrString(instance, "last_notify_type");
    ASSERT_NE(last_type, nullptr) << "Failed to get last_notify_type attribute";
    ASSERT_TRUE(PyUnicode_Check(last_type)) << "last_notify_type is not a string";
    EXPECT_STREQ(PyUnicode_AsUTF8(last_type), notify_type.c_str());
    Py_DECREF(last_type);

    PyObject* last_id = PyObject_GetAttrString(instance, "last_notify_id");
    ASSERT_NE(last_id, nullptr) << "Failed to get last_notify_id attribute";
    ASSERT_TRUE(PyUnicode_Check(last_id)) << "last_notify_id is not a string";
    EXPECT_STREQ(PyUnicode_AsUTF8(last_id), notify_id.c_str());
    Py_DECREF(last_id);
  }
}

// A Python exception raised inside notify() must be swallowed (notify() returns
// void) AND leave the module usable: a subsequent notify() still runs and its
// side effect is observable, and no Python error is left pending. A bare
// SUCCEED() after the raising call would pass even if notify() aborted or
// poisoned the interpreter for later calls; these assertions would not.
TEST_F(ActivePyModulesNoGilTest, Notify_MethodRaisesException) {
  const char* module_code = R"(
class Module:
    def __init__(self):
        self.recovered = False

    def notify(self, notify_type, notify_id):
        if notify_type == "raise":
            raise RuntimeError("Notification handler error")
        self.recovered = True
)";

  auto module = create_test_module("test_module", module_code);
  ASSERT_NE(module, nullptr) << "Failed to create test module";

  // Release GIL before calling notify (it acquires it internally)
  PyThreadState* save = PyEval_SaveThread();
  module->notify("raise", "test_id");  // raises inside Python; must be swallowed
  module->notify("ok", "test_id");     // module must still be usable
  PyEval_RestoreThread(save);

  // The second notify ran to completion despite the first having raised.
  PyObject* instance = module->get_class_instance();
  ASSERT_NE(instance, nullptr);
  PyObject* recovered = PyObject_GetAttrString(instance, "recovered");
  ASSERT_NE(recovered, nullptr);
  EXPECT_EQ(recovered, Py_True) << "module unusable after notify() raised";
  Py_DECREF(recovered);

  // notify() must consume the exception, not leave it pending.
  EXPECT_EQ(PyErr_Occurred(), nullptr) << "notify() left a Python error set";
}

// Test 3: notify with multiple sequential calls
TEST_F(ActivePyModulesNoGilTest, Notify_MultipleSequentialCalls) {
  const char* module_code = R"(
class Module:
    def __init__(self):
        self.notify_count = 0
        self.last_notify_type = None
    
    def notify(self, notify_type, notify_id):
        self.notify_count += 1
        self.last_notify_type = notify_type
)";
  
  auto module = create_test_module("test_module", module_code);
  ASSERT_NE(module, nullptr) << "Failed to create test module";
  
  // Release GIL before calling notify
  PyThreadState* save = PyEval_SaveThread();
  
  // Call notify multiple times
  module->notify("osd_map", "1");
  module->notify("pg_summary", "2");
  module->notify("health", "3");
  
  // Re-acquire GIL to verify
  PyEval_RestoreThread(save);
  
  PyObject* instance = module->get_class_instance();
  ASSERT_NE(instance, nullptr);
  
  PyObject* count = PyObject_GetAttrString(instance, "notify_count");
  ASSERT_NE(count, nullptr);
  ASSERT_TRUE(PyLong_Check(count));
  EXPECT_EQ(PyLong_AsLong(count), 3);
  Py_DECREF(count);
  
  PyObject* last_type = PyObject_GetAttrString(instance, "last_notify_type");
  ASSERT_NE(last_type, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(last_type), "health");
  Py_DECREF(last_type);
}

// notify()'s return value is discarded (the C++ signature returns void), but
// the Python body must still run. The module sets an observable flag AND
// returns a non-None dict; asserting the flag proves the body executed and that
// a non-None return is accepted and ignored without leaving an error pending.
// (Merges two former tests: one checked an implicit-None return, the other only
// called SUCCEED() after a non-None return.)
TEST_F(ActivePyModulesNoGilTest, Notify_RunsBodyIgnoringReturnValue) {
  const char* module_code = R"(
class Module:
    def __init__(self):
        self.called = False

    def notify(self, notify_type, notify_id):
        self.called = True
        return {"status": "processed", "type": notify_type}
)";

  auto module = create_test_module("test_module", module_code);
  ASSERT_NE(module, nullptr);

  PyThreadState* save = PyEval_SaveThread();
  module->notify("test", "123");
  PyEval_RestoreThread(save);

  // The body ran even though its (non-None) return value is thrown away.
  PyObject* instance = module->get_class_instance();
  ASSERT_NE(instance, nullptr);
  PyObject* called = PyObject_GetAttrString(instance, "called");
  ASSERT_NE(called, nullptr);
  EXPECT_EQ(called, Py_True);
  Py_DECREF(called);

  // Returning a value must not leave a Python error pending.
  EXPECT_EQ(PyErr_Occurred(), nullptr) << "notify() left a Python error set";
}

// ============================================================================
// NOTIFY_CLOG TESTS
// ============================================================================

// A Python exception raised inside notify_clog()'s handler must be swallowed
// (notify_clog returns void) and leave the module usable: a later notify_clog()
// still runs its body, and no Python error is left pending. The former test
// only called SUCCEED() after the raising call.
TEST_F(ActivePyModulesNoGilTest, NotifyClog_MethodRaisesException) {
  const char* module_code = R"(
class Module:
    def __init__(self):
        self.recovered = False

    def notify(self, notify_type, log_entry):
        if notify_type == "clog":
            if log_entry.get('message') == 'raise':
                raise RuntimeError("Clog handler error")
            self.recovered = True
)";

  auto module = create_test_module("test_module", module_code);
  ASSERT_NE(module, nullptr) << "Failed to create test module";

  LogEntry raising;
  raising.msg = "raise";
  raising.channel = "cluster";
  raising.prio = CLOG_INFO;

  LogEntry ok;
  ok.msg = "ok";
  ok.channel = "cluster";
  ok.prio = CLOG_INFO;

  // Release GIL before calling notify_clog (it acquires it internally)
  PyThreadState* save = PyEval_SaveThread();
  module->notify_clog(raising);  // raises inside Python; must be swallowed
  module->notify_clog(ok);       // module must still be usable
  PyEval_RestoreThread(save);

  // The second notify_clog ran to completion despite the first having raised.
  PyObject* instance = module->get_class_instance();
  ASSERT_NE(instance, nullptr);
  PyObject* recovered = PyObject_GetAttrString(instance, "recovered");
  ASSERT_NE(recovered, nullptr);
  EXPECT_EQ(recovered, Py_True) << "module unusable after notify_clog() raised";
  Py_DECREF(recovered);

  // notify_clog() must consume the exception, not leave it pending.
  EXPECT_EQ(PyErr_Occurred(), nullptr) << "notify_clog() left a Python error set";
}

// Test 6: notify_clog with multiple log entries
TEST_F(ActivePyModulesNoGilTest, NotifyClog_MultipleLogEntries) {
  const char* module_code = R"(
class Module:
    def __init__(self):
        self.clog_count = 0
        self.last_message = None
    
    def notify(self, notify_type, log_entry):
        if notify_type == "clog":
            self.clog_count += 1
            self.last_message = log_entry.get('message', '')
)";
  
  auto module = create_test_module("test_module", module_code);
  ASSERT_NE(module, nullptr) << "Failed to create test module";
  
  // Release GIL before calling notify_clog
  PyThreadState* save = PyEval_SaveThread();
  
  // Send multiple log entries
  for (int i = 1; i <= 3; i++) {
    LogEntry log_entry;
    log_entry.msg = "Message " + std::to_string(i);
    log_entry.channel = "cluster";
    module->notify_clog(log_entry);
  }
  
  // Re-acquire GIL to verify
  PyEval_RestoreThread(save);
  
  PyObject* instance = module->get_class_instance();
  ASSERT_NE(instance, nullptr);
  
  PyObject* count = PyObject_GetAttrString(instance, "clog_count");
  ASSERT_NE(count, nullptr);
  ASSERT_TRUE(PyLong_Check(count));
  EXPECT_EQ(PyLong_AsLong(count), 3);
  Py_DECREF(count);
  
  PyObject* last_msg = PyObject_GetAttrString(instance, "last_message");
  ASSERT_NE(last_msg, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(last_msg), "Message 3");
  Py_DECREF(last_msg);
}

// notify_clog() must serialize each entry's priority into the dict's "priority"
// field (the clog_type stream representation). The module keeps every entry and
// we assert the per-entry priority string for each level -- the behavior the
// name promises. (Formerly this only checked the list size, duplicating
// NotifyClog_MultipleLogEntries.)
TEST_F(ActivePyModulesNoGilTest, NotifyClog_DifferentPriorities) {
  const char* module_code = R"(
class Module:
    def __init__(self):
        self.log_entries = []

    def notify(self, notify_type, log_entry):
        if notify_type == "clog":
            self.log_entries.append(log_entry)
)";

  auto module = create_test_module("test_module", module_code);
  ASSERT_NE(module, nullptr);

  // Each priority level paired with its expected serialized "priority" string
  // (see operator<<(std::ostream&, clog_type) in common/LogEntry.h).
  const std::vector<std::pair<clog_type, const char*>> cases = {
    {CLOG_DEBUG, "[DBG]"},
    {CLOG_INFO,  "[INF]"},
    {CLOG_WARN,  "[WRN]"},
    {CLOG_ERROR, "[ERR]"},
    {CLOG_SEC,   "[SEC]"},
  };

  PyThreadState* save = PyEval_SaveThread();
  for (const auto& c : cases) {
    LogEntry log_entry;
    log_entry.prio = c.first;
    log_entry.msg = "Test message";
    log_entry.channel = "cluster";
    module->notify_clog(log_entry);
  }
  PyEval_RestoreThread(save);

  PyObject* instance = module->get_class_instance();
  ASSERT_NE(instance, nullptr);
  PyObject* log_entries = PyObject_GetAttrString(instance, "log_entries");
  ASSERT_NE(log_entries, nullptr);
  ASSERT_TRUE(PyList_Check(log_entries));
  ASSERT_EQ(PyList_Size(log_entries), static_cast<Py_ssize_t>(cases.size()));

  for (size_t i = 0; i < cases.size(); ++i) {
    PyObject* entry = PyList_GetItem(log_entries, i);  // borrowed reference
    ASSERT_NE(entry, nullptr);
    ASSERT_TRUE(PyDict_Check(entry));
    PyObject* priority = PyDict_GetItemString(entry, "priority");  // borrowed
    ASSERT_NE(priority, nullptr) << "entry " << i << " missing 'priority' field";
    ASSERT_TRUE(PyUnicode_Check(priority));
    EXPECT_STREQ(PyUnicode_AsUTF8(priority), cases[i].second)
      << "wrong priority string for entry " << i;
  }

  Py_DECREF(log_entries);
}

// Test 22: notify_clog with different channels
TEST_F(ActivePyModulesNoGilTest, NotifyClog_DifferentChannels) {
  const char* module_code = R"(
class Module:
    def __init__(self):
        self.channels = []
    
    def notify(self, notify_type, log_entry):
        if notify_type == "clog":
            self.channels.append(log_entry.get('channel', 'unknown'))
)";
  
  auto module = create_test_module("test_module", module_code);
  ASSERT_NE(module, nullptr);
  
  PyThreadState* save = PyEval_SaveThread();
  
  // Test different channels
  std::vector<std::string> channels = {"cluster", "audit", "ceph", "custom"};
  
  for (const auto& channel : channels) {
    LogEntry log_entry;
    log_entry.msg = "Test message";
    log_entry.channel = channel;
    log_entry.prio = CLOG_INFO;
    module->notify_clog(log_entry);
  }
  
  PyEval_RestoreThread(save);
  
  PyObject* instance = module->get_class_instance();
  PyObject* channels_list = PyObject_GetAttrString(instance, "channels");
  ASSERT_NE(channels_list, nullptr);
  ASSERT_TRUE(PyList_Check(channels_list));
  EXPECT_EQ(PyList_Size(channels_list), 4);
  
  // Verify channel names
  for (size_t i = 0; i < channels.size(); i++) {
    PyObject* item = PyList_GetItem(channels_list, i);
    ASSERT_NE(item, nullptr);
    EXPECT_STREQ(PyUnicode_AsUTF8(item), channels[i].c_str());
  }
  
  Py_DECREF(channels_list);
}

// notify_clog() must round-trip the log message verbatim into the dict's
// "message" field for empty, special/escaped, and long inputs, and the
// "message" key must always be PRESENT. A module using log_entry.get('message',
// '') cannot otherwise tell an empty message from a missing key, so we assert
// presence explicitly via `'message' in log_entry`. (Merges three former
// per-input tests that shared one code path.)
TEST_F(ActivePyModulesNoGilTest, NotifyClog_MessageRoundTrip) {
  const char* module_code = R"(
class Module:
    def __init__(self):
        self.last_entry = None
        self.message_present = None

    def notify(self, notify_type, log_entry):
        if notify_type == "clog":
            self.last_entry = log_entry
            self.message_present = 'message' in log_entry
)";

  auto module = create_test_module("test_module", module_code);
  ASSERT_NE(module, nullptr);

  const std::vector<std::string> messages = {
    "",                                                                    // empty
    "Message with\nnewlines\tand\ttabs and \"quotes\" and 'apostrophes'",  // special
    std::string(1000, 'x'),                                                // long
  };

  for (const auto& msg : messages) {
    LogEntry log_entry;
    log_entry.msg = msg;
    log_entry.channel = "cluster";
    log_entry.prio = CLOG_INFO;

    PyThreadState* save = PyEval_SaveThread();
    module->notify_clog(log_entry);
    PyEval_RestoreThread(save);

    PyObject* instance = module->get_class_instance();
    ASSERT_NE(instance, nullptr);

    // The 'message' key must be present, so an empty message is distinguishable
    // from a missing key.
    PyObject* present = PyObject_GetAttrString(instance, "message_present");
    ASSERT_NE(present, nullptr);
    EXPECT_EQ(present, Py_True)
      << "'message' key absent for input of length " << msg.size();
    Py_DECREF(present);

    PyObject* last_entry = PyObject_GetAttrString(instance, "last_entry");
    ASSERT_NE(last_entry, nullptr);
    ASSERT_TRUE(PyDict_Check(last_entry));
    PyObject* message = PyDict_GetItemString(last_entry, "message");  // borrowed
    ASSERT_NE(message, nullptr);
    ASSERT_TRUE(PyUnicode_Check(message));
    EXPECT_STREQ(PyUnicode_AsUTF8(message), msg.c_str());
    Py_DECREF(last_entry);
  }
}

// Test 26: notify_clog verifying all LogEntry fields are passed
TEST_F(ActivePyModulesNoGilTest, NotifyClog_AllFields) {
  const char* module_code = R"(
class Module:
    def __init__(self):
        self.last_entry = None
    
    def notify(self, notify_type, log_entry):
        if notify_type == "clog":
            self.last_entry = log_entry
)";
  
  auto module = create_test_module("test_module", module_code);
  ASSERT_NE(module, nullptr);
  
  LogEntry log_entry;
  log_entry.stamp = ceph_clock_now();
  log_entry.seq = 12345;
  log_entry.prio = CLOG_WARN;
  log_entry.msg = "Complete log entry test";
  log_entry.channel = "audit";
  
  PyThreadState* save = PyEval_SaveThread();
  
  module->notify_clog(log_entry);
  
  PyEval_RestoreThread(save);
  
  PyObject* instance = module->get_class_instance();
  PyObject* last_entry = PyObject_GetAttrString(instance, "last_entry");
  ASSERT_NE(last_entry, nullptr);
  ASSERT_TRUE(PyDict_Check(last_entry));
  
  // Verify all expected fields are present
  PyObject* message = PyDict_GetItemString(last_entry, "message");
  ASSERT_NE(message, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(message), "Complete log entry test");
  
  PyObject* channel = PyDict_GetItemString(last_entry, "channel");
  ASSERT_NE(channel, nullptr);
  EXPECT_STREQ(PyUnicode_AsUTF8(channel), "audit");
  
  PyObject* seq = PyDict_GetItemString(last_entry, "seq");
  ASSERT_NE(seq, nullptr);
  EXPECT_EQ(PyLong_AsLong(seq), 12345);
  
  // Priority should be present (field name is "priority" not "prio")
  // Note: priority is dumped as a string representation, not an integer
  PyObject* priority = PyDict_GetItemString(last_entry, "priority");
  ASSERT_NE(priority, nullptr);
  ASSERT_TRUE(PyUnicode_Check(priority));
  
  Py_DECREF(last_entry);
}

// ============================================================================
// DISPATCH_REMOTE TESTS
// ============================================================================

// Test 7: dispatch_remote with valid method and arguments
TEST_F(ActivePyModulesNoGilTest, DispatchRemote_ValidMethodAndArgs) {
  const char* module_code = R"(
class Module:
    def remote_method(self, arg1, arg2, kwarg1=None):
        return {"result": arg1 + arg2, "kwarg1": kwarg1}
)";
  
  auto module = create_test_module("test_module", module_code);
  ASSERT_NE(module, nullptr) << "Failed to create test module";
  
  // Prepare pickled arguments using Python's pickle module (GIL is held)
  PyObject* pickle_module = PyImport_ImportModule("pickle");
  ASSERT_NE(pickle_module, nullptr) << "Failed to import pickle module";
  
  // Create args tuple: (10, 20)
  PyObject* args = PyTuple_New(2);
  PyTuple_SetItem(args, 0, PyLong_FromLong(10));
  PyTuple_SetItem(args, 1, PyLong_FromLong(20));
  
  // Create kwargs dict: {"kwarg1": "test_value"}
  PyObject* kwargs = PyDict_New();
  PyDict_SetItemString(kwargs, "kwarg1", PyUnicode_FromString("test_value"));
  
  // Pickle args and kwargs with protocol 4 (compatible with Python 3.8+)
  PyObject* pickled_args = PyObject_CallMethod(pickle_module, "dumps", "Oi", args, 4);
  ASSERT_NE(pickled_args, nullptr) << "Failed to pickle args";
  
  PyObject* pickled_kwargs = PyObject_CallMethod(pickle_module, "dumps", "Oi", kwargs, 4);
  ASSERT_NE(pickled_kwargs, nullptr) << "Failed to pickle kwargs";
  
  // Convert to byte spans
  Py_ssize_t args_size, kwargs_size;
  char* args_buf;
  char* kwargs_buf;
  PyBytes_AsStringAndSize(pickled_args, &args_buf, &args_size);
  PyBytes_AsStringAndSize(pickled_kwargs, &kwargs_buf, &kwargs_size);
  
  std::span<std::byte const> args_span(
    reinterpret_cast<const std::byte*>(args_buf), args_size);
  std::span<std::byte const> kwargs_span(
    reinterpret_cast<const std::byte*>(kwargs_buf), kwargs_size);
  
  // Release GIL before calling dispatch_remote (it will acquire it internally)
  PyThreadState* save = PyEval_SaveThread();
  
  // Call dispatch_remote
  std::string err;
  auto result = module->dispatch_remote("remote_method", args_span, kwargs_span, &err);
  
  // Re-acquire GIL to verify results
  PyEval_RestoreThread(save);
  
  // Verify result
  ASSERT_TRUE(result.has_value()) << "dispatch_remote failed: " << err;
  EXPECT_TRUE(err.empty()) << "Error message should be empty on success";
  
  // Unpickle the result
  PyObject* result_bytes = PyBytes_FromStringAndSize(
    reinterpret_cast<const char*>(result->data()), result->size());
  PyObject* unpickled_result = PyObject_CallMethod(
    pickle_module, "loads", "O", result_bytes);
  ASSERT_NE(unpickled_result, nullptr) << "Failed to unpickle result";
  
  // Verify the result is a dict with expected values
  ASSERT_TRUE(PyDict_Check(unpickled_result)) << "Result is not a dict";
  
  PyObject* result_value = PyDict_GetItemString(unpickled_result, "result");
  ASSERT_NE(result_value, nullptr) << "Result dict missing 'result' key";
  EXPECT_EQ(PyLong_AsLong(result_value), 30) << "Expected result to be 30";
  
  PyObject* kwarg1_value = PyDict_GetItemString(unpickled_result, "kwarg1");
  ASSERT_NE(kwarg1_value, nullptr) << "Result dict missing 'kwarg1' key";
  EXPECT_STREQ(PyUnicode_AsUTF8(kwarg1_value), "test_value");
  
  // Cleanup
  Py_DECREF(unpickled_result);
  Py_DECREF(result_bytes);
  Py_DECREF(pickled_kwargs);
  Py_DECREF(pickled_args);
  Py_DECREF(kwargs);
  Py_DECREF(args);
  Py_DECREF(pickle_module);
}

// Test 8: dispatch_remote with method that raises exception
TEST_F(ActivePyModulesNoGilTest, DispatchRemote_MethodRaisesException) {
  const char* module_code = R"(
class Module:
    def failing_method(self):
        raise RuntimeError("Method intentionally fails")
)";
  
  auto module = create_test_module("test_module", module_code);
  ASSERT_NE(module, nullptr) << "Failed to create test module";
  
  // Prepare empty pickled arguments
  PyObject* pickle_module = PyImport_ImportModule("pickle");
  ASSERT_NE(pickle_module, nullptr);
  
  PyObject* empty_tuple = PyTuple_New(0);
  PyObject* empty_dict = PyDict_New();
  
  PyObject* pickled_args = PyObject_CallMethod(pickle_module, "dumps", "Oi", empty_tuple, 4);
  PyObject* pickled_kwargs = PyObject_CallMethod(pickle_module, "dumps", "Oi", empty_dict, 4);
  
  ASSERT_NE(pickled_args, nullptr) << "Failed to pickle empty args";
  ASSERT_NE(pickled_kwargs, nullptr) << "Failed to pickle empty kwargs";
  
  Py_ssize_t args_size, kwargs_size;
  char* args_buf;
  char* kwargs_buf;
  PyBytes_AsStringAndSize(pickled_args, &args_buf, &args_size);
  PyBytes_AsStringAndSize(pickled_kwargs, &kwargs_buf, &kwargs_size);
  
  std::span<std::byte const> args_span(
    reinterpret_cast<const std::byte*>(args_buf), args_size);
  std::span<std::byte const> kwargs_span(
    reinterpret_cast<const std::byte*>(kwargs_buf), kwargs_size);
  
  // Release GIL before calling dispatch_remote
  PyThreadState* save = PyEval_SaveThread();
  
  // Call dispatch_remote - should handle exception gracefully
  std::string err;
  auto result = module->dispatch_remote("failing_method", args_span, kwargs_span, &err);
  
  // Re-acquire GIL
  PyEval_RestoreThread(save);
  
  // Verify that it returns nullopt and sets error message
  EXPECT_FALSE(result.has_value()) << "dispatch_remote should return nullopt on exception";
  EXPECT_FALSE(err.empty()) << "Error message should be set on exception";
  EXPECT_NE(err.find("RuntimeError"), std::string::npos)
    << "Error should mention RuntimeError: " << err;
  
  // Cleanup
  Py_DECREF(pickled_kwargs);
  Py_DECREF(pickled_args);
  Py_DECREF(empty_dict);
  Py_DECREF(empty_tuple);
  Py_DECREF(pickle_module);
}

// Test 9: dispatch_remote with no arguments
TEST_F(ActivePyModulesNoGilTest, DispatchRemote_NoArguments) {
  const char* module_code = R"(
class Module:
    def no_args_method(self):
        return "success"
)";
  
  auto module = create_test_module("test_module", module_code);
  ASSERT_NE(module, nullptr) << "Failed to create test module";
  
  // Prepare empty pickled arguments
  PyObject* pickle_module = PyImport_ImportModule("pickle");
  ASSERT_NE(pickle_module, nullptr);
  
  PyObject* empty_tuple = PyTuple_New(0);
  PyObject* empty_dict = PyDict_New();
  
  PyObject* pickled_args = PyObject_CallMethod(pickle_module, "dumps", "Oi", empty_tuple, 4);
  PyObject* pickled_kwargs = PyObject_CallMethod(pickle_module, "dumps", "Oi", empty_dict, 4);
  
  Py_ssize_t args_size, kwargs_size;
  char* args_buf;
  char* kwargs_buf;
  PyBytes_AsStringAndSize(pickled_args, &args_buf, &args_size);
  PyBytes_AsStringAndSize(pickled_kwargs, &kwargs_buf, &kwargs_size);
  
  std::span<std::byte const> args_span(
    reinterpret_cast<const std::byte*>(args_buf), args_size);
  std::span<std::byte const> kwargs_span(
    reinterpret_cast<const std::byte*>(kwargs_buf), kwargs_size);
  
  // Release GIL before calling dispatch_remote
  PyThreadState* save = PyEval_SaveThread();
  
  std::string err;
  auto result = module->dispatch_remote("no_args_method", args_span, kwargs_span, &err);
  
  // Re-acquire GIL
  PyEval_RestoreThread(save);
  
  ASSERT_TRUE(result.has_value()) << "dispatch_remote failed: " << err;
  
  // Unpickle and verify result
  PyObject* result_bytes = PyBytes_FromStringAndSize(
    reinterpret_cast<const char*>(result->data()), result->size());
  PyObject* unpickled_result = PyObject_CallMethod(
    pickle_module, "loads", "O", result_bytes);
  ASSERT_NE(unpickled_result, nullptr);
  ASSERT_TRUE(PyUnicode_Check(unpickled_result));
  EXPECT_STREQ(PyUnicode_AsUTF8(unpickled_result), "success");
  
  // Cleanup
  Py_DECREF(unpickled_result);
  Py_DECREF(result_bytes);
  Py_DECREF(pickled_kwargs);
  Py_DECREF(pickled_args);
  Py_DECREF(empty_dict);
  Py_DECREF(empty_tuple);
  Py_DECREF(pickle_module);
}

// Test 10: dispatch_remote with only positional arguments
TEST_F(ActivePyModulesNoGilTest, DispatchRemote_OnlyPositionalArgs) {
  const char* module_code = R"(
class Module:
    def positional_method(self, a, b, c):
        return a + b + c
)";
  
  auto module = create_test_module("test_module", module_code);
  ASSERT_NE(module, nullptr);
  
  PyObject* pickle_module = PyImport_ImportModule("pickle");
  ASSERT_NE(pickle_module, nullptr);
  
  // Create args: (1, 2, 3)
  PyObject* args = PyTuple_New(3);
  PyTuple_SetItem(args, 0, PyLong_FromLong(1));
  PyTuple_SetItem(args, 1, PyLong_FromLong(2));
  PyTuple_SetItem(args, 2, PyLong_FromLong(3));
  
  PyObject* empty_dict = PyDict_New();
  
  PyObject* pickled_args = PyObject_CallMethod(pickle_module, "dumps", "Oi", args, 4);
  PyObject* pickled_kwargs = PyObject_CallMethod(pickle_module, "dumps", "Oi", empty_dict, 4);
  
  Py_ssize_t args_size, kwargs_size;
  char* args_buf;
  char* kwargs_buf;
  PyBytes_AsStringAndSize(pickled_args, &args_buf, &args_size);
  PyBytes_AsStringAndSize(pickled_kwargs, &kwargs_buf, &kwargs_size);
  
  std::span<std::byte const> args_span(
    reinterpret_cast<const std::byte*>(args_buf), args_size);
  std::span<std::byte const> kwargs_span(
    reinterpret_cast<const std::byte*>(kwargs_buf), kwargs_size);
  
  PyThreadState* save = PyEval_SaveThread();
  
  std::string err;
  auto result = module->dispatch_remote("positional_method", args_span, kwargs_span, &err);
  
  PyEval_RestoreThread(save);
  
  ASSERT_TRUE(result.has_value()) << "dispatch_remote failed: " << err;
  
  PyObject* result_bytes = PyBytes_FromStringAndSize(
    reinterpret_cast<const char*>(result->data()), result->size());
  PyObject* unpickled_result = PyObject_CallMethod(
    pickle_module, "loads", "O", result_bytes);
  ASSERT_NE(unpickled_result, nullptr);
  EXPECT_EQ(PyLong_AsLong(unpickled_result), 6);
  
  Py_DECREF(unpickled_result);
  Py_DECREF(result_bytes);
  Py_DECREF(pickled_kwargs);
  Py_DECREF(pickled_args);
  Py_DECREF(empty_dict);
  Py_DECREF(args);
  Py_DECREF(pickle_module);
}

// Test 11: dispatch_remote with only keyword arguments
TEST_F(ActivePyModulesNoGilTest, DispatchRemote_OnlyKeywordArgs) {
  const char* module_code = R"(
class Module:
    def keyword_method(self, x=0, y=0, z=0):
        return x * y * z
)";
  
  auto module = create_test_module("test_module", module_code);
  ASSERT_NE(module, nullptr);
  
  PyObject* pickle_module = PyImport_ImportModule("pickle");
  ASSERT_NE(pickle_module, nullptr);
  
  PyObject* empty_tuple = PyTuple_New(0);
  
  // Create kwargs: {"x": 2, "y": 3, "z": 4}
  PyObject* kwargs = PyDict_New();
  PyDict_SetItemString(kwargs, "x", PyLong_FromLong(2));
  PyDict_SetItemString(kwargs, "y", PyLong_FromLong(3));
  PyDict_SetItemString(kwargs, "z", PyLong_FromLong(4));
  
  PyObject* pickled_args = PyObject_CallMethod(pickle_module, "dumps", "Oi", empty_tuple, 4);
  PyObject* pickled_kwargs = PyObject_CallMethod(pickle_module, "dumps", "Oi", kwargs, 4);
  
  Py_ssize_t args_size, kwargs_size;
  char* args_buf;
  char* kwargs_buf;
  PyBytes_AsStringAndSize(pickled_args, &args_buf, &args_size);
  PyBytes_AsStringAndSize(pickled_kwargs, &kwargs_buf, &kwargs_size);
  
  std::span<std::byte const> args_span(
    reinterpret_cast<const std::byte*>(args_buf), args_size);
  std::span<std::byte const> kwargs_span(
    reinterpret_cast<const std::byte*>(kwargs_buf), kwargs_size);
  
  PyThreadState* save = PyEval_SaveThread();
  
  std::string err;
  auto result = module->dispatch_remote("keyword_method", args_span, kwargs_span, &err);
  
  PyEval_RestoreThread(save);
  
  ASSERT_TRUE(result.has_value()) << "dispatch_remote failed: " << err;
  
  PyObject* result_bytes = PyBytes_FromStringAndSize(
    reinterpret_cast<const char*>(result->data()), result->size());
  PyObject* unpickled_result = PyObject_CallMethod(
    pickle_module, "loads", "O", result_bytes);
  ASSERT_NE(unpickled_result, nullptr);
  EXPECT_EQ(PyLong_AsLong(unpickled_result), 24);
  
  Py_DECREF(unpickled_result);
  Py_DECREF(result_bytes);
  Py_DECREF(pickled_kwargs);
  Py_DECREF(pickled_args);
  Py_DECREF(kwargs);
  Py_DECREF(empty_tuple);
  Py_DECREF(pickle_module);
}

// Test 12: dispatch_remote with complex return types
TEST_F(ActivePyModulesNoGilTest, DispatchRemote_ComplexReturnTypes) {
  const char* module_code = R"(
class Module:
    def complex_return(self):
        return {
            "list": [1, 2, 3],
            "dict": {"nested": "value"},
            "tuple": (4, 5, 6),
            "string": "test",
            "number": 42,
            "bool": True,
            "none": None
        }
)";
  
  auto module = create_test_module("test_module", module_code);
  ASSERT_NE(module, nullptr);
  
  PyObject* pickle_module = PyImport_ImportModule("pickle");
  ASSERT_NE(pickle_module, nullptr);
  
  PyObject* empty_tuple = PyTuple_New(0);
  PyObject* empty_dict = PyDict_New();
  
  PyObject* pickled_args = PyObject_CallMethod(pickle_module, "dumps", "Oi", empty_tuple, 4);
  PyObject* pickled_kwargs = PyObject_CallMethod(pickle_module, "dumps", "Oi", empty_dict, 4);
  
  Py_ssize_t args_size, kwargs_size;
  char* args_buf;
  char* kwargs_buf;
  PyBytes_AsStringAndSize(pickled_args, &args_buf, &args_size);
  PyBytes_AsStringAndSize(pickled_kwargs, &kwargs_buf, &kwargs_size);
  
  std::span<std::byte const> args_span(
    reinterpret_cast<const std::byte*>(args_buf), args_size);
  std::span<std::byte const> kwargs_span(
    reinterpret_cast<const std::byte*>(kwargs_buf), kwargs_size);
  
  PyThreadState* save = PyEval_SaveThread();
  
  std::string err;
  auto result = module->dispatch_remote("complex_return", args_span, kwargs_span, &err);
  
  PyEval_RestoreThread(save);
  
  ASSERT_TRUE(result.has_value()) << "dispatch_remote failed: " << err;
  
  PyObject* result_bytes = PyBytes_FromStringAndSize(
    reinterpret_cast<const char*>(result->data()), result->size());
  PyObject* unpickled_result = PyObject_CallMethod(
    pickle_module, "loads", "O", result_bytes);
  ASSERT_NE(unpickled_result, nullptr);
  ASSERT_TRUE(PyDict_Check(unpickled_result));
  
  // Verify some fields
  PyObject* list_val = PyDict_GetItemString(unpickled_result, "list");
  ASSERT_NE(list_val, nullptr);
  ASSERT_TRUE(PyList_Check(list_val));
  EXPECT_EQ(PyList_Size(list_val), 3);
  
  PyObject* number_val = PyDict_GetItemString(unpickled_result, "number");
  ASSERT_NE(number_val, nullptr);
  EXPECT_EQ(PyLong_AsLong(number_val), 42);
  
  PyObject* none_val = PyDict_GetItemString(unpickled_result, "none");
  ASSERT_NE(none_val, nullptr);
  EXPECT_EQ(none_val, Py_None);
  
  Py_DECREF(unpickled_result);
  Py_DECREF(result_bytes);
  Py_DECREF(pickled_kwargs);
  Py_DECREF(pickled_args);
  Py_DECREF(empty_dict);
  Py_DECREF(empty_tuple);
  Py_DECREF(pickle_module);
}

// Test 13: dispatch_remote with method returning None
TEST_F(ActivePyModulesNoGilTest, DispatchRemote_ReturnsNone) {
  const char* module_code = R"(
class Module:
    def returns_none(self):
        return None
)";
  
  auto module = create_test_module("test_module", module_code);
  ASSERT_NE(module, nullptr);
  
  PyObject* pickle_module = PyImport_ImportModule("pickle");
  ASSERT_NE(pickle_module, nullptr);
  
  PyObject* empty_tuple = PyTuple_New(0);
  PyObject* empty_dict = PyDict_New();
  
  PyObject* pickled_args = PyObject_CallMethod(pickle_module, "dumps", "Oi", empty_tuple, 4);
  PyObject* pickled_kwargs = PyObject_CallMethod(pickle_module, "dumps", "Oi", empty_dict, 4);
  
  Py_ssize_t args_size, kwargs_size;
  char* args_buf;
  char* kwargs_buf;
  PyBytes_AsStringAndSize(pickled_args, &args_buf, &args_size);
  PyBytes_AsStringAndSize(pickled_kwargs, &kwargs_buf, &kwargs_size);
  
  std::span<std::byte const> args_span(
    reinterpret_cast<const std::byte*>(args_buf), args_size);
  std::span<std::byte const> kwargs_span(
    reinterpret_cast<const std::byte*>(kwargs_buf), kwargs_size);
  
  PyThreadState* save = PyEval_SaveThread();
  
  std::string err;
  auto result = module->dispatch_remote("returns_none", args_span, kwargs_span, &err);
  
  PyEval_RestoreThread(save);
  
  ASSERT_TRUE(result.has_value()) << "dispatch_remote failed: " << err;
  
  PyObject* result_bytes = PyBytes_FromStringAndSize(
    reinterpret_cast<const char*>(result->data()), result->size());
  PyObject* unpickled_result = PyObject_CallMethod(
    pickle_module, "loads", "O", result_bytes);
  ASSERT_NE(unpickled_result, nullptr);
  EXPECT_EQ(unpickled_result, Py_None);
  
  Py_DECREF(unpickled_result);
  Py_DECREF(result_bytes);
  Py_DECREF(pickled_kwargs);
  Py_DECREF(pickled_args);
  Py_DECREF(empty_dict);
  Py_DECREF(empty_tuple);
  Py_DECREF(pickle_module);
}

// Test 14: dispatch_remote with wrong number of arguments
TEST_F(ActivePyModulesNoGilTest, DispatchRemote_WrongNumberOfArgs) {
  const char* module_code = R"(
class Module:
    def needs_two_args(self, a, b):
        return a + b
)";
  
  auto module = create_test_module("test_module", module_code);
  ASSERT_NE(module, nullptr);
  
  PyObject* pickle_module = PyImport_ImportModule("pickle");
  ASSERT_NE(pickle_module, nullptr);
  
  // Only provide one argument when two are required
  PyObject* args = PyTuple_New(1);
  PyTuple_SetItem(args, 0, PyLong_FromLong(5));
  
  PyObject* empty_dict = PyDict_New();
  
  PyObject* pickled_args = PyObject_CallMethod(pickle_module, "dumps", "Oi", args, 4);
  PyObject* pickled_kwargs = PyObject_CallMethod(pickle_module, "dumps", "Oi", empty_dict, 4);
  
  Py_ssize_t args_size, kwargs_size;
  char* args_buf;
  char* kwargs_buf;
  PyBytes_AsStringAndSize(pickled_args, &args_buf, &args_size);
  PyBytes_AsStringAndSize(pickled_kwargs, &kwargs_buf, &kwargs_size);
  
  std::span<std::byte const> args_span(
    reinterpret_cast<const std::byte*>(args_buf), args_size);
  std::span<std::byte const> kwargs_span(
    reinterpret_cast<const std::byte*>(kwargs_buf), kwargs_size);
  
  PyThreadState* save = PyEval_SaveThread();
  
  std::string err;
  auto result = module->dispatch_remote("needs_two_args", args_span, kwargs_span, &err);
  
  PyEval_RestoreThread(save);
  
  // Should fail with TypeError
  EXPECT_FALSE(result.has_value());
  EXPECT_FALSE(err.empty());
  EXPECT_NE(err.find("TypeError"), std::string::npos) << "Error: " << err;
  
  Py_DECREF(pickled_kwargs);
  Py_DECREF(pickled_args);
  Py_DECREF(empty_dict);
  Py_DECREF(args);
  Py_DECREF(pickle_module);
}

// Test 15: dispatch_remote with invalid pickled data
TEST_F(ActivePyModulesNoGilTest, DispatchRemote_InvalidPickledArgs) {
  const char* module_code = R"(
class Module:
    def some_method(self):
        return "success"
)";
  
  auto module = create_test_module("test_module", module_code);
  ASSERT_NE(module, nullptr);
  
  // Create invalid pickled data (just random bytes)
  const char* invalid_data = "this is not valid pickled data";
  std::span<std::byte const> invalid_args_span(
    reinterpret_cast<const std::byte*>(invalid_data), strlen(invalid_data));
  
  // Valid empty kwargs
  PyObject* pickle_module = PyImport_ImportModule("pickle");
  PyObject* empty_dict = PyDict_New();
  PyObject* pickled_kwargs = PyObject_CallMethod(pickle_module, "dumps", "Oi", empty_dict, 4);
  
  Py_ssize_t kwargs_size;
  char* kwargs_buf;
  PyBytes_AsStringAndSize(pickled_kwargs, &kwargs_buf, &kwargs_size);
  std::span<std::byte const> kwargs_span(
    reinterpret_cast<const std::byte*>(kwargs_buf), kwargs_size);
  
  PyThreadState* save = PyEval_SaveThread();
  
  std::string err;
  auto result = module->dispatch_remote("some_method", invalid_args_span, kwargs_span, &err);
  
  PyEval_RestoreThread(save);
  
  // Should fail to unpickle
  EXPECT_FALSE(result.has_value());
  EXPECT_FALSE(err.empty());
  
  Py_DECREF(pickled_kwargs);
  Py_DECREF(empty_dict);
  Py_DECREF(pickle_module);
}

// Test 16: dispatch_remote with method that modifies module state
TEST_F(ActivePyModulesNoGilTest, DispatchRemote_ModifiesState) {
  const char* module_code = R"(
class Module:
    def __init__(self):
        self.counter = 0
    
    def increment(self, amount):
        self.counter += amount
        return self.counter
)";
  
  auto module = create_test_module("test_module", module_code);
  ASSERT_NE(module, nullptr);
  
  PyObject* pickle_module = PyImport_ImportModule("pickle");
  ASSERT_NE(pickle_module, nullptr);
  
  // Call increment(5) three times
  for (int i = 0; i < 3; i++) {
    PyObject* args = PyTuple_New(1);
    PyTuple_SetItem(args, 0, PyLong_FromLong(5));
    PyObject* empty_dict = PyDict_New();
    
    PyObject* pickled_args = PyObject_CallMethod(pickle_module, "dumps", "Oi", args, 4);
    PyObject* pickled_kwargs = PyObject_CallMethod(pickle_module, "dumps", "Oi", empty_dict, 4);
    
    Py_ssize_t args_size, kwargs_size;
    char* args_buf;
    char* kwargs_buf;
    PyBytes_AsStringAndSize(pickled_args, &args_buf, &args_size);
    PyBytes_AsStringAndSize(pickled_kwargs, &kwargs_buf, &kwargs_size);
    
    std::span<std::byte const> args_span(
      reinterpret_cast<const std::byte*>(args_buf), args_size);
    std::span<std::byte const> kwargs_span(
      reinterpret_cast<const std::byte*>(kwargs_buf), kwargs_size);
    
    PyThreadState* save = PyEval_SaveThread();
    
    std::string err;
    auto result = module->dispatch_remote("increment", args_span, kwargs_span, &err);
    
    PyEval_RestoreThread(save);
    
    ASSERT_TRUE(result.has_value()) << "Call " << i << " failed: " << err;
    
    PyObject* result_bytes = PyBytes_FromStringAndSize(
      reinterpret_cast<const char*>(result->data()), result->size());
    PyObject* unpickled_result = PyObject_CallMethod(
      pickle_module, "loads", "O", result_bytes);
    ASSERT_NE(unpickled_result, nullptr);
    EXPECT_EQ(PyLong_AsLong(unpickled_result), (i + 1) * 5);
    
    Py_DECREF(unpickled_result);
    Py_DECREF(result_bytes);
    Py_DECREF(pickled_kwargs);
    Py_DECREF(pickled_args);
    Py_DECREF(empty_dict);
    Py_DECREF(args);
  }
  
  Py_DECREF(pickle_module);
}

// Made with Bob
