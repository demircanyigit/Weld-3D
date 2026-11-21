/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup geo
 *
 * Wrap: straps, tubes and ropes laid tightly over a surface along drawn or generated lines.
 *
 * 1. Lines come from the loose edges of the mesh and from a pattern around the target.
 * 2. Each line is resampled, pulled onto the surface, fitted over corners and tightened.
 * 3. Where lines cross, the later one lifts over the earlier one.
 * 4. A profile is swept along every line, with its ends cut flat, slanted or notched.
 */

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "BLI_array.hh"
#include "BLI_kdopbvh.hh"
#include "BLI_map.hh"
#include "BLI_math_base.h"
#include "BLI_math_base.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_rand.hh"
#include "BLI_vector.hh"

#include "DNA_mesh_types.h"

#include "BKE_attribute.hh"
#include "BKE_bvhutils.hh"
#include "BKE_mesh.hh"

#include "GEO_mesh_wrap.hh"

namespace blender::geometry {

namespace {

/** The target surface, searched in its own space, answering in the space of the lines. */
struct Surface {
  bke::BVHTreeFromMesh bvh;
  Span<float3> positions;
  Span<int> corner_verts;
  Span<int3> tris;
  Span<float3> vert_normals;
  float4x4 to_space;
  float4x4 from_space;
  float3x3 normal_to_space;

