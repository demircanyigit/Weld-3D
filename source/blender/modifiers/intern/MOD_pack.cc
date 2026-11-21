/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Pack: fills the volume of the mesh with copies of other objects, fitted close together.
 *
 * The volume is measured on a grid of cells. Each piece is turned into the cells it fills, and
 * pieces go in one at a time where they fit without overlapping: where they touch the most (Fill)
 * or as low as they can drop (Settle). With Fill Gaps, each piece then swells into the empty cells
 * closest to it, so the pieces fit each other like the stones of a dry stone wall.
 */

#include <algorithm>
#include <climits>
#include <cmath>

#include "BLI_array.hh"
#include "BLI_listbase.h"
#include "BLI_math_base.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_noise.hh"
#include "BLI_rand.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "DNA_collection_types.h"
#include "DNA_layer_types.h"
#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "BKE_collection.hh"
#include "BKE_geometry_set.hh"
#include "BKE_geometry_set_instances.hh"
#include "BKE_instances.hh"
#include "BKE_lib_query.hh"
#include "BKE_mesh.hh"
#include "BKE_modifier.hh"

#include "BLO_read_write.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"

#include "GEO_realize_instances.hh"

#include "MOD_modifiertypes.hh"
#include "MOD_ui_common.hh"
#include "MOD_util.hh"

namespace blender {

static void init_data(ModifierData *md)
{
  PackModifierData *pmd = reinterpret_cast<PackModifierData *>(md);
  INIT_DEFAULT_STRUCT_AFTER(pmd, modifier);
}

static void free_data(ModifierData *md)
{
  PackModifierData &pmd = *reinterpret_cast<PackModifierData *>(md);
  BLI_freelistN(&pmd.objects);
}

static void copy_data(const ModifierData *md, ModifierData *target, const int flag)
{
  BKE_modifier_copydata_generic(md, target, flag);
  const PackModifierData &src = *reinterpret_cast<const PackModifierData *>(md);
  PackModifierData &dst = *reinterpret_cast<PackModifierData *>(target);
  BLI_duplicatelist(&dst.objects, &src.objects);
}

static void foreach_ID_link(ModifierData *md, Object *ob, IDWalkFunc walk, void *user_data)
{
  PackModifierData &pmd = *reinterpret_cast<PackModifierData *>(md);
  walk(user_data, ob, reinterpret_cast<ID **>(&pmd.object), IDWALK_CB_NOP);
  walk(user_data, ob, reinterpret_cast<ID **>(&pmd.collection), IDWALK_CB_USER);
  for (PackModifierObject &item : pmd.objects) {
    walk(user_data, ob, reinterpret_cast<ID **>(&item.object), IDWALK_CB_NOP);
  }
}

/** Every object to pack, once each, without the object with the modifier. */
static Vector<Object *> gather_objects(const PackModifierData &pmd, const Object *self)
{
  Vector<Object *> objects;
  auto add = [&](Object *ob) {
    if (ob != nullptr && ob != self && !objects.contains(ob)) {
      objects.append(ob);
    }
  };
  add(pmd.object);
  if (pmd.collection != nullptr) {
    FOREACH_COLLECTION_OBJECT_RECURSIVE_BEGIN (pmd.collection, ob) {
      add(ob);
    }
    FOREACH_COLLECTION_OBJECT_RECURSIVE_END;
  }
  for (const PackModifierObject &item : pmd.objects) {
    add(item.object);
  }
  return objects;
}

static void update_depsgraph(ModifierData *md, const ModifierUpdateDepsgraphContext *ctx)
{
  PackModifierData &pmd = *reinterpret_cast<PackModifierData *>(md);
  if (pmd.collection != nullptr) {
    DEG_add_collection_geometry_relation(ctx->node, pmd.collection, "Pack Modifier");
  }
  for (Object *ob : gather_objects(pmd, ctx->object)) {
    DEG_add_object_relation(ctx->node, ob, DEG_OB_COMP_GEOMETRY, "Pack Modifier");
    DEG_add_object_relation(ctx->node, ob, DEG_OB_COMP_TRANSFORM, "Pack Modifier");
  }
  /* The turn and scale of the pieces are measured against the object's own. */
  DEG_add_depends_on_transform_relation(ctx->node, "Pack Modifier");
}

static bool is_disabled(const Scene * /*scene*/, ModifierData *md, bool /*use_render_params*/)
{
  const PackModifierData &pmd = *reinterpret_cast<const PackModifierData *>(md);
  if (pmd.object != nullptr || pmd.collection != nullptr) {
    return false;
  }
  for (const PackModifierObject &item : pmd.objects) {
    if (item.object != nullptr) {
      return false;
    }
  }
  return true;
}

/* -------------------------------------------------------------------- */
/** \name Cells
 * \{ */

/**
 * Marks the cells whose middle is inside a closed surface. The points are in cell units, cell
 * (i, j, k) spans [i, i + 1) and so on. Rays along X count how often they cross the surface.
 */
static void fill_inside(const Span<float3> points,
                        const Span<int3> tris,
                        const int3 size,
                        MutableSpan<bool> r_inside)
{
  Array<Vector<float>> crossings(size_t(size.y) * size.z);
  /* Tiny shifts so the rays don't run exactly along edges. */
  const float jy = 0.5f + 1.37e-4f;
  const float jz = 0.5f + 2.91e-4f;
  for (const int3 &tri : tris) {
    const float3 a = points[tri.x], b = points[tri.y], c = points[tri.z];
    const float ymin = std::min({a.y, b.y, c.y}), ymax = std::max({a.y, b.y, c.y});
    const float zmin = std::min({a.z, b.z, c.z}), zmax = std::max({a.z, b.z, c.z});
    const int j0 = std::max(0, int(std::ceil(ymin - jy)));
    const int j1 = std::min(size.y - 1, int(std::floor(ymax - jy)));
    const int k0 = std::max(0, int(std::ceil(zmin - jz)));
    const int k1 = std::min(size.z - 1, int(std::floor(zmax - jz)));
    if (j0 > j1 || k0 > k1) {
      continue;
    }
    const float2 e1(b.y - a.y, b.z - a.z), e2(c.y - a.y, c.z - a.z);
    const float d = e1.x * e2.y - e2.x * e1.y;
    if (std::abs(d) < 1e-12f) {
      continue;
    }
    for (int k = k0; k <= k1; k++) {
      for (int j = j0; j <= j1; j++) {
        const float2 r(float(j) + jy - a.y, float(k) + jz - a.z);
        const float w1 = (r.x * e2.y - e2.x * r.y) / d;
        const float w2 = (e1.x * r.y - r.x * e1.y) / d;
        if (w1 < 0.0f || w2 < 0.0f || w1 + w2 > 1.0f) {
          continue;
        }
        crossings[j + size_t(k) * size.y].append(a.x + (b.x - a.x) * w1 + (c.x - a.x) * w2);
      }
    }
  }
  threading::parallel_for(crossings.index_range(), 256, [&](const IndexRange range) {
    for (const int column : range) {
      Vector<float> &xs = crossings[column];
      std::sort(xs.begin(), xs.end());
      const int j = column % size.y, k = column / size.y;
      for (int i = 0; i + 1 < xs.size(); i += 2) {
        const int x0 = std::max(0, int(std::ceil(xs[i] - 0.5f)));
        const int x1 = std::min(size.x, int(std::ceil(xs[i + 1] - 0.5f)));
        for (int x = x0; x < x1; x++) {
          r_inside[x + size.x * (j + size_t(size.y) * k)] = true;
        }
      }
    }
  });
}

/** Whether every edge of the mesh has exactly two faces. */
static bool is_closed(const Mesh &mesh)
{
  if (mesh.faces_num == 0) {
    return false;
  }
  Array<int> uses(mesh.edges_num, 0);
  for (const int edge : mesh.corner_edges()) {
    uses[edge]++;
  }
  return std::all_of(uses.begin(), uses.end(), [](const int n) { return n == 2; });
}

static Array<int3> triangles(const Mesh &mesh)
{
  const Span<int3> corner_tris = mesh.corner_tris();
  const Span<int> corner_verts = mesh.corner_verts();
  Array<int3> tris(corner_tris.size());
  for (const int i : corner_tris.index_range()) {
    tris[i] = int3(corner_verts[corner_tris[i].x],
                   corner_verts[corner_tris[i].y],
                   corner_verts[corner_tris[i].z]);
  }
  return tris;
}

/** Cell values of the volume. */
static constexpr int FREE = -1;
static constexpr int WALL = -2;

struct Volume {
  int3 size;
  float3 origin;
  float cell;
  /** #FREE, #WALL or the piece that fills the cell. */
  Array<int> owner;
  /** Per column, the lowest cell a piece may drop to. */
  Array<int> floor;

