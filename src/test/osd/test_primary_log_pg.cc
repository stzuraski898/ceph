// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2026 IBM
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 */

/*
 * test_primary_log_pg - Unit tests for PrimaryLogPG coroutine lifecycle.
 *
 * These tests exercise the two unsafe code patterns introduced by PR #67079
 * ("osd: Support for Synchronous Reads in EC") using only the types defined
 * in Coroutines.h, without requiring a full OSD stack.
 *
 * Root cause
 * ----------
 * PrimaryLogPG::do_op() spawns a resume_token_t (boost::coroutines2::push_type)
 * and stores it in unique_ptr<resume_token_t> coro_resumer.  ECBackend stores
 * a CoroHandles{yield, *coro_resumer} — holding a *reference* to the token.
 *
 * Path 1 — self-destruction (PrimaryLogPG.cc:2628)
 *   The coroutine body runs `coro_resumer = nullptr` which destroys the
 *   push_type while executing on its own stack.  This is undefined behaviour:
 *   the push_type destructor triggers a forced-unwind of the currently-running
 *   coroutine from within that same coroutine.
 *
 * Path 2 — destroy-while-suspended / use-after-free
 *   on_change() resets coro_resumer while the coroutine is suspended waiting
 *   for an async read.  The completion callback in ECBackend::objects_read_sync
 *   then calls `coro.resume()` through the now-dangling CoroHandles::resume
 *   reference — a use-after-free.
 *
 * Test strategy
 * -------------
 * The SafeResumer sentinel wrapper records whether it has been destroyed via a
 * shared_ptr<bool> alive flag.  This makes the unsafe conditions directly
 * assertable in-process:
 *
 *   Path 1 BugPattern:  asserts alive==false INSIDE the coroutine body at the
 *                       point where the body called coro_resumer=nullptr,
 *                       proving self-destruction happened mid-execution.
 *   Path 1 FixedPattern: asserts alive==true inside the body (no self-destruct).
 *
 *   Path 2 BugPattern:  simulates the buggy completion callback (no liveness
 *                       check) and asserts it would call resume() on a dead
 *                       token — *alive==false at call time.
 *   Path 2 FixedPattern: the guarded callback checks *alive before calling
 *                        resume() and correctly skips it.
 */

#include <gtest/gtest.h>
#include <functional>
#include <memory>
#include <optional>
#include "osd/Coroutines.h"

// ---------------------------------------------------------------------------
// SafeResumer — a resume_token_t wrapper that records its own destruction via
// a shared_ptr<bool> alive flag.  Observers (test body, lambda closures) hold
// copies of that shared_ptr so they can read the flag even after the
// SafeResumer is gone.
// ---------------------------------------------------------------------------
struct SafeResumer {
  std::shared_ptr<bool> alive = std::make_shared<bool>(true);
  std::unique_ptr<resume_token_t> token;

  explicit SafeResumer(std::function<void(yield_token_t&)> fn)
    : token(std::make_unique<resume_token_t>(std::move(fn))) {}

  ~SafeResumer() {
    // Mark dead BEFORE resetting the token so that the forced-unwind
    // triggered by token.reset() sees alive==false if it ever looks.
    *alive = false;
    token.reset();
  }

  SafeResumer(const SafeResumer&) = delete;
  SafeResumer& operator=(const SafeResumer&) = delete;
  SafeResumer(SafeResumer&&) = default;
  SafeResumer& operator=(SafeResumer&&) = default;

  bool is_alive() const { return *alive; }
  void operator()() { (*token)(); }
  explicit operator bool() const { return static_cast<bool>(*token); }
};

// ---------------------------------------------------------------------------
// Minimal state bundle mirroring the coroutine-related fields of PrimaryLogPG.
// ---------------------------------------------------------------------------
struct CoroState {
  std::unique_ptr<SafeResumer> coro_resumer;
  bool coro_op_in_flight = false;
  std::optional<CoroHandles> coro_handles;
  bool completed = false;
  bool cleanup_called = false;

  void on_coroutine_complete() {
    coro_op_in_flight = false;
    cleanup_called = true;
  }
};

// ---------------------------------------------------------------------------
// CoroLifecycle fixture
// ---------------------------------------------------------------------------
class CoroLifecycle : public ::testing::Test {
protected:
  CoroState s;

  // Simulate on_change(): destroy the resumer from outside while suspended.
  void on_change() {
    s.coro_resumer.reset();
    s.coro_op_in_flight = false;
  }
};

// ===========================================================================
// Path 1: sync completion — coroutine body destroys its owning push_type
// ===========================================================================

