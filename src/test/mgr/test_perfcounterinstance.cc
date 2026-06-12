// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "TestMgr.h"
#include "mgr/PerfCounterInstance.h"
#include "common/perf_counters.h"

// Test basic construction and buffer initialization
TEST_F(PerfCounterInstanceTestHelper, ConstructorNormalCounter) {
  PerfCounterInstance counter(PERFCOUNTER_U64);
  
  // Normal counter should have empty regular buffer
  ASSERT_EQ(counter.get_data().size(), 0);
}

TEST_F(PerfCounterInstanceTestHelper, ConstructorAvgCounter) {
  PerfCounterInstance counter(PERFCOUNTER_LONGRUNAVG);
  
  // Avg counter should have empty avg buffer
  ASSERT_EQ(counter.get_data_avg().size(), 0);
}

// Test push operation for normal counters
TEST_F(PerfCounterInstanceTestHelper, PushSingleValue) {
  PerfCounterInstance counter(PERFCOUNTER_U64);
  utime_t t1(100, 0);
  uint64_t v1 = 42;
  
  counter.push(t1, v1);
  
  ASSERT_EQ(counter.get_data().size(), 1);
  ASSERT_EQ(counter.get_latest_data().v, v1);
  ASSERT_EQ(counter.get_latest_data().t, t1);
}

TEST_F(PerfCounterInstanceTestHelper, PushMultipleValues) {
  PerfCounterInstance counter(PERFCOUNTER_U64);
  
  for (int i = 0; i < 10; i++) {
    utime_t t(100 + i, 0);
    uint64_t v = i * 10;
    counter.push(t, v);
  }
  
  ASSERT_EQ(counter.get_data().size(), 10);
  ASSERT_EQ(counter.get_latest_data().v, 90);
  ASSERT_EQ(counter.get_latest_data().t.sec(), 109);
}

// Test circular buffer behavior (max 20 elements)
TEST_F(PerfCounterInstanceTestHelper, CircularBufferOverflow) {
  PerfCounterInstance counter(PERFCOUNTER_U64);
  
  // Push 25 values (more than buffer capacity of 20)
  for (int i = 0; i < 25; i++) {
    utime_t t(100 + i, 0);
    uint64_t v = i;
    counter.push(t, v);
  }
  
  // Buffer should contain only 20 most recent values
  ASSERT_EQ(counter.get_data().size(), 20);
  
  // Latest value should be the last one pushed
  ASSERT_EQ(counter.get_latest_data().v, 24);
  ASSERT_EQ(counter.get_latest_data().t.sec(), 124);
  
  // First value in buffer should be value 5 (values 0-4 were dropped)
  ASSERT_EQ(counter.get_data().front().v, 5);
  ASSERT_EQ(counter.get_data().front().t.sec(), 105);
}

// Test push_avg operation for average counters
TEST_F(PerfCounterInstanceTestHelper, PushAvgSingleValue) {
  PerfCounterInstance counter(PERFCOUNTER_LONGRUNAVG);
  utime_t t1(100, 0);
  uint64_t sum = 1000;
  uint64_t count = 10;
  
  counter.push_avg(t1, sum, count);
  
  ASSERT_EQ(counter.get_data_avg().size(), 1);
  ASSERT_EQ(counter.get_latest_data_avg().s, sum);
  ASSERT_EQ(counter.get_latest_data_avg().c, count);
  ASSERT_EQ(counter.get_latest_data_avg().t, t1);
}

TEST_F(PerfCounterInstanceTestHelper, PushAvgMultipleValues) {
  PerfCounterInstance counter(PERFCOUNTER_LONGRUNAVG);
  
  for (int i = 0; i < 10; i++) {
    utime_t t(100 + i, 0);
    uint64_t sum = i * 100;
    uint64_t count = i + 1;
    counter.push_avg(t, sum, count);
  }
  
  ASSERT_EQ(counter.get_data_avg().size(), 10);
  ASSERT_EQ(counter.get_latest_data_avg().s, 900);
  ASSERT_EQ(counter.get_latest_data_avg().c, 10);
  ASSERT_EQ(counter.get_latest_data_avg().t.sec(), 109);
}

// Test circular buffer behavior for avg counters
TEST_F(PerfCounterInstanceTestHelper, AvgCircularBufferOverflow) {
  PerfCounterInstance counter(PERFCOUNTER_LONGRUNAVG);
  
  // Push 25 values (more than buffer capacity of 20)
  for (int i = 0; i < 25; i++) {
    utime_t t(100 + i, 0);
    uint64_t sum = i * 10;
    uint64_t count = i + 1;
    counter.push_avg(t, sum, count);
  }
  
  // Buffer should contain only 20 most recent values
  ASSERT_EQ(counter.get_data_avg().size(), 20);
  
  // Latest value should be the last one pushed
  ASSERT_EQ(counter.get_latest_data_avg().s, 240);
  ASSERT_EQ(counter.get_latest_data_avg().c, 25);
  ASSERT_EQ(counter.get_latest_data_avg().t.sec(), 124);
  
  // First value in buffer should be value 5
  ASSERT_EQ(counter.get_data_avg().front().s, 50);
  ASSERT_EQ(counter.get_data_avg().front().c, 6);
  ASSERT_EQ(counter.get_data_avg().front().t.sec(), 105);
}

