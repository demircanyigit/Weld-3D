/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Lean: an object pushes through the rigid pieces of the mesh, which move out of its way and
 * lean, held together by glue so a wall ends up almost fallen instead of crumbled.
 */

#include "BLI_array.hh"
#include "BLI_math_matrix.hh"

#include "BLT_translation.hh"

#include "BLI_listbase_iterator.hh"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "BKE_deform.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_query.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_wrapper.hh"
#include "BKE_modifier.hh"

#include "BLO_read_write.hh"

#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "GEO_mesh_lean.hh"

#include "MOD_colliders.hh"
#include "MOD_modifiertypes.hh"
#include "MOD_ui_common.hh"
#include "MOD_util.hh"

namespace blender {

static void init_data(ModifierData *md)
{
  LeanModifierData *data = reinterpret_cast<LeanModifierData *>(md);
  INIT_DEFAULT_STRUCT_AFTER(data, modifier);
}

static void required_data_mask(ModifierData *md, CustomData_MeshMasks *r_cddata_masks)
{
  const LeanModifierData *lmd = reinterpret_cast<LeanModifierData *>(md);
  if (lmd->vertex_group[0] != '\0') {
    r_cddata_masks->vmask |= CD_MASK_MDEFORMVERT;
  }
}

static void foreach_ID_link(ModifierData *md, Object *ob, IDWalkFunc walk, void *user_data)
{
  LeanModifierData *lmd = reinterpret_cast<LeanModifierData *>(md);
  walk(user_data, ob, reinterpret_cast<ID **>(&lmd->pusher), IDWALK_CB_NOP);
  walk(user_data, ob, reinterpret_cast<ID **>(&lmd->target), IDWALK_CB_NOP);
  MOD_collider_sources_foreach_ID(lmd->colliders, ob, walk, user_data);
}

static void copy_data(const ModifierData *md, ModifierData *target, const int flag)
{
  BKE_modifier_copydata_generic(md, target, flag);
  const LeanModifierData &src = *reinterpret_cast<const LeanModifierData *>(md);
  LeanModifierData &dst = *reinterpret_cast<LeanModifierData *>(target);
  MOD_collider_sources_copy(dst.colliders, src.colliders);
}

static void free_data(ModifierData *md)
{
  MOD_collider_sources_free(reinterpret_cast<LeanModifierData *>(md)->colliders);
}

static void blend_write(BlendWriter *writer, const ID * /*owner*/, const ModifierData *md)
{
  const LeanModifierData *lmd = reinterpret_cast<const LeanModifierData *>(md);
  writer->write_struct(lmd);
  MOD_collider_sources_blend_write(writer, lmd->colliders);
}

static void blend_read(BlendDataReader *reader, ModifierData *md)
{
  MOD_collider_sources_blend_read(reader, reinterpret_cast<LeanModifierData *>(md)->colliders);
}

static bool is_disabled(const Scene * /*scene*/, ModifierData *md, bool /*use_render_params*/)
{
  const LeanModifierData *lmd = reinterpret_cast<LeanModifierData *>(md);
  return lmd->pusher == nullptr || lmd->pusher->type != OB_MESH;
}

static void update_depsgraph(ModifierData *md, const ModifierUpdateDepsgraphContext *ctx)
{
  const LeanModifierData *lmd = reinterpret_cast<LeanModifierData *>(md);
  MOD_colliders_update_depsgraph(lmd->target, lmd->colliders, ctx, "Lean Modifier");
  if (lmd->pusher != nullptr && lmd->pusher != ctx->object) {
    DEG_add_object_relation(ctx->node, lmd->pusher, DEG_OB_COMP_GEOMETRY, "Lean Modifier");
    DEG_add_object_relation(ctx->node, lmd->pusher, DEG_OB_COMP_TRANSFORM, "Lean Modifier");
  }
  /* Down is down in the world, so the object's turn and place matter. */
  DEG_add_depends_on_transform_relation(ctx->node, "Lean Modifier");
}

static Mesh *modify_mesh(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  const LeanModifierData &lmd = *reinterpret_cast<LeanModifierData *>(md);
  if (mesh->verts_num == 0 || lmd.pusher == nullptr || lmd.pusher == ctx->object) {
    return mesh;
  }
  Mesh *pusher_mesh = BKE_modifier_get_evaluated_mesh_from_evaluated_object(lmd.pusher);
  if (pusher_mesh == nullptr) {
    return mesh;
  }
  BKE_mesh_wrapper_ensure_mdata(pusher_mesh);

  geometry::MeshLeanParams params;
  params.mesh_to_space = ctx->object->object_to_world();
  params.pusher = pusher_mesh;
  params.pusher_to_space = lmd.pusher->object_to_world();
  params.pusher_hull = !(lmd.flag & MOD_LEAN_PUSHER_MESH);
  params.direction = float3(lmd.direction);
  params.distance = lmd.distance;
  params.push_time = lmd.push_time;
  params.settle_time = lmd.settle_time;
  params.release = lmd.flag & MOD_LEAN_RELEASE;
  params.glue = lmd.glue;
  params.friction = lmd.friction;
  params.gravity = lmd.gravity;
  params.substeps = lmd.substeps;
  if (lmd.moved_name[0] != '\0') {
    params.moved_id = lmd.moved_name;
  }

  /* Every collider joined, already in world space. */
  Mesh *ground = MOD_colliders_mesh(lmd.target, lmd.colliders, ctx);
  params.ground = ground;
  params.ground_to_space = float4x4::identity();
  params.ground_cage = lmd.cage;

  /* Pieces from an attribute, which may split the mesh first. */
  Mesh *split = nullptr;
  Array<int> pieces;
  if (lmd.pieces == MOD_LEAN_PIECES_ATTRIBUTE) {
    if (MOD_pieces_from_attribute(*mesh, lmd.piece_name, &split, pieces)) {
      params.pieces = pieces;
    }
    else {
      BKE_modifier_set_warning(
          ctx->object, md, "No attribute \"%s\", loose parts are used", lmd.piece_name);
    }
  }
  else if (lmd.pieces == MOD_LEAN_PIECES_FACE_SETS) {
    if (MOD_pieces_from_attribute(*mesh, ".sculpt_face_set", &split, pieces)) {
      params.pieces = pieces;
    }
    else {
      BKE_modifier_set_warning(ctx->object, md, "No face sets, loose parts are used");
    }
  }
  const Mesh &input = split ? *split : *mesh;

  Array<bool> keep;
  const MDeformVert *dvert = nullptr;
  int defgrp_index = -1;
  MOD_get_vgroup(ctx->object, &input, lmd.vertex_group, &dvert, &defgrp_index);
  if (dvert != nullptr) {
    const bool invert = lmd.flag & MOD_LEAN_INVERT_VGROUP;
    keep.reinitialize(input.verts_num);
    for (const int i : keep.index_range()) {
      keep[i] = (BKE_defvert_find_weight(&dvert[i], defgrp_index) > 0.0f) != invert;
    }
    params.keep = keep;
  }

  const char *error = nullptr;
  Mesh *result = geometry::mesh_lean(input, params, &error);
  if (ground != nullptr) {
    BKE_id_free(nullptr, ground);
  }
  if (split != nullptr) {
    BKE_id_free(nullptr, split);
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
  layout.prop(ptr, "pusher", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "use_pusher_mesh", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "direction", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "distance", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  ui::Layout &time = layout.column(true);
  time.prop(ptr, "push_time", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  time.prop(ptr, "settle_time", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "use_release", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  ui::Layout &hold = layout.column(true);
  hold.prop(ptr, "glue", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  hold.prop(ptr, "friction", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  hold.prop(ptr, "gravity", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  layout.prop(ptr, "pieces", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  if (RNA_enum_get(ptr, "pieces") == MOD_LEAN_PIECES_ATTRIBUTE) {
    layout.prop(ptr, "piece_name", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  layout.prop(ptr, "target", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  MOD_collider_sources_draw(layout, ptr);
  layout.prop(ptr, "cage", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  modifier_vgroup_ui(layout, ptr, &ob_ptr, "vertex_group", "invert_vertex_group", IFACE_("Pin"));
  layout.prop(ptr, "quality", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "moved_name", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  modifier_error_message_draw(layout, ptr);
}

static void panel_register(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_Lean, panel_draw);
}

ModifierTypeInfo modifierType_Lean = {
    /*idname*/ "Lean",
    /*name*/ N_("Lean"),
    /*struct_name*/ "LeanModifierData",
    /*struct_size*/ sizeof(LeanModifierData),
    /*srna*/ &RNA_LeanModifier,
    /*type*/ ModifierTypeType::Constructive,
    /*flags*/ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsEditmode,
    /*icon*/ ICON_MOD_LEAN,

    /*copy_data*/ copy_data,

    /*deform_verts*/ nullptr,
    /*deform_matrices*/ nullptr,
    /*deform_verts_EM*/ nullptr,
    /*deform_matrices_EM*/ nullptr,
    /*modify_mesh*/ modify_mesh,
    /*modify_geometry_set*/ nullptr,

    /*init_data*/ init_data,
    /*required_data_mask*/ required_data_mask,
    /*free_data*/ free_data,
    /*is_disabled*/ is_disabled,
    /*update_depsgraph*/ update_depsgraph,
    /*depends_on_time*/ nullptr,
    /*depends_on_normals*/ nullptr,
    /*foreach_ID_link*/ foreach_ID_link,
    /*foreach_tex_link*/ nullptr,
    /*free_runtime_data*/ nullptr,
    /*panel_register*/ panel_register,
    /*blend_write*/ blend_write,
    /*blend_read*/ blend_read,
    /*foreach_cache*/ nullptr,
    /*foreach_working_space_color*/ nullptr,
};

}  // namespace blender
