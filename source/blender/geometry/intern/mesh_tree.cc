/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup geo
 *
 * A tree is grown level by level: every branch is a line of evenly spaced points with a radius,
 * and the next level grows along it. Forces bend the branches while they grow and crown shapes
 * cut or shorten them after. The mesh is made last, at the detail that fits the triangle budget:
 * fewer sides and segments, thin twigs as flat cards and fewer but bigger leaves.
 */

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <memory>
#include <optional>

#include "BLI_array.hh"
#include "BLI_bounds.hh"
#include "BLI_bounds_types.hh"
#include "BLI_color.hh"
#include "BLI_kdopbvh.hh"
#include "BLI_map.hh"
#include "BLI_math_base.hh"
#include "BLI_math_constants.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_set.hh"
#include "BLI_memory_utils.hh"
#include "BLI_noise.hh"
#include "BLI_rand.hh"
#include "BLI_task.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_mesh_types.h"

#include "BKE_attribute.hh"
#include "BKE_bvhutils.hh"
#include "BKE_mesh.hh"

#include "GEO_mesh_tree.hh"

namespace blender::geometry::tree {

/** Keeps a graph with huge counts from filling the memory. */
static constexpr int max_branches = 250000;
static constexpr int max_leaves = 1000000;
static constexpr int max_fronds = 250000;
/** Deepest chain of levels, links can't make a loop longer than this. */
static constexpr int max_depth = 16;

static float sample_curve(const Span<float> values, const float x)
{
  if (values.is_empty()) {
    return 1.0f;
  }
  if (values.size() == 1) {
    return values[0];
  }
  const float f = math::clamp(x, 0.0f, 1.0f) * float(values.size() - 1);
  const int i = std::min(int(f), int(values.size()) - 2);
  return math::interpolate(values[i], values[i + 1], f - float(i));
}

struct Branch {
  /** What the nodes linked into its level's numbers read for it, at its base. */
  FieldContext context;
  /** Evenly spaced along the branch, from base to tip. */
  Vector<float3> points;
  Vector<float> radii;
  float length = 0.0f;
  float base_radius = 0.0f;
  /** Sides and base radius of the trunk this grows from, smaller branches get fewer sides. */
  int sides_ref = 8;
  float radius_ref = 1.0f;
  int material = 0;
  /** Cut by a crown shape: ends flat instead of in a point. */
  bool cut = false;
  /** False for spines. */
  bool mesh = true;
  /** The branch this grows from and the place on it, -1 for trunks. */
  int parent = -1;
  float parent_t = 0.0f;
  /** How far the base spreads out over the parent, 0 to 1. */
  float weld = 0.0f;
  /** Length of the weld as a share of the parent's radius, and its spread up and down. */
  float weld_length = 1.0f;
  float weld_upper = 1.0f;
  float weld_lower = 1.0f;
  int weld_rings = 6;
  int mesh_sides = 0;
  float mesh_detail = 1.0f;
  int u_tiles = 1;
  float v_tiles = 1.0f;
  /** The part of the whole stem this piece is, a stem that forks is made of several pieces. */
  float t0 = 0.0f;
  float t1 = 1.0f;
  /** Ends where it forks, the stems that go on start on its last ring. */
  bool open_end = false;
  /** The piece of the stem this one forks off the end of. */
  int fork_parent = -1;
  /** Bark shape, the bump size already in the space of the tree. */
  int flutes = 0;
  float flute_depth = 0.0f;
  float flute_height = 1.0f;
  float bumps = 0.0f;
  float bump_size = 1.0f;
  /** The level it grew from, for its mesh settings, and how many levels down that is. */
  const Level *level = nullptr;
  int depth = 0;
  /** Random from 0 to 1, the same for the whole branch. */
  float random = 0.0f;
  /** The tree's scale. */
  float scale = 1.0f;
};

struct Leaf {
  float3 position;
  /** From the base of the leaf to its tip. */
  float3 dir;
  /** The side the leaf faces. */
  float3 normal;
  float length = 0.0f;
  float width = 0.0f;
  float fold = 0.0f;
  float soft_normals = 0.0f;
  NormalMode normal_mode = NormalMode::Spherical;
  LeafShape shape = LeafShape::Leaf;
  const Mesh *mesh = nullptr;
  int triangles = 4;
  int material = 1;
  /** Leaves under the kept share stay when the budget thins them out. */
  float keep = 0.0f;
  /** Levels down from the trunk, the random of its branch and its own. */
  int depth = 0;
  float branch_random = 0.0f;
  float random = 0.0f;
  /** Mesh leaves keep the size of their mesh, times the tree's scale. */
  bool real_size = false;
  float scale = 1.0f;
  float3 stretch = float3(1.0f);
  bool cross = false;
  bool flip = false;
  float bend = 0.0f;
  float curl = 0.0f;
  float twist = 0.0f;
  float noise = 0.0f;
  float noise_scale = 1.0f;
  int detail = 1;
};

/** Cards strung along a branch, one per plane. */
struct Frond {
  int branch = 0;
  const Level *level = nullptr;
  /** Turn of the plane around the branch, from facing up. */
  float roll = 0.0f;
  uint32_t id = 0;
};

/** A hidden clump of leaves: an ellipsoid, flatter by its squash. */
struct Clump {
  float3 center;
  float radius = 1.0f;
  float squash = 1.0f;
  float random = 0.0f;
  /** Shown as a blob in the mesh, with this material. */
  bool show = false;
  int material = 0;
  int depth = 0;
  float branch_random = 0.0f;
};

/** All the clumps of a tree, merged softly into one shape. */
struct ClumpField {
  Vector<Clump> clumps;
  /** How softly touching clumps run into each other, as a share of their radius. */
  float merge = 0.25f;
  float cell = 1.0f;
  Map<uint64_t, Vector<int>> grid;

  int3 cell_of(const float3 &p) const
  {
    return int3(int(std::floor(p.x / cell)), int(std::floor(p.y / cell)), int(std::floor(p.z / cell)));
  }

  static uint64_t key(const int3 &c)
  {
    const auto part = [](const int v) { return uint64_t(uint32_t(v + (1 << 20)) & 0x1fffffu); };
    return part(c.x) | (part(c.y) << 21) | (part(c.z) << 42);
  }

  void build()
  {
    grid.clear();
    float largest = 1e-4f;
    for (const Clump &clump : clumps) {
      largest = std::max(largest, clump.radius * std::max(1.0f, clump.squash));
    }
    cell = largest * 2.5f;
    for (const int i : clumps.index_range()) {
      grid.lookup_or_add_default(key(cell_of(clumps[i].center))).append(i);
    }
  }

  /**
   * The merged shape at a point, in shares of the clump radius: below 0 inside. Also the way out
   * of the shape, the random value of the nearest clumps and how much the point is at their sides.
   */
  bool eval(const float3 &p,
            float &r_value,
            float3 &r_gradient,
            float &r_random,
            float &r_edge) const
  {
    if (clumps.is_empty()) {
      return false;
    }
    struct Near {
      float d;
      float3 grad;
      float random;
      float edge;
    };
    Vector<Near, 32> near;
    const int3 c = cell_of(p);
    for (int dz = -1; dz <= 1; dz++) {
      for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
          const Vector<int> *indices = grid.lookup_ptr(key(c + int3(dx, dy, dz)));
          if (indices == nullptr) {
            continue;
          }
          for (const int i : *indices) {
            const Clump &clump = clumps[i];
            const float r = std::max(clump.radius, 1e-5f);
            const float squash = std::max(clump.squash, 1e-3f);
            float3 q = (p - clump.center) / r;
            q.z /= squash;
            const float len = math::length(q);
            Near n;
            n.d = len - 1.0f;
            n.grad = len > 1e-6f ? math::normalize(float3(q.x, q.y, q.z / squash)) :
                                   float3(0.0f, 0.0f, 1.0f);
            n.random = clump.random;
            n.edge = len > 1e-6f ? std::min(std::sqrt(q.x * q.x + q.y * q.y) / len, 1.0f) : 0.0f;
            near.append(n);
          }
        }
      }
    }
    if (near.is_empty()) {
      return false;
    }
    float d_min = FLT_MAX;
    for (const Near &n : near) {
      d_min = std::min(d_min, n.d);
    }
    const float k = std::max(merge, 1e-3f);
    double sum = 0.0;
    float3 grad(0.0f);
    float random = 0.0f;
    float edge = 0.0f;
    for (const Near &n : near) {
      const float w = float(std::exp(-double(n.d - d_min) / double(k)));
      sum += w;
      grad += n.grad * w;
      random += n.random * w;
      edge += n.edge * w;
    }
    r_value = d_min - k * float(std::log(sum));
    r_gradient = math::length_squared(grad) > 1e-12f ? math::normalize(grad) :
                                                       float3(0.0f, 0.0f, 1.0f);
    r_random = random / float(sum);
    r_edge = edge / float(sum);
    return true;
  }
};

struct Tree {
  Vector<Branch> branches;
  Vector<Leaf> leaves;
  Vector<Frond> fronds;
  ClumpField clumps;
  /** Facing of the plane the tree is kept in by a Plane force, zero when there is none: its
   * leaves and fronds face that way too. */
  float3 plane = float3(0.0f);
};

struct Sample {
  float3 position;
  float3 tangent;
  float radius;
};

static Sample sample(const Branch &branch, const float t)
{
  const int segments = branch.points.size() - 1;
  const float f = math::clamp(t, 0.0f, 1.0f) * float(segments);
  const int i = std::min(int(f), segments - 1);
  const float w = f - float(i);
  Sample s;
  s.position = math::interpolate(branch.points[i], branch.points[i + 1], w);
  s.tangent = math::normalize(branch.points[i + 1] - branch.points[i]);
  s.radius = math::interpolate(branch.radii[i], branch.radii[i + 1], w);
  return s;
}

static float3 any_perpendicular(const float3 &dir)
{
  const float3 ref = std::abs(dir.z) < 0.95f ? float3(0.0f, 0.0f, 1.0f) :
                                               float3(1.0f, 0.0f, 0.0f);
  return math::normalize(math::cross(dir, ref));
}

/** A curve by place on the parent, 1 when there is none. */
static float profile_at(const Span<float> values, const float x)
{
  if (values.is_empty()) {
    return 1.0f;
  }
  return std::max(sample_curve(values, x), 0.0f);
}

static float length_profile(const Level &level, const float x)
{
  if (level.length_profile.is_empty()) {
    return 1.0f;
  }
  return std::max(sample_curve(level.length_profile, x), 0.0f);
}

/** Pushes are ready for the growth to use, avoided meshes get a tree to find their surface. */
struct PreparedForce {
  const Force *force = nullptr;
  bke::BVHTreeFromMesh bvh;
  float4x4 to_mesh = float4x4::identity();
  float4x4 from_mesh = float4x4::identity();
  /** Turns normals of the mesh into the space of the tree. */
  float3x3 normal_to_tree = float3x3::identity();
  /** Highest point of the mesh, rays down onto it start above this. */
  float top = 0.0f;
};

static void prepare_surface(PreparedForce &prepared, const Mesh &mesh, const float4x4 &transform)
{
  prepared.bvh = mesh.bvh_corner_tris();
  prepared.from_mesh = transform;
  prepared.to_mesh = math::invert(transform);
  prepared.normal_to_tree = math::transpose(float3x3(prepared.to_mesh));
  prepared.top = -FLT_MAX;
  if (const std::optional<Bounds<float3>> bounds = mesh.bounds_min_max()) {
    for (const int corner : IndexRange(8)) {
      const float3 p((corner & 1) ? bounds->max.x : bounds->min.x,
                     (corner & 2) ? bounds->max.y : bounds->min.y,
                     (corner & 4) ? bounds->max.z : bounds->min.z);
      prepared.top = std::max(prepared.top, math::transform_point(transform, p).z);
    }
  }
}

/** Closest point of the surface to a point, both in the space of all trees. */
static bool surface_nearest(const PreparedForce &surface,
                            const float3 &position,
                            float3 &r_point,
                            float3 &r_normal)
{
  if (surface.bvh.tree == nullptr) {
    return false;
  }
  BVHTreeNearest nearest;
  nearest.index = -1;
  nearest.dist_sq = FLT_MAX;
  BLI_bvhtree_find_nearest(surface.bvh.tree,
                           math::transform_point(surface.to_mesh, position),
                           &nearest,
                           surface.bvh.nearest_callback,
                           const_cast<bke::BVHTreeFromMesh *>(&surface.bvh));
  if (nearest.index == -1) {
    return false;
  }
  const float3 normal = surface.normal_to_tree * float3(nearest.no);
  if (math::length_squared(normal) < 1e-12f) {
    return false;
  }
  r_point = math::transform_point(surface.from_mesh, float3(nearest.co));
  r_normal = math::normalize(normal);
  return true;
}

/** Where a straight drop from above a point lands on the surface. */
static bool surface_below(const PreparedForce &surface,
                          const float3 &position,
                          float3 &r_point,
                          float3 &r_normal)
{
  if (surface.bvh.tree == nullptr) {
    return false;
  }
  const float3 start(position.x, position.y, std::max(surface.top, position.z) + 1.0f);
  const float3 origin = math::transform_point(surface.to_mesh, start);
  const float3 ray = math::transform_direction(surface.to_mesh, float3(0.0f, 0.0f, -1.0f));
  if (math::length_squared(ray) < 1e-12f) {
    return false;
  }
  BVHTreeRayHit hit;
  hit.index = -1;
  hit.dist = FLT_MAX;
  BLI_bvhtree_ray_cast(surface.bvh.tree,
                       origin,
                       math::normalize(ray),
                       0.0f,
                       &hit,
                       surface.bvh.raycast_callback,
                       const_cast<bke::BVHTreeFromMesh *>(&surface.bvh));
  if (hit.index == -1) {
    return false;
  }
  float3 normal = surface.normal_to_tree * float3(hit.no);
  if (math::length_squared(normal) < 1e-12f) {
    return false;
  }
  normal = math::normalize(normal);
  r_point = math::transform_point(surface.from_mesh, float3(hit.co));
  /* The side the drop came from. */
  r_normal = normal.z < 0.0f ? -normal : normal;
  return true;
}

/** Turns a vector the way that turns \a from onto \a to. */
static float3 rotate_between(const float3 &v, const float3 &from, const float3 &to)
{
  const float3 axis = math::cross(from, to);
  const float s = math::length(axis);
  if (s < 1e-6f) {
    return v;
  }
  const float3 k = axis / s;
  const float angle = std::atan2(s, math::dot(from, to));
  const float c = std::cos(angle);
  return v * c + math::cross(k, v) * std::sin(angle) + k * (math::dot(k, v) * (1.0f - c));
}

/** Points grouped by the cube they are in, to find the ones near a place quickly. */
struct PointGrid {
  float cell = 0.0f;
  Map<uint64_t, Vector<float3>> cells;

  static uint64_t key(const int3 &c)
  {
    return (uint64_t(uint32_t(c.x) & 0x1fffffu) << 42) | (uint64_t(uint32_t(c.y) & 0x1fffffu) << 21) |
           uint64_t(uint32_t(c.z) & 0x1fffffu);
  }
  int3 cell_of(const float3 &p) const
  {
    return int3(int(std::floor(p.x / cell)), int(std::floor(p.y / cell)), int(std::floor(p.z / cell)));
  }
  void add(const float3 &p)
  {
    if (cell > 0.0f) {
      cells.lookup_or_add_default(key(cell_of(p))).append(p);
    }
  }
  template<typename Fn> void near(const float3 &p, const Fn &fn) const
  {
    if (cell <= 0.0f) {
      return;
    }
    const int3 c = cell_of(p);
    for (int x = -1; x <= 1; x++) {
      for (int y = -1; y <= 1; y++) {
        for (int z = -1; z <= 1; z++) {
          if (const Vector<float3> *points = cells.lookup_ptr(key(c + int3(x, y, z)))) {
            for (const float3 &q : *points) {
              fn(q);
            }
          }
        }
      }
    }
  }
};

struct Grower {
  const TreeParams &params;
  uint32_t seed;
  Tree &tree;
  /** Where this tree stands, forces and shapes are placed in the space of all trees. */
  float3 offset;
  /** Per level. */
  Span<Vector<PreparedForce>> forces;
  bool full = false;
  /** Per level, the ground of trunks, no tree when there is none. */
  Span<PreparedForce> grounds;
  /** The ground of the trunk being grown, its roots stay out of it. */
  const PreparedForce *ground = nullptr;
  /** Levels down from the trunk of the level being grown. */
  int depth = 0;
  /** Space pruning: the tips kept so far, per level. */
  Map<int, PointGrid> space;
};

/* -------------------------------------------------------------------- */
/** \name Growth
 *
 * The way the stems grow follows EZ-Tree (MIT license, Copyright (c) 2024 Daniel Greenheck):
 * a stem is a line of sections, each turned a little at random and by the forces, by less where
 * the wood is thick. Children sit in even slots along and around their parent, moved a little at
 * random, with a radius that is a share of the parent's there. On broadleaf trees every stem
 * carries on from the tip of its parent as the next level, so only the last level ends in a
 * point. On conifers the branches get shorter toward the top and every stem runs to a point.
 * \{ */

/** How thick a stem is for bending, a stem this thick at scale 1 turns by the force's strength. */
static constexpr float stiff_radius = 0.25f;

static float3 seed_offset(const int seed)
{
  const uint32_t h = noise::hash(uint32_t(seed));
  return float3(noise::hash_to_float(h, 1u), noise::hash_to_float(h, 2u), noise::hash_to_float(h, 3u)) *
         100.0f;
}

/** Turns \a v around the unit axis \a k by \a angle. */
static float3 rotate_around(const float3 &v, const float3 &k, const float angle)
{
  const float c = std::cos(angle);
  return v * c + math::cross(k, v) * std::sin(angle) + k * (math::dot(k, v) * (1.0f - c));
}

/** Turns \a dir toward \a target by \a angle, never past it. A negative angle turns away. */
static float3 turn_toward(const float3 &dir, const float3 &target, const float angle)
{
  const float3 axis = math::cross(dir, target);
  const float s = math::length(axis);
  if (s < 1e-6f) {
    if (math::dot(dir, target) > 0.0f || angle == 0.0f) {
      return dir;
    }
    /* Straight at it from behind: any way round. */
    return math::normalize(
        rotate_around(dir, any_perpendicular(dir), std::min(std::abs(angle), float(M_PI))));
  }
  const float full = std::atan2(s, math::dot(dir, target));
  const float turn = std::clamp(angle, -full, full);
  return math::normalize(rotate_around(dir, axis / s, turn));
}

/** How far a ray goes before it meets the surface, as a share of  reach, 1 when it doesn't. */
static float open_along(const PreparedForce &surface,
                        const float3 &position,
                        const float3 &dir,
                        const float reach)
{
  const float3 origin = math::transform_point(surface.to_mesh, position);
  const float3 ray = math::transform_direction(surface.to_mesh, dir);
  const float len = math::length(ray);
  if (len < 1e-8f) {
    return 1.0f;
  }
  BVHTreeRayHit hit;
  hit.index = -1;
  hit.dist = reach * len;
  BLI_bvhtree_ray_cast(surface.bvh.tree,
                       origin,
                       ray / len,
                       0.0f,
                       &hit,
                       surface.bvh.raycast_callback,
                       const_cast<bke::BVHTreeFromMesh *>(&surface.bvh));
  if (hit.index == -1) {
    return 1.0f;
  }
  return std::clamp(hit.dist / (reach * len), 0.0f, 1.0f);
}

