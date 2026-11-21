/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <cmath>

#include "BLI_array.hh"
#include "BLI_vector.hh"

#include "DNA_mesh_types.h"

#include "BKE_attribute.hh"
#include "BKE_mesh.hh"

#include "bmesh.hh"

#include "GEO_resample_mesh.hh"

namespace blender::geometry {

int resample_mesh_cuts_for_length(const float edge_length, const float segment_length)
{
  if (segment_length <= 0.0f) {
    return 0;
  }
  /* The small margin keeps an edge of exactly N lengths from getting an extra segment. */
  const double segments = std::ceil(double(edge_length) / double(segment_length) - 1e-5);
  return int(std::clamp(segments - 1.0, 0.0, double(resample_mesh_max_new_points)));
}

Mesh *resample_mesh(const Mesh &mesh,
                    const Span<int> cuts,
                    const std::optional<std::string> &new_points_id)
{
  BMeshCreateParams create_params{};
  BMeshFromMeshParams convert_params{};
  BMesh *bm = BKE_mesh_to_bmesh_ex(&mesh, &create_params, &convert_params);
  BM_mesh_elem_table_ensure(bm, BM_EDGE);

  /* Splitting adds edges, so the table doesn't stay valid. */
  Array<BMEdge *> edges(mesh.edges_num);
  for (const int i : edges.index_range()) {
    edges[i] = BM_edge_at_index(bm, i);
  }

  /* Attributes are interpolated by the split. */
  Vector<BMVert *> new_verts;
  for (const int i : edges.index_range()) {
    if (cuts[i] == 0) {
      continue;
    }
    new_verts.resize(cuts[i]);
    BM_edge_split_n(bm, edges[i], cuts[i], new_verts.data());
    for (BMVert *vert : new_verts) {
      BM_elem_flag_enable(vert, BM_ELEM_TAG);
    }
  }

  Array<bool> new_points;
  if (new_points_id) {
    new_points.reinitialize(bm->totvert);
    BMIter iter;
    BMVert *vert;
    int i;
    BM_ITER_MESH_INDEX (vert, &iter, bm, BM_VERTS_OF_MESH, i) {
      new_points[i] = BM_elem_flag_test_bool(vert, BM_ELEM_TAG);
    }
  }

  Mesh *result = BKE_mesh_from_bmesh_for_eval_nomain(bm, nullptr, &mesh);
  BM_mesh_free(bm);

  if (new_points_id) {
    result->attributes_for_write().add<bool>(
        *new_points_id,
        bke::AttrDomain::Point,
        bke::AttributeInitVArray(VArray<bool>::from_span(new_points)));
  }
  return result;
}

}  // namespace blender::geometry
