/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup geo
 *
 * Lean: an object pushes through rigid pieces of a mesh, which move out of its way and lean,
 * held together by glue that breaks where the push is hardest. Used by the Lean modifier.
 */

#include <optional>
#include <string>

#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"

namespace blender {
struct Mesh;
}

namespace blender::geometry {

struct MeshLeanParams {
  /** From the mesh to the simulation space, where -Z is down. */
  float4x4 mesh_to_space = float4x4::identity();

  /** What pushes. Null for no push. */
  const Mesh *pusher = nullptr;
  /** Where the pusher ends up, from its mesh to the simulation space. */
  float4x4 pusher_to_space = float4x4::identity();
  /** The pusher's convex hull instead of its faces. */
  bool pusher_hull = true;
  /** The pusher moves along this direction in simulation space and ends where it is. */
  float3 direction = float3(1.0f, 0.0f, 0.0f);
  float distance = 1.0f;
  /** Seconds the pusher takes to move. */
  float push_time = 1.0f;
  /** Seconds the pieces get to come to rest after the push. */
  float settle_time = 1.0f;
  /** Take the pusher away after the push, so the pieces settle by themselves. */
  bool release = false;

  /** How strongly touching pieces are glued, 0 for loose pieces. */
  float glue = 0.3f;
  float friction = 0.8f;
  float gravity = 9.81f;
  /** Solver steps per 1/60 second. */
  int substeps = 4;

  /** What the pieces stand on. When null, a flat floor under the lowest point. */
  const Mesh *ground = nullptr;
  float4x4 ground_to_space = float4x4::identity();
  /** The ground is pushed out along its normals by this much. */
  float ground_cage = 0.0f;

  /** Per point: pieces with any of these points stay where they are. May be empty. */
  Span<bool> keep;
  /** Per point: points with the same number move as one piece. When empty, every mesh island
   * is a piece. */
  Span<int> pieces;

  /** When set, a point attribute with how far each point's piece moved. */
  std::optional<std::string> moved_id;
};

/** More pieces than this are refused, the solve would take too long. */
constexpr int mesh_lean_max_islands = 20000;

/**
 * Pushes the pusher through the pieces of the mesh and returns a copy of the mesh with each
 * piece moved to where it came to rest. Returns null and sets `r_error` when that isn't possible.
 */
Mesh *mesh_lean(const Mesh &mesh, const MeshLeanParams &params, const char **r_error);

}  // namespace blender::geometry