  explicit Surface(const Mesh &mesh, const float4x4 &target_to_space)
      : bvh(mesh.bvh_corner_tris()),
        positions(mesh.vert_positions()),
        corner_verts(mesh.corner_verts()),
        tris(mesh.corner_tris()),
        vert_normals(mesh.vert_normals()),
        to_space(target_to_space),
        from_space(math::invert(target_to_space)),
        normal_to_space(math::transpose(math::invert(float3x3(target_to_space))))
  {
  }
};

struct Hit {
  float3 co;
  float3 no;
};

/** Smooth normal at a point of a triangle, in target space. */
float3 tri_normal(const Surface &s, const int tri, const float3 &co)
{
  const int3 &t = s.tris[tri];
  const int v0 = s.corner_verts[t[0]];
  const int v1 = s.corner_verts[t[1]];
  const int v2 = s.corner_verts[t[2]];
  float w[3];
  interp_weights_tri_v3(w, s.positions[v0], s.positions[v1], s.positions[v2], co);
  return s.vert_normals[v0] * w[0] + s.vert_normals[v1] * w[1] + s.vert_normals[v2] * w[2];
}

Hit to_space(const Surface &s, const float3 &co, const float3 &no)
{
  const float3 n = math::transform_direction(s.normal_to_space, no);
  const float len = math::length(n);
  return {math::transform_point(s.to_space, co), len > 1e-12f ? n / len : float3(0, 0, 1)};
}

/** Nearest point of the surface to a point in line space. */
Hit nearest(const Surface &s, const float3 &p)
{
  const float3 q = math::transform_point(s.from_space, p);
  BVHTreeNearest n;
  n.index = -1;
  n.dist_sq = FLT_MAX;
  BLI_bvhtree_find_nearest(s.bvh.tree,
                           q,
                           &n,
                           s.bvh.nearest_callback,
                           const_cast<bke::BVHTreeFromMesh *>(&s.bvh));
  if (n.index < 0) {
    return {p, float3(0, 0, 1)};
  }
  return to_space(s, float3(n.co), tri_normal(s, n.index, float3(n.co)));
}

/** First hit of a ray in target space, or the nearest point to its start. */
Hit ray(const Surface &s, const float3 &origin, const float3 &dir)
{
  BVHTreeRayHit hit;
  hit.index = -1;
  hit.dist = FLT_MAX;
  BLI_bvhtree_ray_cast(s.bvh.tree,
                       origin,
                       dir,
                       0.0f,
                       &hit,
                       s.bvh.raycast_callback,
                       const_cast<bke::BVHTreeFromMesh *>(&s.bvh));
  if (hit.index < 0) {
    return nearest(s, math::transform_point(s.to_space, origin));
  }
  return to_space(s, float3(hit.co), tri_normal(s, hit.index, float3(hit.co)));
}

/** True when the point is under the surface at the hit. */
bool is_under(const Hit &h, const float3 &p, const float tolerance)
{
  return math::dot(p - h.co, h.no) < -tolerance;
}

struct Chain {
  Vector<float3> points;
  Vector<float> pin;
  bool cyclic = false;
};

/** Chains of the loose edges, split at forks and ends. Closed loops stay closed. */
Vector<Chain> chains_from_edges(const Mesh &mesh, const Span<bool> pin)
{
  Vector<Chain> chains;
  const bke::LooseEdgeCache &loose = mesh.loose_edges();
  if (loose.count == 0) {
    return chains;
  }
  const Span<int2> edges = mesh.edges();
  const Span<float3> positions = mesh.vert_positions();
  Array<Vector<int, 2>> vert_edges(mesh.verts_num);
  for (const int e : edges.index_range()) {
    if (loose.is_loose_bits[e]) {
      vert_edges[edges[e][0]].append(e);
      vert_edges[edges[e][1]].append(e);
    }
  }
  Array<bool> used(edges.size(), false);
  auto pin_of = [&](const int v) { return pin.is_empty() ? 0.0f : (pin[v] ? 1.0f : 0.0f); };
  auto walk = [&](const int start, const int first_edge) {
    Chain chain;
    chain.points.append(positions[start]);
    chain.pin.append(pin_of(start));
    int v = start;
    int e = first_edge;
    while (true) {
      used[e] = true;
      const int next = edges[e][0] == v ? edges[e][1] : edges[e][0];
      if (next == start) {
        chain.cyclic = true;
        break;
      }
      chain.points.append(positions[next]);
      chain.pin.append(pin_of(next));
      v = next;
      if (vert_edges[v].size() != 2) {
        break;
      }
      const int other = vert_edges[v][0] == e ? vert_edges[v][1] : vert_edges[v][0];
      if (used[other]) {
        break;
      }
      e = other;
    }
    if (chain.points.size() >= 2) {
      chains.append(std::move(chain));
    }
  };
  for (const int v : vert_edges.index_range()) {
    if (vert_edges[v].is_empty() || vert_edges[v].size() == 2) {
      continue;
    }
    for (const int e : vert_edges[v]) {
      if (!used[e]) {
        walk(v, e);
      }
    }
  }
  for (const int e : edges.index_range()) {
    if (loose.is_loose_bits[e] && !used[e]) {
      walk(edges[e][0], e);
    }
  }
  return chains;
}

/** Lines of a pattern around an axis of the target, laid on its surface, in line space. */
Vector<Chain> chains_from_pattern(const Mesh &target,
                                  const Surface &surface,
                                  const MeshWrapParams &params)
{
  Vector<Chain> chains;
  const std::optional<Bounds<float3>> bounds = target.bounds_min_max();
  if (!bounds) {
    return chains;
  }
  const float3 size = bounds->max - bounds->min;
  int axis = params.axis - 1;
  if (axis < 0 || axis > 2) {
    axis = size.x >= size.y && size.x >= size.z ? 0 : (size.y >= size.z ? 1 : 2);
  }
  float3 w(0.0f), u(0.0f), v(0.0f);
  w[axis] = 1.0f;
  u[(axis + 1) % 3] = 1.0f;
  v[(axis + 2) % 3] = 1.0f;
  const float3 center = (bounds->min + bounds->max) * 0.5f;
  const float lo = bounds->min[axis];
  const float hi = bounds->max[axis];
  const float span = std::max(hi - lo, 1e-6f);
  const float radius = std::max(
      0.5f * std::max(size[(axis + 1) % 3], size[(axis + 2) % 3]), 1e-6f);
  /* Strap width measured in target space. */
  const float3x3 m = float3x3(surface.to_space);
  const float space_scale = std::max(
      (math::length(m.x) + math::length(m.y) + math::length(m.z)) / 3.0f, 1e-6f);
  const float width_t = params.width / space_scale;

  RandomNumberGenerator rng(uint32_t(params.seed) * 2654435761u + 17u);
  const float random = std::max(params.random, 0.0f);
  constexpr int points_per_turn = 48;

  /* One line given by angles and heights (as shares of the span). */
  struct Line {
    Vector<float2> angle_height;
    bool cyclic = false;
  };
  Vector<Line> lines;

  auto spiral = [&](const float phase, const float turns, const float f0, const float f1) {
    Line line;
    const int n = std::max(int(std::ceil(std::abs(turns) * points_per_turn)), 2);
    for (int i = 0; i <= n; i++) {
      const float t = float(i) / float(n);
      line.angle_height.append({phase + float(2.0 * M_PI) * turns * t, f0 + (f1 - f0) * t});
    }
    lines.append(std::move(line));
  };
  auto ring = [&](const float phase, const float f) {
    Line line;
    line.cyclic = true;
    for (int i = 0; i < points_per_turn; i++) {
      line.angle_height.append({phase + float(2.0 * M_PI) * float(i) / points_per_turn, f});
    }
    lines.append(std::move(line));
  };
  auto jitter = [&]() { return (rng.get_float() - 0.5f) * random; };
  auto even = [&](const int k, const int n) {
    return n <= 1 ? 0.5f * (params.start + params.end) :
                    params.start + (params.end - params.start) * float(k) / float(n - 1);
  };

  const int count = std::max(params.count, 1);
  /* Each line gets its own tilt and wobble phases. */
  Vector<float> tilts;
  Vector<float> tilt_phases;
  switch (params.pattern) {
    case WrapPattern::Spiral:
    case WrapPattern::Cross: {
      for (int k = 0; k < count; k++) {
        const float phase = float(2.0 * M_PI) * float(k) / float(count) + jitter() * 2.0f;
        const float f0 = params.start + jitter() * 0.1f;
        const float f1 = params.end + jitter() * 0.1f;
        const float turns = params.turns * (1.0f + jitter() * 0.3f);
        spiral(phase, turns, f0, f1);
        if (params.pattern == WrapPattern::Cross) {
          spiral(phase, -turns, f0, f1);
        }
      }
      break;
    }
    case WrapPattern::Rings: {
      for (int k = 0; k < count; k++) {
        ring(rng.get_float() * float(2.0 * M_PI), even(k, count) + jitter() * 0.1f);
      }
      break;
    }
    case WrapPattern::Bands: {
      const int group = std::max(params.group, 1);
      const float gap = params.spacing * width_t / span;
      for (int k = 0; k < count; k++) {
        const float f = even(k, count) + jitter() * 0.1f;
        for (int j = 0; j < group; j++) {
          ring(rng.get_float() * float(2.0 * M_PI), f + (float(j) - 0.5f * float(group - 1)) * gap);
        }
      }
      break;
    }
    case WrapPattern::Random: {
      for (int k = 0; k < count; k++) {
        const float f = params.start + (params.end - params.start) * rng.get_float();
        const float turns = (0.6f + rng.get_float() * std::max(params.turns, 0.0f)) *
                            (rng.get_float() < 0.5f ? -1.0f : 1.0f);
        const float rise = (rng.get_float() - 0.5f) * (0.05f + random * 0.3f);
        spiral(rng.get_float() * float(2.0 * M_PI), turns, f - rise * 0.5f, f + rise * 0.5f);
      }
      break;
    }
  }
  for ([[maybe_unused]] const Line &line : lines) {
    const float spread = params.pattern == WrapPattern::Random ? 0.3f + random : random;
    tilts.append(params.tilt + (rng.get_float() - 0.5f) * spread);
    tilt_phases.append(rng.get_float() * float(2.0 * M_PI));
  }

  const float wobble = params.wobble * width_t / span;
  const float tilt_scale = radius / span;
  for (const int l : lines.index_range()) {
    const Line &line = lines[l];
    const float tilt = std::tan(std::clamp(tilts[l], -1.4f, 1.4f)) * tilt_scale;
    const float p1 = rng.get_float() * float(2.0 * M_PI);
    const float p2 = rng.get_float() * float(2.0 * M_PI);
    Chain chain;
    chain.cyclic = line.cyclic;
    for (const float2 &ah : line.angle_height) {
      const float a = ah.x;
      float f = ah.y + tilt * std::sin(a - tilt_phases[l]);
      f += wobble * (0.6f * std::sin(3.0f * a + p1) + 0.4f * std::sin(7.0f * a + p2));
      const float3 out = u * std::cos(a) + v * std::sin(a);
      float3 origin = center + out * (radius * 3.0f);
      origin[axis] = lo + f * span;
      const Hit h = ray(surface, origin, -out);
      chain.points.append(h.co);
      chain.pin.append(0.0f);
    }
    chains.append(std::move(chain));
  }
  return chains;
}

/** Evenly spaced points along the chain, about `step` apart. */
void resample(Chain &chain, const float step)
{
  const int n = chain.points.size();
  const int segs = chain.cyclic ? n : n - 1;
  Array<float> cum(segs + 1);
  cum[0] = 0.0f;
  for (int i = 0; i < segs; i++) {
    cum[i + 1] = cum[i] + math::distance(chain.points[i], chain.points[(i + 1) % n]);
  }
  const float length = cum[segs];
  if (length < 1e-7f) {
    chain.points.clear();
    return;
  }
  const int count = std::max(int(std::ceil(length / std::max(step, 1e-5f))),
                             chain.cyclic ? 3 : 1);
  const int out_num = chain.cyclic ? count : count + 1;
  Vector<float3> points(out_num);
  Vector<float> pin(out_num);
  int seg = 0;
  for (int j = 0; j < out_num; j++) {
    const float s = length * float(j) / float(count);
    while (seg < segs - 1 && cum[seg + 1] < s) {
      seg++;
    }
    const float seg_len = cum[seg + 1] - cum[seg];
    const float t = seg_len > 1e-9f ? std::clamp((s - cum[seg]) / seg_len, 0.0f, 1.0f) : 0.0f;
    const int a = seg;
    const int b = (seg + 1) % n;
    points[j] = math::interpolate(chain.points[a], chain.points[b], t);
    pin[j] = math::interpolate(chain.pin[a], chain.pin[b], t);
  }
  chain.points = std::move(points);
  chain.pin = std::move(pin);
}

/** A chain on the surface: points, their surface normals and whether they are pinned. */
struct Laid {
  Vector<float3> surf;
  Vector<float3> normal;
  Vector<bool> pinned;
  bool cyclic = false;

