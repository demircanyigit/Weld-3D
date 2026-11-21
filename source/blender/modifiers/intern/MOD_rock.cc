/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Rock: cuts a closed shape with flat cuts, mostly through its corners, for a low poly rock.
 */

#include "BLT_translation.hh"

#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "BKE_attribute.hh"
#include "BKE_mesh.hh"
#include "BKE_lib_query.hh"
#include "BKE_modifier.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "GEO_mesh_rock.hh"
#include "GEO_randomize.hh"

#include "MOD_modifiertypes.hh"
#include "MOD_ui_common.hh"
#include "MOD_util.hh"

namespace blender {

/** Internal face attribute used to find the cut faces for the cut material. */
static const char *CUT_ATTRIBUTE = ".rock_cut";

static void init_data(ModifierData *md)
{
  RockModifierData *data = reinterpret_cast<RockModifierData *>(md);
  INIT_DEFAULT_STRUCT_AFTER(data, modifier);
}

static bool is_disabled(const Scene * /*scene*/, ModifierData *md, bool /*use_render_params*/)
{
  const RockModifierData &rmd = *reinterpret_cast<RockModifierData *>(md);
  return rmd.cuts <= 0 || rmd.depth <= 0.0f;
}

static Mesh *modify_mesh_compute(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  const RockModifierData &rmd = *reinterpret_cast<RockModifierData *>(md);
  if (mesh->faces_num == 0) {
    return mesh;
  }
  const bool use_cut_material = rmd.flag & MOD_ROCK_CUT_MATERIAL;
  const int cut_material = use_cut_material ?
                               MOD_material_slot(*mesh, rmd.cut_material_pick, rmd.cut_material) :
                               0;
  geometry::MeshRockParams params;
  params.cuts = rmd.cuts;
  params.depth = rmd.depth;
  params.corners = rmd.corners;
  params.seed = rmd.seed;
  params.per_island = rmd.flag & MOD_ROCK_PER_ISLAND;
  if (use_cut_material) {
    params.cut_id = CUT_ATTRIBUTE;
  }

  geometry::MeshRockError error = geometry::MeshRockError::NoError;
  Mesh *result = geometry::mesh_rock(*mesh, params, &error);
  if (result != nullptr && result != mesh) {
    MOD_material_slots_from(*result, *mesh);
  }
  if (result == nullptr) {
    if (error == geometry::MeshRockError::NonManifold) {
      BKE_modifier_set_error(ctx->object, md, "The mesh must be closed and manifold");
    }
    else if (error == geometry::MeshRockError::SolverNotAvailable) {
      BKE_modifier_set_error(ctx->object, md, "Built without the Manifold library");
    }
    else if (error != geometry::MeshRockError::NoError) {
      BKE_modifier_set_error(ctx->object, md, "The mesh could not be cut");
    }
    return mesh;
  }

  if (use_cut_material) {
    bke::MutableAttributeAccessor attributes = result->attributes_for_write();
    {
      const VArraySpan<bool> cut = *attributes.lookup_or_default<bool>(
          CUT_ATTRIBUTE, bke::AttrDomain::Face, false);
      bke::SpanAttributeWriter<int> material_index =
          attributes.lookup_or_add_for_write_span<int>("material_index", bke::AttrDomain::Face);
      if (material_index) {
        for (const int face : cut.index_range()) {
          if (cut[face]) {
            material_index.span[face] = cut_material;
          }
        }
        material_index.finish();
      }
    }
    attributes.remove(CUT_ATTRIBUTE);
  }

  geometry::debug_randomize_mesh_order(result);
  return result;
}

/* Taken from the cache when nothing it depends on changed, see #MOD_cached_modify_mesh. */
static Mesh *modify_mesh(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  const RockModifierData &rmd = *reinterpret_cast<RockModifierData *>(md);
  if (rmd.flag & MOD_ROCK_CUT_MATERIAL) {
    MOD_material_slot(*mesh, rmd.cut_material_pick, 0);
  }
  return MOD_cached_modify_mesh(md, ctx, mesh, modify_mesh_compute);
}

static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);

  layout.use_property_split_set(true);
  layout.prop(ptr, "cuts", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "depth", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "corners", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "seed", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "use_per_island", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  ui::Layout &row = layout.row(true, IFACE_("Cut Material"));
  row.prop(ptr, "use_cut_material", UI_ITEM_NONE, "", ICON_NONE);
  ui::Layout &sub = row.row(true);
  sub.active_set(RNA_boolean_get(ptr, "use_cut_material"));
  modifier_material_ui(sub, ptr, "cut_material_pick", "cut_material", "");

  modifier_error_message_draw(layout, ptr);
}

static void panel_register(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_Rock, panel_draw);
}

static void foreach_ID_link(ModifierData *md, Object *ob, IDWalkFunc walk, void *user_data)
{
  RockModifierData *pick_data = reinterpret_cast<RockModifierData *>(md);
  walk(user_data, ob, reinterpret_cast<ID **>(&pick_data->cut_material_pick), IDWALK_CB_USER);
}

ModifierTypeInfo modifierType_Rock = {
    /*idname*/ "Rock",
    /*name*/ N_("Rock"),
    /*struct_name*/ "RockModifierData",
    /*struct_size*/ sizeof(RockModifierData),
    /*srna*/ &RNA_RockModifier,
    /*type*/ ModifierTypeType::Constructive,
    /*flags*/ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsEditmode,
    /*icon*/ ICON_MOD_ROCK,

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
    /*is_disabled*/ is_disabled,
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