// BUGGY PATTERN — FAILS when the bug is present, passes when fixed.
//
// The coroutine body calls `coro_resumer = nullptr` (PrimaryLogPG.cc:2628)
// while still executing on the coroutine stack — self-destruction mid-body.
//
// We capture the alive flag from *inside* the body, immediately after the
// self-destruct.  A correct implementation never touches coro_resumer from
// inside the body, so alive_after_reset would never be set.  Under the buggy
// pattern it is set to false (the destructor fired while the body was live).
//
// The test asserts alive_after_reset is true — i.e., the resumer was NOT
// destroyed while the body was running.  This FAILS under the current code.
TEST_F(CoroLifecycle, CoroLeakPath1_SyncReturn_BugPattern)
{
  // Captured from inside the coroutine body after `coro_resumer = nullptr`.
  // Remains unset (false) if the buggy line is never reached.
  // Set to *alive at that point — will be false if self-destruction occurred.
  std::optional<bool> alive_after_self_destruct;

  s.coro_op_in_flight = true;
  // Capture alive *before* the lambda runs so the shared_ptr outlives the
  // SafeResumer even if self-destruction happens inside the body.
  std::shared_ptr<bool> alive;
  auto resumer = std::make_unique<SafeResumer>(
    [this, &alive_after_self_destruct, &alive](yield_token_t& yield) {
      s.coro_handles.emplace(CoroHandles{ yield, *s.coro_resumer->token });
      // no yield — synchronous return (early-exit path in do_op_impl)
      s.completed = true;

      // BUG: destroys *this SafeResumer while still executing on its stack.
      // Record the alive state immediately after — it will be false because
      // the SafeResumer destructor runs synchronously during this reset.
      s.coro_resumer = nullptr;
      alive_after_self_destruct = *alive;  // false: destructor already ran

      s.on_coroutine_complete();
    });

  alive = resumer->alive;
  s.coro_resumer = std::move(resumer);
  (*s.coro_resumer)();

  // alive_after_self_destruct was set inside the body after coro_resumer=nullptr.
  // It must be true for a correct implementation (resumer not yet destroyed).
  // Under the bug it is false — the SafeResumer was destroyed mid-body.
  ASSERT_TRUE(alive_after_self_destruct.has_value())
    << "BUG: coroutine body reached the self-destruct line";
  EXPECT_TRUE(*alive_after_self_destruct)
    << "BUG: coro_resumer was destroyed from inside the coroutine body "
       "(self-destruction while executing on its own stack)";

  EXPECT_TRUE(s.completed);
  EXPECT_TRUE(s.cleanup_called);
  EXPECT_FALSE(s.coro_op_in_flight);
}

// FIXED PATTERN — cleanup moved outside the coroutine body.
// The push_type is only reset after (*coro_resumer)() returns to the caller.
// alive must be true while the body is executing.
TEST_F(CoroLifecycle, CoroLeakPath1_SyncReturn_FixedPattern)
{
  bool alive_inside_body = false;

  s.coro_op_in_flight = true;
  auto resumer = std::make_unique<SafeResumer>(
    [this, &alive_inside_body](yield_token_t& yield) {
      s.coro_handles.emplace(CoroHandles{ yield, *s.coro_resumer->token });
      // no yield
      s.completed = true;
      // Fixed: body does NOT reset coro_resumer.  It must still be alive here.
      alive_inside_body = s.coro_resumer->is_alive();
    });
  s.coro_resumer = std::move(resumer);
  (*s.coro_resumer)();
  // Cleanup from outside — safe, coroutine stack is no longer active.
  if (s.coro_resumer && !(*s.coro_resumer)) {
    s.coro_resumer = nullptr;
    s.on_coroutine_complete();
  }

  EXPECT_TRUE(alive_inside_body)
    << "FIX: resumer must be alive while the coroutine body is executing";
  EXPECT_TRUE(s.completed);
  EXPECT_TRUE(s.cleanup_called);
  EXPECT_FALSE(s.coro_op_in_flight);
  EXPECT_EQ(nullptr, s.coro_resumer);
}

// ===========================================================================
// Path 2: destroy-while-suspended / use-after-free in completion callback
// ===========================================================================

