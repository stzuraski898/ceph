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
    /*
    PyFormatter py_f;
    py_f.open_object_section("temp");
    //py_f.dump_format_va
    py_f.close_section();
    PyObject* py_obj = py_f.get();
    ASSERT_NE(py_obj, nullptr);
    Py_DECREF(py_obj);
    */
}

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  ::testing::AddGlobalTestEnvironment(new PythonEnv);

  return RUN_ALL_TESTS();
}
