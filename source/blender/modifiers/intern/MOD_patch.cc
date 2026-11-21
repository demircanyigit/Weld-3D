/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Patch: cuts a surface into pieces, Voronoi cells around random points on the surface, boxes of a
 * grid, or the faces of a cut object, and turns them into patches with stitches across their
 * borders.
 *
 * Overlay keeps the surface whole and lays some of the pieces on top of it, a little bigger than
 * their cell. Split cuts the whole surface into pieces, with a gap between them and stitches along
 * the seams.
 *
 * The faces are cut by clipping each one against the planes of the cells it touches, so the
 * borders are straight and exact even on a coarse mesh. Pieces of the same cell are welded back
 * together, and their attributes are interpolated from the face they were cut from.
 */

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <numeric>

#include "BLI_array.hh"
#include "BLI_kdtree.hh"
#include "BLI_map.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_noise.hh"
#include "BLI_ordered_edge.hh"
#include "BLI_set.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "DNA_collection_types.h"
#include "DNA_layer_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "BKE_attribute.hh"
#include "BKE_attribute_math.hh"
#include "BKE_collection.hh"
#include "BKE_geometry_set.hh"
#include "BKE_geometry_set_instances.hh"
#include "BKE_instances.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_query.hh"
#include "BKE_mesh.h"
#include "BKE_mesh.hh"
#include "BKE_modifier.hh"

#include "DEG_depsgraph_build.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "GEO_realize_instances.hh"

#include "MOD_modifiertypes.hh"
#include "MOD_ui_common.hh"
#include "MOD_util.hh"

namespace blender {

static constexpr int MAX_CELLS = 200000;
static constexpr int MAX_STITCHES = 500000;
static constexpr int NO_LABEL = -1;
/** An edge cut by the cut object. Its neighbor is found once the piece is done. */
static constexpr int CUT_LABEL = -2;

static void init_data(ModifierData *md)
{
  PatchModifierData *data = reinterpret_cast<PatchModifierData *>(md);
  INIT_DEFAULT_STRUCT_AFTER(data, modifier);
}

static void required_data_mask(ModifierData *md, CustomData_MeshMasks *r_cddata_masks)
{
  PatchModifierData *pmd = reinterpret_cast<PatchModifierData *>(md);
  if (pmd->vertex_group[0] != '\0') {
    r_cddata_masks->vmask |= CD_MASK_MDEFORMVERT;
  }
}

static void foreach_ID_link(ModifierData *md, Object *ob, IDWalkFunc walk, void *user_data)
{
  PatchModifierData *pmd = reinterpret_cast<PatchModifierData *>(md);
  walk(user_data, ob, reinterpret_cast<ID **>(&pmd->stitch_object), IDWALK_CB_NOP);
  walk(user_data, ob, reinterpret_cast<ID **>(&pmd->stitch_collection), IDWALK_CB_USER);
  walk(user_data, ob, reinterpret_cast<ID **>(&pmd->cut_object), IDWALK_CB_NOP);
  PatchModifierData *pick_data = reinterpret_cast<PatchModifierData *>(md);
  walk(user_data, ob, reinterpret_cast<ID **>(&pick_data->patch_material_pick), IDWALK_CB_USER);
  walk(user_data, ob, reinterpret_cast<ID **>(&pick_data->stitch_material_pick), IDWALK_CB_USER);
}

static void update_depsgraph(ModifierData *md, const ModifierUpdateDepsgraphContext *ctx)
{
  PatchModifierData *pmd = reinterpret_cast<PatchModifierData *>(md);
  if (pmd->stitch_object != nullptr) {
    DEG_add_object_relation(ctx->node, pmd->stitch_object, DEG_OB_COMP_GEOMETRY, "Patch Modifier");
    DEG_add_object_relation(
        ctx->node, pmd->stitch_object, DEG_OB_COMP_TRANSFORM, "Patch Modifier");
  }
  if (pmd->stitch_collection != nullptr) {
    DEG_add_collection_geometry_relation(ctx->node, pmd->stitch_collection, "Patch Modifier");
  }
  if (pmd->cut_object != nullptr && pmd->layout == MOD_PATCH_LAYOUT_OBJECT) {
    DEG_add_object_relation(ctx->node, pmd->cut_object, DEG_OB_COMP_GEOMETRY, "Patch Modifier");
    DEG_add_object_relation(ctx->node, pmd->cut_object, DEG_OB_COMP_TRANSFORM, "Patch Modifier");
    DEG_add_depends_on_transform_relation(ctx->node, "Patch Modifier");
  }
}

static float rand01(const uint32_t a, const uint32_t b, const uint32_t c)
{
  return noise::hash_to_float(a, b, c);
}

/* -------------------------------------------------------------------- */
/** \name Islands and cells
 * \{ */

static int find_root(MutableSpan<int> parent, int i)
{
  while (parent[i] != i) {
    parent[i] = parent[parent[i]];
    i = parent[i];
  }
  return i;
}

/** The loose part of each face. Returns the number of parts. */
static int face_islands(const Mesh &mesh, MutableSpan<int> r_island)
{
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  Array<int> parent(mesh.verts_num);
  std::iota(parent.begin(), parent.end(), 0);
  for (const int f : faces.index_range()) {
    const int first = find_root(parent, corner_verts[faces[f].first()]);
    for (const int c : faces[f].drop_front(1)) {
      const int r = find_root(parent, corner_verts[c]);
      if (r != first) {
        parent[std::max(r, first)] = std::min(r, first);
      }
    }
  }
  Map<int, int> ids;
  for (const int f : faces.index_range()) {
    const int r = find_root(parent, corner_verts[faces[f].first()]);
    r_island[f] = ids.lookup_or_add(r, int(ids.size()));
  }
  return std::max(int(ids.size()), 1);
}

/** Random points spread evenly over the area of the surface, with the face of each. */
static Vector<float3> sample_surface(const Mesh &mesh,
                                     const int count,
                                     const uint32_t seed,
                                     Vector<int> &r_faces)
{
  const Span<float3> positions = mesh.vert_positions();
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<int3> tris = mesh.corner_tris();
  const Span<int> tri_faces = mesh.corner_tri_faces();
  Vector<float3> points;
  r_faces.clear();
  if (tris.is_empty() || count <= 0) {
    return points;
  }
  Array<float> cumulative(tris.size() + 1);
  cumulative[0] = 0.0f;
  for (const int t : tris.index_range()) {
    const float3 &a = positions[corner_verts[tris[t][0]]];
    const float3 &b = positions[corner_verts[tris[t][1]]];
    const float3 &c = positions[corner_verts[tris[t][2]]];
    cumulative[t + 1] = cumulative[t] + 0.5f * math::length(math::cross(b - a, c - a));
  }
  const float total = cumulative.last();
  if (total <= 0.0f) {
    return points;
  }
  points.reserve(count);
  for (const int i : IndexRange(count)) {
    const float r = rand01(uint32_t(i), seed, 1) * total;
    int t = int(std::upper_bound(cumulative.begin() + 1, cumulative.end(), r) -
                (cumulative.begin() + 1));
    t = std::clamp(t, 0, int(tris.size()) - 1);
    float u = rand01(uint32_t(i), seed, 2);
    float v = rand01(uint32_t(i), seed, 3);
    if (u + v > 1.0f) {
      u = 1.0f - u;
      v = 1.0f - v;
    }
    const float3 &a = positions[corner_verts[tris[t][0]]];
    const float3 &b = positions[corner_verts[tris[t][1]]];
    const float3 &c = positions[corner_verts[tris[t][2]]];
    points.append(a + (b - a) * u + (c - a) * v);
    r_faces.append(tri_faces[t]);
  }
  return points;
}

/** One tree of points per loose part, holding indices into the full list. */
static Array<KDTree_3d *> island_trees(const Span<float3> points,
                                       const Span<int> point_island,
                                       const int islands)
{
  Array<int> num(islands, 0);
  for (const int i : point_island) {
    num[i]++;
  }
  Array<KDTree_3d *> trees(islands);
  for (const int i : IndexRange(islands)) {
    trees[i] = kdtree_3d_new(std::max(num[i], 1));
  }
  for (const int p : points.index_range()) {
    kdtree_3d_insert(trees[point_island[p]], p, points[p]);
  }
  for (KDTree_3d *tree : trees) {
    kdtree_3d_balance(tree);
  }
  return trees;
}

static void free_trees(MutableSpan<KDTree_3d *> trees)
{
  for (KDTree_3d *&tree : trees) {
    if (tree != nullptr) {
      kdtree_3d_free(tree);
      tree = nullptr;
    }
  }
}

/** Moves each seed to the middle of the surface it owns, so the cells get more even. The seeds
 * are in the stretched space of the Voronoi Scale, given by \a inv_scale. */
static void relax_seeds(const Mesh &mesh,
                        MutableSpan<float3> seeds,
                        const Span<int> seed_island,
                        const Span<int> face_island,
                        const int islands,
                        const int steps,
                        const uint32_t seed,
                        const float3 &inv_scale)
{
  if (steps <= 0 || seeds.size() < 2) {
    return;
  }
  const int sample_num = std::clamp(int(seeds.size()) * 16, 4000, 200000);
  Vector<int> sample_faces;
  Vector<float3> samples = sample_surface(mesh, sample_num, seed + 7777u, sample_faces);
  for (float3 &p : samples) {
    p *= inv_scale;
  }
  if (samples.is_empty()) {
    return;
  }
  Array<float3> sum(seeds.size());
  Array<int> num(seeds.size());
  Array<int> owner(samples.size());
  for ([[maybe_unused]] const int step : IndexRange(steps)) {
    Array<KDTree_3d *> trees = island_trees(seeds, seed_island, islands);
    sum.fill(float3(0.0f));
    num.fill(0);
    for (const int s : samples.index_range()) {
      const int i = kdtree_3d_find_nearest(
          trees[face_island[sample_faces[s]]], samples[s], nullptr);
      owner[s] = i;
      if (i >= 0) {
        sum[i] += samples[s];
        num[i]++;
      }
    }
    free_trees(trees);
    /* The middle may be off the surface, so snap to the nearest sample of the same cell. */
    Array<float> best(seeds.size(), FLT_MAX);
    Array<float3> mean(seeds.size());
    for (const int i : seeds.index_range()) {
      mean[i] = num[i] > 0 ? sum[i] / float(num[i]) : seeds[i];
    }
    Array<float3> snapped(seeds.as_span());
    for (const int s : samples.index_range()) {
      const int i = owner[s];
      if (i < 0) {
        continue;
      }
      const float d = math::distance_squared(samples[s], mean[i]);
      if (d < best[i]) {
        best[i] = d;
        snapped[i] = samples[s];
      }
    }
    for (const int i : seeds.index_range()) {
      seeds[i] = snapped[i];
    }
  }
}

/** A half space a piece is clipped to: points with `dot(normal, p) <= offset` are kept. */
struct Plane {
  float3 normal;
  float offset;
  /** The cell on the other side, written on the edges this plane makes. */
  int label;
  /** Not moved by the margin or gap: it splits a concave face of the cut object. */
  bool fixed = false;
};

static float3 newell_normal(const Span<float3> poly)
{
  float3 n(0.0f);
  for (const int i : poly.index_range()) {
    n += math::cross(poly[i], poly[(i + 1) % poly.size()]);
  }
  return n;
}

/** A node of a BSP tree. In front of the plane is outside, behind it inside. */
struct BspNode {
  float3 normal;
  float offset;
  int front = -1;
  int back = -1;
};

/** A closed shape of the cut object. */
struct Solid {
  Vector<BspNode> nodes;
  float3 min = float3(FLT_MAX);
  float3 max = float3(-FLT_MAX);
};

/** Builds the BSP tree of a closed shape from its faces, with normals facing out. */
static Solid build_solid(Vector<Vector<float3>> polys, const float eps)
{
  Solid solid;
  for (const Vector<float3> &poly : polys) {
    for (const float3 &p : poly) {
      solid.min = math::min(solid.min, p);
      solid.max = math::max(solid.max, p);
    }
  }
  struct Job {
    int parent;
    bool front;
    Vector<Vector<float3>> polys;
  };
  Vector<Job> jobs;
  jobs.append({-1, false, std::move(polys)});
  while (!jobs.is_empty()) {
    Job job = jobs.pop_last();
    float3 n(0.0f);
    float d = 0.0f;
    int found = -1;
    for (const int i : job.polys.index_range()) {
      const Vector<float3> &poly = job.polys[i];
      const float3 pn = newell_normal(poly);
      if (math::length_squared(pn) > 1e-24f) {
        n = math::normalize(pn);
        for (const float3 &q : poly) {
          d += math::dot(n, q);
        }
        d /= float(poly.size());
        found = i;
        break;
      }
    }
    if (found < 0) {
      continue;
    }
    job.polys.remove_and_reorder(found);
    const int index = solid.nodes.append_and_get_index({n, d});
    if (job.parent >= 0) {
      if (job.front) {
        solid.nodes[job.parent].front = index;
      }
      else {
        solid.nodes[job.parent].back = index;
      }
    }
    Vector<Vector<float3>> front_polys, back_polys;
    for (const Vector<float3> &poly : job.polys) {
      float lo = FLT_MAX, hi = -FLT_MAX;
      for (const float3 &q : poly) {
        const float sd = math::dot(n, q) - d;
        lo = std::min(lo, sd);
        hi = std::max(hi, sd);
      }
      if (lo >= -eps && hi <= eps) {
        /* On this plane: it's part of this node. */
        continue;
      }
      if (lo >= -eps) {
        front_polys.append(poly);
        continue;
      }
      if (hi <= eps) {
        back_polys.append(poly);
        continue;
      }
      Vector<float3> f, b;
      for (const int i : poly.index_range()) {
        const float3 &a = poly[i];
        const float3 &c = poly[(i + 1) % poly.size()];
        const float sa = math::dot(n, a) - d;
        const float sc = math::dot(n, c) - d;
        if (sa >= -eps) {
          f.append(a);
        }
        if (sa <= eps) {
          b.append(a);
        }
        if ((sa > eps && sc < -eps) || (sa < -eps && sc > eps)) {
          const float3 q = math::interpolate(a, c, sa / (sa - sc));
          f.append(q);
          b.append(q);
        }
      }
      if (f.size() >= 3) {
        front_polys.append(std::move(f));
      }
      if (b.size() >= 3) {
        back_polys.append(std::move(b));
      }
    }
    if (!front_polys.is_empty()) {
      jobs.append({index, true, std::move(front_polys)});
    }
    if (!back_polys.is_empty()) {
      jobs.append({index, false, std::move(back_polys)});
    }
  }
  return solid;
}

static bool point_inside(const Solid &solid, const float3 &p)
{
  if (solid.nodes.is_empty()) {
    return false;
  }
  int i = 0;
  while (true) {
    const BspNode &node = solid.nodes[i];
    if (math::dot(node.normal, p) - node.offset > 0.0f) {
      if (node.front < 0) {
        return false;
      }
      i = node.front;
    }
    else {
      if (node.back < 0) {
        return true;
      }
      i = node.back;
    }
  }
}

enum class CellKind { Voronoi, Grid, Object };

/** The cells: Voronoi seeds, the boxes of a grid, or the faces of the cut object. */
struct Cells {
  CellKind kind = CellKind::Voronoi;

