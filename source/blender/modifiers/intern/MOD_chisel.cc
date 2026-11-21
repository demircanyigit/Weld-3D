/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Chisel: flat hand-cut chips on the sharp edges of boards and beams. The work is done by shared
 * code in the geometry module.
 */

#include <algorithm>

#include "BLI_array.hh"
#include "BLI_math_base.h"

#include "BLT_translation.hh"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "BKE_deform.hh"
#include "BKE_mesh.hh"
#include "BKE_lib_query.hh"
#include "BKE_modifier.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "GEO_mesh_chisel.hh"

#include "MOD_modifiertypes.hh"
#include "MOD_ui_common.hh"
#include "MOD_util.hh"

namespace blender {

static void init_data(ModifierData *md)
{
  ChiselModifierData *cmd = reinterpret_cast<ChiselModifierData *>(md);
  INIT_DEFAULT_STRUCT_AFTER(cmd, modifier);
}

static void required_data_mask(ModifierData *md, CustomData_MeshMasks *r_cddata_masks)
{
  const ChiselModifierData &cmd = *reinterpret_cast<ChiselModifierData *>(md);
  if (cmd.vertex_group[0] != '\0') {
    r_cddata_masks->vmask |= CD_MASK_MDEFORMVERT;
  }
}

static Mesh *modify_mesh(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  const ChiselModifierData &cmd = *reinterpret_cast<ChiselModifierData *>(md);
  if (mesh->faces_num == 0) {
    return mesh;
  }

  geometry::MeshChiselParams params;
  params.amount = cmd.amount;
  params.mix = cmd.mix;
  params.length_min = std::min(cmd.length_min, cmd.length_max);
  params.length_max = std::max(cmd.length_min, cmd.length_max);
  params.depth_min = std::min(cmd.depth_min, cmd.depth_max);
  params.depth_max = std::max(cmd.depth_min, cmd.depth_max);
  params.edge_angle = cmd.edge_angle;
  params.ends = cmd.flag & MOD_CHISEL_ENDS;
  params.seed = cmd.seed;
  params.material_index = MOD_material_slot(*mesh, cmd.material_index_pick, cmd.material_index);
  if (cmd.cut_name[0] != '\0') {
    params.cut_id = cmd.cut_name;
  }

  /* Edges with both points in the group. */
  Array<bool> selection;
  const MDeformVert *dvert = nullptr;
  int defgrp_index = -1;
  MOD_get_vgroup(ctx->object, mesh, cmd.vertex_group, &dvert, &defgrp_index);
  if (dvert != nullptr) {
    const bool invert = cmd.flag & MOD_CHISEL_INVERT_VGROUP;
    auto in_group = [&](const int v) {
      return (BKE_defvert_find_weight(&dvert[v], defgrp_index) > 0.0f) != invert;
    };
    const Span<int2> edges = mesh->edges();
    selection.reinitialize(edges.size());
    for (const int i : edges.index_range()) {
      selection[i] = in_group(edges[i][0]) && in_group(edges[i][1]);
    }
    params.edge_selection = selection;
  }

  const char *error = nullptr;
  Mesh *result = geometry::mesh_chisel(*mesh, params, &error);
  if (result != nullptr && result != mesh) {
    MOD_material_slots_from(*result, *mesh);
  }
  if (error != nullptr) {
    BKE_modifier_set_error(ctx->object, md, "%s", error);
  }
  return result ? result : mesh;
}

static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  layout.use_property_split_set(true);
  layout.prop(ptr, "amount", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "mix", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  ui::Layout &length = layout.column(true);
  length.prop(ptr, "length_min", UI_ITEM_NONE, IFACE_("Length Min"), ICON_NONE);
  length.prop(ptr, "length_max", UI_ITEM_NONE, IFACE_("Max"), ICON_NONE);
  ui::Layout &depth = layout.column(true);
  depth.prop(ptr, "depth_min", UI_ITEM_NONE, IFACE_("Depth Min"), ICON_NONE);
  depth.prop(ptr, "depth_max", UI_ITEM_NONE, IFACE_("Max"), ICON_NONE);

  layout.prop(ptr, "edge_angle", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "use_ends", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "seed", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  modifier_vgroup_ui(layout, ptr, &ob_ptr, "vertex_group", "invert_vertex_group", std::nullopt);
  modifier_material_ui(layout, ptr, "material_index_pick", "material_index", std::nullopt);
  layout.prop(ptr, "cut_name", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  modifier_error_message_draw(layout, ptr);
}

static void panel_register(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_Chisel, panel_draw);
}

static void foreach_ID_link(ModifierData *md, Object *ob, IDWalkFunc walk, void *user_data)
{
  ChiselModifierData *pick_data = reinterpret_cast<ChiselModifierData *>(md);
  walk(user_data, ob, reinterpret_cast<ID **>(&pick_data->material_index_pick), IDWALK_CB_USER);
}

ModifierTypeInfo modifierType_Chisel = {
    /*idname*/ "Chisel",
    /*name*/ N_("Chisel"),
    /*struct_name*/ "ChiselModifierData",
    /*struct_size*/ sizeof(ChiselModifierData),
    /*srna*/ &RNA_ChiselModifier,
    /*type*/ ModifierTypeType::Constructive,
    /*flags*/ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsEditmode,
    /*icon*/ ICON_MOD_CHISEL,

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
    /*update_depsgraph*/ nullptr,
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