/**
 * Lean: a cone of rays ahead of the stem, it turns toward the open ones, by more when more of the
 * way ahead is blocked. A stem near a wall starts leaning away long before it gets there.
 */
static float3 lean_to_open(const PreparedForce &surface,
                           const Force &force,
                           const float3 &position,
                           const float3 &dir,
                           const float strength)
{
  if (surface.bvh.tree == nullptr || force.reach <= 0.0f) {
    return dir;
  }
  const int rays = std::clamp(force.rays, 1, 64);
  const float3 x = any_perpendicular(dir);
  const float3 y = math::cross(dir, x);
  const float3 up(0.0f, 0.0f, 1.0f);
  float3 open_sum(0.0f);
  float open_total = 0.0f;
  /* The way straight ahead counts too. */
  for (const int i : IndexRange(rays + 1)) {
    float3 ray = dir;
    if (i > 0) {
      /* Golden angle spiral over a cone of 70 degrees around the stem. */
      const float f = (float(i) - 0.5f) / float(rays);
      const float tilt = 1.2217f * std::sqrt(f);
      const float spin = 2.39996f * float(i);
      ray = math::normalize(dir * std::cos(tilt) +
                            (x * std::cos(spin) + y * std::sin(spin)) * std::sin(tilt));
    }
    const float open = open_along(surface, position, ray, force.reach);
    const float weight = open * open * (1.0f + force.up_bias * std::max(math::dot(ray, up), 0.0f));
    open_sum += ray * weight;
    open_total += open;
  }
  const float blocked = 1.0f - open_total / float(rays + 1);
  if (blocked <= 1e-4f || math::length_squared(open_sum) < 1e-12f) {
    return dir;
  }
  return turn_toward(dir, math::normalize(open_sum), strength * blocked);
}

/** The forces of a level turn the direction of one section, less where the wood is thick. */
static float3 apply_forces(const Grower &g,
                           const Span<PreparedForce> forces,
                           const float3 &position,
                           const float3 &dir,
                           const float radius,
                           const FieldContext &context)
{
  float3 d = dir;
  const float stiffness = std::max(radius / (stiff_radius * std::max(g.params.scale, 1e-6f)),
                                   1e-3f);
  for (const PreparedForce &prepared : forces) {
    const Force &force = *prepared.force;
    const float strength = force.strength.at(context);
    if (strength == 0.0f) {
      continue;
    }
    switch (force.type) {
      case ForceType::Direction:
        if (math::length_squared(force.direction) > 1e-12f) {
          d = turn_toward(d, math::normalize(force.direction), strength / stiffness);
        }
        break;
      case ForceType::Toward:
      case ForceType::Away: {
        const float3 to = force.point - position;
        if (math::length_squared(to) < 1e-10f) {
          break;
        }
        const float sign = force.type == ForceType::Toward ? 1.0f : -1.0f;
        d = turn_toward(d, math::normalize(to), sign * strength / stiffness);
        break;
      }
      case ForceType::Avoid: {
        if (force.lean) {
          d = lean_to_open(prepared, force, position, d, strength / stiffness);
          break;
        }
        float3 surface;
        float3 normal;
        if (!surface_nearest(prepared, position, surface, normal)) {
          break;
        }
        const float3 away = position - surface;
        const float keep = std::max(force.distance.at(context), 1e-4f);
        const bool inside = math::dot(away, normal) < 0.0f;
        const float distance = math::length(away);
        if (!inside && distance >= keep) {
          break;
        }
        /* Close by, thick and thin alike turn away. */
        const float push = inside ? 1.0f : 1.0f - distance / keep;
        d = turn_toward(d, normal, strength * push);
        break;
      }
    }
  }
  return d;
}

/** What a branch takes from its level for its mesh. */
static void level_settings(const Grower &g,
                           const Level &level,
                           Branch &branch,
                           const FieldContext &context)
{
  FieldContext at = context;
  at.along_branch = 0.0f;
  branch.context = at;
  branch.material = level.material;
  branch.mesh = level.make_mesh;
  branch.weld_length = level.weld_length.at(at);
  branch.weld_upper = level.weld_upper.at(at);
  branch.weld_lower = level.weld_lower.at(at);
  branch.weld_rings = level.weld_rings;
  branch.mesh_sides = level.mesh_sides;
  branch.mesh_detail = level.mesh_detail;
  branch.u_tiles = level.u_tiles;
  branch.v_tiles = level.v_tiles;
  branch.flutes = level.flutes;
  branch.flute_depth = level.flute_depth.at(at);
  branch.flute_height = level.flute_height.at(at);
  branch.bumps = level.bumps.at(at);
  branch.bump_size = std::max(level.bump_size.at(at) * g.params.scale, 1e-4f);
  branch.level = &level;
  branch.depth = g.depth;
  branch.scale = g.params.scale;
  branch.random = context.random;
}

/** How a stem starts and how thick it is. */
struct StemStart {
  float3 position;
  float3 dir;
  float length = 1.0f;
  float radius = 0.1f;
  /** Share of the radius it loses up to its tip, 1 runs to a point. */
  float taper = 0.5f;
  int sections = 8;
  /** Stays above the ground plane. */
  bool above_floor = true;
};

/**
 * Lays out the sections of a stem: each one turned a little at random by the gnarl, more where
 * the wood is thin, its frame turned by the twist, and its direction turned by the forces.
 */
static void grow_stem_path(const Grower &g,
                           const int level_index,
                           FieldContext context,
                           const StemStart &start,
                           Branch &branch)
{
  const Level &level = g.params.levels[level_index];
  const Span<PreparedForce> forces = g.forces[level_index];
  RandomNumberGenerator rng(noise::hash(context.id, 0x5ec7u));
  const int sections = std::max(start.sections, 1);
  const float step = start.length / float(sections);
  const float scale = std::max(g.params.scale, 1e-6f);
  float3 p = start.position;
  float3 d = math::normalize(start.dir);
  float3 side = any_perpendicular(d);
  branch.points.reserve(sections + 1);
  branch.radii.reserve(sections + 1);
  for (const int k : IndexRange(sections + 1)) {
    const float t = float(k) / float(sections);
    context.along_branch = t;
    context.position = p + g.offset;
    float radius = start.radius * (1.0f - std::clamp(start.taper, 0.0f, 1.0f) * t);
    radius = std::max(radius, 1e-4f * scale);
    branch.points.append(p);
    branch.radii.append(radius);
    if (k == sections) {
      break;
    }
    /* Gnarl: thin wood wanders more than thick. */
    const float gnarl = level.gnarl.at(context) *
                        std::max(1.0f, 1.0f / std::sqrt(std::max(radius / (stiff_radius * scale),
                                                                 1e-4f)));
    if (gnarl != 0.0f) {
      const float3 binormal = math::cross(d, side);
      d = rotate_around(d, side, gnarl * (rng.get_float() * 2.0f - 1.0f));
      d = rotate_around(math::normalize(d), binormal, gnarl * (rng.get_float() * 2.0f - 1.0f));
      d = math::normalize(d);
    }
    side = rotate_around(side, d, level.stem_twist.at(context));
    d = apply_forces(g, forces, p + g.offset, d, radius, context);
    side -= d * math::dot(side, d);
    side = math::length_squared(side) > 1e-8f ? math::normalize(side) : any_perpendicular(d);
    /* No stem goes under the ground plane, it levels out along it. */
    if (start.above_floor && g.ground == nullptr && start.position.z >= 0.0f &&
        p.z + d.z * step < radius && d.z < 0.0f)
    {
      const float3 level_d(d.x, d.y, 0.0f);
      d = math::length_squared(level_d) > 1e-8f ? math::normalize(level_d) :
                                                  float3(0.0f, 0.0f, 1.0f);
    }
    p += d * step;
  }
  branch.length = start.length;
  branch.base_radius = branch.radii[0];
  branch.t0 = 0.0f;
  branch.t1 = 1.0f;
}

static bool inside_shape(const CrownShape &shape, const float3 &position, const float reach)
{
  const float3 p = math::transform_point(shape.to_shape, position) / std::max(reach, 1e-3f);
  const float r_sq = p.x * p.x + p.y * p.y;
  switch (shape.type) {
    case ShapeType::Sphere:
      return r_sq + p.z * p.z <= 1.0f;
    case ShapeType::Dome:
      return p.z >= -0.05f && r_sq + p.z * p.z <= 1.0f;
    case ShapeType::Cone: {
      if (p.z < -1.0f || p.z > 1.0f) {
        return false;
      }
      const float r = (1.0f - p.z) * 0.5f;
      return r_sq <= r * r;
    }
    case ShapeType::Cylinder:
      return r_sq <= 1.0f && std::abs(p.z) <= 1.0f;
  }
  return true;
}

/** Keeps the first \a fraction of the branch, evenly spaced again. With \a stretch_radii the
 * radius still tapers all the way to the new tip. */
static void trim_branch(Branch &branch, const float fraction, const bool stretch_radii)
{
  const Vector<float3> old_points = branch.points;
  const Vector<float> old_radii = branch.radii;
  const int old_segments = old_points.size() - 1;
  const int segments = std::max(2, int(std::ceil(float(old_segments) * fraction)));
  const auto at = [&](const auto &values, const float t) {
    const float f = math::clamp(t, 0.0f, 1.0f) * float(old_segments);
    const int i = std::min(int(f), old_segments - 1);
    return math::interpolate(values[i], values[i + 1], f - float(i));
  };
  branch.points.resize(segments + 1);
  branch.radii.resize(segments + 1);
  for (const int j : IndexRange(segments + 1)) {
    const float u = float(j) / float(segments);
    branch.points[j] = at(old_points, u * fraction);
    branch.radii[j] = at(old_radii, stretch_radii ? u : u * fraction);
  }
  branch.length *= fraction;
}

/** Shortens the branch to end inside the crowns of its level, false when it starts outside. */
static bool fit_shapes(const Grower &g, const Level &level, Branch &branch)
{
  for (const CrownShape &shape : level.shapes) {
    /* Soft crowns let some branches reach further out than others. */
    const float reach = 1.0f + shape.softness * (noise::hash_to_float(branch.context.id, 0xc20u) -
                                                 0.3f);
    const int segments = branch.points.size() - 1;
    if (!inside_shape(shape, branch.points[0] + g.offset, reach)) {
      return false;
    }
    int k = 1;
    while (k <= segments && inside_shape(shape, branch.points[k] + g.offset, reach)) {
      k++;
    }
    if (k > segments) {
      continue;
    }
    float low = 0.0f;
    float high = 1.0f;
    for ([[maybe_unused]] const int i : IndexRange(12)) {
      const float mid = 0.5f * (low + high);
      const float3 p = math::interpolate(branch.points[k - 1], branch.points[k], mid);
      if (inside_shape(shape, p + g.offset, reach)) {
        low = mid;
      }
      else {
        high = mid;
      }
    }
    const float fraction = (float(k - 1) + low) / float(segments);
    if (fraction < 0.05f) {
      return false;
    }
    trim_branch(branch, fraction, !shape.cut);
    if (shape.cut) {
      branch.cut = true;
    }
  }
  return true;
}

/** True when a prune rule removes something at \a position going \a dir. */
static bool prune_removes(Grower &g,
                          const int level_index,
                          const Prune &prune,
                          const float amount,
                          const float3 &position,
                          const float3 &dir,
                          const float random)
{
  switch (prune.mode) {
    case PruneMode::Random:
      return random < amount;
    case PruneMode::Down:
      return -dir.z > 1.0f - 2.0f * amount;
    case PruneMode::Crowded: {
      const float spacing = std::max(prune.spacing * g.params.scale, 1e-5f);
      const Level &level = g.params.levels[level_index];
      PointGrid &grid = g.space.lookup_or_add_default(level_index * 64 +
                                                      int(&prune - level.prunes.data()));
      if (grid.cell <= 0.0f) {
        grid.cell = spacing;
      }
      const float3 p = position + g.offset;
      bool crowded = false;
      grid.near(p, [&](const float3 &q) {
        crowded |= math::distance_squared(p, q) < spacing * spacing;
      });
      if (crowded && random < amount) {
        return true;
      }
      grid.add(p);
      return false;
    }
  }
  return false;
}

/** Runs the prune rules of a level on a branch: false when it is taken away. */
static bool prune_branch(Grower &g, const int level_index, const Branch &branch)
{
  const Level &level = g.params.levels[level_index];
  for (const Prune &prune : level.prunes) {
    FieldContext context = branch.context;
    context.position = branch.points[0] + g.offset;
    const float amount = std::clamp(prune.amount.at(context), 0.0f, 1.0f);
    const uint32_t h = noise::hash(uint32_t(prune.seed), uint32_t(level.id), branch.context.id);
    const float3 dir = math::normalize(branch.points.last() - branch.points[0]);
    /* Crowded keeps room between the tips, the others look at the whole branch. */
    const float3 at = prune.mode == PruneMode::Crowded ? branch.points.last() : branch.points[0];
    if (prune_removes(g, level_index, prune, amount, at, dir, noise::hash_to_float(h, 1u))) {
      return false;
    }
  }
  return true;
}

/** Adds a grown branch after its crowns and prune rules, false when the tree is full. */
static bool add_branch(Grower &g, const Level &level, Branch &&branch, Vector<int> &made)
{
  if (!level.shapes.is_empty() && !fit_shapes(g, level, branch)) {
    return true;
  }
  const int level_index = int(&level - g.params.levels.data());
  if (!level.prunes.is_empty() && branch.fork_parent < 0 && !prune_branch(g, level_index, branch))
  {
    return true;
  }
  if (g.tree.branches.size() >= max_branches) {
    g.full = true;
    return false;
  }
  made.append(g.tree.branches.size());
  g.tree.branches.append(std::move(branch));
  return true;
}

/** The context of a new branch or leaf: number \a i of its level on \a parent. */
static FieldContext element_context(const Grower &g,
                                    const Level &level,
                                    const int parent,
                                    const int i,
                                    const float along_parent,
                                    const float3 &position)
{
  FieldContext context;
  context.along_parent = along_parent;
  context.id = noise::hash(g.seed, uint32_t(level.id), uint32_t(parent), uint32_t(i));
  context.random = noise::hash_to_float(context.id);
  context.position = position + g.offset;
  context.origin = g.offset;
  context.depth = g.depth;
  context.index = i;
  context.scale = g.params.scale;
  if (parent >= 0 && parent < g.tree.branches.size()) {
    const Branch &p = g.tree.branches[parent];
    const float scale = std::max(g.params.scale, 1e-6f);
    context.parent_length = p.length / scale;
    context.parent_radius = p.base_radius / scale;
  }
  return context;
}

/** The range on the parent that children grow along, used by fronds too. */
static bool piece_range(const Level &level, const Branch &parent, float &r_lo, float &r_hi)
{
  r_lo = std::max(level.start.at(parent.context), parent.t0);
  r_hi = std::min(level.end.at(parent.context), parent.t1);
  return r_hi > r_lo + 1e-6f;
}

static float piece_t(const Branch &parent, const float u)
{
  return math::clamp((u - parent.t0) / std::max(parent.t1 - parent.t0, 1e-6f), 0.0f, 1.0f);
}

/** The indices 0 to \a count - 1 in a random order. */
static Array<int> shuffled(const int count, RandomNumberGenerator &rng)
{
  Array<int> order(count);
  for (const int i : IndexRange(count)) {
    order[i] = i;
  }
  for (int i = count - 1; i > 0; i--) {
    const int j = std::min(int(rng.get_float() * float(i + 1)), i);
    std::swap(order[i], order[j]);
  }
  return order;
}

/** True when a level has a level of branches growing on it, its stems then carry on. */
static bool has_branch_children(const TreeParams &params, const Level &level)
{
  for (const int child : level.children) {
    if (params.levels[child].type == LevelType::Branches) {
      return true;
    }
  }
  return false;
}

/** Share of the radius a stem of this level loses up to its tip. */
static float stem_taper(const Grower &g, const Level &level, const FieldContext &context)
{
  if (g.params.conifer || !has_branch_children(g.params, level)) {
    /* The last level, and every stem of a conifer, runs to a point. */
    return 1.0f;
  }
  return std::clamp(level.taper.at(context), 0.0f, 0.95f);
}

static void grow_trunks(Grower &g, const int level_index, Vector<int> &made)
{
  const Level &level = g.params.levels[level_index];
  const float scale = g.params.scale;
  const int count = std::clamp(level.count, 1, 64);
  RandomNumberGenerator rng(noise::hash(g.seed, uint32_t(level.id), 0x7a11u));
  for (const int c : IndexRange(count)) {
    const float tp = count > 1 ? float(c) / float(count - 1) : 0.0f;
    FieldContext context = element_context(g, level, -1, c, tp, float3(0.0f));
    context.count = count;
    const float radius = std::max(level.radius.at(context), 0.0f) * scale;
    float3 base(0.0f);
    float3 dir(0.0f, 0.0f, 1.0f);
    if (count > 1) {
      /* Several stems from one foot, leaning out a little. */
      const float a = float(c) * 2.39996f + rng.get_float() * 0.5f;
      const float3 out(std::cos(a), std::sin(a), 0.0f);
      base = out * (radius * 1.5f * std::sqrt((float(c) + 0.5f) / float(count)));
      dir = math::normalize(float3(0.0f, 0.0f, 1.0f) + out * (0.15f + 0.15f * rng.get_float()));
    }
    context.position = base + g.offset;
    StemStart start;
    start.position = base;
    start.dir = dir;
    start.length = std::max(level.length.at(context), 0.0f) * scale;
    start.radius = radius;
    start.taper = stem_taper(g, level, context);
    start.sections = std::max(level.segments, 1);
    if (start.length < 1e-4f * scale || start.radius <= 0.0f) {
      continue;
    }
    Branch branch;
    level_settings(g, level, branch, context);
    grow_stem_path(g, level_index, context, start, branch);
    branch.sides_ref = std::max(level.sides, 3);
    branch.radius_ref = branch.base_radius;
    if (!add_branch(g, level, std::move(branch), made)) {
      return;
    }
  }
}

/** Branches on every parent: the one that carries on from the parent's tip on broadleaf trees,
 * then the side branches in even slots along and around it. */
