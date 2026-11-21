/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Cracks: branching cracks that run over the surface from impact points. They are cut in as
 * grooves, which needs enough points on the surface, or added as thin strips just above it. A
 * point attribute marks where they are, for the shader.
 */

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>

#include "BLI_array.hh"
#include "BLI_kdopbvh.hh"
#include "BLI_kdtree.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_rand.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "BKE_attribute.hh"
#include "BKE_bvhutils.hh"
#include "BKE_geometry_set.hh"
#include "BKE_lib_query.hh"
#include "BKE_mesh.hh"
#include "BKE_modifier.hh"

#include "DEG_depsgraph_build.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "GEO_join_geometries.hh"

#include "MOD_modifiertypes.hh"
#include "MOD_ui_common.hh"
#include "MOD_util.hh"

namespace blender {

static void init_data(ModifierData *md)
{
  CracksModifierData *data = reinterpret_cast<CracksModifierData *>(md);
  INIT_DEFAULT_STRUCT_AFTER(data, modifier);
}

static void foreach_ID_link(ModifierData *md, Object *ob, IDWalkFunc walk, void *user_data)
{
  CracksModifierData *data = reinterpret_cast<CracksModifierData *>(md);
  walk(user_data, ob, reinterpret_cast<ID **>(&data->object), IDWALK_CB_NOP);
  CracksModifierData *pick_data = reinterpret_cast<CracksModifierData *>(md);
  walk(user_data, ob, reinterpret_cast<ID **>(&pick_data->material_index_pick), IDWALK_CB_USER);
}

static void update_depsgraph(ModifierData *md, const ModifierUpdateDepsgraphContext *ctx)
{
  const CracksModifierData &data = *reinterpret_cast<CracksModifierData *>(md);
  if (data.object != nullptr && data.object != ctx->object) {
    DEG_add_object_relation(ctx->node, data.object, DEG_OB_COMP_TRANSFORM, "Cracks Modifier");
    DEG_add_depends_on_transform_relation(ctx->node, "Cracks Modifier");
  }
}

/** One point along a crack. */
struct CrackPoint {
  float3 position;
  float3 normal;
  /** Half the width here, it narrows toward the tip. */
  float half_width;
};

using Crack = Vector<CrackPoint>;

/** Keeps the cracks on the surface. */
struct Surface {
  const bke::BVHTreeFromMesh *bvh;

