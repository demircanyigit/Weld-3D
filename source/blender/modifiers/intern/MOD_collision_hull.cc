/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Collision Hull: simple closed shapes around the mesh for game collision. One convex hull, one
 * per loose part, a number of hulls over parts of the mesh, or boxes.
 */

#include <algorithm>
#include <array>
#include <cfloat>

#include "BLI_array.hh"
#include "BLI_bounds.hh"
#include "BLI_disjoint_set.hh"
#include "BLI_math_vector.hh"
#include "BLI_rand.hh"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "BKE_attribute.hh"
#include "BKE_geometry_set.hh"
#include "BKE_mesh.hh"
#include "BKE_modifier.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "GEO_join_geometries.hh"

#include "MOD_modifiertypes.hh"
#include "MOD_ui_common.hh"

#ifdef WITH_BULLET
#  include "RBI_hull_api.h"
#endif

namespace blender {

static void init_data(ModifierData *md)
{
  CollisionHullModifierData *data = reinterpret_cast<CollisionHullModifierData *>(md);
  INIT_DEFAULT_STRUCT_AFTER(data, modifier);
}

/** A box from its corners, six quads facing out. */
static Mesh *box_mesh(const float3 &min, const float3 &max)
{
  Mesh *mesh = BKE_mesh_new_nomain(8, 0, 6, 24);
  MutableSpan<float3> positions = mesh->vert_positions_for_write();
  for (const int i : IndexRange(8)) {
    positions[i] = float3(i & 1 ? max.x : min.x, i & 2 ? max.y : min.y, i & 4 ? max.z : min.z);
  }
  static const int faces[6][4] = {
      {0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
  MutableSpan<int> offsets = mesh->face_offsets_for_write();
  MutableSpan<int> corner_verts = mesh->corner_verts_for_write();
  for (const int f : IndexRange(6)) {
    offsets[f] = f * 4;
    for (const int k : IndexRange(4)) {
      corner_verts[f * 4 + k] = faces[f][k];
    }
  }
  offsets.last() = 24;
  bke::mesh_calc_edges(*mesh, false, false);
  mesh->tag_positions_changed();
  return mesh;
}

#ifdef WITH_BULLET
/** The convex hull of the points, made the same way as the Convex Hull node. */
static Mesh *hull_mesh(const Span<float3> coords)
{
  if (coords.size() < 4) {
    return nullptr;
  }
  plConvexHull hull = plConvexHullCompute((float (*)[3])coords.data(), coords.size());
  const int verts_num = plConvexHullNumVertices(hull);
  const int faces_num = verts_num <= 2 ? 0 : plConvexHullNumFaces(hull);
  const int loops_num = verts_num <= 2 ? 0 : plConvexHullNumLoops(hull);
  if (faces_num == 0) {
    plConvexHullDelete(hull);
    return nullptr;
  }
  Mesh *result = BKE_mesh_new_nomain(verts_num, 0, faces_num, loops_num);
  MutableSpan<float3> positions = result->vert_positions_for_write();
  for (const int i : IndexRange(verts_num)) {
    float3 co;
    int original_index;
    plConvexHullGetVertex(hull, i, co, &original_index);
    positions[i] = coords.index_range().contains(original_index) ? coords[original_index] : co;
  }
  Array<int> loop_verts(loops_num);
  for (const int i : IndexRange(loops_num)) {
    int v_from;
    int v_to;
    plConvexHullGetLoop(hull, i, &v_from, &v_to);
    loop_verts[i] = v_from;
  }
  MutableSpan<int> offsets = result->face_offsets_for_write();
  MutableSpan<int> corner_verts = result->corner_verts_for_write();
  Array<int> loops;
  int corner = 0;
  for (const int i : IndexRange(faces_num)) {
    const int len = plConvexHullGetFaceSize(hull, i);
    loops.reinitialize(len);
    plConvexHullGetFaceLoops(hull, i, loops.data());
    offsets[i] = corner;
    for (const int k : IndexRange(len)) {
      corner_verts[corner++] = loop_verts[loops[k]];
    }
  }
  offsets.last() = corner;
  plConvexHullDelete(hull);
  bke::mesh_calc_edges(*result, false, false);
  result->tag_positions_changed();
  return result;
}
#endif

/** Points grouped by loose part of the mesh. */
static Vector<Vector<float3>> split_by_islands(const Mesh &mesh)
{
  const Span<float3> positions = mesh.vert_positions();
  DisjointSet<int> sets(mesh.verts_num);
  for (const int2 edge : mesh.edges()) {
    sets.join(edge[0], edge[1]);
  }
  Array<int> group_of_root(mesh.verts_num, -1);
  Vector<Vector<float3>> groups;
  for (const int v : positions.index_range()) {
    int &group = group_of_root[sets.find_root(v)];
    if (group == -1) {
      group = groups.append_and_get_index({});
    }
    groups[group].append(positions[v]);
  }
  return groups;
}

/** Points grouped around centers placed by k-means, for a number of hulls over one shape. */
static Vector<Vector<float3>> split_by_parts(const Mesh &mesh, const int parts, const int seed)
{
  const Span<float3> positions = mesh.vert_positions();
  const int count = std::clamp(parts, 1, std::max(int(positions.size()) / 4, 1));
  RandomNumberGenerator rng(uint32_t(seed) * 2654435761u + 11u);
  /* Start far apart: each next center is the point farthest from the ones so far. */
  Vector<float3> centers;
  centers.append(positions[rng.get_int32(positions.size())]);
  Array<float> nearest(positions.size(), FLT_MAX);
  while (centers.size() < count) {
    int farthest = 0;
    for (const int v : positions.index_range()) {
      nearest[v] = std::min(nearest[v], math::distance_squared(positions[v], centers.last()));
      if (nearest[v] > nearest[farthest]) {
        farthest = v;
      }
    }
    centers.append(positions[farthest]);
  }
  Array<int> owner(positions.size(), 0);
  for ([[maybe_unused]] const int iteration : IndexRange(12)) {
    Array<float3> sum(count, float3(0.0f));
    Array<int> members(count, 0);
    for (const int v : positions.index_range()) {
      int best = 0;
      float best_distance = FLT_MAX;
      for (const int c : IndexRange(count)) {
        const float d = math::distance_squared(positions[v], centers[c]);
        if (d < best_distance) {
          best_distance = d;
          best = c;
        }
      }
      owner[v] = best;
      sum[best] += positions[v];
      members[best]++;
    }
    for (const int c : IndexRange(count)) {
      if (members[c] > 0) {
        centers[c] = sum[c] / float(members[c]);
      }
    }
  }
  Vector<Vector<float3>> groups(count);
  for (const int v : positions.index_range()) {
    groups[owner[v]].append(positions[v]);
  }
  /* Neighboring hulls overlap a little, so no gap is left between them. */
  for (const int v : positions.index_range()) {
    for (const int c : IndexRange(count)) {
      if (c != owner[v] && math::distance(positions[v], centers[c]) <
                               math::distance(positions[v], centers[owner[v]]) * 1.08f)
      {
        groups[c].append(positions[v]);
      }
    }
  }
  return groups;
}

/** Grows the points away from their center by a distance. */
static void inflate(MutableSpan<float3> points, const float offset)
{
  if (offset == 0.0f || points.is_empty()) {
    return;
  }
  float3 center(0.0f);
  for (const float3 &p : points) {
    center += p;
  }
  center /= float(points.size());
  for (float3 &p : points) {
    const float3 d = p - center;
    const float length = math::length(d);
    if (length > 1e-8f) {
      p += d / length * offset;
    }
  }
}

static Mesh *modify_mesh(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  const CollisionHullModifierData &hmd = *reinterpret_cast<CollisionHullModifierData *>(md);
  if (mesh->verts_num < 4) {
    return mesh;
  }
  Vector<Vector<float3>> groups;
  switch (hmd.mode) {
    case MOD_HULL_ISLANDS:
    case MOD_HULL_BOXES:
      groups = split_by_islands(*mesh);
      break;
    case MOD_HULL_PARTS:
      groups = split_by_parts(*mesh, hmd.parts, hmd.seed);
      break;
    default:
      groups.append(Vector<float3>(mesh->vert_positions()));
      break;
  }

  Vector<bke::GeometrySet> pieces;
  if (hmd.flag & MOD_HULL_KEEP_MESH) {
    pieces.append(bke::GeometrySet::from_mesh(mesh, bke::GeometryOwnershipType::ReadOnly));
  }
  for (Vector<float3> &points : groups) {
    Mesh *piece = nullptr;
    if (hmd.mode == MOD_HULL_BOXES) {
      const std::optional<Bounds<float3>> bounds = bounds::min_max(points.as_span());
      if (bounds) {
        piece = box_mesh(bounds->min - float3(hmd.offset), bounds->max + float3(hmd.offset));
      }
    }
    else {
#ifdef WITH_BULLET
      inflate(points, hmd.offset);
      piece = hull_mesh(points);
#else
      BKE_modifier_set_error(ctx->object, md, "Built without Bullet, only Boxes work");
      return mesh;
#endif
    }
    if (piece != nullptr) {
      BKE_mesh_copy_parameters_for_eval(piece, mesh);
      bke::mesh_smooth_set(*piece, false);
      pieces.append(bke::GeometrySet::from_mesh(piece));
    }
  }
  if (pieces.is_empty()) {
    return mesh;
  }
  bke::GeometrySet joined = geometry::join_geometries(pieces, {});
  Mesh *result = joined.get_component_for_write<bke::MeshComponent>().release();
  UNUSED_VARS(ctx);
  return result ? result : mesh;
}

static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);
  layout.use_property_split_set(true);
  layout.prop(ptr, "mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  if (RNA_enum_get(ptr, "mode") == MOD_HULL_PARTS) {
    layout.prop(ptr, "parts", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    layout.prop(ptr, "seed", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  layout.prop(ptr, "offset", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "use_keep_mesh", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  modifier_error_message_draw(layout, ptr);
}

static void panel_register(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_CollisionHull, panel_draw);
}

ModifierTypeInfo modifierType_CollisionHull = {
    /*idname*/ "CollisionHull",
    /*name*/ N_("Collision Hull"),
    /*struct_name*/ "CollisionHullModifierData",
    /*struct_size*/ sizeof(CollisionHullModifierData),
    /*srna*/ &RNA_CollisionHullModifier,
    /*type*/ ModifierTypeType::Constructive,
    /*flags*/ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsEditmode |
        eModifierTypeFlag_EnableInEditmode,
    /*icon*/ ICON_MOD_COLLISION_HULL,

    /*copy_data*/ BKE_modifier_copydata_generic,

    /*deform_verts*/ nullptr,
    /*deform_matrices*/ nullptr,
    /*deform_verts_EM*/ nullptr,
    /*deform_matrices_EM*/ nullptr,
    /*modify_mesh*/ modify_mesh,
    /*modify_geometry_set*/ nullptr,

    /*init_data*/ init_data,
    /*required_data_mask*/ nullptr,
    /*free_data*/ nullptr,
    /*is_disabled*/ nullptr,
    /*update_depsgraph*/ nullptr,
    /*depends_on_time*/ nullptr,
    /*depends_on_normals*/ nullptr,
    /*foreach_ID_link*/ nullptr,
    /*foreach_tex_link*/ nullptr,
    /*free_runtime_data*/ nullptr,
    /*panel_register*/ panel_register,
    /*blend_write*/ nullptr,
    /*blend_read*/ nullptr,
    /*foreach_cache*/ nullptr,
    /*foreach_working_space_color*/ nullptr,
};

}  // namespace blender
