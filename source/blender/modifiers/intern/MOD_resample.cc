/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Resample: adds points along the edges. The work is done by shared code in the geometry module,
 * which the Resample Mesh node uses too.
 */

#include <algorithm>

#include "BLI_array.hh"
#include "BLI_math_vector.hh"
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

#include "GEO_randomize.hh"
#include "GEO_resample_mesh.hh"

namespace blender {

static void init_data(ModifierData *md)
{
  ResampleModifierData *rmd = reinterpret_cast<ResampleModifierData *>(md);
  INIT_DEFAULT_STRUCT_AFTER(rmd, modifier);
}

static void required_data_mask(ModifierData *md, CustomData_MeshMasks *r_cddata_masks)
{
  const ResampleModifierData &rmd = *reinterpret_cast<ResampleModifierData *>(md);
  if (rmd.vertex_group[0] != '\0') {
    r_cddata_masks->vmask |= CD_MASK_MDEFORMVERT;
  }
}

/** Edges with both points in the vertex group, or all edges when there is no group. */
static Array<bool> edge_selection(const ResampleModifierData &rmd,
                                  const ModifierEvalContext *ctx,
                                  const Mesh &mesh)
{
  Array<bool> selection(mesh.edges_num, true);
  const MDeformVert *dvert = nullptr;
  int defgrp_index = -1;
  MOD_get_vgroup(ctx->object, &mesh, rmd.vertex_group, &dvert, &defgrp_index);
  if (dvert == nullptr || defgrp_index == -1) {
    return selection;
  }
  const bool invert = rmd.flag & MOD_RESAMPLE_INVERT_VGROUP;
  auto in_group = [&](const int vert) {
    const bool weighted = BKE_defvert_find_weight(&dvert[vert], defgrp_index) > 0.0f;
    return weighted != invert;
  };
  const Span<int2> edges = mesh.edges();
  for (const int i : edges.index_range()) {
    selection[i] = in_group(edges[i][0]) && in_group(edges[i][1]);
  }
  return selection;
}

static Mesh *modify_mesh(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  const ResampleModifierData &rmd = *reinterpret_cast<ResampleModifierData *>(md);
  if (mesh->edges_num == 0) {
    return mesh;
  }

  const Array<bool> selection = edge_selection(rmd, ctx, *mesh);
  const Span<float3> positions = mesh->vert_positions();
  const Span<int2> edges = mesh->edges();
  Array<int> cuts(mesh->edges_num, 0);
  int64_t total_cuts = 0;
  for (const int i : edges.index_range()) {
    if (!selection[i]) {
      continue;
    }
    if (rmd.mode == MOD_RESAMPLE_COUNT) {
      cuts[i] = std::max(rmd.count - 1, 0);
    }
    else {
      cuts[i] = geometry::resample_mesh_cuts_for_length(
          math::distance(positions[edges[i][0]], positions[edges[i][1]]), rmd.length);
    }
    total_cuts += cuts[i];
  }
  if (total_cuts == 0) {
    return mesh;
  }
  if (total_cuts > geometry::resample_mesh_max_new_points) {
    BKE_modifier_set_error(
        ctx->object, md, "The segments are so short that the mesh would get too big");
    return mesh;
  }

  std::optional<std::string> new_points_id;
  if (rmd.new_points_name[0] != '\0') {
    new_points_id = rmd.new_points_name;
  }
  Mesh *result = geometry::resample_mesh(*mesh, cuts, new_points_id);
  geometry::debug_randomize_mesh_order(result);
  return result;
}

static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  layout.use_property_split_set(true);
  layout.prop(ptr, "mode", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
  if (RNA_enum_get(ptr, "mode") == MOD_RESAMPLE_COUNT) {
    layout.prop(ptr, "count", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  else {
    layout.prop(ptr, "length", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  modifier_vgroup_ui(layout, ptr, &ob_ptr, "vertex_group", "invert_vertex_group", std::nullopt);
  layout.prop(ptr, "new_points_name", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  modifier_error_message_draw(layout, ptr);
}

static void panel_register(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_Resample, panel_draw);
}

ModifierTypeInfo modifierType_Resample = {
    /*idname*/ "Resample",
    /*name*/ N_("Resample"),
    /*struct_name*/ "ResampleModifierData",
    /*struct_size*/ sizeof(ResampleModifierData),
    /*srna*/ &RNA_ResampleModifier,
    /*type*/ ModifierTypeType::Constructive,
    /*flags*/ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsEditmode |
        eModifierTypeFlag_EnableInEditmode,
    /*icon*/ ICON_MOD_RESAMPLE,

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
