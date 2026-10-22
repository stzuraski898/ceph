// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
//
// Ordering / invariant tests for DaemonStateIndex and DaemonState.
//
// These tests replay fixed and randomized sequences of index operations
// (insert, rm, update_metadata, cull, cull_services, with_device_create)
// and assert the internal-consistency INVARIANTS that must hold regardless
// of the order of operations.  They assert the invariant, not the current
// behaviour: a test that fails here is exposing a real defect.
//
// Covers hypotheses H7 (get_all snapshot vs mutation) and H8 (index
// internal consistency).  H1-H6 and H9 target DaemonServer, which cannot be
// constructed in a unit test (see ORDERING-MODEL.md / pilot report); the
// daemon_state sub-invariants those hypotheses depend on are tested here at
// the DaemonStateIndex level where they actually live.

#include <atomic>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "global/global_context.h"
#include "global/global_init.h"
#include "gtest/gtest.h"

#include "mgr/DaemonKey.h"
#include "mgr/DaemonState.h"

namespace {

// DaemonKey only defines operator<; derive equality from it.
bool key_eq(const DaemonKey& a, const DaemonKey& b)
{
  return !(a < b) && !(b < a);
}

std::string keystr(const DaemonKey& k)
{
  std::ostringstream oss;
  oss << k;
  return oss.str();
}

// Collect every internal-consistency violation of the index, using only the
// public accessors.  Empty result == consistent.
std::vector<std::string> find_violations(DaemonStateIndex& index)
{
  std::vector<std::string> v;

  const DaemonStateCollection all = index.get_all();

  // Snapshot by_server through the public copy-under-lock accessor.
  std::map<std::string, DaemonStateCollection> by_server;
  index.with_daemons_by_server(
    [&](const std::map<std::string, DaemonStateCollection>& m) {
      by_server = m;
    });

  // INV-9: every entry in the all-snapshot is self-consistent.
  for (const auto& [k, ptr] : all) {
    if (!ptr) {
      v.push_back("all[" + keystr(k) + "] is null");
      continue;
    }
    if (!key_eq(ptr->key, k)) {
      v.push_back("all[" + keystr(k) + "] holds a daemon whose key is " +
                  keystr(ptr->key));
    }
    // INV-2: reachable via by_server[hostname].
    auto hit = by_server.find(ptr->hostname);
    if (hit == by_server.end() || hit->second.count(k) == 0) {
      v.push_back("daemon " + keystr(k) + " is in all but not in by_server['" +
                  ptr->hostname + "']");
    }
  }

  // INV-1 / INV-3 / INV-4: by_server buckets.
  for (const auto& [host, coll] : by_server) {
    if (coll.empty()) {
      v.push_back("by_server['" + host + "'] is an empty bucket");
    }
    if (host.empty() && !coll.empty()) {
      v.push_back("daemon(s) registered under an empty hostname (by_server[''])");
    }
    for (const auto& [k, ptr] : coll) {
      if (!ptr) {
        v.push_back("by_server['" + host + "'][" + keystr(k) + "] is null");
        continue;
      }
      if (all.count(k) == 0) {
        v.push_back("daemon " + keystr(k) + " is in by_server but not in all");
      }
      if (ptr->hostname != host) {
        v.push_back("daemon " + keystr(k) + " filed under host '" + host +
                    "' but its hostname is '" + ptr->hostname + "'");
      }
    }
  }

  // INV-5 / INV-6 / INV-7 / INV-8: device index.
  index.with_devices([&](const DeviceState& d) {
    if (d.empty()) {
      v.push_back("device '" + d.devid +
                  "' is empty() but still present (not pruned)");
    }
    for (const auto& dk : d.daemons) {
      auto it = all.find(dk);
      if (it == all.end()) {
        v.push_back("device '" + d.devid + "' references daemon " + keystr(dk) +
                    " which is not in all");
        continue;
      }
      DaemonStatePtr dsp = it->second;
      std::lock_guard l(dsp->lock);
      auto dn = dsp->devices.find(d.devid);
      if (dn == dsp->devices.end()) {
        v.push_back("device '" + d.devid + "' lists daemon " + keystr(dk) +
                    " but that daemon does not reference the device");
        continue;
      }
      std::string path;
      if (auto pit = dsp->devices_bypath.find(d.devid);
          pit != dsp->devices_bypath.end()) {
        path = pit->second;
      }
      auto tup = std::make_tuple(dsp->hostname, dn->second, path);
      if (d.attachments.count(tup) == 0) {
        v.push_back("device '" + d.devid + "' references daemon " + keystr(dk) +
                    " but is missing its attachment tuple (" + dsp->hostname +
                    ", " + dn->second + ", " + path + "); attachments now hold " +
                    std::to_string(d.attachments.size()) + " tuple(s)");
      }
    }
  });

  return v;
}

std::string join(const std::vector<std::string>& v)
{
  std::string s;
  for (const auto& e : v) {
    s += "\n    - " + e;
  }
  return s;
}

} // namespace

