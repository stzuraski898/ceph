// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "TestMgr.h"
#include "mgr/PyOSDMap.h"
#include "osd/OSDMap.h"

// Test that OSDMap is initialized with epoch 1
TEST_F(PyOSDMapTestHelper, OSDMapInitialized) {
  ASSERT_EQ(osd_map.get_epoch(), 1);
}

// Test OSDMap epoch getter
TEST_F(PyOSDMapTestHelper, GetEpoch) {
  EXPECT_EQ(osd_map.get_epoch(), 1);
  EXPECT_GT(osd_map.get_epoch(), 0);
}

// Test OSDMap max OSD count
TEST_F(PyOSDMapTestHelper, GetMaxOSD) {
  // Initially should be 0 OSDs
  EXPECT_EQ(osd_map.get_max_osd(), 0);
}

// Test OSDMap pool count
TEST_F(PyOSDMapTestHelper, GetNumPools) {
  // Initially should have no pools
  EXPECT_EQ(osd_map.get_pools().size(), 0);
}

// Test OSDMap flags
TEST_F(PyOSDMapTestHelper, GetFlags) {
  // Check that we can get flags (should be 0 initially)
  EXPECT_EQ(osd_map.get_flags(), 0);
}

// Test OSDMap created time
TEST_F(PyOSDMapTestHelper, GetCreated) {
  // Created time should be accessible (may be 0 initially)
  auto created = osd_map.get_created();
  EXPECT_GE(created.sec(), 0);
}

// Test OSDMap modified time
TEST_F(PyOSDMapTestHelper, GetModified) {
  // Modified time should be accessible (may be 0 initially)
  auto modified = osd_map.get_modified();
  EXPECT_GE(modified.sec(), 0);
}

// Test OSDMap with incremental update
TEST_F(PyOSDMapTestHelper, ApplyIncremental) {
  OSDMap::Incremental inc(osd_map.get_epoch() + 1);
  
  // Apply the incremental
  osd_map.apply_incremental(inc);
  
  // Epoch should have increased
  EXPECT_EQ(osd_map.get_epoch(), 2);
}

// Test OSDMap pool operations
TEST_F(PyOSDMapTestHelper, PoolOperations) {
  OSDMap::Incremental inc(osd_map.get_epoch() + 1);
  
  // Add a pool
  pg_pool_t pool;
  pool.set_pg_num(8);
  pool.set_pgp_num(8);
  inc.new_pool_max = 1;
  inc.new_pools[1] = pool;
  
  osd_map.apply_incremental(inc);
  
  // Should now have 1 pool
  EXPECT_EQ(osd_map.get_pools().size(), 1);
  EXPECT_TRUE(osd_map.have_pg_pool(1));
}

// Test OSDMap OSD operations
TEST_F(PyOSDMapTestHelper, OSDOperations) {
  OSDMap::Incremental inc(osd_map.get_epoch() + 1);
  
  // Add an OSD
  inc.new_max_osd = 1;
  inc.new_state[0] = CEPH_OSD_EXISTS | CEPH_OSD_UP;
  inc.new_weight[0] = CEPH_OSD_IN;
  inc.new_up_client[0] = entity_addrvec_t();
  inc.new_up_cluster[0] = entity_addrvec_t();
  inc.new_hb_back_up[0] = entity_addrvec_t();
  inc.new_hb_front_up[0] = entity_addrvec_t();
  
  osd_map.apply_incremental(inc);
  
  // Should now have 1 OSD
  EXPECT_EQ(osd_map.get_max_osd(), 1);
  EXPECT_TRUE(osd_map.exists(0));
  EXPECT_TRUE(osd_map.is_up(0));
}

// Test OSDMap get_pools returns correct type
TEST_F(PyOSDMapTestHelper, GetPoolsType) {
  const auto& pools = osd_map.get_pools();
  EXPECT_TRUE(pools.empty());
}

// Test OSDMap FSMap ID
TEST_F(PyOSDMapTestHelper, GetFSMapEpoch) {
  // FSMap epoch should be 0 initially
  EXPECT_EQ(osd_map.get_fsid(), uuid_d());
}

// Test OSDMap cluster snapshot
TEST_F(PyOSDMapTestHelper, GetClusterSnapshot) {
  // Cluster snapshot should be empty string initially
  EXPECT_EQ(osd_map.get_cluster_snapshot(), "");
}