  /** In the stretched space: real positions times #inv_scale. */
  Vector<float3> seeds;
  Vector<int> seed_island;
  float3 inv_scale = float3(1.0f);
  Array<KDTree_3d *> trees;

  /** Box size per axis, zero where that axis isn't cut. */
  float3 size = float3(0.0f);
  Map<int3, int> grid_ids;
  Vector<int3> grid_keys;
  bool too_many = false;

  /** The shapes of the cut object, one cell each. */
  Vector<Solid> solids;
  /** The cell of everything the cut object doesn't cover. */
  int outside = 0;
  /** Tolerance for cutting by the solids. */
  float eps = 1e-6f;

  ~Cells()
  {
    free_trees(trees);
  }

  int grid_id(const int3 &key)
  {
    return grid_ids.lookup_or_add_cb(key, [&]() {
      grid_keys.append(key);
      return int(grid_keys.size()) - 1;
    });
  }

  static bool overlaps(const Solid &solid, const float3 &lo, const float3 &hi, const float g)
  {
    return solid.min.x <= hi.x + g && solid.max.x >= lo.x - g && solid.min.y <= hi.y + g &&
           solid.max.y >= lo.y - g && solid.min.z <= hi.z + g && solid.max.z >= lo.z - g;
  }

  /** The cells a face can reach, sorted. */
  void candidates(const Span<float3> face,
                  const int island,
                  const float grow,
                  Vector<int> &r_cells)
  {
    r_cells.clear();
    float3 center(0.0f), bmin(FLT_MAX), bmax(-FLT_MAX);
    for (const float3 &p : face) {
      center += p;
      bmin = math::min(bmin, p);
      bmax = math::max(bmax, p);
    }
    center /= float(face.size());
    if (kind == CellKind::Voronoi) {
      center *= inv_scale;
      float radius = 0.0f;
      for (const float3 &p : face) {
        radius = std::max(radius, math::distance(center, p * inv_scale));
      }
      /* A distance in the stretched space is at most this many times the real one. */
      const float stretch = std::max({inv_scale.x, inv_scale.y, inv_scale.z});
      KDTreeNearest_3d nearest;
      KDTree_3d *tree = trees[island];
      if (kdtree_3d_find_nearest(tree, center, &nearest) < 0) {
        return;
      }
      /* Any seed that is nearest to a point of the face, or that a grown cell reaches it from,
       * is within this distance of the middle. */
      const float range = nearest.dist + 2.0f * radius + 2.0f * std::max(grow, 0.0f) * stretch +
                          1e-6f;
      kdtree_range_search_cb_cpp<float3>(
          tree, center, range, [&](const int index, const float3 & /*co*/, const float /*d*/) {
            r_cells.append(index);
            return true;
          });
      std::sort(r_cells.begin(), r_cells.end());
      return;
    }
    const float g = std::abs(grow);
    if (kind == CellKind::Object) {
      for (const int i : solids.index_range()) {
        if (overlaps(solids[i], bmin, bmax, g)) {
          r_cells.append(i);
        }
      }
      r_cells.append(outside);
      return;
    }
    int3 lo(0), hi(0);
    for (const int a : IndexRange(3)) {
      if (size[a] > 0.0f) {
        /* The boxes are centered on the origin, so a flat surface there isn't on a border. */
        lo[a] = int(std::floor((bmin[a] - g) / size[a] + 0.5f));
        hi[a] = int(std::floor((bmax[a] + g) / size[a] + 0.5f));
      }
    }
    for (int x = lo.x; x <= hi.x; x++) {
      for (int y = lo.y; y <= hi.y; y++) {
        for (int z = lo.z; z <= hi.z; z++) {
          if (grid_keys.size() >= MAX_CELLS) {
            too_many = true;
            return;
          }
          r_cells.append(grid_id(int3(x, y, z)));
        }
      }
    }
  }

  /** The cell a point is in. */
  int owner(const float3 &p, const int island)
  {
    if (kind == CellKind::Voronoi) {
      return kdtree_3d_find_nearest(trees[island], p * inv_scale, nullptr);
    }
    if (kind == CellKind::Object) {
      for (const int i : solids.index_range()) {
        const Solid &solid = solids[i];
        if (p.x >= solid.min.x && p.y >= solid.min.y && p.z >= solid.min.z &&
            p.x <= solid.max.x && p.y <= solid.max.y && p.z <= solid.max.z &&
            point_inside(solid, p))
        {
          return i;
        }
      }
      return outside;
    }
    int3 key(0);
    for (const int a : IndexRange(3)) {
      if (size[a] > 0.0f) {
        key[a] = int(std::floor(p[a] / size[a] + 0.5f));
      }
    }
    return grid_id(key);
  }

