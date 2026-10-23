// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "common/ceph_argparse.h"
#include "global/global_context.h"
#include "global/global_init.h"
#include "gtest/gtest.h"
#include "mgr/ThreadMonitor.h"

using namespace std::chrono_literals;

// PyModule.cc needs the embedded interpreter and cannot be linked into this
// test, so the out-of-line members ThreadMonitor depends on are defined here.
// perf_counter_build mirrors the production one minus the collection
// registration.
PyModule::PyModule(const std::string& module_name_)
  : module_name(module_name_)
{}

PyModule::~PyModule() {}

int PyModule::perf_counter_build(CephContext* cct)
{
  PerfCountersBuilder pcb(cct, "mgr_module_" + get_name(), l_pym_first,
                          l_pym_last);
  pcb.add_u64_avg(l_pym_notify_avg_usec, "notify_avg_usec", "", "nsec", 0);
  pcb.add_u64_avg(l_pym_cmd_avg_usec, "cmd_avg_usec", "", "csec", 0);
  pcb.add_u64(l_pym_alive, "alive", "", "aliv", 0, uint64_t(1));
  pcb.add_u64(l_pym_cpu_usage, "cpu_usage", "", "cpu", 0, uint64_t(100));
  pcb.add_u64(l_pym_mem_rss_change, "mem_rss_change", "", "", 0);
  pcb.add_u64(l_pym_mem_rss_current, "mem_rss_current", "", "", 0);
  pcb.add_u64(l_pym_serve_cpu_usage, "serve_cpu_usage", "", "cpu", 0,
              uint64_t(100));
  perfcounter.reset(pcb.create_perf_counters());
  return 0;
}

namespace {

std::mutex helper_mutex;
std::set<pid_t> helper_tids;

std::set<pid_t> list_tasks()
{
  std::set<pid_t> tasks;
  std::error_code ec;
  std::filesystem::directory_iterator end;
  for (std::filesystem::directory_iterator it("/proc/self/task", ec);
       !ec && it != end; it.increment(ec)) {
    tasks.insert(std::stoi(it->path().filename().string()));
  }
  return tasks;
}

std::set<pid_t> monitor_tasks(const std::set<pid_t>& baseline)
{
  std::lock_guard l(helper_mutex);
  std::set<pid_t> out;
  for (pid_t t : list_tasks()) {
    if (!baseline.count(t) && !helper_tids.count(t)) {
      out.insert(t);
    }
  }
  return out;
}

double process_cpu_seconds()
{
  timespec ts;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

double seconds_since(std::chrono::steady_clock::time_point t0)
{
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
    .count();
}

template <typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds timeout)
{
  const auto end = std::chrono::steady_clock::now() + timeout;
  while (!pred()) {
    if (std::chrono::steady_clock::now() >= end) {
      return pred();
    }
    std::this_thread::sleep_for(5ms);
  }
  return true;
}

class Worker {
public:
  explicit Worker(bool busy_ = false)
    : busy(busy_)
  {
    th = std::thread([this] {
      tid.store(static_cast<pid_t>(syscall(SYS_gettid)));
      while (!stop.load()) {
        if (!busy) {
          std::this_thread::sleep_for(1ms);
        }
      }
    });
    wait_until([this] { return tid.load() != 0; }, 2s);
    std::lock_guard l(helper_mutex);
    helper_tids.insert(tid.load());
  }

  ~Worker() { exit(); }

  pid_t get_tid() const { return tid.load(); }

