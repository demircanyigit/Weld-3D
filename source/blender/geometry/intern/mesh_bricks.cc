/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup geo
 *
 * Bricks, cut mode: the mesh is cut into solid blocks the way Dice cuts it, but the cuts follow
 * courses of masonry instead of a grid.
 *
 * 1. The height of the mesh is split into courses of the block height.
 * 2. In every course, a grid of cells half a block deep marks where the mesh is.
 * 3. Each marked cell is part of a wall along X or along Y, the way its longer run of cells goes.
 *    Where both runs are about as long, at corners and in solid parts, the way swaps from course
 *    to course, so the blocks of two walls overlap at their corner in turns.
 * 4. Runs of cells along the same way are blocks of wall, split into layers one block deep, and
 *    cut into blocks at the joints of the pattern.
 * 5. The mesh is cut into courses, and each course into its blocks, splitting it in halves like
 *    Dice does so the time doesn't grow with the number of blocks times the size of the mesh.
 */

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>

#include "DNA_mesh_types.h"

#include "BLI_array.hh"
#include "BLI_bounds.hh"
#include "BLI_kdopbvh.hh"
#include "BLI_math_base.hh"
#include "BLI_math_geom.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_noise.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "BKE_attribute.hh"
#include "BKE_attribute_filter.hh"
#include "BKE_bvhutils.hh"
#include "BKE_customdata.hh"
#include "BKE_geometry_set.hh"
#include "BKE_mesh.hh"

#include "GEO_join_geometries.hh"
#include "GEO_mesh_bricks.hh"
#include "GEO_mesh_clip.hh"

#include "bmesh.hh"
#include "bmesh_tools.hh"

namespace blender::geometry {

namespace {

/** The way a wall of cells runs. */
enum class Way : uint8_t { None = 0, X = 1, Y = 2 };

struct Block {
  /** Where the block is cut. Sides along X and Y are always cut: with the gap where another
   * block is next to it, or just outside it otherwise. */
  float3 min;
  float3 max;
  /** Whether the bottom and top are cut, with the gap. Otherwise the course bounds them. */
  bool cut_bottom;
  bool cut_top;
  int course;
  uint32_t id;
};

struct Piece {
  Mesh *mesh;
  int block;
};

struct Grid {
  float2 origin;
  float cell;
  int nx;
  int ny;
  /** Bottom of every course, and the top of the last one. */
  Vector<float> z;
  /** Per course and cell: the mesh is there. */
  Array<bool> used;
  /** Per course and cell: the way of the wall the cell is part of. */
  Array<Way> way;

  int courses() const
  {
    return z.size() - 1;
  }

  int64_t index(const int k, const int x, const int y) const
  {
    return (int64_t(k) * ny + y) * nx + x;
  }