  /** The planes that bound a Voronoi or grid \a cell among \a others, moved out by \a grow. */
  void planes(const int cell, const Span<int> others, const float grow, Vector<Plane> &r_planes)
  {
    r_planes.clear();
    if (kind == CellKind::Voronoi) {
      const float3 &sc = seeds[cell];
      for (const int o : others) {
        if (o == cell) {
          continue;
        }
        float3 n = seeds[o] - sc;
        const float len = math::length(n);
        if (len < 1e-12f) {
          continue;
        }
        n /= len;
        /* The bisector in the stretched space, as a plane in real space. */
        float3 real_n = n * inv_scale;
        const float real_len = math::length(real_n);
        if (real_len < 1e-12f) {
          continue;
        }
        const float offset = math::dot(n, (seeds[o] + sc) * 0.5f) / real_len;
        real_n /= real_len;
        r_planes.append({real_n, offset + grow, o});
      }
      return;
    }
    const int3 key = grid_keys[cell];
    for (const int a : IndexRange(3)) {
      if (size[a] <= 0.0f) {
        continue;
      }
      float3 n(0.0f);
      n[a] = 1.0f;
      int3 below = key, above = key;
      below[a]--;
      above[a]++;
      r_planes.append({-n, -(float(key[a]) - 0.5f) * size[a] + grow, grid_id(below)});
      r_planes.append({n, (float(key[a]) + 0.5f) * size[a] + grow, grid_id(above)});
    }
  }
};

/**
 * Reads the cut object, in the space of the patched object. Each closed loose part is one cell:
 * the surface inside it. In an open part, each face is a cell: the surface it covers, seen along
 * the object's Z axis.
 */
static bool build_cut_solids(Object *cutter,
                             const Object *self,
                             const float3 &target_min,
                             const float3 &target_max,
                             Cells &cells)
{
  if (cutter == nullptr || cutter == self) {
    return false;
  }
  bke::GeometrySet set = bke::object_get_evaluated_geometry_set(*cutter);
  if (set.has_instances()) {
    set = geometry::realize_instances(set, geometry::RealizeInstancesOptions()).geometry;
  }
  const Mesh *mesh = set.get_mesh();
  if (mesh == nullptr || mesh->faces_num == 0) {
    return false;
  }
  const float4x4 to_local = math::invert(self->object_to_world()) * cutter->object_to_world();
  const float3 dir_raw = float3x3(to_local).z_axis();
  const float3 dir = math::length_squared(dir_raw) > 1e-12f ? math::normalize(dir_raw) :
                                                               float3(0.0f, 0.0f, 1.0f);
  const Span<float3> src_positions = mesh->vert_positions();
  const OffsetIndices<int> faces = mesh->faces();
  const Span<int> corner_verts = mesh->corner_verts();
  const Span<int3> tris = mesh->corner_tris();
  Array<float3> positions(src_positions.size());
  float3 cmin(FLT_MAX), cmax(-FLT_MAX);
  for (const int v : positions.index_range()) {
    positions[v] = math::transform_point(to_local, src_positions[v]);
    cmin = math::min(cmin, positions[v]);
    cmax = math::max(cmax, positions[v]);
  }
  const float eps = std::max(math::distance(cmin, cmax) * 1e-6f, 1e-8f);
  /* Long enough that a column of a flat face goes through the whole surface. */
  const float3 all_min = math::min(cmin, target_min);
  const float3 all_max = math::max(cmax, target_max);
  const float reach = (math::distance(all_min, all_max) + math::length(all_min) +
                       math::length(all_max)) *
                          2.0f +
                      1.0f;

  Array<int> island(faces.size());
  const int islands = face_islands(*mesh, island);
  Map<OrderedEdge, int> edge_count;
  for (const int f : faces.index_range()) {
    const IndexRange face = faces[f];
    for (const int c : face) {
      const int next = c + 1 < face.one_after_last() ? c + 1 : face.first();
      edge_count.add_or_modify(
          OrderedEdge(corner_verts[c], corner_verts[next]),
          [](int *v) { *v = 1; },
          [](int *v) { (*v)++; });
    }
  }
  Array<bool> closed(islands, true);
  Array<Vector<int>> island_faces(islands);
  for (const int f : faces.index_range()) {
    island_faces[island[f]].append(f);
    const IndexRange face = faces[f];
    for (const int c : face) {
      const int next = c + 1 < face.one_after_last() ? c + 1 : face.first();
      if (edge_count.lookup(OrderedEdge(corner_verts[c], corner_verts[next])) != 2) {
        closed[island[f]] = false;
      }
    }
  }
  auto face_points = [&](const int f) {
    Vector<float3> poly;
    for (const int c : faces[f]) {
      poly.append(positions[corner_verts[c]]);
    }
    return poly;
  };

  for (const int i : IndexRange(islands)) {
    if (closed[i]) {
      /* Triangles, since faces moved out of flat can't be cut by their own plane. */
      Vector<Vector<float3>> polys;
      float volume = 0.0f;
      for (const int f : island_faces[i]) {
        for (const int t : bke::mesh::face_triangles_range(faces, f)) {
          const int3 &tri = tris[t];
          const float3 &a = positions[corner_verts[tri[0]]];
          const float3 &b = positions[corner_verts[tri[1]]];
          const float3 &c = positions[corner_verts[tri[2]]];
          volume += math::dot(a, math::cross(b, c));
          polys.append({a, b, c});
        }
      }
      /* Normals facing in: turn the faces around. */
      if (volume < 0.0f) {
        for (Vector<float3> &poly : polys) {
          std::reverse(poly.begin(), poly.end());
        }
      }
      Solid solid = build_solid(std::move(polys), eps);
      if (!solid.nodes.is_empty()) {
        cells.solids.append(std::move(solid));
      }
      continue;
    }
    for (const int f : island_faces[i]) {
      Vector<float3> poly = face_points(f);
      for (float3 &q : poly) {
        q -= dir * math::dot(q, dir);
      }
      const float along = math::dot(newell_normal(poly), dir);
      if (std::abs(along) < 1e-12f) {
        /* Seen edge on. */
        continue;
      }
      if (along < 0.0f) {
        std::reverse(poly.begin(), poly.end());
      }
      /* A column through the face along the Z axis. */
      Vector<Vector<float3>> polys;
      Vector<float3> top, bottom;
      for (const float3 &q : poly) {
        top.append(q + dir * reach);
      }
      for (int k = poly.size() - 1; k >= 0; k--) {
        bottom.append(poly[k] - dir * reach);
      }
      polys.append(std::move(top));
      polys.append(std::move(bottom));
      for (const int k : poly.index_range()) {
        const float3 &a = poly[k];
        const float3 &b = poly[(k + 1) % poly.size()];
        polys.append({a - dir * reach, b - dir * reach, b + dir * reach, a + dir * reach});
      }
      Solid solid = build_solid(std::move(polys), eps);
      if (!solid.nodes.is_empty()) {
        cells.solids.append(std::move(solid));
      }
    }
  }
  cells.outside = cells.solids.size();
  return !cells.solids.is_empty();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Clipping
 * \{ */

struct PolyVert {
  float3 co;
  /** Cell on the other side of the edge to the next point, or #NO_LABEL for an edge of the
   * original mesh. */
  int label;
};

static void clip_polygon(Vector<PolyVert> &poly, const Plane &plane, Vector<PolyVert> &tmp)
{
  tmp.clear();
  const int num = poly.size();
  for (const int i : IndexRange(num)) {
    const PolyVert &a = poly[i];
    const PolyVert &b = poly[(i + 1) % num];
    const float sa = math::dot(plane.normal, a.co) - plane.offset;
    const float sb = math::dot(plane.normal, b.co) - plane.offset;
    const bool a_in = sa <= 0.0f;
    const bool b_in = sb <= 0.0f;
    if (a_in) {
      tmp.append(a);
      if (!b_in) {
        tmp.append({math::interpolate(a.co, b.co, sa / (sa - sb)), plane.label});
      }
    }
    else if (b_in) {
      tmp.append({math::interpolate(a.co, b.co, sa / (sa - sb)), a.label});
    }
  }
  std::swap(poly, tmp);
}

/**
 * Adds the pieces of \a poly inside the solid grown by \a grow, or outside it, to \a r_out.
 * New edges get #CUT_LABEL.
 */
static void clip_by_solid(const Solid &solid,
                          const Span<PolyVert> poly,
                          const float grow,
                          const bool keep_inside,
                          const float eps,
                          Vector<Vector<PolyVert>> &r_out)
{
  struct Job {
    int node;
    Vector<PolyVert> poly;
  };
  Vector<Job> jobs;
  jobs.append({0, Vector<PolyVert>(poly)});
  Vector<PolyVert> tmp;
  Vector<float3> points;
  auto route = [&](Vector<PolyVert> &&piece, const int child, const bool inside) {
    if (piece.size() < 3) {
      return;
    }
    if (child >= 0) {
      jobs.append({child, std::move(piece)});
    }
    else if (inside == keep_inside) {
      r_out.append(std::move(piece));
    }
  };
  while (!jobs.is_empty()) {
    Job job = jobs.pop_last();
    const BspNode &node = solid.nodes[job.node];
    const float offset = node.offset + grow;
    float lo = FLT_MAX, hi = -FLT_MAX;
    for (const PolyVert &pv : job.poly) {
      const float sd = math::dot(node.normal, pv.co) - offset;
      lo = std::min(lo, sd);
      hi = std::max(hi, sd);
    }
    if (lo >= -eps && hi <= eps) {
      /* Lying on the plane: goes with the side it faces. */
      points.clear();
      for (const PolyVert &pv : job.poly) {
        points.append(pv.co);
      }
      if (math::dot(newell_normal(points), node.normal) >= 0.0f) {
        route(std::move(job.poly), node.front, false);
      }
      else {
        route(std::move(job.poly), node.back, true);
      }
      continue;
    }
    if (lo >= -eps) {
      route(std::move(job.poly), node.front, false);
      continue;
    }
    if (hi <= eps) {
      route(std::move(job.poly), node.back, true);
      continue;
    }
    Vector<PolyVert> front = job.poly;
    clip_polygon(front, {-node.normal, -offset, CUT_LABEL}, tmp);
    clip_polygon(job.poly, {node.normal, offset, CUT_LABEL}, tmp);
    route(std::move(front), node.front, false);
    route(std::move(job.poly), node.back, true);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Polygon soup
 * \{ */

/** Faces with their own points, welded later. */
struct Soup {
  Vector<float3> positions;
  /** Points are only welded to points of the same group. */
  Vector<int> group;
  Vector<int> face_offsets = {0};
  Vector<int> corner_verts;
  Vector<int> corner_label;
  Vector<int> face_src;
  Vector<int> face_cell;
  /** Source corners and weights to interpolate the corner from, filled after welding. */
  Vector<int3> corner_src;
  Vector<float3> corner_weight;

  int faces_num() const
  {
    return face_offsets.size() - 1;
  }
  IndexRange face(const int f) const
  {
    return IndexRange::from_begin_end(face_offsets[f], face_offsets[f + 1]);
  }

  void add_face(const Span<PolyVert> poly, const int src, const int cell, const int weld_group)
  {
    for (const PolyVert &pv : poly) {
      corner_verts.append(positions.size());
      corner_label.append(pv.label);
      positions.append(pv.co);
      group.append(weld_group);
    }
    face_offsets.append(corner_verts.size());
    face_src.append(src);
    face_cell.append(cell);
  }
};

/** Joins points of the same group closer than \a eps, and drops faces that collapse. */
static void weld_soup(Soup &soup, const float eps)
{
  const int n = soup.positions.size();
  if (n == 0) {
    return;
  }
  Array<int> parent(n);
  std::iota(parent.begin(), parent.end(), 0);
  KDTree_3d *tree = kdtree_3d_new(n);
  for (const int i : IndexRange(n)) {
    kdtree_3d_insert(tree, i, soup.positions[i]);
  }
  kdtree_3d_balance(tree);
  for (const int i : IndexRange(n)) {
    kdtree_range_search_cb_cpp<float3>(
        tree, soup.positions[i], eps, [&](const int j, const float3 & /*co*/, const float /*d*/) {
          if (j != i && soup.group[j] == soup.group[i]) {
            const int a = find_root(parent, i);
            const int b = find_root(parent, j);
            if (a != b) {
              parent[std::max(a, b)] = std::min(a, b);
            }
          }
          return true;
        });
  }
  kdtree_3d_free(tree);

  Array<int> new_index(n, -1);
  Vector<float3> positions;
  Vector<int> group;
  for (const int i : IndexRange(n)) {
    const int r = find_root(parent, i);
    if (new_index[r] < 0) {
      new_index[r] = positions.size();
      positions.append(soup.positions[r]);
      group.append(soup.group[r]);
    }
    new_index[i] = new_index[r];
  }

  Soup out;
  out.positions = std::move(positions);
  out.group = std::move(group);
  struct Corner {
    int vert;
    int label;
  };
  Vector<Corner> corners;
  for (const int f : IndexRange(soup.faces_num())) {
    corners.clear();
    for (const int c : soup.face(f)) {
      const int v = new_index[soup.corner_verts[c]];
      if (!corners.is_empty() && corners.last().vert == v) {
        corners.last().label = soup.corner_label[c];
        continue;
      }
      corners.append({v, soup.corner_label[c]});
    }
    while (corners.size() > 1 && corners.last().vert == corners.first().vert) {
      corners.remove_last();
    }
    if (corners.size() < 3) {
      continue;
    }
    /* Drop faces with no area. */
    float3 normal(0.0f);
    for (const int i : corners.index_range()) {
      const float3 &a = out.positions[corners[i].vert];
      const float3 &b = out.positions[corners[(i + 1) % corners.size()].vert];
      normal += math::cross(a, b);
    }
    if (math::length(normal) <= eps * eps) {
      continue;
    }
    for (const Corner &corner : corners) {
      out.corner_verts.append(corner.vert);
      out.corner_label.append(corner.label);
    }
    out.face_offsets.append(out.corner_verts.size());
    out.face_src.append(soup.face_src[f]);
    out.face_cell.append(soup.face_cell[f]);
  }
  soup = std::move(out);
}

/**
 * Cutting leaves a face in more bits than it needs. Joins the bits that come from the same face
 * and belong to the same cell, so only the cut itself adds edges. Then drops the points left in
 * the middle of straight edges.
 */
static void merge_soup_faces(Soup &soup, const float eps)
{
  struct Face {
    Vector<int> verts;
    Vector<int> labels;
    int src;
    int cell;
  };
  /* The bits of each source face and cell. */
  Map<int2, Vector<int>> groups;
  for (const int f : IndexRange(soup.faces_num())) {
    groups.lookup_or_add_default(int2(soup.face_src[f], soup.face_cell[f])).append(f);
  }
  Vector<Face> result;
  Vector<Face> faces;
  Map<int2, int> edge_face;
  Map<int, int> next;
  Map<int, int> next_label;
  for (const auto item : groups.items()) {
    faces.clear();
    for (const int f : item.value) {
      Face face;
      for (const int c : soup.face(f)) {
        face.verts.append(soup.corner_verts[c]);
        face.labels.append(soup.corner_label[c]);
      }
      face.src = item.key.x;
      face.cell = item.key.y;
      faces.append(std::move(face));
    }
    if (faces.size() > 1) {
      /* Where a bit was cut and its neighbor wasn't, the neighbor's edge gets the point too, so
       * their edges match. */
      Vector<int> group_verts;
      for (const Face &face : faces) {
        group_verts.extend(face.verts);
      }
      std::sort(group_verts.begin(), group_verts.end());
      group_verts.resize(std::unique(group_verts.begin(), group_verts.end()) -
                         group_verts.begin());
      for (Face &face : faces) {
        Face fixed;
        Vector<std::pair<float, int>> on_edge;
        const int num = face.verts.size();
        for (const int k : IndexRange(num)) {
          const int a = face.verts[k];
          const int b = face.verts[(k + 1) % num];
          fixed.verts.append(a);
          fixed.labels.append(face.labels[k]);
          const float3 &pa = soup.positions[a];
          const float3 ab = soup.positions[b] - pa;
          const float len_sq = math::length_squared(ab);
          if (len_sq <= 0.0f) {
            continue;
          }
          on_edge.clear();
          for (const int v : group_verts) {
            if (v == a || v == b) {
              continue;
            }
            const float t = math::dot(soup.positions[v] - pa, ab) / len_sq;
            if (t <= 0.0f || t >= 1.0f) {
              continue;
            }
            if (math::distance(pa + ab * t, soup.positions[v]) <= eps) {
              on_edge.append({t, v});
            }
          }
          std::sort(on_edge.begin(), on_edge.end());
          for (const std::pair<float, int> &item : on_edge) {
            fixed.verts.append(item.second);
            fixed.labels.append(face.labels[k]);
          }
        }
        face.verts = std::move(fixed.verts);
        face.labels = std::move(fixed.labels);
      }
      Array<bool> alive(faces.size(), true);
      bool changed = true;
      while (changed) {
        changed = false;
        edge_face.clear();
        for (const int i : faces.index_range()) {
          if (!alive[i]) {
            continue;
          }
          const Vector<int> &verts = faces[i].verts;
          for (const int k : verts.index_range()) {
            edge_face.add(int2(verts[k], verts[(k + 1) % verts.size()]), i);
          }
        }
        for (const int i : faces.index_range()) {
          if (!alive[i]) {
            continue;
          }
          /* A neighbor across any edge. */
          int other = -1;
          const Vector<int> &verts = faces[i].verts;
          for (const int k : verts.index_range()) {
            const int *j = edge_face.lookup_ptr(int2(verts[(k + 1) % verts.size()], verts[k]));
            if (j != nullptr && *j != i && alive[*j]) {
              other = *j;
              break;
            }
          }
          if (other < 0) {
            continue;
          }
          /* The edges of both, without the ones they share. The result must be one loop. */
          next.clear();
          next_label.clear();
          bool ok = true;
          int edges = 0;
          for (const int f : {i, other}) {
            const Face &face = faces[f];
            const int g = f == i ? other : i;
            for (const int k : face.verts.index_range()) {
              const int a = face.verts[k];
              const int b = face.verts[(k + 1) % face.verts.size()];
              const int *back = edge_face.lookup_ptr(int2(b, a));
              if (back != nullptr && *back == g) {
                continue;
              }
              if (!next.add(a, b)) {
                ok = false;
                break;
              }
              next_label.add(a, face.labels[k]);
              edges++;
            }
            if (!ok) {
              break;
            }
          }
          if (!ok || edges < 3) {
            continue;
          }
          Face merged;
          merged.src = faces[i].src;
          merged.cell = faces[i].cell;
          const int start = next.keys().begin() == next.keys().end() ? -1 : *next.keys().begin();
          int v = start;
          do {
            merged.verts.append(v);
            merged.labels.append(next_label.lookup(v));
            const int *nv = next.lookup_ptr(v);
            if (nv == nullptr) {
              ok = false;
              break;
            }
            v = *nv;
          } while (v != start && int(merged.verts.size()) <= edges);
          if (!ok || int(merged.verts.size()) != edges) {
            continue;
          }
          faces[i] = std::move(merged);
          alive[other] = false;
          changed = true;
          /* The edge map is out of date now. */
          break;
        }
      }
      for (const int i : faces.index_range()) {
        if (alive[i]) {
          result.append(std::move(faces[i]));
        }
      }
    }
    else {
      result.append(std::move(faces[0]));
    }
  }

  /* Points in the middle of a straight edge, used by at most two faces, that have the same
   * neighbor on both sides. */
  Array<int> uses(soup.positions.size(), 0);
  for (const Face &face : result) {
    for (const int v : face.verts) {
      uses[v]++;
    }
  }
  for (const Face &face : result) {
    const int num = face.verts.size();
    for (const int k : IndexRange(num)) {
      const int v = face.verts[k];
      if (uses[v] > 2) {
        continue;
      }
      const float3 &a = soup.positions[face.verts[(k + num - 1) % num]];
      const float3 &b = soup.positions[v];
      const float3 &c = soup.positions[face.verts[(k + 1) % num]];
      const float3 d1 = b - a;
      const float3 d2 = c - b;
      const float l1 = math::length(d1);
      const float l2 = math::length(d2);
      const bool straight = l1 > 0.0f && l2 > 0.0f &&
                            math::dot(d1, d2) > 0.9999f * l1 * l2;
      if (!straight || face.labels[(k + num - 1) % num] != face.labels[k]) {
        /* Must stay. */
        uses[v] = 1000;
      }
    }
  }
  Array<bool> drop(soup.positions.size(), false);
  for (const Face &face : result) {
    for (const int v : face.verts) {
      drop[v] = uses[v] <= 2;
    }
  }

  Soup out;
  Array<int> new_index(soup.positions.size(), -1);
  for (const Face &face : result) {
    int kept = 0;
    for (const int v : face.verts) {
      kept += drop[v] ? 0 : 1;
    }
    const bool can_drop = kept >= 3;
    for (const int k : face.verts.index_range()) {
      const int v = face.verts[k];
      if (can_drop && drop[v]) {
        continue;
      }
      if (new_index[v] < 0) {
        new_index[v] = out.positions.size();
        out.positions.append(soup.positions[v]);
        out.group.append(soup.group[v]);
      }
      out.corner_verts.append(new_index[v]);
      /* A dropped point's edge label carries on to the next kept one, it's the same edge. */
      out.corner_label.append(face.labels[k]);
    }
    out.face_offsets.append(out.corner_verts.size());
    out.face_src.append(face.src);
    out.face_cell.append(face.cell);
  }
  soup = std::move(out);
}

/** For each corner, the corners of its source face and weights to interpolate from. */
static void find_corner_sources(Soup &soup, const Mesh &mesh)
{
  const Span<float3> positions = mesh.vert_positions();
  const Span<int> corner_verts = mesh.corner_verts();
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int3> tris = mesh.corner_tris();
  soup.corner_src.resize(soup.corner_verts.size());
  soup.corner_weight.resize(soup.corner_verts.size());
  for (const int f : IndexRange(soup.faces_num())) {
    const int src = soup.face_src[f];
    const IndexRange src_tris = bke::mesh::face_triangles_range(faces, src);
    for (const int c : soup.face(f)) {
      const float3 &p = soup.positions[soup.corner_verts[c]];
      float best = -FLT_MAX;
      int3 best_tri = tris[src_tris.first()];
      float3 best_w(1.0f, 0.0f, 0.0f);
      for (const int t : src_tris) {
        const int3 &tri = tris[t];
        float w[3];
        interp_weights_tri_v3(w,
                              positions[corner_verts[tri[0]]],
                              positions[corner_verts[tri[1]]],
                              positions[corner_verts[tri[2]]],
                              p);
        const float score = std::min({w[0], w[1], w[2]});
        if (score > best) {
          best = score;
          best_tri = tri;
          best_w = float3(w[0], w[1], w[2]);
        }
      }
      best_w = math::max(best_w, float3(0.0f));
      const float total = best_w.x + best_w.y + best_w.z;
      best_w = total > 0.0f ? best_w / total : float3(1.0f, 0.0f, 0.0f);
      soup.corner_src[c] = best_tri;
      soup.corner_weight[c] = best_w;
    }
  }
}

template<typename T, typename... Ts>
static constexpr bool one_of_v = (std::is_same_v<T, Ts> || ...);

template<typename T>
static constexpr bool can_mix_v = one_of_v<T,
                                           float,
                                           float2,
                                           float3,
                                           ColorGeometry4f,
                                           ColorGeometry4b,
                                           int,
                                           int8_t,
                                           bool,
                                           int2,
                                           short2>;

template<typename T> static T mix_corner(const Span<T> src, const int3 &i, const float3 &w)
{
  if constexpr (can_mix_v<T>) {
    return bke::attribute_math::mix3<T>(w, src[i[0]], src[i[1]], src[i[2]]);
  }
  else {
    const int k = w[0] >= w[1] && w[0] >= w[2] ? 0 : (w[1] >= w[2] ? 1 : 2);
    return src[i[k]];
  }
}

/** Builds a mesh from the soup, with the attributes of \a src interpolated onto it. */
static Mesh *soup_to_mesh(const Soup &soup, const Mesh &src)
{
  const int corners_num = soup.corner_verts.size();
  Mesh *mesh = BKE_mesh_new_nomain(soup.positions.size(), 0, soup.faces_num(), corners_num);
  BKE_mesh_copy_parameters_for_eval(mesh, &src);
  mesh->vert_positions_for_write().copy_from(soup.positions);
  mesh->face_offsets_for_write().copy_from(soup.face_offsets);
  mesh->corner_verts_for_write().copy_from(soup.corner_verts);
  bke::mesh_calc_edges(*mesh, false, false);

  /* A corner of each point, to interpolate point attributes from. */
  Array<int> vert_corner(soup.positions.size(), 0);
  for (int c = corners_num - 1; c >= 0; c--) {
    vert_corner[soup.corner_verts[c]] = c;
  }

  const Span<int> src_corner_verts = src.corner_verts();
  const bke::AttributeAccessor src_attributes = src.attributes();
  bke::MutableAttributeAccessor dst_attributes = mesh->attributes_for_write();
  src_attributes.foreach_attribute([&](const bke::AttributeIter &iter) {
    if (iter.data_type == bke::AttrType::String) {
      return;
    }
    if (iter.name == "position" || iter.name.startswith(".") || iter.name == "patch_id") {
      return;
    }
    if (!ELEM(iter.domain, bke::AttrDomain::Point, bke::AttrDomain::Corner, bke::AttrDomain::Face))
    {
      return;
    }
    const bke::GAttributeReader reader = iter.get();
    if (!reader) {
      return;
    }
    const GVArraySpan src_data(*reader);
    bke::GSpanAttributeWriter dst = dst_attributes.lookup_or_add_for_write_span(
        iter.name, iter.domain, iter.data_type);
    if (!dst) {
      return;
    }
    bke::attribute_math::to_static_type(src_data.type(), [&]<typename T>() {
      const Span<T> values = src_data.typed<T>();
      MutableSpan<T> out = dst.span.typed<T>();
      if (iter.domain == bke::AttrDomain::Face) {
        for (const int f : out.index_range()) {
          out[f] = values[soup.face_src[f]];
        }
      }
      else if (iter.domain == bke::AttrDomain::Corner) {
        for (const int c : out.index_range()) {
          out[c] = mix_corner<T>(values, soup.corner_src[c], soup.corner_weight[c]);
        }
      }
      else {
        for (const int v : out.index_range()) {
          const int c = vert_corner[v];
          const int3 &corners = soup.corner_src[c];
          const int3 verts(src_corner_verts[corners[0]],
                           src_corner_verts[corners[1]],
                           src_corner_verts[corners[2]]);
          out[v] = mix_corner<T>(values, verts, soup.corner_weight[c]);
        }
      }
    });
    dst.finish();
  });
  return mesh;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Stitch shapes
 * \{ */

/** A stitch shape, turned and scaled as its object is. */
static Mesh *make_stitch_shape(Object *ob, const Object *self, const Mesh &src)
{
  if (ob == nullptr || ob == self) {
    return nullptr;
  }
  bke::GeometrySet set = bke::object_get_evaluated_geometry_set(*ob);
  if (set.has_instances()) {
    set = geometry::realize_instances(set, geometry::RealizeInstancesOptions()).geometry;
  }
  const Mesh *shape = set.get_mesh();
  if (shape == nullptr || shape->verts_num == 0) {
    return nullptr;
  }
  Mesh *copy = BKE_mesh_copy_for_eval(*shape);
  const float3x3 matrix(ob->object_to_world());
  for (float3 &p : copy->vert_positions_for_write()) {
    p = matrix * p;
  }
  copy->tag_positions_changed();
  BKE_mesh_copy_parameters_for_eval(copy, &src);
  return copy;
}

/** A unit box from -0.5 to 0.5 on each axis. */
static Mesh *make_box(const Mesh &src)
{
  Mesh *mesh = BKE_mesh_new_nomain(8, 0, 6, 24);
  BKE_mesh_copy_parameters_for_eval(mesh, &src);
  MutableSpan<float3> positions = mesh->vert_positions_for_write();
  for (const int i : IndexRange(8)) {
    positions[i] = float3(i & 1 ? 0.5f : -0.5f, i & 2 ? 0.5f : -0.5f, i & 4 ? 0.5f : -0.5f);
  }
  const int quads[6][4] = {
      {0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
  MutableSpan<int> offsets = mesh->face_offsets_for_write();
  MutableSpan<int> corner_verts = mesh->corner_verts_for_write();
  for (const int f : IndexRange(6)) {
    offsets[f] = f * 4;
    for (const int k : IndexRange(4)) {
      corner_verts[f * 4 + k] = quads[f][k];
    }
  }
  offsets[6] = 24;
  bke::mesh_calc_edges(*mesh, false, false);
  return mesh;
}

static void set_face_ints(Mesh &mesh, const StringRef name, const int value)
{
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  bke::SpanAttributeWriter<int> writer = attributes.lookup_or_add_for_write_only_span<int>(
      name, bke::AttrDomain::Face);
  if (writer) {
    writer.span.fill(value);
    writer.finish();
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Modifier
 * \{ */

struct StitchSpot {
  float4x4 transform;
  /** Index of the shape, -1 for the box. */
  int shape;
};

/** The pieces of \a face that belong to \a cell, grown by \a grow. */
static void cut_cell(Cells &cells,
                     const int cell,
                     const Span<int> candidates,
                     const Span<float3> face,
                     const float grow,
                     Vector<Vector<PolyVert>> &r_pieces)
{
  r_pieces.clear();
  Vector<PolyVert> poly, tmp;
  auto whole = [&]() {
    poly.clear();
    for (const float3 &p : face) {
      poly.append({p, NO_LABEL});
    }
  };
  if (cells.kind != CellKind::Object) {
    whole();
    Vector<Plane> planes;
    cells.planes(cell, candidates, grow, planes);
    for (const Plane &plane : planes) {
      clip_polygon(poly, plane, tmp);
      if (poly.size() < 3) {
        return;
      }
    }
    r_pieces.append(std::move(poly));
    return;
  }

  /* Take away the shapes that come first, then keep what is inside this one. Growing a cell means
   * shrinking the shapes around it. */
  whole();
  Vector<Vector<PolyVert>> pieces = {poly};
  Vector<Vector<PolyVert>> next;
  auto keep = [&](const Solid &solid, const float g, const bool inside) {
    next.clear();
    for (const Vector<PolyVert> &piece : pieces) {
      clip_by_solid(solid, piece, g, inside, cells.eps, next);
    }
    std::swap(pieces, next);
  };
  for (const int other : candidates) {
    if (other >= cell || other == cells.outside || pieces.is_empty()) {
      break;
    }
    keep(cells.solids[other], -grow, false);
  }
  if (cell != cells.outside && !pieces.is_empty()) {
    keep(cells.solids[cell], grow, true);
  }

  /* What is across each new edge, looked up just past it. */
  const float reach = std::abs(grow) + cells.eps * 20.0f;
  Vector<float3> points;
  for (Vector<PolyVert> &piece : pieces) {
    points.clear();
    for (const PolyVert &pv : piece) {
      points.append(pv.co);
    }
    const float3 n = newell_normal(points);
    for (const int i : piece.index_range()) {
      PolyVert &pv = piece[i];
      if (pv.label != CUT_LABEL) {
        continue;
      }
      const float3 &b = piece[(i + 1) % piece.size()].co;
      const float3 out = math::cross(b - pv.co, n);
      if (math::length_squared(out) < 1e-30f) {
        pv.label = NO_LABEL;
        continue;
      }
      const int other = cells.owner((pv.co + b) * 0.5f + math::normalize(out) * reach, 0);
      pv.label = other == cell ? NO_LABEL : other;
    }
  }
  r_pieces = std::move(pieces);
}

/** Point normals of the soup, from the faces around each point. */
static Array<float3> soup_normals(const Soup &soup)
{
  Array<float3> normals(soup.positions.size(), float3(0.0f));
  for (const int f : IndexRange(soup.faces_num())) {
    const IndexRange face = soup.face(f);
    float3 n(0.0f);
    for (const int c : face) {
      const int next = c + 1 < face.one_after_last() ? c + 1 : face.first();
      n += math::cross(soup.positions[soup.corner_verts[c]],
                       soup.positions[soup.corner_verts[next]]);
    }
    for (const int c : face) {
      normals[soup.corner_verts[c]] += n;
    }
  }
  for (float3 &n : normals) {
    n = math::length_squared(n) > 0.0f ? math::normalize(n) : float3(0.0f, 0.0f, 1.0f);
  }
  return normals;
}

static Map<OrderedEdge, int> soup_edge_count(const Soup &soup)
{
  Map<OrderedEdge, int> count;
  for (const int f : IndexRange(soup.faces_num())) {
    const IndexRange face = soup.face(f);
    for (const int c : face) {
      const int next = c + 1 < face.one_after_last() ? c + 1 : face.first();
      count.add_or_modify(
          OrderedEdge(soup.corner_verts[c], soup.corner_verts[next]),
          [](int *v) { *v = 1; },
          [](int *v) { (*v)++; });
    }
  }
  return count;
}

/** Gives the soup thickness, from \a bottom to \a top along the normals, with a rim. */
static void solidify_soup(Soup &soup, const float bottom, const float top)
{
  if (soup.faces_num() == 0 || top <= bottom) {
    return;
  }
  const Array<float3> normals = soup_normals(soup);
  const Map<OrderedEdge, int> edge_count = soup_edge_count(soup);
  Soup solid;
  const int n = soup.positions.size();
  solid.positions.resize(n * 2);
  solid.group.resize(n * 2);
  for (const int v : IndexRange(n)) {
    solid.positions[v] = soup.positions[v] + normals[v] * top;
    solid.positions[n + v] = soup.positions[v] + normals[v] * bottom;
    solid.group[v] = solid.group[n + v] = soup.group[v];
  }
  auto add_corner = [&](const int vert, const int src_corner) {
    solid.corner_verts.append(vert);
    solid.corner_label.append(NO_LABEL);
    solid.corner_src.append(soup.corner_src[src_corner]);
    solid.corner_weight.append(soup.corner_weight[src_corner]);
  };
  auto end_face = [&](const int f) {
    solid.face_offsets.append(solid.corner_verts.size());
    solid.face_src.append(soup.face_src[f]);
    solid.face_cell.append(soup.face_cell[f]);
  };
  for (const int f : IndexRange(soup.faces_num())) {
    const IndexRange face = soup.face(f);
    for (const int c : face) {
      add_corner(soup.corner_verts[c], c);
    }
    end_face(f);
    for (int c = face.last(); c >= face.first(); c--) {
      add_corner(n + soup.corner_verts[c], c);
    }
    end_face(f);
  }
  for (const int f : IndexRange(soup.faces_num())) {
    const IndexRange face = soup.face(f);
    for (const int c : face) {
      const int next = c + 1 < face.one_after_last() ? c + 1 : face.first();
      const int a = soup.corner_verts[c];
      const int b = soup.corner_verts[next];
      if (edge_count.lookup(OrderedEdge(a, b)) != 1) {
        continue;
      }
      add_corner(a, c);
      add_corner(n + a, c);
      add_corner(n + b, next);
      add_corner(b, next);
      end_face(f);
    }
  }
  soup = std::move(solid);
}

/** The whole mesh as a soup, with its points shared. */
static Soup soup_from_mesh(const Mesh &mesh)
{
  Soup soup;
  soup.positions.extend(mesh.vert_positions());
  soup.group.append_n_times(0, mesh.verts_num);
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  for (const int f : faces.index_range()) {
    for (const int c : faces[f]) {
      soup.corner_verts.append(corner_verts[c]);
      soup.corner_label.append(NO_LABEL);
      soup.corner_src.append(int3(c));
      soup.corner_weight.append(float3(1.0f, 0.0f, 0.0f));
    }
    soup.face_offsets.append(soup.corner_verts.size());
    soup.face_src.append(f);
    soup.face_cell.append(-1);
  }
  return soup;
}

static void modify_geometry_set(ModifierData *md,
                                const ModifierEvalContext *ctx,
                                bke::GeometrySet *geometry_set)
{
  const PatchModifierData &pmd = *reinterpret_cast<const PatchModifierData *>(md);
  if (geometry_set->get_mesh() == nullptr) {
    return;
  }
  int patch_material = pmd.patch_material;
  int stitch_material = pmd.stitch_material;
  if (pmd.patch_material_pick != nullptr || pmd.stitch_material_pick != nullptr) {
    /* The parts copy the materials of the mesh, so the picked ones are added to it. */
    Mesh &mesh_for_write = *geometry_set->get_mesh_for_write();
    patch_material = MOD_material_slot(mesh_for_write, pmd.patch_material_pick, patch_material);
    stitch_material = MOD_material_slot(mesh_for_write, pmd.stitch_material_pick, stitch_material);
  }
  const Mesh *mesh_ptr = geometry_set->get_mesh();
  if (mesh_ptr == nullptr) {
    return;
  }
  const Mesh &mesh = *mesh_ptr;
  if (mesh.faces_num == 0) {
    BKE_modifier_set_warning(ctx->object, md, "Needs a mesh with faces");
    return;
  }
  const bool split = pmd.mode == MOD_PATCH_MODE_SPLIT;
  const uint32_t seed = uint32_t(pmd.seed);
  const Span<float3> positions = mesh.vert_positions();
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();

  float3 bmin(FLT_MAX), bmax(-FLT_MAX);
  for (const float3 &p : positions) {
    bmin = math::min(bmin, p);
    bmax = math::max(bmax, p);
  }
  const float eps = std::max(math::distance(bmin, bmax) * 1e-5f, 1e-7f);

  /* Loose parts. */
  Array<int> face_island(faces.size(), 0);
  const int islands = (pmd.flag & MOD_PATCH_PER_ISLAND) ? face_islands(mesh, face_island) : 1;

  /* The weight of each face in the vertex group. */
  Array<float> face_weight(faces.size(), 1.0f);
  bool use_group = false;
  if (pmd.vertex_group[0] != '\0') {
    const bke::AttributeAccessor attributes = mesh.attributes();
    if (attributes.contains(pmd.vertex_group)) {
      use_group = true;
      const VArraySpan<float> weights = *attributes.lookup_or_default<float>(
          pmd.vertex_group, bke::AttrDomain::Point, 0.0f);
      const bool invert = pmd.flag & MOD_PATCH_INVERT_VGROUP;
      for (const int f : faces.index_range()) {
        float sum = 0.0f;
        for (const int c : faces[f]) {
          sum += weights[corner_verts[c]];
        }
        const float w = std::clamp(sum / float(faces[f].size()), 0.0f, 1.0f);
        face_weight[f] = invert ? 1.0f - w : w;
      }
    }
  }

  /* The cells. */
  Cells cells;
  if (pmd.layout == MOD_PATCH_LAYOUT_VORONOI) {
    cells.kind = CellKind::Voronoi;
    cells.inv_scale = 1.0f / math::max(float3(pmd.voronoi_scale), float3(0.001f));
    Vector<int> seed_faces;
    Vector<float3> seeds = sample_surface(mesh, std::max(pmd.count, 1), seed, seed_faces);
    if (seeds.is_empty()) {
      BKE_modifier_set_warning(ctx->object, md, "The surface has no area");
      return;
    }
    Vector<int> seed_island;
    Array<bool> has_seed(islands, false);
    for (const int f : seed_faces) {
      seed_island.append(face_island[f]);
      has_seed[face_island[f]] = true;
    }
    /* Every loose part gets at least one piece. */
    for (const int f : faces.index_range()) {
      if (!has_seed[face_island[f]]) {
        float3 center(0.0f);
        for (const int c : faces[f]) {
          center += positions[corner_verts[c]];
        }
        seeds.append(center / float(faces[f].size()));
        seed_island.append(face_island[f]);
        has_seed[face_island[f]] = true;
      }
    }
    for (float3 &p : seeds) {
      p *= cells.inv_scale;
    }
    relax_seeds(mesh, seeds, seed_island, face_island, islands, pmd.relax, seed, cells.inv_scale);
    cells.trees = island_trees(seeds, seed_island, islands);
    cells.seeds = std::move(seeds);
    cells.seed_island = std::move(seed_island);
  }
  else if (pmd.layout == MOD_PATCH_LAYOUT_OBJECT) {
    cells.kind = CellKind::Object;
    cells.eps = eps;
    if (!build_cut_solids(pmd.cut_object, ctx->object, bmin, bmax, cells)) {
      BKE_modifier_set_warning(ctx->object, md, "Pick a cut object with faces");
      return;
    }
  }
  else {
    cells.kind = CellKind::Grid;
    if (pmd.layout == MOD_PATCH_LAYOUT_GRID) {
      cells.size = math::max(float3(pmd.grid_size), float3(0.0f));
    }
    else {
      cells.size[std::clamp(pmd.strip_axis, 0, 2)] = std::max(pmd.strip_width, 1e-4f);
    }
  }

  /* Cells are numbered per loose part. */
  Map<int2, int> ids;
  Vector<int> id_raw;
  Vector<float> weight_sum, weight_area;
  Vector<int8_t> patched;
  auto cell_id = [&](const int raw, const int island) {
    return ids.lookup_or_add_cb(int2(raw, island), [&]() {
      id_raw.append(raw);
      weight_sum.append(0.0f);
      weight_area.append(0.0f);
      patched.append(-1);
      return int(id_raw.size()) - 1;
    });
  };
  Vector<float3> face_points;
  auto read_face = [&](const int f) {
    face_points.clear();
    for (const int c : faces[f]) {
      face_points.append(positions[corner_verts[c]]);
    }
  };
  if (use_group) {
    /* The weight of a cell is the average of the faces whose middle is in it. */
    for (const int f : faces.index_range()) {
      read_face(f);
      float3 center(0.0f), n(0.0f);
      for (const int i : face_points.index_range()) {
        center += face_points[i];
        n += math::cross(face_points[i], face_points[(i + 1) % face_points.size()]);
      }
      center /= float(face_points.size());
      const float area = 0.5f * math::length(n);
      const int raw = cells.owner(center, face_island[f]);
      if (raw < 0) {
        continue;
      }
      const int id = cell_id(raw, face_island[f]);
      weight_sum[id] += face_weight[f] * area;
      weight_area[id] += area;
    }
  }
  const float chance = (split || cells.kind == CellKind::Object) ? 1.0f : pmd.amount;
  auto is_patched = [&](const int id, const int f) {
    if (patched[id] < 0) {
      float w = 1.0f;
      if (use_group) {
        w = weight_area[id] > 0.0f ? weight_sum[id] / weight_area[id] : face_weight[f];
      }
      bool p = rand01(uint32_t(id), seed, 101) < chance * w;
      if (!split && cells.kind == CellKind::Object && id_raw[id] == cells.outside) {
        p = false;
      }
      patched[id] = p ? 1 : 0;
    }
    return patched[id] == 1;
  };
  auto was_patched = [&](const int id) { return id >= 0 && patched[id] == 1; };

  /* How far the patches reach past their cell. */
  const float grow = split ? -pmd.gap * 0.5f : pmd.margin;
  const bool cut_hole = !split && (pmd.flag & MOD_PATCH_CUT_HOLE);
  /* Split mode keeps the cells outside the group as one whole piece of cloth. */
  const bool need_cloth = split || cut_hole;

  /* Cut the faces. */
  Soup patches;
  Soup cloth;
  Vector<int> candidates;
  Vector<Vector<PolyVert>> pieces;
  for (const int f : faces.index_range()) {
    const int island = face_island[f];
    read_face(f);
    cells.candidates(face_points, island, std::max(grow, 0.0f), candidates);
    if (cells.too_many) {
      BKE_modifier_set_error(ctx->object, md, "Too many pieces, make them bigger");
      return;
    }
    bool any_patched = false;
    for (const int raw : candidates) {
      const int id = cell_id(raw, island);
      if (!is_patched(id, f)) {
        continue;
      }
      any_patched = true;
      cut_cell(cells, raw, candidates, face_points, grow, pieces);
      for (Vector<PolyVert> &piece : pieces) {
        for (PolyVert &pv : piece) {
          if (pv.label >= 0) {
            pv.label = cell_id(pv.label, island);
          }
        }
        patches.add_face(piece, f, id, id);
      }
    }
    if (!need_cloth) {
      continue;
    }
    if (!any_patched) {
      Vector<PolyVert> poly;
      for (const float3 &p : face_points) {
        poly.append({p, NO_LABEL});
      }
      cloth.add_face(poly, f, -1, 0);
      continue;
    }
    for (const int raw : candidates) {
      if (is_patched(cell_id(raw, island), f)) {
        continue;
      }
      cut_cell(cells, raw, candidates, face_points, 0.0f, pieces);
      for (const Vector<PolyVert> &piece : pieces) {
        cloth.add_face(piece, f, -1, 0);
      }
    }
  }

  weld_soup(patches, eps);
  merge_soup_faces(patches, eps);
  find_corner_sources(patches, mesh);
  if (need_cloth) {
    weld_soup(cloth, eps);
    merge_soup_faces(cloth, eps);
    find_corner_sources(cloth, mesh);
  }

  /* Normals of the patch points, and their shortest edges. */
  const int pverts = patches.positions.size();
  const Array<float3> normals = soup_normals(patches);
  Array<float> min_edge(pverts, FLT_MAX);
  for (const int f : IndexRange(patches.faces_num())) {
    const IndexRange face = patches.face(f);
    for (const int c : face) {
      const int next = c + 1 < face.one_after_last() ? c + 1 : face.first();
      const int a = patches.corner_verts[c];
      const int b = patches.corner_verts[next];
      const float len = math::distance(patches.positions[a], patches.positions[b]);
      min_edge[a] = std::min(min_edge[a], len);
      min_edge[b] = std::min(min_edge[b], len);
    }
  }

  /* Border edges: used by one face only. */
  const Map<OrderedEdge, int> edge_count = soup_edge_count(patches);
  struct BorderEdge {
    int a, b, label, cell;
  };
  Vector<BorderEdge> border;
  Array<bool> on_border(pverts, false);
  for (const int f : IndexRange(patches.faces_num())) {
    const IndexRange face = patches.face(f);
    for (const int c : face) {
      const int next = c + 1 < face.one_after_last() ? c + 1 : face.first();
      const int a = patches.corner_verts[c];
      const int b = patches.corner_verts[next];
      if (edge_count.lookup(OrderedEdge(a, b)) == 1) {
        border.append({a, b, patches.corner_label[c], patches.face_cell[f]});
        on_border[a] = true;
        on_border[b] = true;
      }
    }
  }

  /* Ragged borders, then the lift. */
  if (pmd.jitter > 0.0f) {
    for (const int v : IndexRange(pverts)) {
      if (!on_border[v]) {
        continue;
      }
      float3 r(rand01(uint32_t(v), seed, 11) - 0.5f,
               rand01(uint32_t(v), seed, 12) - 0.5f,
               rand01(uint32_t(v), seed, 13) - 0.5f);
      r -= normals[v] * math::dot(r, normals[v]);
      if (math::length_squared(r) < 1e-12f) {
        continue;
      }
      const float limit = std::min(pmd.jitter, 0.3f * min_edge[v]);
      patches.positions[v] += math::normalize(r) * (limit * rand01(uint32_t(v), seed, 14));
    }
  }
  auto cell_lift = [&](const int cell) {
    return pmd.lift + pmd.lift_random * rand01(uint32_t(cell), seed, 21);
  };
  for (const int v : IndexRange(pverts)) {
    patches.positions[v] += normals[v] * cell_lift(patches.group[v]);
  }
  const float thickness = std::max(pmd.thickness, 0.0f);

  /* Stitches along the borders, one straight side at a time. */
  Vector<StitchSpot> spots;
  Vector<Mesh *> shapes;
  bool too_many_stitches = false;
  if (pmd.flag & MOD_PATCH_STITCHES) {
    if (pmd.stitch_collection != nullptr) {
      Vector<Object *> objects;
      FOREACH_COLLECTION_OBJECT_RECURSIVE_BEGIN (pmd.stitch_collection, ob) {
        objects.append(ob);
      }
      FOREACH_COLLECTION_OBJECT_RECURSIVE_END;
      std::sort(objects.begin(), objects.end(), [](const Object *a, const Object *b) {
        return std::strcmp(a->id.name, b->id.name) < 0;
      });
      for (Object *ob : objects) {
        if (Mesh *shape = make_stitch_shape(ob, ctx->object, mesh)) {
          shapes.append(shape);
        }
      }
    }
    else if (Mesh *shape = make_stitch_shape(pmd.stitch_object, ctx->object, mesh)) {
      shapes.append(shape);
    }

    Vector<int> usable;
    for (const int e : border.index_range()) {
      const BorderEdge &be = border[e];
      if (be.label == NO_LABEL) {
        continue;
      }
      /* Where two patches meet, only one of them is sewn, so there's one row of stitches. */
      if (was_patched(be.label) && be.cell > be.label) {
        continue;
      }
      const uint32_t lo = uint32_t(std::min(be.cell, be.label));
      const uint32_t hi = uint32_t(std::max(be.cell, be.label));
      if (rand01(lo, hi, seed + 31u) >= pmd.stitch_amount) {
        continue;
      }
      usable.append(e);
    }
    /* Follow each side from edge to edge while the neighbor stays the same and it runs
     * straight on. */
    Map<int, Vector<int>> starts;
    for (const int e : usable) {
      starts.lookup_or_add_default(border[e].a).append(e);
    }
    auto top = [&](const int v) { return patches.positions[v] + normals[v] * thickness; };
    auto dir = [&](const int e) {
      const float3 d = top(border[e].b) - top(border[e].a);
      return math::length_squared(d) > 0.0f ? math::normalize(d) : float3(0.0f);
    };
    Map<int, int> next_edge;
    Set<int> has_prev;
    for (const int e : usable) {
      const Vector<int> *after = starts.lookup_ptr(border[e].b);
      if (after == nullptr) {
        continue;
      }
      for (const int e2 : *after) {
        if (e2 == e || border[e2].label != border[e].label || border[e2].cell != border[e].cell) {
          continue;
        }
        if (math::dot(dir(e), dir(e2)) < 0.82f) {
          continue;
        }
        next_edge.add(e, e2);
        has_prev.add(e2);
        break;
      }
    }
    Set<int> used;
    auto lay_side = [&](const int first) {
      Vector<int> verts = {border[first].a};
      int e = first;
      while (true) {
        used.add(e);
        verts.append(border[e].b);
        const int *nxt = next_edge.lookup_ptr(e);
        if (nxt == nullptr || used.contains(*nxt)) {
          break;
        }
        e = *nxt;
      }
      const int cell = border[first].cell;
      const float lift = cell_lift(cell);
      /* In Split mode the stitch sits in the middle of the gap. The cloth beside a piece isn't
       * shrunk, so that gap is half as wide. */
      const float shift = split ? pmd.gap * (was_patched(border[first].label) ? 0.5f : 0.25f) :
                                  0.0f;
      Array<float> dist(verts.size());
      dist[0] = 0.0f;
      for (const int i : IndexRange(1, verts.size() - 1)) {
        dist[i] = dist[i - 1] + math::distance(top(verts[i - 1]), top(verts[i]));
      }
      const float length = dist.last();
      const int count = int(length / std::max(pmd.stitch_spacing, 1e-5f) + 0.5f);
      for (const int k : IndexRange(count)) {
        if (spots.size() >= MAX_STITCHES) {
          too_many_stitches = true;
          return;
        }
        const float x = (float(k) + 0.5f) * length / float(count);
        int i = int(std::upper_bound(dist.begin() + 1, dist.end(), x) - (dist.begin() + 1));
        i = std::clamp(i, 0, int(verts.size()) - 2);
        const float seg = dist[i + 1] - dist[i];
        const float t = seg > 0.0f ? (x - dist[i]) / seg : 0.0f;
        const float3 p = math::interpolate(top(verts[i]), top(verts[i + 1]), t);
        float3 n = math::interpolate(normals[verts[i]], normals[verts[i + 1]], t);
        n = math::length_squared(n) > 0.0f ? math::normalize(n) : float3(0.0f, 0.0f, 1.0f);
        float3 along = top(verts[i + 1]) - top(verts[i]);
        along -= n * math::dot(along, n);
        if (math::length_squared(along) < 1e-14f) {
          continue;
        }
        along = math::normalize(along);
        const float3 inward = math::normalize(math::cross(n, along));
        const uint32_t key = uint32_t(spots.size());
        const float angle = (rand01(key, seed, 41) * 2.0f - 1.0f) * pmd.stitch_angle_random;
        const float scale = std::max(
            1.0f + (rand01(key, seed, 42) * 2.0f - 1.0f) * pmd.stitch_size_random, 0.05f);
        const float3 x_axis = inward * std::cos(angle) + along * std::sin(angle);
        const float3 y_axis = math::cross(n, x_axis);
        const float3 center = p - inward * shift;

        StitchSpot spot;
        spot.transform = float4x4::identity();
        if (shapes.is_empty()) {
          /* From the surface under the patch up to above its top. */
          const float z0 = -(lift + thickness);
          const float z1 = pmd.stitch_thickness * scale;
          if (z1 - z0 <= 0.0f || pmd.stitch_length <= 0.0f || pmd.stitch_width <= 0.0f) {
            continue;
          }
          spot.shape = -1;
          spot.transform.x_axis() = x_axis * (pmd.stitch_length * scale);
          spot.transform.y_axis() = y_axis * (pmd.stitch_width * scale);
          spot.transform.z_axis() = n * (z1 - z0);
          spot.transform.location() = center + n * ((z0 + z1) * 0.5f);
        }
        else {
          spot.shape = shapes.size() > 1 ?
                           int(noise::hash(key, seed + 43u) % uint32_t(shapes.size())) :
                           0;
          spot.transform.x_axis() = x_axis * scale;
          spot.transform.y_axis() = y_axis * scale;
          spot.transform.z_axis() = n * scale;
          spot.transform.location() = center;
        }
        spots.append(spot);
      }
    };
    for (const int e : usable) {
      if (!used.contains(e) && !has_prev.contains(e)) {
        lay_side(e);
      }
    }
    for (const int e : usable) {
      if (!used.contains(e)) {
        lay_side(e);
      }
    }
  }
  if (too_many_stitches) {
    BKE_modifier_set_warning(ctx->object, md, "Stopped at %d stitches", MAX_STITCHES);
  }

  /* Thickness: the patches grow up from where they sit. The cloth grows toward the back in
   * Overlay mode, so the patches stay on top, and like the pieces in Split mode. */
  solidify_soup(patches, 0.0f, thickness);
  const bool thick_cloth = (pmd.flag & MOD_PATCH_THICK_CLOTH) && thickness > 0.0f;

  /* Put it together. */
  std::unique_ptr<bke::Instances> instances = std::make_unique<bke::Instances>();
  Vector<int> instance_handles;
  Vector<float4x4> instance_transforms;
  auto add_part = [&](Mesh *part) {
    instance_handles.append(instances->add_new_reference(
        bke::InstanceReference(bke::GeometrySet::from_mesh(part))));
    instance_transforms.append(float4x4::identity());
  };

  Mesh *base = nullptr;
  if (need_cloth) {
    if (cloth.faces_num() > 0) {
      if (thick_cloth) {
        solidify_soup(cloth, split ? 0.0f : -thickness, split ? thickness : 0.0f);
      }
      base = soup_to_mesh(cloth, mesh);
    }
  }
  else if (thick_cloth) {
    Soup whole = soup_from_mesh(mesh);
    solidify_soup(whole, -thickness, 0.0f);
    base = soup_to_mesh(whole, mesh);
  }
  else {
    base = BKE_mesh_copy_for_eval(mesh);
  }
  if (base != nullptr) {
    set_face_ints(*base, "patch_id", -1);
    add_part(base);
  }
  if (patches.faces_num() > 0) {
    Mesh *part = soup_to_mesh(patches, mesh);
    bke::MutableAttributeAccessor attributes = part->attributes_for_write();
    bke::SpanAttributeWriter<int> material = attributes.lookup_or_add_for_write_span<int>(
        "material_index", bke::AttrDomain::Face);
    bke::SpanAttributeWriter<int> patch_ids = attributes.lookup_or_add_for_write_only_span<int>(
        "patch_id", bke::AttrDomain::Face);
    /* A picked material is one material, not the first of several slots. */
    const int choices = pmd.patch_material_pick ? 1 : std::max(pmd.material_random, 1);
    for (const int f : IndexRange(patches.faces_num())) {
      const int cell = patches.face_cell[f];
      material.span[f] = patch_material +
                         int(noise::hash(uint32_t(cell), seed + 51u) % uint32_t(choices));
      patch_ids.span[f] = cell;
    }
    material.finish();
    patch_ids.finish();
    add_part(part);
  }
  if (!spots.is_empty()) {
    Array<int> handles(std::max(int(shapes.size()), 1), -1);
    int box_handle = -1;
    for (const StitchSpot &spot : spots) {
      int handle;
      if (spot.shape < 0) {
        if (box_handle < 0) {
          Mesh *box = make_box(mesh);
          set_face_ints(*box, "material_index", stitch_material);
          set_face_ints(*box, "patch_id", -1);
          box_handle = instances->add_new_reference(
              bke::InstanceReference(bke::GeometrySet::from_mesh(box)));
        }
        handle = box_handle;
      }
      else {
        if (handles[spot.shape] < 0) {
          Mesh *shape = shapes[spot.shape];
          shapes[spot.shape] = nullptr;
          set_face_ints(*shape, "material_index", stitch_material);
          set_face_ints(*shape, "patch_id", -1);
          handles[spot.shape] = instances->add_new_reference(
              bke::InstanceReference(bke::GeometrySet::from_mesh(shape)));
        }
        handle = handles[spot.shape];
      }
      instance_handles.append(handle);
      instance_transforms.append(spot.transform);
    }
  }
  for (Mesh *shape : shapes) {
    if (shape != nullptr) {
      BKE_id_free(nullptr, shape);
    }
  }

  instances->resize(instance_handles.size());
  instances->reference_handles_for_write().copy_from(instance_handles);
  instances->transforms_for_write().copy_from(instance_transforms);
  bke::GeometrySet result = bke::GeometrySet::from_instances(instances.release());
  result = geometry::realize_instances(result, geometry::RealizeInstancesOptions()).geometry;
  *geometry_set = std::move(result);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name UI
 * \{ */

static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  layout.use_property_split_set(true);
  layout.prop(ptr, "mode", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
  layout.prop(ptr, "layout", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  const int pieces = RNA_enum_get(ptr, "layout");
  const bool split = RNA_enum_get(ptr, "mode") == MOD_PATCH_MODE_SPLIT;
  if (pieces == MOD_PATCH_LAYOUT_VORONOI) {
    ui::Layout &col = layout.column(true);
    col.prop(ptr, "count", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col.prop(ptr, "relax", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    layout.prop(ptr, "voronoi_scale", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  else if (pieces == MOD_PATCH_LAYOUT_GRID) {
    layout.prop(ptr, "grid_size", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  else if (pieces == MOD_PATCH_LAYOUT_STRIPS) {
    layout.prop(ptr, "strip_axis", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
    layout.prop(ptr, "strip_width", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  else {
    layout.prop(ptr, "cut_object", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  layout.prop(ptr, "seed", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "use_per_island", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  modifier_vgroup_ui(layout, ptr, &ob_ptr, "vertex_group", "invert_vertex_group", std::nullopt);

  layout.separator();
  ui::Layout &shape = layout.column(true);
  if (split) {
    shape.prop(ptr, "gap", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  else {
    if (pieces != MOD_PATCH_LAYOUT_OBJECT) {
      shape.prop(ptr, "amount", ui::ITEM_R_SLIDER, std::nullopt, ICON_NONE);
    }
    shape.prop(ptr, "margin", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    layout.prop(ptr, "use_cut_hole", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  ui::Layout &lift = layout.column(true);
  lift.prop(ptr, "lift", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  lift.prop(ptr, "lift_random", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  lift.prop(ptr, "jitter", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "thickness", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  ui::Layout &row = layout.row(false);
  row.active_set(RNA_float_get(ptr, "thickness") > 0.0f);
  row.prop(ptr, "use_thick_cloth", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  modifier_error_message_draw(layout, ptr);
}

static void stitches_header_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);
  layout.prop(ptr, "use_stitches", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void stitches_panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  layout.use_property_split_set(true);
  layout.active_set(RNA_boolean_get(ptr, "use_stitches"));
  layout.prop(ptr, "stitch_object", UI_ITEM_NONE, IFACE_("Object"), ICON_NONE);
  layout.prop(ptr, "stitch_collection", UI_ITEM_NONE, IFACE_("Collection"), ICON_NONE);
  PointerRNA object_ptr = RNA_pointer_get(ptr, "stitch_object");
  PointerRNA collection_ptr = RNA_pointer_get(ptr, "stitch_collection");
  const bool custom = !RNA_pointer_is_null(&object_ptr) || !RNA_pointer_is_null(&collection_ptr);
  ui::Layout &col = layout.column(true);
  col.prop(ptr, "stitch_spacing", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  if (!custom) {
    col.prop(ptr, "stitch_length", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col.prop(ptr, "stitch_width", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col.prop(ptr, "stitch_thickness", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  ui::Layout &random = layout.column(true);
  random.prop(ptr, "stitch_amount", ui::ITEM_R_SLIDER, std::nullopt, ICON_NONE);
  random.prop(ptr, "stitch_size_random", ui::ITEM_R_SLIDER, std::nullopt, ICON_NONE);
  random.prop(ptr, "stitch_angle_random", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void materials_panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  layout.use_property_split_set(true);
  ui::Layout &col = layout.column(true);
  modifier_material_ui(col, ptr, "patch_material_pick", "patch_material", IFACE_("Patch"));
  col.prop(ptr, "material_random", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  modifier_material_ui(layout, ptr, "stitch_material_pick", "stitch_material", IFACE_("Stitch"));
}

static void panel_register(ARegionType *region_type)
{
  PanelType *panel_type = modifier_panel_register(region_type, eModifierType_Patch, panel_draw);
  modifier_subpanel_register(region_type,
                             "stitches",
                             "",
                             stitches_header_draw,
                             stitches_panel_draw,
                             panel_type);
  modifier_subpanel_register(
      region_type, "materials", "Materials", nullptr, materials_panel_draw, panel_type);
}

/** \} */

ModifierTypeInfo modifierType_Patch = {
    /*idname*/ "Patch",
    /*name*/ N_("Patch"),
    /*struct_name*/ "PatchModifierData",
    /*struct_size*/ sizeof(PatchModifierData),
    /*srna*/ &RNA_PatchModifier,
    /*type*/ ModifierTypeType::Constructive,
    /*flags*/ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsEditmode |
        eModifierTypeFlag_EnableInEditmode,
    /*icon*/ ICON_MOD_PATCH,
    /*copy_data*/ BKE_modifier_copydata_generic,
    /*deform_verts*/ nullptr,
    /*deform_matrices*/ nullptr,
    /*deform_verts_EM*/ nullptr,
    /*deform_matrices_EM*/ nullptr,
    /*modify_mesh*/ nullptr,
    /*modify_geometry_set*/ modify_geometry_set,
    /*init_data*/ init_data,
    /*required_data_mask*/ required_data_mask,
    /*free_data*/ nullptr,
    /*is_disabled*/ nullptr,
    /*update_depsgraph*/ update_depsgraph,
    /*depends_on_time*/ nullptr,
    /*depends_on_normals*/ nullptr,
    /*foreach_ID_link*/ foreach_ID_link,
    /*foreach_tex_link*/ nullptr,
    /*free_runtime_data*/ nullptr,
    /*panel_register*/ panel_register,
    /*blend_write*/ nullptr,
    /*blend_read*/ nullptr,
    /*foreach_cache*/ nullptr,
    /*foreach_working_space_color*/ nullptr,
};

}  // namespace blender