  int64_t index(const int3 &c) const
  {
    return c.x + int64_t(size.x) * (c.y + int64_t(size.y) * c.z);
  }
  bool contains(const int3 &c) const
  {
    return c.x >= 0 && c.y >= 0 && c.z >= 0 && c.x < size.x && c.y < size.y && c.z < size.z;
  }
  /** Outside the volume counts as wall. */
  int at(const int3 &c) const
  {
    return contains(c) ? owner[index(c)] : WALL;
  }
};

/** The cells a piece fills in one turn, around the cell it is placed at. */
struct PieceCells {
  /** The turn and scale, in the units of the object with the modifier. */
  float3x3 linear;
  Vector<int3> cells;
  /** The empty cells around it. */
  Vector<int3> shell;
  int3 lo, hi;
  /** Per column from #lo to #hi in X and Y, the lowest and highest cell. INT_MAX when empty. */
  Array<int> bottom, top;
  bool valid = false;

  int column(const int x, const int y) const
  {
    return (x - lo.x) + (hi.x - lo.x + 1) * (y - lo.y);
  }
};

/** A source object, measured once. */
struct PackSource {
  Object *object;
  bke::GeometrySet geometry;
  const Mesh *mesh;
  /** The object's turn and scale, in the space of the object with the modifier. */
  float3x3 shape;
  /** Middle of the mesh bounds, in the object's own space. */
  float3 center;
  /** The thinnest direction of the shaped mesh. */
  float3 flat_axis;
  Array<int3> tris;
};

static PieceCells piece_cells(const PackSource &src,
                              const float3x3 &linear,
                              const float cell,
                              const int dilate)
{
  PieceCells piece;
  piece.linear = linear;
  const Span<float3> positions = src.mesh->vert_positions();
  Array<float3> q(positions.size());
  float3 lo_f(FLT_MAX), hi_f(-FLT_MAX);
  for (const int i : positions.index_range()) {
    q[i] = linear * (positions[i] - src.center) / cell;
    lo_f = math::min(lo_f, q[i]);
    hi_f = math::max(hi_f, q[i]);
  }
  const int pad = dilate + 1;
  const int3 origin = int3(int(std::floor(lo_f.x)), int(std::floor(lo_f.y)), int(std::floor(lo_f.z))) -
                      int3(pad);
  const int3 size = int3(int(std::ceil(hi_f.x)), int(std::ceil(hi_f.y)), int(std::ceil(hi_f.z))) -
                    origin + int3(pad + 1);
  const int64_t total = int64_t(size.x) * size.y * size.z;
  if (total <= 0 || total > 16 * 1024 * 1024) {
    return piece;
  }
  for (float3 &p : q) {
    p -= float3(origin);
  }
  Array<bool> inside(total, false);
  if (!src.tris.is_empty()) {
    fill_inside(q, src.tris, size, inside);
  }
  auto index = [&](const int3 &c) { return c.x + int64_t(size.x) * (c.y + int64_t(size.y) * c.z); };
  /* Open shapes have no inside: the cells of their points stand in for it. Closed ones only take
   * the cells whose middle is inside, so they don't grow by a cell all around. */
  if (src.tris.is_empty()) {
    for (const float3 &p : q) {
      const int3 c(int(p.x), int(p.y), int(p.z));
      inside[index(c)] = true;
    }
  }
  if (dilate > 0) {
    Array<bool> grown = inside;
    const int r2 = dilate * dilate;
    for (int z = 0; z < size.z; z++) {
      for (int y = 0; y < size.y; y++) {
        for (int x = 0; x < size.x; x++) {
          if (!inside[index(int3(x, y, z))]) {
            continue;
          }
          for (int dz = -dilate; dz <= dilate; dz++) {
            for (int dy = -dilate; dy <= dilate; dy++) {
              for (int dx = -dilate; dx <= dilate; dx++) {
                if (dx * dx + dy * dy + dz * dz > r2) {
                  continue;
                }
                const int3 c(x + dx, y + dy, z + dz);
                if (c.x >= 0 && c.y >= 0 && c.z >= 0 && c.x < size.x && c.y < size.y &&
                    c.z < size.z)
                {
                  grown[index(c)] = true;
                }
              }
            }
          }
        }
      }
    }
    inside = std::move(grown);
  }

  piece.lo = int3(INT_MAX);
  piece.hi = int3(INT_MIN);
  Array<bool> is_shell(total, false);
  const int3 sides[6] = {
      int3(1, 0, 0), int3(-1, 0, 0), int3(0, 1, 0), int3(0, -1, 0), int3(0, 0, 1), int3(0, 0, -1)};
  for (int z = 0; z < size.z; z++) {
    for (int y = 0; y < size.y; y++) {
      for (int x = 0; x < size.x; x++) {
        const int3 c(x, y, z);
        if (!inside[index(c)]) {
          continue;
        }
        piece.cells.append(c + origin);
        piece.lo = math::min(piece.lo, c + origin);
        piece.hi = math::max(piece.hi, c + origin);
        for (const int3 &side : sides) {
          const int3 n = c + side;
          if (n.x < 0 || n.y < 0 || n.z < 0 || n.x >= size.x || n.y >= size.y || n.z >= size.z) {
            continue;
          }
          if (!inside[index(n)] && !is_shell[index(n)]) {
            is_shell[index(n)] = true;
            piece.shell.append(n + origin);
          }
        }
      }
    }
  }
  if (piece.cells.is_empty()) {
    return piece;
  }
  const int columns = (piece.hi.x - piece.lo.x + 1) * (piece.hi.y - piece.lo.y + 1);
  piece.bottom = Array<int>(columns, INT_MAX);
  piece.top = Array<int>(columns, INT_MIN);
  for (const int3 &c : piece.cells) {
    const int col = piece.column(c.x, c.y);
    piece.bottom[col] = std::min(piece.bottom[col], c.z);
    piece.top[col] = std::max(piece.top[col], c.z);
  }
  piece.valid = true;
  return piece;
}

static bool fits(const Volume &volume, const PieceCells &piece, const int3 &at)
{
  const int3 lo = at + piece.lo, hi = at + piece.hi;
  if (!volume.contains(lo) || !volume.contains(hi)) {
    return false;
  }
  for (const int3 &c : piece.cells) {
    if (volume.owner[volume.index(at + c)] != FREE) {
      return false;
    }
  }
  return true;
}

/** How much of the piece touches walls or other pieces, 0 to 1. */
static float contact(const Volume &volume, const PieceCells &piece, const int3 &at)
{
  int touching = 0;
  for (const int3 &c : piece.shell) {
    if (volume.at(at + c) != FREE) {
      touching++;
    }
  }
  return piece.shell.is_empty() ? 0.0f : float(touching) / float(piece.shell.size());
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Turns
 * \{ */

static float3x3 rotation_axis_angle(const float3 &axis, const float angle)
{
  const float c = std::cos(angle), s = std::sin(angle), t = 1.0f - c;
  const float3 k = math::normalize(axis);
  float3x3 m;
  m[0] = float3(t * k.x * k.x + c, t * k.x * k.y + s * k.z, t * k.x * k.z - s * k.y);
  m[1] = float3(t * k.x * k.y - s * k.z, t * k.y * k.y + c, t * k.y * k.z + s * k.x);
  m[2] = float3(t * k.x * k.z + s * k.y, t * k.y * k.z - s * k.x, t * k.z * k.z + c);
  return m;
}

/** A turn picked evenly from all turns. */
static float3x3 random_rotation(RandomNumberGenerator &rng)
{
  const float u1 = rng.get_float(), u2 = rng.get_float(), u3 = rng.get_float();
  const float a = std::sqrt(1.0f - u1), b = std::sqrt(u1);
  const float x = a * std::sin(2.0f * float(M_PI) * u2), y = a * std::cos(2.0f * float(M_PI) * u2);
  const float z = b * std::sin(2.0f * float(M_PI) * u3), w = b * std::cos(2.0f * float(M_PI) * u3);
  float3x3 m;
  m[0] = float3(1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y));
  m[1] = float3(2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x));
  m[2] = float3(2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y));
  return m;
}