  bool is_used(const int k, const int x, const int y) const
  {
    if (k < 0 || k >= this->courses() || x < 0 || y < 0 || x >= nx || y >= ny) {
      return false;
    }
    return used[this->index(k, x, y)];
  }
};

struct CutData {
  const Mesh &mesh;
  const MeshBricksParams &params;
  const Grid &grid;
  Span<Block> blocks;
  /** Blocks of every course. */
  Span<Vector<int>> course_blocks;
  float threshold;
  /** How far an uncut side may reach past the next block's cut, to keep the sorting simple. */
  float reach;
};

/** True when the triangle touches the box: the boxes overlap and the plane crosses the box. */
bool triangle_touches_box(const float3 &a,
                          const float3 &b,
                          const float3 &c,
                          const float3 &box_min,
                          const float3 &box_max)
{
  const float3 tri_min = math::min(math::min(a, b), c);
  const float3 tri_max = math::max(math::max(a, b), c);
  for (const int i : IndexRange(3)) {
    if (tri_max[i] < box_min[i] || tri_min[i] > box_max[i]) {
      return false;
    }
  }
  const float3 normal = math::cross(b - a, c - a);
  const float3 center = (box_min + box_max) * 0.5f;
  const float3 half = (box_max - box_min) * 0.5f;
  const float radius = math::dot(math::abs(normal), half);
  return std::abs(math::dot(normal, center - a)) <= radius;
}

/** True when the point is inside the closed mesh: the nearest face looks away from it. */
bool point_inside(const bke::BVHTreeFromMesh &bvh, const float3 &point)
{
  BVHTreeNearest nearest;
  nearest.index = -1;
  nearest.dist_sq = FLT_MAX;
  BLI_bvhtree_find_nearest(
      bvh.tree, point, &nearest, bvh.nearest_callback, const_cast<bke::BVHTreeFromMesh *>(&bvh));
  if (nearest.index == -1) {
    return false;
  }
  return math::dot(point - float3(nearest.co), float3(nearest.no)) < 0.0f;
}

/** Every edge has two faces. */
bool mesh_is_closed(const Mesh &mesh)
{
  if (mesh.edges_num == 0) {
    return false;
  }
  Array<int> users(mesh.edges_num, 0);
  for (const int edge : mesh.corner_edges()) {
    users[edge]++;
  }
  return std::all_of(users.begin(), users.end(), [](const int count) { return count == 2; });
}

float random_float(const uint32_t seed, const uint32_t a, const uint32_t b, const uint32_t c)
{
  return noise::hash_to_float(seed, a, b, c);
}

/** Course boundaries along Z, from the bottom of the mesh. */
Vector<float> course_heights(const MeshBricksParams &params,
                             const float min,
                             const float max,
                             const uint32_t seed)
{
  const float height = std::max(params.size.z, 1e-4f);
  const float extent = max - min;
  int count = std::max(int(std::ceil(extent / height - 1e-4f)), 1);
  /* A very thin top course is added to the one under it. */
  if (count > 1 && extent - float(count - 1) * height < height * 0.35f) {
    count--;
  }
  const float random = params.pattern == BricksPattern::Ashlar ?
                           std::max(params.random_height, 0.7f) :
                           params.random_height;
  Vector<float> z(count + 1);
  z[0] = min;
  z[count] = max;
  for (int i = 1; i < count; i++) {
    const float jitter = (random_float(seed, 11, uint32_t(i), 0) - 0.5f) * random * height * 0.6f;
    float value = min + float(i) * height + jitter;
    value = std::max(value, z[i - 1] + height * 0.2f);
    value = std::min(value, max - height * 0.2f);
    z[i] = value;
  }
  return z;
}

/** Mark the cells the mesh is in. */
void fill_cells(const Mesh &mesh, const bool closed, const float threshold, Grid &grid)
{
  const Span<float3> positions = mesh.vert_positions();
  const Span<int3> tris = mesh.corner_tris();
  const Span<int> corner_verts = mesh.corner_verts();
  Array<Bounds<float3>> tri_bounds(tris.size());
  threading::parallel_for(tris.index_range(), 4096, [&](const IndexRange range) {
    for (const int i : range) {
      const float3 &a = positions[corner_verts[tris[i][0]]];
      const float3 &b = positions[corner_verts[tris[i][1]]];
      const float3 &c = positions[corner_verts[tris[i][2]]];
      tri_bounds[i] = {math::min(math::min(a, b), c), math::max(math::max(a, b), c)};
    }
  });

  const float cell = grid.cell;
  const auto cell_of = [&](const float value, const float origin, const int count) {
    return std::clamp(int(std::floor((value - origin) / cell)), 0, count - 1);
  };
  threading::parallel_for(IndexRange(grid.courses()), 1, [&](const IndexRange range) {
    for (const int k : range) {
      const float z0 = grid.z[k] + threshold;
      const float z1 = grid.z[k + 1] - threshold;
      for (const int i : tris.index_range()) {
        const Bounds<float3> &bounds = tri_bounds[i];
        if (bounds.max.z < z0 || bounds.min.z > z1) {
          continue;
        }
        const float3 &a = positions[corner_verts[tris[i][0]]];
        const float3 &b = positions[corner_verts[tris[i][1]]];
        const float3 &c = positions[corner_verts[tris[i][2]]];
        const int x0 = cell_of(bounds.min.x, grid.origin.x, grid.nx);
        const int x1 = cell_of(bounds.max.x, grid.origin.x, grid.nx);
        const int y0 = cell_of(bounds.min.y, grid.origin.y, grid.ny);
        const int y1 = cell_of(bounds.max.y, grid.origin.y, grid.ny);
        for (int y = y0; y <= y1; y++) {
          for (int x = x0; x <= x1; x++) {
            const int64_t index = grid.index(k, x, y);
            if (grid.used[index]) {
              continue;
            }
            /* Slightly smaller cells, so faces lying on a cell border don't mark both. */
            const float3 box_min(grid.origin.x + float(x) * cell + threshold,
                                 grid.origin.y + float(y) * cell + threshold,
                                 z0);
            const float3 box_max(grid.origin.x + float(x + 1) * cell - threshold,
                                 grid.origin.y + float(y + 1) * cell - threshold,
                                 z1);
            if (triangle_touches_box(a, b, c, box_min, box_max)) {
              grid.used[index] = true;
            }
          }
        }
      }
    }
  });

  if (!closed) {
    return;
  }
  const bke::BVHTreeFromMesh bvh = mesh.bvh_corner_tris();
  if (bvh.tree == nullptr) {
    return;
  }
  threading::parallel_for(IndexRange(grid.courses()), 1, [&](const IndexRange range) {
    for (const int k : range) {
      const float z = (grid.z[k] + grid.z[k + 1]) * 0.5f;
      for (const int y : IndexRange(grid.ny)) {
        for (const int x : IndexRange(grid.nx)) {
          const int64_t index = grid.index(k, x, y);
          if (grid.used[index]) {
            continue;
          }
          const float3 center(grid.origin.x + (float(x) + 0.5f) * cell,
                              grid.origin.y + (float(y) + 0.5f) * cell,
                              z);
          grid.used[index] = point_inside(bvh, center);
        }
      }
    }
  });
}

/** The way of the wall each cell is part of. */
void find_ways(Grid &grid)
{
  const int nx = grid.nx;
  const int ny = grid.ny;
  threading::parallel_for(IndexRange(grid.courses()), 1, [&](const IndexRange range) {
    Array<int> run_x(int64_t(nx) * ny);
    Array<int> run_y(int64_t(nx) * ny);
    for (const int k : range) {
      const auto used = [&](const int x, const int y) { return grid.used[grid.index(k, x, y)]; };
      for (const int y : IndexRange(ny)) {
        int x = 0;
        while (x < nx) {
          if (!used(x, y)) {
            run_x[int64_t(y) * nx + x] = 0;
            x++;
            continue;
          }
          int end = x;
          while (end < nx && used(end, y)) {
            end++;
          }
          for (int i = x; i < end; i++) {
            run_x[int64_t(y) * nx + i] = end - x;
          }
          x = end;
        }
      }
      for (const int x : IndexRange(nx)) {
        int y = 0;
        while (y < ny) {
          if (!used(x, y)) {
            run_y[int64_t(y) * nx + x] = 0;
            y++;
            continue;
          }
          int end = y;
          while (end < ny && used(x, end)) {
            end++;
          }
          for (int i = y; i < end; i++) {
            run_y[int64_t(i) * nx + x] = end - y;
          }
          y = end;
        }
      }
      for (const int y : IndexRange(ny)) {
        for (const int x : IndexRange(nx)) {
          const int64_t index = grid.index(k, x, y);
          if (!grid.used[index]) {
            grid.way[index] = Way::None;
            continue;
          }
          const int along_x = run_x[int64_t(y) * nx + x];
          const int along_y = run_y[int64_t(y) * nx + x];
          if (std::min(along_x, along_y) * 2 > std::max(along_x, along_y)) {
            /* A corner, or a solid part: take turns. */
            grid.way[index] = (k % 2 == 0) ? Way::X : Way::Y;
          }
          else {
            grid.way[index] = along_x > along_y ? Way::X : Way::Y;
          }
        }
      }
    }
  });
}

/** Cells of one way, the same along the wall in rows next to each other. */
struct Band {
  /** 0 when the wall runs along X, 1 along Y. */
  int axis;
  int course;
  /** Cells along the wall, and across it. */
  int i0, i1;
  int j0, j1;
};

Vector<Band> find_bands(const Grid &grid)
{
  Vector<Band> bands;
  for (const int k : IndexRange(grid.courses())) {
    for (const int axis : {0, 1}) {
      const Way way = axis == 0 ? Way::X : Way::Y;
      const int count_along = axis == 0 ? grid.nx : grid.ny;
      const int count_across = axis == 0 ? grid.ny : grid.nx;
      const auto has_way = [&](const int i, const int j) {
        const int x = axis == 0 ? i : j;
        const int y = axis == 0 ? j : i;
        return grid.way[grid.index(k, x, y)] == way;
      };
      /* Bands that reached the row before. */
      Vector<int> open;
      for (const int j : IndexRange(count_across)) {
        Vector<int> next_open;
        int i = 0;
        while (i < count_along) {
          if (!has_way(i, j)) {
            i++;
            continue;
          }
          int end = i;
          while (end < count_along && has_way(end, j)) {
            end++;
          }
          bool extended = false;
          for (const int band_index : open) {
            Band &band = bands[band_index];
            if (band.i0 == i && band.i1 == end && band.j1 == j) {
              band.j1 = j + 1;
              next_open.append(band_index);
              extended = true;
              break;
            }
          }
          if (!extended) {
            next_open.append(int(bands.size()));
            bands.append({axis, k, i, end, j, j + 1});
          }
          i = end;
        }
        open = std::move(next_open);
      }
    }
  }
  return bands;
}

/** A piece of a band between two joints. */
struct Span1 {
  float t0;
  float t1;
  /** Goes through all the layers of the band. */
  bool through;
};

/** The pieces of one layer of a band between \a ta and \a tb along the wall. */
Vector<Span1> layer_spans(const MeshBricksParams &params,
                          const uint32_t seed,
                          const int course,
                          const int axis,
                          const int layer_key,
                          const int layer,
                          const float ta,
                          const float tb)
{
  const float length = std::max(params.size.x, 1e-4f);
  struct Step {
    float length;
    bool through;
  };
  Vector<Step, 2> steps;
  float offset = 0.0f;
  float random = params.random_length;
  const bool odd = (course & 1) != 0;
  switch (params.pattern) {
    case BricksPattern::Running:
      steps.append({length, false});
      offset = ((course + layer) & 1) ? length * 0.5f : 0.0f;
      break;
    case BricksPattern::Stack:
      steps.append({length, false});
      break;
    case BricksPattern::Flemish:
      steps.append({length, false});
      steps.append({length * 0.5f, true});
      offset = odd ? length * 0.75f : 0.0f;
      break;
    case BricksPattern::English:
      if (odd) {
        steps.append({length * 0.5f, true});
        offset = length * 0.25f;
      }
      else {
        steps.append({length, false});
        offset = (layer & 1) ? length * 0.5f : 0.0f;
      }
      break;
    case BricksPattern::Ashlar:
      steps.append({length, false});
      offset = random_float(seed, 23, uint32_t(course), uint32_t(axis * 7919 + layer_key)) *
               length;
      random = std::max(random, 0.7f);
      break;
  }
  float period = 0.0f;
  float shortest = FLT_MAX;
  for (const Step &step : steps) {
    period += step.length;
    shortest = std::min(shortest, step.length);
  }

  /* Joints with the kind of piece that starts at each. */
  struct Joint {
    float t;
    bool through;
  };
  Vector<Joint> joints;
  const int first = int(std::floor((ta - offset) / period)) - 1;
  const int last = int(std::floor((tb - offset) / period)) + 1;
  const uint32_t row_seed = noise::hash(
      seed, uint32_t(course), uint32_t(axis * 7919 + layer_key) + 31u);
  for (int p = first; p <= last; p++) {
    float t = float(p) * period + offset;
    for (const int s : steps.index_range()) {
      const uint32_t index = uint32_t(p * int(steps.size()) + s);
      const float jitter = (noise::hash_to_float(row_seed, index) - 0.5f) * random * shortest *
                           0.8f;
      joints.append({t + jitter, steps[s].through});
      t += steps[s].length;
    }
    if (joints.size() > 100000) {
      break;
    }
  }

  Vector<Span1> spans;
  for (int i = 0; i + 1 < joints.size(); i++) {
    const float t0 = std::max(joints[i].t, ta);
    const float t1 = std::min(joints[i + 1].t, tb);
    if (t1 > t0) {
      spans.append({t0, t1, joints[i].through});
    }
  }

  /* Short pieces join the shorter piece next to them. */
  const float min_length = std::min(shortest * 0.3f, (tb - ta) * 0.5f);
  while (spans.size() > 1) {
    int shortest_index = -1;
    float shortest_length = min_length;
    for (const int i : spans.index_range()) {
      const float span_length = spans[i].t1 - spans[i].t0;
      if (span_length < shortest_length) {
        shortest_length = span_length;
        shortest_index = i;
      }
    }
    if (shortest_index == -1) {
      break;
    }
    const int i = shortest_index;
    int other;
    if (i == 0) {
      other = 1;
    }
    else if (i == spans.size() - 1) {
      other = i - 1;
    }
    else {
      const float before = spans[i - 1].t1 - spans[i - 1].t0;
      const float after = spans[i + 1].t1 - spans[i + 1].t0;
      other = before < after ? i - 1 : i + 1;
    }
    const int low = std::min(i, other);
    const int high = std::max(i, other);
    spans[low].t1 = spans[high].t1;
    spans[low].through = spans[other].through;
    spans.remove(high);
  }
  return spans;
}

/** Share of the cells in the ranges that are used, the course may be out of range. */
float used_share(const Grid &grid,
                 const int k,
                 const int axis,
                 const int i0,
                 const int i1,
                 const int j0,
                 const int j1)
{
  int count = 0;
  int total = 0;
  for (int j = j0; j < j1; j++) {
    for (int i = i0; i < i1; i++) {
      const int x = axis == 0 ? i : j;
      const int y = axis == 0 ? j : i;
      total++;
      count += grid.is_used(k, x, y) ? 1 : 0;
    }
  }
  return total == 0 ? 0.0f : float(count) / float(total);
}

/** The blocks of all bands. Returns false when there are too many. */
bool make_blocks(const MeshBricksParams &params,
                 const Grid &grid,
                 const Span<Band> bands,
                 const uint32_t seed,
                 const float reach,
                 Vector<Block> &r_blocks)
{
  const float cell = grid.cell;
  const float half_gap = std::max(params.gap, 0.0f) * 0.5f;
  const float depth = std::max(params.size.y, 1e-4f);
  for (const Band &band : bands) {
    const int a = band.axis;
    const int b = 1 - a;
    const float origin_a = grid.origin[a];
    const float origin_b = grid.origin[b];
    const float ta = origin_a + float(band.i0) * cell;
    const float tb = origin_a + float(band.i1) * cell;
    const float sa = origin_b + float(band.j0) * cell;
    const float sb = origin_b + float(band.j1) * cell;
    const int layers = std::max(int(std::round((sb - sa) / depth)), 1);
    const float layer_width = (sb - sa) / float(layers);

    /* Cells overlapping a range along an axis. */
    const auto cells_of = [&](const float v0, const float v1, const float origin) {
      const float eps = 1e-4f;
      return int2(int(std::floor((v0 - origin) / cell + eps)),
                  int(std::ceil((v1 - origin) / cell - eps)));
    };

    for (const int layer : IndexRange(layers)) {
      const Vector<Span1> spans = layer_spans(
          params, seed, band.course, a, band.j0, layer, ta, tb);
      for (const Span1 &span : spans) {
        if (span.through && layer > 0 && layers > 1) {
          continue;
        }
        const bool whole = span.through && layers > 1;
        const float s0 = whole ? sa : sa + float(layer) * layer_width;
        const float s1 = whole ? sb : sa + float(layer + 1) * layer_width;
        const int2 cells_a = cells_of(span.t0, span.t1, origin_a);
        const int2 cells_b = cells_of(s0, s1, origin_b);

        /* Sides next to another block are cut with the gap, the others just outside. */
        const bool at_start = span.t0 <= ta + cell * 1e-3f;
        const bool at_end = span.t1 >= tb - cell * 1e-3f;
        const bool at_side_low = s0 <= sa + cell * 1e-3f;
        const bool at_side_high = s1 >= sb - cell * 1e-3f;
        const int k = band.course;
        const bool cut_start = !at_start || used_share(grid,
                                                       k,
                                                       a,
                                                       band.i0 - 1,
                                                       band.i0,
                                                       cells_b.x,
                                                       cells_b.y) >= 0.5f;
        const bool cut_end = !at_end || used_share(grid,
                                                   k,
                                                   a,
                                                   band.i1,
                                                   band.i1 + 1,
                                                   cells_b.x,
                                                   cells_b.y) >= 0.5f;
        const bool cut_low = !at_side_low || used_share(grid,
                                                        k,
                                                        a,
                                                        cells_a.x,
                                                        cells_a.y,
                                                        band.j0 - 1,
                                                        band.j0) >= 0.5f;
        const bool cut_high = !at_side_high || used_share(grid,
                                                          k,
                                                          a,
                                                          cells_a.x,
                                                          cells_a.y,
                                                          band.j1,
                                                          band.j1 + 1) >= 0.5f;

        Block block;
        block.min[a] = cut_start ? span.t0 + half_gap : span.t0 - reach;
        block.max[a] = cut_end ? span.t1 - half_gap : span.t1 + reach;
        block.min[b] = cut_low ? s0 + half_gap : s0 - reach;
        block.max[b] = cut_high ? s1 - half_gap : s1 + reach;
        block.cut_bottom =
            used_share(grid, k - 1, a, cells_a.x, cells_a.y, cells_b.x, cells_b.y) >= 0.5f;
        block.cut_top = used_share(grid, k + 1, a, cells_a.x, cells_a.y, cells_b.x, cells_b.y) >=
                        0.5f;
        block.min.z = block.cut_bottom ? grid.z[k] + half_gap : grid.z[k];
        block.max.z = block.cut_top ? grid.z[k + 1] - half_gap : grid.z[k + 1];
        block.course = k;
        block.id = noise::hash(seed, uint32_t(r_blocks.size()), 97u);
        if (block.max.x <= block.min.x || block.max.y <= block.min.y ||
            block.max.z <= block.min.z)
        {
          continue;
        }
        if (r_blocks.size() >= mesh_bricks_max_blocks) {
          return false;
        }
        r_blocks.append(block);
      }
    }
  }
  return true;
}

void clip(const CutData &data,
          BMesh *bm,
          const int axis,
          const float position,
          const BMeshClipKeep keep)
{
  BMeshClipParams params;
  params.plane_co = float3(0.0f);
  params.plane_co[axis] = position;
  params.plane_no = float3(0.0f);
  params.plane_no[axis] = 1.0f;
  params.threshold = data.threshold;
  params.keep = keep;
  params.fill = true;
  if (data.params.interior_id) {
    params.interior_face_offset = CustomData_get_offset_named(
        &bm->pdata, CD_PROP_BOOL, data.params.interior_id->c_str());
  }
  bmesh_clip(bm, params);
}

/** Small random turn and shrink of the block around its middle. */
void make_uneven(const MeshBricksParams &params, const uint32_t id, BMesh *bm)
{
  if (params.uneven <= 0.0f || bm->totvert == 0) {
    return;
  }
  float3 center(0.0f);
  BMIter iter;
  BMVert *vert;
  BM_ITER_MESH (vert, &iter, bm, BM_VERTS_OF_MESH) {
    center += float3(vert->co);
  }
  center /= float(bm->totvert);

  const float3 axis_random(noise::hash_to_float(id, 1u) - 0.5f,
                           noise::hash_to_float(id, 2u) - 0.5f,
                           noise::hash_to_float(id, 3u) - 0.5f);
  const float3 axis = math::length_squared(axis_random) > 1e-8f ?
                          math::normalize(axis_random) :
                          float3(0.0f, 0.0f, 1.0f);
  /* Up to about four degrees. */
  const float angle = params.uneven * 0.07f * (noise::hash_to_float(id, 4u) * 2.0f - 1.0f);
  const float scale = 1.0f - params.uneven * 0.06f * noise::hash_to_float(id, 5u);
  const float c = std::cos(angle);
  const float s = std::sin(angle);
  BM_ITER_MESH (vert, &iter, bm, BM_VERTS_OF_MESH) {
    const float3 p = (float3(vert->co) - center) * scale;
    /* Turn around the axis (Rodrigues). */
    const float3 turned = p * c + math::cross(axis, p) * s +
                          axis * math::dot(axis, p) * (1.0f - c);
    copy_v3_v3(vert->co, center + turned);
  }
}

/** Round the sharp edges of the block. */
void bevel_block(const MeshBricksParams &params, BMesh *bm)
{
  if (params.bevel <= 0.0f || bm->totedge == 0) {
    return;
  }
  BM_mesh_normals_update(bm);
  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);
  bool any = false;
  BMIter iter;
  BMEdge *edge;
  BM_ITER_MESH (edge, &iter, bm, BM_EDGES_OF_MESH) {
    BMLoop *l_a, *l_b;
    if (BM_edge_loop_pair(edge, &l_a, &l_b) && dot_v3v3(l_a->f->no, l_b->f->no) < 0.866f) {
      BM_elem_flag_enable(edge, BM_ELEM_TAG);
      BM_elem_flag_enable(edge->v1, BM_ELEM_TAG);
      BM_elem_flag_enable(edge->v2, BM_ELEM_TAG);
      any = true;
    }
  }
  if (!any) {
    return;
  }
  BM_mesh_bevel(bm,
                params.bevel,
                /*offset_type*/ 0,
                /*profile_type*/ 0,
                std::clamp(params.bevel_segments, 1, 16),
                /*profile*/ 0.5f,
                /*affect_type (edges)*/ true,
                /*use_weights*/ false,
                /*limit_offset*/ true,
                /*dvert*/ nullptr,
                /*vertex_group*/ -1,
                /*mat*/ -1,
                /*loop_slide*/ true,
                /*mark_seam*/ false,
                /*mark_sharp*/ false,
                /*harden_normals*/ false,
                /*face_strength_mode*/ 0,
                /*miter_outer*/ 0,
                /*miter_inner*/ 0,
                /*spread*/ 0.1f,
                /*custom_profile*/ nullptr,
                /*vmesh_method*/ 0,
                /*bweight_offset_vert*/ -1,
                /*bweight_offset_edge*/ -1);
}

/** Cut one block out of \a bm, which this takes. */
void make_piece(const CutData &data, BMesh *bm, const int block_index, Vector<Piece> &r_pieces)
{
  const Block &block = data.blocks[block_index];
  for (const int axis : {0, 1}) {
    if (bm->totvert == 0) {
      break;
    }
    clip(data, bm, axis, block.min[axis], BMeshClipKeep::Above);
    if (bm->totvert == 0) {
      break;
    }
    clip(data, bm, axis, block.max[axis], BMeshClipKeep::Below);
  }
  if (block.cut_bottom && bm->totvert != 0) {
    clip(data, bm, 2, block.min.z, BMeshClipKeep::Above);
  }
  if (block.cut_top && bm->totvert != 0) {
    clip(data, bm, 2, block.max.z, BMeshClipKeep::Below);
  }
  if (bm->totvert == 0 || bm->totface == 0) {
    BM_mesh_free(bm);
    return;
  }
  bevel_block(data.params, bm);
  make_uneven(data.params, block.id, bm);
  Mesh *mesh = BKE_mesh_from_bmesh_for_eval_nomain(bm, nullptr, &data.mesh);
  BM_mesh_free(bm);
  r_pieces.append({mesh, block_index});
}

/** Split \a bm in two at \a position, keeping \a bm as the part below. */
BMesh *split(const CutData &data, BMesh *bm, const int axis, const float position)
{
  BMesh *above = BM_mesh_copy(bm);
  clip(data, bm, axis, position, BMeshClipKeep::Below);
  clip(data, above, axis, position, BMeshClipKeep::Above);
  return above;
}

/** Cut the blocks of one course out of \a bm, which this takes. */
void cut_blocks(const CutData &data, BMesh *bm, Vector<int> blocks, Vector<Piece> &r_pieces)
{
  if (bm->totvert == 0 || blocks.is_empty()) {
    BM_mesh_free(bm);
    return;
  }
  if (blocks.size() == 1) {
    make_piece(data, bm, blocks[0], r_pieces);
    return;
  }

  /* Look for a line between the blocks, as near the middle of the list as possible. */
  const float eps = data.reach * 2.0f + data.threshold;
  int best_axis = -1;
  float best_position = 0.0f;
  int best_balance = INT_MAX;
  for (const int axis : {0, 1}) {
    Vector<int> sorted = blocks;
    std::sort(sorted.begin(), sorted.end(), [&](const int a, const int b) {
      return data.blocks[a].min[axis] < data.blocks[b].min[axis];
    });
    float end = -FLT_MAX;
    for (int i = 0; i + 1 < sorted.size(); i++) {
      end = std::max(end, data.blocks[sorted[i]].max[axis]);
      const float next = data.blocks[sorted[i + 1]].min[axis];
      if (end <= next + eps) {
        const int balance = std::abs((i + 1) - int(sorted.size()) / 2);
        if (balance < best_balance) {
          best_balance = balance;
          best_axis = axis;
          best_position = (std::min(end, next) + std::max(end, next)) * 0.5f;
        }
      }
    }
  }

  Vector<int> below;
  Vector<int> above;
  if (best_axis != -1) {
    for (const int b : blocks) {
      const float middle = (data.blocks[b].min[best_axis] + data.blocks[b].max[best_axis]) * 0.5f;
      (middle < best_position ? below : above).append(b);
    }
  }
  if (below.is_empty() || above.is_empty()) {
    /* No clean line: each block from its own copy. */
    for (const int i : blocks.index_range()) {
      BMesh *copy = i + 1 < blocks.size() ? BM_mesh_copy(bm) : bm;
      make_piece(data, copy, blocks[i], r_pieces);
    }
    return;
  }

  BMesh *bm_above = split(data, bm, best_axis, best_position);
  Vector<Piece> pieces_below;
  Vector<Piece> pieces_above;
  threading::parallel_invoke(
      data.mesh.verts_num > 1024,
      [&]() { cut_blocks(data, bm, std::move(below), pieces_below); },
      [&]() { cut_blocks(data, bm_above, std::move(above), pieces_above); });
  r_pieces.extend(pieces_below);
  r_pieces.extend(pieces_above);
}

/** Cut \a bm into the courses from \a k0 to \a k1, and those into blocks. Takes \a bm. */
void cut_courses(
    const CutData &data, BMesh *bm, const int k0, const int k1, Vector<Piece> &r_pieces)
{
  if (bm->totvert == 0) {
    BM_mesh_free(bm);
    return;
  }
  if (k1 - k0 == 1) {
    cut_blocks(data, bm, data.course_blocks[k0], r_pieces);
    return;
  }
  const int middle = k0 + (k1 - k0) / 2;
  BMesh *bm_above = split(data, bm, 2, data.grid.z[middle]);
  Vector<Piece> pieces_below;
  Vector<Piece> pieces_above;
  threading::parallel_invoke(
      data.mesh.verts_num > 1024,
      [&]() { cut_courses(data, bm, k0, middle, pieces_below); },
      [&]() { cut_courses(data, bm_above, middle, k1, pieces_above); });
  r_pieces.extend(pieces_below);
  r_pieces.extend(pieces_above);
}

}  // namespace

