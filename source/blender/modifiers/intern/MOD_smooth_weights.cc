/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Smooth Weights: blurs vertex group weights along the edges of the mesh, so they don't jump
 * across gaps.
 */

#include "BLI_array.hh"

#include "BLT_translation.hh"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "BKE_deform.hh"
#include "BKE_mesh.hh"
#include "BKE_modifier.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "MOD_modifiertypes.hh"
#include "MOD_ui_common.hh"
#include "MOD_util.hh"
#include "MOD_weights_util.hh"

namespace blender {

static void init_data(ModifierData *md)
{
  SmoothWeightsModifierData *smd = reinterpret_cast<SmoothWeightsModifierData *>(md);
  INIT_DEFAULT_STRUCT_AFTER(smd, modifier);
}

static void required_data_mask(ModifierData * /*md*/, CustomData_MeshMasks *r_cddata_masks)
{
  r_cddata_masks->vmask |= CD_MASK_MDEFORMVERT;
}

static bool is_disabled(const Scene * /*scene*/, ModifierData *md, bool /*use_render_params*/)
{
  const SmoothWeightsModifierData &smd = *reinterpret_cast<SmoothWeightsModifierData *>(md);
  return smd.iterations <= 0 || smd.factor <= 0.0f;
}

static void update_depsgraph(ModifierData * /*md*/, const ModifierUpdateDepsgraphContext *ctx)
{
  weights::add_armature_relations(ctx, "Smooth Weights");
}

static Mesh *modify_mesh(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  const SmoothWeightsModifierData &smd = *reinterpret_cast<const SmoothWeightsModifierData *>(md);
  if (mesh->verts_num == 0 || mesh->deform_verts().is_empty()) {
    return mesh;
  }
  Object *armature = weights::find_armature(*ctx->object);
  if (smd.groups == MOD_WEIGHT_GROUPS_SELECTED_BONES && armature == nullptr) {
    BKE_modifier_set_error(ctx->object, md, "No armature found for the selected bones");
    return mesh;
  }
  const Array<bool> mask = weights::group_mask(*mesh, armature, smd.groups, smd.group);
  if (!mask.as_span().contains(true)) {
    return mesh;
  }

  /* Where to smooth. */
  Array<float> influence;
  const MDeformVert *dvert = nullptr;
  int defgrp_index = -1;
  MOD_get_vgroup(ctx->object, mesh, smd.vertex_group, &dvert, &defgrp_index);
  if (dvert != nullptr && defgrp_index != -1) {
    const bool invert = smd.flag & MOD_SMOOTHWEIGHTS_INVERT_VGROUP;
    influence.reinitialize(mesh->verts_num);
    for (const int i : influence.index_range()) {
      const float w = BKE_defvert_find_weight(&dvert[i], defgrp_index);
      influence[i] = invert ? 1.0f - w : w;
    }
  }

  MutableSpan<MDeformVert> dverts = mesh->deform_verts_for_write();
  weights::smooth_groups(*mesh, dverts, mask, smd.iterations, smd.factor, influence);
  if (smd.flag & MOD_SMOOTHWEIGHTS_NORMALIZE) {
    weights::limit_and_normalize(dverts, mask, 0, true);
  }
  return mesh;
}

static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  layout.use_property_split_set(true);
  layout.prop(ptr, "groups", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  if (RNA_enum_get(ptr, "groups") == MOD_WEIGHT_GROUPS_SINGLE) {
    layout.prop_search(ptr, "group", &ob_ptr, "vertex_groups", std::nullopt, ICON_GROUP_VERTEX);
  }
  layout.prop(ptr, "factor", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "iterations", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "use_normalize", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  modifier_vgroup_ui(layout, ptr, &ob_ptr, "vertex_group", "invert_vertex_group", IFACE_("Mask"));
  modifier_error_message_draw(layout, ptr);
}

static void panel_register(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_SmoothWeights, panel_draw);
}

ModifierTypeInfo modifierType_SmoothWeights = {
    /*idname*/ "SmoothWeights",
    /*name*/ N_("Smooth Weights"),
    /*struct_name*/ "SmoothWeightsModifierData",
    /*struct_size*/ sizeof(SmoothWeightsModifierData),
    /*srna*/ &RNA_SmoothWeightsModifier,
    /*type*/ ModifierTypeType::NonGeometrical,
    /*flags*/ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsMapping |
        eModifierTypeFlag_SupportsEditmode,
    /*icon*/ ICON_MOD_SMOOTH_WEIGHTS,
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
