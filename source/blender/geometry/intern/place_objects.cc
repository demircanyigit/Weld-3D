/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup geo
 */

#include <algorithm>
#include <cmath>
#include <optional>

#include "BLI_array.hh"
#include "BLI_map.hh"
#include "BLI_math_base.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_noise.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_mesh_types.h"

#include "BKE_mesh.hh"
#include "BKE_mesh_types.hh"

#include "GEO_place_objects.hh"

namespace blender::geometry {

/* -------------------------------------------------------------------- */
/** \name Pattern and Picking
 * \{ */

static bool is_separator(const char c)
{
  return c == ' ' || c == ',' || c == ';' || c == '\t' || c == '\n';
}

static std::optional<int> parse_number(StringRef text)
{
  if (text.is_empty() || text.size() > 6) {
    return std::nullopt;
  }
  int value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    value = value * 10 + (c - '0');
  }
  return value;
}

Vector<int> place_parse_pattern(const StringRef text, const int rows_num, std::string &r_error)
{
  Vector<int> result;
  int64_t i = 0;
  while (i < text.size()) {
    if (is_separator(text[i])) {
      i++;
      continue;
    }
    int64_t end = i;
    while (end < text.size() && !is_separator(text[end])) {
      end++;
    }
    const StringRef word = text.substr(i, end - i);
    i = end;

    /* "2" or "2x3" (also "2*3"): list number 2, three times. */
    int64_t times_at = word.find_first_of("xX*");
    const StringRef number_text = times_at == StringRef::not_found ? word :
                                                                     word.substr(0, times_at);
    const std::optional<int> number = parse_number(number_text);
    int times = 1;
    bool ok = number.has_value() && *number >= 1 && *number <= rows_num;
    if (ok && times_at != StringRef::not_found) {
      const std::optional<int> parsed = parse_number(word.substr(times_at + 1));
      ok = parsed.has_value() && *parsed >= 1 && *parsed <= 1000;
      times = parsed.value_or(1);
    }
    if (!ok) {
      r_error = "Pattern: \"" + std::string(word) + "\" is not a number in the list";
      return result;
    }
    for (int t = 0; t < times; t++) {
      result.append(*number - 1);
    }
    if (result.size() > 100000) {
      break;
    }
  }
  return result;
}

PlacePicker::PlacePicker(const PlacePick pick,
                         const Span<int> sequence,
                         const Span<PlaceItem> items,
                         const int seed)
    : pick_(pick), items_num_(int(items.size())), seed_(seed)
{
  for (const int item : sequence) {
    if (item >= 0 && item < items_num_ && items[item].valid) {
      sequence_.append(item);
    }
  }
  float total = 0.0f;
  cumulative_.reserve(items.size());
  for (const PlaceItem &item : items) {
    total += item.valid ? std::max(item.weight, 0.0f) : 0.0f;
    cumulative_.append(total);
  }
  /* All weights zero: every usable item counts the same. */
  if (total <= 0.0f) {
    total = 0.0f;
    for (const int i : items.index_range()) {
      total += items[i].valid ? 1.0f : 0.0f;
      cumulative_[i] = total;
    }
  }
}

bool PlacePicker::is_empty() const
{
  return cumulative_.is_empty() || cumulative_.last() <= 0.0f;
}

