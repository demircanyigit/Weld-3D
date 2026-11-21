/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup geo
 *
 * Rigid pieces of a mesh for the Bullet solver, shared by Collapse and Lean.
 */

#ifdef WITH_BULLET

#  include "BLI_array.hh"
#  include "BLI_math_matrix_types.hh"
#  include "BLI_math_vector_types.hh"
#  include "BLI_span.hh"

#  include "RBI_api.h"

namespace blender {
struct Mesh;
}

namespace blender::geometry::rigid_pieces {

struct Island {
  /** Center of mass in simulation space. */
  float3 center = float3(0.0f);
  float mass = 0.0f;
  float size = 0.0f;
  bool keep = false;
};

/** The pieces of a mesh and the points of each. */
struct Pieces {
  int num = 0;
  /** The piece of every point. */
  Array<int> island_of_vert;
  /** The points of piece i are `verts_of_island[offsets[i]]` to `verts_of_island[offsets[i+1]]`. */
  Array<int> offsets;
  Array<int> verts_of_island;

  Span<int> verts(const int i) const
  {
    return verts_of_island.as_span().slice(offsets[i], offsets[i + 1] - offsets[i]);
  }
};

/**
 * Points with the same number in \a pieces are one piece, numbered in the order they first
 * appear. When \a pieces doesn't have one number per point, every mesh island is a piece.
 */
Pieces find_pieces(const Mesh &mesh, Span<int> pieces);

/** Mass and center of mass of each island, from the volume its triangles enclose. Pieces that
 * enclose no volume (open or flat) use their bounding box and the mean of their points. */
void compute_islands(const Mesh &mesh,
                     Span<float3> positions,
                     Span<int> island_of_vert,
                     Span<int> verts_of_island_offsets,
                     Span<int> verts_of_island,
                     MutableSpan<Island> islands);

/** The convex hull of a piece, its points given around its center of mass. */
rbCollisionShape *island_shape(Span<float3> local_positions, float size);

/** A static triangle mesh. Null when \a ground has no faces. */
rbCollisionShape *ground_shape(const Mesh &ground, const float4x4 &ground_to_space, float cage);

}  // namespace blender::geometry::rigid_pieces

#endif
