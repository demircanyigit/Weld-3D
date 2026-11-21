/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup geo
 *
 * Points added along the edges of a mesh. Used by the Resample modifier and Resample Mesh node.
 */

#include <optional>
#include <string>

#include "BLI_span.hh"

namespace blender {
struct Mesh;
}

namespace blender::geometry {

/** Keeps a very small length from making a mesh that doesn't fit into memory. */
constexpr int64_t resample_mesh_max_new_points = 100'000'000;

/** The points to add to an edge so none of its segments is longer than \a segment_length. */
int resample_mesh_cuts_for_length(float edge_length, float segment_length);

/**
 * Splits every edge into `cuts[i] + 1` even segments. Attributes are interpolated.
 * \param new_points_id: When set, a boolean point attribute that is true on the added points.
 */
Mesh *resample_mesh(const Mesh &mesh,
                    Span<int> cuts,
                    const std::optional<std::string> &new_points_id);

}  // namespace blender::geometry
