/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup geo
 *
 * Collapse: every mesh island falls as a rigid piece and the mesh is returned in the pose where
 * the pieces came to rest. A private Bullet world is made for each solve and freed after it.
 */

#include <algorithm>
#include <cmath>

#include "DNA_mesh_types.h"

#include "BLI_array.hh"
#include "BLI_disjoint_set.hh"
#include "BLI_map.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_quaternion.hh"
#include "BLI_math_vector.hh"
#include "BLI_rand.hh"
#include "BLI_vector.hh"

#include "BKE_mesh.hh"

#include "GEO_mesh_collapse.hh"
#include "GEO_mesh_sim_cache.hh"

#include "rigid_pieces.hh"

#ifdef WITH_BULLET
#  include "RBI_api.h"
#endif

namespace blender::geometry {

#ifdef WITH_BULLET
using namespace rigid_pieces;
#endif

static Mesh *mesh_collapse_compute(const Mesh &mesh,
                                   const MeshCollapseParams &params,
                                   const char **r_error)
{
  *r_error = nullptr;
#ifndef WITH_BULLET
  UNUSED_VARS(mesh, params);
  *r_error = "Physics is not available in this build";
  return nullptr;
#else
  const int verts_num = mesh.verts_num;
  if (verts_num == 0) {
    return nullptr;
  }

  /* Positions in simulation space. */
  const Span<float3> mesh_positions = mesh.vert_positions();
  Array<float3> positions(verts_num);
  for (const int i : IndexRange(verts_num)) {
    positions[i] = math::transform_point(params.mesh_to_space, mesh_positions[i]);
  }

  const Pieces found = find_pieces(mesh, params.pieces);
  const int islands_num = found.num;
  const Span<int> island_of_vert = found.island_of_vert;
  const Span<int> offsets = found.offsets;
  const Span<int> verts_of_island = found.verts_of_island;
  if (islands_num > mesh_collapse_max_islands) {
    *r_error = "Too many pieces, the limit is 20000";
    return nullptr;
  }

  Array<Island> islands(islands_num);
  compute_islands(mesh, positions, island_of_vert, offsets, verts_of_island, islands);
  if (!params.keep.is_empty()) {
    for (const int v : IndexRange(std::min<int>(verts_num, params.keep.size()))) {
      if (params.keep[v]) {
        islands[island_of_vert[v]].keep = true;
      }
    }
  }

  /* How high each piece is over the lowest point decides how hard it's pushed. */
  float lowest = FLT_MAX, highest = -FLT_MAX;
  for (const float3 &p : positions) {
    lowest = std::min(lowest, p.z);
    highest = std::max(highest, p.z);
  }
  const float tallest = std::max(highest - lowest, 1e-4f);
  const float lean_angle = RandomNumberGenerator(uint32_t(params.seed) * 2654435761u + 7u)
                               .get_float() *
                           6.28318530718f;

  /* The world and its bodies. */
  const float gravity[3] = {0.0f, 0.0f, -params.gravity};
  rbDynamicsWorld *world = RB_dworld_new(gravity);
  RB_dworld_set_solver_iterations(world, 20);
  RB_dworld_set_split_impulse(world, 1);

  Vector<rbCollisionShape *> shapes;
  Vector<rbRigidBody *> bodies;
  Array<rbRigidBody *> island_bodies(islands_num, nullptr);
  const float unit_quat[4] = {1.0f, 0.0f, 0.0f, 0.0f};

  for (const int i : IndexRange(islands_num)) {
    const Island &island = islands[i];
    const Span<int> verts = verts_of_island.slice(offsets[i], offsets[i + 1] - offsets[i]);
    Array<float3> local(verts.size());
    for (const int j : verts.index_range()) {
      local[j] = positions[verts[j]] - island.center;
    }
    rbCollisionShape *shape = island_shape(local, island.size);
    shapes.append(shape);
    rbRigidBody *body = RB_body_new(shape, island.center, unit_quat);
    RB_body_set_mass(body, island.keep ? 0.0f : island.mass);
    RB_body_set_friction(body, params.friction);
    RB_body_set_restitution(body, params.bounce);
    RB_body_set_damping(body, 0.04f, 0.1f);
    RB_body_set_sleep_thresh(body, 0.4f, 0.5f);
    RB_body_set_activation_state(body, true);
    if (!island.keep && params.push > 0.0f) {
      /* Lean everything one way, harder the higher up, like a column tipping over, with some
       * randomness per piece so the pile doesn't look neat. */
      RandomNumberGenerator rng(uint32_t(params.seed) * 2654435761u + uint32_t(i) * 40503u);
      const float height = (island.center.z - lowest) / tallest;
      const float speed = params.push * (0.2f + 1.8f * height) * (0.7f + 0.6f * rng.get_float());
      const float angle = lean_angle + (rng.get_float() - 0.5f) * 1.2f;
      const float3 velocity(std::cos(angle) * speed, std::sin(angle) * speed, 0.0f);
      const float3 spin = rng.get_unit_float3() * params.push;
      RB_body_set_linear_velocity(body, velocity);
      RB_body_set_angular_velocity(body, spin);
    }
    RB_dworld_add_body(world, body, 1);
    bodies.append(body);
    island_bodies[i] = body;
  }

  /* The ground: the given mesh, or a floor under the lowest point. */
  rbCollisionShape *floor = nullptr;
  if (params.ground != nullptr) {
    floor = ground_shape(*params.ground, params.ground_to_space, params.ground_cage);
  }
  if (floor == nullptr) {
    float3 min(FLT_MAX), max(-FLT_MAX);
    for (const float3 &p : positions) {
      min = math::min(min, p);
      max = math::max(max, p);
    }
    const float size = math::length(max - min);
    const float half_xy = size * 20.0f + 10.0f;
    const float half_z = size * 0.5f + 0.1f;
    floor = RB_shape_new_box(half_xy, half_xy, half_z);
    RB_shape_set_margin(floor, 1e-4f);
    const float3 center = float3((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f, min.z - half_z);
    rbRigidBody *body = RB_body_new(floor, center, unit_quat);
    RB_body_set_mass(body, 0.0f);
    RB_body_set_friction(body, params.friction);
    RB_body_set_restitution(body, params.bounce);
    RB_dworld_add_body(world, body, 1);
    bodies.append(body);
  }
  else {
    const float origin[3] = {0.0f, 0.0f, 0.0f};
    rbRigidBody *body = RB_body_new(floor, origin, unit_quat);
    RB_body_set_mass(body, 0.0f);
    RB_body_set_friction(body, params.friction);
    RB_body_set_restitution(body, params.bounce);
    RB_dworld_add_body(world, body, 1);
    bodies.append(body);
  }
  shapes.append(floor);

  /* Fall. */
  const int substeps = std::max(params.substeps, 1);
  const int frames = std::max(int(std::round(params.time * 60.0f)), 0);
  for (int frame = 0; frame < frames; frame++) {
    RB_dworld_step_simulation(world, 1.0f / 60.0f, substeps, 1.0f / (60.0f * substeps));
  }

  /* Move each island to where its body is. */
  Mesh *result = BKE_mesh_copy_for_eval(mesh);
  MutableSpan<float3> result_positions = result->vert_positions_for_write();
  const float4x4 space_to_mesh = math::invert(params.mesh_to_space);
  Array<float4x4> transforms(islands_num);
  for (const int i : IndexRange(islands_num)) {
    /* Bullet uses doubles here, so read the place and turn rather than its matrix. */
    float3 location;
    float4 rotation;
    RB_body_get_position(island_bodies[i], location);
    RB_body_get_orientation(island_bodies[i], rotation);
    const float4x4 body_matrix = math::from_loc_rot<float4x4>(
        location, math::normalize(math::Quaternion(rotation)));
    transforms[i] = space_to_mesh * body_matrix * math::from_location<float4x4>(-islands[i].center);
  }
  for (const int v : IndexRange(verts_num)) {
    const int island = island_of_vert[v];
    if (!islands[island].keep) {
      result_positions[v] = math::transform_point(transforms[island], positions[v]);
    }
  }
  result->tag_positions_changed();

  for (rbRigidBody *body : bodies) {
    RB_dworld_remove_body(world, body);
    RB_body_delete(body);
  }
  for (rbCollisionShape *shape : shapes) {
    RB_shape_delete(shape);
  }
  RB_dworld_delete(world);
  return result;
#endif
}

Mesh *mesh_collapse(const Mesh &mesh, const MeshCollapseParams &params, const char **r_error)
{
  /* Objects are evaluated again for undo and for changes to other objects. Taking the result from
   * the cache when nothing it depends on changed keeps that from simulating all over again. */
  SimCacheHasher hasher("mesh_collapse");
  hasher.add_mesh(mesh);
  hasher.add(params.time);
  hasher.add(params.gravity);
  hasher.add(params.push);
  hasher.add(params.seed);
  hasher.add(params.friction);
  hasher.add(params.bounce);
  hasher.add(params.substeps);
  hasher.add(params.mesh_to_space);
  hasher.add(params.ground != nullptr);
  if (params.ground != nullptr) {
    hasher.add_mesh_shape(*params.ground);
    hasher.add(params.ground_to_space);
    hasher.add(params.ground_cage);
  }
  hasher.add_span(params.keep);
  hasher.add_span(params.pieces);
  return sim_cache_get(
      hasher.finish(),
      mesh,
      [&](const char **r_compute_error) { return mesh_collapse_compute(mesh, params, r_compute_error); },
      r_error);
}

}  // namespace blender::geometry