  int size() const
  {
    return surf.size();
  }
};

Laid lay(const Surface &surface, const Chain &chain, const float step, const float tight)
{
  Laid laid;
  laid.cyclic = chain.cyclic;
  for (const int i : chain.points.index_range()) {
    const bool pinned = chain.pin[i] > 0.5f;
    const Hit h = nearest(surface, chain.points[i]);
    laid.surf.append(pinned ? chain.points[i] : h.co);
    laid.normal.append(h.no);
    laid.pinned.append(pinned);
  }

  /* Fit over corners: where a segment dips under the surface, add its middle, laid on it. */
  const float tolerance = 0.05f * step;
  for (int pass = 0; pass < 3; pass++) {
    Laid fitted;
    fitted.cyclic = laid.cyclic;
    bool changed = false;
    const int n = laid.size();
    for (int i = 0; i < n; i++) {
      fitted.surf.append(laid.surf[i]);
      fitted.normal.append(laid.normal[i]);
      fitted.pinned.append(laid.pinned[i]);
      if (!laid.cyclic && i == n - 1) {
        break;
      }
      const int j = (i + 1) % n;
      if (laid.pinned[i] || laid.pinned[j]) {
        continue;
      }
      const float3 mid = (laid.surf[i] + laid.surf[j]) * 0.5f;
      const Hit h = nearest(surface, mid);
      if (is_under(h, mid, tolerance)) {
        fitted.surf.append(h.co);
        fitted.normal.append(h.no);
        fitted.pinned.append(false);
        changed = true;
      }
    }
    laid = std::move(fitted);
    if (!changed) {
      break;
    }
  }

  /* Tighten: relax along the chain, but never into the surface, so dips are bridged. */
  if (tight > 0.0f) {
    const float factor = std::min(tight, 1.0f) * 0.5f;
    const int n = laid.size();
    for (int iter = 0; iter < 16; iter++) {
      Vector<float3> next = laid.surf;
      for (int i = 0; i < n; i++) {
        if (laid.pinned[i] || (!laid.cyclic && (i == 0 || i == n - 1))) {
          continue;
        }
        const float3 avg = (laid.surf[(i + n - 1) % n] + laid.surf[(i + 1) % n]) * 0.5f;
        float3 q = math::interpolate(laid.surf[i], avg, factor);
        const Hit h = nearest(surface, q);
        if (is_under(h, q, 0.0f)) {
          q = h.co;
        }
        next[i] = q;
        laid.normal[i] = h.no;
      }
      laid.surf = std::move(next);
    }
  }

  /* Smooth the normals a little so the strap does not flip on small facets. */
  const int n = laid.size();
  for (int pass = 0; pass < 2; pass++) {
    Vector<float3> next = laid.normal;
    for (int i = 0; i < n; i++) {
      float3 sum = laid.normal[i] * 2.0f;
      if (laid.cyclic || i > 0) {
        sum += laid.normal[(i + n - 1) % n];
      }
      if (laid.cyclic || i < n - 1) {
        sum += laid.normal[(i + 1) % n];
      }
      next[i] = math::normalize(sum);
    }
    laid.normal = std::move(next);
  }
  return laid;
}

/** Arc length at every point, and the total at the end (with the closing segment if cyclic). */
Array<float> arc_lengths(const Span<float3> points, const bool cyclic)
{
  const int n = points.size();
  Array<float> cum(n + 1);
  cum[0] = 0.0f;
  for (int i = 1; i < n; i++) {
    cum[i] = cum[i - 1] + math::distance(points[i - 1], points[i]);
  }
  cum[n] = cum[n - 1] + (cyclic ? math::distance(points[n - 1], points[0]) : 0.0f);
  return cum;
}

/** Per point, how much it lifts over other straps where they cross: 0 to 1. */
Vector<Array<float>> crossing_lifts(const Span<Laid> laid,
                                    const Span<Array<float>> cums,
                                    const float width,
                                    const bool flip)
{
  Vector<Array<float>> lifts;
  for (const Laid &l : laid) {
    lifts.append(Array<float>(l.size(), 0.0f));
  }
  const float cell = std::max(width, 1e-5f);
  Map<int3, Vector<int2>> grid;
  auto key = [&](const float3 &p) {
    return int3(int(std::floor(p.x / cell)), int(std::floor(p.y / cell)), int(std::floor(p.z / cell)));
  };
  for (const int c : laid.index_range()) {
    for (const int i : IndexRange(laid[c].size())) {
      grid.lookup_or_add_default(key(laid[c].surf[i])).append({c, i});
    }
  }
  auto tangent = [&](const Laid &l, const int i) {
    const int n = l.size();
    const int a = l.cyclic ? (i + n - 1) % n : std::max(i - 1, 0);
    const int b = l.cyclic ? (i + 1) % n : std::min(i + 1, n - 1);
    return math::normalize(l.surf[b] - l.surf[a]);
  };
  const float radius = width * 0.75f;
  for (const int c : laid.index_range()) {
    const Laid &l = laid[c];
    Array<bool> over(l.size(), false);
    for (const int i : IndexRange(l.size())) {
      const float3 &p = l.surf[i];
      const int3 k = key(p);
      const float3 t = tangent(l, i);
      bool found = false;
      for (int dx = -1; dx <= 1 && !found; dx++) {
        for (int dy = -1; dy <= 1 && !found; dy++) {
          for (int dz = -1; dz <= 1 && !found; dz++) {
            const Vector<int2> *cell_items = grid.lookup_ptr(k + int3(dx, dy, dz));
            if (cell_items == nullptr) {
              continue;
            }
            for (const int2 &item : *cell_items) {
              const int c2 = item.x;
              const int j = item.y;
              if (c2 == c) {
                /* The same strap further back: far enough along it to be a real crossing. */
                float along = std::abs(cums[c][i] - cums[c][j]);
                if (l.cyclic) {
                  along = std::min(along, cums[c][l.size()] - along);
                }
                if (along < width * 3.0f) {
                  continue;
                }
              }
              const bool later = flip ? (c2 > c || (c2 == c && j > i)) :
                                        (c2 < c || (c2 == c && j < i));
              if (!later) {
                continue;
              }
              if (math::distance(p, laid[c2].surf[j]) > radius) {
                continue;
              }
              /* Side by side straps of a spiral do not cross. */
              if (std::abs(math::dot(t, tangent(laid[c2], j))) > 0.85f) {
                continue;
              }
              found = true;
              break;
            }
          }
        }
      }
      over[i] = found;
    }
    /* Ease the lift in and out over about a strap width on each side. */
    const float reach = width * 1.5f;
    const float total = cums[c][l.size()];
    Array<float> &lift = lifts[c];
    for (const int i : IndexRange(l.size())) {
      if (!over[i]) {
        continue;
      }
      for (const int j : IndexRange(l.size())) {
        float d = std::abs(cums[c][i] - cums[c][j]);
        if (l.cyclic) {
          d = std::min(d, total - d);
        }
        if (d >= reach) {
          continue;
        }
        const float x = 1.0f - d / reach;
        lift[j] = std::max(lift[j], x * x * (3.0f - 2.0f * x));
      }
    }
  }
  return lifts;
}

/** A point along a finished strap path, used to place every profile point. */
struct PathPoint {
  float3 center;
  float3 side;
  float3 up;
  float3 tangent;
  float width_mul;
};

struct Path {
  Vector<PathPoint> points;
  Array<float> cum;
  bool cyclic = false;