static void grow_branches(Grower &g, const int level_index, const Span<int> parents, Vector<int> &made)
{
  const Level &level = g.params.levels[level_index];
  const float scale = g.params.scale;
  const bool conifer = g.params.conifer;
  const int sections = std::max(level.segments, 1);
  for (const int parent_index : parents) {
    /* A copy, adding branches moves them in memory. */
    const Branch parent = g.tree.branches[parent_index];
    const FieldContext &pc = parent.context;
    RandomNumberGenerator rng(noise::hash(g.seed, uint32_t(level.id), uint32_t(parent_index)));

    /* The stem carries on from the tip of its parent. */
    if (!conifer && !parent.cut && parent.points.size() > 1) {
      const Sample tip = sample(parent, 1.0f);
      FieldContext context = element_context(g, level, parent_index, 0xffff, 1.0f, tip.position);
      StemStart start;
      start.position = tip.position;
      start.dir = tip.tangent;
      start.length = std::max(level.length.at(context), 0.0f) * scale;
      start.radius = parent.radii.last();
      start.taper = stem_taper(g, level, context);
      start.sections = sections;
      if (start.length >= 1e-4f * scale) {
        Branch branch;
        level_settings(g, level, branch, context);
        grow_stem_path(g, level_index, context, start, branch);
        branch.parent = parent_index;
        branch.parent_t = 1.0f;
        branch.fork_parent = parent_index;
        branch.sides_ref = parent.sides_ref;
        branch.radius_ref = parent.radius_ref;
        /* It goes on with the parent's rings, so it keeps the parent's sides. */
        branch.mesh_sides = parent.mesh_sides;
        const int64_t before = made.size();
        if (!add_branch(g, level, std::move(branch), made)) {
          return;
        }
        if (made.size() > before) {
          g.tree.branches[parent_index].open_end = true;
        }
      }
    }

    /* Side branches. */
    const float count_value = level.count_field.fn ? level.count_field.at(pc) : float(level.count);
    const int count = std::clamp(int(std::round(count_value)), 0, 4096);
    if (count == 0) {
      continue;
    }
    const float lo = std::clamp(level.start.at(pc), 0.0f, 1.0f);
    const float jitter = std::clamp(level.jitter.at(pc), 0.0f, 1.0f);
    int group = 1;
    if (level.arrangement == Arrangement::Opposite) {
      group = 2;
    }
    else if (level.arrangement == Arrangement::Whorled) {
      group = std::max(level.per_node, 1);
    }
    const int nodes = (count + group - 1) / group;
    const float step = (1.0f - lo) / float(nodes);
    const Array<int> slots = shuffled(nodes, rng);
    const float turn0 = rng.get_float();
    for (const int n : IndexRange(nodes)) {
      const float along = std::clamp(lo + (float(n) + 0.5f + (rng.get_float() - 0.5f) * jitter) *
                                              step,
                                     0.0f,
                                     1.0f);
      const Sample s = sample(parent, along);
      const float3 n0 = any_perpendicular(s.tangent);
      const float3 b0 = math::cross(s.tangent, n0);
      for (const int j : IndexRange(group)) {
        const int i = n * group + j;
        if (i >= count) {
          break;
        }
        float around;
        if (group == 1) {
          /* Even slots around in a shuffled order, so long ones don't line up on one side. */
          around = 2.0f * float(M_PI) *
                   (turn0 + (float(slots[n]) + (rng.get_float() - 0.5f) * jitter) / float(nodes));
        }
        else {
          /* Each ring turned half a gap from the one below. */
          around = 2.0f * float(M_PI) *
                   (turn0 + (float(j) + 0.5f * float(n % 2)) / float(group) +
                    (rng.get_float() - 0.5f) * jitter * 0.15f);
        }
        FieldContext context = element_context(g, level, parent_index, i, along, s.position);
        context.count = count;
        context.around = around;
        if (level.mask.at(context) < 0.5f) {
          continue;
        }
        const float3 radial = n0 * std::cos(around) + b0 * std::sin(around);
        const float angle = level.angle.at(context);
        StemStart start;
        start.position = s.position;
        start.dir = math::normalize(s.tangent * std::cos(angle) + radial * std::sin(angle));
        start.length = std::max(level.length.at(context), 0.0f) * scale *
                       (conifer ? std::max(1.0f - along, 0.0f) : 1.0f);
        start.radius = std::min(std::max(level.radius.at(context), 0.0f) * s.radius, s.radius);
        start.taper = stem_taper(g, level, context);
        start.sections = sections;
        if (start.length < 1e-4f * scale || start.radius <= 0.0f) {
          continue;
        }
        Branch branch;
        level_settings(g, level, branch, context);
        grow_stem_path(g, level_index, context, start, branch);
        branch.parent = parent_index;
        branch.parent_t = along;
        branch.weld = std::clamp(level.weld.at(context), 0.0f, 1.0f);
        branch.sides_ref = parent.sides_ref;
        branch.radius_ref = parent.radius_ref;
        if (!add_branch(g, level, std::move(branch), made)) {
          return;
        }
      }
    }
  }
}

/** Roots from the foot of every trunk, going out and down into the ground. */
static void grow_roots(Grower &g, const int level_index, const Span<int> parents, Vector<int> &made)
{
  const Level &level = g.params.levels[level_index];
  const float scale = g.params.scale;
  for (const int parent_index : parents) {
    const Branch parent = g.tree.branches[parent_index];
    if (parent.parent >= 0) {
      /* Roots grow at the foot of a trunk only. */
      continue;
    }
    RandomNumberGenerator rng(
        noise::hash(g.seed, uint32_t(level.id), uint32_t(parent_index) + 77777u));
    const int count = std::clamp(int(std::round(level.count_field.fn ?
                                                    level.count_field.at(parent.context) :
                                                    float(level.count))),
                                 0,
                                 256);
    const float turn0 = rng.get_float();
    const float t = math::clamp(parent.radii[0] * 0.5f / std::max(parent.length, 1e-6f),
                                0.0f,
                                0.2f);
    const Sample s = sample(parent, t);
    const float3 n0 = any_perpendicular(s.tangent);
    const float3 b0 = math::cross(s.tangent, n0);
    for (const int i : IndexRange(count)) {
      const float around = 2.0f * float(M_PI) *
                           (turn0 + (float(i) + (rng.get_float() - 0.5f) * 0.6f) / float(count));
      const float tp = count > 1 ? float(i) / float(count - 1) : 0.0f;
      FieldContext context = element_context(g, level, parent_index, i, tp, s.position);
      context.count = count;
      context.around = around;
      if (level.mask.at(context) < 0.5f) {
        continue;
      }
      const float3 radial = n0 * std::cos(around) + b0 * std::sin(around);
      const float depth = std::clamp(level.depth.at(context), 0.0f, 1.0f);
      StemStart start;
      start.position = s.position;
      start.dir = math::normalize(radial * (1.0f - depth) - s.tangent * depth + radial * 1e-3f);
      start.length = std::max(level.length.at(context), 0.0f) * scale;
      start.radius = std::min(std::max(level.radius.at(context), 0.0f) * s.radius, s.radius);
      start.taper = std::clamp(level.taper.at(context), 0.0f, 1.0f);
      start.sections = std::max(level.segments, 1);
      start.above_floor = false;
      if (start.length < 1e-4f * scale || start.radius <= 0.0f) {
        continue;
      }
      Branch branch;
      level_settings(g, level, branch, context);
      grow_stem_path(g, level_index, context, start, branch);
      branch.parent = parent_index;
      branch.parent_t = t;
      branch.weld = std::clamp(level.weld.at(context), 0.0f, 1.0f);
      branch.sides_ref = parent.sides_ref;
      branch.radius_ref = parent.radius_ref;
      if (!add_branch(g, level, std::move(branch), made)) {
        return;
      }
    }
  }
}

static int mesh_triangles(const Mesh &mesh)
{
  int total = 0;
  for (const int face : mesh.faces().index_range()) {
    total += std::max(mesh.faces()[face].size() - 2, int64_t(0));
  }
  return total;
}

/** A mesh from the level's collection, picked by weight. */
static const Mesh *pick_leaf_mesh(const Level &level, const uint32_t id)
{
  float total = 0.0f;
  for (const LeafChoice &choice : level.leaf_choices) {
    total += std::max(choice.weight, 0.0f);
  }
  if (total <= 0.0f) {
    return nullptr;
  }
  float at = noise::hash_to_float(id, uint32_t(level.leaf_seed) + 0x1eafu) * total;
  for (const LeafChoice &choice : level.leaf_choices) {
    at -= std::max(choice.weight, 0.0f);
    if (at <= 0.0f) {
      return choice.mesh;
    }
  }
  return level.leaf_choices.last().mesh;
}

/** Leaves in even slots along and around every parent, and one at each tip that doesn't carry
 * on. */
static void grow_leaves(Grower &g, const int level_index, const Span<int> parents)
{
  const Level &level = g.params.levels[level_index];
  const float scale = g.params.scale;
  const float3 up(0.0f, 0.0f, 1.0f);
  LeafShape level_shape = level.shape;
  const bool use_choices = level_shape == LeafShape::Mesh && !level.leaf_choices.is_empty();
  if (level_shape == LeafShape::Mesh && !use_choices &&
      (level.leaf_mesh == nullptr || level.leaf_mesh->faces_num == 0))
  {
    level_shape = LeafShape::Square;
  }
  const int detail = std::clamp(level.leaf_detail, 1, 16);
  const int card_triangles = level_shape == LeafShape::Leaf ? (detail > 1 ? 4 * detail : 4) :
                                                              (detail > 1 ? 4 * detail : 2);
  const int level_mesh_triangles = (level_shape == LeafShape::Mesh && !use_choices) ?
                                       mesh_triangles(*level.leaf_mesh) :
                                       0;

  const auto add_leaf = [&](const Branch &parent,
                            const FieldContext &context,
                            const float3 &base,
                            const float3 &dir,
                            const float3 &radial,
                            RandomNumberGenerator &rng,
                            const float3 *face = nullptr) {
    if (g.tree.leaves.size() >= max_leaves) {
      g.full = true;
      return;
    }
    /* Facing the sky, turned around the leaf's length. */
    float3 n_up = up - dir * math::dot(dir, up);
    if (math::length_squared(n_up) < 1e-6f) {
      n_up = radial - dir * math::dot(dir, radial);
    }
    n_up = math::length_squared(n_up) > 1e-10f ? math::normalize(n_up) : any_perpendicular(dir);
    const float phi = (rng.get_float() * 2.0f - 1.0f) * float(M_PI);
    const float3 n_rand = n_up * std::cos(phi) + math::cross(dir, n_up) * std::sin(phi);
    float3 normal = math::interpolate(n_rand, n_up, level.face_up.at(context));
    normal = math::length_squared(normal) > 1e-6f ? math::normalize(normal) : n_up;
    if (face != nullptr) {
      /* Leaves on clumps face out of their clump. */
      const float3 out = *face - dir * math::dot(dir, *face);
      if (math::length_squared(out) > 1e-8f) {
        normal = math::normalize(math::interpolate(math::normalize(out), n_up,
                                                   level.face_up.at(context) * 0.5f));
      }
    }

    const float variance = std::clamp(level.size_random.at(context), 0.0f, 1.0f);
    const float size = std::max(level.length.at(context), 0.0f) * scale *
                       std::max(1.0f + variance * (rng.get_float() * 2.0f - 1.0f), 0.05f);
    Leaf leaf;
    leaf.position = base;
    leaf.dir = dir;
    leaf.normal = normal;
    leaf.length = size;
    leaf.width = size * level.width.at(context);
    leaf.shape = level_shape;
    leaf.fold = leaf.shape == LeafShape::Leaf ? level.fold.at(context) : 0.0f;
    leaf.soft_normals = level.soft_normals.at(context);
    leaf.normal_mode = level.normal_mode;
    if (leaf.shape == LeafShape::Mesh) {
      leaf.mesh = use_choices ? pick_leaf_mesh(level, context.id) : level.leaf_mesh;
      if (leaf.mesh == nullptr || leaf.mesh->faces_num == 0) {
        return;
      }
      leaf.triangles = use_choices ? mesh_triangles(*leaf.mesh) : level_mesh_triangles;
    }
    else {
      leaf.triangles = card_triangles;
    }
    leaf.material = level.material;
    leaf.keep = rng.get_float();
    leaf.depth = g.depth;
    leaf.branch_random = parent.random;
    leaf.random = context.random;
    leaf.real_size = level.real_size;
    leaf.scale = scale;
    leaf.stretch = level.leaf_scale;
    leaf.cross = level.cross;
    if (leaf.cross) {
      leaf.triangles *= 2;
    }
    leaf.flip = level.flip && noise::hash_to_float(context.id, 0xf11bu) < 0.5f;
    leaf.bend = level.bend.at(context);
    leaf.curl = level.curl.at(context);
    leaf.twist = level.twist.at(context);
    leaf.noise = std::max(level.leaf_noise.at(context), 0.0f);
    leaf.noise_scale = std::max(level.leaf_noise_scale.at(context), 1e-3f);
    leaf.detail = leaf.shape == LeafShape::Mesh ? 1 : detail;
    /* No leaves under the ground plane, unless the tree stands on a ground object. */
    if (g.ground == nullptr && leaf.position.z < 0.0f) {
      return;
    }
    for (const Prune &prune : level.prunes) {
      const float amount = std::clamp(prune.amount.at(context), 0.0f, 1.0f);
      const float random = noise::hash_to_float(context.id, uint32_t(prune.seed) + 0x9e11u);
      if (prune_removes(g, level_index, prune, amount, leaf.position, leaf.dir, random)) {
        return;
      }
    }
    /* No leaves inside a mesh the level avoids. */
    for (const PreparedForce &prepared : g.forces[level_index]) {
      if (prepared.force->type != ForceType::Avoid) {
        continue;
      }
      float3 surface;
      float3 surface_normal;
      const float3 at = leaf.position + g.offset;
      if (surface_nearest(prepared, at, surface, surface_normal) &&
          math::dot(at - surface, surface_normal) < 0.0f)
      {
        return;
      }
    }
    g.tree.leaves.append(leaf);
  };

  for (const int parent_index : parents) {
    const Branch parent = g.tree.branches[parent_index];
    const FieldContext &pc = parent.context;
    RandomNumberGenerator rng(noise::hash(g.seed,
                                          uint32_t(level.id),
                                          uint32_t(parent_index) + 31337u +
                                              uint32_t(level.leaf_seed) * 7919u));
    const float count_value = level.count_field.fn ? level.count_field.at(pc) : float(level.count);
    const int count = std::clamp(int(std::round(count_value)), 0, 4096);
    const float lo = std::clamp(level.start.at(pc), 0.0f, 1.0f);
    const float step = (1.0f - lo) / float(std::max(count, 1));
    const Array<int> slots = shuffled(count, rng);
    const float turn0 = rng.get_float();
    if (level.use_clumps) {
      /* Hidden clumps along the parent, toward its tip, with the leaves on their surface. */
      const int clump_count = std::clamp(level.clump_count, 1, 64);
      const float squash = std::clamp(level.clump_squash, 0.05f, 4.0f);
      Vector<int, 16> mine;
      Vector<float, 16> mine_along;
      for (const int j : IndexRange(clump_count)) {
        const float along = std::clamp(
            lo + (1.0f - lo) * (float(j) + 0.5f + (rng.get_float() - 0.5f) * 0.5f) /
                     float(clump_count),
            0.0f,
            1.0f);
        Clump clump;
        clump.center = sample(parent, along).position;
        /* Its own number, apart from the leaves, so linked random nodes differ per clump. */
        FieldContext clump_context = element_context(
            g, level, parent_index, 0x10000 + j, along, clump.center);
        clump_context.count = clump_count;
        clump.radius = std::max(level.clump_size.at(clump_context) * scale, 1e-4f);
        clump.squash = squash;
        clump.random = rng.get_float();
        clump.show = level.show_clumps;
        clump.material = level.material;
        clump.depth = g.depth;
        clump.branch_random = parent.random;
        mine.append(g.tree.clumps.clumps.append_and_get_index(clump));
        mine_along.append(along);
      }
      g.tree.clumps.merge = std::clamp(level.clump_merge, 0.0f, 1.0f) * 0.5f;
      for (const int i : IndexRange(count)) {
        const int which = i % clump_count;
        const Clump clump = g.tree.clumps.clumps[mine[which]];
        /* A point evenly over the ellipsoid. */
        const float z = rng.get_float() * 2.0f - 1.0f;
        const float a = rng.get_float() * 2.0f * float(M_PI);
        const float rr = std::sqrt(std::max(1.0f - z * z, 0.0f));
        const float3 d(rr * std::cos(a), rr * std::sin(a), z);
        const float3 surface = clump.center +
                               float3(d.x, d.y, d.z * clump.squash) * clump.radius;
        /* Leaves hidden deep inside the other clumps of this parent are left out. */
        bool hidden = false;
        for (const int other : mine) {
          if (other == mine[which]) {
            continue;
          }
          const Clump &o = g.tree.clumps.clumps[other];
          float3 q = (surface - o.center) / o.radius;
          q.z /= o.squash;
          if (math::length(q) < 0.8f) {
            hidden = true;
            break;
          }
        }
        if (hidden) {
          continue;
        }
        FieldContext context = element_context(
            g, level, parent_index, i, mine_along[which], surface);
        context.count = count;
        if (level.mask.at(context) < 0.5f) {
          continue;
        }
        const float3 out = math::normalize(float3(d.x, d.y, d.z / clump.squash));
        const float3 tangent = rotate_around(
            any_perpendicular(out), out, rng.get_float() * 2.0f * float(M_PI));
        /* Leaves stand a little out of the surface, so the outline looks leafy. */
        const float3 dir = math::normalize(tangent + out * 0.35f);
        add_leaf(parent, context, surface, dir, out, rng, &out);
        if (g.full) {
          return;
        }
      }
      continue;
    }
    for (const int i : IndexRange(count)) {
      const float along = std::clamp(lo + (float(i) + rng.get_float()) * step, 0.0f, 1.0f);
      const Sample s = sample(parent, along);
      const float around = 2.0f * float(M_PI) *
                           (turn0 + (float(slots[i]) + rng.get_float() - 0.5f) / float(count));
      FieldContext context = element_context(g, level, parent_index, i, along, s.position);
      context.count = count;
      context.around = around;
      if (level.mask.at(context) < 0.5f) {
        continue;
      }
      const float3 n0 = any_perpendicular(s.tangent);
      const float3 radial = n0 * std::cos(around) +
                            math::cross(s.tangent, n0) * std::sin(around);
      const float angle = level.angle.at(context);
      const float3 dir = math::normalize(s.tangent * std::cos(angle) + radial * std::sin(angle));
      add_leaf(parent,
               context,
               s.position + radial * (level.leaf_weld ? s.radius : 0.0f),
               dir,
               radial,
               rng);
      if (g.full) {
        return;
      }
    }
    /* One at the tip, unless the stem carries on from there. */
    if (level.tip_leaf && !parent.open_end) {
      const Sample tip = sample(parent, 1.0f);
      FieldContext context = element_context(g, level, parent_index, 0xfffe, 1.0f, tip.position);
      context.count = count;
      if (level.mask.at(context) >= 0.5f) {
        add_leaf(parent, context, tip.position, tip.tangent, any_perpendicular(tip.tangent), rng);
      }
    }
  }
}