// Test OSDMap pool name operations
TEST_F(PyOSDMapTestHelper, PoolNameOperations) {
  OSDMap::Incremental inc(osd_map.get_epoch() + 1);
  
  // Add a pool with a name
  pg_pool_t pool;
  pool.set_pg_num(8);
  inc.new_pool_max = 1;
  inc.new_pools[1] = pool;
  inc.new_pool_names[1] = "test_pool";
  
  osd_map.apply_incremental(inc);
  
  // Check pool name
  EXPECT_EQ(osd_map.get_pool_name(1), "test_pool");
  EXPECT_TRUE(osd_map.have_pg_pool(1));
}

// Test OSDMap multiple pools
TEST_F(PyOSDMapTestHelper, MultiplePools) {
  OSDMap::Incremental inc(osd_map.get_epoch() + 1);
  
  // Add multiple pools
  pg_pool_t pool1, pool2;
  pool1.set_pg_num(8);
  pool2.set_pg_num(16);
  
  inc.new_pool_max = 2;
  inc.new_pools[1] = pool1;
  inc.new_pools[2] = pool2;
  inc.new_pool_names[1] = "pool1";
  inc.new_pool_names[2] = "pool2";
  
  osd_map.apply_incremental(inc);
  
  // Should have 2 pools
  EXPECT_EQ(osd_map.get_pools().size(), 2);
  EXPECT_TRUE(osd_map.have_pg_pool(1));
  EXPECT_TRUE(osd_map.have_pg_pool(2));
  EXPECT_EQ(osd_map.get_pool_name(1), "pool1");
  EXPECT_EQ(osd_map.get_pool_name(2), "pool2");
}

// Test OSDMap epoch progression
TEST_F(PyOSDMapTestHelper, EpochProgression) {
  EXPECT_EQ(osd_map.get_epoch(), 1);
  
  // Apply multiple incrementals
  for (int i = 0; i < 5; i++) {
    OSDMap::Incremental inc(osd_map.get_epoch() + 1);
    osd_map.apply_incremental(inc);
  }
  
  EXPECT_EQ(osd_map.get_epoch(), 6);
}

// Test OSDMap is_up and is_down
TEST_F(PyOSDMapTestHelper, OSDUpDown) {
  OSDMap::Incremental inc(osd_map.get_epoch() + 1);
  
  // Add an OSD that is up
  inc.new_max_osd = 1;
  inc.new_state[0] = CEPH_OSD_EXISTS | CEPH_OSD_UP;
  inc.new_weight[0] = CEPH_OSD_IN;
  inc.new_up_client[0] = entity_addrvec_t();
  inc.new_up_cluster[0] = entity_addrvec_t();
  inc.new_hb_back_up[0] = entity_addrvec_t();
  inc.new_hb_front_up[0] = entity_addrvec_t();
  
  osd_map.apply_incremental(inc);
  
  EXPECT_TRUE(osd_map.is_up(0));
  EXPECT_FALSE(osd_map.is_down(0));
}

// Test OSDMap get_num_up_osds
TEST_F(PyOSDMapTestHelper, GetNumUpOSDs) {
  // Initially no OSDs
  EXPECT_EQ(osd_map.get_num_up_osds(), 0);
  
  OSDMap::Incremental inc(osd_map.get_epoch() + 1);
  inc.new_max_osd = 1;
  inc.new_state[0] = CEPH_OSD_EXISTS | CEPH_OSD_UP;
  inc.new_weight[0] = CEPH_OSD_IN;
  inc.new_up_client[0] = entity_addrvec_t();
  inc.new_up_cluster[0] = entity_addrvec_t();
  inc.new_hb_back_up[0] = entity_addrvec_t();
  inc.new_hb_front_up[0] = entity_addrvec_t();
  
  osd_map.apply_incremental(inc);
  
  EXPECT_EQ(osd_map.get_num_up_osds(), 1);
}

// Test OSDMap get_num_in_osds
TEST_F(PyOSDMapTestHelper, GetNumInOSDs) {
  // Initially no OSDs
  EXPECT_EQ(osd_map.get_num_in_osds(), 0);
  
  OSDMap::Incremental inc(osd_map.get_epoch() + 1);
  inc.new_max_osd = 1;
  inc.new_state[0] = CEPH_OSD_EXISTS | CEPH_OSD_UP;
  inc.new_weight[0] = CEPH_OSD_IN;
  inc.new_up_client[0] = entity_addrvec_t();
  inc.new_up_cluster[0] = entity_addrvec_t();
  inc.new_hb_back_up[0] = entity_addrvec_t();
  inc.new_hb_front_up[0] = entity_addrvec_t();
  
  osd_map.apply_incremental(inc);
  
  EXPECT_EQ(osd_map.get_num_in_osds(), 1);
}

// Made with Bob