  void exit()
  {
    if (th.joinable()) {
      stop.store(true);
      th.join();
      std::lock_guard l(helper_mutex);
      helper_tids.erase(tid.load());
    }
  }

private:
  const bool busy;
  std::atomic<pid_t> tid{0};
  std::atomic<bool> stop{false};
  std::thread th;
};

std::string join_ops(const std::vector<std::string>& ops)
{
  std::ostringstream oss;
  for (size_t i = 0; i < ops.size(); ++i) {
    oss << (i ? "; " : "") << ops[i];
  }
  return oss.str();
}

constexpr uint64_t RssChangeSane = uint64_t(1) << 62;

[[noreturn]] void finish_child(ThreadMonitor* mon, const std::set<pid_t>& baseline)
{
  mon->stop_monitoring();
  size_t alive = 0;
  wait_until(
    [&] {
      alive = monitor_tasks(baseline).size();
      return alive == 0;
    },
    500ms);
  if (alive != 0) {
    fprintf(stderr, "monitor threads alive after final stop: %zu\n", alive);
    _exit(1);
  }
  delete mon;
  _exit(0);
}

[[noreturn]] void start_stop_race_child(const std::set<pid_t>& baseline,
                                        CephContext* cct, int millis)
{
  auto* mon = new ThreadMonitor(cct);
  std::atomic<bool> done{false};
  std::atomic<long> starts{0}, stops{0};
  std::thread starter([&] {
    while (!done.load()) {
      mon->start_monitoring();
      starts.fetch_add(1);
    }
  });
  std::thread stopper([&] {
    while (!done.load()) {
      mon->stop_monitoring();
      stops.fetch_add(1);
    }
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(millis));
  done.store(true);
  starter.join();
  stopper.join();
  if (starts.load() < 10000 || stops.load() < 10000) {
    fprintf(stderr, "too few iterations: %ld starts, %ld stops\n",
            starts.load(), stops.load());
    _exit(4);
  }
  finish_child(mon, baseline);
}

[[noreturn]] void start_during_join_child(const std::set<pid_t>& baseline,
                                          CephContext* cct)
{
  auto* mon = new ThreadMonitor(cct);
  mon->start_monitoring();
  std::this_thread::sleep_for(100ms);
  std::atomic<bool> stop_returned{false};
  std::thread stopper([&] {
    mon->stop_monitoring();
    stop_returned.store(true);
  });
  std::this_thread::sleep_for(100ms);
  mon->start_monitoring();
  if (!wait_until([&] { return stop_returned.load(); }, 3s)) {
    fprintf(stderr, "stop_monitoring() still blocked 3s after a start\n");
    _exit(3);
  }
  stopper.join();
  finish_child(mon, baseline);
}

} // namespace

class ThreadMonitorOrdering : public ::testing::Test {
public:
  static void SetUpTestSuite()
  {
    if (!cct) {
      std::vector<const char*> args = {"unittest_mgr_threadmonitor_ordering"};
      cct = global_init(nullptr, args, CEPH_ENTITY_TYPE_CLIENT,
                        CODE_ENVIRONMENT_UTILITY,
                        CINIT_FLAG_NO_DEFAULT_CONFIG_FILE);
      common_init_finish(cct.get());
    }
  }

  void SetUp() override
  {
    set_interval(1);
    baseline = list_tasks();
  }

  void TearDown() override
  {
    mon.reset();
    size_t n = 0;
    EXPECT_TRUE(settle_monitor_tasks(0, 1s, &n))
      << "Sequence: test body then ~ThreadMonitor\n"
      << "Current: " << n << " monitor thread(s) alive after destruction\n"
      << "Expected: 0";
  }

protected:
  static inline boost::intrusive_ptr<CephContext> cct;
  std::set<pid_t> baseline;
  std::unique_ptr<ThreadMonitor> mon;

  void set_interval(int64_t seconds)
  {
    EXPECT_EQ(0, g_conf().set_val("mgr_module_monitor_interval",
                                  std::to_string(seconds)));
    g_conf().apply_changes(nullptr);
  }

  void make_monitor() { mon = std::make_unique<ThreadMonitor>(cct.get()); }

  size_t monitor_task_count() const { return monitor_tasks(baseline).size(); }

  bool settle_monitor_tasks(size_t expected,
                            std::chrono::milliseconds timeout = 500ms,
                            size_t* observed = nullptr)
  {
    size_t n = 0;
    bool ok = wait_until(
      [&] {
        n = monitor_task_count();
        return n == expected;
      },
      timeout);
    if (observed) {
      *observed = n;
    }
    return ok;
  }

