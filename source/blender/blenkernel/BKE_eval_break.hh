/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * Stopping long evaluations with Esc.
 *
 * Modifiers are computed on the main thread, which then can't read the keyboard, so Esc never
 * reaches Blender until they are done. While the main thread updates the scene for longer than a
 * moment, a small thread reads the Esc key straight from the system instead. Each press counts
 * up a number; work that noted the number when it started knows it should stop when the number
 * has changed since.
 */

#include <cstdint>
#include <string>
#include <vector>

namespace blender {
struct ID;
}  // namespace blender

namespace blender::bke::eval_break {

/** How many times Esc was pressed during long evaluations so far. */
uint64_t presses();

/** Whether Esc was pressed since \a start, a value from #presses taken when the work began. */
inline bool requested_since(const uint64_t start)
{
  return presses() != start;
}

/** Something Esc stopped: the original object, and what to tell the user. */
struct Stopped {
  ID *id = nullptr;
  std::string message;
};

/** Remember that something was stopped, from any thread. */
void note_stopped(ID *id, std::string message);
/** What was stopped since the last call, to report it and update the objects again. */
std::vector<Stopped> take_stopped();

/**
 * Marks the main thread updating the scene, so Esc is watched while it takes long. Does nothing
 * on other threads, renders stop through their own means.
 */
class Scope {
  bool active_ = false;

 public:
  Scope();
  ~Scope();
  Scope(const Scope &) = delete;
  Scope &operator=(const Scope &) = delete;
};

}  // namespace blender::bke::eval_break