class IndexOrderingTest : public ::testing::Test {
protected:
  DaemonStateIndex index;

  // Build a DaemonState via the real metadata-parse path (set_metadata), so
  // the device/hostname derivation matches production exactly.
  DaemonStatePtr make_daemon(const DaemonKey& k,
                             const std::string& host,
                             const std::string& device_ids = std::string(),
                             bool service_daemon = false)
  {
    auto s = std::make_shared<DaemonState>(index.types);
    s->key = k;
    std::map<std::string, std::string> m;
    if (!host.empty()) {
      m["hostname"] = host;
    }
    if (!device_ids.empty()) {
      m["device_ids"] = device_ids;
    }
    s->set_metadata(m);
    s->service_daemon = service_daemon;
    return s;
  }

  std::string attachments_of(const std::string& devid)
  {
    std::ostringstream oss;
    oss << "{";
    index.with_device(devid, [&](const DeviceState& d) {
      bool first = true;
      for (const auto& [h, dn, p] : d.attachments) {
        if (!first) oss << ", ";
        first = false;
        oss << "(" << h << "," << dn << "," << p << ")";
      }
    });
    oss << "}";
    return oss.str();
  }
};

// ---------------------------------------------------------------------------
// H8 (INV-6): shared-device attachment loss.  NEW finding.
//
// Two OSDs on one host with block.db on the same shared NVMe device both
// report the same (devname=devid) mapping, so _insert collapses their
// attachment tuples into one set element.  _erase of the first OSD removes
// that single shared tuple even though the second OSD still references the
// device.
// ---------------------------------------------------------------------------
TEST_F(IndexOrderingTest, OrderingBug_H8_SharedDevice_EraseDropsLiveAttachment)
{
  const std::string seq =
    "insert(osd.0, host=ceph-1, device_ids='nvme0n1=DEV-SHARED'); "
    "insert(osd.1, host=ceph-1, device_ids='nvme0n1=DEV-SHARED'); "
    "rm(osd.0)";

  DaemonKey k0{"osd", "0"};
  DaemonKey k1{"osd", "1"};

  index.insert(make_daemon(k0, "ceph-1", "nvme0n1=DEV-SHARED"));
  index.insert(make_daemon(k1, "ceph-1", "nvme0n1=DEV-SHARED"));

  // Both OSDs are registered against the shared device.
  bool found = false;
  std::set<DaemonKey> daemons_before;
  index.with_device("DEV-SHARED", [&](const DeviceState& d) {
    found = true;
    daemons_before = d.daemons;
  });
  ASSERT_TRUE(found) << "Sequence: " << seq
                     << "\n  Current: device 'DEV-SHARED' absent"
                     << "\n  Expected: device created referencing osd.0 and osd.1";
  ASSERT_EQ(daemons_before.size(), 2u)
      << "Sequence: " << seq
      << "\n  Current: device 'DEV-SHARED' referenced by " << daemons_before.size()
      << " daemon(s)"
      << "\n  Expected: referenced by 2 daemons (osd.0, osd.1)";

  // Remove the first OSD.  osd.1 still references DEV-SHARED.
  index.rm(k0);

  auto violations = find_violations(index);
  EXPECT_TRUE(violations.empty())
      << "Sequence: " << seq
      << "\n  Current state: device 'DEV-SHARED' attachments = "
      << attachments_of("DEV-SHARED")
      << "\n  Expected state: osd.1 still references DEV-SHARED, so its"
         " attachment tuple (ceph-1, nvme0n1, ) must still be present"
      << "\n  Invariant violations:" << join(violations);
}

