/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_math_matrix_types.hh"
#include "BLI_span.hh"

namespace blender {
struct Mesh;
}

namespace blender::geometry {

enum class WrapPattern : int8_t {
  Spiral = 0,
  Cross = 1,
  Rings = 2,
  Bands = 3,
  Random = 4,
};

enum class WrapShape : int8_t {
  Strap = 0,
  Tube = 1,
  Rope = 2,
};

enum class WrapEndCut : int8_t {
  Flat = 0,
  Slant = 1,
  Notch = 2,
};

struct MeshWrapParams {
  /** The surface the lines are laid on, and its transform into the space of the input mesh. */
  const Mesh *target = nullptr;
  float4x4 target_to_space = float4x4::identity();

  /** Lines from the loose edges of the input mesh (all of its edges when it has no faces). */
  bool use_edges = true;
  /** Per vertex of the input mesh: keep the drawn position, do not pull onto the surface. */
  Span<bool> pin;

  /** Lines from a pattern around an axis of the target. */
  bool use_pattern = false;
  WrapPattern pattern = WrapPattern::Spiral;
  /** 0 picks the longest side of the target, 1 to 3 are X, Y and Z. */
  int axis = 0;
  int count = 1;
  /** Rings in each group, for #WrapPattern::Bands. */
  int group = 2;
  float turns = 3.0f;
  float tilt = 0.0f;
  /** Height span as a share of the target's size along the axis. */
  float start = 0.1f;
  float end = 0.9f;
  /** Gap between the rings of a group, in strap widths. */
  float spacing = 1.5f;
  float random = 0.0f;
  float wobble = 0.0f;
  int seed = 0;

  WrapShape shape = WrapShape::Strap;
  float width = 0.05f;
  float thickness = 0.005f;
  int strands = 3;
  /** Rope turns per meter. */
  float twist = 4.0f;
  /** Points around a tube or rope strand. */
  int resolution = 8;
  float gap = 0.0f;
  float step = 0.02f;
  float tight = 0.5f;
  bool crossings = true;
  bool crossings_flip = false;
  float width_random = 0.0f;
  WrapEndCut end_cut = WrapEndCut::Flat;
  /** Depth of the end cut, in strap widths. */
  float end_cut_depth = 1.0f;
  int material_index = 0;

  /** Output only the pattern lines laid on the surface, as loose edges. */
  bool lines_only = false;
};

/**
 * Lay straps, tubes or ropes over the target's surface along the given lines.
 * Returns null when there is nothing to make.
 */
Mesh *mesh_wrap(const Mesh &mesh, const MeshWrapParams &params, const char **r_error);

}  // namespace blender::geometry
