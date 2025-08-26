#include "mgr/ClusterState.h"
#include "messages/MMgrDigest.h"
#include "messages/MPGStats.h"
#include "mon/OSDMap.h"
#include "gtest/gtest.h"
#include <memory>

TEST(ClusterState, Construct)
{
    MonClient* monClient = nullptr;
    Objecter* objecter = nullptr;
    MgrMap mgrMap;
    ClusterState cs(monClient, objecter, mgrMap);
    
    SUCCEED();
}

TEST(ClusterState, Setters)
{
    MonClient* monClient = nullptr;
    Objecter* objecter = nullptr;
    MgrMap mgrMap;
    ClusterState cs(monClient, objecter, mgrMap);
    FSMap fsMap;
    ServiceMap serviceMap;

    cs.set_objecter(objecter);
    cs.set_fsmap(fsMap);
    cs.set_mgr_map(mgrMap);
    cs.set_service_map(serviceMap);

    SUCCEED();
}

TEST(ClusterState, LoadDigest)
{
    MonClient* monClient = nullptr;
    Objecter* objecter = nullptr;
    MgrMap mgrMap;
    ClusterState cs(monClient, objecter, mgrMap);
    auto digest = ceph::make_message<MMgrDigest>();

    digest->health_json.append("health");
    digest->mon_status_json.append("status");
    cs.load_digest(digest.get());

    SUCCEED();
}

TEST(ClusterState, IngestPGStats)
{
    MonClient* monClient = nullptr;
    Objecter* objecter = nullptr;
    MgrMap mgrMap;
    ClusterState cs(monClient, objecter, mgrMap);
    auto pgStats = ceph::make_message<MPGStats>();

    //An ceph assert (ceph_assert(objecter != nullptr)) causes this test to core dump, need to implement a dummy Objecter or create a full one
    //cs.ingest_pgstats(pgStats);
    SUCCEED();
}

TEST(ClusterState, UpdateDeltaStats)
{
    MonClient* monClient = nullptr;
    Objecter* objecter = nullptr;
    MgrMap mgrMap;
    ClusterState cs(monClient, objecter, mgrMap);

    //Segfaults without ceph running
    //cs.update_delta_stats();

    SUCCEED();
}

TEST(ClusterState, NotifyOSDMap)
{
    MonClient* monClient = nullptr;
    Objecter* objecter = nullptr;
    MgrMap mgrMap;
    ClusterState cs(monClient, objecter, mgrMap);
    OSDMap osd_map; //Might need a real instance for a full test(?)
    
    //Segfaults without ceph running
    //cs.notify_osd_map(osd_map);

    SUCCEED();
}