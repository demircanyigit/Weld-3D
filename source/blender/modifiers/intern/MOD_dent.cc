/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Dent: press round dents into the surface, at an object or at random spots, with a raised rim.
 */

#include <algorithm>
#include <cmath>

#include "BLI_array.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_noise.hh"
#include "BLI_rand.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "BKE_deform.hh"
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
#include "MOD_util.hh"

namespace blender {

static void init_data(ModifierData *md)
{
  DentModifierData *data = reinterpret_cast<DentModifierData *>(md);
  INIT_DEFAULT_STRUCT_AFTER(data, modifier);
}

static void required_data_mask(ModifierData *md, CustomData_MeshMasks *r_cddata_masks)
{
  const DentModifierData &data = *reinterpret_cast<DentModifierData *>(md);
  if (data.vertex_group[0] != '\0') {
    r_cddata_masks->vmask |= CD_MASK_MDEFORMVERT;
  }
}

static void foreach_ID_link(ModifierData *md, Object *ob, IDWalkFunc walk, void *user_data)
{
  DentModifierData *data = reinterpret_cast<DentModifierData *>(md);
  walk(user_data, ob, reinterpret_cast<ID **>(&data->object), IDWALK_CB_NOP);
}

static void update_depsgraph(ModifierData *md, const ModifierUpdateDepsgraphContext *ctx)
{
  const DentModifierData &data = *reinterpret_cast<DentModifierData *>(md);
  if (data.object != nullptr && data.object != ctx->object) {
    DEG_add_object_relation(ctx->node, data.object, DEG_OB_COMP_TRANSFORM, "Dent Modifier");
    DEG_add_depends_on_transform_relation(ctx->node, "Dent Modifier");
  }
}

/** Weights of a vertex group, or an empty array when there is none. */
static Array<float> vgroup_weights(const Object *ob,
                                   const Mesh *mesh,
                                   const char *name,
                                   const bool invert,
                                   const int verts_num)
{
  if (mesh == nullptr || name[0] == '\0') {
    return {};
  }
  const MDeformVert *dvert = nullptr;
  int defgrp_index = -1;
  MOD_get_vgroup(ob, mesh, name, &dvert, &defgrp_index);
  if (dvert == nullptr || defgrp_index == -1) {
    return {};
  }
  Array<float> weights(verts_num);
  for (const int i : IndexRange(verts_num)) {
    const float weight = BKE_defvert_find_weight(&dvert[i], defgrp_index);
    weights[i] = invert ? 1.0f - weight : weight;
  }
  return weights;
}

struct Dent {
  float3 center;
  /** Into the surface, for the Object direction. */
  float3 inward;
  float radius;
  float depth;
};

/** Random spots on the faces, by area, each with the face normal pointing out. */
static Vector<Dent> random_dents(const Mesh &mesh, const DentModifierData &dmd)
{
  Vector<Dent> dents;
  const Span<float3> positions = mesh.vert_positions();
  const Span<int3> tris = mesh.corner_tris();
  const Span<int> corner_verts = mesh.corner_verts();
  if (tris.is_empty()) {
    return dents;
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
  if (total <= 0.0f) {
    return dents;
  }
  RandomNumberGenerator rng(uint32_t(dmd.seed) * 2654435761u + 13u);
  for ([[maybe_unused]] const int i : IndexRange(dmd.count)) {
    const float pick = rng.get_float() * total;
    const int tri = std::min(int(std::lower_bound(cumulative.begin(), cumulative.end(), pick) -
                                 cumulative.begin()),
                             int(tris.size()) - 1);
    const float3 &a = positions[corner_verts[tris[tri][0]]];
    const float3 &b = positions[corner_verts[tris[tri][1]]];
    const float3 &c = positions[corner_verts[tris[tri][2]]];
    float u = rng.get_float();
    float v = rng.get_float();
    if (u + v > 1.0f) {
      u = 1.0f - u;
      v = 1.0f - v;
    }
    const float3 normal = math::normalize(math::cross(b - a, c - a));
    const float size = 1.0f - std::clamp(dmd.radius_random, 0.0f, 1.0f) * rng.get_float();
    dents.append({a + (b - a) * u + (c - a) * v, -normal, dmd.radius * size, dmd.depth * size});
  }
  return dents;
}

/** Depth of the dent at a distance, as a share of its radius: the bowl, then the rim. */
static float dent_profile(const float u, const float sharpness, const float rim)
{
  float value = 0.0f;
  if (u < 1.0f) {
    const float bowl = (1.0f - u * u) * (1.0f - u * u);
    const float cone = 1.0f - u;
    value = bowl + (cone - bowl) * std::clamp(sharpness, 0.0f, 1.0f);
  }
  /* The rim: pushed out around the edge, where the material went. */
  const float x = (u - 1.0f) / 0.22f;
  value -= rim * std::exp(-x * x);
  return value;
}

static Mesh *modify_mesh(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  const DentModifierData &dmd = *reinterpret_cast<DentModifierData *>(md);
  if (mesh->verts_num == 0 || dmd.radius <= 0.0f) {
    return mesh;
  }
  Vector<Dent> dents;
  if (dmd.object != nullptr) {
    const float4x4 to_self = ctx->object->world_to_object() * dmd.object->object_to_world();
    const float3 down = math::normalize(math::transform_direction(to_self, float3(0, 0, -1)));
    dents.append({to_self.location(), down, dmd.radius, dmd.depth});
  }
  else if (dmd.count > 0) {
    dents = random_dents(*mesh, dmd);
  }
  else {
    dents.append({float3(0.0f), float3(0, 0, -1), dmd.radius, dmd.depth});
  }
  if (dents.is_empty()) {
    return mesh;
  }
  const Array<float> weights = vgroup_weights(
      ctx->object, mesh, dmd.vertex_group, dmd.flag & MOD_DENT_INVERT_VGROUP, mesh->verts_num);
  const Array<float3> normals(mesh->vert_normals());
  MutableSpan<float3> positions = mesh->vert_positions_for_write();
  const bool use_normals = dmd.direction == MOD_DENT_DIR_NORMAL ||
                           (dmd.count > 0 && dmd.object == nullptr);

  threading::parallel_for(positions.index_range(), 2048, [&](const IndexRange range) {
    for (const int v : range) {
      const float weight = weights.is_empty() ? 1.0f : weights[v];
      if (weight <= 0.0f) {
        continue;
      }
      const float3 p = positions[v];
      float3 offset(0.0f);
      for (const Dent &dent : dents) {
        const float distance = math::distance(p, dent.center);
        const float u = distance / dent.radius;
        if (u > 1.6f) {
          continue;
        }
        float amount = dent_profile(u, dmd.sharpness, dmd.rim);
        if (dmd.roughness > 0.0f) {
          const float n = noise::perlin(p / dent.radius * 4.0f + float3(float(dmd.seed) * 7.31f));
          amount *= 1.0f + dmd.roughness * (n - 0.5f) * 2.0f;
        }
        float3 inward;
        if (use_normals) {
          inward = -normals[v];
        }
        else if (dmd.direction == MOD_DENT_DIR_CENTER && distance > 1e-6f) {
          inward = (p - dent.center) / distance;
        }
        else {
          inward = dent.inward;
        }
        offset += inward * (dent.depth * amount);
      }
      positions[v] = p + offset * weight;
    }
  });
  mesh->tag_positions_changed();
  return mesh;
}

static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);
  layout.use_property_split_set(true);
  layout.prop(ptr, "object", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  const bool has_object = RNA_pointer_get(ptr, "object").data != nullptr;
  if (!has_object) {
    layout.prop(ptr, "count", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  const bool random = !has_object && RNA_int_get(ptr, "count") > 0;
  if (!random) {
    layout.prop(ptr, "direction", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  layout.prop(ptr, "radius", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "depth", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "rim", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "sharpness", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "roughness", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  if (random) {
    layout.prop(ptr, "radius_random", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  if (random || RNA_float_get(ptr, "roughness") > 0.0f) {
    layout.prop(ptr, "seed", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  modifier_vgroup_ui(layout, ptr, &ob_ptr, "vertex_group", "invert_vertex_group", std::nullopt);
  modifier_error_message_draw(layout, ptr);
}

static void panel_register(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_Dent, panel_draw);
}

ModifierTypeInfo modifierType_Dent = {
    /*idname*/ "Dent",
    /*name*/ N_("Dent"),
    /*struct_name*/ "DentModifierData",
    /*struct_size*/ sizeof(DentModifierData),
    /*srna*/ &RNA_DentModifier,
    /*type*/ ModifierTypeType::Nonconstructive,
    /*flags*/ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsEditmode |
        eModifierTypeFlag_EnableInEditmode,
    /*icon*/ ICON_MOD_DENT,

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
