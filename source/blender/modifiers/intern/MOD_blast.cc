/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "BKE_lib_query.hh"
#include "BKE_modifier.hh"
#include "BKE_object.hh"
#include "BLI_math_matrix.hh"
#include "BLI_utildefines.h"
#include "BLT_translation.hh"
#include "DEG_depsgraph_build.hh"
#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"
#include "GEO_mesh_blast.hh"
#include "MOD_ui_common.hh"
#include "MOD_util.hh"
#include "RNA_access.hh"
#include "RNA_prototypes.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender {
static void init_data(ModifierData *md)
{
  auto *data = reinterpret_cast<BlastModifierData *>(md);
  INIT_DEFAULT_STRUCT_AFTER(data, modifier);
}

/** The object is only read by Inside and Gradient. */
static bool uses_object(const BlastModifierData &data)
{
  return ELEM(data.select, MOD_BLAST_SELECT_INSIDE, MOD_BLAST_SELECT_GRADIENT) &&
         data.object != nullptr;
}

static void foreach_ID_link(ModifierData *md, Object *ob, IDWalkFunc walk, void *user_data)
{
  auto *data = reinterpret_cast<BlastModifierData *>(md);
  walk(user_data, ob, reinterpret_cast<ID **>(&data->object), IDWALK_CB_NOP);
}

static void update_depsgraph(ModifierData *md, const ModifierUpdateDepsgraphContext *ctx)
{
  const auto &data = *reinterpret_cast<BlastModifierData *>(md);
  if (!uses_object(data) || data.object == ctx->object) {
    return;
  }
  DEG_add_object_relation(ctx->node, data.object, DEG_OB_COMP_TRANSFORM, "Blast Modifier");
  if (data.select == MOD_BLAST_SELECT_INSIDE) {
    DEG_add_object_relation(ctx->node, data.object, DEG_OB_COMP_GEOMETRY, "Blast Modifier");
  }
  DEG_add_depends_on_transform_relation(ctx->node, "Blast Modifier");
}

static Mesh *modify_mesh(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  const auto &data = *reinterpret_cast<BlastModifierData *>(md);
  geometry::MeshBlastParams params;
  params.metric = geometry::BlastMetric(data.metric);
  params.filter = geometry::BlastFilter(data.filter);
  params.keep_count = data.keep_count;
  params.invert = data.flag & MOD_BLAST_INVERT;
  switch (data.metric) {
    case MOD_BLAST_FACE_COUNT:
      params.threshold = data.minimum_faces;
      break;
    case MOD_BLAST_AREA:
      params.threshold = data.minimum_area;
      break;
    case MOD_BLAST_VOLUME:
      params.threshold = data.minimum_volume;
      break;
    case MOD_BLAST_BOUNDS_SIZE:
      params.threshold = data.minimum_size;
      break;
  }
  params.select = geometry::BlastSelect(
      std::clamp(data.select, int(MOD_BLAST_SELECT_SIZE), int(MOD_BLAST_SELECT_ATTRIBUTE)));
  params.amount = data.amount;
  params.randomness = data.randomness;
  params.seed = data.seed;
  params.inside = geometry::BlastInside(
      std::clamp(data.inside_test, int(MOD_BLAST_INSIDE_CENTER), int(MOD_BLAST_INSIDE_ALL)));
  params.gradient = data.gradient_shape == MOD_BLAST_GRADIENT_SPHERICAL ?
                        geometry::BlastGradient::Spherical :
                        geometry::BlastGradient::Linear;
  params.distance = std::max(data.distance, 1e-4f);
  params.noise_scale = data.noise_scale;
  params.attribute = data.attribute_name;
  params.mark = data.flag & MOD_BLAST_MARK;
  params.mark_name = data.mark_name[0] ? data.mark_name : "blasted";

  const Object *object = uses_object(data) && data.object != ctx->object ? data.object : nullptr;
  if (object != nullptr) {
    /* From the object's space into the space of this mesh. */
    const float4x4 to_self = ctx->object->world_to_object() * object->object_to_world();
    if (data.select == MOD_BLAST_SELECT_INSIDE) {
      params.volume = object->type == OB_MESH ? BKE_object_get_evaluated_mesh(object) : nullptr;
      params.volume_transform = to_self;
    }
    else {
      params.use_gradient_object = true;
      params.gradient_transform = to_self;
    }
  }

  std::string message;
  Mesh *result = geometry::mesh_blast(*mesh, params, message);
  if (!message.empty()) {
    BKE_modifier_set_warning(ctx->object, md, "%s", message.c_str());
  }
  return result ? result : mesh;
}

