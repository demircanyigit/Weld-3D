/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_attribute.hh"
#include "BKE_bvhutils.hh"
#include "BKE_mesh.hh"
#include "BLI_array.hh"
#include "BLI_bounds.hh"
#include "BLI_disjoint_set.hh"
#include "BLI_kdopbvh.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_noise.hh"
#include "DNA_mesh_types.h"
#include "GEO_mesh_blast.hh"
#include "GEO_mesh_copy_selection.hh"
#include <algorithm>
#include <cmath>
#include <exception>
#include <vector>

namespace blender::geometry {
namespace {
struct Island {
  double3 minimum, maximum, origin;
  double measure = 0;
  double correction = 0;
  int faces = 0;
  bool volume_valid = true;
  void accumulate(const double value)
  {
    const double adjusted = value - correction;
    const double next = measure + adjusted;
    correction = (next - measure) - adjusted;
    measure = next;
  }
};
/** True when the point, in the space of the volume mesh, is inside it. */
bool point_inside(const bke::BVHTreeFromMesh &bvh, const float3 &point)
{
  BVHTreeNearest nearest;
  nearest.index = -1;
  nearest.dist_sq = FLT_MAX;
  BLI_bvhtree_find_nearest(bvh.tree,
                           point,
                           &nearest,
                           bvh.nearest_callback,
                           const_cast<bke::BVHTreeFromMesh *>(&bvh));
  if (nearest.index == -1) {
    return false;
  }
  return math::dot(point - float3(nearest.co), float3(nearest.no)) < 0.0f;
}

/** The islands the selection modes remove, by island. */
Array<bool> select_islands(const Mesh &mesh,
                           const MeshBlastParams &params,
                           const Span<int> vertex_island,
                           const int islands_num,
                           std::string &message)
{
  const Span<float3> positions = mesh.vert_positions();
  Array<float3> center(islands_num, float3(0.0f));
  Array<int> verts(islands_num, 0);
  for (const int v : positions.index_range()) {
    center[vertex_island[v]] += positions[v];
    verts[vertex_island[v]]++;
  }
  for (const int i : IndexRange(islands_num)) {
    center[i] /= float(std::max(verts[i], 1));
  }
  /* A random value per island that stays when other islands change. */
  Array<float> random(islands_num);
  for (const int i : IndexRange(islands_num)) {
    random[i] = noise::hash_float_to_float(
        float4(center[i].x, center[i].y, center[i].z, float(params.seed) * 1.618f));
  }
  Array<bool> removed(islands_num, false);
  const auto by_value = [&](const Span<float> value) {
    for (const int i : IndexRange(islands_num)) {
      const float v = value[i] + params.randomness * (random[i] * 2.0f - 1.0f);
      removed[i] = v >= params.amount;
    }
  };
  switch (params.select) {
    case BlastSelect::Size:
      break;
    case BlastSelect::Random:
      for (const int i : IndexRange(islands_num)) {
        removed[i] = random[i] < params.amount;
      }
      break;
    case BlastSelect::Inside: {
      if (params.volume == nullptr || params.volume->faces_num == 0) {
        message = "Choose a mesh object for the volume";
        break;
      }
      const bke::BVHTreeFromMesh bvh = params.volume->bvh_corner_tris();
      if (bvh.tree == nullptr) {
        break;
      }
      const float4x4 to_volume = math::invert(params.volume_transform);
      if (params.inside == BlastInside::Center) {
        for (const int i : IndexRange(islands_num)) {
          removed[i] = point_inside(bvh, math::transform_point(to_volume, center[i]));
        }
        break;
      }
      Array<int> inside(islands_num, 0);
      for (const int v : positions.index_range()) {
        if (point_inside(bvh, math::transform_point(to_volume, positions[v]))) {
          inside[vertex_island[v]]++;
        }
      }
      for (const int i : IndexRange(islands_num)) {
        removed[i] = params.inside == BlastInside::Any ? inside[i] > 0 :
                                                         inside[i] == verts[i] && verts[i] > 0;
      }
      break;
    }
    case BlastSelect::Gradient: {
      Array<float> value(islands_num, 0.0f);
      float3 origin(0.0f);
      float3 axis(0.0f, 0.0f, 1.0f);
      float distance = std::max(params.distance, 1e-6f);
      float start = 0.0f;
      if (params.use_gradient_object) {
        origin = params.gradient_transform.location();
        axis = math::normalize(params.gradient_transform.z_axis());
      }
      else if (const std::optional<Bounds<float3>> bounds = mesh.bounds_min_max()) {
        if (params.gradient == BlastGradient::Linear) {
          start = bounds->min.z;
          distance = std::max(bounds->max.z - bounds->min.z, 1e-6f);
        }
        else {
          origin = (bounds->min + bounds->max) * 0.5f;
          distance = std::max(math::length(bounds->max - bounds->min) * 0.5f, 1e-6f);
        }
      }
      for (const int i : IndexRange(islands_num)) {
        if (params.gradient == BlastGradient::Linear) {
          value[i] = params.use_gradient_object ? math::dot(center[i] - origin, axis) / distance :
                                                  (center[i].z - start) / distance;
        }
        else {
          value[i] = 1.0f - math::distance(center[i], origin) / distance;
        }
      }
      by_value(value);
      break;
    }
    case BlastSelect::Noise: {
      Array<float> value(islands_num);
      const float3 offset = float3(13.7f, 31.1f, 7.3f) * float(params.seed % 10000);
      for (const int i : IndexRange(islands_num)) {
        value[i] = noise::perlin(center[i] * params.noise_scale + offset);
      }
      by_value(value);
      break;
    }
    case BlastSelect::Attribute: {
      const VArraySpan<float> attribute = *mesh.attributes().lookup<float>(
          params.attribute, bke::AttrDomain::Point);
      if (attribute.is_empty()) {
        message = "The mesh has no point attribute \"" + params.attribute + "\"";
        break;
      }
      Array<float> value(islands_num, 0.0f);
      for (const int v : positions.index_range()) {
        value[vertex_island[v]] += attribute[v];
      }
      for (const int i : IndexRange(islands_num)) {
        value[i] /= float(std::max(verts[i], 1));
      }
      by_value(value);
      break;
    }
  }
  return removed;
}

/** The mesh as it is, with the islands that would be removed stored in an attribute. */
Mesh *mark_removed(const Mesh &mesh, const Span<bool> keep, const std::string &name)
{
  const std::string attribute_name = name.empty() ? std::string("blasted") : name;
  Mesh *result = BKE_mesh_copy_for_eval(mesh);
  bke::MutableAttributeAccessor attributes = result->attributes_for_write();
  attributes.remove(attribute_name);
  bke::SpanAttributeWriter<bool> marked = attributes.lookup_or_add_for_write_only_span<bool>(
      attribute_name, bke::AttrDomain::Point);
  if (marked) {
    for (const int v : keep.index_range()) {
      marked.span[v] = !keep[v];
    }
    marked.finish();
  }
  return result;
}

/** The mesh without the vertices that are not kept, an empty mesh when none are. */
Mesh *keep_only(const Mesh &mesh, const Span<bool> keep)
{
  const std::optional<Mesh *> result = mesh_copy_selection(
      mesh, VArray<bool>::from_span(keep), bke::AttrDomain::Point);
  if (!result) {
    return nullptr;
  }
  if (*result) {
    return *result;
  }
  Mesh *empty = BKE_mesh_new_nomain(0, 0, 0, 0);
  BKE_mesh_copy_parameters_for_eval(empty, &mesh);
  return empty;
}
}  // namespace

Mesh *mesh_blast(const Mesh &mesh, const MeshBlastParams &params, std::string &message)
{
  if (mesh.verts_num == 0)
    return nullptr;
  if (!std::isfinite(params.threshold) || params.threshold < 0 || params.keep_count < 0) {
    message = "Blast threshold and island count must be finite and non-negative";
    return nullptr;
  }
  if (params.metric < BlastMetric::FaceCount || params.metric > BlastMetric::BoundsSize ||
      params.filter < BlastFilter::Threshold || params.filter > BlastFilter::ThresholdAndLargest)
  {
    message = "Unsupported Blast measure or filter";
    return nullptr;
  }
  try {
    const Span<float3> positions = mesh.vert_positions();
    const Span<int2> edges = mesh.edges();
    const OffsetIndices<int> faces = mesh.faces();
    const Span<int> corners = mesh.corner_verts();
    DisjointSet<int> components(mesh.verts_num);
    for (const int2 &edge : edges)
      components.join(edge.x, edge.y);
    Array<int> root_to_island(mesh.verts_num, -1);
    Array<int> vertex_island(mesh.verts_num);
    std::vector<Island> islands;
    for (const int vertex : positions.index_range()) {
      const float3 point = positions[vertex];
      if (params.metric != BlastMetric::FaceCount &&
          (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)))
      {
        message = "Input has non-finite coordinates; choose Face Count or repair the input";
        return nullptr;
      }
      const int root = components.find_root(vertex);
      int &index = root_to_island[root];
      if (index == -1) {
        index = int(islands.size());
        islands.push_back({double3(point), double3(point), double3(point)});
      }
      vertex_island[vertex] = index;
      Island &island = islands[index];
      if (params.metric == BlastMetric::BoundsSize) {
        for (int axis = 0; axis < 3; axis++) {
          island.minimum[axis] = std::min(island.minimum[axis], double(point[axis]));
          island.maximum[axis] = std::max(island.maximum[axis], double(point[axis]));
        }
      }
    }
    if (params.select != BlastSelect::Size) {
      const Array<bool> removed = select_islands(
          mesh, params, vertex_island, int(islands.size()), message);
      Array<bool> keep(mesh.verts_num);
      for (const int vertex : keep.index_range()) {
        keep[vertex] = removed[vertex_island[vertex]] == params.invert;
      }
      return params.mark ? mark_removed(mesh, keep, params.mark_name) : keep_only(mesh, keep);
    }
    const bool volume = params.metric == BlastMetric::Volume;
    Array<int> edge_users, edge_orientation;
    if (volume) {
      edge_users.reinitialize(mesh.edges_num);
      edge_users.fill(0);
      edge_orientation.reinitialize(mesh.edges_num);
      edge_orientation.fill(0);
    }
    const Span<int> corner_edges = mesh.corner_edges();
    for (const int face : faces.index_range()) {
      const IndexRange range = faces[face];
      if (range.is_empty())
        continue;
      Island &island = islands[vertex_island[corners[range.first()]]];
      island.faces++;
      if (volume) {
        for (const int corner : range) {
          const int edge = corner_edges[corner];
          edge_users[edge]++;
          edge_orientation[edge] += corners[corner] == edges[edge].x ? 1 : -1;
        }
      }
    }
    if (params.metric == BlastMetric::Area || volume) {
      for (const int3 &tri : mesh.corner_tris()) {
        Island &island = islands[vertex_island[corners[tri.x]]];
        const double3 a = double3(positions[corners[tri.x]]);
        const double3 b = double3(positions[corners[tri.y]]);
        const double3 c = double3(positions[corners[tri.z]]);
        island.accumulate(volume ? math::dot(a - island.origin,
                                             math::cross(b - island.origin, c - island.origin)) /
                                       6.0 :
                                   math::length(math::cross(b - a, c - a)) * 0.5);
      }
    }
    if (volume) {
      for (const int edge : edges.index_range()) {
        if (edge_users[edge] != 2 || edge_orientation[edge] != 0) {
          islands[vertex_island[edges[edge].x]].volume_valid = false;
        }
      }
    }
    std::vector<int> candidates;
    Array<bool> kept(int64_t(islands.size()), false);
    int protected_count = 0;
    for (int index = 0; index < int(islands.size()); index++) {
      Island &island = islands[index];
      switch (params.metric) {
        case BlastMetric::FaceCount:
          island.measure = island.faces;
          break;
        case BlastMetric::Area:
          break;
        case BlastMetric::Volume:
          island.measure = std::abs(island.measure);
          break;
        case BlastMetric::BoundsSize: {
          const double3 size = island.maximum - island.minimum;
          island.measure = std::max({size.x, size.y, size.z});
          break;
        }
      }
      if (volume && (!island.volume_valid || island.faces == 0)) {
        kept[index] = true;
        protected_count++;
        continue;
      }
      if (params.filter == BlastFilter::Largest || island.measure >= params.threshold) {
        candidates.push_back(index);
      }
    }
    int count = int(candidates.size());
    if (params.filter != BlastFilter::Threshold) {
      count = std::min(count, params.keep_count);
      /* A total ordering makes ties deterministic by the first vertex of the island. */
      std::partial_sort(candidates.begin(),
                        candidates.begin() + count,
                        candidates.end(),
                        [&](const int a, const int b) {
                          return islands[a].measure == islands[b].measure ?
                                     a < b :
                                     islands[a].measure > islands[b].measure;
                        });
    }
    for (int i = 0; i < count; i++)
      kept[candidates[i]] = true;
    Array<bool> keep_vertices(mesh.verts_num);
    for (const int vertex : keep_vertices.index_range()) {
      const int index = vertex_island[vertex];
      const bool protected_island = volume &&
                                    (!islands[index].volume_valid || islands[index].faces == 0);
      keep_vertices[vertex] = protected_island || (kept[index] != params.invert);
    }
    if (protected_count) {
      message = "Volume filtering preserved " + std::to_string(protected_count) +
                " open, non-manifold, inconsistently oriented or face-less islands";
    }
    if (params.mark) {
      return mark_removed(mesh, keep_vertices, params.mark_name);
    }
    const std::optional<Mesh *> result = mesh_copy_selection(
        mesh, VArray<bool>::from_span(keep_vertices), bke::AttrDomain::Point);
    if (!result)
      return nullptr;
    if (*result)
      return *result;
    Mesh *empty = BKE_mesh_new_nomain(0, 0, 0, 0);
    BKE_mesh_copy_parameters_for_eval(empty, &mesh);
    return empty;
  }
  catch (const std::exception &error) {
    message = std::string("Blast: ") + error.what();
    return nullptr;
  }
}
}  // namespace blender::geometry
