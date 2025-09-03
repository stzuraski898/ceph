#include "mgr/PyFormatter.h"
#include "TestMgr.h"
#include "gtest/gtest.h"
#include <Python.h>

TEST(PyFormatter, CreateAndGet)
{
    PyFormatter py_f;
    PyObject* py_obj = py_f.get();
    ASSERT_NE(py_obj, nullptr);
    Py_DECREF(py_obj);
}

TEST(PyFormatter, ArraySection)
{
    PyFormatter py_f;
    py_f.open_array_section("items");
    py_f.dump_int("item", 123);
    py_f.close_section();
    PyObject* py_obj = py_f.get();
    ASSERT_NE(py_obj, nullptr);
    Py_DECREF(py_obj);
}

TEST(PyFormatter, ObjectSection)
{
    PyFormatter py_f;
    py_f.open_object_section("temp");
    py_f.dump_string("a", "b,");
    py_f.dump_int("abc", 123);
    py_f.close_section();
    PyObject* py_obj = py_f.get();
    ASSERT_NE(py_obj, nullptr);
    Py_DECREF(py_obj);
}

TEST(PyFormatter, DumpNull)
{
    PyFormatter py_f;
    py_f.open_object_section("temp");
    py_f.dump_null("abc");
    py_f.close_section();
    PyObject* py_obj = py_f.get();
    ASSERT_NE(py_obj, nullptr);
    Py_DECREF(py_obj);

}

TEST(PyFormatter, DumpUnsigned)
{
    PyFormatter py_f;
    py_f.open_object_section("temp");
    py_f.dump_unsigned("testUInt", 123u);
    py_f.close_section();

    PyObject* py_obj = py_f.get();
    ASSERT_NE(py_obj, nullptr);
    ASSERT_TRUE(PyDict_Check(py_obj));

    PyObject* temp = PyDict_GetItemString(py_obj, "temp");
    ASSERT_TRUE(temp != nullptr);
    
    PyObject* py_value = PyDict_GetItemString(temp, "testUInt");
    ASSERT_TRUE(py_value != nullptr);
    ASSERT_TRUE(PyLong_Check(py_value));
    ASSERT_EQ(PyLong_AsLong(py_value), 123);

    Py_DECREF(py_obj);
}

TEST(PyFormatter, DumpInt)
{
    PyFormatter py_f;
    py_f.open_object_section("temp");
    py_f.dump_int("testInt", -123);
    py_f.close_section();

    PyObject* py_obj = py_f.get();
    ASSERT_NE(py_obj, nullptr);
    ASSERT_TRUE(PyDict_Check(py_obj));

    PyObject* temp = PyDict_GetItemString(py_obj, "temp");
    ASSERT_TRUE(temp != nullptr);

    PyObject* py_value = PyDict_GetItemString(temp, "testInt");
    ASSERT_TRUE(py_value != nullptr);
    ASSERT_TRUE(PyLong_Check(py_value));
    ASSERT_EQ(PyLong_AsLong(py_value), -123);

    Py_DECREF(py_obj);
}

TEST(PyFormatter, DumpFloat)
{
    PyFormatter py_f;
    py_f.open_object_section("temp");
    py_f.dump_float("testFloat", 1.23);
    py_f.close_section();

    PyObject* py_obj = py_f.get();
    ASSERT_NE(py_obj, nullptr);
    ASSERT_TRUE(PyDict_Check(py_obj));

    PyObject* temp = PyDict_GetItemString(py_obj, "temp");
    ASSERT_TRUE(temp != nullptr);
    
    PyObject* py_value = PyDict_GetItemString(temp, "testFloat");
    ASSERT_TRUE(py_value != nullptr);
    ASSERT_TRUE(PyFloat_Check(py_value));
    ASSERT_DOUBLE_EQ(PyFloat_AsDouble(py_value), 1.23);

    Py_DECREF(py_obj);
}

TEST(PyFormatter, DumpString)
{
    PyFormatter py_f;
    py_f.open_object_section("temp");
    py_f.dump_string("testStr", "testing");
    py_f.close_section();
    
    PyObject* py_obj = py_f.get();
    ASSERT_NE(py_obj, nullptr);
    ASSERT_TRUE(PyDict_Check(py_obj));

    PyObject* temp = PyDict_GetItemString(py_obj, "temp");
    ASSERT_TRUE(temp != nullptr);
    
    PyObject* py_value = PyDict_GetItemString(temp, "testStr");
    ASSERT_TRUE(py_value != nullptr);
    ASSERT_TRUE(PyUnicode_Check(py_value));
    ASSERT_STREQ(PyUnicode_AsUTF8(py_value), "testing");

    Py_DECREF(py_obj);
}

TEST(PyFormatter, DumpBool)
{
    PyFormatter py_f;
    py_f.open_object_section("temp");
    py_f.dump_bool("testFlag", true);
    py_f.close_section();
    
    PyObject* py_obj = py_f.get();
    ASSERT_NE(py_obj, nullptr);
    ASSERT_TRUE(PyDict_Check(py_obj));

    PyObject* temp = PyDict_GetItemString(py_obj, "temp");
    ASSERT_TRUE(temp != nullptr);
    
    PyObject* py_value = PyDict_GetItemString(temp, "testFlag");
    ASSERT_TRUE(py_value != nullptr);
    ASSERT_TRUE(PyBool_Check(py_value));
    ASSERT_EQ(py_value, Py_True);

    Py_DECREF(py_obj);
}

TEST(PyFormatter, DumpStream)
{
    PyFormatter py_f;
    std::ostringstream oss;
    oss << "test stream value";
    py_f.open_object_section("temp");
    py_f.dump_stream(oss.str());
    py_f.close_section();
    PyObject* py_obj = py_f.get();
    ASSERT_NE(py_obj, nullptr);
    Py_DECREF(py_obj);
}

TEST(PyFormatter, DumpFormatVa)
{
    auto dump_format_va_helper = [](PyFormatter* py_formatter, std::string_view name, const char* ns, bool quoted, const char* py_formatstr, ...)
    {
        va_list args;
        va_start(args, py_formatstr);
        py_formatter->dump_format_va(name, ns, quoted, py_formatstr, args);
        va_end(args);
    };
    PyFormatter py_f;
    py_f.open_object_section("temp");
    dump_format_va_helper(&py_f, "testVa", nullptr, false, "testing %s %d", "abc", 123);
    py_f.close_section();

    PyObject* py_obj = py_f.get();
    ASSERT_NE(py_obj, nullptr);
    ASSERT_TRUE(PyDict_Check(py_obj));

    PyObject* temp = PyDict_GetItemString(py_obj, "temp");
    ASSERT_TRUE(temp != nullptr);
    
    PyObject* py_value = PyDict_GetItemString(temp, "testVa");
    ASSERT_TRUE(py_value != nullptr);
    ASSERT_TRUE(PyUnicode_Check(py_value));
    ASSERT_STREQ(PyUnicode_AsUTF8(py_value), "testing abc 123");

    Py_DECREF(py_obj);
}

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  ::testing::AddGlobalTestEnvironment(new PythonEnv);

  return RUN_ALL_TESTS();
}