static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);
  ui::Layout &layout = *panel->layout;
  layout.use_property_split_set(true);
  layout.prop(ptr, "select", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  const int select = RNA_enum_get(ptr, "select");
  switch (select) {
    case MOD_BLAST_SELECT_SIZE: {
      layout.prop(ptr, "metric", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      layout.prop(ptr, "filter", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      const int filter = RNA_enum_get(ptr, "filter");
      const int metric = RNA_enum_get(ptr, "metric");
      if (filter != MOD_BLAST_LARGEST) {
        const char *threshold = metric == MOD_BLAST_FACE_COUNT ? "minimum_faces" :
                                metric == MOD_BLAST_AREA       ? "minimum_area" :
                                metric == MOD_BLAST_VOLUME     ? "minimum_volume" :
                                                                 "minimum_size";
        layout.prop(ptr, threshold, UI_ITEM_NONE, std::nullopt, ICON_NONE);
      }
      if (filter != MOD_BLAST_THRESHOLD) {
        layout.prop(ptr, "keep_count", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      }
      if (metric == MOD_BLAST_VOLUME) {
        layout.label(IFACE_("Open islands are preserved"), ICON_INFO);
      }
      break;
    }
    case MOD_BLAST_SELECT_RANDOM:
      layout.prop(ptr, "amount", UI_ITEM_NONE, IFACE_("Amount"), ICON_NONE);
      layout.prop(ptr, "seed", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;
    case MOD_BLAST_SELECT_INSIDE:
      layout.prop(ptr, "object", UI_ITEM_NONE, IFACE_("Volume"), ICON_NONE);
      layout.prop(ptr, "inside_test", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;
    default: {
      if (select == MOD_BLAST_SELECT_GRADIENT) {
        layout.prop(ptr, "gradient_shape", UI_ITEM_NONE, std::nullopt, ICON_NONE);
        layout.prop(ptr, "object", UI_ITEM_NONE, std::nullopt, ICON_NONE);
        if (RNA_pointer_get(ptr, "object").data != nullptr) {
          layout.prop(ptr, "distance", UI_ITEM_NONE, std::nullopt, ICON_NONE);
        }
      }
      else if (select == MOD_BLAST_SELECT_NOISE) {
        layout.prop(ptr, "noise_scale", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      }
      else {
        PointerRNA mesh_ptr = RNA_pointer_get(&ob_ptr, "data");
        layout.prop_search(
            ptr, "attribute_name", &mesh_ptr, "attributes", std::nullopt, ICON_NONE);
      }
      layout.prop(ptr, "amount", UI_ITEM_NONE, IFACE_("Threshold"), ICON_NONE);
      layout.prop(ptr, "randomness", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      layout.prop(ptr, "seed", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;
    }
  }
  layout.prop(ptr, "invert", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  ui::Layout &row = layout.row(true, IFACE_("Mark"));
  row.prop(ptr, "use_mark", UI_ITEM_NONE, "", ICON_NONE);
  ui::Layout &sub = row.row(true);
  sub.active_set(RNA_boolean_get(ptr, "use_mark"));
  sub.prop(ptr, "mark_name", UI_ITEM_NONE, "", ICON_NONE);
  modifier_error_message_draw(layout, ptr);
}

static void panel_register(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_Blast, panel_draw);
}

ModifierTypeInfo modifierType_Blast = {
    /*idname*/ "Blast",
    /*name*/ N_("Blast"),
    /*struct_name*/ "BlastModifierData",
    /*struct_size*/ sizeof(BlastModifierData),
    /*srna*/ &RNA_BlastModifier,
    /*type*/ ModifierTypeType::Nonconstructive,
    /*flags*/ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsEditmode,
    /*icon*/ ICON_MOD_BLAST,

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