// ---------------------------------------------------------------------------
// H8 (INV-4): empty hostname contaminates by_server.  KNOWN (UNGROUNDED).
// ---------------------------------------------------------------------------
TEST_F(IndexOrderingTest, OrderingBug_H8a_NoMetadata_EmptyHostnameBucket)
{
  const std::string seq = "insert(osd.5, host=<none>)";
  DaemonKey k{"osd", "5"};
  index.insert(make_daemon(k, /*host=*/std::string()));

  bool under_empty = false;
  index.with_daemons_by_server(
    [&](const std::map<std::string, DaemonStateCollection>& bs) {
      if (auto it = bs.find(std::string()); it != bs.end()) {
        under_empty = it->second.count(k) > 0;
      }
    });

  EXPECT_FALSE(under_empty)
      << "Sequence: " << seq
      << "\n  Current: osd.5 is filed in by_server[''] (empty-hostname bucket)"
      << "\n  Expected: a daemon with no hostname metadata must not be indexed"
         " under an empty hostname (would collide with every other"
         " metadata-less daemon)";
}

// ---------------------------------------------------------------------------
// H8 (INV-8): with_device_create leaves an empty device un-pruned.
// KNOWN (UNGROUNDED).
// ---------------------------------------------------------------------------
TEST_F(IndexOrderingTest, OrderingBug_H8b_WithDeviceCreate_EmptyDeviceLeak)
{
  const std::string seq = "with_device_create('ghost-dev', <no-op callback>)";
  index.with_device_create("ghost-dev", [](DeviceState&) { /* no-op */ });

  bool present = false;
  bool empty = false;
  index.with_device("ghost-dev", [&](const DeviceState& d) {
    present = true;
    empty = d.empty();
  });

  EXPECT_FALSE(present && empty)
      << "Sequence: " << seq
      << "\n  Current: device 'ghost-dev' exists with empty()==true and is"
         " never pruned"
      << "\n  Expected: a device left empty() by the create-callback must be"
         " pruned (as with_device_write does), or not have been created";
}

// ---------------------------------------------------------------------------
// H8 (INV-6 regression direction): non-shared devices must survive erase.
// This is the control: a device with distinct attachment tuples per daemon
// must keep the surviving daemon's tuple.
// ---------------------------------------------------------------------------
TEST_F(IndexOrderingTest, OrderingBug_H8_DistinctAttachments_SurviveErase)
{
  const std::string seq =
    "insert(osd.0, host=ceph-1, 'sda=DEV-A'); "
    "insert(osd.1, host=ceph-2, 'sda=DEV-A'); rm(osd.0)";
  index.insert(make_daemon({"osd", "0"}, "ceph-1", "sda=DEV-A"));
  index.insert(make_daemon({"osd", "1"}, "ceph-2", "sda=DEV-A"));
  index.rm({"osd", "0"});

  auto violations = find_violations(index);
  EXPECT_TRUE(violations.empty())
      << "Sequence: " << seq
      << "\n  Current: device 'DEV-A' attachments = " << attachments_of("DEV-A")
      << "\n  Expected: osd.1 on ceph-2 keeps its (ceph-2, sda, ) attachment"
      << "\n  Invariant violations:" << join(violations);
}

// ---------------------------------------------------------------------------
// H3/H10 kernel (INV-10): after rm, the index does not silently recreate the
// daemon; a stale read returns null.  This is the DaemonStateIndex-level core
// of the "no resurrection after close" hypotheses (H2/H3), which cannot be
// driven end-to-end through DaemonServer.
// ---------------------------------------------------------------------------
TEST_F(IndexOrderingTest, OrderingBug_H3_RmThenGet_NoResurrection)
{
  const std::string seq = "insert(mds.a, host=ceph-1); rm(mds.a); get(mds.a)";
  DaemonKey k{"mds", "a"};
  index.insert(make_daemon(k, "ceph-1"));
  index.rm(k);

  EXPECT_EQ(index.get(k), nullptr)
      << "Sequence: " << seq
      << "\n  Current: get(mds.a) returned a non-null daemon after rm"
      << "\n  Expected: get(mds.a) == nullptr (removed daemon stays removed)";
  EXPECT_FALSE(index.exists(k))
      << "Sequence: " << seq
      << "\n  Current: exists(mds.a) true after rm"
      << "\n  Expected: exists(mds.a) false";
}