  std::shared_ptr<PyModule> make_module(const std::string& name,
                                        bool with_counters = false)
  {
    auto m = std::make_shared<PyModule>(name);
    if (with_counters) {
      m->perf_counter_build(cct.get());
    }
    return m;
  }
};

TEST_F(ThreadMonitorOrdering, OrderingBug_T1_ZeroIntervalThenStart_BusySpins)
{
  set_interval(0);
  make_monitor();
  mon->start_monitoring();

  const double cpu0 = process_cpu_seconds();
  const auto t0 = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(300ms);
  const double wall = seconds_since(t0);
  const double cpu = process_cpu_seconds() - cpu0;
  mon->stop_monitoring();

  EXPECT_LT(cpu, 0.2 * wall)
    << "Sequence: set mgr_module_monitor_interval=0; construct ThreadMonitor; "
       "start_monitoring(); idle 300ms\n"
    << "Current: process CPU " << cpu << "s over " << wall << "s wall ("
    << (100.0 * cpu / wall) << "% of one core)\n"
    << "Expected: collection disabled at interval 0, CPU < 20% of wall time";
}

TEST_F(ThreadMonitorOrdering, OrderingBug_T1_NonZeroInterval_IdleIsQuiet)
{
  make_monitor();
  mon->start_monitoring();
  std::this_thread::sleep_for(100ms);

  const double cpu0 = process_cpu_seconds();
  const auto t0 = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(300ms);
  const double wall = seconds_since(t0);
  const double cpu = process_cpu_seconds() - cpu0;
  mon->stop_monitoring();

  EXPECT_LT(cpu, 0.2 * wall)
    << "Sequence: interval 1; start_monitoring(); idle 300ms\n"
    << "Current: process CPU " << cpu << "s over " << wall << "s wall\n"
    << "Expected: CPU < 20% of wall time (measurement control)";
}

TEST_F(ThreadMonitorOrdering, OrderingBug_T2_ConcurrentStartStop_NoLeakedThread)
{
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  set_interval(0);
  ASSERT_EXIT(start_stop_race_child(baseline, cct.get(), 1000),
              ::testing::ExitedWithCode(0), "")
    << "Sequence: interval 0; thread A: start_monitoring() in a loop; thread "
       "B: stop_monitoring() in a loop; both for 1s (>= 10000 calls each); "
       "join both; stop_monitoring(); ~ThreadMonitor\n"
    << "Current: child terminated abnormally or reported a live monitor thread\n"
    << "Expected: no crash, no leaked joinable thread, monitor stopped after "
       "the final stop";
}

TEST_F(ThreadMonitorOrdering, OrderingBug_T2_StartWhileStopJoins_NoTerminate)
{
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  ASSERT_EXIT(start_during_join_child(baseline, cct.get()),
              ::testing::ExitedWithCode(0), "")
    << "Sequence: interval 1; start; thread A: stop_monitoring() (blocked in "
       "join while the loop sleeps); 100ms later thread B: start_monitoring()\n"
    << "Current: child terminated abnormally, or stop_monitoring() never "
       "returned\n"
    << "Expected: A's stop completes, B's start yields one running monitor, "
       "no std::terminate";
}

TEST_F(ThreadMonitorOrdering, OrderingBug_T3_StopDuringSleep_ReturnsPromptly)
{
  make_monitor();
  mon->start_monitoring();
  std::this_thread::sleep_for(100ms);

  const auto t0 = std::chrono::steady_clock::now();
  mon->stop_monitoring();
  const double took = seconds_since(t0);

  EXPECT_LT(took, 0.25)
    << "Sequence: interval 1s; start_monitoring(); idle 100ms; "
       "stop_monitoring()\n"
    << "Current: stop_monitoring() took " << took << "s\n"
    << "Expected: < 0.25s (a /proc pass takes milliseconds; a full interval "
       "is 1s)";
}

TEST_F(ThreadMonitorOrdering, OrderingBug_T4_IntervalChangesWhileRunning)
{
  set_interval(2);
  make_monitor();
  mon->start_monitoring();
  size_t n = 0;

  EXPECT_TRUE(settle_monitor_tasks(1, 500ms, &n))
    << "Sequence: interval 2; start_monitoring()\n"
    << "Current: " << n << " monitor thread(s)\nExpected: 1";

  set_interval(0);
  EXPECT_TRUE(settle_monitor_tasks(0, 500ms, &n))
    << "Sequence: interval 2; start; interval 0\n"
    << "Current: " << n << " monitor thread(s) after interval 0\nExpected: 0";

  set_interval(1);
  EXPECT_TRUE(settle_monitor_tasks(1, 500ms, &n))
    << "Sequence: interval 2; start; 0; 1\n"
    << "Current: " << n << " monitor thread(s) after 0 -> 1\nExpected: 1";

  set_interval(2);
  EXPECT_TRUE(settle_monitor_tasks(1, 500ms, &n))
    << "Sequence: interval 2; start; 0; 1; 2\n"
    << "Current: " << n << " monitor thread(s) after 1 -> 2 while running\n"
    << "Expected: 1 (no second thread)";

  set_interval(0);
  EXPECT_TRUE(settle_monitor_tasks(0, 500ms, &n))
    << "Sequence: interval 2; start; 0; 1; 2; 0\n"
    << "Current: " << n << " monitor thread(s) after final 0\nExpected: 0";
}

TEST_F(ThreadMonitorOrdering, OrderingBug_T4_IntervalHammerWhileRunning_OneThread)
{
  set_interval(1);
  make_monitor();
  mon->start_monitoring();

  std::thread writer([&] {
    for (int i = 0; i < 10000; ++i) {
      set_interval(1 + (i % 3));
    }
  });
  writer.join();

  size_t n = 0;
  EXPECT_TRUE(settle_monitor_tasks(1, 500ms, &n))
    << "Sequence: interval 1; start; 10000 x set interval in {1,2,3} from "
       "another thread\n"
    << "Current: " << n << " monitor thread(s)\nExpected: 1";
}

TEST_F(ThreadMonitorOrdering, OrderingBug_T4_HugeInterval_NoSpin)
{
  set_interval(10000000000);
  make_monitor();
  mon->start_monitoring();
  std::this_thread::sleep_for(100ms);

  const double cpu0 = process_cpu_seconds();
  const auto t0 = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(300ms);
  const double wall = seconds_since(t0);
  const double cpu = process_cpu_seconds() - cpu0;
  mon->stop_monitoring();

  EXPECT_LT(cpu, 0.2 * wall)
    << "Sequence: set mgr_module_monitor_interval=10000000000; construct; "
       "start_monitoring(); idle 300ms\n"
    << "Current: process CPU " << cpu << "s over " << wall << "s wall\n"
    << "Expected: a very long interval sleeps for a very long time, CPU < 20% "
       "of wall time";
}

TEST_F(ThreadMonitorOrdering, OrderingBug_T4_Int64OptionTruncated_NonZeroKeepsRunning)
{
  make_monitor();
  mon->start_monitoring();
  size_t n = 0;
  ASSERT_TRUE(settle_monitor_tasks(1, 500ms, &n));

  set_interval(4294967296);
  EXPECT_TRUE(settle_monitor_tasks(1, 500ms, &n))
    << "Sequence: interval 1; start; set mgr_module_monitor_interval=4294967296 "
       "(2^32)\n"
    << "Current: " << n << " monitor thread(s); handle_conf_change narrows the "
       "int64 option to int, giving 0, which stops monitoring\n"
    << "Expected: 1 (the option is non-zero, so monitoring stays enabled)";
}

TEST_F(ThreadMonitorOrdering, OrderingBug_T5_DeadThreadRemoved_ModuleReleased)
{
  auto mod = make_module("t5");
  Worker w;
  make_monitor();
  mon->register_thread(w.get_tid(), 0, "t5", mod);
  ASSERT_GE(mod.use_count(), 2) << "register_thread did not take a reference";
  mon->start_monitoring();
  std::this_thread::sleep_for(300ms);
  w.exit();

  const bool released =
    wait_until([&] { return mod.use_count() == 1; }, 3500ms);
  EXPECT_TRUE(released)
    << "Sequence: interval 1; register live tid; start; thread exits; wait "
       "3.5s (3 passes)\n"
    << "Current: use_count=" << mod.use_count() << "\n"
    << "Expected: 1 (entry erased and PyModuleRef released within 3 passes)";
}

TEST_F(ThreadMonitorOrdering, OrderingBug_T5_ServeThreadExit_EntryKept)
{
  auto mod = make_module("t5s");
  Worker main_thread;
  Worker serve_thread;
  make_monitor();
  mon->register_thread(main_thread.get_tid(), serve_thread.get_tid(), "t5s",
                       mod);
  mon->start_monitoring();
  std::this_thread::sleep_for(300ms);
  serve_thread.exit();
  std::this_thread::sleep_for(2300ms);

  EXPECT_GE(mod.use_count(), 2)
    << "Sequence: interval 1; register (live main, live serve); start; serve "
       "thread exits; wait 2 passes\n"
    << "Current: use_count=" << mod.use_count() << "\n"
    << "Expected: >= 2 (entry kept because the main thread is alive)";
}

TEST_F(ThreadMonitorOrdering, OrderingBug_T5_NeverExistedTid_Removed)
{
  auto mod = make_module("t5n");
  make_monitor();
  mon->register_thread(0x3ffffff0, 0, "t5n", mod);
  mon->start_monitoring();

  EXPECT_TRUE(wait_until([&] { return mod.use_count() == 1; }, 3500ms))
    << "Sequence: interval 1; register tid that is not a task of this "
       "process; start; wait 3 passes\n"
    << "Current: use_count=" << mod.use_count() << "\n"
    << "Expected: 1 (entry erased on the first failed /proc read)";
}

TEST_F(ThreadMonitorOrdering, OrderingBug_T5_NeverExistedServeTid_EntryKept)
{
  auto mod = make_module("t5ns");
  Worker w;
  make_monitor();
  mon->register_thread(w.get_tid(), 0x3ffffff0, "t5ns", mod);
  mon->start_monitoring();
  std::this_thread::sleep_for(2300ms);

  EXPECT_GE(mod.use_count(), 2)
    << "Sequence: interval 1; register (live main, serve tid that does not "
       "exist); start; 2 passes\n"
    << "Current: use_count=" << mod.use_count() << "\n"
    << "Expected: >= 2 (a dead serve thread must not remove the entry)";
}

TEST_F(ThreadMonitorOrdering, OrderingBug_T5_RegisterChurnDuringPasses_AllReleased)
{
  make_monitor();
  mon->start_monitoring();

  constexpr int cycles = 10000;
  std::vector<std::shared_ptr<PyModule>> mods;
  mods.reserve(cycles);
  for (int i = 0; i < cycles; ++i) {
    auto mod = make_module("churn");
    {
      Worker w;
      mon->register_thread(w.get_tid(), 0, "churn", mod);
    }
    mods.push_back(std::move(mod));
  }

  size_t held = 0;
  const bool released = wait_until(
    [&] {
      held = 0;
      for (const auto& m : mods) {
        held += (m.use_count() != 1);
      }
      return held == 0;
    },
    4500ms);
  mon->stop_monitoring();

  EXPECT_TRUE(released)
    << "Sequence: interval 1; start; " << cycles
    << " x (spawn thread, register its tid, thread exits); wait 4.5s\n"
    << "Current: " << held << " of " << cycles
    << " modules still referenced by the monitor\n"
    << "Expected: 0";
}

TEST_F(ThreadMonitorOrdering, OrderingBug_T6_DuplicateRegistration_FirstKept)
{
  auto first = make_module("first");
  auto second = make_module("second");
  Worker w;
  make_monitor();
  mon->register_thread(w.get_tid(), 0, "first", first);
  mon->register_thread(w.get_tid(), 0, "second", second);

  EXPECT_EQ(1, second.use_count())
    << "Sequence: register(tid, first); register(tid, second)\n"
    << "Current: second.use_count=" << second.use_count() << "\n"
    << "Expected: 1 (duplicate ignored, nothing retains the second module)";

  mon->start_monitoring();
  std::this_thread::sleep_for(1200ms);

  EXPECT_GE(first.use_count(), 2)
    << "Sequence: register(tid, first); register(tid, second); start; 2 passes\n"
    << "Current: first.use_count=" << first.use_count() << "\n"
    << "Expected: >= 2 (first registration kept while its thread lives)";
  EXPECT_EQ(1, second.use_count())
    << "Sequence: register(tid, first); register(tid, second); start; 2 passes\n"
    << "Current: second.use_count=" << second.use_count() << "\n"
    << "Expected: 1";
}

TEST_F(ThreadMonitorOrdering, OrderingBug_T6_NullModule_LoopSurvives)
{
  auto live_mod = make_module("live", true);
  Worker null_live;
  Worker null_dead;
  Worker live;
  make_monitor();
  mon->register_thread(null_live.get_tid(), 0, "null_live", PyModuleRef());
  mon->register_thread(null_dead.get_tid(), 0, "null_dead", PyModuleRef());
  mon->register_thread(live.get_tid(), 0, "live", live_mod);
  null_dead.exit();
  mon->start_monitoring();
  std::this_thread::sleep_for(1200ms);

  size_t n = 0;
  EXPECT_TRUE(settle_monitor_tasks(1, 200ms, &n))
    << "Sequence: register(null module, live); register(null module, dead); "
       "register(real module, live); start; 2 passes\n"
    << "Current: " << n << " monitor thread(s)\nExpected: 1 (loop alive)";
  EXPECT_GE(live_mod.use_count(), 2)
    << "Sequence: as above\n"
    << "Current: live_mod.use_count=" << live_mod.use_count() << "\n"
    << "Expected: >= 2 (entry after the null-module entries still processed)";
  EXPECT_GT(live_mod->perfcounter->get(PyModule::l_pym_mem_rss_current), 0u)
    << "Sequence: as above\n"
    << "Current: mem_rss_current counter not written\n"
    << "Expected: > 0 (loop kept writing counters past null-module entries)";
}

TEST_F(ThreadMonitorOrdering, OrderingBug_T7_DestroyRightAfterStart_NoThreadLeft)
{
  make_monitor();
  mon->start_monitoring();
  mon.reset();

  size_t n = 0;
  EXPECT_TRUE(settle_monitor_tasks(0, 500ms, &n))
    << "Sequence: interval 1; construct; start_monitoring(); ~ThreadMonitor\n"
    << "Current: " << n << " monitor thread(s) after destruction\nExpected: 0";

  set_interval(0);
  set_interval(1);
  EXPECT_TRUE(settle_monitor_tasks(0, 200ms, &n))
    << "Sequence: construct; start; destroy; set interval 0; set interval 1\n"
    << "Current: " << n << " monitor thread(s) started by a stale observer\n"
    << "Expected: 0 (observer removed before destruction)";
}

TEST_F(ThreadMonitorOrdering, OrderingBug_T7_DestroyRacesConfChange_NoThreadLeft)
{
  constexpr int batches = 10;
  constexpr int per_batch = 20;
  std::mt19937 rng(0x7ead0007);
  for (int b = 0; b < batches; ++b) {
    set_interval(0);
    std::vector<std::unique_ptr<ThreadMonitor>> monitors;
    for (int i = 0; i < per_batch; ++i) {
      monitors.push_back(std::make_unique<ThreadMonitor>(cct.get()));
    }
    std::vector<size_t> order(per_batch);
    for (size_t i = 0; i < order.size(); ++i) {
      order[i] = i;
    }
    std::shuffle(order.begin(), order.end(), rng);

    std::thread flipper([&] { set_interval(1); });
    for (size_t i : order) {
      monitors[i].reset();
    }
    flipper.join();

    size_t n = 0;
    ASSERT_TRUE(settle_monitor_tasks(0, 1s, &n))
      << "Sequence: batch " << b << ": interval 0; construct " << per_batch
      << " monitors; thread A: set interval 1; thread B: destroy all "
         "monitors in shuffled order\n"
      << "Current: " << n << " monitor thread(s) after destruction\n"
      << "Expected: 0";
  }
}

TEST_F(ThreadMonitorOrdering, OrderingBug_T8_ConcurrentStop_SecondWaitsForJoin)
{
  make_monitor();
  mon->start_monitoring();
  std::this_thread::sleep_for(100ms);

  std::atomic<bool> first_returned{false};
  std::thread first([&] {
    {
      std::lock_guard l(helper_mutex);
      helper_tids.insert(static_cast<pid_t>(syscall(SYS_gettid)));
    }
    mon->stop_monitoring();
    first_returned.store(true);
  });
  std::this_thread::sleep_for(100ms);

  mon->stop_monitoring();
  const bool first_done = first_returned.load();
  size_t alive = 0;
  const bool gone = settle_monitor_tasks(0, 100ms, &alive);
  first.join();
  {
    std::lock_guard l(helper_mutex);
    helper_tids.clear();
  }

  EXPECT_TRUE(gone)
    << "Sequence: interval 1; start; thread A: stop_monitoring() (blocked in "
       "join); 100ms later thread B: stop_monitoring()\n"
    << "Current: B returned with " << alive
    << " monitor thread(s) still alive; A had returned=" << first_done << "\n"
    << "Expected: when any stop_monitoring() returns the monitor thread is "
       "gone";
}

TEST_F(ThreadMonitorOrdering, OrderingBug_T9_RssShrinks_ChangeCounterNotWrapped)
{
  auto mod = make_module("t9", true);
  Worker w;
  make_monitor();
  mon->register_thread(w.get_tid(), 0, "t9", mod);

  constexpr size_t len = 64u << 20;
  void* p = mmap(nullptr, len, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ASSERT_NE(MAP_FAILED, p);
  memset(p, 1, len);

  mon->start_monitoring();
  const bool first_pass = wait_until(
    [&] {
      return mod->perfcounter->get(PyModule::l_pym_mem_rss_current) >=
             len / 2;
    },
    2s);
  const uint64_t before =
    mod->perfcounter->get(PyModule::l_pym_mem_rss_current);
  munmap(p, len);
  ASSERT_TRUE(first_pass) << "first pass never reported RSS >= 32MB";

  const bool second_pass = wait_until(
    [&] {
      return mod->perfcounter->get(PyModule::l_pym_mem_rss_current) <
             before - len / 2;
    },
    3s);
  std::this_thread::sleep_for(50ms);
  ASSERT_TRUE(second_pass) << "RSS never dropped after munmap";

  const uint64_t change = mod->perfcounter->get(PyModule::l_pym_mem_rss_change);
  EXPECT_LE(change, RssChangeSane)
    << "Sequence: register live tid; touch 64MB; start; pass 1; munmap 64MB; "
       "pass 2\n"
    << "Current: mem_rss_change=" << change << " (rss before=" << before
    << ")\n"
    << "Expected: a 64MB shrink is not published as a value above 2^62";
}

TEST_F(ThreadMonitorOrdering, OrderingBug_Walk_RandomOperationSequences)
{
  constexpr uint64_t seed = 0x7ead0001;
  constexpr int sequences = 30;
  constexpr int steps = 10;
  constexpr int max_workers = 3;
  std::mt19937_64 rng(seed);

  struct Slot {
    std::unique_ptr<Worker> worker;
    std::shared_ptr<PyModule> mod;
    bool exited = false;
  };

  for (int s = 0; s < sequences; ++s) {
    set_interval(1);
    int opt = 1;
    bool running = false;
    make_monitor();
    std::vector<Slot> slots;
    std::vector<std::string> ops;

    auto verify = [&]() -> std::string {
      size_t n = 0;
      if (!settle_monitor_tasks(running ? 1 : 0, 300ms, &n)) {
        std::ostringstream oss;
        oss << "Current: " << n << " monitor thread(s)\nExpected: "
            << (running ? 1 : 0) << " (running=" << running << ")";
        return oss.str();
      }
      for (size_t i = 0; i < slots.size(); ++i) {
        if (!slots[i].exited && slots[i].mod.use_count() < 2) {
          std::ostringstream oss;
          oss << "Current: live worker " << i << " lost its entry (use_count="
              << slots[i].mod.use_count() << ")\nExpected: >= 2";
          return oss.str();
        }
      }
      return "";
    };

    auto report = [&](const std::string& why) {
      ADD_FAILURE() << "Seed: 0x" << std::hex << seed << std::dec
                    << " sequence " << s << " (prefix up to the failing step)\n"
                    << "Sequence: " << join_ops(ops) << "\n"
                    << why;
    };

    for (int step = 0; step < steps; ++step) {
      switch (rng() % 7) {
      case 0:
        ops.push_back("start");
        mon->start_monitoring();
        running = true;
        break;
      case 1:
        ops.push_back("stop");
        mon->stop_monitoring();
        running = false;
        break;
      case 2:
      case 3: {
        const int v = static_cast<int>(rng() % 2);
        ops.push_back("set" + std::to_string(v));
        set_interval(v);
        if (v != opt) {
          opt = v;
          running = (v != 0);
        }
        break;
      }
      case 4: {
        if (slots.size() >= max_workers) {
          ops.push_back("register(no-op: full)");
          break;
        }
        Slot slot;
        slot.worker = std::make_unique<Worker>();
        slot.mod = make_module("walk" + std::to_string(slots.size()), true);
        ops.push_back("register#" + std::to_string(slots.size()));
        mon->register_thread(slot.worker->get_tid(), 0, "walk", slot.mod);
        slots.push_back(std::move(slot));
        break;
      }
      case 5: {
        std::vector<size_t> live;
        for (size_t i = 0; i < slots.size(); ++i) {
          if (!slots[i].exited) {
            live.push_back(i);
          }
        }
        if (live.empty()) {
          ops.push_back("duplicate(no-op: none live)");
          break;
        }
        const size_t i = live[rng() % live.size()];
        auto dup = make_module("dup");
        ops.push_back("duplicate#" + std::to_string(i));
        mon->register_thread(slots[i].worker->get_tid(), 0, "dup", dup);
        if (dup.use_count() != 1) {
          report("Current: duplicate registration retained (use_count=" +
                 std::to_string(dup.use_count()) +
                 ")\nExpected: 1 (duplicate ignored)");
          return;
        }
        break;
      }
      default: {
        std::vector<size_t> live;
        for (size_t i = 0; i < slots.size(); ++i) {
          if (!slots[i].exited) {
            live.push_back(i);
          }
        }
        if (live.empty()) {
          ops.push_back("exit(no-op: none live)");
          break;
        }
        const size_t i = live[rng() % live.size()];
        ops.push_back("exit#" + std::to_string(i));
        slots[i].worker->exit();
        slots[i].exited = true;
        break;
      }
      }
      const std::string bad = verify();
      if (!bad.empty()) {
        report(bad);
        return;
      }
    }

    bool any_exited = false;
    for (const auto& slot : slots) {
      any_exited |= slot.exited;
    }
    if (any_exited) {
      set_interval(1);
      opt = 1;
      mon->start_monitoring();
      ops.push_back("ensure running at interval 1; wait 3.5s");
      const bool released = wait_until(
        [&] {
          for (const auto& slot : slots) {
            if (slot.exited && slot.mod.use_count() != 1) {
              return false;
            }
          }
          return true;
        },
        3500ms);
      if (!released) {
        std::ostringstream oss;
        oss << "Current: exited workers still registered, use_counts:";
        for (const auto& slot : slots) {
          oss << " " << (slot.exited ? slot.mod.use_count() : -1);
        }
        oss << "\nExpected: 1 for every exited worker within 3 passes";
        report(oss.str());
        return;
      }
    }
    mon.reset();
  }
}