int PlacePicker::pick(const int slot) const
{
  if (this->is_empty()) {
    return -1;
  }
  switch (pick_) {
    case PlacePick::Pattern:
      if (sequence_.is_empty()) {
        return -1;
      }
      return sequence_[slot % sequence_.size()];
    case PlacePick::InOrder:
      return slot % items_num_;
    case PlacePick::Random:
    default: {
      /* Hashed per turn, so more turns keep the picks of the ones before. */
      const float r = noise::hash_to_float(uint32_t(slot), uint32_t(seed_), 101u) *
                      cumulative_.last();
      const auto it = std::upper_bound(cumulative_.begin(), cumulative_.end(), r);
      return std::min(int(it - cumulative_.begin()), items_num_ - 1);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Placing One Object
 * \{ */

/** A random number in [0, 1) for one object and one use. */
static float rand01(const uint32_t key, const int seed, const uint32_t use)
{
  return noise::hash_to_float(key, uint32_t(seed), use);
}

struct PieceSize {
  /** Uniform scale. */
  float scale = 1.0f;
  /** Scale of the up axis, on top of #scale. */
  float height = 1.0f;
};

static PieceSize piece_size(const PlaceLook &look, const uint32_t key)
{
  PieceSize size;
  size.scale = math::interpolate(look.scale_min, look.scale_max, rand01(key, look.seed, 11u));
  size.height = math::interpolate(look.height_min, look.height_max, rand01(key, look.seed, 12u));
  return size;
}

/** Length of the item along the edge. */
static float item_length(const PlaceItem &item, const int along, const float scale)
{
  return std::max(item.max[along] - item.min[along], 0.0f) * scale;
}

/** Width of the item across the edge. */
static float item_depth(const PlaceItem &item, const int along, const float scale)
{
  const int side = 1 - along;
  return std::max(item.max[side] - item.min[side], 0.0f) * scale;
}

/** A unit vector at right angles to \a up. */
static float3 any_perpendicular(const float3 &up)
{
  const float3 axis = std::abs(up.x) < 0.9f ? float3(1.0f, 0.0f, 0.0f) : float3(0.0f, 1.0f, 0.0f);
  return math::normalize(math::cross(up, axis));
}

/** \a forward made at right angles to \a up, both unit length. */
static void orthonormalize(float3 &forward, float3 &up)
{
  up = math::normalize(up);
  if (math::is_zero(up)) {
    up = float3(0.0f, 0.0f, 1.0f);
  }
  forward -= up * math::dot(forward, up);
  const float len = math::length(forward);
  forward = len > 1e-6f ? forward / len : any_perpendicular(up);
}

static float3x3 rotation_around(const int axis, const float angle)
{
  const float c = std::cos(angle);
  const float s = std::sin(angle);
  float3x3 m = float3x3::identity();
  const int a = (axis + 1) % 3;
  const int b = (axis + 2) % 3;
  m[a][a] = c;
  m[a][b] = s;
  m[b][a] = -s;
  m[b][b] = c;
  return m;
}

/**
 * The transform that puts \a item at \a position: its along axis on \a forward, its Z on \a up,
 * its base on the position, moved sideways by \a side_shift.
 * \param stretch: extra scale along the edge.
 */
static float4x4 piece_transform(const PlaceItem &item,
                                const PlaceLook &look,
                                const float3 &position,
                                float3 forward,
                                float3 up,
                                const PieceSize &size,
                                const float stretch,
                                const float side_shift,
                                const uint32_t key)
{
  orthonormalize(forward, up);
  const float3 left = math::cross(up, forward);
  const int along = look.along;

  /* The anchor: the middle of the box, on its bottom for #PlaceBase::Bottom. */
  float3 anchor = (item.min + item.max) * 0.5f;
  if (look.base == PlaceBase::Bottom) {
    anchor.z = item.min.z;
  }

  float3 scale(size.scale, size.scale, size.scale * size.height);
  scale[along] *= stretch;
  float3x3 m = math::from_scale<float3x3>(scale);

  if (look.tilt_random != 0.0f) {
    const float tilt = (rand01(key, look.seed, 13u) * 2.0f - 1.0f) * look.tilt_random;
    m = rotation_around(along, tilt) * m;
  }
  if (look.rotation_random != 0.0f) {
    const float turn = (rand01(key, look.seed, 14u) * 2.0f - 1.0f) * look.rotation_random;
    m = rotation_around(2, turn) * m;
  }

  /* The item's axes on the frame. */
  float3x3 basis;
  if (along == 0) {
    basis[0] = forward;
    basis[1] = left;
  }
  else {
    basis[0] = -left;
    basis[1] = forward;
  }
  basis[2] = up;

  float4x4 result = float4x4(basis * m);
  const float3 moved_anchor = (basis * m) * anchor;
  result.location() = position + up * look.offset + left * side_shift - moved_anchor;
  return result;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Edge Paths
 * \{ */

struct PathPoint {
  float3 co;
  float3 normal;
};

struct EdgePath {
  Vector<int> verts;
  bool cyclic = false;
};

/** The point normals, with Z for points that are on no face. */
static Array<float3> point_normals(const Mesh &mesh)
{
  Array<float3> normals(mesh.verts_num, float3(0.0f, 0.0f, 1.0f));
  if (mesh.faces_num == 0) {
    return normals;
  }
  const Span<float3> vert_normals = mesh.vert_normals();
  Array<bool> on_face(mesh.verts_num, false);
  for (const int v : mesh.corner_verts()) {
    on_face[v] = true;
  }
  for (const int v : normals.index_range()) {
    if (on_face[v]) {
      normals[v] = vert_normals[v];
    }
  }
  return normals;
}

static Array<bool> used_edges(const Mesh &mesh, const PlaceEdgesParams &params)
{
  const Span<int2> edges = mesh.edges();
  Array<bool> used(edges.size(), true);
  if (params.edges == PlaceEdges::Boundary) {
    Array<int> face_count(edges.size(), 0);
    for (const int e : mesh.corner_edges()) {
      face_count[e]++;
    }
    for (const int e : edges.index_range()) {
      used[e] = face_count[e] == 1;
    }
  }
  else if (params.edges == PlaceEdges::Loose) {
    const bke::LooseEdgeCache &loose = mesh.loose_edges();
    for (const int e : edges.index_range()) {
      used[e] = loose.count > 0 && loose.is_loose_bits[e];
    }
  }
  if (!params.vert_selection.is_empty()) {
    for (const int e : edges.index_range()) {
      used[e] = used[e] && params.vert_selection[edges[e][0]] &&
                params.vert_selection[edges[e][1]];
    }
  }
  return used;
}

/**
 * The used edges as chains of points. A chain breaks at points with other than two used edges,
 * and where it turns more than the corner angle. Rings without breaks are cyclic.
 */
static Vector<EdgePath> build_paths(const Mesh &mesh,
                                    const Span<bool> used,
                                    const float corner_angle,
                                    Array<bool> &r_corner)
{
  const Span<int2> edges = mesh.edges();
  const Span<float3> positions = mesh.vert_positions();
  Array<Vector<int, 2>> vert_edges(mesh.verts_num);
  for (const int e : edges.index_range()) {
    if (used[e] && edges[e][0] != edges[e][1]) {
      vert_edges[edges[e][0]].append(e);
      vert_edges[edges[e][1]].append(e);
    }
  }
  auto other = [&](const int e, const int v) {
    return edges[e][0] == v ? edges[e][1] : edges[e][0];
  };

  /* Breaks: ends, forks, and sharp turns. Corners are breaks where paths meet. */
  Array<bool> is_break(mesh.verts_num, false);
  r_corner.reinitialize(mesh.verts_num);
  r_corner.fill(false);
  for (const int v : vert_edges.index_range()) {
    const Span<int> ve = vert_edges[v];
    if (ve.is_empty()) {
      continue;
    }
    if (ve.size() != 2) {
      is_break[v] = true;
      r_corner[v] = ve.size() > 2;
      continue;
    }
    const float3 in = positions[v] - positions[other(ve[0], v)];
    const float3 out = positions[other(ve[1], v)] - positions[v];
    if (math::is_zero(in) || math::is_zero(out)) {
      continue;
    }
    const float turn = math::safe_acos_approx(
        math::dot(math::normalize(in), math::normalize(out)));
    if (turn > corner_angle) {
      is_break[v] = true;
      r_corner[v] = true;
    }
  }

  Vector<EdgePath> paths;
  Array<bool> visited(edges.size(), false);
  auto walk = [&](const int start_vert, const int start_edge, EdgePath &path) {
    path.verts.append(start_vert);
    int v = start_vert;
    int e = start_edge;
    while (true) {
      visited[e] = true;
      v = other(e, v);
      path.verts.append(v);
      if (is_break[v] || v == start_vert) {
        break;
      }
      int next = -1;
      for (const int ne : vert_edges[v]) {
        if (!visited[ne]) {
          next = ne;
          break;
        }
      }
      if (next == -1) {
        break;
      }
      e = next;
    }
  };

  for (const int v : vert_edges.index_range()) {
    if (!is_break[v]) {
      continue;
    }
    for (const int e : vert_edges[v]) {
      if (!visited[e]) {
        EdgePath path;
        walk(v, e, path);
        paths.append(std::move(path));
      }
    }
  }
  /* What is left are rings without breaks. */
  for (const int e : edges.index_range()) {
    if (used[e] && !visited[e] && edges[e][0] != edges[e][1]) {
      EdgePath path;
      walk(edges[e][0], e, path);
      path.cyclic = path.verts.size() > 2 && path.verts.last() == path.verts.first();
      paths.append(std::move(path));
    }
  }
  return paths;
}

/** Distances are measured on the ground for #PlaceUp::Z, so standing objects keep apart. */
struct Metric {
  bool flat = false;
  float3 operator()(const float3 &v) const
  {
    return flat ? float3(v.x, v.y, 0.0f) : v;
  }
};

/** A spot on a path: segment \a seg, at \a t between its two points. */
struct Cursor {
  int seg = 0;
  float t = 0.0f;
};

class PathWalker {
  Span<PathPoint> points_;
  Metric metric_;

 public:
  PathWalker(Span<PathPoint> points, Metric metric) : points_(points), metric_(metric) {}

  int segments_num() const
  {
    return int(points_.size()) - 1;
  }

  float3 position(const Cursor &c) const
  {
    return math::interpolate(points_[c.seg].co, points_[c.seg + 1].co, c.t);
  }

  float3 normal(const Cursor &c) const
  {
    return math::interpolate(points_[c.seg].normal, points_[c.seg + 1].normal, c.t);
  }

  Cursor end() const
  {
    return {this->segments_num() - 1, 1.0f};
  }

  /**
   * The first spot further on that is \a distance away from \a from, in a straight line, so a
   * piece of that length fits between them even on bends. With \a beyond, the last segment goes on
   * past the end of the path.
   */
  std::optional<Cursor> advance(const Cursor &from, const float distance, const bool beyond) const
  {
    const float3 q = this->position(from);
    if (distance <= 0.0f) {
      return from;
    }
    const float l2 = distance * distance;
    for (int seg = from.seg; seg < this->segments_num(); seg++) {
      const float u0 = seg == from.seg ? from.t : 0.0f;
      const float3 a = points_[seg].co;
      const float3 b = points_[seg + 1].co;
      const float3 d0 = metric_(a - q);
      const float3 e = metric_(b - a);
      const float A = math::dot(e, e);
      if (A < 1e-12f) {
        continue;
      }
      const bool last = seg == this->segments_num() - 1;
      if (math::length_squared(d0 + e) < l2 && !(last && beyond)) {
        continue;
      }
      const float B = math::dot(d0, e);
      const float C = math::dot(d0, d0) - l2;
      const float disc = std::max(B * B - A * C, 0.0f);
      const float u = (-B + std::sqrt(disc)) / A;
      if (u < u0 - 1e-6f) {
        continue;
      }
      return Cursor{seg, (last && beyond) ? std::max(u, u0) : std::clamp(u, u0, 1.0f)};
    }
    return std::nullopt;
  }

  /** Straight distance between two spots. */
  float distance(const Cursor &a, const Cursor &b) const
  {
    return math::length(metric_(this->position(b) - this->position(a)));
  }
};

/** The path with \a cut_start and \a cut_end taken off its ends, along its length. */
static Vector<PathPoint> trim_path(Span<PathPoint> points,
                                   const Metric &metric,
                                   const float cut_start,
                                   const float cut_end)
{
  Vector<PathPoint> result(points);
  auto cut = [&](Vector<PathPoint> &pts, float amount) {
    /* Takes \a amount off the front. */
    while (amount > 0.0f && pts.size() >= 2) {
      const float len = math::length(metric(pts[1].co - pts[0].co));
      if (len > amount) {
        const float t = amount / len;
        pts[0].co = math::interpolate(pts[0].co, pts[1].co, t);
        pts[0].normal = math::interpolate(pts[0].normal, pts[1].normal, t);
        return;
      }
      amount -= len;
      pts.remove(0);
    }
  };
  cut(result, cut_start);
  std::reverse(result.begin(), result.end());
  cut(result, cut_end);
  std::reverse(result.begin(), result.end());
  return result;
}

/** Positive when the ring turns left (counter-clockwise from above). */
static float signed_area_xy(Span<PathPoint> points)
{
  float area = 0.0f;
  for (const int i : points.index_range()) {
    const float3 &a = points[i].co;
    const float3 &b = points[(i + 1) % points.size()].co;
    area += a.x * b.y - b.x * a.y;
  }
  return area * 0.5f;
}

struct PieceOnPath {
  int item;
  int slot;
  Cursor start;
  Cursor end;
};

struct PathLayout {
  Vector<PieceOnPath> pieces;
  /** Straight distance from the last piece to the end of the path. */
  float leftover = 0.0f;
  int slots_used = 0;
};

static PathLayout layout_path(const PathWalker &walker,
                              const PlaceEdgesParams &params,
                              const int slot_start,
                              const uint32_t key_start,
                              const float first_gap,
                              const float gap,
                              const float stretch,
                              const bool overhang)
{
  PathLayout layout;
  if (walker.segments_num() < 1) {
    return layout;
  }
  const PlaceLook &look = params.look;
  std::optional<Cursor> cursor = walker.advance(Cursor{0, 0.0f}, first_gap, false);
  int slot = slot_start;
  int misses = 0;
  while (cursor) {
    if (layout.pieces.size() > 100000 || misses > 1000) {
      break;
    }
    const int item_index = params.picker->pick(slot);
    const uint32_t key = key_start + uint32_t(slot - slot_start);
    if (item_index < 0 || !params.items[item_index].valid) {
      slot++;
      misses++;
      continue;
    }
    const PlaceItem &item = params.items[item_index];
    const PieceSize size = piece_size(look, key);
    const float length = item_length(item, look.along, size.scale) * stretch;
    if (length <= 1e-6f) {
      slot++;
      misses++;
      continue;
    }
    misses = 0;
    std::optional<Cursor> end = walker.advance(*cursor, length, false);
    if (!end) {
      if (overhang) {
        end = walker.advance(*cursor, length, true);
        if (end) {
          layout.pieces.append({item_index, slot, *cursor, *end});
          slot++;
        }
      }
      if (!overhang) {
        layout.leftover = walker.distance(*cursor, walker.end());
      }
      break;
    }
    layout.pieces.append({item_index, slot, *cursor, *end});
    slot++;
    layout.leftover = walker.distance(*end, walker.end());
    cursor = walker.advance(*end, gap, false);
  }
  layout.slots_used = slot - slot_start;
  return layout;
}

Vector<PlacedObject> place_along_edges(const Mesh &mesh, const PlaceEdgesParams &params)
{
  Vector<PlacedObject> result;
  if (params.picker == nullptr || params.picker->is_empty() || mesh.edges_num == 0) {
    return result;
  }
  const PlaceLook &look = params.look;
  const Span<float3> positions = mesh.vert_positions();
  const Array<float3> normals = point_normals(mesh);
  const Array<bool> used = used_edges(mesh, params);
  Array<bool> is_corner;
  const Vector<EdgePath> paths = build_paths(mesh, used, params.corner_angle, is_corner);
  const Metric metric{look.up == PlaceUp::Z};

  /* How far runs stay back from the corners. */
  const int rows_num = params.corner_item >= 0 ? params.corner_item : int(params.items.size());
  float pull_back = 0.0f;
  if (params.corners == PlaceCorners::PullBack) {
    for (const int i : IndexRange(rows_num)) {
      if (params.items[i].valid) {
        pull_back = std::max(pull_back,
                             item_depth(params.items[i], look.along, look.scale_max) * 0.5f);
      }
    }
  }
  const bool corner_objects = params.corners == PlaceCorners::CornerObject &&
                              params.corner_item >= 0 && params.items[params.corner_item].valid;
  if (corner_objects) {
    const PlaceItem &item = params.items[params.corner_item];
    pull_back = std::max(item.max.x - item.min.x, item.max.y - item.min.y) * 0.5f + params.gap;
  }

  int slot = 0;
  uint32_t key = 0;
  for (const int path_i : paths.index_range()) {
    const EdgePath &path = paths[path_i];
    Vector<PathPoint> points;
    for (const int v : path.verts) {
      points.append({positions[v], normals[v]});
    }
    float cut_start = 0.0f;
    float cut_end = 0.0f;
    if (!path.cyclic) {
      cut_start = is_corner[path.verts.first()] ? pull_back : 0.0f;
      cut_end = is_corner[path.verts.last()] ? pull_back : 0.0f;
    }
    if (cut_start > 0.0f || cut_end > 0.0f) {
      points = trim_path(points, metric, cut_start, cut_end);
    }
    if (points.size() < 2) {
      continue;
    }
    const PathWalker walker(points, metric);
    if (params.restart_per_path) {
      slot = 0;
    }

    /* Which way is inside. */
    float inside_sign = 1.0f;
    if (path.cyclic) {
      inside_sign = signed_area_xy(points.as_span().drop_back(1)) >= 0.0f ? 1.0f : -1.0f;
    }

    PathLayout layout = layout_path(
        walker, params, slot, key, 0.0f, params.gap, 1.0f, params.fit == PlaceFit::Overhang);
    const int64_t pieces_num = layout.pieces.size();
    float stretch = 1.0f;
    if (pieces_num > 0 && layout.leftover > 1e-5f &&
        ELEM(params.fit, PlaceFit::Spread, PlaceFit::Stretch))
    {
      /* Grow the gaps or the pieces until the path is full, keeping the same pieces. */
      float extra = 0.0f;
      const int64_t gaps_num = path.cyclic ? pieces_num : pieces_num - 1;
      float length_sum = 0.0f;
      for (const PieceOnPath &piece : layout.pieces) {
        length_sum += walker.distance(piece.start, piece.end);
      }
      float leftover = layout.leftover;
      for (int iter = 0; iter < 6 && leftover > 1e-5f; iter++) {
        float try_extra = extra;
        float try_stretch = stretch;
        float try_first = 0.0f;
        if (params.fit == PlaceFit::Spread) {
          if (gaps_num > 0) {
            try_extra += leftover / float(gaps_num);
          }
          else {
            try_first = leftover * 0.5f;
          }
        }
        else {
          try_stretch *= 1.0f + leftover / std::max(length_sum * try_stretch, 1e-6f);
        }
        PathLayout attempt = layout_path(
            walker, params, slot, key, try_first, params.gap + try_extra, try_stretch, false);
        if (attempt.pieces.size() < pieces_num) {
          /* Went past the end: keep the last fit, and only go part of the way. */
          leftover *= 0.5f;
          continue;
        }
        attempt.pieces.resize(pieces_num);
        layout = std::move(attempt);
        extra = try_extra;
        stretch = try_stretch;
        leftover = layout.leftover;
        if (gaps_num == 0) {
          break;
        }
      }
    }

    for (const PieceOnPath &piece : layout.pieces) {
      const PlaceItem &item = params.items[piece.item];
      const uint32_t piece_key = key + uint32_t(piece.slot - slot);
      const PieceSize size = piece_size(look, piece_key);
      const float3 p0 = walker.position(piece.start);
      const float3 p1 = walker.position(piece.end);
      float3 up = look.up == PlaceUp::Z ? float3(0.0f, 0.0f, 1.0f) :
                                          walker.normal(piece.start) + walker.normal(piece.end);
      if (math::is_zero(up)) {
        up = float3(0.0f, 0.0f, 1.0f);
      }
      float side_shift = 0.0f;
      if (look.side != PlaceSide::Center) {
        const float half = item_depth(item, look.along, size.scale) * 0.5f;
        side_shift = (look.side == PlaceSide::Inside ? half : -half) * inside_sign;
      }
      /* The middle of the piece sits halfway between its two ends. */
      const float3 position = (p0 + p1) * 0.5f;
      result.append({piece.item,
                     piece_transform(item,
                                     look,
                                     position,
                                     p1 - p0,
                                     up,
                                     size,
                                     stretch,
                                     side_shift,
                                     piece_key)});
    }
    slot += layout.slots_used;
    key += uint32_t(std::max(layout.slots_used, 1)) + 1000003u * uint32_t(path_i + 1);
  }

  /* One corner object on each corner, facing along its first edge. */
  if (corner_objects) {
    const Span<int2> edges = mesh.edges();
    Array<int> first_edge(mesh.verts_num, -1);
    for (const int e : edges.index_range()) {
      if (!used[e]) {
        continue;
      }
      for (const int v : {edges[e][0], edges[e][1]}) {
        if (first_edge[v] == -1) {
          first_edge[v] = e;
        }
      }
    }
    PlaceLook corner_look = look;
    corner_look.side = PlaceSide::Center;
    for (const int v : IndexRange(mesh.verts_num)) {
      if (!is_corner[v] || first_edge[v] == -1) {
        continue;
      }
      const int2 edge = edges[first_edge[v]];
      const int w = edge[0] == v ? edge[1] : edge[0];
      const float3 up = look.up == PlaceUp::Z ? float3(0.0f, 0.0f, 1.0f) : normals[v];
      const uint32_t corner_key = 0x9E3779B9u ^ uint32_t(v);
      result.append({params.corner_item,
                     piece_transform(params.items[params.corner_item],
                                     corner_look,
                                     positions[v],
                                     positions[w] - positions[v],
                                     up,
                                     piece_size(look, corner_key),
                                     1.0f,
                                     0.0f,
                                     corner_key)});
    }
  }
  return result;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vertices and Faces
 * \{ */

Vector<PlacedObject> place_on_vertices(const Mesh &mesh,
                                       const Span<PlaceItem> items,
                                       const PlacePicker &picker,
                                       const Span<bool> vert_selection,
                                       const PlaceLook &look)
{
  Vector<PlacedObject> result;
  if (picker.is_empty()) {
    return result;
  }
  const Span<float3> positions = mesh.vert_positions();
  const Span<int2> edges = mesh.edges();
  const Array<float3> normals = point_normals(mesh);
  Array<int> first_edge(mesh.verts_num, -1);
  for (const int e : edges.index_range()) {
    for (const int v : {edges[e][0], edges[e][1]}) {
      if (first_edge[v] == -1) {
        first_edge[v] = e;
      }
    }
  }
  int slot = 0;
  for (const int v : positions.index_range()) {
    if (!vert_selection.is_empty() && !vert_selection[v]) {
      continue;
    }
    const int item_index = picker.pick(slot);
    slot++;
    if (item_index < 0 || !items[item_index].valid) {
      continue;
    }
    float3 forward(1.0f, 0.0f, 0.0f);
    if (first_edge[v] != -1) {
      const int2 edge = edges[first_edge[v]];
      forward = positions[edge[0] == v ? edge[1] : edge[0]] - positions[v];
    }
    const float3 up = look.up == PlaceUp::Z ? float3(0.0f, 0.0f, 1.0f) : normals[v];
    const uint32_t key = uint32_t(v);
    result.append({item_index,
                   piece_transform(items[item_index],
                                   look,
                                   positions[v],
                                   forward,
                                   up,
                                   piece_size(look, key),
                                   1.0f,
                                   0.0f,
                                   key)});
  }
  return result;
}

Vector<PlacedObject> place_on_faces(const Mesh &mesh,
                                    const Span<PlaceItem> items,
                                    const PlacePicker &picker,
                                    const Span<bool> vert_selection,
                                    const PlaceLook &look,
                                    const bool fit_to_face)
{
  Vector<PlacedObject> result;
  if (picker.is_empty()) {
    return result;
  }
  const Span<float3> positions = mesh.vert_positions();
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<float3> face_normals = mesh.face_normals();
  int slot = 0;
  for (const int f : faces.index_range()) {
    const Span<int> verts = corner_verts.slice(faces[f]);
    if (!vert_selection.is_empty()) {
      bool all = true;
      for (const int v : verts) {
        all = all && vert_selection[v];
      }
      if (!all) {
        continue;
      }
    }
    const int item_index = picker.pick(slot);
    slot++;
    if (item_index < 0 || !items[item_index].valid) {
      continue;
    }
    const PlaceItem &item = items[item_index];
    const float3 center = bke::mesh::face_center_calc(positions, verts);
    float3 up = look.up == PlaceUp::Z ? float3(0.0f, 0.0f, 1.0f) : face_normals[f];
    /* Forward along the longest edge. */
    float3 forward(1.0f, 0.0f, 0.0f);
    float longest = -1.0f;
    for (const int i : verts.index_range()) {
      const float3 d = positions[verts[(i + 1) % verts.size()]] - positions[verts[i]];
      const float len = math::length_squared(d);
      if (len > longest) {
        longest = len;
        forward = d;
      }
    }
    const uint32_t key = uint32_t(f);
    PieceSize size = piece_size(look, key);
    if (fit_to_face) {
      float3 fwd = forward;
      float3 n = up;
      orthonormalize(fwd, n);
      const float3 left = math::cross(n, fwd);
      float2 lo(1e30f), hi(-1e30f);
      for (const int v : verts) {
        const float3 d = positions[v] - center;
        const float2 p(math::dot(d, fwd), math::dot(d, left));
        lo = math::min(lo, p);
        hi = math::max(hi, p);
      }
      const float length = item_length(item, look.along, 1.0f);
      const float depth = item_depth(item, look.along, 1.0f);
      float fit = 1e30f;
      if (length > 1e-6f) {
        fit = std::min(fit, (hi.x - lo.x) / length);
      }
      if (depth > 1e-6f) {
        fit = std::min(fit, (hi.y - lo.y) / depth);
      }
      if (fit < 1e29f) {
        size.scale *= fit;
      }
    }
    result.append({item_index,
                   piece_transform(item, look, center, forward, up, size, 1.0f, 0.0f, key)});
  }
  return result;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Scatter
 * \{ */

struct ScatterSpot {
  float3 co;
  float3 normal;
  /** Fixed per spot, for its random values. */
  uint32_t key;
  float order;
};

Vector<PlacedObject> scatter_on_faces(const Mesh &mesh, const ScatterParams &params)
{
  Vector<PlacedObject> result;
  if (params.picker == nullptr || params.picker->is_empty() || mesh.faces_num == 0) {
    return result;
  }
  const PlaceLook &look = params.look;
  const Span<float3> positions = mesh.vert_positions();
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<int3> tris = mesh.corner_tris();
  const Span<int> tri_faces = mesh.corner_tri_faces();
  const Span<float3> face_normals = mesh.face_normals();
  const Span<float> weights = params.vert_weights;
  const int seed = look.seed;

  Array<float> cumulative(tris.size());
  float total = 0.0f;
  for (const int t : tris.index_range()) {
    const float3 &a = positions[corner_verts[tris[t][0]]];
    const float3 &b = positions[corner_verts[tris[t][1]]];
    const float3 &c = positions[corner_verts[tris[t][2]]];
    total += math::length(math::cross(b - a, c - a)) * 0.5f;
    cumulative[t] = total;
  }
  if (total <= 0.0f) {
    return result;
  }

  Vector<ScatterSpot> spots;
  auto add_spot = [&](const int t, const uint32_t key) {
    float u = rand01(key, seed, 1u);
    float v = rand01(key, seed, 2u);
    if (u + v > 1.0f) {
      u = 1.0f - u;
      v = 1.0f - v;
    }
    const float w = 1.0f - u - v;
    const int3 tri = tris[t];
    const int va = corner_verts[tri[0]];
    const int vb = corner_verts[tri[1]];
    const int vc = corner_verts[tri[2]];
    if (!weights.is_empty()) {
      const float chance = weights[va] * w + weights[vb] * u + weights[vc] * v;
      if (rand01(key, seed, 3u) >= chance) {
        return;
      }
    }
    ScatterSpot spot;
    spot.co = positions[va] * w + positions[vb] * u + positions[vc] * v;
    spot.normal = face_normals[tri_faces[t]];
    spot.key = key;
    spot.order = rand01(key, seed, 4u);
    spots.append(spot);
  };

  /* Never more tries than this, however large the faces. */
  const int64_t limit = std::max<int64_t>(int64_t(params.max_count) * 8, 1000);
  if (params.use_count) {
    const int count = int(std::min<int64_t>(std::max(params.count, 0), limit));
    for (const int i : IndexRange(count)) {
      const uint32_t key = uint32_t(i);
      const float r = rand01(key, seed, 5u) * total;
      const auto it = std::upper_bound(cumulative.begin(), cumulative.end(), r);
      const int t = std::min(int(it - cumulative.begin()), int(tris.size()) - 1);
      add_spot(t, key);
    }
  }
  else {
    float density = std::max(params.density, 0.0f);
    if (double(total) * density > double(limit)) {
      density = float(double(limit) / double(total));
    }
    float previous = 0.0f;
    for (const int t : tris.index_range()) {
      const float area = cumulative[t] - previous;
      previous = cumulative[t];
      const float expected = area * density;
      int n = int(expected);
      if (rand01(uint32_t(t), seed, 6u) < expected - float(n)) {
        n++;
      }
      for (int i = 0; i < n; i++) {
        /* Keyed by triangle and number, so the spots stay put as the density changes. */
        add_spot(t, noise::hash(uint32_t(t), uint32_t(i)));
      }
    }
  }

  /* Placed in a random order, so keeping apart does not favor one side. */
  std::sort(spots.begin(), spots.end(), [](const ScatterSpot &a, const ScatterSpot &b) {
    return a.order < b.order;
  });

  /* Spots taken, in a grid of cells as large as the largest reach. */
  const int rows_num = int(params.items.size());
  float reach = params.spacing * 0.5f;
  if (params.no_overlap) {
    float largest = 0.0f;
    for (const int i : IndexRange(rows_num)) {
      const PlaceItem &item = params.items[i];
      if (item.valid) {
        largest = std::max(largest,
                           std::max(item.max.x - item.min.x, item.max.y - item.min.y) * 0.5f);
      }
    }
    reach += largest * std::max(look.scale_max, look.scale_min);
  }
  const bool check = reach > 1e-6f;
  const float cell = std::max(reach * 2.0f, 1e-4f);
  struct Taken {
    float3 co;
    float radius;
  };
  Map<int3, Vector<Taken>> grid;
  auto cell_of = [&](const float3 &co) {
    return int3(int(std::floor(co.x / cell)),
                int(std::floor(co.y / cell)),
                int(std::floor(co.z / cell)));
  };

  int slot = 0;
  for (const ScatterSpot &spot : spots) {
    if (result.size() >= params.max_count) {
      break;
    }
    const int item_index = params.picker->pick(slot);
    if (item_index < 0 || !params.items[item_index].valid) {
      slot++;
      continue;
    }
    const PlaceItem &item = params.items[item_index];
    const PieceSize size = piece_size(look, spot.key);
    float radius = params.spacing * 0.5f;
    if (params.no_overlap) {
      radius += std::max(item.max.x - item.min.x, item.max.y - item.min.y) * 0.5f * size.scale;
    }
    if (check) {
      const int3 c = cell_of(spot.co);
      bool free = true;
      for (int dx = -1; dx <= 1 && free; dx++) {
        for (int dy = -1; dy <= 1 && free; dy++) {
          for (int dz = -1; dz <= 1 && free; dz++) {
            const Vector<Taken> *taken = grid.lookup_ptr(c + int3(dx, dy, dz));
            if (taken == nullptr) {
              continue;
            }
            for (const Taken &other : *taken) {
              const float limit_dist = radius + other.radius;
              if (math::distance_squared(other.co, spot.co) < limit_dist * limit_dist) {
                free = false;
                break;
              }
            }
          }
        }
      }
      if (!free) {
        continue;
      }
      grid.lookup_or_add_default(c).append({spot.co, radius});
    }
    slot++;
    const float3 up = look.up == PlaceUp::Z ? float3(0.0f, 0.0f, 1.0f) : spot.normal;
    result.append({item_index,
                   piece_transform(item,
                                   look,
                                   spot.co,
                                   float3(1.0f, 0.0f, 0.0f),
                                   up,
                                   size,
                                   1.0f,
                                   0.0f,
                                   spot.key)});
  }
  return result;
}

/** \} */

}  // namespace blender::geometry
