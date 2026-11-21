/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup geo
 *
 * Chisel: flat hand-cut chips on the sharp edges of boards and beams, V notches across the edge
 * and long slants along it. Used by the Chisel modifier.
 */

#include <optional>
#include <string>

#include "BLI_span.hh"

namespace blender {
struct Mesh;
}

namespace blender::geometry {

struct MeshChiselParams {
  /** Cuts per meter of edge. */
  float amount = 2.0f;
  /** 0 makes only slants, 1 only notches. */
  float mix = 0.5f;
  /** Length of a slant along the edge. */
  float length_min = 0.1f;
  float length_max = 0.4f;
  /** How deep a cut goes into the faces beside the edge. */
  float depth_min = 0.01f;
  float depth_max = 0.03f;
  /** Edges with faces turning more than this, in radians, get cuts. */
  float edge_angle = 0.5236f;
  /** Cut the edges across the ends of a board too, not only those along it. */
  bool ends = false;
  int seed = 0;
  /** Per edge, the edges that may get cuts. All when empty. */
  Span<bool> edge_selection;
  /** Material index of the cut faces, or -1 for the material of the face that was cut. */
  int material_index = -1;
  /** When set, a boolean face attribute that is true on the cut faces. */
  std::optional<std::string> cut_id;
};

/**
 * Returns a copy of the closed mesh with the cuts made, or null when nothing was cut.
 * Sets `r_error` when the cut couldn't be made.
 */
Mesh *mesh_chisel(const Mesh &mesh, const MeshChiselParams &params, const char **r_error);

}  // namespace blender::geometry
