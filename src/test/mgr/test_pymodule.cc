#include "mgr/PyModule.h"
#include "TestMgr.h"
#include "gtest/gtest.h"
#include <Python.h>

TEST(PyModule, HandlePyError_NoError)
{
    PyErr_Clear();
    std::string error_msg = handle_pyerror(true, "test", "noErrorTest");
    ASSERT_TRUE(error_msg.empty() || error_msg.find("No Python Error") != std::string::npos);
}


int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  ::testing::AddGlobalTestEnvironment(new PythonEnv);

  return RUN_ALL_TESTS();
}