Mesh *mesh_bricks(const Mesh &mesh, const MeshBricksParams &params, MeshBricksError &r_error)
{
  r_error = MeshBricksError::None;
  const std::optional<Bounds<float3>> bounds = mesh.bounds_min_max();
  if (!bounds || mesh.faces_num == 0) {
    return nullptr;
  }
  const float threshold = std::max(math::distance(bounds->min, bounds->max) * 1e-5f, 1e-7f);
  const uint32_t seed = noise::hash(uint32_t(params.seed), 0x6b72u);
  const bool closed = mesh_is_closed(mesh);

  Grid grid;
  grid.cell = std::max(params.size.y * 0.5f, 1e-4f);
  grid.z = course_heights(params, bounds->min.z, bounds->max.z, seed);
  /* Closed meshes line up with the cells from their lowest corner, so straight walls fill whole
   * cells. Other meshes are moved half a cell in, so a flat wall is in the middle of its cells. */
  const float2 extent = bounds->max.xy() - bounds->min.xy();
  const float shift = closed ? 0.0f : grid.cell * 0.5f;
  grid.origin = bounds->min.xy() - float2(shift);
  grid.nx = std::max(int(std::ceil((extent.x + shift * 2.0f) / grid.cell - 1e-4f)), 1);
  grid.ny = std::max(int(std::ceil((extent.y + shift * 2.0f) / grid.cell - 1e-4f)), 1);
  const int64_t cells_num = int64_t(grid.nx) * grid.ny * grid.courses();
  if (cells_num > mesh_bricks_max_cells) {
    r_error = MeshBricksError::TooManyCells;
    return nullptr;
  }
  grid.used = Array<bool>(cells_num, false);
  grid.way = Array<Way>(cells_num, Way::None);
  fill_cells(mesh, closed, threshold, grid);
  find_ways(grid);

  const Vector<Band> bands = find_bands(grid);
  const float reach = std::max(std::max(params.gap, 0.0f) * 0.25f, threshold * 50.0f);
  Vector<Block> blocks;
  if (!make_blocks(params, grid, bands, seed, reach, blocks)) {
    r_error = MeshBricksError::TooManyBlocks;
  }
  if (blocks.is_empty()) {
    return BKE_mesh_new_nomain_from_template(&mesh, 0, 0, 0, 0);
  }
  Array<Vector<int>> course_blocks(grid.courses());
  for (const int i : blocks.index_range()) {
    course_blocks[blocks[i].course].append(i);
  }

  BMeshCreateParams create_params{};
  create_params.use_toolflags = true;
  BMeshFromMeshParams convert_params{};
  convert_params.calc_face_normal = true;
  convert_params.calc_vert_normal = true;
  BMesh *bm = BKE_mesh_to_bmesh_ex(&mesh, &create_params, &convert_params);
  if (params.interior_id) {
    BM_data_layer_add_named(bm, &bm->pdata, CD_PROP_BOOL, params.interior_id->c_str());
  }

  const CutData data{mesh, params, grid, blocks, course_blocks, threshold, reach};
  Vector<Piece> pieces;
  cut_courses(data, bm, 0, grid.courses(), pieces);
  if (pieces.is_empty()) {
    return BKE_mesh_new_nomain_from_template(&mesh, 0, 0, 0, 0);
  }
  std::sort(pieces.begin(), pieces.end(), [](const Piece &a, const Piece &b) {
    return a.block < b.block;
  });

  Array<int> vert_offsets(pieces.size() + 1);
  vert_offsets[0] = 0;
  Array<bke::GeometrySet> geometries(pieces.size());
  for (const int i : pieces.index_range()) {
    vert_offsets[i + 1] = vert_offsets[i] + pieces[i].mesh->verts_num;
    geometries[i] = bke::GeometrySet::from_mesh(pieces[i].mesh);
  }
  bke::GeometrySet joined = join_geometries(geometries, bke::AttributeFilter::default_filter());
  geometries = {};
  Mesh *result = joined.get_component_for_write<bke::MeshComponent>().release();
  if (result == nullptr) {
    return BKE_mesh_new_nomain_from_template(&mesh, 0, 0, 0, 0);
  }

  if (params.piece_index_id || params.piece_random_id) {
    bke::MutableAttributeAccessor attributes = result->attributes_for_write();
    if (params.piece_index_id) {
      bke::SpanAttributeWriter<int> piece = attributes.lookup_or_add_for_write_only_span<int>(
          *params.piece_index_id, bke::AttrDomain::Point);
      if (piece) {
        for (const int i : pieces.index_range()) {
          piece.span.slice(vert_offsets[i], vert_offsets[i + 1] - vert_offsets[i]).fill(i);
        }
        piece.finish();
      }
    }
    if (params.piece_random_id) {
      bke::SpanAttributeWriter<float> random = attributes.lookup_or_add_for_write_only_span<float>(
          *params.piece_random_id, bke::AttrDomain::Point);
      if (random) {
        for (const int i : pieces.index_range()) {
          const float value = noise::hash_to_float(blocks[pieces[i].block].id, 211u);
          random.span.slice(vert_offsets[i], vert_offsets[i + 1] - vert_offsets[i]).fill(value);
        }
        random.finish();
      }
    }
  }
  return result;
}

}  // namespace blender::geometry