  float length() const
  {
    return cum.last();
  }

  /** The path at an arc length, between its points. */
  PathPoint at(const float s) const
  {
    const int n = points.size();
    if (s <= 0.0f) {
      return points[0];
    }
    if (s >= cum[n - 1] && !cyclic) {
      return points[n - 1];
    }
    const int seg = std::min(int(std::upper_bound(cum.begin(), cum.begin() + n, s) - cum.begin()) - 1,
                             n - 1);
    const int next = (seg + 1) % n;
    const float seg_len = cum[seg + 1] - cum[seg];
    const float t = seg_len > 1e-9f ? std::clamp((s - cum[seg]) / seg_len, 0.0f, 1.0f) : 0.0f;
    const PathPoint &a = points[seg];
    const PathPoint &b = points[next];
    PathPoint r;
    r.center = math::interpolate(a.center, b.center, t);
    r.side = math::normalize(math::interpolate(a.side, b.side, t));
    r.up = math::normalize(math::interpolate(a.up, b.up, t));
    r.tangent = math::normalize(math::interpolate(a.tangent, b.tangent, t));
    r.width_mul = math::interpolate(a.width_mul, b.width_mul, t);
    return r;
  }
};

Path build_path(const Laid &laid,
                const Span<float> lift,
                const MeshWrapParams &params,
                const int chain_index)
{
  const int n = laid.size();
  const bool strap = params.shape == WrapShape::Strap;
  const float base = params.gap + (strap ? params.thickness * 0.5f : params.width * 0.5f);
  const float lift_amount = strap ? params.thickness * 1.05f : params.width * 0.9f;

  Path path;
  path.cyclic = laid.cyclic;
  Vector<float3> centers(n);
  for (int i = 0; i < n; i++) {
    const float offset = (laid.pinned[i] ? 0.0f : base) + lift[i] * lift_amount;
    centers[i] = laid.surf[i] + laid.normal[i] * offset;
  }
  path.cum = arc_lengths(centers, laid.cyclic);

  RandomNumberGenerator rng(uint32_t(params.seed) * 747796405u + uint32_t(chain_index) * 2891336453u);
  const float p1 = rng.get_float() * float(2.0 * M_PI);
  const float p2 = rng.get_float() * float(2.0 * M_PI);
  const float w = std::max(params.width, 1e-5f);
  const float f1 = float(2.0 * M_PI) / (8.0f * w);
  const float f2 = float(2.0 * M_PI) / (3.0f * w);

  for (int i = 0; i < n; i++) {
    const int a = laid.cyclic ? (i + n - 1) % n : std::max(i - 1, 0);
    const int b = laid.cyclic ? (i + 1) % n : std::min(i + 1, n - 1);
    float3 t = centers[b] - centers[a];
    t = math::length(t) > 1e-9f ? math::normalize(t) : float3(1, 0, 0);
    float3 up = laid.normal[i] - t * math::dot(laid.normal[i], t);
    if (math::length(up) < 1e-6f) {
      up = math::cross(t, std::abs(t.z) < 0.9f ? float3(0, 0, 1) : float3(1, 0, 0));
    }
    up = math::normalize(up);
    PathPoint p;
    p.center = centers[i];
    p.tangent = t;
    p.up = up;
    p.side = math::normalize(math::cross(t, up));
    const float s = path.cum[i];
    const float noise = 0.6f * std::sin(s * f1 + p1) + 0.4f * std::sin(s * f2 + p2);
    p.width_mul = std::max(1.0f + params.width_random * noise, 0.2f);
    path.points.append(p);
  }
  return path;
}

/** Collects the swept mesh. */
struct Builder {
  Vector<float3> positions;
  Vector<int> face_sizes;
  Vector<int> corner_verts;
  Vector<float2> corner_uvs;
  /** Per vertex: a column id when the vertex is on a hard lengthwise edge, else -1. */
  Vector<int> hard_column;
  /** Per vertex: on the first or last ring of an open strap. */
  Vector<bool> end_ring;
  int next_column = 0;

