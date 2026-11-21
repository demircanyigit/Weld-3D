/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef WITH_BULLET

#  include <algorithm>
#  include <cfloat>
#  include <cmath>

#  include "BLI_disjoint_set.hh"
#  include "BLI_map.hh"
#  include "BLI_math_matrix.hh"
#  include "BLI_math_vector.hh"

#  include "DNA_mesh_types.h"

#  include "BKE_mesh.hh"

#  include "rigid_pieces.hh"

namespace blender::geometry::rigid_pieces {

Pieces find_pieces(const Mesh &mesh, const Span<int> pieces)
{
  const int verts_num = mesh.verts_num;
  Pieces result;
  result.island_of_vert.reinitialize(verts_num);
  MutableSpan<int> island_of_vert = result.island_of_vert;
  int islands_num = 0;
  if (pieces.size() == int64_t(verts_num)) {
    /* Pieces given per point, numbered from 0 in the order they first appear. */
    Map<int, int> island_of_piece;
    for (const int v : IndexRange(verts_num)) {
      island_of_vert[v] = island_of_piece.lookup_or_add(pieces[v], islands_num);
      if (island_of_vert[v] == islands_num) {
        islands_num++;
      }
    }
  }
  else {
    /* Islands: points joined by edges. */
    DisjointSet<int> sets(verts_num);
    for (const int2 edge : mesh.edges()) {
      sets.join(edge[0], edge[1]);
    }
    Array<int> island_of_root(verts_num, -1);
    for (const int v : IndexRange(verts_num)) {
      const int root = sets.find_root(v);
      if (island_of_root[root] == -1) {
        island_of_root[root] = islands_num++;
      }
      island_of_vert[v] = island_of_root[root];
    }
  }
  result.num = islands_num;

  /* The points of each island, in order. */
  result.offsets.reinitialize(islands_num + 1);
  result.offsets.fill(0);
  for (const int island : island_of_vert) {
    result.offsets[island + 1]++;
  }
  for (const int i : IndexRange(islands_num)) {
    result.offsets[i + 1] += result.offsets[i];
  }
  result.verts_of_island.reinitialize(verts_num);
  Array<int> fill(result.offsets.as_span().drop_back(1));
  for (const int v : IndexRange(verts_num)) {
    result.verts_of_island[fill[island_of_vert[v]]++] = v;
  }
  return result;
}


/** Mass and center of mass of each island, from the volume its triangles enclose. Pieces that
 * enclose no volume (open or flat) use their bounding box and the mean of their points. */
void compute_islands(const Mesh &mesh,
                     const Span<float3> positions,
                     const Span<int> island_of_vert,
                     const Span<int> verts_of_island_offsets,
                     const Span<int> verts_of_island,
                     MutableSpan<Island> islands)
{
  const int islands_num = islands.size();
  Array<double> volume(islands_num, 0.0);
  Array<double3> moment(islands_num, double3(0.0));
  const Span<int> corner_verts = mesh.corner_verts();
  for (const int3 tri : mesh.corner_tris()) {
    const int v0 = corner_verts[tri[0]];
    const float3 &a = positions[v0];
    const float3 &b = positions[corner_verts[tri[1]]];
    const float3 &c = positions[corner_verts[tri[2]]];
    const double vol6 = double(math::dot(a, math::cross(b, c)));
    const int island = island_of_vert[v0];
    volume[island] += vol6;
    moment[island] += vol6 * (double3(a) + double3(b) + double3(c));
  }

  for (const int i : IndexRange(islands_num)) {
    const Span<int> verts = verts_of_island.slice(verts_of_island_offsets[i],
                                                  verts_of_island_offsets[i + 1] -
                                                      verts_of_island_offsets[i]);
    float3 min(FLT_MAX), max(-FLT_MAX), sum(0.0f);
    for (const int v : verts) {
      min = math::min(min, positions[v]);
      max = math::max(max, positions[v]);
      sum += positions[v];
    }
    const float3 extent = math::max(max - min, float3(1e-4f));
    const float size = math::length(max - min);
    Island &island = islands[i];
    island.size = size;
    const double vol = std::abs(volume[i]) / 6.0;
    const double box_volume = double(extent.x) * extent.y * extent.z;
    if (vol > box_volume * 1e-3) {
      const float3 center = float3(moment[i] / (4.0 * volume[i]));
      /* An open piece can put the center far outside, use the mean of the points then. */
      const bool inside = math::reduce_min(center - min) >= -size * 0.01f &&
                          math::reduce_min(max - center) >= -size * 0.01f;
      island.center = inside ? center : sum / float(verts.size());
      island.mass = float(vol);
    }
    else {
      island.center = sum / float(verts.size());
      island.mass = float(box_volume);
    }
    island.mass = std::max(island.mass, 1e-6f);
  }
}

rbCollisionShape *island_shape(const Span<float3> local_positions, const float size)
{
  float3 min(FLT_MAX), max(-FLT_MAX);
  for (const float3 &p : local_positions) {
    min = math::min(min, p);
    max = math::max(max, p);
  }
  const float3 half = math::max((max - min) * 0.5f, float3(1e-4f));
  if (local_positions.size() < 4 || math::reduce_min(max - min) < size * 1e-4f) {
    /* Too few points or flat: the hull can't be made, use the box around the points. */
    rbCollisionShape *shape = RB_shape_new_box(half.x, half.y, half.z);
    RB_shape_set_margin(shape, std::min(math::reduce_min(half), 1e-3f));
    return shape;
  }
  /* The margin is taken off the hull and added back by Bullet, so pieces that touch at the start
   * don't overlap and push each other apart. */
  const float margin = std::clamp(size * 0.02f, 1e-4f, 0.04f);
  bool can_embed = true;
  rbCollisionShape *shape = RB_shape_new_convex_hull(
      reinterpret_cast<const float *>(local_positions.data()),
      sizeof(float3),
      int(local_positions.size()),
      margin,
      &can_embed);
  RB_shape_set_margin(shape, can_embed ? margin : std::max(size * 1e-3f, 1e-5f));
  return shape;
}

rbCollisionShape *ground_shape(const Mesh &ground,
                              const float4x4 &ground_to_space,
                              const float cage)
{
  const Span<int3> tris = ground.corner_tris();
  if (tris.is_empty()) {
    return nullptr;
  }
  const Span<float3> positions = ground.vert_positions();
  const Span<int> corner_verts = ground.corner_verts();
  Array<float3> space_positions(positions.size());
  for (const int i : positions.index_range()) {
    space_positions[i] = math::transform_point(ground_to_space, positions[i]);
  }
  if (cage != 0.0f) {
    /* The cage: pushed out along the normals, so the pieces land that far away. */
    Array<float3> normals(positions.size(), float3(0.0f));
    for (const int3 &tri : tris) {
      const int a = corner_verts[tri[0]];
      const int b = corner_verts[tri[1]];
      const int c = corner_verts[tri[2]];
      /* Weighted by area, so small triangles don't tip the normals. */
      const float3 n = math::cross(space_positions[b] - space_positions[a],
                                   space_positions[c] - space_positions[a]);
      normals[a] += n;
      normals[b] += n;
      normals[c] += n;
    }
    for (const int i : positions.index_range()) {
      space_positions[i] += math::normalize(normals[i]) * cage;
    }
  }
  rbMeshData *data = RB_trimesh_data_new(int(tris.size()), int(positions.size()));
  RB_trimesh_add_vertices(data,
                          reinterpret_cast<float *>(space_positions.data()),
                          int(positions.size()),
                          sizeof(float3));
  for (const int i : tris.index_range()) {
    RB_trimesh_add_triangle_indices(data,
                                    i,
                                    corner_verts[tris[i][0]],
                                    corner_verts[tris[i][1]],
                                    corner_verts[tris[i][2]]);
  }
  RB_trimesh_finish(data);
  rbCollisionShape *shape = RB_shape_new_trimesh(data);
  RB_shape_set_margin(shape, 1e-4f);
  return shape;
}


}  // namespace blender::geometry::rigid_pieces

#endif
