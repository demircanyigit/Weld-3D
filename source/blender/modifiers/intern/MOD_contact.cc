/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Contact: where another object presses into the mesh, the surface is pushed back to it and bulges
 * out around the contact, like a cushion under a hand.
 */

#include <algorithm>
#include <cfloat>

#include "BLI_array.hh"
#include "BLI_kdopbvh.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_task.hh"

#include "BLT_translation.hh"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "BKE_bvhutils.hh"
#include "BKE_deform.hh"
#include "BKE_lib_query.hh"
#include "BKE_mesh.hh"
#include "BKE_modifier.hh"
#include "BKE_object.hh"

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
  ContactModifierData *data = reinterpret_cast<ContactModifierData *>(md);
  INIT_DEFAULT_STRUCT_AFTER(data, modifier);
}

static void required_data_mask(ModifierData *md, CustomData_MeshMasks *r_cddata_masks)
{
  const ContactModifierData &data = *reinterpret_cast<ContactModifierData *>(md);
  if (data.vertex_group[0] != '\0') {
    r_cddata_masks->vmask |= CD_MASK_MDEFORMVERT;
  }
}

static bool is_disabled(const Scene * /*scene*/, ModifierData *md, bool /*use_render_params*/)
{
  const ContactModifierData &data = *reinterpret_cast<ContactModifierData *>(md);
  return data.object == nullptr;
}

static void foreach_ID_link(ModifierData *md, Object *ob, IDWalkFunc walk, void *user_data)
{
  ContactModifierData *data = reinterpret_cast<ContactModifierData *>(md);
  walk(user_data, ob, reinterpret_cast<ID **>(&data->object), IDWALK_CB_NOP);
}

