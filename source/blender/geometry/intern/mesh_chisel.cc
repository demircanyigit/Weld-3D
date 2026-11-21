/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup geo
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>

#include "BLI_array.hh"
#include "BLI_disjoint_set.hh"
#include "BLI_hash.h"
#include "BLI_map.hh"
#include "BLI_math_base.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_rand.hh"
#include "BLI_vector.hh"

#include "DNA_mesh_types.h"

#include "BKE_attribute.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"

#include "GEO_mesh_boolean.hh"
#include "GEO_mesh_chisel.hh"
#include "GEO_mesh_sim_cache.hh"

namespace blender::geometry {

/** Marks the faces of the cutters, so the cut faces can be found in the result. */
static constexpr const char *cut_marker = ".chisel_cut";
/** Material of the cut faces, carried through the boolean, which remaps material indices. */
static constexpr const char *cut_material = ".chisel_material";

namespace {

/** Sharp convex edges in a straight line, one after another. */
struct Run {
  /** The points, in order along the run. */
  Vector<int> verts;
  /** The edge between `verts[i]` and `verts[i + 1]`. */
  Vector<int> edges;
  float3 start;
  float3 direction;
  float length = 0.0f;
  int island = 0;
};

struct Plane {
  float3 normal;
  /** Points x with `dot(normal, x) <= offset` are inside. */
  float offset;
};

/** The point where three planes meet. */
float3 meet(const Plane &a, const Plane &b, const Plane &c)
{
  const float3 bc = math::cross(b.normal, c.normal);
  const float det = math::dot(a.normal, bc);
  if (std::abs(det) < 1e-12f) {
    return float3(0.0f);
  }
  return (bc * a.offset + math::cross(c.normal, a.normal) * b.offset +
          math::cross(a.normal, b.normal) * c.offset) /
         det;
}

Plane plane_through(const float3 &a, const float3 &b, const float3 &c, const float3 &away_from)
{
  float3 normal = math::normalize(math::cross(b - a, c - a));
  if (math::dot(normal, away_from - a) > 0.0f) {
    normal = -normal;
  }
  return {normal, math::dot(normal, a)};
}

/**
 * A cutter: the tetrahedron between the two faces beside an edge and two cut planes. The cut
 * planes go through the edge points `e0` and `e1` and through the points at `depth` into both
 * faces at `s`. The faces are moved out by a little, so the cutter pokes out of the mesh.
 */
Mesh *cutter_mesh(const float3 &e0,
                  const float3 &e1,
                  const float3 &s,
                  const float3 &normal_a,
                  const float3 &normal_b,
                  const float3 &into_a,
                  const float3 &into_b,
                  const float depth,
                  const int material)
{
  const float margin = std::max(depth * 0.25f, 1e-4f);
  const float3 pa = s + into_a * depth;
  const float3 pb = s + into_b * depth;
  const std::array<Plane, 4> planes = {
      plane_through(e0, pa, pb, e1),
      plane_through(e1, pa, pb, e0),
      Plane{normal_a, math::dot(normal_a, e0) + margin},
      Plane{normal_b, math::dot(normal_b, e0) + margin},
  };
  /* Corner i is where the planes other than i meet, so face i is made of the other corners. */
  std::array<float3, 4> corners;
  for (const int i : IndexRange(4)) {
    std::array<int, 3> others;
    int n = 0;
    for (const int j : IndexRange(4)) {
      if (j != i) {
        others[n++] = j;
      }
    }
    corners[i] = meet(planes[others[0]], planes[others[1]], planes[others[2]]);
  }

  Mesh *mesh = BKE_mesh_new_nomain(4, 0, 4, 12);
  mesh->vert_positions_for_write().copy_from(corners);
  MutableSpan<int> face_offsets = mesh->face_offsets_for_write();
  for (const int i : IndexRange(5)) {
    face_offsets[i] = i * 3;
  }
  MutableSpan<int> corner_verts = mesh->corner_verts_for_write();
  for (const int i : IndexRange(4)) {
    std::array<int, 3> tri;
    int n = 0;
    for (const int j : IndexRange(4)) {
      if (j != i) {
        tri[n++] = j;
      }
    }
    /* Wind the face so it faces away from the corner it doesn't have. */
    const float3 face_normal = math::cross(corners[tri[1]] - corners[tri[0]],
                                           corners[tri[2]] - corners[tri[0]]);
    if (math::dot(face_normal, corners[i] - corners[tri[0]]) > 0.0f) {
      std::swap(tri[1], tri[2]);
    }
    corner_verts[i * 3 + 0] = tri[0];
    corner_verts[i * 3 + 1] = tri[1];
    corner_verts[i * 3 + 2] = tri[2];
  }
  bke::mesh_calc_edges(*mesh, false, false);

  bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
  attributes.add<bool>(cut_marker,
                       bke::AttrDomain::Face,
                       bke::AttributeInitVArray(VArray<bool>::from_single(true, 4)));
  attributes.add<int>(cut_material,
                      bke::AttrDomain::Face,
                      bke::AttributeInitVArray(VArray<int>::from_single(material, 4)));
  return mesh;
}

/** Flat UVs on the cut faces, projected along the axis the face looks at most. */
void project_cut_uvs(Mesh &mesh, const Span<bool> cut_faces)
{
  const VectorSet<StringRefNull> uv_names = mesh.uv_map_names();
  if (uv_names.is_empty()) {
    return;
  }
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<float3> positions = mesh.vert_positions();
  const Span<float3> face_normals = mesh.face_normals();
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  for (const StringRefNull name : uv_names) {
    bke::SpanAttributeWriter<float2> uv_map = attributes.lookup_for_write_span<float2>(name);
    if (!uv_map || uv_map.domain != bke::AttrDomain::Corner) {
      continue;
    }
    for (const int face : faces.index_range()) {
      if (!cut_faces[face]) {
        continue;
      }
      const float3 n = math::abs(face_normals[face]);
      const int axis = (n.x > n.y && n.x > n.z) ? 0 : (n.y > n.z ? 1 : 2);
      for (const int corner : faces[face]) {
        const float3 &p = positions[corner_verts[corner]];
        uv_map.span[corner] = axis == 0 ? float2(p.y, p.z) :
                              axis == 1 ? float2(p.x, p.z) :
                                          float2(p.x, p.y);
      }
    }
    uv_map.finish();
  }
}

}  // namespace

static Mesh *mesh_chisel_compute(const Mesh &mesh,
                                 const MeshChiselParams &params,
                                 const char **r_error)
{
  *r_error = nullptr;
  if (mesh.faces_num == 0 || params.amount <= 0.0f || params.depth_max <= 0.0f) {
    return nullptr;
  }
  const Span<float3> positions = mesh.vert_positions();
  const Span<int2> edges = mesh.edges();
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<int> corner_edges = mesh.corner_edges();
  const Span<float3> face_normals = mesh.face_normals();
  const VArraySpan<int> materials = *mesh.attributes().lookup_or_default<int>(
      "material_index", bke::AttrDomain::Face, 0);

  /* The two faces of every edge, -1 when it doesn't have exactly two. */
  Array<int2> edge_faces(mesh.edges_num, int2(-1));
  Array<int> edge_face_count(mesh.edges_num, 0);
  for (const int face : faces.index_range()) {
    for (const int edge : corner_edges.slice(faces[face])) {
      const int count = edge_face_count[edge]++;
      if (count < 2) {
        edge_faces[edge][count] = face;
      }
    }
  }
  Array<float3> face_centers(mesh.faces_num);
  for (const int face : faces.index_range()) {
    float3 sum(0.0f);
    for (const int v : corner_verts.slice(faces[face])) {
      sum += positions[v];
    }
    face_centers[face] = sum / float(faces[face].size());
  }

  /* Sharp convex edges. */
  const float cos_angle = std::cos(params.edge_angle);
  Array<bool> sharp(mesh.edges_num, false);
  for (const int e : edges.index_range()) {
    if (edge_face_count[e] != 2) {
      continue;
    }
    if (!params.edge_selection.is_empty() && !params.edge_selection[e]) {
      continue;
    }
    const int2 f = edge_faces[e];
    const float3 &na = face_normals[f[0]];
    const float3 &nb = face_normals[f[1]];
    if (math::dot(na, nb) > cos_angle) {
      continue;
    }
    const float3 mid = math::midpoint(positions[edges[e][0]], positions[edges[e][1]]);
    /* Convex: the other face is behind this one. */
    if (math::dot(face_centers[f[1]] - mid, na) >= 0.0f) {
      continue;
    }
    sharp[e] = true;
  }

  /* Islands, to tell edges along a board from those across its ends. */
  DisjointSet<int> islands(mesh.verts_num);
  for (const int2 edge : edges) {
    islands.join(edge[0], edge[1]);
  }

  /* Join sharp edges in a straight line into runs. */
  Array<Vector<int>> vert_sharp_edges(mesh.verts_num);
  for (const int e : edges.index_range()) {
    if (sharp[e]) {
      vert_sharp_edges[edges[e][0]].append(e);
      vert_sharp_edges[edges[e][1]].append(e);
    }
  }
  const float straight = std::cos(0.0873f);
  Array<bool> used(mesh.edges_num, false);
  Vector<Run> runs;
  auto other_vert = [&](const int e, const int v) {
    return edges[e][0] == v ? edges[e][1] : edges[e][0];
  };
  for (const int first : edges.index_range()) {
    if (!sharp[first] || used[first]) {
      continue;
    }
    used[first] = true;
    std::deque<int> verts = {edges[first][0], edges[first][1]};
    std::deque<int> run_edges = {first};
    const float3 direction = math::normalize(positions[edges[first][1]] -
                                             positions[edges[first][0]]);
    /* Grow at the back, then at the front. */
    for (const bool back : {true, false}) {
      while (true) {
        const int end = back ? verts.back() : verts.front();
        const float3 way = back ? direction : -direction;
        int best = -1;
        float best_dot = straight;
        for (const int e : vert_sharp_edges[end]) {
          if (used[e]) {
            continue;
          }
          const float3 d = math::normalize(positions[other_vert(e, end)] - positions[end]);
          const float dot = math::dot(d, way);
          if (dot > best_dot) {
            best_dot = dot;
            best = e;
          }
        }
        if (best == -1) {
          break;
        }
        used[best] = true;
        if (back) {
          verts.push_back(other_vert(best, end));
          run_edges.push_back(best);
        }
        else {
          verts.push_front(other_vert(best, end));
          run_edges.push_front(best);
        }
      }
    }
    Run run;
    for (const int v : verts) {
      run.verts.append(v);
    }
    for (const int e : run_edges) {
      run.edges.append(e);
    }
    run.start = positions[run.verts.first()];
    const float3 span = positions[run.verts.last()] - run.start;
    run.length = math::length(span);
    if (run.length < 1e-6f) {
      continue;
    }
    run.direction = span / run.length;
    run.island = islands.find_root(run.verts.first());
    runs.append(std::move(run));
  }
  if (runs.is_empty()) {
    return nullptr;
  }

  /* The way each board goes: its longest run. */
  Map<int, float3> island_direction;
  {
    Map<int, float> island_longest;
    for (const Run &run : runs) {
      float &longest = island_longest.lookup_or_add(run.island, 0.0f);
      if (run.length > longest) {
        longest = run.length;
        island_direction.add_overwrite(run.island, run.direction);
      }
    }
  }

  /* Place the cuts. */
  Vector<Mesh *> cutters;
  for (const int run_index : runs.index_range()) {
    const Run &run = runs[run_index];
    if (!params.ends &&
        std::abs(math::dot(run.direction, island_direction.lookup(run.island))) < 0.7f)
    {
      continue;
    }
    RandomNumberGenerator rng(BLI_hash_int_2d(uint(run_index), uint(params.seed)));
    const float expected = run.length * params.amount;
    const int wanted = int(expected) + (rng.get_float() < expected - std::floor(expected) ? 1 : 0);
    Vector<float2> taken;
    int placed = 0;
    for (int attempt = 0; attempt < wanted * 8 && placed < wanted; attempt++) {
      const bool notch = rng.get_float() < params.mix;
      const float depth = math::interpolate(params.depth_min, params.depth_max, rng.get_float());
      /* Long enough that the sides of a chip slope gently instead of dropping steeply. */
      const float length = notch ? depth * (4.0f + 3.0f * rng.get_float()) :
                                   math::interpolate(
                                       params.length_min, params.length_max, rng.get_float());
      /* Keep away from the ends of the run, where the cuts would meet other edges' cuts. */
      const float margin = depth * 1.5f;
      if (length <= 0.0f || length + 2.0f * margin > run.length) {
        continue;
      }
      const float start = margin + rng.get_float() * (run.length - 2.0f * margin - length);
      const float end = start + length;
      const float gap = depth;
      bool free = true;
      for (const float2 &t : taken) {
        if (start < t[1] + gap && end > t[0] - gap) {
          free = false;
          break;
        }
      }
      if (!free) {
        continue;
      }
      taken.append(float2(start, end));
      placed++;

      /* The cut point, and the faces beside the edge there. */
      /* The deepest point. A slant runs out over its last part instead of ending in a wall. */
      const bool step_at_end = rng.get_float() < 0.5f;
      const float peak = 0.7f + 0.15f * rng.get_float();
      const float s_along = notch ? (start + end) * 0.5f :
                                    (step_at_end ? start + length * peak : end - length * peak);
      int edge = run.edges.first();
      for (const int i : run.edges.index_range()) {
        const float along = math::dot(positions[run.verts[i + 1]] - run.start, run.direction);
        edge = run.edges[i];
        if (along >= s_along) {
          break;
        }
      }
      const int2 f = edge_faces[edge];
      const float3 &na = face_normals[f[0]];
      const float3 &nb = face_normals[f[1]];
      const float3 into_a = math::normalize(-nb + na * math::dot(na, nb));
      const float3 into_b = math::normalize(-na + nb * math::dot(na, nb));
      /* Points on the edge line where it is cut, the line through the edge in the run. */
      const float3 edge_point = positions[edges[edge][0]];
      auto on_edge = [&](const float along) {
        const float3 p = run.start + run.direction * along;
        /* Snap onto the edge line, the run may bend a tiny bit. */
        const float3 d = math::normalize(positions[edges[edge][1]] - edge_point);
        return edge_point + d * math::dot(p - edge_point, d);
      };
      cutters.append(cutter_mesh(on_edge(start),
                                 on_edge(end),
                                 on_edge(s_along),
                                 na,
                                 nb,
                                 into_a,
                                 into_b,
                                 depth,
                                 params.material_index >= 0 ? params.material_index :
                                                              materials[f[0]]));
    }
  }
  if (cutters.is_empty()) {
    return nullptr;
  }

  /* Cut them all away at once. */
  Vector<const Mesh *> operands;
  operands.append(&mesh);
  for (const Mesh *cutter : cutters) {
    operands.append(cutter);
  }
  Array<float4x4> transforms(operands.size(), float4x4::identity());
  boolean::BooleanOpParameters op_params;
  op_params.boolean_mode = boolean::Operation::Difference;
#ifdef WITH_MANIFOLD
  const boolean::Solver solver = boolean::Solver::Manifold;
#else
  const boolean::Solver solver = boolean::Solver::MeshArr;
#endif
  boolean::BooleanError error = boolean::BooleanError::NoError;
  Mesh *result = boolean::mesh_boolean(
      operands, transforms, {}, op_params, solver, nullptr, &error);
  for (Mesh *cutter : cutters) {
    BKE_id_free(nullptr, cutter);
  }
  if (result == nullptr || error != boolean::BooleanError::NoError) {
    if (result != nullptr) {
      BKE_id_free(nullptr, result);
    }
    *r_error = error == boolean::BooleanError::NonManifold ?
                   "The mesh needs to be closed, with no holes" :
                   "The cuts couldn't be made";
    return nullptr;
  }

  /* The cut faces: flat, their own UVs and the asked for attribute. */
  bke::MutableAttributeAccessor attributes = result->attributes_for_write();
  const Array<bool> cut_faces = [&]() {
    Array<bool> values(result->faces_num, false);
    if (const VArray<bool> marker = *attributes.lookup<bool>(cut_marker, bke::AttrDomain::Face)) {
      marker.materialize(values);
    }
    return values;
  }();
  attributes.remove(cut_marker);
  if (const VArray<int> cut_materials = *attributes.lookup<int>(cut_material,
                                                                 bke::AttrDomain::Face))
  {
    bke::SpanAttributeWriter<int> material_index =
        attributes.lookup_or_add_for_write_span<int>("material_index", bke::AttrDomain::Face);
    for (const int i : cut_faces.index_range()) {
      if (cut_faces[i]) {
        material_index.span[i] = cut_materials[i];
      }
    }
    material_index.finish();
  }
  attributes.remove(cut_material);
  bke::SpanAttributeWriter<bool> sharp_faces = attributes.lookup_or_add_for_write_span<bool>(
      "sharp_face", bke::AttrDomain::Face);
  if (sharp_faces) {
    for (const int i : cut_faces.index_range()) {
      if (cut_faces[i]) {
        sharp_faces.span[i] = true;
      }
    }
    sharp_faces.finish();
  }
  project_cut_uvs(*result, cut_faces);
  if (params.cut_id) {
    attributes.remove(*params.cut_id);
    attributes.add<bool>(*params.cut_id,
                         bke::AttrDomain::Face,
                         bke::AttributeInitVArray(VArray<bool>::from_span(cut_faces)));
  }
  return result;
}

Mesh *mesh_chisel(const Mesh &mesh, const MeshChiselParams &params, const char **r_error)
{
  /* A boolean with many cutters is slow, keep the result while nothing it depends on changes. */
  SimCacheHasher hasher("mesh_chisel");
  hasher.add_mesh(mesh);
  hasher.add(params.amount);
  hasher.add(params.mix);
  hasher.add(params.length_min);
  hasher.add(params.length_max);
  hasher.add(params.depth_min);
  hasher.add(params.depth_max);
  hasher.add(params.edge_angle);
  hasher.add(params.ends);
  hasher.add(params.seed);
  hasher.add_span(params.edge_selection);
  hasher.add(params.material_index);
  hasher.add_string(params.cut_id ? StringRef(*params.cut_id) : StringRef());
  return sim_cache_get(
      hasher.finish(),
      mesh,
      [&](const char **r_compute_error) {
        return mesh_chisel_compute(mesh, params, r_compute_error);
      },
      r_error);
}

}  // namespace blender::geometry