/** The closest point to \a p on a line of points, as its distance and share along the line. */
static float closest_on_line(const Span<float3> points, const float3 &p, float &r_t)
{
  float best = FLT_MAX;
  r_t = 0.0f;
  const int segments = points.size() - 1;
  for (const int k : IndexRange(segments)) {
    const float3 a = points[k];
    const float3 ab = points[k + 1] - a;
    const float len_sq = math::length_squared(ab);
    const float w = len_sq > 1e-12f ? std::clamp(math::dot(p - a, ab) / len_sq, 0.0f, 1.0f) :
                                      0.0f;
    const float d = math::distance(p, a + ab * w);
    if (d < best) {
      best = d;
      r_t = (float(k) + w) / float(std::max(segments, 1));
    }
  }
  return best;
}

/** Fronds follow the whole line of each parent, some ribbons turned evenly around it. */
static void grow_fronds(Grower &g, const int level_index, const Span<int> parents)
{
  const Level &level = g.params.levels[level_index];
  const int planes = std::clamp(level.planes, 1, 8);
  for (const int parent_index : parents) {
    const Branch &parent = g.tree.branches[parent_index];
    float lo;
    float hi;
    if (!piece_range(level, parent, lo, hi)) {
      continue;
    }
    for (const int i : IndexRange(planes)) {
      if (g.tree.fronds.size() >= max_fronds) {
        g.full = true;
        return;
      }
      Frond frond;
      frond.branch = parent_index;
      frond.level = &level;
      frond.id = noise::hash(uint32_t(level.id), uint32_t(parent_index), uint32_t(i), g.seed);
      const float random = noise::hash_to_float(frond.id, 0x5e11u) * 2.0f - 1.0f;
      frond.roll = float(M_PI) * float(i) / float(planes) +
                   random * level.frond_roll_random * float(M_PI);
      /* Prune and Crown nodes take whole fronds away, judged at the middle of the frond. */
      const Sample middle = sample(parent, piece_t(parent, (lo + hi) * 0.5f));
      FieldContext context = parent.context;
      context.position = middle.position + g.offset;
      context.id = frond.id;
      bool removed = false;
      for (const Prune &prune : level.prunes) {
        const float amount = std::clamp(prune.amount.at(context), 0.0f, 1.0f);
        const float r = noise::hash_to_float(frond.id, uint32_t(prune.seed) + 0x9e11u);
        if (prune_removes(g, level_index, prune, amount, middle.position, middle.tangent, r)) {
          removed = true;
          break;
        }
      }
      if (!removed) {
        g.tree.fronds.append(frond);
      }
    }
  }
}

/** Branches along the drawn curves of a level. A curve that starts on another grows from it,
 * the others grow from the nearest parent, or stand as trunks when the level has none. */
static void grow_drawn(Grower &g, const int level_index, const Span<int> parents, Vector<int> &made)
{
  const Level &level = g.params.levels[level_index];
  const int count = level.splines.size();
  if (count == 0) {
    return;
  }
  const int segments = std::max(level.segments, 2);

  /* Every curve evenly spaced along its length. */
  Array<Vector<float3>> paths(count);
  Array<Vector<float>> curve_radii(count);
  Array<float> lengths(count, 0.0f);
  for (const int i : IndexRange(count)) {
    const DrawnSpline &spline = level.splines[i];
    const int n = spline.points.size();
    if (n < 2) {
      continue;
    }
    Array<float> at(n, 0.0f);
    for (const int k : IndexRange(1, n - 1)) {
      at[k] = at[k - 1] + math::distance(spline.points[k - 1], spline.points[k]);
    }
    const float total = at.last();
    if (total <= 1e-6f) {
      continue;
    }
    lengths[i] = total;
    int k = 1;
    for (const int s_i : IndexRange(segments + 1)) {
      const float want = total * float(s_i) / float(segments);
      while (k < n - 1 && at[k] < want) {
        k++;
      }
      const float w = std::clamp((want - at[k - 1]) / std::max(at[k] - at[k - 1], 1e-8f), 0.0f, 1.0f);
      paths[i].append(math::interpolate(spline.points[k - 1], spline.points[k], w));
      const float r0 = spline.radii.size() == n ? spline.radii[k - 1] : 1.0f;
      const float r1 = spline.radii.size() == n ? spline.radii[k] : 1.0f;
      curve_radii[i].append(std::max(math::interpolate(r0, r1, w), 0.0f));
    }
  }

  /* Which curve each one starts on. */
  Array<int> on_curve(count, -1);
  Array<float> on_t(count, 0.0f);
  const float snap = std::max(level.snap, 0.0f);
  for (const int i : IndexRange(count)) {
    if (paths[i].is_empty()) {
      continue;
    }
    float best = FLT_MAX;
    for (const int j : IndexRange(count)) {
      if (j == i || paths[j].is_empty()) {
        continue;
      }
      float t;
      const float d = closest_on_line(paths[j], paths[i][0], t);
      /* Starting at the very end of the other one is going on from it, not growing on it. */
      if (d <= snap && d < best && t < 0.999f) {
        best = d;
        on_curve[i] = j;
        on_t[i] = t;
      }
    }
  }
  /* Curves that start on each other in a ring: the first of them stands on its own. */
  for (const int i : IndexRange(count)) {
    int j = on_curve[i];
    for (int steps = 0; j >= 0 && steps <= count; steps++) {
      if (j == i) {
        on_curve[i] = -1;
        break;
      }
      j = on_curve[j];
    }
  }

  Array<int> made_index(count, -1);
  Array<bool> done(count, false);
  bool progress = true;
  while (progress) {
    progress = false;
    for (const int i : IndexRange(count)) {
      if (done[i] || paths[i].is_empty()) {
        continue;
      }
      const int on = on_curve[i];
      if (on >= 0 && !done[on]) {
        continue;
      }
      done[i] = true;
      progress = true;
      if (on >= 0 && made_index[on] < 0) {
        /* The curve it starts on was pruned away. */
        continue;
      }

      int parent_index = -1;
      float parent_t = 0.0f;
      if (on >= 0) {
        parent_index = made_index[on];
        parent_t = on_t[i];
      }
      else if (!parents.is_empty()) {
        float best = FLT_MAX;
        for (const int p : parents) {
          float t;
          const float d = closest_on_line(g.tree.branches[p].points, paths[i][0], t);
          if (d < best) {
            best = d;
            parent_index = p;
            parent_t = t;
          }
        }
      }

      FieldContext context = element_context(
          g, level, parent_index, i, count > 1 ? float(i) / float(count - 1) : 0.0f, paths[i][0]);
      context.count = count;
      Branch branch;
      level_settings(g, level, branch, context);
      branch.random = context.random;
      branch.points = paths[i];
      branch.length = lengths[i];
      branch.radii.resize(segments + 1);
      for (const int k : IndexRange(segments + 1)) {
        context.along_branch = float(k) / float(segments);
        context.position = paths[i][k] + g.offset;
        branch.radii[k] = std::max(curve_radii[i][k] * level.radius.at(context) *
                                       (1.0f - std::clamp(level.taper.at(context), 0.0f, 1.0f) *
                                                   context.along_branch),
                                   1e-5f);
      }
      branch.base_radius = branch.radii[0];
      if (parent_index >= 0) {
        const Branch &parent = g.tree.branches[parent_index];
        branch.parent = parent_index;
        branch.parent_t = parent_t;
        context.along_branch = 0.0f;
        branch.weld = std::clamp(level.weld.at(context), 0.0f, 1.0f);
        branch.sides_ref = parent.sides_ref;
        branch.radius_ref = parent.radius_ref;
      }
      else {
        branch.sides_ref = std::max(level.sides, 3);
        branch.radius_ref = branch.base_radius;
      }
      const int64_t before = made.size();
      if (!add_branch(g, level, std::move(branch), made)) {
        return;
      }
      if (made.size() > before) {
        made_index[i] = made.last();
      }
    }
  }
}

/**
 * Ivy: thin vines that wind around the bark of their parent as they climb it, the end of each
 * hanging free. The climb is an angle to the ring around the parent, so they wind as closely on thin
 * stems as on thick ones. They are branches, so leaves and branches can grow on them.
 */
static void grow_ivy(Grower &g, const int level_index, const Span<int> parents, Vector<int> &made)
{
  const Level &level = g.params.levels[level_index];
  const float scale = g.params.scale;
  const int sections = std::max(level.segments, 2);
  for (const int parent_index : parents) {
    /* A copy, adding branches moves them in memory. */
    const Branch parent = g.tree.branches[parent_index];
    const int pn = parent.points.size();
    if (pn < 2) {
      continue;
    }
    float lo, hi;
    if (!piece_range(level, parent, lo, hi)) {
      continue;
    }
    const float t_lo = piece_t(parent, lo);
    const float t_hi = piece_t(parent, hi);
    float parent_length = 0.0f;
    for (const int i : IndexRange(pn - 1)) {
      parent_length += math::distance(parent.points[i], parent.points[i + 1]);
    }
    if (parent_length < 1e-6f) {
      continue;
    }

    /* A frame carried along the parent without turning, so the vines wind evenly. */
    Array<float3> tangents(pn);
    Array<float3> normals(pn);
    for (const int i : IndexRange(pn)) {
      const float3 d = parent.points[std::min(i + 1, pn - 1)] - parent.points[std::max(i - 1, 0)];
      tangents[i] = math::length_squared(d) > 1e-12f ? math::normalize(d) :
                                                       float3(0.0f, 0.0f, 1.0f);
    }
    normals[0] = any_perpendicular(tangents[0]);
    for (const int i : IndexRange(1, pn - 1)) {
      float3 n = rotate_between(normals[i - 1], tangents[i - 1], tangents[i]);
      n -= tangents[i] * math::dot(n, tangents[i]);
      normals[i] = math::length_squared(n) > 1e-12f ? math::normalize(n) :
                                                      any_perpendicular(tangents[i]);
    }
    /* The place on the parent's line at \a t, and the way out from it at \a around. */
    const auto on_parent = [&](const float t, const float around, float3 &r_position, float &r_radius)
        -> float3 {
      const Sample s = sample(parent, t);
      const float f = math::clamp(t, 0.0f, 1.0f) * float(pn - 1);
      const int i = std::min(int(f), pn - 2);
      const float w = f - float(i);
      float3 tangent = math::interpolate(tangents[i], tangents[i + 1], w);
      tangent = math::length_squared(tangent) > 1e-12f ? math::normalize(tangent) : s.tangent;
      float3 n = math::interpolate(normals[i], normals[i + 1], w);
      n -= tangent * math::dot(n, tangent);
      n = math::length_squared(n) > 1e-12f ? math::normalize(n) : any_perpendicular(tangent);
      const float3 b = math::cross(tangent, n);
      r_position = s.position;
      r_radius = s.radius;
      return n * std::cos(around) + b * std::sin(around);
    };

    const FieldContext &pc = parent.context;
    const float count_value = level.count_field.fn ? level.count_field.at(pc) : float(level.count);
    const int count = std::clamp(int(std::round(count_value)), 0, 64);
    RandomNumberGenerator rng(
        noise::hash(g.seed, uint32_t(level.id), uint32_t(parent_index), 0x1e7u));
    for (const int c : IndexRange(count)) {
      /* Even starts around the parent, a little apart along it. */
      const float t_start = t_lo + (t_hi - t_lo) * 0.1f * rng.get_float();
      float around = 2.0f * float(M_PI) * (float(c) + 0.5f * rng.get_float()) / float(count);
      const float spin = rng.get_float() < 0.5f ? -1.0f : 1.0f;
      float3 base;
      float base_radius;
      on_parent(t_start, around, base, base_radius);
      const float along = parent.t0 + t_start * (parent.t1 - parent.t0);
      FieldContext context = element_context(g, level, parent_index, c, along, base);
      context.count = count;
      context.around = around;
      const float length = std::max(level.length.at(context), 0.0f) * scale;
      const float radius = std::max(level.radius.at(context), 0.0f) * scale;
      if (length < 1e-4f * scale || radius <= 0.0f) {
        continue;
      }
      const float taper = std::clamp(level.taper.at(context), 0.0f, 1.0f);
      const float hang = std::clamp(level.droop.at(context), 0.0f, 1.0f);
      const float climb = level.angle.at(context);
      const float gap = std::max(level.ivy_gap, 0.0f) * scale;
      const float step = length / float(sections);

      Branch branch;
      level_settings(g, level, branch, context);
      branch.points.reserve(sections + 1);
      branch.radii.reserve(sections + 1);
      float t = t_start;
      float wander = 0.0f;
      bool hanging = false;
      float3 p(0.0f);
      float3 d(0.0f, 0.0f, 1.0f);
      for (const int k : IndexRange(sections + 1)) {
        const float s = float(k) / float(sections);
        const float r = std::max(radius * (1.0f - taper * s), 1e-4f * scale);
        float offset = 0.0f;
        if (hanging) {
          p += d * step;
        }
        else {
          float3 center;
          float parent_radius;
          const float3 out = on_parent(t, around, center, parent_radius);
          offset = parent_radius + r + gap;
          const float3 next = center + out * offset;
          if (k > 0 && math::length_squared(next - p) > 1e-12f) {
            d = math::normalize(next - p);
          }
          p = next;
        }
        branch.points.append(p);
        branch.radii.append(r);
        if (k == sections) {
          break;
        }
        if (hanging) {
          /* The free end bends down under its own weight. */
          d = turn_toward(d, float3(0.0f, 0.0f, -1.0f), 0.2f);
          continue;
        }
        context.along_branch = s;
        context.position = p + g.offset;
        wander = wander * 0.7f + (rng.get_float() * 2.0f - 1.0f) * 0.3f;
        const float angle = std::clamp(climb + level.wiggle.at(context) * wander, -1.5f, 1.5f);
        t = std::max(t + step * std::sin(angle) / parent_length, 0.0f);
        around += spin * step * std::cos(angle) / std::max(offset, 1e-6f);
        const bool past_top = t > t_hi;
        if (past_top && hang <= 0.0f) {
          break;
        }
        if (past_top || float(k + 1) / float(sections) > 1.0f - hang) {
          hanging = true;
          if (k == 0) {
            d = math::normalize(math::interpolate(parent.points[1] - parent.points[0], d, 0.5f));
          }
        }
      }
      if (branch.points.size() < 2) {
        continue;
      }
      branch.length = step * float(branch.points.size() - 1);
      branch.base_radius = branch.radii[0];
      branch.t0 = 0.0f;
      branch.t1 = 1.0f;
      branch.parent = parent_index;
      branch.parent_t = t_start;
      branch.weld = 0.0f;
      branch.sides_ref = parent.sides_ref;
      branch.radius_ref = parent.radius_ref;
      if (!add_branch(g, level, std::move(branch), made)) {
        return;
      }
    }
  }
}

static void grow_level(Grower &g, const int level_index, const Span<int> parents, const int depth)
{
  if (depth > max_depth || g.full) {
    return;
  }
  const Level &level = g.params.levels[level_index];
  g.depth = depth;
  Vector<int> made;
  switch (level.type) {
    case LevelType::Trunk:
      /* Trunks always stand on the ground. */
      if (depth == 0) {
        grow_trunks(g, level_index, made);
      }
      break;
    case LevelType::Branches:
      grow_branches(g, level_index, parents, made);
      break;
    case LevelType::Roots:
      grow_roots(g, level_index, parents, made);
      break;
    case LevelType::Leaves:
      if (g.params.leaves) {
        grow_leaves(g, level_index, parents);
        if (level.use_clumps) {
          g.tree.clumps.build();
        }
      }
      return;
    case LevelType::Fronds:
      grow_fronds(g, level_index, parents);
      return;
    case LevelType::Drawn:
      grow_drawn(g, level_index, parents, made);
      break;
    case LevelType::Ivy:
      grow_ivy(g, level_index, parents, made);
      break;
  }
  if (made.is_empty()) {
    return;
  }
  /* Branch levels go first, so the leaves know which stems carry on from their tips. */
  for (const int child : level.children) {
    if (g.params.levels[child].type != LevelType::Leaves) {
      grow_level(g, child, made, depth + 1);
    }
  }
  for (const int child : level.children) {
    if (g.params.levels[child].type == LevelType::Leaves) {
      grow_level(g, child, made, depth + 1);
    }
  }
}

/** \} */


/* -------------------------------------------------------------------- */
/** \name Detail
 *
 * One number from 0 to 1 scales the sides, segments and leaf count together, the largest one that
 * fits the triangle budget is used.
 * \{ */

/** The detail of one branch, its level keeps more or less of it than the others. */
static float branch_detail(const Branch &branch, const float detail)
{
  return std::clamp(detail * branch.mesh_detail, 0.02f, 1.0f);
}

static int branch_sides(const Branch &branch, const float tree_detail)
{
  const float detail = branch_detail(branch, tree_detail);
  if (branch.mesh_sides > 0) {
    return std::max(3, int(std::round(float(branch.mesh_sides) * detail)));
  }
  const float sides = float(branch.sides_ref) *
                      std::sqrt(branch.base_radius / std::max(branch.radius_ref, 1e-6f)) * detail;
  /* Even the thinnest twig stays round. */
  return std::max(3, int(std::round(sides)));
}

static int branch_segments(const Branch &branch, const float tree_detail)
{
  const float detail = branch_detail(branch, tree_detail);
  return std::max(2, int(std::round(float(branch.points.size() - 1) * detail)));
}

static int frond_segments(const Frond &frond, const float detail)
{
  return std::max(1, int(std::round(float(std::max(frond.level->segments, 1)) * detail)));
}

static bool frond_folded(const Frond &frond)
{
  return frond.level->fold.fn || frond.level->fold.value != 0.0f;
}

/** Columns of faces across a frond, even when it is folded so the crease has a column. */
/** Sides around and rings up of a shown clump. */
static int2 clump_grid(const float detail)
{
  const int sides = std::max(
      int(std::round(16.0f * std::sqrt(std::clamp(detail, 0.0f, 1.0f)))), 6);
  return int2(sides, std::max(sides / 2, 3));
}

static int frond_columns(const Frond &frond)
{
  int across = std::clamp(frond.level->frond_across, 1, 16);
  if (frond_folded(frond) && across % 2 == 1) {
    across++;
  }
  return across;
}

static int64_t tree_triangles(const Tree &tree, const float detail)
{
  int64_t total = 0;
  for (const Branch &branch : tree.branches) {
    if (!branch.mesh) {
      continue;
    }
    const int sides = branch_sides(branch, detail);
    const int segments = branch_segments(branch, detail);
    if (sides == 2) {
      total += branch.cut ? 2 * segments : 2 * (segments - 1) + 1;
    }
    else {
      /* A cut end has a ring of bark around a low dome of wood. */
      total += branch.cut ? 2 * sides * segments + 3 * sides :
                            2 * sides * (segments - 1) + sides;
    }
  }
  for (const Leaf &leaf : tree.leaves) {
    if (leaf.keep < detail) {
      total += leaf.triangles;
    }
  }
  for (const Frond &frond : tree.fronds) {
    total += int64_t(frond_segments(frond, detail)) * frond_columns(frond) * 2;
  }
  const int2 clump = clump_grid(detail);
  for (const Clump &c : tree.clumps.clumps) {
    if (c.show) {
      total += 2 * clump.x * (clump.y - 1);
    }
  }
  return total;
}