// Test data retrieval methods
TEST_F(PerfCounterInstanceTestHelper, GetDataReturnsCorrectBuffer) {
  PerfCounterInstance counter(PERFCOUNTER_U64);
  
  for (int i = 0; i < 5; i++) {
    utime_t t(100 + i, 0);
    counter.push(t, i * 10);
  }
  
  const auto& data = counter.get_data();
  ASSERT_EQ(data.size(), 5);
  
  // Verify all values are accessible
  int idx = 0;
  for (const auto& point : data) {
    ASSERT_EQ(point.v, idx * 10);
    ASSERT_EQ(point.t.sec(), 100 + idx);
    idx++;
  }
}

TEST_F(PerfCounterInstanceTestHelper, GetDataAvgReturnsCorrectBuffer) {
  PerfCounterInstance counter(PERFCOUNTER_LONGRUNAVG);
  
  for (int i = 0; i < 5; i++) {
    utime_t t(100 + i, 0);
    counter.push_avg(t, i * 100, i + 1);
  }
  
  const auto& data = counter.get_data_avg();
  ASSERT_EQ(data.size(), 5);
  
  // Verify all values are accessible
  int idx = 0;
  for (const auto& point : data) {
    ASSERT_EQ(point.s, idx * 100);
    ASSERT_EQ(point.c, idx + 1);
    ASSERT_EQ(point.t.sec(), 100 + idx);
    idx++;
  }
}

// Test with different perfcounter types
TEST_F(PerfCounterInstanceTestHelper, DifferentCounterTypes) {
  // Test with TIME counter
  PerfCounterInstance time_counter(PERFCOUNTER_TIME);
  utime_t t1(100, 0);
  time_counter.push(t1, 12345);
  ASSERT_EQ(time_counter.get_data().size(), 1);
  ASSERT_EQ(time_counter.get_latest_data().v, 12345);
  
  // Test with LONGRUNAVG | TIME
  PerfCounterInstance avg_time_counter(
      static_cast<perfcounter_type_d>(PERFCOUNTER_LONGRUNAVG | PERFCOUNTER_TIME));
  avg_time_counter.push_avg(t1, 5000, 10);
  ASSERT_EQ(avg_time_counter.get_data_avg().size(), 1);
  ASSERT_EQ(avg_time_counter.get_latest_data_avg().s, 5000);
  ASSERT_EQ(avg_time_counter.get_latest_data_avg().c, 10);
}

// Test edge cases
TEST_F(PerfCounterInstanceTestHelper, ZeroValues) {
  PerfCounterInstance counter(PERFCOUNTER_U64);
  utime_t t(0, 0);
  
  counter.push(t, 0);
  
  ASSERT_EQ(counter.get_data().size(), 1);
  ASSERT_EQ(counter.get_latest_data().v, 0);
  ASSERT_EQ(counter.get_latest_data().t.sec(), 0);
}

TEST_F(PerfCounterInstanceTestHelper, LargeValues) {
  PerfCounterInstance counter(PERFCOUNTER_U64);
  utime_t t(1000000, 999999);
  uint64_t large_val = UINT64_MAX;
  
  counter.push(t, large_val);
  
  ASSERT_EQ(counter.get_data().size(), 1);
  ASSERT_EQ(counter.get_latest_data().v, large_val);
}

TEST_F(PerfCounterInstanceTestHelper, AvgZeroCount) {
  PerfCounterInstance counter(PERFCOUNTER_LONGRUNAVG);
  utime_t t(100, 0);
  
  counter.push_avg(t, 1000, 0);
  
  ASSERT_EQ(counter.get_data_avg().size(), 1);
  ASSERT_EQ(counter.get_latest_data_avg().s, 1000);
  ASSERT_EQ(counter.get_latest_data_avg().c, 0);
}

// Test time ordering
TEST_F(PerfCounterInstanceTestHelper, TimeOrdering) {
  PerfCounterInstance counter(PERFCOUNTER_U64);
  
  // Push values with increasing timestamps
  for (int i = 0; i < 5; i++) {
    utime_t t(100 + i * 10, i * 1000);
    counter.push(t, i);
  }
  
  const auto& data = counter.get_data();
  ASSERT_EQ(data.size(), 5);
  
  // Verify timestamps are in order
  utime_t prev_t(0, 0);
  for (const auto& point : data) {
    ASSERT_TRUE(point.t > prev_t);
    prev_t = point.t;
  }
}

// Made with Bob
