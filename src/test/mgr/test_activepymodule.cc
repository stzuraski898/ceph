#include "mgr/ActivePyModule.h"
#include "mgr/ActivePyModules.h"
#include "mgr/DaemonServer.h"
#include "mgr/DaemonState.h"
#include "mgr/ClusterState.h"
#include "mgr/PyModuleRegistry.h"
#include "common/Finisher.h"
#include "mon/MonClient.h"
#include "osdc/Objecter.h"
#include "gtest/gtest.h"

class DummyActivePyModules : public ActivePyModules
{
    public:
        DummyActivePyModules() : ActivePyModules(
            *(PyModuleConfig*)nullptr, {}, false,
            *(DaemonStateIndex*)nullptr, *(ClusterState*)nullptr, *(MonClient*)nullptr,
            LogChannelRef(), LogChannelRef(),
            *(Objecter*)nullptr, *(Finisher*)nullptr,
            *(DaemonServer*)nullptr, *(PyModuleRegistry*)nullptr) {}
};

TEST(ActivePyModule, Load)
{
    DummyActivePyModules dummy_modules;
    std::shared_ptr<PyModule> success_module, failing_module;
    LogChannelRef dummy_log_channel;
    int ret = -1;

    //Success case
    success_module = std::make_shared<PyModule>("successModule");
    ActivePyModule apm(success_module, dummy_log_channel);
    ret = apm.load(&dummy_modules);
    ASSERT_EQ(ret, 0);

    //Failure case
    failing_module = std::make_shared<PyModule>("failingModule");
    failing_module->pClass = nullptr;
    ActivePyModule apm_fail(failing_module, dummy_log_channel);
    ret = apm_fail.load(&dummy_modules);
    ASSERT_EQ(ret, -EINVAL);
}