// ---------------------------------------------------------------------------
// H8 randomized: long random walk over the public mutators, checking ALL
// invariants after every step.  Prints the seed and the operation trace on
// the first failure.
// ---------------------------------------------------------------------------
TEST_F(IndexOrderingTest, OrderingBug_H8_RandomizedSequence_AllInvariants)
{
  const unsigned seed = 0x5eed0176u; // fixed seed, printed below
  std::mt19937 rng(seed);

  // Small key space (2-3 keys), 2 hosts, shared + distinct devices.
  const std::vector<DaemonKey> keys = {
    {"osd", "0"}, {"osd", "1"}, {"mds", "a"}};
  const std::vector<std::string> hosts = {"ceph-1", "ceph-2"};
  // device_ids strings; "nvme0n1=DEV-X" shared devname+id triggers collisions.
  const std::vector<std::string> devs = {
    "", "nvme0n1=DEV-X", "nvme0n1=DEV-Y", "sdb=DEV-X"};

  std::vector<std::string> trace;
  auto log = [&](const std::string& s) { trace.push_back(s); };

  const int kSteps = 100000;
  for (int step = 0; step < kSteps; ++step) {
    int op = rng() % 6;
    const DaemonKey& k = keys[rng() % keys.size()];
    const std::string& host = hosts[rng() % hosts.size()]; // never empty
    const std::string& dev = devs[rng() % devs.size()];

    switch (op) {
      case 0:
      case 1: { // insert (weighted)
        log("insert(" + keystr(k) + ", host=" + host + ", dev='" + dev + "')");
        index.insert(make_daemon(k, host, dev, /*service_daemon=*/false));
        break;
      }
      case 2: { // rm
        log("rm(" + keystr(k) + ")");
        index.rm(k);
        break;
      }
      case 3: { // update_metadata on an existing daemon
        if (auto s = index.get(k); s) {
          std::map<std::string, std::string> m{{"hostname", host}};
          if (!dev.empty()) m["device_ids"] = dev;
          log("update_metadata(" + keystr(k) + ", host=" + host + ", dev='" +
              dev + "')");
          index.update_metadata(s, m);
        }
        break;
      }
      case 4: { // cull osd by a random surviving-name subset
        std::set<std::string> names;
        if (rng() & 1) names.insert("0");
        if (rng() & 1) names.insert("1");
        log("cull('osd', {names})");
        index.cull("osd", names);
        break;
      }
      case 5: { // with_device_create modelling a persistent-metadata caller
        const std::string& d = (rng() & 1) ? std::string("DEV-X")
                                            : std::string("DEV-Y");
        log("with_device_create('" + d + "', set persistent metadata)");
        index.with_device_create(d, [](DeviceState& ds) {
          // real callers that create a device do so to attach persistent
          // metadata, so the device is legitimately non-empty afterwards.
          ds.metadata["persist"] = "1";
        });
        break;
      }
    }

    auto violations = find_violations(index);
    if (!violations.empty()) {
      // Print the minimal failing tail of the sequence.
      const size_t tail = std::min<size_t>(trace.size(), 14);
      std::ostringstream oss;
      for (size_t i = trace.size() - tail; i < trace.size(); ++i) {
        oss << "\n    [" << i << "] " << trace[i];
      }
      FAIL() << "Randomized invariant violation (H8)."
             << "\n  Seed: 0x" << std::hex << seed << std::dec
             << "\n  Failed at step: " << step
             << "\n  Operation trace (last " << tail << " ops):" << oss.str()
             << "\n  Current invariant violations:" << join(violations)
             << "\n  Expected: index internally consistent after every step";
      return;
    }
  }
  SUCCEED() << "No invariant violation across " << kSteps
            << " random operations (seed 0x" << std::hex << seed << ")";
}

