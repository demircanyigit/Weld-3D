/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup geo
 *
 * Rock: a closed mesh is cut by flat cuts in random directions, mostly through its corners, for a
 * low poly rock. Each cut keeps the side with the middle of the shape, so the rock is the shape
 * with its corners and edges split off.
 */

#include <algorithm>
#include <atomic>

#include "BLI_array.hh"
#include "BLI_disjoint_set.hh"
#include "BLI_hash.h"
#include "BLI_math_vector.hh"
#include "BLI_rand.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "BKE_attribute.hh"
#include "BKE_geometry_set.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"

#include "DNA_mesh_types.h"

#include "GEO_join_geometries.hh"
#include "GEO_mesh_copy_selection.hh"
#include "GEO_mesh_rock.hh"

#include "mesh_boolean_manifold.hh"
#include "mesh_fracture_cell.hh"

namespace blender::geometry {

#ifdef WITH_MANIFOLD

/** The convex cell that is left of the box around the piece after all of its cuts. */
static boolean::FractureCell rock_cell(const Span<float3> positions,
                                       const MeshRockParams &params,
                                       const uint32_t seed)
{
  float3 min(FLT_MAX), max(-FLT_MAX);
  for (const float3 &p : positions) {
    min = math::min(min, p);
    max = math::max(max, p);
  }
  const double size = math::length(double3(max) - double3(min));
  /* The box should not touch the mesh, its sides would look like cuts. */
  const double3 margin(size * 0.01 + 1e-4);
  fracture::CellBuilder cell(double3(min) - margin, double3(max) + margin);
  const double epsilon = size * 1e-10;
  const float3 center = (min + max) * 0.5f;
  const float depth = std::clamp(params.depth, 0.0f, 0.95f);
  const float corners = std::clamp(params.corners, 0.0f, 1.0f);

  RandomNumberGenerator rng(seed);
  for (int i = 0; i < params.cuts; i++) {
    const float3 any_direction = rng.get_unit_float3();
    /* Aim at a random point of the shape: on a box, mostly its corners. */
    const float3 to_point = positions[rng.get_int32(int(positions.size()))] - center;
    float3 normal = any_direction;
    if (math::length_squared(to_point) > 1e-12f) {
      normal = math::interpolate(any_direction, math::normalize(to_point), corners);
    }
    if (math::length_squared(normal) < 1e-8f) {
      normal = any_direction;
    }
    normal = math::normalize(normal);
    /* How far the shape reaches in this direction, so each cut takes a share of it. */
    float reach = 0.0f;
    for (const float3 &p : positions) {
      reach = std::max(reach, math::dot(normal, p - center));
    }
    const float keep = 1.0f - depth * (0.4f + 0.6f * rng.get_float());
    const double3 n(normal);
    cell.clip(n, math::dot(n, double3(center)) + double(reach * keep), epsilon);
  }

  boolean::FractureCell result;
  result.point_index = 0;
  for (const fracture::CellFace &face : cell.faces) {
    result.points.extend(face.verts);
    result.normals.append(cell.normals[face.plane]);
  }
  return result;
}

static Mesh *rock_piece(const Mesh &piece,
                        const MeshRockParams &params,
                        const uint32_t seed,
                        MeshRockError *r_error)
{
  const boolean::FractureCell cell = rock_cell(piece.vert_positions(), params, seed);
  if (cell.points.is_empty()) {
    return nullptr;
  }
  Array<int> vert_pieces;
  Vector<int> piece_cells;
  Array<bool> cut_faces;
  boolean::BooleanError error = boolean::BooleanError::NoError;
  Mesh *result = boolean::mesh_fracture_manifold(
      piece, Span(&cell, 1), false, vert_pieces, piece_cells, cut_faces, &error);
  if (result == nullptr) {
    *r_error = error == boolean::BooleanError::NonManifold ? MeshRockError::NonManifold :
                                                             MeshRockError::UnknownError;
    return nullptr;
  }
  if (params.cut_id) {
    bke::SpanAttributeWriter<bool> attribute =
        result->attributes_for_write().lookup_or_add_for_write_only_span<bool>(
            *params.cut_id, bke::AttrDomain::Face);
    attribute.span.copy_from(cut_faces);
    attribute.finish();
  }
  return result;
}

/** The loose parts of the mesh, as the island of each face. */
static int face_islands(const Mesh &mesh, Array<int> &r_face_island)
{
  DisjointSet<int> sets(mesh.verts_num);
  for (const int2 edge : mesh.edges()) {
    sets.join(edge[0], edge[1]);
  }
  Array<int> island_of_root(mesh.verts_num, -1);
  int islands_num = 0;
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  r_face_island.reinitialize(mesh.faces_num);
  for (const int face : faces.index_range()) {
    const int root = sets.find_root(corner_verts[faces[face].first()]);
    if (island_of_root[root] == -1) {
      island_of_root[root] = islands_num++;
    }
    r_face_island[face] = island_of_root[root];
  }
  return islands_num;
}

#endif

Mesh *mesh_rock(const Mesh &mesh, const MeshRockParams &params, MeshRockError *r_error)
{
  *r_error = MeshRockError::NoError;
#ifdef WITH_MANIFOLD
  if (mesh.faces_num == 0) {
    return nullptr;
  }
  const uint32_t base_seed = BLI_hash_int_2d(uint32_t(params.seed), 7919u);

  Array<int> face_island;
  const int islands_num = params.per_island ? face_islands(mesh, face_island) : 1;
  if (islands_num <= 1) {
    return rock_piece(mesh, params, base_seed, r_error);
  }

  Array<Mesh *> pieces(islands_num, nullptr);
  std::atomic<int> error{int(MeshRockError::NoError)};
  threading::parallel_for(IndexRange(islands_num), 1, [&](const IndexRange range) {
    for (const int island : range) {
      Array<bool> selection(mesh.faces_num);
      for (const int face : selection.index_range()) {
        selection[face] = face_island[face] == island;
      }
      const std::optional<Mesh *> copy = mesh_copy_selection(
          mesh, VArray<bool>::from_span(selection), bke::AttrDomain::Face);
      if (!copy || *copy == nullptr) {
        continue;
      }
      MeshRockError piece_error = MeshRockError::NoError;
      Mesh *rock = rock_piece(
          **copy, params, BLI_hash_int_2d(base_seed, uint32_t(island)), &piece_error);
      BKE_id_free(nullptr, *copy);
      if (rock == nullptr) {
        if (piece_error != MeshRockError::NoError) {
          error = int(piece_error);
        }
        continue;
      }
      pieces[island] = rock;
    }
  });
  if (error != int(MeshRockError::NoError)) {
    for (Mesh *piece : pieces) {
      if (piece) {
        BKE_id_free(nullptr, piece);
      }
    }
    *r_error = MeshRockError(int(error));
    return nullptr;
  }
  Array<bke::GeometrySet> geometries(islands_num);
  for (const int island : pieces.index_range()) {
    if (pieces[island]) {
      geometries[island] = bke::GeometrySet::from_mesh(pieces[island]);
    }
  }
  bke::GeometrySet joined = join_geometries(geometries, {});
  return joined.get_component_for_write<bke::MeshComponent>().release();
#else
  UNUSED_VARS(mesh, params);
  *r_error = MeshRockError::SolverNotAvailable;
  return nullptr;
#endif
}

}  // namespace blender::geometry
