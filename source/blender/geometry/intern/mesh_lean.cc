/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup geo
 */

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <numeric>

#include "BLI_array.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_quaternion.hh"
#include "BLI_math_vector.hh"
#include "BLI_vector.hh"

#include "DNA_mesh_types.h"

#include "BKE_attribute.hh"
#include "BKE_mesh.hh"

#include "GEO_mesh_lean.hh"
#include "GEO_mesh_sim_cache.hh"

#include "rigid_pieces.hh"

namespace blender::geometry {

#ifdef WITH_BULLET

using namespace rigid_pieces;

namespace {

struct Box {
  float3 min = float3(FLT_MAX);
  float3 max = float3(-FLT_MAX);
};

/** Pairs of pieces whose boxes touch or are closer than their gap, found by sweeping along X. */
Vector<int2> touching_pairs(const Span<Box> boxes, const Span<float> gaps)
{
  const int num = boxes.size();
  Array<int> order(num);
  std::iota(order.begin(), order.end(), 0);
  std::sort(order.begin(), order.end(), [&](const int a, const int b) {
    return boxes[a].min.x < boxes[b].min.x;
  });
  float max_gap = 0.0f;
  for (const float gap : gaps) {
    max_gap = std::max(max_gap, gap);
  }
  Vector<int2> pairs;
  for (const int a : IndexRange(num)) {
    const int i = order[a];
    for (int b = a + 1; b < num; b++) {
      const int j = order[b];
      if (boxes[j].min.x > boxes[i].max.x + max_gap) {
        break;
      }
      const float gap = std::max(gaps[i], gaps[j]);
      bool touch = true;
      for (const int axis : IndexRange(3)) {
        if (boxes[j].min[axis] > boxes[i].max[axis] + gap ||
            boxes[i].min[axis] > boxes[j].max[axis] + gap)
        {
          touch = false;
          break;
        }
      }
      if (touch) {
        pairs.append(int2(i, j));
      }
    }
  }
  return pairs;
}

}  // namespace

#endif

static Mesh *mesh_lean_compute(const Mesh &mesh,
                               const MeshLeanParams &params,
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
  if (islands_num > mesh_lean_max_islands) {
    *r_error = "Too many pieces, the limit is 20000";
    return nullptr;
  }
  Array<Island> islands(islands_num);
  compute_islands(
      mesh, positions, found.island_of_vert, found.offsets, found.verts_of_island, islands);
  if (!params.keep.is_empty()) {
    for (const int v : IndexRange(std::min<int>(verts_num, params.keep.size()))) {
      if (params.keep[v]) {
        islands[found.island_of_vert[v]].keep = true;
      }
    }
  }

  /* The box around each piece, and around everything. */
  Array<Box> boxes(islands_num);
  Box all;
  for (const int v : IndexRange(verts_num)) {
    Box &box = boxes[found.island_of_vert[v]];
    box.min = math::min(box.min, positions[v]);
    box.max = math::max(box.max, positions[v]);
    all.min = math::min(all.min, positions[v]);
    all.max = math::max(all.max, positions[v]);
  }

  /* The world and its bodies. */
  const float gravity[3] = {0.0f, 0.0f, -params.gravity};
  rbDynamicsWorld *world = RB_dworld_new(gravity);
  RB_dworld_set_solver_iterations(world, 20);
  RB_dworld_set_split_impulse(world, 1);

  Vector<rbCollisionShape *> shapes;
  Vector<rbRigidBody *> bodies;
  Vector<rbConstraint *> constraints;
  Array<rbRigidBody *> island_bodies(islands_num, nullptr);
  const float unit_quat[4] = {1.0f, 0.0f, 0.0f, 0.0f};

  for (const int i : IndexRange(islands_num)) {
    const Island &island = islands[i];
    const Span<int> verts = found.verts(i);
    Array<float3> local(verts.size());
    for (const int j : verts.index_range()) {
      local[j] = positions[verts[j]] - island.center;
    }
    rbCollisionShape *shape = island_shape(local, island.size);
    shapes.append(shape);
    rbRigidBody *body = RB_body_new(shape, island.center, unit_quat);
    RB_body_set_mass(body, island.keep ? 0.0f : island.mass);
    RB_body_set_friction(body, params.friction);
    RB_body_set_restitution(body, 0.0f);
    /* Extra damping, so pieces come to rest where they are pushed instead of tumbling on. */
    RB_body_set_damping(body, 0.15f, 0.4f);
    RB_body_set_sleep_thresh(body, 0.2f, 0.3f);
    RB_body_set_activation_state(body, true);
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
    const float size = math::length(all.max - all.min);
    const float half_xy = size * 20.0f + 10.0f;
    const float half_z = size * 0.5f + 0.1f;
    floor = RB_shape_new_box(half_xy, half_xy, half_z);
    RB_shape_set_margin(floor, 1e-4f);
    const float3 center = float3(
        (all.min.x + all.max.x) * 0.5f, (all.min.y + all.max.y) * 0.5f, all.min.z - half_z);
    rbRigidBody *body = RB_body_new(floor, center, unit_quat);
    RB_body_set_mass(body, 0.0f);
    RB_body_set_friction(body, params.friction);
    RB_dworld_add_body(world, body, 1);
    bodies.append(body);
  }
  else {
    const float origin[3] = {0.0f, 0.0f, 0.0f};
    rbRigidBody *body = RB_body_new(floor, origin, unit_quat);
    RB_body_set_mass(body, 0.0f);
    RB_body_set_friction(body, params.friction);
    RB_dworld_add_body(world, body, 1);
    bodies.append(body);
  }
  shapes.append(floor);

  const int substeps = std::max(params.substeps, 1);
  const float step = 1.0f / (60.0f * float(substeps));

  /* Glue between touching pieces. It breaks when pulled harder than a multiple of the pieces'
   * weight, so the wall cracks where the push is strongest and stays together elsewhere. */
  if (params.glue > 0.0f) {
    Array<float> gaps(islands_num);
    for (const int i : IndexRange(islands_num)) {
      gaps[i] = std::max(islands[i].size * 0.03f, 1e-4f);
    }
    const float weight_scale = std::max(params.gravity, 1.0f);
    for (const int2 pair : touching_pairs(boxes, gaps)) {
      const Island &a = islands[pair[0]];
      const Island &b = islands[pair[1]];
      if (a.keep && b.keep) {
        continue;
      }
      float3 pivot = (math::max(boxes[pair[0]].min, boxes[pair[1]].min) +
                      math::min(boxes[pair[0]].max, boxes[pair[1]].max)) *
                     0.5f;
      float orientation[4] = {1.0f, 0.0f, 0.0f, 0.0f};
      rbConstraint *constraint = RB_constraint_new_fixed(
          pivot, orientation, island_bodies[pair[0]], island_bodies[pair[1]]);
      const float mass = (a.keep ? b.mass : (b.keep ? a.mass : (a.mass + b.mass) * 0.5f));
      RB_constraint_set_breaking_threshold(constraint,
                                           params.glue * 50.0f * mass * weight_scale * step);
      RB_dworld_add_constraint(world, constraint, 0);
      constraints.append(constraint);
    }
  }

  /* The pusher: moved by hand every step, nothing moves it. It only moves along the direction,
   * so its turn and scale are put into its shape. */
  rbRigidBody *pusher_body = nullptr;
  float3 pusher_start(0.0f);
  float3 pusher_travel(0.0f);
  if (params.pusher != nullptr && params.pusher->verts_num > 0) {
    float4x4 linear = params.pusher_to_space;
    linear.location() = float3(0.0f);
    const float3 end = params.pusher_to_space.location();
    const float3 direction = math::length_squared(params.direction) > 1e-12f ?
                                 math::normalize(params.direction) :
                                 float3(1.0f, 0.0f, 0.0f);
    pusher_travel = direction * params.distance;
    pusher_start = end - pusher_travel;

    rbCollisionShape *shape = nullptr;
    if (params.pusher_hull) {
      const Span<float3> pusher_positions = params.pusher->vert_positions();
      Array<float3> local(pusher_positions.size());
      float3 min(FLT_MAX), max(-FLT_MAX);
      for (const int i : pusher_positions.index_range()) {
        local[i] = math::transform_point(linear, pusher_positions[i]);
        min = math::min(min, local[i]);
        max = math::max(max, local[i]);
      }
      shape = island_shape(local, math::length(max - min));
    }
    else {
      shape = ground_shape(*params.pusher, linear, 0.0f);
    }
    if (shape != nullptr) {
      shapes.append(shape);
      pusher_body = RB_body_new(shape, pusher_start, unit_quat);
      RB_body_set_mass(pusher_body, 0.0f);
      RB_body_set_kinematic_state(pusher_body, true);
      RB_body_set_friction(pusher_body, params.friction);
      RB_dworld_add_body(world, pusher_body, 1);
      bodies.append(pusher_body);
    }
  }

  /* Push, then settle. */
  const int push_steps = std::max(int(std::round(params.push_time * 60.0f * substeps)), 1);
  const int settle_steps = std::max(int(std::round(params.settle_time * 60.0f * substeps)), 0);
  if (pusher_body != nullptr) {
    for (int s = 0; s < push_steps; s++) {
      const float t = float(s + 1) / float(push_steps);
      const float3 location = pusher_start + pusher_travel * t;
      RB_body_set_loc_rot(pusher_body, location, unit_quat);
      RB_dworld_step_simulation(world, step, 1, step);
    }
    if (params.release) {
      RB_dworld_remove_body(world, pusher_body);
      bodies.remove_first_occurrence_and_reorder(pusher_body);
      RB_body_delete(pusher_body);
      pusher_body = nullptr;
    }
  }
  for (int s = 0; s < settle_steps; s++) {
    RB_dworld_step_simulation(world, step, 1, step);
  }

  /* Move each island to where its body is. */
  Mesh *result = BKE_mesh_copy_for_eval(mesh);
  MutableSpan<float3> result_positions = result->vert_positions_for_write();
  const float4x4 space_to_mesh = math::invert(params.mesh_to_space);
  Array<float4x4> transforms(islands_num);
  Array<float> moved(islands_num, 0.0f);
  for (const int i : IndexRange(islands_num)) {
    /* Bullet uses doubles here, so read the place and turn rather than its matrix. */
    float3 location;
    float4 rotation;
    RB_body_get_position(island_bodies[i], location);
    RB_body_get_orientation(island_bodies[i], rotation);
    const float4x4 body_matrix = math::from_loc_rot<float4x4>(
        location, math::normalize(math::Quaternion(rotation)));
    transforms[i] = space_to_mesh * body_matrix *
                    math::from_location<float4x4>(-islands[i].center);
    moved[i] = islands[i].keep ? 0.0f : math::distance(location, islands[i].center);
  }
  for (const int v : IndexRange(verts_num)) {
    const int island = found.island_of_vert[v];
    if (!islands[island].keep) {
      result_positions[v] = math::transform_point(transforms[island], positions[v]);
    }
  }
  result->tag_positions_changed();

  if (params.moved_id) {
    bke::SpanAttributeWriter<float> writer =
        result->attributes_for_write().lookup_or_add_for_write_only_span<float>(
            *params.moved_id, bke::AttrDomain::Point);
    if (writer) {
      for (const int v : IndexRange(verts_num)) {
        writer.span[v] = moved[found.island_of_vert[v]];
      }
      writer.finish();
    }
  }

  for (rbConstraint *constraint : constraints) {
    RB_dworld_remove_constraint(world, constraint);
    RB_constraint_delete(constraint);
  }
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

Mesh *mesh_lean(const Mesh &mesh, const MeshLeanParams &params, const char **r_error)
{
  /* Objects are evaluated again for undo and for changes to other objects. Taking the result from
   * the cache when nothing it depends on changed keeps that from simulating all over again. */
  SimCacheHasher hasher("mesh_lean");
  hasher.add_mesh(mesh);
  hasher.add(params.mesh_to_space);
  hasher.add(params.pusher != nullptr);
  if (params.pusher != nullptr) {
    hasher.add_mesh_shape(*params.pusher);
    hasher.add(params.pusher_to_space);
  }
  hasher.add(params.pusher_hull);
  hasher.add(params.direction);
  hasher.add(params.distance);
  hasher.add(params.push_time);
  hasher.add(params.settle_time);
  hasher.add(params.release);
  hasher.add(params.glue);
  hasher.add(params.friction);
  hasher.add(params.gravity);
  hasher.add(params.substeps);
  hasher.add(params.ground != nullptr);
  if (params.ground != nullptr) {
    hasher.add_mesh_shape(*params.ground);
    hasher.add(params.ground_to_space);
    hasher.add(params.ground_cage);
  }
  hasher.add_span(params.keep);
  hasher.add_span(params.pieces);
  hasher.add_string(params.moved_id ? StringRef(*params.moved_id) : StringRef());
  return sim_cache_get(
      hasher.finish(),
      mesh,
      [&](const char **r_compute_error) { return mesh_lean_compute(mesh, params, r_compute_error); },
      r_error);
}

}  // namespace blender::geometry