// ---------------------------------------------------------------------------
// H7 (INV-9): get_all() copies `all` without taking the index lock, so it can
// race with a concurrent insert/rm mutating the same std::map.  Real threads,
// many iterations.  The reliable detector is ThreadSanitizer; without it the
// test may observe a torn snapshot or crash.
// ---------------------------------------------------------------------------
TEST_F(IndexOrderingTest, OrderingBug_H7_GetAll_ConcurrentMutation)
{
  std::atomic<bool> stop{false};
  std::atomic<int> inconsistencies{0};
  std::atomic<long> reads{0};

  // Writer: churn the index membership.
  std::thread writer([&] {
    int i = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      DaemonKey k{"osd", std::to_string(i % 64)};
      if (i & 1) {
        index.insert(make_daemon(k, "ceph-1", "sd" + std::to_string(i % 8) +
                                                   "=DEV-" + std::to_string(i % 8)));
      } else {
        index.rm(k);
      }
      ++i;
    }
  });

  // Reader: take unlocked snapshots and check self-consistency.
  const int kIters = 20000;
  for (int it = 0; it < kIters; ++it) {
    const DaemonStateCollection snap = index.get_all();
    for (const auto& [k, ptr] : snap) {
      if (!ptr || !key_eq(ptr->key, k)) {
        inconsistencies.fetch_add(1, std::memory_order_relaxed);
      }
    }
    reads.fetch_add(1, std::memory_order_relaxed);
  }

  stop.store(true, std::memory_order_relaxed);
  writer.join();

  EXPECT_EQ(inconsistencies.load(), 0)
      << "Sequence: concurrent get_all() vs insert/rm over " << reads.load()
      << " snapshots"
      << "\n  Current: get_all() returned a torn snapshot "
      << inconsistencies.load() << " time(s) because it copies `all` without"
         " holding the index lock (DaemonState.h: get_all())"
      << "\n  Expected: every get_all() snapshot is internally consistent"
         " (ptr != null and ptr->key == key)"
      << "\n  Note: ThreadSanitizer flags the data race deterministically even"
         " when a plain build does not observe corruption.";
}

// ---------------------------------------------------------------------------
// Concurrency control (shared structures by_server / devices / updating):
// all index maps are guarded by one shared_mutex, so the *locked* accessors
// must stay crash-free and self-consistent under multi-threaded mutation.
// This complements H7, which isolates the single unlocked accessor (get_all).
// Expected to PASS (the mutex is correct); a failure here would mean a lock
// gap beyond get_all.
// ---------------------------------------------------------------------------
TEST_F(IndexOrderingTest, OrderingBug_H7b_ConcurrentLockedAccess_NoCorruption)
{
  std::atomic<bool> stop{false};
  std::atomic<int> corruptions{0};

  auto mutator = [&](int base) {
    std::mt19937 rng(base);
    int i = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      DaemonKey k{"osd", std::to_string((base * 7 + i) % 16)};
      switch (rng() % 4) {
        case 0:
          index.insert(make_daemon(k, "ceph-1", "sda=DEV-" +
                                                     std::to_string(i % 4)));
          break;
        case 1:
          index.rm(k);
          break;
        case 2:
          if (auto s = index.get(k); s) {
            index.update_metadata(
                s, {{"hostname", "ceph-2"},
                    {"device_ids", "sda=DEV-" + std::to_string(i % 4)}});
          }
          break;
        case 3:
          index.cull("osd", {"0", "1", "2", "3"});
          break;
      }
      ++i;
    }
  };

  std::thread m1(mutator, 1);
  std::thread m2(mutator, 2);

  // Reader uses only lock-taking accessors.
  for (int it = 0; it < 20000; ++it) {
    auto by_svc = index.get_by_service("osd");
    for (const auto& [k, ptr] : by_svc) {
      if (!ptr || !key_eq(ptr->key, k)) {
        corruptions.fetch_add(1, std::memory_order_relaxed);
      }
    }
    auto by_srv = index.get_by_server("ceph-1");
    for (const auto& [k, ptr] : by_srv) {
      if (!ptr) corruptions.fetch_add(1, std::memory_order_relaxed);
    }
    index.with_devices([&](const DeviceState& d) {
      if (d.devid.empty()) corruptions.fetch_add(1, std::memory_order_relaxed);
    });
  }

  stop.store(true, std::memory_order_relaxed);
  m1.join();
  m2.join();

  EXPECT_EQ(corruptions.load(), 0)
      << "Sequence: 2 concurrent mutators (insert/rm/update_metadata/cull) vs a"
         " reader using the locked accessors get_by_service/get_by_server/"
         "with_devices"
      << "\n  Current: a locked accessor returned corrupt data "
      << corruptions.load() << " time(s)"
      << "\n  Expected: locked accessors are always consistent (the shared"
         " mutex protects all/by_server/devices together)";
}

int main(int argc, char** argv)
{
  ::testing::InitGoogleTest(&argc, argv);

  std::vector<const char*> args(argv, argv + argc);
  auto cct = global_init(nullptr, args, CEPH_ENTITY_TYPE_CLIENT,
                         CODE_ENVIRONMENT_UTILITY,
                         CINIT_FLAG_NO_DEFAULT_CONFIG_FILE);
  common_init_finish(g_ceph_context);

  return RUN_ALL_TESTS();
}
