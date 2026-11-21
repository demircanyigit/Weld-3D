/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <string>

#include "BLI_math_matrix_types.hh"

namespace blender {
struct Mesh;
}
namespace blender::geometry {
enum class BlastMetric { FaceCount = 0, Area = 1, Volume = 2, BoundsSize = 3 };
enum class BlastFilter { Threshold = 0, Largest = 1, ThresholdAndLargest = 2 };
/** What picks the islands. Size keeps the islands that pass the metric, the others remove the
 * islands they pick. */
enum class BlastSelect { Size = 0, Random = 1, Inside = 2, Gradient = 3, Noise = 4, Attribute = 5 };
enum class BlastGradient { Linear = 0, Spherical = 1 };
/** Inside: which points of an island must be in the volume. */
enum class BlastInside { Center = 0, Any = 1, All = 2 };
struct MeshBlastParams {
  BlastMetric metric = BlastMetric::Area;
  BlastFilter filter = BlastFilter::Threshold;
  double threshold = 0.01;
  int keep_count = 1;
  bool invert = false;

  BlastSelect select = BlastSelect::Size;
  /** Random: share of the islands removed. Gradient, Noise and Attribute: islands whose value is
   * at least this are removed. */
  float amount = 0.5f;
  /** Gradient, Noise and Attribute: random change of each island's value, up and down. */
  float randomness = 0.0f;
  int seed = 0;

  /** Inside: a closed mesh, and the transform from its space into the space of the mesh. */
  const Mesh *volume = nullptr;
  float4x4 volume_transform = float4x4::identity();
  BlastInside inside = BlastInside::Center;

  /** Gradient: from the origin of the object along its Z axis, or out from it, over this
   * distance. Without an object, over the bounds of the mesh. */
  BlastGradient gradient = BlastGradient::Linear;
  bool use_gradient_object = false;
  float4x4 gradient_transform = float4x4::identity();
  float distance = 2.0f;

  /** Noise: size of the noise, bigger for smaller patches. */
  float noise_scale = 1.0f;

  /** Attribute: a point attribute, its mean over each island is the value. */
  std::string attribute;

  /** Keep every island and store the removed ones in a boolean point attribute instead. */
  bool mark = false;
  std::string mark_name = "blasted";
};
/** Filter entire connected components, including loose geometry. A null return bypasses the
 * modifier; an allocated empty mesh represents removal of all eligible geometry. */
Mesh *mesh_blast(const Mesh &mesh, const MeshBlastParams &params, std::string &message);
}  // namespace blender::geometry
