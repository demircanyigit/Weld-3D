/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 */

#include <cstring>

#include "MEM_guardedalloc.h"

#include "BLI_disjoint_set.hh"
#include "BLI_hash.h"
#include "BLI_listbase.h"
#include "BLI_noise.hh"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "BLT_translation.hh"

#include "DNA_mesh_types.h"
#include "DNA_screen_types.h"

#include "BKE_attribute.hh"
#include "BKE_context.hh"
#include "BKE_deform.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"
#include "BKE_screen.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "MOD_modifiertypes.hh"
#include "MOD_ui_common.hh"

#include "MOD_solidify_util.hh"
#include "MOD_util.hh"

namespace blender {

static void init_data(ModifierData *md)
{
  SolidifyModifierData *smd = reinterpret_cast<SolidifyModifierData *>(md);
  INIT_DEFAULT_STRUCT_AFTER(smd, modifier);
}

#ifdef __GNUC__
#  pragma GCC diagnostic error "-Wsign-conversion"
#endif

static void required_data_mask(ModifierData *md, CustomData_MeshMasks *r_cddata_masks)
{
  SolidifyModifierData *smd = reinterpret_cast<SolidifyModifierData *>(md);

  /* Ask for vertex-groups if we need them. */
  if (smd->defgrp_name[0] != '\0' || smd->shell_defgrp_name[0] != '\0' ||
      smd->rim_defgrp_name[0] != '\0')
  {
    r_cddata_masks->vmask |= CD_MASK_MDEFORMVERT;
  }
}

static Mesh *modify_mesh_solidify(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh);

/** The vertex group that carries Random Thickness into the solidify code. */
static const char *RANDOM_GROUP = "__solidify_random_thickness";

/**
 * A copy of the mesh with a vertex group of the thickness of every vertex: the vertex group of
 * the modifier, if any, times a random factor.
 */
static Mesh *random_thickness_mesh(const Object &ob,
                                   const Mesh &mesh,
                                   const SolidifyModifierData &smd)
{
  const MDeformVert *dvert = nullptr;
  int group = -1;
  MOD_get_vgroup(&ob, &mesh, smd.defgrp_name, &dvert, &group);
  const bool invert = smd.flag & MOD_SOLIDIFY_VGROUP_INV;
  const Span<float3> positions = mesh.vert_positions();
  const uint32_t seed = BLI_hash_int_2d(uint32_t(smd.seed), 3571u);

  Array<float> random(mesh.verts_num);
  if (smd.flag & MOD_SOLIDIFY_RANDOM_PER_ISLAND) {
    DisjointSet<int> sets(mesh.verts_num);
    for (const int2 edge : mesh.edges()) {
      sets.join(edge[0], edge[1]);
    }
    Array<int> island_of_root(mesh.verts_num, -1);
    int islands_num = 0;
    for (const int vert : random.index_range()) {
      const int root = sets.find_root(vert);
      if (island_of_root[root] == -1) {
        island_of_root[root] = islands_num++;
      }
      random[vert] = BLI_hash_int_01(BLI_hash_int_2d(seed, uint32_t(island_of_root[root])));
    }
  }
  else {
    const float scale = smd.random_scale > 0.0f ? smd.random_scale : 1.0f;
    const float3 shift(BLI_hash_int_2d_to_float(seed, 1u) * 1000.0f,
                       BLI_hash_int_2d_to_float(seed, 2u) * 1000.0f,
                       BLI_hash_int_2d_to_float(seed, 3u) * 1000.0f);
    for (const int vert : random.index_range()) {
      const float n = noise::perlin(positions[vert] / scale + shift);
      /* Perlin noise stays near the middle, spread it out. */
      random[vert] = std::clamp(n * 2.0f - 0.5f, 0.0f, 1.0f);
    }
  }

  Mesh *copy = BKE_mesh_copy_for_eval(mesh);
  bDeformGroup *dg = MEM_new<bDeformGroup>(__func__);
  STRNCPY(dg->name, RANDOM_GROUP);
  BLI_addtail(&copy->vertex_group_names, dg);
  const int index = BLI_listbase_count(&copy->vertex_group_names) - 1;
  MutableSpan<MDeformVert> dverts = copy->deform_verts_for_write();
  for (const int vert : random.index_range()) {
    float weight = 1.0f;
    if (dvert != nullptr) {
      weight = BKE_defvert_find_weight(&dvert[vert], group);
      if (invert) {
        weight = 1.0f - weight;
      }
      weight = smd.offset_fac_vg + weight * (1.0f - smd.offset_fac_vg);
    }
    weight *= 1.0f - smd.random_thickness * random[vert];
    BKE_defvert_add_index_notest(&dverts[vert], index, weight);
  }
  return copy;
}

static void remove_random_group(Mesh &mesh)
{
  const int index = BKE_id_defgroup_name_index(&mesh.id, RANDOM_GROUP);
  if (index == -1) {
    return;
  }
  if (!mesh.deform_verts().is_empty()) {
    for (MDeformVert &dv : mesh.deform_verts_for_write()) {
      if (MDeformWeight *dw = BKE_defvert_find_index(&dv, index)) {
        BKE_defvert_remove_group(&dv, dw);
      }
    }
  }
  bDeformGroup *dg = static_cast<bDeformGroup *>(BLI_findlink(&mesh.vertex_group_names, index));
  BLI_remlink(&mesh.vertex_group_names, dg);
  MEM_delete(dg);
}

/** Flat, smooth or hard shading of the rim faces. */
static void shade_rim(Mesh &mesh, const Span<bool> rim, const int mode)
{
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  bke::SpanAttributeWriter<bool> sharp_faces = attributes.lookup_or_add_for_write_span<bool>(
      "sharp_face", bke::AttrDomain::Face);
  for (const int face : rim.index_range()) {
    if (rim[face]) {
      sharp_faces.span[face] = mode == MOD_SOLIDIFY_RIM_SHADING_FLAT;
    }
  }
  sharp_faces.finish();
  if (mode != MOD_SOLIDIFY_RIM_SHADING_HARD) {
    return;
  }
  /* Sharp edges between the rim and the rest. */
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_edges = mesh.corner_edges();
  Array<uint8_t> sides(mesh.edges_num, 0);
  for (const int face : faces.index_range()) {
    for (const int edge : corner_edges.slice(faces[face])) {
      sides[edge] |= rim[face] ? 1 : 2;
    }
  }
  bke::SpanAttributeWriter<bool> sharp_edges = attributes.lookup_or_add_for_write_span<bool>(
      "sharp_edge", bke::AttrDomain::Edge);
  for (const int edge : sides.index_range()) {
    if (sides[edge] == 3) {
      sharp_edges.span[edge] = true;
    }
  }
  sharp_edges.finish();
}

static Mesh *modify_mesh(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  SolidifyModifierData *smd = reinterpret_cast<SolidifyModifierData *>(md);
  if (smd->random_thickness <= 0.0f && smd->rim_shading == MOD_SOLIDIFY_RIM_SHADING_KEEP) {
    return modify_mesh_solidify(md, ctx, mesh);
  }

  Mesh *input = mesh;
  const bool random = smd->random_thickness > 0.0f && mesh->verts_num > 0;
  char old_name[sizeof(smd->defgrp_name)];
  const int old_flag = smd->flag;
  const float old_fac_vg = smd->offset_fac_vg;
  if (random) {
    input = random_thickness_mesh(*ctx->object, *mesh, *smd);
    /* The solidify code reads the thickness of every vertex from the new vertex group. */
    STRNCPY(old_name, smd->defgrp_name);
    STRNCPY(smd->defgrp_name, RANDOM_GROUP);
    smd->flag &= ~MOD_SOLIDIFY_VGROUP_INV;
    smd->offset_fac_vg = 0.0f;
  }
  Mesh *result = modify_mesh_solidify(md, ctx, input);
  if (random) {
    STRNCPY(smd->defgrp_name, old_name);
    smd->flag = old_flag;
    smd->offset_fac_vg = old_fac_vg;
    remove_random_group(*result);
    if (result != input) {
      BKE_id_free(nullptr, input);
    }
  }
  if (result == mesh || smd->rim_shading == MOD_SOLIDIFY_RIM_SHADING_KEEP) {
    return result;
  }

  bke::MutableAttributeAccessor attributes = result->attributes_for_write();
  if (smd->mode == MOD_SOLIDIFY_MODE_NONMANIFOLD) {
    const VArraySpan<bool> rim = *attributes.lookup<bool>(".solidify_rim", bke::AttrDomain::Face);
    if (!rim.is_empty()) {
      shade_rim(*result, Array<bool>(Span<bool>(rim)), smd->rim_shading);
    }
    attributes.remove(".solidify_rim");
  }
  else if (smd->flag & MOD_SOLIDIFY_RIM) {
    /* The extrude code adds the rim faces after the faces and the shell. */
    const bool do_shell = !(smd->flag & MOD_SOLIDIFY_NOSHELL);
    const int rim_start = mesh->faces_num * (do_shell ? 2 : 1);
    if (result->faces_num > rim_start) {
      Array<bool> rim(result->faces_num, false);
      rim.as_mutable_span().drop_front(rim_start).fill(true);
      shade_rim(*result, rim, smd->rim_shading);
    }
  }
  return result;
}

static Mesh *modify_mesh_solidify(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  const SolidifyModifierData *smd = reinterpret_cast<SolidifyModifierData *>(md);
  switch (smd->mode) {
    case MOD_SOLIDIFY_MODE_EXTRUDE:
      return MOD_solidify_extrude_modifyMesh(md, ctx, mesh);
    case MOD_SOLIDIFY_MODE_NONMANIFOLD:
      return MOD_solidify_nonmanifold_modifyMesh(md, ctx, mesh);
    default:
      BLI_assert_unreachable();
  }
  return mesh;
}

static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  int solidify_mode = RNA_enum_get(ptr, "solidify_mode");
  bool has_vertex_group = RNA_string_length(ptr, "vertex_group") != 0;

