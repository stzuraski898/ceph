#include <Python.h>
#include <gtest/gtest.h>

struct PythonEnv : public ::testing::Environment
{
    void SetUp()    override { Py_Initialize(); }
    void TearDown() override { Py_Finalize();   }
};