static void update_depsgraph(ModifierData *md, const ModifierUpdateDepsgraphContext *ctx)
{
  const ContactModifierData &data = *reinterpret_cast<ContactModifierData *>(md);
  if (data.object == nullptr || data.object == ctx->object) {
    return;
  }
  DEG_add_object_relation(ctx->node, data.object, DEG_OB_COMP_TRANSFORM, "Contact Modifier");
  DEG_add_object_relation(ctx->node, data.object, DEG_OB_COMP_GEOMETRY, "Contact Modifier");
  DEG_add_depends_on_transform_relation(ctx->node, "Contact Modifier");
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

/** The nearest point of the other mesh, in the space of this mesh. */
struct Nearest {
  bool found = false;
  bool inside = false;
  float3 point;
  float3 normal;
  float distance = FLT_MAX;
};

struct Collider {
  const bke::BVHTreeFromMesh *bvh;
  float4x4 to_other;
  float4x4 to_self;
  float3x3 normal_to_self;

  Nearest nearest(const float3 &p) const
  {
    const float3 local = math::transform_point(to_other, p);
    BVHTreeNearest hit;
    hit.index = -1;
    hit.dist_sq = FLT_MAX;
    BLI_bvhtree_find_nearest(bvh->tree,
                             local,
                             &hit,
                             bvh->nearest_callback,
                             const_cast<bke::BVHTreeFromMesh *>(bvh));
    Nearest result;
    if (hit.index == -1) {
      return result;
    }
    result.found = true;
    result.inside = math::dot(local - float3(hit.co), float3(hit.no)) < 0.0f;
    result.point = math::transform_point(to_self, float3(hit.co));
    result.normal = math::normalize(normal_to_self * float3(hit.no));
    result.distance = math::distance(p, result.point);
    return result;
  }
};

static Mesh *modify_mesh(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  const ContactModifierData &cmd = *reinterpret_cast<ContactModifierData *>(md);
  if (cmd.object == nullptr || cmd.object == ctx->object || mesh->verts_num == 0) {
    return mesh;
  }
  const Mesh *other = cmd.object->type == OB_MESH ? BKE_object_get_evaluated_mesh(cmd.object) :
                                                    nullptr;
  if (other == nullptr || other->faces_num == 0) {
    BKE_modifier_set_error(ctx->object, md, "Choose a mesh object with faces");
    return mesh;
  }
  const bke::BVHTreeFromMesh bvh = other->bvh_corner_tris();
  if (bvh.tree == nullptr) {
    return mesh;
  }
  Collider collider;
  collider.bvh = &bvh;
  collider.to_self = ctx->object->world_to_object() * cmd.object->object_to_world();
  collider.to_other = math::invert(collider.to_self);
  collider.normal_to_self = math::transpose(math::invert(float3x3(collider.to_self)));

  const int verts_num = mesh->verts_num;
  const Array<float> weights = vgroup_weights(
      ctx->object, mesh, cmd.vertex_group, cmd.flag & MOD_CONTACT_INVERT_VGROUP, verts_num);
  const Array<float3> normals(mesh->vert_normals());
  MutableSpan<float3> positions = mesh->vert_positions_for_write();
  const Array<float3> start(positions.as_span());

  /* The squash: points inside go back to the surface of the other mesh. */
  Array<float3> offset(verts_num, float3(0.0f));
  Array<float> outside_distance(verts_num, FLT_MAX);
  Array<float> pressed(verts_num, 0.0f);
  threading::parallel_for(IndexRange(verts_num), 1024, [&](const IndexRange range) {
    for (const int v : range) {
      const Nearest hit = collider.nearest(start[v]);
      if (!hit.found) {
        continue;
      }
      if (hit.inside || hit.distance < cmd.offset) {
        const float3 target = hit.point + hit.normal * cmd.offset;
        offset[v] = target - start[v];
        pressed[v] = math::length(offset[v]);
      }
      else {
        outside_distance[v] = hit.distance - cmd.offset;
      }
    }
  });
  float deepest = 0.0f;
  for (const float depth : pressed) {
    deepest = std::max(deepest, depth);
  }

  /* The bulge: points near the contact swell out along their normals, more for a deeper press. */
  if (deepest > 0.0f && cmd.bulge != 0.0f && cmd.bulge_distance > 0.0f) {
    for (const int v : IndexRange(verts_num)) {
      if (pressed[v] > 0.0f || outside_distance[v] >= cmd.bulge_distance) {
        continue;
      }
      const float t = 1.0f - outside_distance[v] / cmd.bulge_distance;
      offset[v] = normals[v] * (cmd.bulge * deepest * t * t);
    }
  }

  /* Soften the edge of the press by smoothing the offsets along the edges. */
  const Span<int2> edges = mesh->edges();
  for ([[maybe_unused]] const int i : IndexRange(std::max(cmd.smooth, 0))) {
    Array<float3> sum(verts_num, float3(0.0f));
    Array<int> count(verts_num, 0);
    for (const int2 edge : edges) {
      sum[edge[0]] += offset[edge[1]];
      sum[edge[1]] += offset[edge[0]];
      count[edge[0]]++;
      count[edge[1]]++;
    }
    for (const int v : IndexRange(verts_num)) {
      if (count[v] > 0) {
        offset[v] = (offset[v] + sum[v] / float(count[v])) * 0.5f;
      }
    }
  }

  threading::parallel_for(IndexRange(verts_num), 1024, [&](const IndexRange range) {
    for (const int v : range) {
      const float weight = weights.is_empty() ? 1.0f : weights[v];
      float3 p = start[v] + offset[v] * weight;
      /* Smoothing may have pulled a point back in: keep it on the surface. */
      if (weight > 0.0f && cmd.smooth > 0) {
        const Nearest hit = collider.nearest(p);
        if (hit.found && hit.inside) {
          p = hit.point + hit.normal * cmd.offset;
        }
      }
      positions[v] = p;
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
  layout.prop(ptr, "offset", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "bulge", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "bulge_distance", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "smooth", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  modifier_vgroup_ui(layout, ptr, &ob_ptr, "vertex_group", "invert_vertex_group", std::nullopt);
  modifier_error_message_draw(layout, ptr);
}

static void panel_register(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_Contact, panel_draw);
}

ModifierTypeInfo modifierType_Contact = {
    /*idname*/ "Contact",
    /*name*/ N_("Contact"),
    /*struct_name*/ "ContactModifierData",
    /*struct_size*/ sizeof(ContactModifierData),
    /*srna*/ &RNA_ContactModifier,
    /*type*/ ModifierTypeType::Nonconstructive,
    /*flags*/ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsEditmode |
        eModifierTypeFlag_EnableInEditmode,
    /*icon*/ ICON_MOD_CONTACT,

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
    /*is_disabled*/ is_disabled,
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
