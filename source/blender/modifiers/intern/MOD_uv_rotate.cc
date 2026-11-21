/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * UV Rotate: turn the UVs by an angle, or turn each island so a direction in world, object or
 * another object's space points the same way in the UVs.
 */

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "BLI_array.hh"
#include "BLI_disjoint_set.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_noise.hh"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "BKE_attribute.hh"
#include "BKE_customdata.hh"
#include "BKE_lib_query.hh"
#include "BKE_mesh.hh"
#include "BKE_modifier.hh"

#include "DEG_depsgraph_build.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "MOD_modifiertypes.hh"
#include "MOD_ui_common.hh"

namespace blender {

static void init_data(ModifierData *md)
{
  UVRotateModifierData *data = reinterpret_cast<UVRotateModifierData *>(md);
  INIT_DEFAULT_STRUCT_AFTER(data, modifier);
}

static void required_data_mask(ModifierData * /*md*/, CustomData_MeshMasks *mask)
{
  mask->lmask |= CD_MASK_PROP_FLOAT2;
}

static void foreach_ID_link(ModifierData *md, Object *ob, IDWalkFunc walk, void *user_data)
{
  UVRotateModifierData *data = reinterpret_cast<UVRotateModifierData *>(md);
  walk(user_data, ob, reinterpret_cast<ID **>(&data->object), IDWALK_CB_NOP);
}

static void update_depsgraph(ModifierData *md, const ModifierUpdateDepsgraphContext *ctx)
{
  const UVRotateModifierData &data = *reinterpret_cast<UVRotateModifierData *>(md);
  if (data.mode != MOD_UV_ROTATE_ALIGN) {
    return;
  }
  if (data.space == MOD_UV_ROTATE_SPACE_WORLD) {
    DEG_add_depends_on_transform_relation(ctx->node, "UV Rotate Modifier");
  }
  else if (data.space == MOD_UV_ROTATE_SPACE_CUSTOM && data.object != nullptr &&
           data.object != ctx->object)
  {
    DEG_add_object_relation(ctx->node, data.object, DEG_OB_COMP_TRANSFORM, "UV Rotate Modifier");
    DEG_add_depends_on_transform_relation(ctx->node, "UV Rotate Modifier");
  }
}

/** UV islands: corners of a face belong together, and corners of a point with the same UV. */
static Array<int> uv_islands(const Mesh &mesh, const Span<float2> uvs, int &r_islands_num)
{
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  DisjointSet<int> sets(uvs.size());
  for (const int face : faces.index_range()) {
    const IndexRange corners = faces[face];
    for (const int corner : corners.drop_front(1)) {
      sets.join(corners.first(), corner);
    }
  }
  Array<int> order(uvs.size());
  for (const int i : order.index_range()) {
    order[i] = i;
  }
  std::sort(order.begin(), order.end(), [&](const int a, const int b) {
    if (corner_verts[a] != corner_verts[b]) {
      return corner_verts[a] < corner_verts[b];
    }
    if (uvs[a].x != uvs[b].x) {
      return uvs[a].x < uvs[b].x;
    }
    return uvs[a].y < uvs[b].y;
  });
  for (const int i : order.index_range().drop_front(1)) {
    const int a = order[i - 1];
    const int b = order[i];
    if (corner_verts[a] == corner_verts[b] && math::distance(uvs[a], uvs[b]) < 1e-5f) {
      sets.join(a, b);
    }
  }
  Array<int> island_of_root(uvs.size(), -1);
  Array<int> island(uvs.size());
  r_islands_num = 0;
  for (const int corner : uvs.index_range()) {
    int &id = island_of_root[sets.find_root(corner)];
    if (id == -1) {
      id = r_islands_num++;
    }
    island[corner] = id;
  }
  return island;
}

/** The axis, in the space of this mesh. */
static float3 align_axis(const UVRotateModifierData &umd, const ModifierEvalContext *ctx)
{
  float3 axis(0.0f);
  axis[std::clamp(int(umd.axis), 0, 2)] = 1.0f;
  if (umd.space == MOD_UV_ROTATE_SPACE_WORLD) {
    axis = math::transform_direction(ctx->object->world_to_object(), axis);
  }
  else if (umd.space == MOD_UV_ROTATE_SPACE_CUSTOM && umd.object != nullptr) {
    axis = math::transform_direction(
        ctx->object->world_to_object() * umd.object->object_to_world(), axis);
  }
  return math::length(axis) > 0.0f ? math::normalize(axis) : float3(0, 0, 1);
}

/** Per island, the direction the axis runs in the UVs, summed over the faces. */
static Array<float2> axis_in_uvs(const Mesh &mesh,
                                 const Span<float2> uvs,
                                 const Span<int> island,
                                 const int islands_num,
                                 const float3 &axis)
{
  Array<float2> sum(islands_num, float2(0.0f));
  const Span<float3> positions = mesh.vert_positions();
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  for (const int face : faces.index_range()) {
    const IndexRange corners = faces[face];
    const int c0 = corners.first();
    for (const int k : IndexRange(1, corners.size() - 2)) {
      const int c1 = corners[k];
      const int c2 = corners[k + 1];
      const float3 e1 = positions[corner_verts[c1]] - positions[corner_verts[c0]];
      const float3 e2 = positions[corner_verts[c2]] - positions[corner_verts[c0]];
      /* The axis as a mix of the two edges, solved in the plane of the triangle. */
      const float a11 = math::dot(e1, e1);
      const float a12 = math::dot(e1, e2);
      const float a22 = math::dot(e2, e2);
      const float det = a11 * a22 - a12 * a12;
      if (std::abs(det) < 1e-20f) {
        continue;
      }
      const float b1 = math::dot(axis, e1);
      const float b2 = math::dot(axis, e2);
      const float s = (b1 * a22 - b2 * a12) / det;
      const float t = (b2 * a11 - b1 * a12) / det;
      const float2 d1 = uvs[c1] - uvs[c0];
      const float2 d2 = uvs[c2] - uvs[c0];
      /* Weighted by the area, so big faces count more. */
      const float area = std::sqrt(std::max(det, 0.0f));
      sum[island[c0]] += (d1 * s + d2 * t) * area;
    }
  }
  return sum;
}

static float snap_angle(const float angle, const float step)
{
  return step > 1e-6f ? std::round(angle / step) * step : angle;
}

static Mesh *modify_mesh(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  const UVRotateModifierData &umd = *reinterpret_cast<UVRotateModifierData *>(md);
  if (mesh->corners_num == 0) {
    return mesh;
  }
  const std::string name = umd.uv_map[0] ? std::string(umd.uv_map) :
                                           std::string(mesh->active_uv_map_name());
  bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
  const std::optional<bke::AttributeMetaData> meta = attributes.lookup_meta_data(name);
  if (name.empty() || !meta || meta->domain != bke::AttrDomain::Corner ||
      meta->data_type != bke::AttrType::Float2)
  {
    BKE_modifier_set_error(ctx->object, md, "The mesh has no UV map");
    return mesh;
  }
  bke::SpanAttributeWriter<float2> uvs = attributes.lookup_for_write_span<float2>(name);
  if (!uvs) {
    return mesh;
  }
  int islands_num = 0;
  const Array<int> island = uv_islands(*mesh, uvs.span, islands_num);

  /* Island centers, from their UV bounds. */
  Array<float2> lo(islands_num, float2(FLT_MAX));
  Array<float2> hi(islands_num, float2(-FLT_MAX));
  float2 all_lo(FLT_MAX);
  float2 all_hi(-FLT_MAX);
  for (const int corner : uvs.span.index_range()) {
    const float2 uv = uvs.span[corner];
    lo[island[corner]] = math::min(lo[island[corner]], uv);
    hi[island[corner]] = math::max(hi[island[corner]], uv);
    all_lo = math::min(all_lo, uv);
    all_hi = math::max(all_hi, uv);
  }

  Array<float> rotation(islands_num, umd.angle);
  if (umd.mode == MOD_UV_ROTATE_ALIGN) {
    const Array<float2> direction = axis_in_uvs(
        *mesh, uvs.span, island, islands_num, align_axis(umd, ctx));
    /* Where the axis should point in the UVs. */
    static const float targets[4] = {
        float(M_PI) * 0.5f, 0.0f, -float(M_PI) * 0.5f, float(M_PI)};
    const float target = targets[std::clamp(int(umd.align_to), 0, 3)];
    for (const int i : IndexRange(islands_num)) {
      if (math::length(direction[i]) < 1e-12f) {
        continue;
      }
      rotation[i] += target - std::atan2(direction[i].y, direction[i].x);
    }
  }
  for (const int i : IndexRange(islands_num)) {
    if (umd.angle_random != 0.0f) {
      const float2 c = (lo[i] + hi[i]) * 0.5f;
      const float r = noise::hash_float_to_float(float3(c.x, c.y, float(umd.seed) * 1.618f));
      rotation[i] += umd.angle_random * (r * 2.0f - 1.0f);
    }
    rotation[i] = snap_angle(rotation[i], umd.snap);
  }

  for (const int corner : uvs.span.index_range()) {
    const int i = island[corner];
    float2 pivot;
    switch (umd.pivot_mode) {
      case MOD_UV_ROTATE_PIVOT_ISLAND:
        pivot = (lo[i] + hi[i]) * 0.5f;
        break;
      case MOD_UV_ROTATE_PIVOT_BOUNDS:
        pivot = (all_lo + all_hi) * 0.5f;
        break;
      case MOD_UV_ROTATE_PIVOT_CUSTOM:
        pivot = float2(umd.pivot);
        break;
      default:
        pivot = float2(0.5f);
        break;
    }
    const float c = std::cos(rotation[i]);
    const float s = std::sin(rotation[i]);
    const float2 d = uvs.span[corner] - pivot;
    uvs.span[corner] = pivot + float2(d.x * c - d.y * s, d.x * s + d.y * c);
  }
  uvs.finish();
  return mesh;
}

static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);
  PointerRNA mesh_ptr = RNA_pointer_get(&ob_ptr, "data");
  layout.prop(ptr, "mode", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
  layout.use_property_split_set(true);
  layout.prop_search(ptr, "uv_map", &mesh_ptr, "uv_layers", std::nullopt, ICON_GROUP_UVS);
  if (RNA_enum_get(ptr, "mode") == MOD_UV_ROTATE_ALIGN) {
    layout.prop(ptr, "space", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    if (RNA_enum_get(ptr, "space") == MOD_UV_ROTATE_SPACE_CUSTOM) {
      layout.prop(ptr, "object", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    }
    layout.prop(ptr, "axis", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
    layout.prop(ptr, "align_to", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    layout.prop(ptr, "angle", UI_ITEM_NONE, IFACE_("Extra Angle"), ICON_NONE);
  }
  else {
    layout.prop(ptr, "angle", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  layout.prop(ptr, "angle_random", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  if (RNA_float_get(ptr, "angle_random") != 0.0f) {
    layout.prop(ptr, "seed", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  layout.prop(ptr, "snap", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "pivot_mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  if (RNA_enum_get(ptr, "pivot_mode") == MOD_UV_ROTATE_PIVOT_CUSTOM) {
    layout.prop(ptr, "pivot", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  modifier_error_message_draw(layout, ptr);
}

static void panel_register(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_UVRotate, panel_draw);
}

ModifierTypeInfo modifierType_UVRotate = {
    /*idname*/ "UVRotate",
    /*name*/ N_("UV Rotate"),
    /*struct_name*/ "UVRotateModifierData",
    /*struct_size*/ sizeof(UVRotateModifierData),
    /*srna*/ &RNA_UVRotateModifier,
    /*type*/ ModifierTypeType::Nonconstructive,
    /*flags*/ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsEditmode |
        eModifierTypeFlag_EnableInEditmode,
    /*icon*/ ICON_MOD_UV_ROTATE,

    /*copy_data*/ BKE_modifier_copydata_generic,

    /*deform_verts*/ nullptr,
    /*deform_matrices*/ nullptr,
    /*deform_verts_EM*/ nullptr,
    /*deform_matrices_EM*/ nullptr,
    /*modify_mesh*/ modify_mesh,
    /*modify_geometry_set*/ nullptr,

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