  layout.use_property_split_set(true);

  layout.prop(ptr, "solidify_mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  if (solidify_mode == MOD_SOLIDIFY_MODE_NONMANIFOLD) {
    layout.prop(
        ptr, "nonmanifold_thickness_mode", UI_ITEM_NONE, IFACE_("Thickness Mode"), ICON_NONE);
    layout.prop(ptr, "nonmanifold_boundary_mode", UI_ITEM_NONE, IFACE_("Boundary"), ICON_NONE);
  }

  layout.prop(ptr, "thickness", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "offset", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  if (solidify_mode == MOD_SOLIDIFY_MODE_NONMANIFOLD) {
    layout.prop(ptr, "nonmanifold_merge_threshold", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  else {
    layout.prop(ptr, "use_even_offset", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }

  ui::Layout &col = layout.column(false, CTX_IFACE_(BLT_I18NCONTEXT_ID_MESH, "Rim"));
  col.prop(ptr, "use_rim", UI_ITEM_NONE, CTX_IFACE_(BLT_I18NCONTEXT_ID_MESH, "Fill"), ICON_NONE);
  ui::Layout &sub = col.column(false);
  sub.active_set(RNA_boolean_get(ptr, "use_rim"));
  sub.prop(ptr, "use_rim_only", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  sub.prop(ptr, "rim_shading", UI_ITEM_NONE, IFACE_("Shading"), ICON_NONE);

  layout.separator();

  modifier_vgroup_ui(layout, ptr, &ob_ptr, "vertex_group", "invert_vertex_group", std::nullopt);
  ui::Layout *row = &layout.row(false);
  row->active_set(has_vertex_group);
  row->prop(ptr, "thickness_vertex_group", UI_ITEM_NONE, IFACE_("Factor"), ICON_NONE);

  if (solidify_mode == MOD_SOLIDIFY_MODE_NONMANIFOLD) {
    row = &layout.row(false);
    row->active_set(has_vertex_group);
    row->prop(ptr, "use_flat_faces", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }

  modifier_error_message_draw(layout, ptr);
}

static void normals_panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  int solidify_mode = RNA_enum_get(ptr, "solidify_mode");

  layout.use_property_split_set(true);

  ui::Layout &col = layout.column(false);
  col.prop(ptr, "use_flip_normals", UI_ITEM_NONE, IFACE_("Flip"), ICON_NONE);
  if (solidify_mode == MOD_SOLIDIFY_MODE_EXTRUDE) {
    col.prop(ptr, "use_quality_normals", UI_ITEM_NONE, IFACE_("High Quality"), ICON_NONE);
  }
}

static void materials_panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  layout.use_property_split_set(true);

  layout.prop(ptr, "material_offset", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  ui::Layout &col = layout.column(true);
  col.active_set(RNA_boolean_get(ptr, "use_rim"));
  col.prop(ptr,
           "material_offset_rim",
           UI_ITEM_NONE,
           CTX_IFACE_(BLT_I18NCONTEXT_ID_MESH, "Rim"),
           ICON_NONE);
}

static void edge_data_panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  int solidify_mode = RNA_enum_get(ptr, "solidify_mode");

  layout.use_property_split_set(true);

  if (solidify_mode == MOD_SOLIDIFY_MODE_EXTRUDE) {
    ui::Layout &col = layout.column(true);
    col.prop(ptr, "edge_crease_inner", UI_ITEM_NONE, IFACE_("Crease Inner"), ICON_NONE);
    col.prop(ptr, "edge_crease_outer", UI_ITEM_NONE, IFACE_("Outer"), ICON_NONE);
    col.prop(ptr,
             "edge_crease_rim",
             UI_ITEM_NONE,
             CTX_IFACE_(BLT_I18NCONTEXT_ID_MESH, "Rim"),
             ICON_NONE);
  }
  layout.prop(ptr, "bevel_convex", ui::ITEM_R_SLIDER, std::nullopt, ICON_NONE);
}

static void clamp_panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  layout.use_property_split_set(true);

  ui::Layout &col = layout.column(false);
  col.prop(ptr, "thickness_clamp", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  ui::Layout &row = col.row(false);
  row.active_set(RNA_float_get(ptr, "thickness_clamp") > 0.0f);
  row.prop(ptr, "use_thickness_angle_clamp", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void vertex_group_panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  layout.use_property_split_set(true);

  ui::Layout &col = layout.column(false);
  col.prop_search(ptr, "shell_vertex_group", &ob_ptr, "vertex_groups", IFACE_("Shell"), ICON_NONE);
  col.prop_search(ptr,
                  "rim_vertex_group",
                  &ob_ptr,
                  "vertex_groups",
                  CTX_IFACE_(BLT_I18NCONTEXT_ID_MESH, "Rim"),
                  ICON_NONE);
}

static void random_panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  layout.use_property_split_set(true);

  layout.prop(ptr, "random_thickness", ui::ITEM_R_SLIDER, IFACE_("Thickness"), ICON_NONE);
  ui::Layout &col = layout.column(false);
  col.active_set(RNA_float_get(ptr, "random_thickness") > 0.0f);
  ui::Layout &row = col.row(false);
  row.active_set(!RNA_boolean_get(ptr, "use_random_per_island"));
  row.prop(ptr, "random_scale", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  col.prop(ptr, "use_random_per_island", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  col.prop(ptr, "seed", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void panel_register(ARegionType *region_type)
{
  PanelType *panel_type = modifier_panel_register(region_type, eModifierType_Solidify, panel_draw);
  modifier_subpanel_register(
      region_type, "normals", "Normals", nullptr, normals_panel_draw, panel_type);
  modifier_subpanel_register(
      region_type, "materials", "Materials", nullptr, materials_panel_draw, panel_type);
  modifier_subpanel_register(
      region_type, "edge_data", "Edge Data", nullptr, edge_data_panel_draw, panel_type);
  modifier_subpanel_register(
      region_type, "clamp", "Thickness Clamp", nullptr, clamp_panel_draw, panel_type);
  modifier_subpanel_register(
      region_type, "random", "Random", nullptr, random_panel_draw, panel_type);
  modifier_subpanel_register(region_type,
                             "vertex_groups",
                             "Output Vertex Groups",
                             nullptr,
                             vertex_group_panel_draw,
                             panel_type);
}

ModifierTypeInfo modifierType_Solidify = {
    /*idname*/ "Solidify",
    /*name*/ N_("Solidify"),
    /*struct_name*/ "SolidifyModifierData",
    /*struct_size*/ sizeof(SolidifyModifierData),
    /*srna*/ &RNA_SolidifyModifier,
    /*type*/ ModifierTypeType::Constructive,

    /*flags*/ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_AcceptsCVs |
        eModifierTypeFlag_SupportsMapping | eModifierTypeFlag_SupportsEditmode |
        eModifierTypeFlag_EnableInEditmode,
    /*icon*/ ICON_MOD_SOLIDIFY,

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
