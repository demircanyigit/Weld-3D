/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <array>
#include <cmath>

#include "BLI_array.hh"
#include "BLI_bounds_types.hh"
#include "BLI_kdtree.hh"
#include "BLI_math_vector.hh"
#include "BLI_task.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "BKE_attribute.hh"
#include "BKE_mesh.hh"

#include "DNA_mesh_types.h"

#include "GEO_mesh_voronoi_fracture.hh"

#include "mesh_boolean_manifold.hh"
#include "mesh_fracture_cell.hh"

namespace blender::geometry {

using fracture::CellBuilder;
using fracture::CellFace;

/**
 * Find the Voronoi cell of one point: start with a box around the mesh, and cut it with the plane
 * halfway to the other points, starting at the closest one. Once a point is more than twice the
 * size of the cell away, it can't change the cell any more, and so can't any point after it.
 */
static boolean::FractureCell calc_cell(const int index,
                                       const Span<float3> points,
                                       const KDTree_3d &tree,
                                       const double3 &box_min,
                                       const double3 &box_max,
                                       const double gap,
                                       const double epsilon)
{
  boolean::FractureCell result;
  result.point_index = index;

  const double3 point(points[index]);
  CellBuilder cell(box_min, box_max);
  double max_distance = cell.max_distance(point);

  int checked = 0;
  int query_size = std::min(32, int(points.size()));
  Array<KDTreeNearest_3d> nearest;
  bool done = false;
  while (!done) {
    nearest.reinitialize(query_size);
    const int found = kdtree_find_nearest_n<float3>(
        &tree, points[index], nearest.data(), uint(query_size));
    for (const int i : IndexRange(checked, found - checked)) {
      const int other_index = nearest[i].index;
      if (other_index == index) {
        continue;
      }
      const double3 to_other = double3(points[other_index]) - point;
      const double distance = math::length(to_other);
      if (distance <= epsilon) {
        /* Points in the same place would have the same cell, so only the first one gets it. */
        if (other_index < index) {
          return result;
        }
        continue;
      }
      const double half_distance = (distance - gap) * 0.5;
      if (half_distance >= max_distance) {
        done = true;
        break;
      }
      const double3 normal = to_other / distance;
      if (cell.clip(normal, math::dot(normal, point) + half_distance, epsilon)) {
        if (cell.faces.is_empty()) {
          return result;
        }
        max_distance = cell.max_distance(point);
      }
    }
    checked = found;
    if (found < query_size || query_size == points.size()) {
      break;
    }
    query_size = int(std::min(int64_t(query_size) * 2, points.size()));
  }

  for (const CellFace &face : cell.faces) {
    result.points.extend(face.verts);
    result.normals.append(cell.normals[face.plane]);
  }
  return result;
}

static Vector<boolean::FractureCell> calc_cells(const Mesh &mesh,
                                                const Span<float3> points,
                                                const float cut_gap)
{
  const Bounds<float3> bounds = *mesh.bounds_min_max();
  const double size = math::length(double3(bounds.max) - double3(bounds.min));
  /* Cuts of this box would look like part of the mesh, so it should not touch the mesh. */
  const double3 margin(size * 0.01 + 1e-4);
  const double3 box_min = double3(bounds.min) - margin;
  const double3 box_max = double3(bounds.max) + margin;
  const double epsilon = size * 1e-10;

  KDTree_3d *tree = kdtree_3d_new(points.size());
  for (const int i : points.index_range()) {
    kdtree_3d_insert(tree, i, points[i]);
  }
  kdtree_3d_balance(tree);

  Array<boolean::FractureCell> cells(points.size());
  threading::parallel_for(points.index_range(), 16, [&](const IndexRange range) {
    for (const int i : range) {
      cells[i] = calc_cell(i, points, *tree, box_min, box_max, cut_gap, epsilon);
    }
  });
  kdtree_3d_free(tree);

  Vector<boolean::FractureCell> result;
  for (boolean::FractureCell &cell : cells) {
    if (!cell.points.is_empty()) {
      result.append(std::move(cell));
    }
  }
  return result;
}

Mesh *mesh_voronoi_fracture(const Mesh &mesh,
                            const Span<float3> cell_points,
                            const MeshVoronoiFractureParams &params,
                            MeshVoronoiFractureError *r_error)
{
  *r_error = MeshVoronoiFractureError::NoError;
#ifdef WITH_MANIFOLD
  const Vector<boolean::FractureCell> cells = calc_cells(mesh, cell_points, params.cut_gap);

  Array<int> vert_pieces;
  Vector<int> piece_cells;
  Array<bool> interior_faces;
  boolean::BooleanError error = boolean::BooleanError::NoError;
  Mesh *result = boolean::mesh_fracture_manifold(
      mesh, cells, params.separate_islands, vert_pieces, piece_cells, interior_faces, &error);
  if (result == nullptr) {
    *r_error = error == boolean::BooleanError::NonManifold ?
                   MeshVoronoiFractureError::NonManifold :
                   MeshVoronoiFractureError::UnknownError;
    return nullptr;
  }

  bke::MutableAttributeAccessor attributes = result->attributes_for_write();
  if (params.piece_index_id) {
    bke::SpanAttributeWriter<int> attribute = attributes.lookup_or_add_for_write_only_span<int>(
        *params.piece_index_id, bke::AttrDomain::Point);
    attribute.span.copy_from(vert_pieces);
    attribute.finish();
  }
  if (params.cell_index_id) {
    bke::SpanAttributeWriter<int> attribute = attributes.lookup_or_add_for_write_only_span<int>(
        *params.cell_index_id, bke::AttrDomain::Point);
    threading::parallel_for(attribute.span.index_range(), 4096, [&](const IndexRange range) {
      for (const int i : range) {
        attribute.span[i] = piece_cells[vert_pieces[i]];
      }
    });
    attribute.finish();
  }
  if (params.interior_id) {
    bke::SpanAttributeWriter<bool> attribute = attributes.lookup_or_add_for_write_only_span<bool>(
        *params.interior_id, bke::AttrDomain::Face);
    attribute.span.copy_from(interior_faces);
    attribute.finish();
  }
  return result;
#else
  UNUSED_VARS(mesh, cell_points, params);
  *r_error = MeshVoronoiFractureError::SolverNotAvailable;
  return nullptr;
#endif
}

}  // namespace blender::geometry