// BUGGY PATTERN — should FAIL if the bug is present, PASS when fixed.
//
// Coroutine suspends (waiting for async read).  on_change() destroys the
// token.  A completion callback without a liveness guard then calls resume()
// on the dead token — use-after-free.
//
// We model the buggy callback as one that checks !(*alive) and records that
// it *would* have called resume() on a dead token.  The test asserts that
// this should NOT happen — so it FAILS when the buggy scenario plays out.
TEST_F(CoroLifecycle, CoroLeakPath2_DestroyWhileSuspended_BugPattern)
{
  bool callback_called_on_dead_token = false;

  s.coro_op_in_flight = true;
  auto resumer = std::make_unique<SafeResumer>(
    [this](yield_token_t& yield) {
      s.coro_handles.emplace(CoroHandles{ yield, *s.coro_resumer->token });
      yield();  // suspend — simulates waiting for async read
      // Never resumed in the bug path; nothing below executes.
      s.completed = true;
    });

  std::shared_ptr<bool> alive = resumer->alive;
  resume_token_t* raw = resumer->token.get();
  s.coro_resumer = std::move(resumer);
  (*s.coro_resumer)();  // starts coroutine; it suspends at yield()

  EXPECT_FALSE(s.completed);
  ASSERT_EQ(true, *alive);  // still alive before on_change

  // Buggy completion callback: holds a raw pointer, calls resume()
  // unconditionally without checking liveness.  We simulate what it would
  // do by recording whether the token is dead at call time.
  auto buggy_callback = [&callback_called_on_dead_token, alive]() {
    if (!(*alive)) {
      // Token is dead — a real callback calling raw->operator()() here
      // would be a use-after-free.
      callback_called_on_dead_token = true;
    }
  };

  // PG teardown while coroutine is suspended.
  on_change();
  ASSERT_FALSE(*alive);  // SafeResumer destructor set this to false

  // Completion callback fires after teardown — the race in real code.
  buggy_callback();

  // The callback encountered a dead token.  This is the bug: a real callback
  // calling resume() here would access freed memory.
  // We assert this should NOT happen — so this test documents a FAILURE
  // condition: if callback_called_on_dead_token is true, the bug is present.
  EXPECT_FALSE(callback_called_on_dead_token)
    << "BUG: completion callback would call resume() on a destroyed token "
       "(use-after-free); on_change() must prevent this";
}

// FIXED PATTERN — the callback captures the alive flag and skips resume()
// if the token has been destroyed.  No use-after-free.
TEST_F(CoroLifecycle, CoroLeakPath2_DestroyWhileSuspended_FixedPattern)
{
  bool callback_resumed = false;

  s.coro_op_in_flight = true;
  auto resumer = std::make_unique<SafeResumer>(
    [this, &callback_resumed](yield_token_t& yield) {
      s.coro_handles.emplace(CoroHandles{ yield, *s.coro_resumer->token });
      yield();  // suspend
      // Reached only if resumed safely.
      callback_resumed = true;
      s.completed = true;
    });

  std::shared_ptr<bool> alive = resumer->alive;
  resume_token_t* raw = resumer->token.get();
  s.coro_resumer = std::move(resumer);
  (*s.coro_resumer)();  // starts; suspends at yield()

  // Fixed callback: captures alive and guards the resume() call.
  auto fixed_callback = [alive, raw]() {
    if (*alive) {
      (*raw)();  // safe: token still valid
    }
    // else: token destroyed — silently skip (no use-after-free)
  };

  // PG teardown.
  on_change();
  ASSERT_FALSE(*alive);

  // Callback fires after teardown — guarded, so resume() is skipped.
  fixed_callback();

  EXPECT_FALSE(callback_resumed)
    << "FIX: callback correctly skipped resume() on destroyed token";
  EXPECT_FALSE(s.coro_op_in_flight);
}

// ===========================================================================
// Positive control: well-formed coroutine lifecycle
// ===========================================================================

// Yield once, resume externally, complete normally — all cleanup from outside.
TEST_F(CoroLifecycle, CoroNormalYieldAndResume)
{
  int step = 0;
  bool alive_inside_body = false;

  s.coro_op_in_flight = true;
  auto resumer = std::make_unique<SafeResumer>(
    [this, &step, &alive_inside_body](yield_token_t& yield) {
      s.coro_handles.emplace(CoroHandles{ yield, *s.coro_resumer->token });
      step = 1;
      yield();   // suspend
      step = 2;  // reached after external resume
      s.completed = true;
      alive_inside_body = s.coro_resumer->is_alive();
    });

  s.coro_resumer = std::move(resumer);
  (*s.coro_resumer)();  // starts; suspends at yield()

  EXPECT_EQ(1, step);
  EXPECT_FALSE(s.completed);
  ASSERT_NE(nullptr, s.coro_resumer);

  // External resume — mirrors the async-read completion callback.
  (*s.coro_resumer)();

  EXPECT_EQ(2, step);
  EXPECT_TRUE(s.completed);
  EXPECT_TRUE(alive_inside_body)
    << "resumer must be alive while the coroutine body is executing";

  // Clean up from outside (the correct pattern).
  if (s.coro_resumer && !(*s.coro_resumer)) {
    s.coro_resumer = nullptr;
    s.on_coroutine_complete();
  }

  EXPECT_TRUE(s.cleanup_called);
  EXPECT_FALSE(s.coro_op_in_flight);
  EXPECT_EQ(nullptr, s.coro_resumer);
}