/** The turn that takes direction #from to #to. */
static float3x3 rotation_between(const float3 &from, const float3 &to)
{
  const float3 axis = math::cross(from, to);
  const float s = math::length(axis), c = math::dot(from, to);
  if (s < 1e-6f) {
    if (c > 0.0f) {
      return float3x3::identity();
    }
    const float3 other = std::abs(from.x) < 0.9f ? float3(1, 0, 0) : float3(0, 1, 0);
    return rotation_axis_angle(math::cross(from, other), float(M_PI));
  }
  return rotation_axis_angle(axis / s, std::atan2(s, c));
}

/** The thinnest direction of a cloud of points. */
static float3 thinnest_axis(const Span<float3> points)
{
  if (points.size() < 3) {
    return float3(0, 0, 1);
  }
  float3 mean(0.0f);
  for (const float3 &p : points) {
    mean += p;
  }
  mean /= float(points.size());
  float3x3 cov = float3x3::zero();
  for (const float3 &p : points) {
    const float3 d = p - mean;
    for (int i = 0; i < 3; i++) {
      for (int j = 0; j < 3; j++) {
        cov[i][j] += d[i] * d[j];
      }
    }
  }
  /* The largest direction of (trace - covariance) is the smallest of the covariance. */
  const float trace = cov[0][0] + cov[1][1] + cov[2][2];
  float3x3 m = float3x3::identity() * trace - cov;
  float3 v = math::normalize(float3(0.3f, 0.5f, 0.8f));
  for (int i = 0; i < 40; i++) {
    const float3 next = m * v;
    const float len = math::length(next);
    if (len < 1e-12f) {
      break;
    }
    v = next / len;
  }
  return v;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Placing
 * \{ */

struct Found {
  bool valid = false;
  int3 at;
  /** Higher is better. */
  float score = -FLT_MAX;
};

/** Fill: the anchor among #anchors (and the steps from there) where the piece touches the most. */
static Found search_fill(const Volume &volume, const PieceCells &piece, const Span<int3> anchors)
{
  Found best;
  for (const int3 &a : anchors) {
    if (!fits(volume, piece, a)) {
      continue;
    }
    const float score = contact(volume, piece, a);
    if (score > best.score) {
      best = {true, a, score};
    }
  }
  if (!best.valid) {
    return best;
  }
  /* Slide into the closest contact. */
  const int3 steps[6] = {
      int3(1, 0, 0), int3(-1, 0, 0), int3(0, 1, 0), int3(0, -1, 0), int3(0, 0, 1), int3(0, 0, -1)};
  for (int i = 0; i < 24; i++) {
    Found next = best;
    for (const int3 &step : steps) {
      const int3 a = best.at + step;
      if (!fits(volume, piece, a)) {
        continue;
      }
      const float score = contact(volume, piece, a);
      if (score > next.score + 1e-6f) {
        next = {true, a, score};
      }
    }
    if (next.at == best.at) {
      break;
    }
    best = next;
  }
  return best;
}

/** Settle: the lowest place the piece drops to, within the columns of #window (all when its
 * size is zero). */
static Found search_settle_step(const Volume &volume,
                                const PieceCells &piece,
                                const uint32_t seed,
                                const int2 window_min,
                                const int2 window_max,
                                const int stride)
{
  struct Candidate {
    float key;
    int3 at;
  };
  Vector<Candidate> candidates;
  const int x0 = std::max(-piece.lo.x, window_min.x);
  const int x1 = std::min(volume.size.x - 1 - piece.hi.x, window_max.x);
  const int y0 = std::max(-piece.lo.y, window_min.y);
  const int y1 = std::min(volume.size.y - 1 - piece.hi.y, window_max.y);
  for (int ay = y0; ay <= y1; ay += stride) {
    for (int ax = x0; ax <= x1; ax += stride) {
      int z = -piece.lo.z;
      for (int y = piece.lo.y; y <= piece.hi.y; y++) {
        for (int x = piece.lo.x; x <= piece.hi.x; x++) {
          const int bottom = piece.bottom[piece.column(x, y)];
          if (bottom == INT_MAX) {
            continue;
          }
          z = std::max(z, volume.floor[(ax + x) + volume.size.x * (ay + y)] - bottom);
        }
      }
      if (z + piece.hi.z >= volume.size.z) {
        continue;
      }
      /* A little random order among places at the same height. */
      const float jitter = noise::hash_to_float(seed, uint32_t(ax), uint32_t(ay)) * 0.9f;
      candidates.append({float(z) + jitter, int3(ax, ay, z)});
    }
  }
  std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
    return a.key < b.key;
  });
  for (const int i : candidates.index_range().take_front(96)) {
    for (int lift = 0; lift < 4; lift++) {
      const int3 a = candidates[i].at + int3(0, 0, lift);
      if (fits(volume, piece, a)) {
        return {true, a, -float(a.z)};
      }
    }
  }
  return {};
}

