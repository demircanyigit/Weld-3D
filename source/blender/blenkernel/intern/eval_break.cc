/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

#include "BLI_threads.h"

#include "BKE_eval_break.hh"

namespace blender::bke::eval_break {

using Clock = std::chrono::steady_clock;

static std::atomic<uint64_t> press_count{0};
/** Nesting depth of #Scope on the main thread. */
static std::atomic<int> depth{0};
/** When the outermost #Scope began, in nanoseconds of #Clock. */
static std::atomic<int64_t> started_ns{0};

/** Updates shorter than this never look at the keyboard, so Esc meant for a tool is left alone. */
static constexpr auto watch_after = std::chrono::milliseconds(250);
static constexpr auto poll_interval = std::chrono::milliseconds(30);

uint64_t presses()
{
  return press_count.load(std::memory_order_relaxed);
}

static std::mutex stopped_mutex;
static std::vector<Stopped> stopped_list;

void note_stopped(ID *id, std::string message)
{
  std::lock_guard lock(stopped_mutex);
  stopped_list.push_back({id, std::move(message)});
}

std::vector<Stopped> take_stopped()
{
  std::lock_guard lock(stopped_mutex);
  return std::move(stopped_list);
}

#ifdef _WIN32

/** Whether one of Blender's own windows has the keyboard. */
static bool blender_has_focus()
{
  HWND window = GetForegroundWindow();
  if (window == nullptr) {
    return false;
  }
  DWORD process = 0;
  GetWindowThreadProcessId(window, &process);
  return process == GetCurrentProcessId();
}

static bool esc_down()
{
  return (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
}

static void watch()
{
  /* A press counts when the key goes down during a long update, not when it was already held. */
  bool armed = false;
  bool was_down = false;
  while (true) {
    std::this_thread::sleep_for(poll_interval);
    const bool long_update =
        depth.load() > 0 &&
        Clock::now().time_since_epoch().count() - started_ns.load() >=
            std::chrono::duration_cast<Clock::duration>(watch_after).count();
    if (!long_update) {
      armed = false;
      continue;
    }
    const bool down = esc_down() && blender_has_focus();
    if (!armed) {
      armed = true;
      was_down = down;
      continue;
    }
    if (down && !was_down) {
      press_count.fetch_add(1);
    }
    was_down = down;
  }
}

static void ensure_watcher()
{
  static std::once_flag once;
  std::call_once(once, [] { std::thread(watch).detach(); });
}

#else

/* Other systems: Esc during a long update is not watched yet. */
static void ensure_watcher() {}

#endif

Scope::Scope()
{
  if (!BLI_thread_is_main()) {
    return;
  }
  active_ = true;
  ensure_watcher();
  if (depth.fetch_add(1) == 0) {
    started_ns.store(Clock::now().time_since_epoch().count());
  }
}

Scope::~Scope()
{
  if (active_) {
    depth.fetch_sub(1);
  }
}

}  // namespace blender::bke::eval_break
