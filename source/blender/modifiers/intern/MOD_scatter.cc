/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Scatter: objects at random spots on the faces, picked by a pattern, in order or at random by
 * weight, kept apart by a spacing or by their measured size.
 */

#include <algorithm>

#include "BLI_listbase.h"
#include "BLI_listbase_iterator.hh"
#include "BLI_string.h"

#include "BLT_translation.hh"

#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "BKE_attribute.hh"
#include "BKE_geometry_set.hh"
#include "BKE_lib_query.hh"
#include "BKE_modifier.hh"

#include "BLO_read_write.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "DEG_depsgraph_query.hh"

#include "GEO_place_objects.hh"

#include "MOD_modifiertypes.hh"
#include "MOD_place_util.hh"
#include "MOD_ui_common.hh"

namespace blender {

static void init_data(ModifierData *md)
{
  ScatterModifierData *smd = reinterpret_cast<ScatterModifierData *>(md);
  INIT_DEFAULT_STRUCT_AFTER(smd, modifier);
}

static void free_data(ModifierData *md)
{
  ScatterModifierData &smd = *reinterpret_cast<ScatterModifierData *>(md);
  BLI_freelistN(&smd.objects);
}

static void copy_data(const ModifierData *md, ModifierData *target, const int flag)
{
  BKE_modifier_copydata_generic(md, target, flag);
  const ScatterModifierData &src = *reinterpret_cast<const ScatterModifierData *>(md);
  ScatterModifierData &dst = *reinterpret_cast<ScatterModifierData *>(target);
  BLI_duplicatelist(&dst.objects, &src.objects);
}

static void foreach_ID_link(ModifierData *md, Object *ob, IDWalkFunc walk, void *user_data)
{
  ScatterModifierData &smd = *reinterpret_cast<ScatterModifierData *>(md);
  walk(user_data, ob, reinterpret_cast<ID **>(&smd.collection), IDWALK_CB_USER);
  for (ArraySourceObject &item : smd.objects) {
    walk(user_data, ob, reinterpret_cast<ID **>(&item.object), IDWALK_CB_NOP);
  }
}

static void update_depsgraph(ModifierData *md, const ModifierUpdateDepsgraphContext *ctx)
{
  ScatterModifierData &smd = *reinterpret_cast<ScatterModifierData *>(md);
  MOD_place_update_depsgraph(
      ctx, smd.source, smd.collection, smd.objects, nullptr, "Scatter Modifier");
}

static bool is_disabled(const Scene * /*scene*/, ModifierData *md, bool /*use_render_params*/)
{
  const ScatterModifierData &smd = *reinterpret_cast<const ScatterModifierData *>(md);
  if (smd.source == MOD_PLACE_SOURCE_COLLECTION) {
    return smd.collection == nullptr;
  }
  return BLI_listbase_is_empty(&smd.objects);
}

static void blend_write(BlendWriter *writer, const ID * /*id_owner*/, const ModifierData *md)
{
  const ScatterModifierData &smd = *reinterpret_cast<const ScatterModifierData *>(md);
  writer->write_struct(&smd);
  writer->write_struct_list(&smd.objects);
}

static void blend_read(BlendDataReader *reader, ModifierData *md)
{
  ScatterModifierData &smd = *reinterpret_cast<ScatterModifierData *>(md);
  BLO_read_struct_list(reader, ArraySourceObject, &smd.objects);
}

static void store_count(ModifierData *md, const ModifierEvalContext *ctx, const int count)
{
  if (!DEG_is_active(ctx->depsgraph)) {
    return;
  }
  ScatterModifierData *orig = reinterpret_cast<ScatterModifierData *>(
      BKE_modifier_get_original(ctx->object, md));
  if (orig != nullptr) {
    orig->count_num = count;
  }
}

static void modify_geometry_set(ModifierData *md,
                                const ModifierEvalContext *ctx,
                                bke::GeometrySet *geometry_set)
{
  const ScatterModifierData &smd = *reinterpret_cast<ScatterModifierData *>(md);
  const Mesh *mesh = geometry_set->get_mesh();
  if (mesh == nullptr) {
    return;
  }

  const Vector<PlaceRow> rows = MOD_place_rows(
      smd.source, smd.collection, smd.objects, ctx->object);
  const PlaceObjects objects = MOD_place_objects(rows);

  std::string error;
  const Vector<int> pattern = MOD_place_pattern(smd.pattern, rows, error);
  if (!error.empty() && smd.pick == MOD_PLACE_PICK_PATTERN) {
    BKE_modifier_set_error(ctx->object, md, "%s", error.c_str());
  }
  const geometry::PlacePicker picker(
      geometry::PlacePick(smd.pick), pattern, objects.items, smd.seed);

  /* The vertex group is the chance of an object at each spot. */
  Array<float> weights;
  if (smd.vertex_group[0] != '\0') {
    const bke::AttributeAccessor attributes = mesh->attributes();
    const VArraySpan<float> group = *attributes.lookup_or_default<float>(
        smd.vertex_group, bke::AttrDomain::Point, 0.0f);
    const bool invert = smd.flag & MOD_SCATTER_INVERT_VGROUP;
    weights.reinitialize(mesh->verts_num);
    for (const int v : weights.index_range()) {
      const float w = std::clamp(group[v], 0.0f, 1.0f);
      weights[v] = invert ? 1.0f - w : w;
    }
  }

  geometry::ScatterParams params;
  params.items = objects.items;
  params.picker = &picker;
  params.vert_weights = weights;
  params.use_count = smd.amount == MOD_SCATTER_COUNT;
  params.density = smd.density;
  params.count = smd.count;
  params.max_count = smd.max_count;
  params.spacing = smd.spacing;
  params.no_overlap = smd.flag & MOD_SCATTER_NO_OVERLAP;
  params.look.up = geometry::PlaceUp(smd.up);
  params.look.base = geometry::PlaceBase(smd.base);
  params.look.offset = smd.offset;
  params.look.rotation_random = smd.rotation_random;
  params.look.tilt_random = smd.tilt_random;
  params.look.scale_min = smd.scale_min;
  params.look.scale_max = smd.scale_max;
  params.look.height_min = smd.height_min;
  params.look.height_max = smd.height_max;
  params.look.seed = smd.seed;

  const Vector<geometry::PlacedObject> placed = geometry::scatter_on_faces(*mesh, params);
  store_count(md, ctx, int(placed.size()));

  *geometry_set = MOD_place_output(objects,
                                   placed,
                                   (smd.flag & MOD_SCATTER_KEEP_MESH) ? mesh : nullptr,
                                   smd.flag & MOD_SCATTER_REALIZE);
}

static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);
  ScatterModifierData &smd = *static_cast<ScatterModifierData *>(ptr->data);
  Object *ob = static_cast<Object *>(ob_ptr.data);

  layout.use_property_split_set(true);

  std::string error;
  std::string names;
  if (smd.pick == MOD_PLACE_PICK_PATTERN) {
    const Vector<PlaceRow> rows = MOD_place_rows(smd.source, smd.collection, smd.objects, ob);
    names = MOD_place_pattern_names(smd.pattern, rows, error);
  }
  modifier_place_source_draw(layout, ptr, names, error);

  layout.separator();
  layout.prop(ptr, "amount", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
  if (smd.amount == MOD_SCATTER_COUNT) {
    layout.prop(ptr, "count", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  else {
    layout.prop(ptr, "density", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  layout.prop(ptr, "max_count", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "spacing", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "use_no_overlap", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  modifier_vgroup_ui(layout, ptr, &ob_ptr, "vertex_group", "invert_vertex_group", std::nullopt);

  if (smd.count_num > 0) {
    char text[64];
    SNPRINTF(text, IFACE_("Objects: %d"), smd.count_num);
    layout.label(text, ICON_NONE);
  }

  modifier_error_message_draw(layout, ptr);
}

static void align_panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);

  layout.use_property_split_set(true);
  layout.prop(ptr, "up", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
  layout.prop(ptr, "base", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
  layout.prop(ptr, "offset", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void random_panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);
  const ScatterModifierData &smd = *static_cast<const ScatterModifierData *>(ptr->data);

  layout.use_property_split_set(true);
  layout.prop(ptr, "rotation_random", UI_ITEM_NONE, IFACE_("Rotation"), ICON_NONE);
  layout.prop(ptr, "tilt_random", UI_ITEM_NONE, IFACE_("Tilt"), ICON_NONE);
  ui::Layout &scale = layout.column(true);
  scale.prop(ptr, "scale_min", UI_ITEM_NONE, IFACE_("Scale Min"), ICON_NONE);
  scale.prop(ptr, "scale_max", UI_ITEM_NONE, IFACE_("Max"), ICON_NONE);
  ui::Layout &height = layout.column(true);
  height.prop(ptr, "height_min", UI_ITEM_NONE, IFACE_("Height Min"), ICON_NONE);
  height.prop(ptr, "height_max", UI_ITEM_NONE, IFACE_("Max"), ICON_NONE);
  /* The seed shows above for Random picks. */
  if (smd.pick != MOD_PLACE_PICK_RANDOM) {
    layout.prop(ptr, "seed", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
}

static void output_panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);

  layout.use_property_split_set(true);
  layout.prop(ptr, "use_keep_mesh", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "use_realize", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void panel_register(ARegionType *region_type)
{
  PanelType *panel_type = modifier_panel_register(region_type, eModifierType_Scatter, panel_draw);
  modifier_subpanel_register(region_type, "align", "Align", nullptr, align_panel_draw, panel_type);
  modifier_subpanel_register(
      region_type, "random", "Random", nullptr, random_panel_draw, panel_type);
  modifier_subpanel_register(
      region_type, "output", "Output", nullptr, output_panel_draw, panel_type);
}

ModifierTypeInfo modifierType_Scatter = {
    /*idname*/ "Scatter",
    /*name*/ N_("Scatter"),
    /*struct_name*/ "ScatterModifierData",
    /*struct_size*/ sizeof(ScatterModifierData),
    /*srna*/ &RNA_ScatterModifier,
    /*type*/ ModifierTypeType::Constructive,
    /*flags*/ eModifierTypeFlag_AcceptsMesh,
    /*icon*/ ICON_MOD_SCATTER,

    /*copy_data*/ copy_data,

    /*deform_verts*/ nullptr,
    /*deform_matrices*/ nullptr,
    /*deform_verts_EM*/ nullptr,
    /*deform_matrices_EM*/ nullptr,
    /*modify_mesh*/ nullptr,
    /*modify_geometry_set*/ modify_geometry_set,

    /*init_data*/ init_data,
    /*required_data_mask*/ nullptr,
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