/** Settle: the lowest place the piece drops to, within the columns of #window. Looks over every
 * few columns first, then at every column around the best of those. */
static Found search_settle(const Volume &volume,
                           const PieceCells &piece,
                           const uint32_t seed,
                           const int2 window_min,
                           const int2 window_max)
{
  const int3 size = piece.hi - piece.lo + int3(1);
  const int stride = std::max(1, std::min(size.x, size.y) / 6);
  Found found = search_settle_step(volume, piece, seed, window_min, window_max, stride);
  if (!found.valid || stride == 1) {
    return found;
  }
  const int2 at(found.at.x, found.at.y);
  const Found fine = search_settle_step(volume,
                                        piece,
                                        seed,
                                        math::max(window_min, at - int2(stride)),
                                        math::min(window_max, at + int2(stride)),
                                        1);
  return (fine.valid && fine.score > found.score) ? fine : found;
}

/** The direction away from the walls near a place, zero when there are none. */
static float3 away_from_walls(const Volume &volume, const int3 &at, const int radius)
{
  float3 sum(0.0f);
  int walls = 0;
  const int stride = std::max(1, radius / 6);
  for (int z = -radius; z <= radius; z += stride) {
    for (int y = -radius; y <= radius; y += stride) {
      for (int x = -radius; x <= radius; x += stride) {
        if (x * x + y * y + z * z > radius * radius) {
          continue;
        }
        if (volume.at(at + int3(x, y, z)) == WALL) {
          sum -= float3(x, y, z);
          walls++;
        }
      }
    }
  }
  if (walls == 0 || math::length(sum) < 1e-6f) {
    return float3(0.0f);
  }
  return math::normalize(sum);
}

struct Placed {
  int source;
  float3x3 linear;
  int3 at;
  PieceCells cells;
};

static void put(Volume &volume, const PieceCells &piece, const int3 &at, const int value)
{
  for (const int3 &c : piece.cells) {
    volume.owner[volume.index(at + c)] = value;
  }
}

/** The lowest cell of each column that pieces may drop to: above the floor of the volume and
 * above every piece in the column. */
static void rebuild_floor(Volume &volume)
{
  threading::parallel_for(IndexRange(volume.size.y), 16, [&](const IndexRange range) {
    for (const int y : range) {
      for (int x = 0; x < volume.size.x; x++) {
        int floor = volume.size.z;
        for (int z = 0; z < volume.size.z; z++) {
          if (volume.owner[volume.index(int3(x, y, z))] == FREE) {
            floor = z;
            break;
          }
        }
        for (int z = volume.size.z - 1; z >= floor; z--) {
          if (volume.owner[volume.index(int3(x, y, z))] >= 0) {
            floor = z + 1;
            break;
          }
        }
        volume.floor[x + volume.size.x * y] = floor;
      }
    }
  });
}

/** How good a place is for Shake: lower first for Settle, then touching more. */
static float shake_score(const Volume &volume,
                         const PieceCells &piece,
                         const int3 &at,
                         const bool settle)
{
  const float touch = contact(volume, piece, at);
  return settle ? -float(at.z + piece.lo.z) + touch * 0.9f : touch;
}

