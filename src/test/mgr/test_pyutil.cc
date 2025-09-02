#include "mgr/PyUtil.h"
#include "TestMgr.h"
#include "gtest/gtest.h"
#include <Python.h>
#include "common/options.h"

TEST(PyUtil, IntType)
{
    PyObject* intObj = get_python_typed_option_value(Option::TYPE_INT, "11");
    ASSERT_TRUE(PyLong_Check(intObj));
    EXPECT_EQ(PyLong_AsLong(intObj), 11);
    Py_DECREF(intObj);
}

TEST(PyUtil, FloatType)
{
    PyObject* floatObj = get_python_typed_option_value(Option::TYPE_FLOAT, "11.22");
    ASSERT_TRUE(PyFloat_Check(floatObj));
    EXPECT_DOUBLE_EQ(PyFloat_AsDouble(floatObj), 11.22);
    Py_DECREF(floatObj);
}

TEST(PyUtil, BoolType)
{
    PyObject* boolObjTrue = get_python_typed_option_value(Option::TYPE_BOOL, "true");
    ASSERT_EQ(boolObjTrue, Py_True);
    Py_DECREF(boolObjTrue);
    PyObject* boolObjFalse = get_python_typed_option_value(Option::TYPE_BOOL, "badinput");
    ASSERT_EQ(boolObjFalse, Py_False);
    Py_DECREF(boolObjFalse);
}

TEST(PyUtil, StringType)
{
    PyObject* stringObj = get_python_typed_option_value(Option::TYPE_STR, "test");
    ASSERT_TRUE(PyUnicode_Check(stringObj));
    EXPECT_STREQ(PyUnicode_AsUTF8(stringObj), "test");
    Py_DECREF(stringObj);
}

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  ::testing::AddGlobalTestEnvironment(new PythonEnv);

  return RUN_ALL_TESTS();
}