  bool snap(const float3 &p, float3 &r_point, float3 &r_normal) const
  {
    BVHTreeNearest hit;
    hit.index = -1;
    hit.dist_sq = FLT_MAX;
    BLI_bvhtree_find_nearest(bvh->tree,
                             p,
                             &hit,
                             bvh->nearest_callback,
                             const_cast<bke::BVHTreeFromMesh *>(bvh));
    if (hit.index == -1) {
      return false;
    }
    r_point = float3(hit.co);
    r_normal = math::normalize(float3(hit.no));
    return true;
  }
};

/** A direction at right angles to the normal, turned by an angle around it. */
static float3 turn(const float3 &dir, const float3 &normal, const float angle)
{
  const float3 side = math::cross(normal, dir);
  return math::normalize(dir * std::cos(angle) + side * std::sin(angle));
}

static constexpr int MAX_CRACK_POINTS = 60000;

/** Walks a crack over the surface, adding branches to the list as it goes. */
static void grow_crack(const Surface &surface,
                       const CracksModifierData &cmd,
                       RandomNumberGenerator &rng,
                       float3 position,
                       float3 normal,
                       float3 dir,
                       const float length,
                       const float width,
                       const int depth,
                       Vector<Crack> &r_cracks,
                       int &r_points)
{
  /* Steps short enough for a smooth groove: about half the width, at most a hundred per crack. */
  const float step = std::max(std::max(width * 0.75f, length / 100.0f), 1e-5f);
  const int steps = std::max(int(length / step), 2);
  Crack crack;
  crack.reserve(steps + 1);
  for (const int i : IndexRange(steps + 1)) {
    if (r_points >= MAX_CRACK_POINTS) {
      break;
    }
    const float t = float(i) / float(steps);
    crack.append({position, normal, 0.5f * width * std::pow(1.0f - t, 0.7f) + 1e-6f});
    r_points++;
    if (i == steps) {
      break;
    }
    /* A random wiggle, and a branch now and then. */
    dir = turn(dir, normal, (rng.get_float() * 2.0f - 1.0f) * cmd.jitter * 0.6f);
    if (depth < 3 && rng.get_float() < cmd.branching * 4.0f / float(steps)) {
      const float side = rng.get_float() < 0.5f ? -1.0f : 1.0f;
      const float angle = side * (0.5f + rng.get_float() * 0.5f);
      const float remaining = length * (1.0f - t);
      grow_crack(surface,
                 cmd,
                 rng,
                 position,
                 normal,
                 turn(dir, normal, angle),
                 remaining * (0.35f + rng.get_float() * 0.3f),
                 width * std::pow(1.0f - t, 0.7f) * 0.7f,
                 depth + 1,
                 r_cracks,
                 r_points);
    }
    float3 next_point;
    float3 next_normal;
    if (!surface.snap(position + dir * step, next_point, next_normal)) {
      break;
    }
    const float3 moved = next_point - position;
    if (math::length(moved) < step * 0.2f) {
      /* Stuck at an edge of an open mesh. */
      break;
    }
    /* Keep heading the same way along the surface. */
    float3 along = moved - next_normal * math::dot(moved, next_normal);
    if (math::length(along) > 1e-8f) {
      dir = math::normalize(along);
    }
    position = next_point;
    normal = next_normal;
  }
  if (crack.size() >= 2) {
    r_cracks.append(std::move(crack));
  }
}

/** Random spots on the faces, by area. */
static Vector<float3> random_spots(const Mesh &mesh, const int count, RandomNumberGenerator &rng)
{
  Vector<float3> spots;
  const Span<float3> positions = mesh.vert_positions();
  const Span<int3> tris = mesh.corner_tris();
  const Span<int> corner_verts = mesh.corner_verts();
  if (tris.is_empty()) {
    return spots;
  }
  Array<float> cumulative(tris.size());
  float total = 0.0f;
  for (const int i : tris.index_range()) {
    const float3 &a = positions[corner_verts[tris[i][0]]];
    const float3 &b = positions[corner_verts[tris[i][1]]];
    const float3 &c = positions[corner_verts[tris[i][2]]];
    total += 0.5f * math::length(math::cross(b - a, c - a));
    cumulative[i] = total;
  }
  for ([[maybe_unused]] const int i : IndexRange(count)) {
    const float pick = rng.get_float() * total;
    const int tri = std::min(int(std::lower_bound(cumulative.begin(), cumulative.end(), pick) -
                                 cumulative.begin()),
                             int(tris.size()) - 1);
    float u = rng.get_float();
    float v = rng.get_float();
    if (u + v > 1.0f) {
      u = 1.0f - u;
      v = 1.0f - v;
    }
    const float3 &a = positions[corner_verts[tris[tri][0]]];
    const float3 &b = positions[corner_verts[tris[tri][1]]];
    const float3 &c = positions[corner_verts[tris[tri][2]]];
    spots.append(a + (b - a) * u + (c - a) * v);
  }
  return spots;
}

static Vector<Crack> make_cracks(const Mesh &mesh,
                                 const CracksModifierData &cmd,
                                 const ModifierEvalContext *ctx,
                                 const bke::BVHTreeFromMesh &bvh)
{
  Vector<Crack> cracks;
  const Surface surface{&bvh};
  RandomNumberGenerator rng(uint32_t(cmd.seed) * 2654435761u + 3u);
  Vector<float3> impacts;
  if (cmd.object != nullptr && cmd.object != ctx->object) {
    const float4x4 to_self = ctx->object->world_to_object() * cmd.object->object_to_world();
    impacts.append(to_self.location());
  }
  else {
    impacts = random_spots(mesh, std::max(cmd.impacts, 1), rng);
  }
  int points = 0;
  for (const float3 &impact : impacts) {
    float3 start;
    float3 normal;
    if (!surface.snap(impact, start, normal)) {
      continue;
    }
    /* Any direction along the surface to turn from. */
    const float3 helper = std::abs(normal.z) < 0.9f ? float3(0, 0, 1) : float3(1, 0, 0);
    const float3 base = math::normalize(math::cross(normal, helper));
    const int count = std::max(cmd.count, 1);
    for (const int i : IndexRange(count)) {
      const float angle = (float(i) + (rng.get_float() - 0.5f) * cmd.jitter) / float(count) *
                          float(M_PI) * 2.0f;
      const float length = cmd.length * (0.6f + rng.get_float() * 0.4f);
      grow_crack(surface,
                 cmd,
                 rng,
                 start,
                 normal,
                 turn(base, normal, angle),
                 length,
                 cmd.width,
                 0,
                 cracks,
                 points);
    }
  }
  return cracks;
}

/** Grooves: points near a crack sink along their normals, deepest on the crack's line. */
static void cut_grooves(Mesh &mesh,
                        const CracksModifierData &cmd,
                        const Span<Crack> cracks,
                        MutableSpan<float> r_value)
{
  int total = 0;
  for (const Crack &crack : cracks) {
    total += crack.size();
  }
  /* Each sample knows its crack and its place on it, to measure to the lines beside it. */
  Vector<int2> samples;
  samples.reserve(total);
  KDTree_3d *tree = kdtree_3d_new(total);
  float max_step = 0.0f;
  for (const int c : cracks.index_range()) {
    const Crack &crack = cracks[c];
    for (const int i : crack.index_range()) {
      kdtree_3d_insert(tree, samples.size(), crack[i].position);
      samples.append(int2(c, i));
      if (i > 0) {
        max_step = std::max(max_step, math::distance(crack[i - 1].position, crack[i].position));
      }
    }
  }
  kdtree_3d_balance(tree);
  const Array<float3> normals(mesh.vert_normals());
  MutableSpan<float3> positions = mesh.vert_positions_for_write();
  const float max_half_width = cmd.width * 0.5f + 1e-6f;
  threading::parallel_for(positions.index_range(), 1024, [&](const IndexRange range) {
    for (const int v : range) {
      const float3 p = positions[v];
      KDTreeNearest_3d nearest;
      if (kdtree_3d_find_nearest(tree, p, &nearest) == -1 ||
          nearest.dist > max_half_width + max_step)
      {
        continue;
      }
      /* The distance to the crack's line, not to its samples, so the groove runs evenly. */
      const Crack &crack = cracks[samples[nearest.index].x];
      const int i = samples[nearest.index].y;
      float dist = nearest.dist;
      float half_width = crack[i].half_width;
      for (const int a : {i - 1, i}) {
        if (a < 0 || a + 1 >= crack.size()) {
          continue;
        }
        const float3 &p0 = crack[a].position;
        const float3 &p1 = crack[a + 1].position;
        const float3 seg = p1 - p0;
        const float len_sq = math::length_squared(seg);
        const float t = len_sq > 1e-12f ?
                            std::clamp(math::dot(p - p0, seg) / len_sq, 0.0f, 1.0f) :
                            0.0f;
        const float d = math::distance(p, p0 + seg * t);
        if (d < dist) {
          dist = d;
          half_width = math::interpolate(crack[a].half_width, crack[a + 1].half_width, t);
        }
      }
      const float u = dist / half_width;
      if (u >= 1.0f) {
        continue;
      }
      /* A V shape, a little rounded at the bottom. */
      const float profile = (1.0f - u) * (1.0f - u * 0.3f);
      const float taper = half_width / max_half_width;
      positions[v] -= normals[v] * (cmd.depth * profile * taper);
      r_value[v] = std::max(r_value[v], profile);
    }
  });
  kdtree_3d_free(tree);
  mesh.tag_positions_changed();
}

/** Strips: a thin ribbon over each crack, lifted a little off the surface. */
static Mesh *make_strips(const Mesh &mesh,
                         const CracksModifierData &cmd,
                         const Span<Crack> cracks,
                         const int material_index)
{
  int verts_num = 0;
  int faces_num = 0;
  for (const Crack &crack : cracks) {
    verts_num += crack.size() * 2;
    faces_num += crack.size() - 1;
  }
  Mesh *strips = BKE_mesh_new_nomain(verts_num, 0, faces_num, faces_num * 4);
  BKE_mesh_copy_parameters_for_eval(strips, &mesh);
  MutableSpan<float3> positions = strips->vert_positions_for_write();
  MutableSpan<int> offsets = strips->face_offsets_for_write();
  MutableSpan<int> corner_verts = strips->corner_verts_for_write();
  const float lift = std::max(cmd.width * 0.05f, 1e-5f);
  int vert = 0;
  int face = 0;
  for (const Crack &crack : cracks) {
    const int first = vert;
    for (const int i : crack.index_range()) {
      const CrackPoint &point = crack[i];
      const float3 next = crack[std::min(i + 1, int(crack.size()) - 1)].position;
      const float3 previous = crack[std::max(i - 1, 0)].position;
      float3 side = math::cross(point.normal, next - previous);
      side = math::length(side) > 1e-12f ? math::normalize(side) : float3(1, 0, 0);
      const float3 center = point.position + point.normal * lift;
      positions[vert++] = center - side * point.half_width;
      positions[vert++] = center + side * point.half_width;
    }
    for (const int i : IndexRange(crack.size() - 1)) {
      offsets[face] = face * 4;
      const int a = first + i * 2;
      corner_verts[face * 4 + 0] = a;
      corner_verts[face * 4 + 1] = a + 1;
      corner_verts[face * 4 + 2] = a + 3;
      corner_verts[face * 4 + 3] = a + 2;
      face++;
    }
  }
  offsets.last() = faces_num * 4;
  bke::mesh_calc_edges(*strips, false, false);
  if (material_index > 0) {
    bke::SpanAttributeWriter<int> materials =
        strips->attributes_for_write().lookup_or_add_for_write_only_span<int>(
            "material_index", bke::AttrDomain::Face);
    if (materials) {
      materials.span.fill(material_index);
      materials.finish();
    }
  }
  strips->tag_positions_changed();
  return strips;
}

static void write_value(Mesh &mesh, const char *name, const Span<float> value)
{
  if (name[0] == '\0') {
    return;
  }
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  attributes.remove(name);
  bke::SpanAttributeWriter<float> writer = attributes.lookup_or_add_for_write_only_span<float>(
      name, bke::AttrDomain::Point);
  if (writer) {
    writer.span.copy_from(value);
    writer.finish();
  }
}

static Mesh *modify_mesh(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  const CracksModifierData &cmd = *reinterpret_cast<CracksModifierData *>(md);
  if (mesh->faces_num == 0 || cmd.length <= 0.0f || cmd.width <= 0.0f) {
    return mesh;
  }
  Vector<Crack> cracks;
  {
    const bke::BVHTreeFromMesh bvh = mesh->bvh_corner_tris();
    if (bvh.tree == nullptr) {
      return mesh;
    }
    cracks = make_cracks(*mesh, cmd, ctx, bvh);
  }
  if (cracks.is_empty()) {
    return mesh;
  }
  if (cmd.mode == MOD_CRACKS_GROOVE) {
    Array<float> value(mesh->verts_num, 0.0f);
    cut_grooves(*mesh, cmd, cracks, value);
    /* Grooves only press in the points that lie on the cracks. */
    if (!std::any_of(value.begin(), value.end(), [](const float v) { return v > 0.0f; })) {
      BKE_modifier_set_error(
          ctx->object, md, "Too few points for the grooves, subdivide the mesh or use Strips");
    }
    write_value(*mesh, cmd.attribute_name, value);
    return mesh;
  }
  const int material_index = MOD_material_slot(
      *mesh, cmd.material_index_pick, cmd.material_index);
  Mesh *strips = make_strips(*mesh, cmd, cracks, material_index);
  write_value(*mesh, cmd.attribute_name, Array<float>(mesh->verts_num, 0.0f));
  write_value(*strips, cmd.attribute_name, Array<float>(strips->verts_num, 1.0f));
  const std::array<bke::GeometrySet, 2> parts = {
      bke::GeometrySet::from_mesh(mesh, bke::GeometryOwnershipType::ReadOnly),
      bke::GeometrySet::from_mesh(strips)};
  bke::GeometrySet joined = geometry::join_geometries(parts, {});
  Mesh *result = joined.get_component_for_write<bke::MeshComponent>().release();
  return result ? result : mesh;
}

static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);
  layout.prop(ptr, "mode", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
  layout.use_property_split_set(true);
  layout.prop(ptr, "object", UI_ITEM_NONE, IFACE_("Impact"), ICON_NONE);
  if (RNA_pointer_get(ptr, "object").data == nullptr) {
    layout.prop(ptr, "impacts", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  layout.prop(ptr, "count", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "length", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "width", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  if (RNA_enum_get(ptr, "mode") == MOD_CRACKS_GROOVE) {
    layout.prop(ptr, "depth", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  else {
    modifier_material_ui(layout, ptr, "material_index_pick", "material_index", std::nullopt);
  }
  layout.prop(ptr, "jitter", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "branching", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "seed", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "attribute_name", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  if (RNA_enum_get(ptr, "mode") == MOD_CRACKS_GROOVE) {
    layout.label(IFACE_("Grooves need a finely divided surface"), ICON_INFO);
  }
  modifier_error_message_draw(layout, ptr);
}

static void panel_register(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_Cracks, panel_draw);
}

ModifierTypeInfo modifierType_Cracks = {
    /*idname*/ "Cracks",
    /*name*/ N_("Cracks"),
    /*struct_name*/ "CracksModifierData",
    /*struct_size*/ sizeof(CracksModifierData),
    /*srna*/ &RNA_CracksModifier,
    /*type*/ ModifierTypeType::Constructive,
    /*flags*/ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsEditmode |
        eModifierTypeFlag_EnableInEditmode,
    /*icon*/ ICON_MOD_CRACKS,

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