/** Moves a piece one step at a time, down and towards its neighbours, while that is better.
 * Steps start large and get smaller. */
static Found slide(const Volume &volume, const PieceCells &piece, const int3 start, const bool settle)
{
  Found best{true, start, shake_score(volume, piece, start, settle)};
  const int3 size = piece.hi - piece.lo + int3(1);
  for (int step = std::max(1, std::min({size.x, size.y, size.z}) / 4); step >= 1; step /= 2) {
    for (int moves = 0; moves < 64; moves++) {
      Found next = best;
      for (int dz = -1; dz <= 1; dz++) {
        for (int dy = -1; dy <= 1; dy++) {
          for (int dx = -1; dx <= 1; dx++) {
            if ((dx == 0 && dy == 0 && dz == 0) || (settle && dz > 0)) {
              continue;
            }
            const int3 at = best.at + int3(dx, dy, dz) * step;
            if (!fits(volume, piece, at)) {
              continue;
            }
            const float score = shake_score(volume, piece, at, settle);
            if (score > next.score + 1e-4f) {
              next = {true, at, score};
            }
          }
        }
      }
      if (next.at == best.at) {
        break;
      }
      best = next;
    }
  }
  return best;
}


/** \} */

/* -------------------------------------------------------------------- */
/** \name Fill Gaps
 * \{ */

/** For every cell, the nearest cell of a piece (jump flooding), or -1. */
static Array<int> nearest_piece_cells(const Volume &volume)
{
  const int64_t total = volume.owner.size();
  Array<int> seed(total, -1);
  for (const int64_t i : IndexRange(total)) {
    if (volume.owner[i] >= 0) {
      seed[i] = int(i);
    }
  }
  auto position = [&](const int i) {
    const int x = i % volume.size.x;
    const int y = (i / volume.size.x) % volume.size.y;
    const int z = i / (volume.size.x * volume.size.y);
    return int3(x, y, z);
  };
  int step = 1;
  while (step * 2 < std::max({volume.size.x, volume.size.y, volume.size.z})) {
    step *= 2;
  }
  Array<int> next(total);
  for (; step >= 1; step /= 2) {
    threading::parallel_for(IndexRange(total), 4096, [&](const IndexRange range) {
      for (const int64_t i : range) {
        const int3 p = position(int(i));
        int best = seed[i];
        int64_t best_d = best >= 0 ? math::distance_squared(position(best), p) : INT64_MAX;
        for (int dz = -1; dz <= 1; dz++) {
          for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
              const int3 n = p + int3(dx, dy, dz) * step;
              if (!volume.contains(n)) {
                continue;
              }
              const int s = seed[volume.index(n)];
              if (s < 0) {
                continue;
              }
              const int64_t d = math::distance_squared(position(s), p);
              if (d < best_d) {
                best_d = d;
                best = s;
              }
            }
          }
        }
        next[i] = best;
      }
    });
    std::swap(seed, next);
  }
  return seed;
}

/** Moves the points of a placed piece out along their normals into the cells closest to it. */
static void swell(Mesh &mesh,
                  const Volume &volume,
                  const Span<int> nearest,
                  const int piece,
                  const float gap,
                  const float amount,
                  const bool blocky)
{
  MutableSpan<float3> positions = mesh.vert_positions_for_write();
  const Span<float3> normals = mesh.vert_normals();
  Array<float> distance(positions.size(), 0.0f);
  const int max_steps = 2 * std::max({volume.size.x, volume.size.y, volume.size.z});
  threading::parallel_for(positions.index_range(), 512, [&](const IndexRange range) {
    for (const int v : range) {
      const float3 start = (positions[v] - volume.origin) / volume.cell;
      const float3 dir = normals[v];
      float reached = 0.0f;
      for (int step = 1; step <= max_steps; step++) {
        const float t = float(step) * 0.5f;
        const float3 q = start + dir * t;
        const int3 c(int(std::floor(q.x)), int(std::floor(q.y)), int(std::floor(q.z)));
        if (!volume.contains(c)) {
          break;
        }
        const int64_t i = volume.index(c);
        if (volume.owner[i] == WALL) {
          break;
        }
        const int s = nearest[i];
        if (s >= 0 && volume.owner[s] != piece) {
          break;
        }
        reached = t;
      }
      distance[v] = std::max(reached * volume.cell - gap * 0.5f - volume.cell * 0.25f, 0.0f);
    }
  });
  if (!blocky) {
    /* Even out the swelling along the surface, then keep each point short of where it may go. */
    const Span<int2> edges = mesh.edges();
    Array<float> smooth = distance;
    Array<float> sum(positions.size());
    Array<int> count(positions.size());
    for (int iteration = 0; iteration < 12; iteration++) {
      sum.fill(0.0f);
      count.fill(0);
      for (const int2 &e : edges) {
        sum[e[0]] += smooth[e[1]];
        sum[e[1]] += smooth[e[0]];
        count[e[0]]++;
        count[e[1]]++;
      }
      for (const int v : positions.index_range()) {
        if (count[v] > 0) {
          smooth[v] = 0.5f * (smooth[v] + sum[v] / float(count[v]));
        }
      }
    }
    for (const int v : positions.index_range()) {
      distance[v] = std::min(smooth[v], distance[v]);
    }
  }
  for (const int v : positions.index_range()) {
    positions[v] += normals[v] * distance[v] * amount;
  }
  mesh.tag_positions_changed();
}

/** \} */

