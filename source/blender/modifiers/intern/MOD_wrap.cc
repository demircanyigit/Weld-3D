/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Wrap: straps, tubes or ropes laid over an object along the loose edges of the mesh or along
 * pattern lines. Without a target, the mesh's own faces are wrapped by its loose edges.
 */

#include "BLI_array.hh"
#include "BLI_math_matrix.hh"
#include "BLI_utildefines.h"

#include "BLT_translation.hh"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "BKE_deform.hh"
#include "BKE_geometry_set.hh"
#include "BKE_lib_query.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_wrapper.hh"
#include "BKE_modifier.hh"

#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "GEO_join_geometries.hh"
#include "GEO_mesh_copy_selection.hh"
#include "GEO_mesh_wrap.hh"

#include "MEM_guardedalloc.h"

#include "MOD_modifiertypes.hh"
#include "MOD_ui_common.hh"
#include "MOD_util.hh"

namespace blender {

static void init_data(ModifierData *md)
{
  WrapModifierData *wmd = reinterpret_cast<WrapModifierData *>(md);
  INIT_DEFAULT_STRUCT_AFTER(wmd, modifier);
}

static void required_data_mask(ModifierData *md, CustomData_MeshMasks *r_cddata_masks)
{
  const WrapModifierData *wmd = reinterpret_cast<WrapModifierData *>(md);
  if (wmd->vertex_group[0] != '\0') {
    r_cddata_masks->vmask |= CD_MASK_MDEFORMVERT;
  }
}

static void foreach_ID_link(ModifierData *md, Object *ob, IDWalkFunc walk, void *user_data)
{
  WrapModifierData *wmd = reinterpret_cast<WrapModifierData *>(md);
  walk(user_data, ob, reinterpret_cast<ID **>(&wmd->target), IDWALK_CB_NOP);
  WrapModifierData *pick_data = reinterpret_cast<WrapModifierData *>(md);
  walk(user_data, ob, reinterpret_cast<ID **>(&pick_data->material_index_pick), IDWALK_CB_USER);
}

static void update_depsgraph(ModifierData *md, const ModifierUpdateDepsgraphContext *ctx)
{
  const WrapModifierData *wmd = reinterpret_cast<WrapModifierData *>(md);
  if (wmd->target != nullptr && wmd->target != ctx->object) {
    DEG_add_object_relation(ctx->node, wmd->target, DEG_OB_COMP_GEOMETRY, "Wrap Modifier");
    DEG_add_object_relation(ctx->node, wmd->target, DEG_OB_COMP_TRANSFORM, "Wrap Modifier");
    DEG_add_depends_on_transform_relation(ctx->node, "Wrap Modifier");
  }
}

static Mesh *modify_mesh(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  const WrapModifierData &wmd = *reinterpret_cast<WrapModifierData *>(md);
  const bool self = wmd.target == nullptr || wmd.target == ctx->object;

  geometry::MeshWrapParams params;
  if (self) {
    if (mesh->faces_num == 0) {
      BKE_modifier_set_error(ctx->object, md, "Pick a target, or add faces to wrap");
      return mesh;
    }
    params.target = mesh;
  }
  else {
    Mesh *target_mesh = BKE_modifier_get_evaluated_mesh_from_evaluated_object(wmd.target);
    if (target_mesh == nullptr) {
      return mesh;
    }
    BKE_mesh_wrapper_ensure_mdata(target_mesh);
    params.target = target_mesh;
    params.target_to_space = math::invert(ctx->object->object_to_world()) *
                             wmd.target->object_to_world();
  }

  params.use_edges = wmd.lines != MOD_WRAP_LINES_PATTERN;
  params.use_pattern = wmd.lines != MOD_WRAP_LINES_EDGES;
  params.lines_only = wmd.flag & MOD_WRAP_LINES_ONLY;
  if (params.lines_only) {
    params.use_pattern = true;
  }
  params.pattern = geometry::WrapPattern(wmd.pattern);
  params.axis = wmd.axis;
  params.count = wmd.count;
  params.group = wmd.group;
  params.turns = wmd.turns;
  params.tilt = wmd.tilt;
  params.start = wmd.start;
  params.end = wmd.end;
  params.spacing = wmd.spacing;
  params.random = wmd.random;
  params.wobble = wmd.wobble;
  params.seed = wmd.seed;
  params.shape = geometry::WrapShape(wmd.shape);
  params.width = wmd.width;
  params.thickness = wmd.thickness;
  params.strands = wmd.strands;
  params.twist = wmd.twist;
  params.resolution = wmd.resolution;
  params.gap = wmd.gap;
  params.step = wmd.step;
  params.tight = wmd.tight;
  params.crossings = wmd.flag & MOD_WRAP_CROSSINGS;
  params.crossings_flip = wmd.flag & MOD_WRAP_CROSSINGS_FLIP;
  params.width_random = wmd.width_random;
  params.end_cut = geometry::WrapEndCut(wmd.end_cut);
  params.end_cut_depth = wmd.end_cut_depth;
  params.material_index = MOD_material_slot(*mesh, wmd.material_index_pick, wmd.material_index);

  Array<bool> pin;
  const MDeformVert *dvert = nullptr;
  int defgrp_index = -1;
  MOD_get_vgroup(ctx->object, mesh, wmd.vertex_group, &dvert, &defgrp_index);
  if (dvert != nullptr) {
    const bool invert = wmd.flag & MOD_WRAP_INVERT_VGROUP;
    pin.reinitialize(mesh->verts_num);
    for (const int i : pin.index_range()) {
      pin[i] = (BKE_defvert_find_weight(&dvert[i], defgrp_index) > 0.0f) != invert;
    }
    params.pin = pin;
  }

  const char *error = nullptr;
  Mesh *wrap = geometry::mesh_wrap(*mesh, params, &error);
  if (error != nullptr) {
    BKE_modifier_set_error(ctx->object, md, "%s", error);
  }
  if (wrap == nullptr) {
    return mesh;
  }
  /* The strap uses the object's material slots, so its Material number picks one of them. */
  if (mesh->mat != nullptr && mesh->totcol > 0) {
    MEM_SAFE_DELETE(wrap->mat);
    wrap->mat = MEM_dupalloc(mesh->mat);
    wrap->totcol = mesh->totcol;
  }
  if (!self || params.lines_only) {
    return wrap;
  }

  /* The mesh's own faces, without the loose edges that were drawn, together with the wrap. */
  Mesh *faces = nullptr;
  if (mesh->loose_edges().count > 0) {
    std::optional<Mesh *> copy = geometry::mesh_copy_selection(
        *mesh, VArray<bool>::from_single(true, mesh->faces_num), bke::AttrDomain::Face);
    if (copy.has_value() && *copy != nullptr) {
      faces = *copy;
    }
  }
  if (faces == nullptr) {
    faces = BKE_mesh_copy_for_eval(*mesh);
  }
  const bke::GeometrySet parts[2] = {bke::GeometrySet::from_mesh(faces),
                                     bke::GeometrySet::from_mesh(wrap)};
  bke::GeometrySet joined = geometry::join_geometries(Span<bke::GeometrySet>(parts, 2), {});
  if (!joined.has_mesh()) {
    return mesh;
  }
  return joined.get_component_for_write<bke::MeshComponent>().release();
}

static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  layout.use_property_split_set(true);
  layout.prop(ptr, "target", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "lines", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  if (RNA_enum_get(ptr, "lines") != MOD_WRAP_LINES_EDGES) {
    ui::Layout &col = layout.column(false);
    col.prop(ptr, "pattern", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col.prop(ptr, "axis", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    const int pattern = RNA_enum_get(ptr, "pattern");
    col.prop(ptr, "count", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    if (pattern == MOD_WRAP_PATTERN_BANDS) {
      col.prop(ptr, "group", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      col.prop(ptr, "spacing", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    }
    if (ELEM(pattern, MOD_WRAP_PATTERN_SPIRAL, MOD_WRAP_PATTERN_CROSS, MOD_WRAP_PATTERN_RANDOM))
    {
      col.prop(ptr, "turns", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    }
    col.prop(ptr, "tilt", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    ui::Layout &span = col.column(true);
    span.prop(ptr, "start", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    span.prop(ptr, "end", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col.prop(ptr, "random", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col.prop(ptr, "wobble", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col.op("OBJECT_OT_wrap_to_edges", IFACE_("To Edges"), ICON_NONE);
  }

  layout.separator();
  layout.prop(ptr, "shape", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  const int shape = RNA_enum_get(ptr, "shape");
  layout.prop(ptr, "width", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  if (shape == MOD_WRAP_SHAPE_STRAP) {
    layout.prop(ptr, "thickness", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  if (shape == MOD_WRAP_SHAPE_ROPE) {
    layout.prop(ptr, "strands", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    layout.prop(ptr, "twist", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  if (shape != MOD_WRAP_SHAPE_STRAP) {
    layout.prop(ptr, "resolution", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  layout.prop(ptr, "width_random", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  if (shape == MOD_WRAP_SHAPE_STRAP) {
    layout.prop(ptr, "end_cut", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    if (RNA_enum_get(ptr, "end_cut") != MOD_WRAP_END_FLAT) {
      layout.prop(ptr, "end_cut_depth", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    }
  }

  layout.separator();
  layout.prop(ptr, "gap", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "step", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "tight", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  ui::Layout &row = layout.row(true, IFACE_("Crossings"));
  row.prop(ptr, "use_crossings", UI_ITEM_NONE, "", ICON_NONE);
  ui::Layout &flip = row.row(true);
  flip.active_set(RNA_boolean_get(ptr, "use_crossings"));
  flip.prop(ptr, "use_crossings_flip", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "seed", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  modifier_vgroup_ui(layout, ptr, &ob_ptr, "vertex_group", "invert_vertex_group", IFACE_("Pin"));
  modifier_material_ui(layout, ptr, "material_index_pick", "material_index", std::nullopt);

  modifier_error_message_draw(layout, ptr);
}

static void panel_register(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_Wrap, panel_draw);
}

ModifierTypeInfo modifierType_Wrap = {
    /*idname*/ "Wrap",
    /*name*/ N_("Wrap"),
    /*struct_name*/ "WrapModifierData",
    /*struct_size*/ sizeof(WrapModifierData),
    /*srna*/ &RNA_WrapModifier,
    /*type*/ ModifierTypeType::Constructive,
    /*flags*/ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsEditmode |
        eModifierTypeFlag_EnableInEditmode,
    /*icon*/ ICON_MOD_WRAP,

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
