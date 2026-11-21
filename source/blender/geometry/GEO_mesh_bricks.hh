/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <optional>
#include <string>

#include "BLI_math_vector_types.hh"

namespace blender {
struct Mesh;
}

namespace blender::geometry {

/** How the blocks are laid. The values are those of the row patterns of Masonry. */
enum class BricksPattern {
  Running = 0,
  Stack = 1,
  Flemish = 2,
  English = 3,
  Ashlar = 4,
};

struct MeshBricksParams {
  BricksPattern pattern = BricksPattern::Running;
  /** One block: length along the wall, depth into it, and height. */
  float3 size = float3(0.4f, 0.2f, 0.2f);
  /** Space between the blocks. */
  float gap = 0.005f;
  /** Random changes of the joints and the course heights, 0 to 1. */
  float random_length = 0.0f;
  float random_height = 0.0f;
  /** Small random turns and shrinking of each block, 0 to 1. */
  float uneven = 0.0f;
  /** Width of the rounded block edges, and the faces across them. */
  float bevel = 0.0f;
  int bevel_segments = 1;
  int seed = 0;
  /** Optional face attribute that is true for the faces that fill the cuts. */
  std::optional<std::string> interior_id;
  /** Optional point attributes with the block number and a random value per block. */
  std::optional<std::string> piece_index_id;
  std::optional<std::string> piece_random_id;
};

enum class MeshBricksError {
  None,
  /** The blocks are too small for the mesh. */
  TooManyCells,
  /** More blocks than #mesh_bricks_max_blocks. Only the first ones are made. */
  TooManyBlocks,
};

constexpr int64_t mesh_bricks_max_cells = 4000000;
constexpr int mesh_bricks_max_blocks = 100000;

/**
 * Cut a mesh into solid blocks laid in horizontal courses along the mesh's Z axis. Walls along X
 * and Y are found on a grid of cells half a block deep, and their blocks interlock where walls
 * meet. The surface of the mesh is kept, the cuts are filled.
 *
 * \return The new mesh, or null when nothing could be made (see \a r_error).
 */
Mesh *mesh_bricks(const Mesh &mesh, const MeshBricksParams &params, MeshBricksError &r_error);

}  // namespace blender::geometry