static void modify_geometry_set(ModifierData *md,
                                const ModifierEvalContext *ctx,
                                bke::GeometrySet *geometry_set)
{
  const PackModifierData &pmd = *reinterpret_cast<PackModifierData *>(md);
  const Mesh *container = geometry_set->get_mesh();
  if (container == nullptr || container->verts_num == 0) {
    return;
  }
  const std::optional<Bounds<float3>> bounds = container->bounds_min_max();
  if (!bounds) {
    return;
  }

  /* The sources, measured in the space of the object with the modifier. */
  const float3x3 to_self = float3x3(ctx->object->world_to_object());
  Vector<PackSource> sources;
  for (Object *ob : gather_objects(pmd, ctx->object)) {
    bke::GeometrySet set = bke::object_get_evaluated_geometry_set(*ob);
    if (set.has_instances()) {
      set = geometry::realize_instances(set, geometry::RealizeInstancesOptions()).geometry;
    }
    const Mesh *mesh = set.get_mesh();
    if (mesh == nullptr || mesh->verts_num == 0) {
      continue;
    }
    PackSource src;
    src.object = ob;
    src.mesh = mesh;
    src.shape = to_self * float3x3(ob->object_to_world());
    const Bounds<float3> b = *mesh->bounds_min_max();
    src.center = (b.min + b.max) * 0.5f;
    Array<float3> shaped(mesh->verts_num);
    const Span<float3> positions = mesh->vert_positions();
    for (const int i : positions.index_range()) {
      shaped[i] = src.shape * (positions[i] - src.center);
    }
    src.flat_axis = thinnest_axis(shaped);
    if (is_closed(*mesh)) {
      src.tris = triangles(*mesh);
    }
    src.geometry = std::move(set);
    sources.append(std::move(src));
  }
  if (sources.is_empty()) {
    return;
  }
  /* The geometry sets hold the meshes, point at them again now that they stopped moving. */
  for (PackSource &src : sources) {
    src.mesh = src.geometry.get_mesh();
  }

  /* The volume. */
  Volume volume;
  const float3 extent = bounds->max - bounds->min;
  const float longest = std::max({extent.x, extent.y, extent.z});
  if (longest <= 0.0f) {
    return;
  }
  volume.cell = longest / float(std::clamp(pmd.detail, 8, 512));
  volume.origin = bounds->min;
  volume.size = int3(std::max(1, int(std::ceil(extent.x / volume.cell))),
                     std::max(1, int(std::ceil(extent.y / volume.cell))),
                     std::max(1, int(std::ceil(extent.z / volume.cell))));
  const int64_t total = int64_t(volume.size.x) * volume.size.y * volume.size.z;
  volume.owner = Array<int>(total, FREE);
  if (is_closed(*container)) {
    Array<float3> points(container->verts_num);
    const Span<float3> positions = container->vert_positions();
    for (const int i : positions.index_range()) {
      points[i] = (positions[i] - volume.origin) / volume.cell;
    }
    Array<bool> inside(total, false);
    fill_inside(points, triangles(*container), volume.size, inside);
    for (const int64_t i : IndexRange(total)) {
      volume.owner[i] = inside[i] ? FREE : WALL;
    }
  }
  else {
    BKE_modifier_set_warning(ctx->object, md, "The mesh is not closed, its bounds are filled");
  }
  volume.floor = Array<int>(volume.size.x * volume.size.y, volume.size.z);
  for (int y = 0; y < volume.size.y; y++) {
    for (int x = 0; x < volume.size.x; x++) {
      for (int z = 0; z < volume.size.z; z++) {
        if (volume.owner[volume.index(int3(x, y, z))] == FREE) {
          volume.floor[x + volume.size.x * y] = z;
          break;
        }
      }
    }
  }
  Vector<int64_t> free_cells;
  for (const int64_t i : IndexRange(total)) {
    if (volume.owner[i] == FREE) {
      free_cells.append(i);
    }
  }

  const bool settle = pmd.mode == MOD_PACK_SETTLE;
  const int dilate = int(std::round(pmd.gap / volume.cell * 0.5f));
  const int wanted = pmd.count > 0 ? pmd.count : INT_MAX;
  const int candidates_num = pmd.rotation == MOD_PACK_ROTATION_NONE ? 1 : 8;
  RandomNumberGenerator rng(uint32_t(pmd.seed) * 7919u + 13u);
  Vector<Placed> placed;

  auto place_more = [&](const int give_up) {
  int failures = 0;
  while (placed.size() < wanted && placed.size() < 20000 && failures < give_up &&
         !free_cells.is_empty())
  {
    const int source = rng.get_int32(int(sources.size()));
    const PackSource &src = sources[source];
    const float scale = std::max(1.0f - std::clamp(pmd.random_scale, 0.0f, 1.0f) * rng.get_float(),
                                 0.05f);
    Array<float3x3> turns(candidates_num);
    for (const int i : turns.index_range()) {
      switch (pmd.rotation) {
        case MOD_PACK_ROTATION_NONE:
          turns[i] = float3x3::identity();
          break;
        case MOD_PACK_ROTATION_UPRIGHT:
          turns[i] = rotation_axis_angle(float3(0, 0, 1), rng.get_float() * 2.0f * float(M_PI));
          break;
        default:
          turns[i] = random_rotation(rng);
          break;
      }
    }
    /* Places to try, for Fill: random empty cells. */
    Vector<int3> anchors;
    if (!settle) {
      for (int tries = 0; tries < 1200 && anchors.size() < 160 && !free_cells.is_empty(); tries++) {
        const int pick = rng.get_int32(int(free_cells.size()));
        const int64_t i = free_cells[pick];
        if (volume.owner[i] != FREE) {
          free_cells.remove_and_reorder(pick);
          continue;
        }
        anchors.append(int3(int(i % volume.size.x),
                            int((i / volume.size.x) % volume.size.y),
                            int(i / (int64_t(volume.size.x) * volume.size.y))));
      }
      if (anchors.is_empty()) {
        break;
      }
    }
    const uint32_t order_seed = noise::hash(uint32_t(pmd.seed), uint32_t(placed.size()));

    Array<PieceCells> pieces(candidates_num);
    Array<Found> found(candidates_num);
    threading::parallel_for(IndexRange(candidates_num), 1, [&](const IndexRange range) {
      for (const int i : range) {
        pieces[i] = piece_cells(src, turns[i] * src.shape * scale, volume.cell, dilate);
        if (!pieces[i].valid) {
          continue;
        }
        found[i] = settle ? search_settle(volume, pieces[i], order_seed, int2(INT_MIN), int2(INT_MAX)) :
                            search_fill(volume, pieces[i], anchors);
      }
    });
    int best = -1;
    for (const int i : found.index_range()) {
      if (found[i].valid && (best < 0 || found[i].score > found[best].score)) {
        best = i;
      }
    }
    if (best < 0) {
      failures++;
      continue;
    }
    PieceCells chosen = std::move(pieces[best]);
    Found where = found[best];

    if (pmd.rotation == MOD_PACK_ROTATION_FOLLOW_WALLS) {
      /* Near a wall, turn the flattest side to it and look for a place close by. */
      const int3 size = chosen.hi - chosen.lo;
      const int radius = std::clamp(std::max({size.x, size.y, size.z}) / 2 + 2, 2, 24);
      const float3 away = away_from_walls(volume, where.at, radius);
      if (math::length_squared(away) > 0.0f) {
        const float3 flat = math::normalize(src.shape * src.flat_axis);
        const float3x3 align = rotation_between(flat, away);
        Array<PieceCells> aligned(4);
        Array<Found> aligned_found(4);
        const float spin0 = rng.get_float() * 2.0f * float(M_PI);
        threading::parallel_for(IndexRange(4), 1, [&](const IndexRange range) {
          for (const int i : range) {
            const float3x3 turn = rotation_axis_angle(away, spin0 + float(i) * float(M_PI) * 0.5f) *
                                  align;
            aligned[i] = piece_cells(src, turn * src.shape * scale, volume.cell, dilate);
            if (!aligned[i].valid) {
              continue;
            }
            if (settle) {
              aligned_found[i] = search_settle(volume,
                                               aligned[i],
                                               order_seed,
                                               int2(where.at.x, where.at.y) - int2(3),
                                               int2(where.at.x, where.at.y) + int2(3));
            }
            else {
              Vector<int3> near;
              for (int z = -2; z <= 2; z++) {
                for (int y = -2; y <= 2; y++) {
                  for (int x = -2; x <= 2; x++) {
                    near.append(where.at + int3(x, y, z));
                  }
                }
              }
              aligned_found[i] = search_fill(volume, aligned[i], near);
            }
          }
        });
        int best_aligned = -1;
        for (const int i : aligned_found.index_range()) {
          if (aligned_found[i].valid &&
              (best_aligned < 0 || aligned_found[i].score > aligned_found[best_aligned].score))
          {
            best_aligned = i;
          }
        }
        /* Settle keeps the aligned piece unless it ends up much higher. */
        if (best_aligned >= 0 &&
            (!settle || aligned_found[best_aligned].at.z <= where.at.z + 2))
        {
          chosen = std::move(aligned[best_aligned]);
          where = aligned_found[best_aligned];
        }
      }
    }

    const int id = int(placed.size());
    put(volume, chosen, where.at, id);
    for (int y = chosen.lo.y; y <= chosen.hi.y; y++) {
      for (int x = chosen.lo.x; x <= chosen.hi.x; x++) {
        const int top = chosen.top[chosen.column(x, y)];
        if (top == INT_MIN) {
          continue;
        }
        int &floor = volume.floor[(where.at.x + x) + volume.size.x * (where.at.y + y)];
        floor = std::max(floor, where.at.z + top + 1);
      }
    }
    placed.append({source, chosen.linear, where.at, std::move(chosen)});
    failures = 0;
  }
  };
  place_more(40);

  /* Shake: every piece moves down and towards its neighbours while it can, and tries a small
   * turn. Then more pieces go into the space that frees up. */
  for (int pass = 0; pass < pmd.shake && !placed.is_empty(); pass++) {
    Array<int> order(placed.size());
    for (const int i : order.index_range()) {
      order[i] = i;
    }
    if (settle) {
      std::sort(order.begin(), order.end(), [&](const int a, const int b) {
        return placed[a].at.z + placed[a].cells.lo.z < placed[b].at.z + placed[b].cells.lo.z;
      });
    }
    else {
      for (int i = order.size() - 1; i > 0; i--) {
        std::swap(order[i], order[rng.get_int32(i + 1)]);
      }
    }
    for (const int i : order) {
      Placed &p = placed[i];
      put(volume, p.cells, p.at, FREE);
      Found best = slide(volume, p.cells, p.at, settle);
      if (pmd.rotation != MOD_PACK_ROTATION_NONE) {
        /* A small turn, kept when it lets the piece go somewhere better. */
        const float angle = (rng.get_float() - 0.5f) * 0.5f;
        const float3 axis = pmd.rotation == MOD_PACK_ROTATION_UPRIGHT ?
                                float3(0, 0, 1) :
                                math::normalize(float3(rng.get_float() - 0.5f,
                                                       rng.get_float() - 0.5f,
                                                       rng.get_float() - 0.5f) +
                                                float3(1e-4f));
        const float3x3 linear = rotation_axis_angle(axis, angle) * p.linear;
        PieceCells turned = piece_cells(sources[p.source], linear, volume.cell, dilate);
        if (turned.valid) {
          for (int lift = 0; lift < 3; lift++) {
            const int3 at = best.at + int3(0, 0, lift);
            if (fits(volume, turned, at)) {
              const Found moved = slide(volume, turned, at, settle);
              if (moved.score > best.score + 1e-3f) {
                p.cells = std::move(turned);
                p.linear = linear;
                best = moved;
              }
              break;
            }
          }
        }
      }
      p.at = best.at;
      put(volume, p.cells, p.at, i);
    }
    rebuild_floor(volume);
    free_cells.clear();
    for (const int64_t c : IndexRange(total)) {
      if (volume.owner[c] == FREE) {
        free_cells.append(c);
      }
    }
    place_more(20);
  }

  /* The pieces. */
  const bool as_instances = pmd.output == MOD_PACK_OUTPUT_INSTANCES;
  const bool fill_gaps = !as_instances && (pmd.flag & MOD_PACK_FILL_GAPS) &&
                         pmd.fill_amount > 0.0f && !placed.is_empty();
  std::unique_ptr<bke::Instances> instances = std::make_unique<bke::Instances>(placed.size());
  MutableSpan<int> handles = instances->reference_handles_for_write();
  MutableSpan<float4x4> transforms = instances->transforms_for_write();
  auto transform_of = [&](const Placed &p) {
    return math::from_location<float4x4>(volume.origin + float3(p.at) * volume.cell) *
           float4x4(p.linear) * math::from_location<float4x4>(-sources[p.source].center);
  };
  if (fill_gaps) {
    const Array<int> nearest = nearest_piece_cells(volume);
    Array<bke::GeometrySet> swollen(placed.size());
    threading::parallel_for(placed.index_range(), 1, [&](const IndexRange range) {
      for (const int i : range) {
        const Placed &p = placed[i];
        Mesh *mesh = BKE_mesh_copy_for_eval(*sources[p.source].mesh);
        const float4x4 transform = transform_of(p);
        for (float3 &pos : mesh->vert_positions_for_write()) {
          pos = math::transform_point(transform, pos);
        }
        mesh->tag_positions_changed();
        swell(*mesh,
              volume,
              nearest,
              i,
              pmd.gap,
              std::clamp(pmd.fill_amount, 0.0f, 1.0f),
              pmd.shape == MOD_PACK_SHAPE_BLOCKY);
        swollen[i] = bke::GeometrySet::from_mesh(mesh);
      }
    });
    for (const int i : placed.index_range()) {
      handles[i] = instances->add_reference(bke::InstanceReference(std::move(swollen[i])));
      transforms[i] = float4x4::identity();
    }
  }
  else {
    Array<int> handle_of(sources.size(), -1);
    for (const int i : placed.index_range()) {
      const Placed &p = placed[i];
      if (handle_of[p.source] < 0) {
        handle_of[p.source] = instances->add_reference(
            bke::InstanceReference(*sources[p.source].object));
      }
      handles[i] = handle_of[p.source];
      transforms[i] = transform_of(p);
    }
  }

  bke::GeometrySet result;
  if (pmd.flag & MOD_PACK_KEEP_CONTAINER) {
    result.replace_mesh(BKE_mesh_copy_for_eval(*container));
  }
  result.replace_instances(instances.release());
  if (!as_instances) {
    result = geometry::realize_instances(result, geometry::RealizeInstancesOptions()).geometry;
  }
  *geometry_set = std::move(result);
}