static float choose_detail(const Tree &tree, const int budget)
{
  if (budget <= 0 || tree_triangles(tree, 1.0f) <= budget) {
    return 1.0f;
  }
  float low = 0.02f;
  float high = 1.0f;
  if (tree_triangles(tree, low) > budget) {
    return low;
  }
  for (int i = 0; i < 20; i++) {
    const float mid = 0.5f * (low + high);
    if (tree_triangles(tree, mid) <= budget) {
      low = mid;
    }
    else {
      high = mid;
    }
  }
  return low;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mesh
 * \{ */

/** What the vertex data of one vertex is made from. */
struct VertInfo {
  float branch_random = 0.0f;
  float leaf_random = 0.0f;
  int level = 0;
  int depth = 0;
  float along = 0.0f;
  float height = 0.0f;
  float radius = 0.0f;
  float facing = 0.5f;
  float clump_random = 0.0f;
  float clump_depth = 0.0f;
  float clump_edge = 0.0f;
};

struct MeshBuilder {
  Vector<float3> positions;
  Vector<int> face_sizes;
  Vector<int> corner_verts;
  Vector<float2> uvs;
  Vector<float3> normals;
  Vector<int> materials;
  /** Per vertex, filled up to the vertices of branches, the rest are 0. */
  Vector<float> weld;
  bool any_material = false;
  bool any_weld = false;
  /** Per vertex, for the vertex data. Vertices not set get #current. */
  Vector<VertInfo> info;
  VertInfo current;

  void fill()
  {
    info.resize(positions.size(), current);
  }

  void corner(const int vert, const float2 &uv, const float3 &normal)
  {
    corner_verts.append(vert);
    uvs.append(uv);
    normals.append(normal);
  }

  void face(const int size, const int material)
  {
    face_sizes.append(size);
    materials.append(material);
    any_material |= material != 0;
  }
};

/** What the mesh leaves of one mesh need, read once. */
struct LeafMeshData {
  Span<float3> positions;
  OffsetIndices<int> faces;
  Span<int> corner_verts;
  Span<float3> corner_normals;
  /** Empty when the mesh has no UV map. */
  VArraySpan<float2> uvs;
  /** Empty when the mesh has no material indices. */
  VArraySpan<int> materials;
};

/** Where a stem that forks off the end of another starts: on the last ring of that one. */
struct RingStart {
  int sides = 0;
  float3 normal{};
  float3 tangent{};
  float v_scale = 1.0f;
  float v = 0.0f;
};

/** The rings of one branch, from its base to its end. Positions are in the space of the tree. */
struct BranchRings {
  int sides = 0;
  int segments = 0;
  /** Per ring: its place on the branch, 0 to 1, its middle, side direction and radius. */
  Vector<float> ts;
  Vector<float3> centers;
  Vector<float3> frames;
  Vector<float> radii;
  /** Per vertex. */
  Vector<float3> positions;
  Vector<float3> normals;
  /** How much of each vertex is in the weld onto the parent, 1 on the parent's surface. */
  Vector<float> weld;
  /** Texture coordinate along the branch. */
  Vector<float> vs;
  Sample tip{};
  float v_scale = 1.0f;
  float v_offset = 0.0f;

  int count() const
  {
    return ts.size();
  }
  int index(const int i, const int j) const
  {
    return i * sides + (j % sides + sides) % sides;
  }
  const float3 &position(const int i, const int j) const
  {
    return positions[this->index(i, j)];
  }
  float v(const int i, const int j) const
  {
    return vs[this->index(i, j)];
  }
};

static bool has_bark_shape(const Branch &branch)
{
  const Level *level = branch.level;
  return (branch.flutes > 0 && (branch.flute_depth > 0.0f || (level && level->flute_depth.fn))) ||
         branch.bumps > 0.0f || (level && level->bumps.fn) ||
         (level != nullptr && !level->displaces.is_empty());
}

/** Gray of a displacement image, wrapping around. */
static float image_gray(const Displace &displace, const float2 &uv)
{
  const int w = displace.image_size.x;
  const int h = displace.image_size.y;
  if (w <= 0 || h <= 0 || displace.image.size() < int64_t(w) * h) {
    return 0.0f;
  }
  const float x = (uv.x - std::floor(uv.x)) * float(w) - 0.5f;
  const float y = (uv.y - std::floor(uv.y)) * float(h) - 0.5f;
  const int x0 = int(std::floor(x));
  const int y0 = int(std::floor(y));
  const float fx = x - float(x0);
  const float fy = y - float(y0);
  const auto at = [&](const int i, const int j) {
    return displace.image[((j % h + h) % h) * w + ((i % w + w) % w)];
  };
  return math::interpolate(math::interpolate(at(x0, y0), at(x0 + 1, y0), fx),
                           math::interpolate(at(x0, y0 + 1), at(x0 + 1, y0 + 1), fx),
                           fy);
}

/** How far out the surface moves by the level's displacement, in the tree's units. */
static float displacement(const Branch &branch,
                          const Displace &d,
                          const float theta,
                          const float u,
                          const float radius,
                          const FieldContext &at)
{
  /* The numbers of the node, for this place on the branch. */
  struct {
    float radial, along, scale, fine_amount, fine_scale, shape_amount, shape_twist, shape_scale;
    float flare_radius, flare_height, flare_pinch, flare_angle;
    int seed, flare_count;
  } displace = {d.radial.at(at),
                d.along.at(at),
                std::max(d.scale.at(at), 1e-3f),
                std::max(d.fine_amount.at(at), 0.0f),
                std::max(d.fine_scale.at(at), 1e-3f),
                d.shape_amount.at(at),
                d.shape_twist.at(at),
                std::max(d.shape_scale.at(at), 1e-3f),
                std::max(d.flare_radius.at(at), 0.0f),
                std::clamp(d.flare_height.at(at), 0.0f, 1.0f),
                std::clamp(d.flare_pinch.at(at), 0.0f, 1.0f),
                d.flare_angle.at(at),
                d.seed,
                d.flare_count};
  const float scale = std::max(branch.scale, 1e-6f);
  const float stem_length = branch.length / std::max(branch.t1 - branch.t0, 1e-3f);
  const float along = u * stem_length;
  const float base_radius = std::max(branch.radius_ref, radius);
  float out = 0.0f;
  if (d.use_image) {
    const float u_tiles = branch.level ? float(std::max(branch.level->u_tiles, 1)) : 1.0f;
    const float v = along / (2.0f * float(M_PI) * std::max(base_radius, 1e-5f)) * u_tiles;
    out += (image_gray(d, float2(theta / (2.0f * float(M_PI)) * u_tiles, v)) - 0.5f) *
           2.0f * displace.radial * scale;
  }
  else if (displace.radial != 0.0f) {
    const float size = std::max(displace.scale * scale, 1e-5f);
    const float3 seed = seed_offset(displace.seed);
    /* Around the stem at its own radius, along it stretched for ridges. */
    const float3 p = float3(std::cos(theta) * radius,
                            std::sin(theta) * radius,
                            along / (1.0f + std::max(displace.along, 0.0f) * 4.0f)) /
                         size +
                     seed;
    float n = noise::perlin_signed(p);
    if (displace.fine_amount > 0.0f) {
      const float fine = std::max(displace.fine_scale, 1e-3f) * 4.0f;
      n += noise::perlin_signed(p * fine + float3(17.0f)) * displace.fine_amount * 0.5f;
    }
    out += n * displace.radial * scale;
  }
  if (displace.shape_amount != 0.0f) {
    const float twisted = theta + displace.shape_twist * along / std::max(stem_length, 1e-5f) *
                                      2.0f * float(M_PI);
    const float size = std::max(displace.shape_scale * scale, 1e-5f);
    const float3 p = float3(std::cos(twisted), std::sin(twisted), along / size) +
                     seed_offset(displace.seed + 101);
    out += noise::perlin_signed(p) * displace.shape_amount * radius;
  }
  if (displace.flare_count > 0 && branch.level != nullptr &&
      branch.level->type == LevelType::Trunk && displace.flare_height > 0.0f)
  {
    const float h = u / displace.flare_height;
    if (h < 1.0f) {
      const float wave = std::abs(
          std::cos(float(displace.flare_count) * (theta - displace.flare_angle) * 0.5f));
      const float blade = std::pow(wave, 1.0f + std::clamp(displace.flare_pinch, 0.0f, 1.0f) * 8.0f);
      out += displace.flare_radius * base_radius * blade * (1.0f - h) * (1.0f - h);
    }
  }
  return out;
}

/** How far out the bark is at a point, as a share of the round radius: grooves between ridges
 * around the stem, fading out higher up, lumps and the level's displacement. */
static float bark_shape(const Branch &branch,
                        const float theta,
                        const float u,
                        const float3 &position,
                        const float radius)
{
  float shape = 1.0f;
  /* Nodes linked into the numbers read this place on the branch. */
  FieldContext at = branch.context;
  at.along_branch = u;
  at.position = position + branch.context.origin;
  const Level *level = branch.level;
  const auto value = [&](const Param *param, const float base) {
    return (param != nullptr && param->fn) ? param->at(at) : base;
  };
  if (level != nullptr && radius > 1e-7f) {
    for (const Displace &displace : level->displaces) {
      shape += displacement(branch, displace, theta, u, radius, at) / radius;
    }
  }
  const float flute_depth = value(level ? &level->flute_depth : nullptr, branch.flute_depth);
  if (branch.flutes > 0 && flute_depth > 0.0f) {
    const float flute_height = value(level ? &level->flute_height : nullptr, branch.flute_height);
    const float fade = flute_height >= 1.0f ?
                           1.0f :
                           std::clamp(1.0f - u / std::max(flute_height, 1e-3f), 0.0f, 1.0f);
    /* Round ridges with narrow grooves between them. */
    const float groove = 1.0f - std::abs(std::cos(float(branch.flutes) * theta * 0.5f));
    shape -= std::min(flute_depth, 0.9f) * fade * groove * groove;
  }
  const float bumps = value(level ? &level->bumps : nullptr, branch.bumps);
  if (bumps > 0.0f) {
    const float bump_size = (level && level->bump_size.fn) ?
                                std::max(level->bump_size.at(at) * branch.scale, 1e-4f) :
                                branch.bump_size;
    shape += bumps * noise::perlin_signed(position / bump_size);
  }
  return std::max(shape, 0.1f);
}

/** Normals from the shape of the rings themselves, for rings \a first up to \a end. */
static void ring_normals(BranchRings &r, const int first, const int end)
{
  const int count = r.count();
  for (const int i : IndexRange(first, std::max(end - first, 0))) {
    const int i_prev = std::max(i - 1, 0);
    const int i_next = std::min(i + 1, count - 1);
    for (const int j : IndexRange(r.sides)) {
      const float3 around = r.position(i, j + 1) - r.position(i, j - 1);
      const float3 along = r.position(i_next, j) - r.position(i_prev, j);
      float3 normal = math::cross(around, along);
      if (math::length_squared(normal) < 1e-14f) {
        continue;
      }
      normal = math::normalize(normal);
      float3 &old = r.normals[r.index(i, j)];
      if (math::dot(normal, old) < 0.0f) {
        normal = -normal;
      }
      old = normal;
    }
  }
}

/** Side directions carried along a stem without twisting, in small steps so any place on it can
 * be looked up. */
struct FrameTable {
  static constexpr int steps = 32;
  std::array<float3, steps + 1> normals;

  float3 at(const float t, const float3 &tangent) const
  {
    const float f = std::clamp(t, 0.0f, 1.0f) * float(steps);
    const int i = std::min(int(f), steps - 1);
    float3 n = math::interpolate(normals[i], normals[i + 1], f - float(i));
    n -= tangent * math::dot(n, tangent);
    return math::length_squared(n) > 1e-10f ? math::normalize(n) : any_perpendicular(tangent);
  }
};

/** Cards face up, tubes start their sides at the back so the UV seam is out of sight, a forked
 * stem starts with the sides of the stem it goes on from (\a start_normal). */
static FrameTable stem_frames(const Branch &branch,
                              const int branch_index,
                              const int sides,
                              const float3 *start_normal)
{
  FrameTable table;
  float3 normal;
  if (start_normal) {
    normal = *start_normal;
  }
  else {
    const Sample s0 = sample(branch, 0.0f);
    const float3 ref = sides == 2 ? float3(0.0f, 0.0f, 1.0f) : float3(0.0f, 1.0f, 0.0f);
    normal = ref - s0.tangent * math::dot(ref, s0.tangent);
    normal = math::length_squared(normal) > 1e-6f ? math::normalize(normal) :
                                                     any_perpendicular(s0.tangent);
    if (sides > 2 && branch.level != nullptr) {
      /* Turned by the roll, and anywhere when the seam is not kept at the back. */
      float roll = branch.level->roll;
      if (!branch.level->seam_fix) {
        roll += noise::hash_to_float(uint32_t(branch_index), 0x5ea3u) * 2.0f * float(M_PI);
      }
      if (roll != 0.0f) {
        normal = math::normalize(rotate_around(normal, s0.tangent, roll));
      }
    }
  }
  for (const int k : IndexRange(FrameTable::steps + 1)) {
    const Sample s = sample(branch, float(k) / float(FrameTable::steps));
    const float3 moved = normal - s.tangent * math::dot(normal, s.tangent);
    if (math::length_squared(moved) > 1e-8f) {
      normal = math::normalize(moved);
    }
    table.normals[k] = normal;
  }
  return table;
}

/** The parent's surface near a point, for the weld. */
struct OnParent {
  /** The closest point on the surface, its normal and the parent's direction there. */
  float3 point;
  float3 normal;
  float3 tangent;
  /** The normal of the parent's bark there, with its displacement. */
  float3 surface_normal;
  /** How far the point is from the parent's middle, and the parent's radius there. */
  float out;
  float radius;
};

/** One line along the side of a welded branch, from the parent's surface into the branch. */
struct WeldLine {
  /** On the parent where the weld ends, the corner where the side comes out of the parent, and
   * where the weld ends on the branch. */
  float3 base;
  float3 corner;
  float3 end;
  float3 base_normal;
  float3 end_normal;
  float t_corner;
  float t_end;
  float spread;
};

static BranchRings branch_rings(const Tree &tree,
                                const Branch &branch,
                                const int branch_index,
                                const float detail,
                                const RingStart *from)
{
  BranchRings r;
  const int sides = from ? from->sides : branch_sides(branch, detail);
  const int segments = branch_segments(branch, detail);
  const float length = std::max(branch.length, 1e-6f);
  r.sides = sides;
  r.segments = segments;
  r.tip = sample(branch, 1.0f);
  if (from) {
    /* Going on from the stem it forks off, the bark runs on without a seam. */
    r.v_scale = from->v_scale;
    r.v_offset = from->v;
  }
  else {
    /* The bark texture tiles around the given times, and along at the same scale. Every branch
     * starts at another place in the texture so their bases don't all look the same. */
    const Level *level = branch.level;
    const float stem_length = branch.length / std::max(branch.t1 - branch.t0, 1e-3f);
    if (level && level->fit_one_tile) {
      r.v_scale = std::max(branch.v_tiles, 1e-3f) / std::max(stem_length, 1e-6f);
    }
    else if (level && level->v_mode == 1) {
      r.v_scale = std::max(branch.v_tiles, 1e-3f) / std::max(branch.scale, 1e-6f);
    }
    else {
      r.v_scale = float(std::max(branch.u_tiles, 1)) * std::max(branch.v_tiles, 1e-3f) /
                  (2.0f * float(M_PI) * std::max(branch.base_radius, 1e-6f));
    }
    r.v_offset = (level && level->fit_one_tile) ?
                     0.0f :
                     noise::hash_to_float(uint32_t(branch_index), 0x51a7u);
    if (level) {
      r.v_offset += level->v_offset;
    }
  }
  const auto v_at = [&](const float t) { return length * t * r.v_scale + r.v_offset; };
  /* A cut or forking branch ends in a ring, others in a point after the last ring. */
  const bool ends_in_ring = branch.cut || branch.open_end;
  const int last_k = ends_in_ring ? segments : segments - 1;

  /* Side directions carried along the branch without twisting, in small steps so any place on
   * it can be looked up. Cards face up, tubes start their sides at the back so the UV seam is out
   * of sight, a forked stem starts with the sides of the stem it goes on from. */
  const FrameTable frame_table = stem_frames(
      branch, branch_index, sides, from ? &from->normal : nullptr);
  const auto frame_at = [&](const float t, const float3 &tangent) {
    return frame_table.at(t, tangent);
  };
  const auto side_dir = [&](const float3 &tangent, const float3 &normal, const int j) {
    const float theta = 2.0f * float(M_PI) * float(j) / float(sides);
    return normal * std::cos(theta) + math::cross(tangent, normal) * std::sin(theta);
  };
  const auto add_ring = [&](const float t, const float3 &center, const float3 &frame) {
    r.ts.append(t);
    r.centers.append(center);
    r.frames.append(frame);
    r.radii.append(sample(branch, t).radius);
  };

  if (sides == 2) {
    for (const int k : IndexRange(last_k + 1)) {
      const float t = float(k) / float(segments);
      const Sample s = sample(branch, t);
      const float3 normal = frame_at(t, s.tangent);
      const float3 binormal = math::cross(s.tangent, normal);
      add_ring(t, s.position, normal);
      r.positions.append(s.position - binormal * s.radius);
      r.positions.append(s.position + binormal * s.radius);
      for ([[maybe_unused]] const int j : IndexRange(2)) {
        r.normals.append(normal);
        r.weld.append(0.0f);
        r.vs.append(v_at(t));
      }
    }
    return r;
  }

  /* Welded onto the parent. Every line along the branch's side comes out of the parent at its
   * own place: there it rounds off onto the parent's surface, spreading out over it, so the
   * branch grows out of the parent instead of being stuck into it. */
  Vector<WeldLine> lines;
  float weld_t_end = 0.0f;
  if (!from && branch.weld > 0.0f && branch.parent >= 0 && tree.branches[branch.parent].mesh) {
    const Branch &parent = tree.branches[branch.parent];
    const Sample at_parent = sample(parent, branch.parent_t);
    const float parent_length = std::max(parent.length, 1e-6f);
    /* A little inside the round, so the weld does not float over the flat faces of the parent. */
    const int parent_sides = std::max(branch_sides(parent, detail), 3);
    const float sink = 0.99f * std::cos(float(M_PI) / float(parent_sides));
    /* The parent's bark is pushed in and out by its displacement, flutes and bumps: the weld
     * sits on that surface, not on the round one. */
    const bool parent_shaped = has_bark_shape(parent);
    const FrameTable parent_frames = parent_shaped ?
                                         stem_frames(parent, branch.parent, parent_sides, nullptr) :
                                         FrameTable();
    const auto parent_surface = [&](const float t, const float theta) {
      const Sample s = sample(parent, t);
      const float3 n0 = parent_frames.at(t, s.tangent);
      const float3 dir = n0 * std::cos(theta) + math::cross(s.tangent, n0) * std::sin(theta);
      const float u = parent.t0 + (parent.t1 - parent.t0) * t;
      const float shape = bark_shape(parent, theta, u, s.position + dir * s.radius, s.radius);
      return s.position + dir * (s.radius * shape);
    };
    const auto on_parent = [&](const float3 &p) {
      float t = branch.parent_t;
      Sample s = at_parent;
      for ([[maybe_unused]] const int step : IndexRange(3)) {
        t = std::clamp(t + math::dot(p - s.position, s.tangent) / parent_length, 0.0f, 1.0f);
        s = sample(parent, t);
      }
      const float3 rel = p - s.position;
      const float3 radial = rel - s.tangent * math::dot(rel, s.tangent);
      OnParent o;
      o.out = math::length(radial);
      o.radius = s.radius * sink;
      o.tangent = s.tangent;
      o.normal = o.out > 1e-8f ? radial / o.out : any_perpendicular(s.tangent);
      o.surface_normal = o.normal;
      if (parent_shaped) {
        const float3 n0 = parent_frames.at(t, s.tangent);
        float theta = std::atan2(math::dot(o.normal, math::cross(s.tangent, n0)),
                                 math::dot(o.normal, n0));
        if (theta < 0.0f) {
          theta += 2.0f * float(M_PI);
        }
        const float u = parent.t0 + (parent.t1 - parent.t0) * t;
        o.radius *= bark_shape(parent, theta, u, s.position + o.normal * s.radius, s.radius);
        /* The normal of the shaped surface, from its slopes around and along. */
        const float d_theta = 0.5f * float(M_PI) / float(parent_sides);
        const float d_t = std::max(0.25f * s.radius / parent_length, 1e-4f);
        const float3 around = parent_surface(t, theta + d_theta) -
                              parent_surface(t, theta - d_theta);
        const float3 along = parent_surface(std::min(t + d_t, 1.0f), theta) -
                             parent_surface(std::max(t - d_t, 0.0f), theta);
        const float3 n = math::cross(along, around);
        if (math::length_squared(n) > 1e-14f) {
          const float3 nn = math::normalize(n);
          o.surface_normal = math::dot(nn, o.normal) < 0.0f ? -nn : nn;
        }
      }
      o.point = p - radial + o.normal * o.radius;
      return o;
    };
    const auto side_point = [&](const float t, const int j) {
      const Sample s = sample(branch, t);
      return s.position + side_dir(s.tangent, frame_at(t, s.tangent), j) * s.radius;
    };
    const auto outside = [&](const float t, const int j) {
      const OnParent o = on_parent(side_point(t, j));
      return o.out >= o.radius;
    };
    constexpr float t_search = 0.6f;
    bool ok = true;
    for (const int j : IndexRange(sides)) {
      if (outside(0.0f, j) || !outside(t_search, j)) {
        ok = false;
        break;
      }
      float lo = 0.0f;
      float hi = t_search;
      for ([[maybe_unused]] const int step : IndexRange(20)) {
        const float mid = 0.5f * (lo + hi);
        if (outside(mid, j)) {
          hi = mid;
        }
        else {
          lo = mid;
        }
      }
      WeldLine line;
      line.t_corner = hi;
      const Sample sc = sample(branch, hi);
      const float3 dir = side_dir(sc.tangent, frame_at(hi, sc.tangent), j);
      const OnParent at_corner = on_parent(sc.position + dir * sc.radius);
      line.corner = at_corner.point;

      line.t_end = std::min(hi + std::max(branch.weld_length, 0.05f) * sc.radius / length,
                            0.75f);
      const Sample se = sample(branch, line.t_end);
      const float3 dir_end = side_dir(se.tangent, frame_at(line.t_end, se.tangent), j);
      const float theta = 2.0f * float(M_PI) * float(j) / float(sides);
      const float u_end = branch.t0 + (branch.t1 - branch.t0) * line.t_end;
      const float shape = bark_shape(
          branch, theta, u_end, se.position + dir_end * se.radius, se.radius);
      line.end = se.position + dir_end * (se.radius * shape);
      line.end_normal = dir_end;

      /* Spreads further toward the parent's tip or its base. */
      const float side = math::dot(dir, at_corner.tangent);
      const float spread_side = side > 0.0f ? 1.0f + (branch.weld_upper - 1.0f) * side :
                                              1.0f + (branch.weld_lower - 1.0f) * -side;
      const float spread = std::min(branch.weld * sc.radius * std::max(spread_side, 0.0f),
                                    at_corner.radius * 1.5f);
      line.base = line.corner;
      line.base_normal = at_corner.surface_normal;
      const float3 over = dir - at_corner.normal * math::dot(dir, at_corner.normal);
      if (spread > 0.0f && math::length_squared(over) > 1e-6f) {
        const OnParent at_base = on_parent(line.corner + math::normalize(over) * spread);
        line.base = at_base.point;
        line.base_normal = at_base.surface_normal;
      }
      line.spread = math::distance(line.base, line.corner);
      weld_t_end = std::max(weld_t_end, line.t_end);
      lines.append(line);
    }
    /* Too short to weld, or not coming out of the parent. */
    if (!ok || weld_t_end > (float(last_k) - 0.5f) / float(segments)) {
      lines.clear();
    }

    if (!lines.is_empty()) {
      /* Fewer when the detail is cut down, never less than the two ends. */
      const int weld_rings = std::max(
          int(std::lround(float(std::max(branch.weld_rings, 2)) * std::max(detail, 0.34f))), 2);
      for (const int w : IndexRange(weld_rings)) {
        const float f = float(w) / float(weld_rings - 1);
        float t_mean = 0.0f;
        for (const WeldLine &line : lines) {
          t_mean += line.t_corner + f * f * (line.t_end - line.t_corner);
        }
        t_mean /= float(sides);
        const Sample s = sample(branch, t_mean);
        add_ring(t_mean, s.position, frame_at(t_mean, s.tangent));
        for (const WeldLine &line : lines) {
          /* A curve from the parent's surface over the corner into the side of the branch,
           * flat on both. */
          float3 p = (1.0f - f) * (1.0f - f) * line.base + 2.0f * f * (1.0f - f) * line.corner +
                     f * f * line.end;
          if (w > 0 && w < weld_rings - 1) {
            const OnParent o = on_parent(p);
            if (o.out < o.radius) {
              p = o.point;
            }
          }
          r.positions.append(p);
          const float3 n = math::interpolate(line.base_normal, line.end_normal, f);
          r.normals.append(math::length_squared(n) > 1e-8f ? math::normalize(n) :
                                                             line.end_normal);
          r.weld.append(1.0f - f);
          r.vs.append(v_at(line.t_corner) -
                      (1.0f - f) * (1.0f - f) * line.spread * r.v_scale +
                      f * f * (line.t_end - line.t_corner) * length * r.v_scale);
        }
      }
    }
  }
  const int weld_ring_count = r.count();

  /* Trunks without a ground object stand on the plane through their base. */
  const bool flat_foot = !from && branch.parent < 0 && branch.t0 <= 0.0f &&
                         branch.level != nullptr && branch.level->type == LevelType::Trunk &&
                         branch.level->ground == nullptr && branch.points.size() > 1 &&
                         sample(branch, 0.0f).tangent.z > 0.2f;
  const float foot_z = branch.points.is_empty() ? 0.0f : branch.points[0].z;

  for (const int k : IndexRange(last_k + 1)) {
    const float t = float(k) / float(segments);
    if (!lines.is_empty() && t <= weld_t_end + 0.3f / float(segments)) {
      continue;
    }
    const Sample s = sample(branch, t);
    /* A forked stem starts on the last ring of the stem it goes on from. */
    const bool first_of_fork = from && k == 0;
    const float3 tangent = first_of_fork ? from->tangent : s.tangent;
    const float3 normal = first_of_fork ? from->normal : frame_at(t, s.tangent);
    add_ring(t, s.position, normal);
    const float u = branch.t0 + (branch.t1 - branch.t0) * t;
    for (const int j : IndexRange(sides)) {
      const float theta = 2.0f * float(M_PI) * float(j) / float(sides);
      const float3 dir = side_dir(tangent, normal, j);
      const float shape = bark_shape(branch, theta, u, s.position + dir * s.radius, s.radius);
      float3 position = s.position + dir * (s.radius * shape);
      /* A leaning trunk stands flat on the ground: its foot, and anything that would go below
       * it, slides along the trunk down or up to the ground. */
      if (flat_foot && tangent.z > 0.2f && (k == 0 || position.z < foot_z)) {
        position -= tangent * ((position.z - foot_z) / tangent.z);
      }
      r.positions.append(position);
      r.normals.append(dir);
      r.weld.append(0.0f);
      r.vs.append(v_at(t));
    }
  }

  if (weld_ring_count > 0) {
    /* The weld shades from its own shape, except where it lies on the parent. */
    ring_normals(r, 1, weld_ring_count - 1);
  }
  if (has_bark_shape(branch)) {
    ring_normals(r, std::max(weld_ring_count - 1, from ? 1 : 0), r.count());
  }

  /* The base takes the parent's normals and turns to its own over the blend length, so it shades
   * as if it grew out of the parent instead of being stuck into it. With a weld, the part lying on
   * the parent shades as the parent, and the curve up into the branch turns over the same length. */
  if (!from && branch.parent >= 0 && tree.branches[branch.parent].mesh &&
      branch.level != nullptr && branch.level->parent_normals)
  {
    const Branch &parent = tree.branches[branch.parent];
    const float parent_length = std::max(parent.length, 1e-6f);
    const float blend = std::max(branch.level->normal_blend.at(branch.context), 0.05f) *
                        std::max(branch.base_radius, 1e-6f);
    for (const int i : IndexRange(r.count())) {
      bool any = false;
      for (const int j : IndexRange(sides)) {
        const float3 p = r.positions[r.index(i, j)];
        float t = branch.parent_t;
        Sample s = sample(parent, t);
        for ([[maybe_unused]] const int step : IndexRange(3)) {
          t = std::clamp(t + math::dot(p - s.position, s.tangent) / parent_length, 0.0f, 1.0f);
          s = sample(parent, t);
        }
        const float3 rel = p - s.position;
        const float3 radial = rel - s.tangent * math::dot(rel, s.tangent);
        const float out = math::length(radial);
        if (out < 1e-8f) {
          continue;
        }
        const float f = std::clamp((out - s.radius) / blend, 0.0f, 1.0f);
        if (f >= 1.0f) {
          continue;
        }
        any = true;
        const float w = f * f * (3.0f - 2.0f * f);
        float3 &n = r.normals[r.index(i, j)];
        const float3 mixed = math::interpolate(radial / out, n, w);
        if (math::length_squared(mixed) > 1e-8f) {
          n = math::normalize(mixed);
        }
      }
      if (!any && i > 0) {
        break;
      }
    }
  }
  return r;
}

/** Adds the mesh of one branch. */
static void mesh_branch(MeshBuilder &m,
                        const Branch &branch,
                        const BranchRings &r,
                        const float3 &offset)
{
  const int sides = r.sides;
  const int count = r.count();
  const int first = m.positions.size();
  const int material = branch.material;
  const float u_tiles = float(std::max(branch.u_tiles, 1));
  Vector<float3> vert_normals;

  /* Texture across: moved, turned along the branch and mirrored on some branches. */
  const Level *level = branch.level;
  const float u_offset = level ? level->u_offset : 0.0f;
  const float uv_twist = level ? level->uv_twist : 0.0f;
  const float u_flip = (level && level->random_flip &&
                        noise::hash_to_float(uint32_t(branch.random * 16777216.0f), 0xf1a9u) < 0.5f) ?
                           -1.0f :
                           1.0f;
  const auto u_at = [&](const float u, const float t) {
    return u_flip * (u + u_offset + uv_twist * u_tiles * t);
  };

  m.fill();
  m.weld.resize(first, 0.0f);
  /* Flat shading: every corner of a face takes the face's own normal. */
  const int first_corner = m.corner_verts.size();
  const int first_face = m.face_sizes.size();
  BLI_SCOPED_DEFER([&]() {
    if (level == nullptr || level->smooth) {
      return;
    }
    int corner = first_corner;
    for (const int f : IndexRange(first_face, m.face_sizes.size() - first_face)) {
      const int size = m.face_sizes[f];
      float3 n(0.0f);
      for (const int k : IndexRange(size)) {
        const float3 &a = m.positions[m.corner_verts[corner + k]];
        const float3 &b = m.positions[m.corner_verts[corner + (k + 1) % size]];
        n += math::cross(a, b);
      }
      if (math::length_squared(n) > 1e-20f) {
        n = math::normalize(n);
        for (const int k : IndexRange(size)) {
          m.normals[corner + k] = n;
        }
      }
      corner += size;
    }
  });
  m.current = VertInfo();
  m.current.branch_random = branch.random;
  m.current.level = level ? int(level->type) : 0;
  m.current.depth = branch.depth;
  for (const int i : IndexRange(count)) {
    VertInfo ring_info = m.current;
    ring_info.along = branch.t0 + (branch.t1 - branch.t0) * r.ts[i];
    ring_info.radius = r.radii[i];
    for (const int j : IndexRange(sides)) {
      m.positions.append(r.position(i, j) + offset);
      vert_normals.append(r.normals[r.index(i, j)]);
      const float weld = r.weld[r.index(i, j)];
      m.weld.append(weld);
      m.any_weld |= weld > 0.0f;
      m.info.append(ring_info);
    }
  }
  m.current.along = branch.t1;
  m.current.radius = r.radii.is_empty() ? 0.0f : r.radii.last();
  BLI_SCOPED_DEFER([&]() { m.fill(); });
  const Sample &tip = r.tip;
  int tip_vert = -1;
  if (!branch.cut && !branch.open_end) {
    tip_vert = m.positions.size();
    m.positions.append(tip.position + offset);
    vert_normals.append(tip.tangent);
  }

  const float tip_v = branch.length * r.v_scale + r.v_offset;
  const auto vert = [&](const int i, const int j) { return first + i * sides + j % sides; };
  const auto add = [&](const int v, const float2 &uv) {
    m.corner(v, uv, vert_normals[v - first]);
  };
  const int last = count - 1;

  if (sides == 2) {
    for (const int i : IndexRange(last)) {
      add(vert(i, 0), float2(0.0f, r.v(i, 0)));
      add(vert(i, 1), float2(1.0f, r.v(i, 1)));
      add(vert(i + 1, 1), float2(1.0f, r.v(i + 1, 1)));
      add(vert(i + 1, 0), float2(0.0f, r.v(i + 1, 0)));
      m.face(4, material);
    }
    if (tip_vert >= 0) {
      add(vert(last, 0), float2(0.0f, r.v(last, 0)));
      add(vert(last, 1), float2(1.0f, r.v(last, 1)));
      add(tip_vert, float2(0.5f, tip_v));
      m.face(3, material);
    }
    return;
  }

  for (const int i : IndexRange(last)) {
    for (const int j : IndexRange(sides)) {
      const float u0 = float(j) / float(sides) * u_tiles;
      const float u1 = float(j + 1) / float(sides) * u_tiles;
      const float t0 = r.ts[i];
      const float t1 = r.ts[i + 1];
      add(vert(i, j), float2(u_at(u0, t0), r.v(i, j)));
      add(vert(i, j + 1), float2(u_at(u1, t0), r.v(i, j + 1)));
      add(vert(i + 1, j + 1), float2(u_at(u1, t1), r.v(i + 1, j + 1)));
      add(vert(i + 1, j), float2(u_at(u0, t1), r.v(i + 1, j)));
      m.face(4, material);
    }
  }
  const auto disc_uv = [&](const int j) {
    const float theta = 2.0f * float(M_PI) * float(j) / float(sides);
    return float2(0.5f + 0.5f * std::cos(theta), 0.5f + 0.5f * std::sin(theta));
  };
  if (branch.open_end) {
    /* Where it forks: the stems going on start on this ring, close it flat behind them. */
    for (const int j : IndexRange(sides)) {
      m.corner(vert(last, j), disc_uv(j), tip.tangent);
    }
    m.face(sides, material);
    return;
  }
  if (branch.cut) {
    /* The bark rolls over the edge onto a ring set in from the side, the wood inside rises to a
     * low dome. The wood is a disc in the middle of the texture. */
    const float3 last_normal = r.frames.last();
    const float3 last_binormal = math::cross(tip.tangent, last_normal);
    const float rad = r.radii.last();
    const int rim = m.positions.size();
    for (const int j : IndexRange(sides)) {
      const float theta = 2.0f * float(M_PI) * float(j) / float(sides);
      const float3 dir = last_normal * std::cos(theta) + last_binormal * std::sin(theta);
      m.positions.append(tip.position + dir * (rad * 0.82f) + tip.tangent * (rad * 0.07f) +
                         offset);
      vert_normals.append(math::normalize(tip.tangent + dir * 0.6f));
    }
    const int center = m.positions.size();
    m.positions.append(tip.position + tip.tangent * (rad * 0.16f) + offset);
    vert_normals.append(tip.tangent);

    const float lip_v = r.v(last, 0) + rad * 0.25f * r.v_scale;
    for (const int j : IndexRange(sides)) {
      const float u0 = float(j) / float(sides) * u_tiles;
      const float u1 = float(j + 1) / float(sides) * u_tiles;
      add(vert(last, j), float2(u_at(u0, 1.0f), r.v(last, 0)));
      add(vert(last, j + 1), float2(u_at(u1, 1.0f), r.v(last, 0)));
      add(rim + (j + 1) % sides, float2(u_at(u1, 1.0f), lip_v));
      add(rim + j, float2(u_at(u0, 1.0f), lip_v));
      m.face(4, material);
    }
    /* The wood tilts out a little toward its edge, so the dome shades round. */
    const auto wood_normal = [&](const int v) {
      const float3 out = (m.positions[v] - offset - tip.position) / std::max(rad, 1e-6f);
      return math::normalize(tip.tangent + out * 0.25f);
    };
    for (const int j : IndexRange(sides)) {
      const int j1 = (j + 1) % sides;
      m.corner(rim + j, disc_uv(j), wood_normal(rim + j));
      m.corner(rim + j1, disc_uv(j + 1), wood_normal(rim + j1));
      m.corner(center, float2(0.5f), tip.tangent);
      m.face(3, material);
    }
    return;
  }
  for (const int j : IndexRange(sides)) {
    const float u0 = float(j) / float(sides) * u_tiles;
    const float u1 = float(j + 1) / float(sides) * u_tiles;
    add(vert(last, j), float2(u_at(u0, 1.0f), r.v(last, 0)));
    add(vert(last, j + 1), float2(u_at(u1, 1.0f), r.v(last, 0)));
    add(tip_vert, float2(u_at((u0 + u1) * 0.5f, 1.0f), tip_v));
    m.face(3, material);
  }
}

/** Meshes all the branches of a tree. */
static void mesh_branches(MeshBuilder &m,
                          const Tree &tree,
                          const float detail,
                          const float3 &offset)
{
  /* The last rings of forking stems, the stems going on from them start there. */
  Array<std::optional<RingStart>> ends(tree.branches.size());
  for (const int b : tree.branches.index_range()) {
    const Branch &branch = tree.branches[b];
    if (!branch.mesh) {
      continue;
    }
    const RingStart *from = nullptr;
    if (branch.fork_parent >= 0 && branch.fork_parent < b && ends[branch.fork_parent]) {
      from = &*ends[branch.fork_parent];
    }
    const BranchRings r = branch_rings(tree, branch, b, detail, from);
    if (branch.open_end && r.sides > 2) {
      RingStart end;
      end.sides = r.sides;
      end.normal = r.frames.last();
      end.tangent = r.tip.tangent;
      end.v_scale = r.v_scale;
      end.v = branch.length * r.v_scale + r.v_offset;
      ends[b] = end;
    }
    mesh_branch(m, branch, r, offset);
  }
}

/** Outline of a leaf card, x across from -0.5 to 0.5 and y along from 0 to 1, counter-clockwise
 * seen from the side it faces. */
static Span<float2> leaf_outline(const LeafShape shape)
{
  static const float2 leaf[] = {{0.0f, 0.0f},
                                {0.5f, 0.3f},
                                {0.35f, 0.75f},
                                {0.0f, 1.0f},
                                {-0.35f, 0.75f},
                                {-0.5f, 0.3f}};
  static const float2 square[] = {{-0.5f, 0.0f}, {0.5f, 0.0f}, {0.5f, 1.0f}, {-0.5f, 1.0f}};
  if (shape == LeafShape::Leaf) {
    return Span<float2>(leaf, 6);
  }
  return Span<float2>(square, 4);
}

/** The middle and size of all the leaves and fronds of one tree, for their normals. */
struct Foliage {
  float3 center = float3(0.0f);
  /** Half the size of the box around the foliage. */
  float3 extent = float3(1.0f);
  /** The hidden clumps of the tree, for the Clumps normals. */
  const ClumpField *clumps = nullptr;
};

/** The clump values of a point of the foliage. */
static void clump_info(VertInfo &info, const float3 &position, const Foliage &foliage)
{
  if (foliage.clumps == nullptr) {
    return;
  }
  float value;
  float3 gradient;
  float random;
  float edge;
  if (foliage.clumps->eval(position, value, gradient, random, edge)) {
    info.clump_random = random;
    info.clump_depth = std::clamp(-value, 0.0f, 1.0f);
    info.clump_edge = edge;
  }
}

static float3 soften_normal(const float3 &normal,
                            const float3 &position,
                            const Foliage &foliage,
                            const NormalMode mode,
                            const float amount)
{
  if (amount <= 0.0f || mode == NormalMode::Flat) {
    return normal;
  }
  float3 target;
  switch (mode) {
    case NormalMode::Up:
      target = float3(0.0f, 0.0f, 1.0f);
      break;
    case NormalMode::Clumps: {
      float value;
      float random;
      float edge;
      if (foliage.clumps == nullptr ||
          !foliage.clumps->eval(position, value, target, random, edge))
      {
        target = position - foliage.center;
      }
      break;
    }
    case NormalMode::Crown: {
      /* The normal of the ellipsoid through the point, scaled to the foliage. */
      const float3 e = math::max(foliage.extent, float3(1e-4f));
      target = (position - foliage.center) / (e * e);
      break;
    }
    default:
      target = position - foliage.center;
      break;
  }
  if (math::length_squared(target) < 1e-12f) {
    return normal;
  }
  const float3 blended = math::interpolate(normal, math::normalize(target), amount);
  return math::length_squared(blended) > 1e-8f ? math::normalize(blended) : normal;
}

/** The width of a leaf card along it, from its outline. */
static float leaf_width_at(const LeafShape shape, const float y)
{
  if (shape != LeafShape::Leaf) {
    return 0.5f;
  }
  const Span<float2> outline = leaf_outline(shape);
  /* The right side of the outline, from base to tip. */
  for (const int i : IndexRange(3)) {
    const float2 a = outline[i];
    const float2 b = outline[i + 1];
    if (y <= b.y) {
      const float f = (y - a.y) / std::max(b.y - a.y, 1e-6f);
      return math::interpolate(a.x, b.x, std::clamp(f, 0.0f, 1.0f));
    }
  }
  return 0.0f;
}

static void mesh_leaf(MeshBuilder &m,
                      const Leaf &leaf,
                      const float size_scale,
                      const Foliage &foliage,
                      const float3 &offset,
                      const Map<const Mesh *, std::unique_ptr<LeafMeshData>> &leaf_meshes)
{
  const float3 base_across = math::normalize(math::cross(leaf.dir, leaf.normal));
  const float length = (leaf.shape == LeafShape::Mesh && leaf.real_size ? leaf.scale :
                                                                          leaf.length) *
                       size_scale * leaf.stretch.y;
  const float width = leaf.width * size_scale * leaf.stretch.x;
  const float cos_fold = std::cos(leaf.fold);
  const float sin_fold = std::sin(leaf.fold);
  const bool deforms = leaf.bend != 0.0f || leaf.curl != 0.0f || leaf.twist != 0.0f ||
                       leaf.noise != 0.0f;
  const float noise_size = std::max(leaf.noise_scale * leaf.scale * 0.25f, 1e-5f);
  const float3 noise_seed = seed_offset(int(leaf.random * 9973.0f));

  m.fill();
  m.current = VertInfo();
  m.current.branch_random = leaf.branch_random;
  m.current.leaf_random = leaf.random;
  m.current.level = int(LevelType::Leaves);
  m.current.depth = leaf.depth;
  m.current.along = 1.0f;
  {
    const float3 out = leaf.position - foliage.center;
    m.current.facing = math::length_squared(out) > 1e-12f ?
                           math::dot(leaf.normal, math::normalize(out)) * 0.5f + 0.5f :
                           0.5f;
  }
  clump_info(m.current, leaf.position, foliage);

  /* Each card once, or twice at a right angle. */
  for (const int card : IndexRange(leaf.cross ? 2 : 1)) {
    const float3 across = card == 0 ? base_across : leaf.normal;
    const float3 normal = card == 0 ? leaf.normal : -base_across;
    /* A point of the leaf: x across and z out in the tree's units, y along from 0 to 1. */
    const auto point = [&](float x, const float y, const float z) {
      if (leaf.flip) {
        x = -x;
      }
      float3 side = across * x + normal * z;
      if (leaf.curl != 0.0f && width > 0.0f) {
        const float c = 2.0f * x / width;
        side += normal * (leaf.curl * c * c * width * 0.5f);
      }
      if (leaf.twist != 0.0f) {
        side = rotate_around(side, leaf.dir, leaf.twist * y);
      }
      float3 p = leaf.position + leaf.dir * (y * length) + side;
      if (leaf.bend != 0.0f) {
        p += normal * (leaf.bend * y * y * length);
      }
      if (leaf.noise != 0.0f) {
        p += normal * (leaf.noise * length * noise::perlin_signed(p / noise_size + noise_seed));
      }
      return p;
    };
    const auto normal_at = [&](const float x, const float y, const float z, const float3 &flat) {
      if (!deforms) {
        return flat;
      }
      const float ex = std::max(width, 1e-5f) * 0.05f;
      const float3 dx = point(x + ex, y, z) - point(x - ex, y, z);
      const float3 dy = point(x, std::min(y + 0.02f, 1.0f), z) - point(x, std::max(y - 0.02f, 0.0f), z);
      float3 n = math::cross(dx, dy);
      if (leaf.flip) {
        n = -n;
      }
      if (math::length_squared(n) < 1e-16f) {
        return flat;
      }
      n = math::normalize(n);
      return math::dot(n, flat) < 0.0f ? -n : n;
    };
    const auto soft = [&](const float3 &n, const float3 &position) {
      return soften_normal(n, position, foliage, leaf.normal_mode, leaf.soft_normals);
    };

    if (leaf.shape == LeafShape::Mesh) {
      const LeafMeshData &data = *leaf_meshes.lookup(leaf.mesh);
      const float mesh_size = leaf.real_size ? leaf.scale * size_scale : leaf.length * size_scale;
      const int first = m.positions.size();
      for (const float3 &p : data.positions) {
        const float x = p.x * mesh_size * leaf.stretch.x;
        const float y = p.y * mesh_size * leaf.stretch.y / std::max(length, 1e-6f);
        const float z = p.z * mesh_size * leaf.stretch.z;
        m.positions.append(point(x, y, z) + offset);
      }
      const float3 inv_stretch = float3(1.0f) / math::max(leaf.stretch, float3(1e-4f));
      for (const int face : data.faces.index_range()) {
        const IndexRange corners = data.faces[face];
        const auto add_corner = [&](const int corner) {
          const int vert = data.corner_verts[corner];
          const float3 n = data.corner_normals[corner] * inv_stretch;
          float3 normal_world = across * (leaf.flip ? -n.x : n.x) + leaf.dir * n.y + normal * n.z;
          normal_world = math::length_squared(normal_world) > 1e-12f ?
                             math::normalize(normal_world) :
                             normal;
          if (leaf.twist != 0.0f) {
            const float y = data.positions[vert].y * mesh_size * leaf.stretch.y /
                            std::max(length, 1e-6f);
            normal_world = rotate_around(normal_world, leaf.dir, leaf.twist * y);
          }
          const float3 &p = data.positions[vert];
          const float2 uv = data.uvs.is_empty() ? float2(p.x + 0.5f, p.y) : data.uvs[corner];
          m.corner(first + vert, uv, soft(normal_world, m.positions[first + vert] - offset));
        };
        /* Mirrored leaves keep facing out by going round the other way. */
        if (leaf.flip) {
          for (int c = int(corners.last()); c >= int(corners.first()); c--) {
            add_corner(c);
          }
        }
        else {
          for (const int corner : corners) {
            add_corner(corner);
          }
        }
        const int own = data.materials.is_empty() ? 0 : data.materials[face];
        m.face(corners.size(), leaf.material + own);
      }
      continue;
    }

    const int rows = std::max(leaf.detail, 1);
    if (rows == 1) {
      /* The outline as one face. */
      const Span<float2> outline = leaf_outline(leaf.shape);
      const int count = outline.size();
      for (const int k : IndexRange(count)) {
        const float2 &p = outline[leaf.flip ? (count - k) % count : k];
        const float x = p.x * width * cos_fold;
        const float z = std::abs(p.x) * width * sin_fold;
        const float3 position = point(x, p.y, z);
        const int vert = m.positions.size();
        m.positions.append(position + offset);
        m.corner(vert, float2(p.x + 0.5f, p.y), soft(normal_at(x, p.y, z, normal), position));
      }
      m.face(count, leaf.material);
      continue;
    }

    /* Rows of three: left, middle and right. A leaf shape starts and ends in one point. */
    const bool pointed = leaf.shape == LeafShape::Leaf;
    const int first = m.positions.size();
    Vector<float2> uvs;
    Vector<float3> vert_normals;
    Vector<int> row_start;
    for (const int k : IndexRange(rows + 1)) {
      const float y = float(k) / float(rows);
      row_start.append(m.positions.size());
      const bool single = pointed && (k == 0 || k == rows);
      const float half = leaf_width_at(leaf.shape, y);
      for (const int c : IndexRange(single ? 1 : 3)) {
        const float px = single ? 0.0f : (float(c) - 1.0f) * half;
        const float x = px * width * cos_fold;
        const float z = std::abs(px) * width * sin_fold;
        const float3 position = point(x, y, z);
        m.positions.append(position + offset);
        uvs.append(float2(px + 0.5f, y));
        vert_normals.append(soft(normal_at(x, y, z, normal), position));
      }
    }
    const auto add = [&](const int vert) {
      m.corner(vert, uvs[vert - first], vert_normals[vert - first]);
    };
    const auto quad_or_tri = [&](const Span<int> verts) {
      if (leaf.flip) {
        for (int i = int(verts.size()) - 1; i >= 0; i--) {
          add(verts[i]);
        }
      }
      else {
        for (const int v : verts) {
          add(v);
        }
      }
      m.face(verts.size(), leaf.material);
    };
    for (const int k : IndexRange(rows)) {
      const int a = row_start[k];
      const int b = row_start[k + 1];
      const bool single_a = pointed && k == 0;
      const bool single_b = pointed && k + 1 == rows;
      for (const int c : IndexRange(2)) {
        if (single_a) {
          const int verts[3] = {a, b + c, b + c + 1};
          quad_or_tri(Span<int>(verts, 3));
        }
        else if (single_b) {
          const int verts[3] = {a + c, a + c + 1, b};
          quad_or_tri(Span<int>(verts, 3));
        }
        else {
          const int verts[4] = {a + c, a + c + 1, b + c + 1, b + c};
          quad_or_tri(Span<int>(verts, 4));
        }
      }
    }
  }
  m.fill();
}

static void mesh_frond(MeshBuilder &m,
                       const Tree &tree,
                       const Frond &frond,
                       const float detail,
                       const float scale,
                       const Foliage &foliage,
                       const float3 &offset)
{
  const Level &level = *frond.level;
  const Branch &branch = tree.branches[frond.branch];
  const int segments = frond_segments(frond, detail);
  const int across = frond_columns(frond);
  const int columns = across + 1;
  m.fill();
  const int first = m.positions.size();
  Vector<float3> vert_normals;
  m.current = VertInfo();
  m.current.branch_random = branch.random;
  m.current.leaf_random = noise::hash_to_float(frond.id);
  m.current.level = int(LevelType::Fronds);
  m.current.depth = branch.depth + 1;
  BLI_SCOPED_DEFER([&]() { m.fill(); });

  float lo;
  float hi;
  if (!piece_range(level, branch, lo, hi)) {
    return;
  }
  const float t0 = piece_t(branch, lo);
  const float t1 = std::max(piece_t(branch, hi), t0 + 1e-3f);
  const Sample s0 = sample(branch, t0);
  const float3 up_ref = math::length_squared(tree.plane) > 1e-12f ? math::normalize(tree.plane) :
                                                                    float3(0.0f, 0.0f, 1.0f);
  float3 up = up_ref - s0.tangent * math::dot(up_ref, s0.tangent);
  up = math::length_squared(up) > 1e-6f ? math::normalize(up) : any_perpendicular(s0.tangent);
  const float width_scale = std::max(
      1.0f + level.size_random.value * (noise::hash_to_float(frond.id, 0x3d1u) * 2.0f - 1.0f),
      0.05f);
  const float tip = std::clamp(level.frond_tip, 0.0f, 1.0f);

  for (const int k : IndexRange(segments + 1)) {
    const float u = float(k) / float(segments);
    const Sample s = sample(branch, t0 + (t1 - t0) * u);
    const float3 moved = up - s.tangent * math::dot(up, s.tangent);
    if (math::length_squared(moved) > 1e-8f) {
      up = math::normalize(moved);
    }
    FieldContext context = branch.context;
    context.along_parent = t0 + (t1 - t0) * u;
    context.along_branch = u;
    context.id = frond.id;
    context.random = noise::hash_to_float(frond.id);
    context.position = s.position + offset;
    const float roll = frond.roll + level.twist.at(context) * u;
    const float3 side = math::cross(s.tangent, up);
    const float3 across_dir = side * std::cos(roll) + up * std::sin(roll);
    const float3 plane_normal = up * std::cos(roll) - side * std::sin(roll);

    /* Narrower toward the tip, to a point or in a curve. */
    float narrow = 1.0f;
    if (tip > 0.0f && u > 1.0f - tip) {
      const float x = std::clamp((u - (1.0f - tip)) / tip, 0.0f, 1.0f);
      narrow = level.frond_round_tip ? std::sqrt(std::max(1.0f - x * x, 0.0f)) : 1.0f - x;
    }
    const float half = 0.5f * std::max(level.width.at(context), 0.0f) * scale * width_scale *
                       narrow;
    const float fold = level.fold.at(context);
    const float curl = level.curl.at(context);
    const float soft = level.soft_normals.at(context);

    for (const int c : IndexRange(columns)) {
      /* -1 to 1 across, the sides roll in on an arc, and a fold turns both halves up. */
      const float x = float(c) / float(across) * 2.0f - 1.0f;
      float a = x;
      float n = 0.0f;
      if (std::abs(curl) > 1e-4f) {
        a = std::sin(curl * x) / curl;
        n = (1.0f - std::cos(curl * x)) / curl;
      }
      n += std::abs(a) * std::sin(fold);
      a *= std::cos(fold);
      const float3 position = s.position + across_dir * (a * half) + plane_normal * (n * half);
      /* The normal of the curled surface at this column. */
      float3 surface_normal = plane_normal;
      const float slope = curl * x;
      if (std::abs(curl) > 1e-4f || fold != 0.0f) {
        const float side_sign = x < 0.0f ? -1.0f : 1.0f;
        const float angle = slope + (x == 0.0f ? 0.0f : side_sign * fold);
        surface_normal = plane_normal * std::cos(angle) - across_dir * std::sin(angle);
      }
      m.positions.append(position + offset);
      vert_normals.append(
          soften_normal(surface_normal, position, foliage, level.normal_mode, soft));
      VertInfo info = m.current;
      info.along = u;
      const float3 out = position - foliage.center;
      info.facing = math::length_squared(out) > 1e-12f ?
                        math::dot(plane_normal, math::normalize(out)) * 0.5f + 0.5f :
                        0.5f;
      clump_info(info, position, foliage);
      m.info.append(info);
    }
  }

  const auto add = [&](const int k, const int c) {
    const int vert = first + k * columns + c;
    m.corner(vert,
             float2(float(c) / float(across), float(k) / float(segments)),
             vert_normals[vert - first]);
  };
  for (const int k : IndexRange(segments)) {
    for (const int c : IndexRange(across)) {
      add(k, c);
      add(k, c + 1);
      add(k + 1, c + 1);
      add(k + 1, c);
      m.face(4, level.material);
    }
  }
}

/** How open each vertex is to the sky around it, 1 when nothing of the tree is in the way. */
/**
 * A shown clump: a smooth ellipsoid. Its normals come from the merged clumps, so clumps that touch
 * shade as one soft shape.
 */
static void mesh_clump(MeshBuilder &m,
                       const Clump &clump,
                       const float detail,
                       const Foliage &foliage,
                       const float3 &offset)
{
  const int2 grid = clump_grid(detail);
  const int sides = grid.x;
  const int rings = grid.y;
  m.fill();
  m.current = VertInfo();
  m.current.branch_random = clump.branch_random;
  m.current.leaf_random = clump.random;
  m.current.level = int(LevelType::Leaves);
  m.current.depth = clump.depth;
  BLI_SCOPED_DEFER([&]() { m.fill(); });
  const int first = m.positions.size();
  Vector<float3> normals;
  const auto add_vert = [&](const float3 &unit) {
    const float3 p = clump.center + float3(unit.x, unit.y, unit.z * clump.squash) * clump.radius;
    m.positions.append(p + offset);
    float3 normal = math::normalize(float3(unit.x, unit.y, unit.z / clump.squash));
    float value;
    float3 gradient;
    float random;
    float edge;
    if (foliage.clumps != nullptr && foliage.clumps->eval(p, value, gradient, random, edge)) {
      normal = gradient;
    }
    normals.append(normal);
    VertInfo info = m.current;
    clump_info(info, p, foliage);
    m.info.append(info);
  };
  /* The bottom, the rings from the bottom up, then the top. */
  add_vert(float3(0.0f, 0.0f, -1.0f));
  for (const int r : IndexRange(1, rings - 1)) {
    const float phi = float(M_PI) * (float(r) / float(rings) - 0.5f);
    for (const int j : IndexRange(sides)) {
      const float a = 2.0f * float(M_PI) * float(j) / float(sides);
      add_vert(float3(std::cos(phi) * std::cos(a), std::cos(phi) * std::sin(a), std::sin(phi)));
    }
  }
  add_vert(float3(0.0f, 0.0f, 1.0f));
  const int bottom = first;
  const int top = first + 1 + (rings - 1) * sides;
  const auto vert = [&](const int r, const int j) { return first + 1 + (r - 1) * sides + j % sides; };
  const auto add = [&](const int v, const float2 &uv) { m.corner(v, uv, normals[v - first]); };
  for (const int j : IndexRange(sides)) {
    const float u0 = float(j) / float(sides);
    const float u1 = float(j + 1) / float(sides);
    add(bottom, float2((u0 + u1) * 0.5f, 0.0f));
    add(vert(1, j + 1), float2(u1, 1.0f / float(rings)));
    add(vert(1, j), float2(u0, 1.0f / float(rings)));
    m.face(3, clump.material);
    for (const int r : IndexRange(1, rings - 2)) {
      const float v0 = float(r) / float(rings);
      const float v1 = float(r + 1) / float(rings);
      add(vert(r, j), float2(u0, v0));
      add(vert(r, j + 1), float2(u1, v0));
      add(vert(r + 1, j + 1), float2(u1, v1));
      add(vert(r + 1, j), float2(u0, v1));
      m.face(4, clump.material);
    }
    const float v_last = float(rings - 1) / float(rings);
    add(vert(rings - 1, j), float2(u0, v_last));
    add(vert(rings - 1, j + 1), float2(u1, v_last));
    add(top, float2((u0 + u1) * 0.5f, 1.0f));
    m.face(3, clump.material);
  }
}

static Array<float> vertex_occlusion(const Mesh &mesh, const int rays)
{
  const Span<float3> positions = mesh.vert_positions();
  const Span<float3> normals = mesh.vert_normals();
  Array<float> open(positions.size(), 1.0f);
  const bke::BVHTreeFromMesh bvh = mesh.bvh_corner_tris();
  if (bvh.tree == nullptr || rays <= 0) {
    return open;
  }
  /* Directions over the sphere, spread evenly by the golden angle. */
  Array<float3> dirs(rays);
  for (const int i : IndexRange(rays)) {
    const float z = 1.0f - (float(i) + 0.5f) / float(rays) * 2.0f;
    const float r = std::sqrt(std::max(1.0f - z * z, 0.0f));
    const float a = float(i) * 2.39996323f;
    dirs[i] = float3(r * std::cos(a), r * std::sin(a), z);
  }
  Bounds<float3> bounds = *bounds::min_max(positions);
  const float reach = math::distance(bounds.min, bounds.max) * 0.25f;
  const float lift = std::max(reach * 1e-3f, 1e-5f);
  threading::parallel_for(positions.index_range(), 256, [&](const IndexRange range) {
    for (const int v : range) {
      const float3 n = normals[v];
      int hits = 0;
      int cast = 0;
      for (const float3 &d0 : dirs) {
        /* Only the half facing out of the surface. */
        const float3 d = math::dot(d0, n) < 0.0f ? -d0 : d0;
        BVHTreeRayHit hit;
        hit.index = -1;
        hit.dist = reach;
        BLI_bvhtree_ray_cast(bvh.tree,
                             positions[v] + n * lift,
                             d,
                             0.0f,
                             &hit,
                             bvh.raycast_callback,
                             const_cast<bke::BVHTreeFromMesh *>(&bvh));
        hits += hit.index != -1;
        cast++;
      }
      open[v] = cast > 0 ? 1.0f - float(hits) / float(cast) : 1.0f;
    }
  });
  return open;
}

static Mesh *build_mesh(MeshBuilder &m, const VertexData &data)
{
  Mesh *mesh = BKE_mesh_new_nomain(
      m.positions.size(), 0, m.face_sizes.size(), m.corner_verts.size());
  mesh->vert_positions_for_write().copy_from(m.positions);
  MutableSpan<int> offsets = mesh->face_offsets_for_write();
  int offset = 0;
  for (const int face : m.face_sizes.index_range()) {
    offsets[face] = offset;
    offset += m.face_sizes[face];
  }
  offsets.last() = offset;
  mesh->corner_verts_for_write().copy_from(m.corner_verts);
  bke::mesh_calc_edges(*mesh, false, false);

  bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
  bke::SpanAttributeWriter<float2> uv = attributes.lookup_or_add_for_write_only_span<float2>(
      "UVMap", bke::AttrDomain::Corner);
  uv.span.copy_from(m.uvs);
  uv.finish();
  mesh->uv_maps_active_set("UVMap");
  mesh->uv_maps_default_set("UVMap");

  if (m.any_material) {
    bke::SpanAttributeWriter<int> material = attributes.lookup_or_add_for_write_only_span<int>(
        "material_index", bke::AttrDomain::Face);
    material.span.copy_from(m.materials);
    material.finish();
  }

  if (m.any_weld) {
    /* How much of each vertex is in a weld, for materials to blend the parent's bark in. */
    m.weld.resize(m.positions.size(), 0.0f);
    bke::SpanAttributeWriter<float> weld = attributes.lookup_or_add_for_write_only_span<float>(
        "Weld", bke::AttrDomain::Point);
    weld.span.copy_from(m.weld);
    weld.finish();
  }

  /* Vertex data, each as its own attribute and some packed into one color. */
  const auto wants = [&](const VertexValue value) {
    if (data.write[int(value)]) {
      return true;
    }
    if (data.use_color) {
      for (const VertexValue c : data.color) {
        if (c == value) {
          return true;
        }
      }
    }
    return false;
  };
  bool any = false;
  for (const int i : IndexRange(1, vertex_value_count - 1)) {
    any |= wants(VertexValue(i));
  }
  if (any) {
    m.fill();
    m.weld.resize(m.positions.size(), 0.0f);
    const int verts = m.positions.size();
    Array<float> occlusion;
    if (wants(VertexValue::Occlusion)) {
      occlusion = vertex_occlusion(*mesh, std::clamp(data.rays, 1, 256));
    }
    /* Gradient Height runs from the lowest to the highest leaf or frond, else the whole mesh. */
    float foliage_low = FLT_MAX;
    float foliage_high = -FLT_MAX;
    for (const int v : IndexRange(verts)) {
      if (ELEM(m.info[v].level, int(LevelType::Leaves), int(LevelType::Fronds))) {
        foliage_low = std::min(foliage_low, m.positions[v].z);
        foliage_high = std::max(foliage_high, m.positions[v].z);
      }
    }
    if (foliage_low > foliage_high) {
      for (const int v : IndexRange(verts)) {
        foliage_low = std::min(foliage_low, m.positions[v].z);
        foliage_high = std::max(foliage_high, m.positions[v].z);
      }
    }
    const float foliage_span = std::max(foliage_high - foliage_low, 1e-6f);
    /* Raw values, the color gets them from 0 to 1. */
    const auto value = [&](const VertexValue kind, const int v) -> float {
      const VertInfo &info = m.info[v];
      switch (kind) {
        case VertexValue::None:
          return 0.0f;
        case VertexValue::BranchRandom:
          return info.branch_random;
        case VertexValue::LeafRandom:
          return info.leaf_random;
        case VertexValue::Level:
          return float(info.level);
        case VertexValue::Depth:
          return float(info.depth);
        case VertexValue::AlongBranch:
          return info.along;
        case VertexValue::Height:
          return info.height;
        case VertexValue::Radius:
          return info.radius;
        case VertexValue::Occlusion:
          return occlusion.is_empty() ? 1.0f : occlusion[v];
        case VertexValue::Weld:
          return m.weld[v];
        case VertexValue::LeafFacing:
          return info.facing;
        case VertexValue::ClumpRandom:
          return info.clump_random;
        case VertexValue::ClumpDepth:
          return info.clump_depth;
        case VertexValue::ClumpEdge:
          return info.clump_edge;
        case VertexValue::GradientHeight:
          return std::clamp((m.positions[v].z - foliage_low) / foliage_span, 0.0f, 1.0f);
      }
      return 0.0f;
    };
    static const char *names[vertex_value_count] = {"",
                                                    "Branch Random",
                                                    "Leaf Random",
                                                    "Level",
                                                    "Depth",
                                                    "Along Branch",
                                                    "Height",
                                                    "Radius",
                                                    "Occlusion",
                                                    "Weld",
                                                    "Leaf Facing",
                                                    "Clump Random",
                                                    "Clump Depth",
                                                    "Clump Edge",
                                                    "Gradient Height"};
    for (const int i : IndexRange(1, vertex_value_count - 1)) {
      const VertexValue kind = VertexValue(i);
      /* The weld is written below as before. */
      if (!data.write[i] || kind == VertexValue::Weld) {
        continue;
      }
      if (kind == VertexValue::Level || kind == VertexValue::Depth) {
        bke::SpanAttributeWriter<int> attr = attributes.lookup_or_add_for_write_only_span<int>(
            names[i], bke::AttrDomain::Point);
        for (const int v : IndexRange(verts)) {
          attr.span[v] = int(value(kind, v));
        }
        attr.finish();
        continue;
      }
      bke::SpanAttributeWriter<float> attr = attributes.lookup_or_add_for_write_only_span<float>(
          names[i], bke::AttrDomain::Point);
      for (const int v : IndexRange(verts)) {
        attr.span[v] = value(kind, v);
      }
      attr.finish();
    }
    if (data.write[int(VertexValue::Weld)] && !m.any_weld) {
      /* Asked for, so written even when there is no weld. */
      bke::SpanAttributeWriter<float> attr = attributes.lookup_or_add_for_write_only_span<float>(
          "Weld", bke::AttrDomain::Point);
      attr.span.fill(0.0f);
      attr.finish();
    }
    if (data.use_color) {
      float max_level = float(int(LevelType::Ivy));
      float max_depth = 1.0f;
      float max_height = 1e-6f;
      float max_radius = 1e-6f;
      for (const VertInfo &info : m.info) {
        max_depth = std::max(max_depth, float(info.depth));
        max_height = std::max(max_height, info.height);
        max_radius = std::max(max_radius, info.radius);
      }
      const auto normalized = [&](const VertexValue kind, const int v) {
        const float x = value(kind, v);
        switch (kind) {
          case VertexValue::Level:
            return x / max_level;
          case VertexValue::Depth:
            return x / max_depth;
          case VertexValue::Height:
            return x / max_height;
          case VertexValue::Radius:
            return x / max_radius;
          default:
            return x;
        }
      };
      bke::SpanAttributeWriter<ColorGeometry4f> color =
          attributes.lookup_or_add_for_write_only_span<ColorGeometry4f>("Tree Color",
                                                                         bke::AttrDomain::Point);
      for (const int v : IndexRange(verts)) {
        float4 c(0.0f, 0.0f, 0.0f, 1.0f);
        for (const int channel : IndexRange(4)) {
          const VertexValue kind = data.color[channel];
          if (kind != VertexValue::None) {
            c[channel] = std::clamp(normalized(kind, v), 0.0f, 1.0f);
          }
        }
        color.span[v] = ColorGeometry4f(c.x, c.y, c.z, c.w);
      }
      color.finish();
    }
  }

  bke::mesh_set_custom_normals_normalized(*mesh, m.normals);
  return mesh;
}

/** \} */

static Vector<Vector<PreparedForce>> prepare_forces(const TreeParams &params)
{
  const int levels_num = params.levels.size();
  /* An Avoid also keeps everything that grows on its level out of the mesh. */
  Array<int> parent(levels_num, -1);
  for (const int level_index : params.levels.index_range()) {
    for (const int child : params.levels[level_index].children) {
      if (child >= 0 && child < levels_num && parent[child] == -1 && child != level_index) {
        parent[child] = level_index;
      }
    }
  }
  Vector<Vector<PreparedForce>> forces(levels_num);
  for (const int level_index : params.levels.index_range()) {
    for (const Force &force : params.levels[level_index].forces) {
      PreparedForce prepared;
      prepared.force = &force;
      if (force.type == ForceType::Avoid) {
        if (force.mesh == nullptr || force.mesh->faces_num == 0) {
          continue;
        }
        prepare_surface(prepared, *force.mesh, force.mesh_transform);
      }
      forces[level_index].append(std::move(prepared));
    }
    Set<const Force *> seen;
    for (const Force &force : params.levels[level_index].forces) {
      seen.add(&force);
    }
    int up = parent[level_index];
    for (int guard = 0; up != -1 && guard < levels_num; guard++, up = parent[up]) {
      for (const Force &force : params.levels[up].forces) {
        if (force.type != ForceType::Avoid || force.mesh == nullptr ||
            force.mesh->faces_num == 0 || !seen.add(&force))
        {
          continue;
        }
        PreparedForce prepared;
        prepared.force = &force;
        prepare_surface(prepared, *force.mesh, force.mesh_transform);
        forces[level_index].append(std::move(prepared));
      }
    }
  }
  return forces;
}

static Array<PreparedForce> prepare_grounds(const TreeParams &params)
{
  Array<PreparedForce> grounds(params.levels.size());
  for (const int level_index : params.levels.index_range()) {
    const Level &level = params.levels[level_index];
    if (level.ground != nullptr && level.ground->faces_num > 0) {
      prepare_surface(grounds[level_index], *level.ground, level.ground_transform);
    }
  }
  return grounds;
}

static Map<const Mesh *, std::unique_ptr<LeafMeshData>> prepare_leaf_meshes(
    const TreeParams &params)
{
  Map<const Mesh *, std::unique_ptr<LeafMeshData>> meshes;
  for (const Level &level : params.levels) {
    if (level.shape != LeafShape::Mesh) {
      continue;
    }
    Vector<const Mesh *> level_meshes;
    level_meshes.append(level.leaf_mesh);
    for (const LeafChoice &choice : level.leaf_choices) {
      level_meshes.append(choice.mesh);
    }
    for (const Mesh *mesh : level_meshes) {
    if (mesh == nullptr || meshes.contains(mesh)) {
      continue;
    }
    std::unique_ptr<LeafMeshData> data = std::make_unique<LeafMeshData>();
    data->positions = mesh->vert_positions();
    data->faces = mesh->faces();
    data->corner_verts = mesh->corner_verts();
    data->corner_normals = mesh->corner_normals();
    if (const VArray<int> materials = *mesh->attributes().lookup<int>("material_index",
                                                                     bke::AttrDomain::Face))
    {
      data->materials = VArraySpan<int>(materials);
    }
    const StringRef uv_name = mesh->active_uv_map_name();
    if (!uv_name.is_empty()) {
      const VArray<float2> uvs = *mesh->attributes().lookup<float2>(uv_name,
                                                                    bke::AttrDomain::Corner);
      if (uvs) {
        data->uvs = VArraySpan<float2>(uvs);
      }
    }
    meshes.add(mesh, std::move(data));
    }
  }
  return meshes;
}

Mesh *mesh_trees(const TreeParams &params,
                 const Span<float3> positions,
                 int *r_triangles,
                 const FunctionRef<bool()> stop)
{
  if (r_triangles) {
    *r_triangles = 0;
  }
  if (params.roots.is_empty()) {
    return nullptr;
  }
  const bool at_origin = positions.is_empty();
  const int tree_count = at_origin ? 1 : positions.size();
  const Vector<Vector<PreparedForce>> forces = prepare_forces(params);
  const Array<PreparedForce> grounds = prepare_grounds(params);
  const Map<const Mesh *, std::unique_ptr<LeafMeshData>> leaf_meshes = prepare_leaf_meshes(params);

  MeshBuilder m;
  for (const int tree_i : IndexRange(tree_count)) {
    if (stop && stop()) {
      return nullptr;
    }
    const uint32_t seed = at_origin ? noise::hash(uint32_t(params.seed)) :
                                      noise::hash(uint32_t(params.seed), uint32_t(tree_i));
    const float3 offset = at_origin ? float3(0.0f) : positions[tree_i];
    Tree tree;
    Grower g{params, seed, tree, offset, forces, false, grounds};
    for (const int root : params.roots) {
      grow_level(g, root, {}, 0);
    }
    if (stop && stop()) {
      return nullptr;
    }

    /* The tree's own detail, or less when the triangle budget needs it. */
    const float detail = std::min(choose_detail(tree, params.triangle_budget),
                                  std::clamp(params.detail, 0.02f, 1.0f));
    if (tree_i == 0 && r_triangles) {
      *r_triangles = int(std::min<int64_t>(tree_triangles(tree, detail), INT32_MAX));
    }

    const int tree_first_vert = m.positions.size();
    /* The height of each vertex above the foot of its tree, once the tree is meshed. */
    BLI_SCOPED_DEFER([&]() {
      m.fill();
      for (const int v : IndexRange(tree_first_vert, m.positions.size() - tree_first_vert)) {
        m.info[v].height = std::max(m.positions[v].z - offset.z, 0.0f);
      }
    });
    mesh_branches(m, tree, detail, offset);
    if (tree.leaves.is_empty() && tree.fronds.is_empty()) {
      continue;
    }
    /* Soft normals point out from the middle of the foliage, or from a box fitted around it. */
    float3 bounds_min(FLT_MAX);
    float3 bounds_max(-FLT_MAX);
    float3 sum(0.0f);
    int count = 0;
    const auto add_point = [&](const float3 &p) {
      bounds_min = math::min(bounds_min, p);
      bounds_max = math::max(bounds_max, p);
      sum += p;
      count++;
    };
    for (const Leaf &leaf : tree.leaves) {
      add_point(leaf.position);
    }
    for (const Frond &frond : tree.fronds) {
      for (const float t : {0.0f, 0.5f, 1.0f}) {
        add_point(sample(tree.branches[frond.branch], t).position);
      }
    }
    Foliage foliage;
    foliage.center = sum / float(count);
    foliage.extent = (bounds_max - bounds_min) * 0.5f;
    foliage.clumps = tree.clumps.clumps.is_empty() ? nullptr : &tree.clumps;
    /* Fewer leaves get bigger, so the crown stays as full. */
    const float size_scale = std::min(1.0f / std::sqrt(std::max(detail, 0.05f)), 3.0f);
    for (const Leaf &leaf : tree.leaves) {
      if (leaf.keep < detail) {
        mesh_leaf(m, leaf, size_scale, foliage, offset, leaf_meshes);
      }
    }
    for (const Frond &frond : tree.fronds) {
      mesh_frond(m, tree, frond, detail, params.scale, foliage, offset);
    }
    for (const Clump &clump : tree.clumps.clumps) {
      if (clump.show) {
        mesh_clump(m, clump, detail, foliage, offset);
      }
    }
  }

  if (m.face_sizes.is_empty()) {
    return nullptr;
  }
  return build_mesh(m, params.vertex_data);
}

}  // namespace blender::geometry::tree