  void face(const Span<int> verts, const Span<float2> uvs)
  {
    face_sizes.append(verts.size());
    corner_verts.extend(verts);
    corner_uvs.extend(uvs);
  }
};

/** Profile points in (side, up), counter clockwise, with their distance around the profile. */
struct Profile {
  Vector<float2> points;
  Vector<float> around;
  float perimeter = 0.0f;
  /** Points that are hard corners. */
  Vector<bool> hard;
};

Profile make_profile(const Span<float2> points, const Span<bool> hard)
{
  Profile p;
  p.points.extend(points);
  p.hard.extend(hard);
  float sum = 0.0f;
  for (const int i : points.index_range()) {
    p.around.append(sum);
    sum += math::distance(points[i], points[(i + 1) % points.size()]);
  }
  p.perimeter = sum;
  return p;
}

/** Sweep one profile along the path. `offset_fn` gives the profile's center at an arc length,
 * in (side, up), and `turn_fn` its rotation. */
template<typename OffsetFn>
void sweep(Builder &b,
           const Path &path,
           const Profile &profile,
           const MeshWrapParams &params,
           const bool cut_ends,
           const OffsetFn &offset_fn)
{
  const int rings = path.points.size();
  const int m = profile.points.size();
  const float length = path.length();
  const bool open = !path.cyclic;
  const int first = b.positions.size();

  /* Arc length of each profile column at each ring, compressed at the ends for the cut. */
  const float w = std::max(params.width, 1e-5f);
  float cut = cut_ends && open && params.end_cut != WrapEndCut::Flat ?
                  std::max(params.end_cut_depth, 0.0f) * w :
                  0.0f;
  cut = std::min(cut, length * 0.25f);
  const float zone = cut * 2.0f;
  auto cut_depth = [&](const float x, const bool at_end) {
    const float u = std::clamp(x / w + 0.5f, 0.0f, 1.0f);
    switch (params.end_cut) {
      case WrapEndCut::Slant:
        return (at_end ? u : 1.0f - u) * cut;
      case WrapEndCut::Notch:
        return (1.0f - std::abs(2.0f * u - 1.0f)) * cut;
      default:
        return 0.0f;
    }
  };
  auto remap = [&](const float s, const float x) {
    if (cut <= 0.0f) {
      return s;
    }
    if (s < zone) {
      const float c = cut_depth(x, false);
      return c + s * (zone - c) / zone;
    }
    if (s > length - zone) {
      const float c = cut_depth(x, true);
      const float d = length - s;
      return length - (c + d * (zone - c) / zone);
    }
    return s;
  };

  Array<float> column_s(rings * m);
  for (int i = 0; i < rings; i++) {
    const float s = path.cum[i];
    const PathPoint here = path.points[i];
    for (int k = 0; k < m; k++) {
      const float2 pr = profile.points[k];
      const float s2 = remap(s, pr.x);
      const PathPoint pp = s2 == s ? here : path.at(s2);
      float2 local = pr;
      local.x *= pp.width_mul;
      if (params.shape != WrapShape::Strap) {
        local.y *= pp.width_mul;
      }
      local += offset_fn(s2, pp.width_mul);
      b.positions.append(pp.center + pp.side * local.x + pp.up * local.y);
      column_s[i * m + k] = s2;
      b.hard_column.append(profile.hard[k] ? b.next_column + k : -1);
      b.end_ring.append(open && (i == 0 || i == rings - 1));
    }
  }
  b.next_column += m;

  auto vert = [&](const int i, const int k) { return first + (i % rings) * m + (k % m); };
  const int segs = open ? rings - 1 : rings;
  for (int i = 0; i < segs; i++) {
    for (int k = 0; k < m; k++) {
      const int k2 = (k + 1) % m;
      const float v0 = profile.around[k];
      const float v1 = k2 == 0 ? profile.perimeter : profile.around[k2];
      const bool wrap = !open && i == rings - 1;
      const float s0k = column_s[i * m + k];
      const float s0k2 = column_s[i * m + k2];
      const float s1k = wrap ? length : column_s[((i + 1) % rings) * m + k];
      const float s1k2 = wrap ? length : column_s[((i + 1) % rings) * m + k2];
      const int verts[4] = {vert(i, k), vert(i + 1, k), vert(i + 1, k2), vert(i, k2)};
      const float2 uvs[4] = {{s0k, v0}, {s1k, v0}, {s1k2, v1}, {s0k2, v1}};
      b.face(Span<int>(verts, 4), Span<float2>(uvs, 4));
    }
  }
}

void cap_ngon(Builder &b, const int first, const int m, const Profile &profile, const bool end)
{
  Vector<int> verts;
  Vector<float2> uvs;
  for (int k = 0; k < m; k++) {
    const int kk = end ? m - 1 - k : k;
    verts.append(first + kk);
    uvs.append(profile.points[kk] + float2(0.5f));
  }
  b.face(verts, uvs);
}

/** Two quads closing the strap's six point profile, so a notched end stays clean. */
void cap_strap(Builder &b, const int first, const Profile &profile, const bool end)
{
  const int quads[2][4] = {{0, 1, 4, 5}, {1, 2, 3, 4}};
  for (const auto &q : quads) {
    int verts[4];
    float2 uvs[4];
    for (int c = 0; c < 4; c++) {
      const int k = end ? q[3 - c] : q[c];
      verts[c] = first + k;
      uvs[c] = profile.points[k] + float2(0.5f);
    }
    b.face(Span<int>(verts, 4), Span<float2>(uvs, 4));
  }
}

void sweep_chain(Builder &b, const Path &path, const MeshWrapParams &params)
{
  const int rings = path.points.size();
  const bool open = !path.cyclic;
  const float hw = params.width * 0.5f;
  const int res = std::clamp(params.resolution, 3, 64);

  auto circle = [&](const float r) {
    Vector<float2> pts;
    Vector<bool> hard;
    for (int i = 0; i < res; i++) {
      const float a = float(2.0 * M_PI) * float(i) / float(res);
      pts.append({r * std::cos(a), r * std::sin(a)});
      hard.append(false);
    }
    return make_profile(pts, hard);
  };

  switch (params.shape) {
    case WrapShape::Strap: {
      const float ht = params.thickness * 0.5f;
      const float2 pts[6] = {{-hw, -ht}, {0.0f, -ht}, {hw, -ht}, {hw, ht}, {0.0f, ht}, {-hw, ht}};
      const bool hard[6] = {true, false, true, true, false, true};
      const Profile profile = make_profile(Span<float2>(pts, 6), Span<bool>(hard, 6));
      const int first = b.positions.size();
      sweep(b, path, profile, params, true, [](float, float) { return float2(0.0f); });
      if (open) {
        cap_strap(b, first, profile, false);
        cap_strap(b, first + (rings - 1) * 6, profile, true);
      }
      break;
    }
    case WrapShape::Tube: {
      const Profile profile = circle(hw);
      const int first = b.positions.size();
      sweep(b, path, profile, params, false, [](float, float) { return float2(0.0f); });
      if (open) {
        cap_ngon(b, first, res, profile, false);
        cap_ngon(b, first + (rings - 1) * res, res, profile, true);
      }
      break;
    }
    case WrapShape::Rope: {
      const int strands = std::clamp(params.strands, 1, 16);
      /* Strands touching each other and the outer circle. */
      const float strand_r = strands == 1 ? hw : hw / (1.0f + 1.0f / std::sin(float(M_PI) / strands));
      const float center_r = strands == 1 ? 0.0f : hw - strand_r;
      float twist = params.twist;
      const float length = path.length();
      if (!open && length > 0.0f) {
        /* Whole turns round a closed loop, so the strands meet where it closes. */
        twist = std::round(twist * length) / length;
      }
      const Profile profile = circle(strand_r);
      for (int k = 0; k < strands; k++) {
        const float phase = float(2.0 * M_PI) * float(k) / float(strands);
        const int first = b.positions.size();
        sweep(b, path, profile, params, false, [&](const float s, const float width_mul) {
          const float a = phase + float(2.0 * M_PI) * twist * s;
          return float2(std::cos(a), std::sin(a)) * center_r * width_mul;
        });
        if (open) {
          cap_ngon(b, first, res, profile, false);
          cap_ngon(b, first + (rings - 1) * res, res, profile, true);
        }
      }
      break;
    }
  }
}

Mesh *lines_mesh(const Span<Laid> laid)
{
  int verts_num = 0;
  int edges_num = 0;
  for (const Laid &l : laid) {
    verts_num += l.size();
    edges_num += l.cyclic ? l.size() : l.size() - 1;
  }
  Mesh *mesh = bke::mesh_new_no_attributes(verts_num, edges_num, 0, 0);
  MutableSpan<float3> positions = mesh->vert_positions_for_write();
  MutableSpan<int2> edges = mesh->edges_for_write();
  int v = 0;
  int e = 0;
  for (const Laid &l : laid) {
    const int n = l.size();
    for (int i = 0; i < n; i++) {
      positions[v + i] = l.surf[i];
    }
    for (int i = 0; i < (l.cyclic ? n : n - 1); i++) {
      edges[e++] = int2(v + i, v + (i + 1) % n);
    }
    v += n;
  }
  return mesh;
}

}  // namespace

Mesh *mesh_wrap(const Mesh &mesh, const MeshWrapParams &params, const char **r_error)
{
  if (params.target == nullptr || params.target->faces_num == 0) {
    *r_error = "The target has no faces to wrap";
    return nullptr;
  }
  const Surface surface(*params.target, params.target_to_space);
  if (surface.bvh.tree == nullptr) {
    return nullptr;
  }

  Vector<Chain> chains;
  if (params.use_pattern) {
    chains.extend(chains_from_pattern(*params.target, surface, params));
  }
  if (params.lines_only) {
    Vector<Laid> laid;
    for (const Chain &chain : chains) {
      if (chain.points.size() >= 2) {
        laid.append(lay(surface, chain, std::max(params.step, 1e-4f), 0.0f));
      }
    }
    return lines_mesh(laid);
  }
  if (params.use_edges) {
    chains.extend(chains_from_edges(mesh, params.pin));
  }

  const float step = std::max(params.step, std::max(params.width, 1e-4f) * 0.05f);
  Vector<Laid> laid;
  for (Chain &chain : chains) {
    resample(chain, step);
    if (chain.points.size() >= 2) {
      laid.append(lay(surface, chain, step, params.tight));
    }
  }
  if (laid.is_empty()) {
    return nullptr;
  }

  Vector<Array<float>> cums;
  for (const Laid &l : laid) {
    cums.append(arc_lengths(l.surf, l.cyclic));
  }
  Vector<Array<float>> lifts;
  if (params.crossings) {
    lifts = crossing_lifts(laid, cums, params.width, params.crossings_flip);
  }
  else {
    for (const Laid &l : laid) {
      lifts.append(Array<float>(l.size(), 0.0f));
    }
  }

  Builder builder;
  for (const int c : laid.index_range()) {
    const Path path = build_path(laid[c], lifts[c], params, c);
    sweep_chain(builder, path, params);
  }

  const int verts_num = builder.positions.size();
  const int faces_num = builder.face_sizes.size();
  const int corners_num = builder.corner_verts.size();
  Mesh *result = bke::mesh_new_no_attributes(verts_num, 0, faces_num, corners_num);
  result->vert_positions_for_write().copy_from(builder.positions);
  MutableSpan<int> offsets = result->face_offsets_for_write();
  int sum = 0;
  for (const int f : IndexRange(faces_num)) {
    offsets[f] = sum;
    sum += builder.face_sizes[f];
  }
  offsets[faces_num] = sum;
  result->corner_verts_for_write().copy_from(builder.corner_verts);
  bke::mesh_calc_edges(*result, false, false);

  bke::MutableAttributeAccessor attributes = result->attributes_for_write();
  bke::SpanAttributeWriter<float2> uvs = attributes.lookup_or_add_for_write_only_span<float2>(
      "UVMap", bke::AttrDomain::Corner);
  uvs.span.copy_from(builder.corner_uvs);
  uvs.finish();

  /* Hard edges along the strap's corners and around its ends. */
  const Span<int2> edges = result->edges();
  bke::SpanAttributeWriter<bool> sharp = attributes.lookup_or_add_for_write_span<bool>(
      "sharp_edge", bke::AttrDomain::Edge);
  bool any_sharp = false;
  for (const int e : edges.index_range()) {
    const int a = edges[e][0];
    const int b = edges[e][1];
    const bool along = builder.hard_column[a] >= 0 &&
                       builder.hard_column[a] == builder.hard_column[b];
    const bool around_end = builder.end_ring[a] && builder.end_ring[b];
    sharp.span[e] = along || around_end;
    any_sharp |= sharp.span[e];
  }
  sharp.finish();
  if (!any_sharp) {
    attributes.remove("sharp_edge");
  }

  if (params.material_index > 0) {
    bke::SpanAttributeWriter<int> materials = attributes.lookup_or_add_for_write_only_span<int>(
        "material_index", bke::AttrDomain::Face);
    materials.span.fill(params.material_index);
    materials.finish();
  }
  return result;
}

}  // namespace blender::geometry
