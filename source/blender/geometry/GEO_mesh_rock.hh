/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <optional>
#include <string>

struct Mesh;

namespace blender::geometry {

enum class MeshRockError {
  NoError = 0,
  /** The input is not a closed volume, so it can't be cut. */
  NonManifold = 1,
  SolverNotAvailable = 2,
  UnknownError = 3,
};

struct MeshRockParams {
  /** How many flat cuts. */
  int cuts = 24;
  /** How deep the cuts go, as a share of the size of the shape in their direction. */
  float depth = 0.45f;
  /** How much the cuts aim at the corners of the shape instead of any direction. */
  float corners = 0.4f;
  int seed = 0;
  /** Every loose part is cut on its own. */
  bool per_island = true;
  /** Optional name of a face-domain boolean attribute, true for faces made by the cuts. */
  std::optional<std::string> cut_id;
};

/**
 * Make a low poly rock of a closed mesh: cut it with flat cuts in random directions, mostly
 * through its corners. Every face made by a cut is flat shaded.
 *
 * \return A newly allocated, main-database-free #Mesh, or null if \a r_error was set.
 */
Mesh *mesh_rock(const Mesh &mesh, const MeshRockParams &params, MeshRockError *r_error);

}  // namespace blender::geometry