static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);
  const std::string name = RNA_string_get(ptr, "name");

  layout.use_property_split_set(true);
  layout.prop(ptr, "object", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "collection", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  PropertyRNA *list = RNA_struct_find_property(ptr, "objects");
  const int count = RNA_property_collection_length(ptr, list);
  for (int i = 0; i < count; i++) {
    PointerRNA item;
    RNA_property_collection_lookup_int(ptr, list, i, &item);
    ui::Layout &row = layout.row(true);
    row.prop(&item, "object", UI_ITEM_NONE, i == 0 ? IFACE_("More") : "", ICON_NONE);
    PointerRNA remove = row.op("OBJECT_OT_pack_object_remove", "", ICON_X);
    RNA_string_set(&remove, "modifier", name.c_str());
    RNA_int_set(&remove, "index", i);
  }
  ui::Layout &add = layout.row(true);
  add.use_property_split_set(false);
  PointerRNA op = add.op("OBJECT_OT_pack_object_add", IFACE_("Add Object"), ICON_ADD);
  RNA_string_set(&op, "modifier", name.c_str());

  layout.prop(ptr, "output", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
  layout.prop(ptr, "mode", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
  layout.prop(ptr, "rotation", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  ui::Layout &col = layout.column(true);
  col.prop(ptr, "count", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  col.prop(ptr, "gap", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  col.prop(ptr, "detail", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  col.prop(ptr, "shake", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  ui::Layout &random = layout.column(true);
  random.prop(ptr, "random_scale", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  random.prop(ptr, "seed", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "use_keep_container", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  modifier_error_message_draw(layout, ptr);
}

static void fill_gaps_header_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);
  layout.active_set(RNA_enum_get(ptr, "output") == MOD_PACK_OUTPUT_GEOMETRY);
  layout.prop(ptr, "use_fill_gaps", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void fill_gaps_panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);
  layout.use_property_split_set(true);
  layout.active_set(RNA_boolean_get(ptr, "use_fill_gaps") &&
                    RNA_enum_get(ptr, "output") == MOD_PACK_OUTPUT_GEOMETRY);
  layout.prop(ptr, "fill_amount", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "shape", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
}

static void panel_register(ARegionType *region_type)
{
  PanelType *panel_type = modifier_panel_register(region_type, eModifierType_Pack, panel_draw);
  modifier_subpanel_register(
      region_type, "fill_gaps", "", fill_gaps_header_draw, fill_gaps_panel_draw, panel_type);
}

static void blend_write(BlendWriter *writer, const ID * /*id_owner*/, const ModifierData *md)
{
  const PackModifierData &pmd = *reinterpret_cast<const PackModifierData *>(md);
  writer->write_struct(&pmd);
  writer->write_struct_list(&pmd.objects);
}

static void blend_read(BlendDataReader *reader, ModifierData *md)
{
  PackModifierData &pmd = *reinterpret_cast<PackModifierData *>(md);
  BLO_read_struct_list(reader, PackModifierObject, &pmd.objects);
}

ModifierTypeInfo modifierType_Pack = {
    /*idname*/ "Pack",
    /*name*/ N_("Pack"),
    /*struct_name*/ "PackModifierData",
    /*struct_size*/ sizeof(PackModifierData),
    /*srna*/ &RNA_PackModifier,
    /*type*/ ModifierTypeType::Constructive,
    /*flags*/ eModifierTypeFlag_AcceptsMesh,
    /*icon*/ ICON_MOD_PACK,
    /*copy_data*/ copy_data,
    /*deform_verts*/ nullptr,
    /*deform_matrices*/ nullptr,
    /*deform_verts_EM*/ nullptr,
    /*deform_matrices_EM*/ nullptr,
    /*modify_mesh*/ nullptr,
    /*modify_geometry_set*/ modify_geometry_set,
    /*init_data*/ init_data,
    /*required_data_mask*/ nullptr,
    /*free_data*/ free_data,
    /*is_disabled*/ is_disabled,
    /*update_depsgraph*/ update_depsgraph,
    /*depends_on_time*/ nullptr,
    /*depends_on_normals*/ nullptr,
    /*foreach_ID_link*/ foreach_ID_link,
    /*foreach_tex_link*/ nullptr,
    /*free_runtime_data*/ nullptr,
    /*panel_register*/ panel_register,
    /*blend_write*/ blend_write,
    /*blend_read*/ blend_read,
    /*foreach_cache*/ nullptr,
    /*foreach_working_space_color*/ nullptr,
};

}  // namespace blender
