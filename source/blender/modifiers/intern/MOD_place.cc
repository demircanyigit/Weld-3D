/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Place: objects along the edges, on the faces or on the vertices of the mesh, picked by a
 * pattern, in order or at random, and packed end to end by their measured sides.
 */

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
  PlaceModifierData *pmd = reinterpret_cast<PlaceModifierData *>(md);
  INIT_DEFAULT_STRUCT_AFTER(pmd, modifier);
}

static void free_data(ModifierData *md)
{
  PlaceModifierData &pmd = *reinterpret_cast<PlaceModifierData *>(md);
  BLI_freelistN(&pmd.objects);
}

static void copy_data(const ModifierData *md, ModifierData *target, const int flag)
{
  BKE_modifier_copydata_generic(md, target, flag);
  const PlaceModifierData &src = *reinterpret_cast<const PlaceModifierData *>(md);
  PlaceModifierData &dst = *reinterpret_cast<PlaceModifierData *>(target);
  BLI_duplicatelist(&dst.objects, &src.objects);
}

static void foreach_ID_link(ModifierData *md, Object *ob, IDWalkFunc walk, void *user_data)
{
  PlaceModifierData &pmd = *reinterpret_cast<PlaceModifierData *>(md);
  walk(user_data, ob, reinterpret_cast<ID **>(&pmd.collection), IDWALK_CB_USER);
  walk(user_data, ob, reinterpret_cast<ID **>(&pmd.corner_object), IDWALK_CB_NOP);
  for (ArraySourceObject &item : pmd.objects) {
    walk(user_data, ob, reinterpret_cast<ID **>(&item.object), IDWALK_CB_NOP);
  }
}

static void update_depsgraph(ModifierData *md, const ModifierUpdateDepsgraphContext *ctx)
{
  PlaceModifierData &pmd = *reinterpret_cast<PlaceModifierData *>(md);
  Object *corner = pmd.corners == MOD_PLACE_CORNERS_OBJECT ? pmd.corner_object : nullptr;
  MOD_place_update_depsgraph(
      ctx, pmd.source, pmd.collection, pmd.objects, corner, "Place Modifier");
}

static bool is_disabled(const Scene * /*scene*/, ModifierData *md, bool /*use_render_params*/)
{
  const PlaceModifierData &pmd = *reinterpret_cast<const PlaceModifierData *>(md);
  if (pmd.source == MOD_PLACE_SOURCE_COLLECTION) {
    return pmd.collection == nullptr;
  }
  return BLI_listbase_is_empty(&pmd.objects);
}

static void blend_write(BlendWriter *writer, const ID * /*id_owner*/, const ModifierData *md)
{
  const PlaceModifierData &pmd = *reinterpret_cast<const PlaceModifierData *>(md);
  writer->write_struct(&pmd);
  writer->write_struct_list(&pmd.objects);
}

static void blend_read(BlendDataReader *reader, ModifierData *md)
{
  PlaceModifierData &pmd = *reinterpret_cast<PlaceModifierData *>(md);
  BLO_read_struct_list(reader, ArraySourceObject, &pmd.objects);
}

static void store_count(ModifierData *md, const ModifierEvalContext *ctx, const int count)
{
  if (!DEG_is_active(ctx->depsgraph)) {
    return;
  }
  PlaceModifierData *orig = reinterpret_cast<PlaceModifierData *>(
      BKE_modifier_get_original(ctx->object, md));
  if (orig != nullptr) {
    orig->count_num = count;
  }
}

/** Per point, the points in the vertex group. Empty when there is no group. */
static Array<bool> vert_selection(const Mesh &mesh, const PlaceModifierData &pmd)
{
  Array<bool> selection;
  if (pmd.vertex_group[0] == '\0') {
    return selection;
  }
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan<float> weights = *attributes.lookup_or_default<float>(
      pmd.vertex_group, bke::AttrDomain::Point, 0.0f);
  const bool invert = pmd.flag & MOD_PLACE_INVERT_VGROUP;
  selection.reinitialize(mesh.verts_num);
  for (const int v : selection.index_range()) {
    selection[v] = (weights[v] > 0.0f) != invert;
  }
  return selection;
}

static void modify_geometry_set(ModifierData *md,
                                const ModifierEvalContext *ctx,
                                bke::GeometrySet *geometry_set)
{
  const PlaceModifierData &pmd = *reinterpret_cast<PlaceModifierData *>(md);
  const Mesh *mesh = geometry_set->get_mesh();
  if (mesh == nullptr) {
    return;
  }

  const Vector<PlaceRow> rows = MOD_place_rows(
      pmd.source, pmd.collection, pmd.objects, ctx->object);
  PlaceObjects objects = MOD_place_objects(rows);
  const int rows_num = int(rows.size());

  std::string error;
  const Vector<int> pattern = MOD_place_pattern(pmd.pattern, rows, error);
  if (!error.empty() && pmd.pick == MOD_PLACE_PICK_PATTERN) {
    BKE_modifier_set_error(ctx->object, md, "%s", error.c_str());
  }
  const geometry::PlacePicker picker(geometry::PlacePick(pmd.pick),
                                     pattern,
                                     objects.items.as_span().take_front(rows_num),
                                     pmd.seed);

  geometry::PlaceLook look;
  look.along = pmd.along == 1 ? 1 : 0;
  look.up = geometry::PlaceUp(pmd.up);
  look.base = geometry::PlaceBase(pmd.base);
  look.side = geometry::PlaceSide(pmd.side);
  look.offset = pmd.offset;
  look.rotation_random = pmd.rotation_random;
  look.tilt_random = pmd.tilt_random;
  look.scale_min = pmd.scale_min;
  look.scale_max = pmd.scale_max;
  look.height_min = pmd.height_min;
  look.height_max = pmd.height_max;
  look.seed = pmd.seed;

  const Array<bool> selection = vert_selection(*mesh, pmd);

  Vector<geometry::PlacedObject> placed;
  switch (pmd.mode) {
    case MOD_PLACE_EDGES: {
      geometry::PlaceEdgesParams params;
      if (pmd.corners == MOD_PLACE_CORNERS_OBJECT && pmd.corner_object != nullptr &&
          pmd.corner_object != ctx->object)
      {
        params.corner_item = int(objects.items.size());
        MOD_place_add_object(objects, pmd.corner_object, 0.0f);
      }
      params.items = objects.items;
      params.picker = &picker;
      params.vert_selection = selection;
      params.edges = geometry::PlaceEdges(pmd.edges);
      params.corner_angle = pmd.corner_angle;
      params.gap = pmd.gap;
      params.fit = geometry::PlaceFit(pmd.fit);
      params.corners = geometry::PlaceCorners(pmd.corners);
      params.restart_per_path = pmd.flag & MOD_PLACE_RESTART_PATH;
      params.look = look;
      placed = geometry::place_along_edges(*mesh, params);
      break;
    }
    case MOD_PLACE_FACES:
      placed = geometry::place_on_faces(*mesh,
                                        objects.items,
                                        picker,
                                        selection,
                                        look,
                                        pmd.flag & MOD_PLACE_FIT_FACE);
      break;
    case MOD_PLACE_VERTICES:
    default:
      placed = geometry::place_on_vertices(*mesh, objects.items, picker, selection, look);
      break;
  }
  store_count(md, ctx, int(placed.size()));

  *geometry_set = MOD_place_output(objects,
                                   placed,
                                   (pmd.flag & MOD_PLACE_KEEP_MESH) ? mesh : nullptr,
                                   pmd.flag & MOD_PLACE_REALIZE);
}

static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);
  PlaceModifierData &pmd = *static_cast<PlaceModifierData *>(ptr->data);
  Object *ob = static_cast<Object *>(ob_ptr.data);

  layout.use_property_split_set(true);
  layout.prop(ptr, "mode", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);

  std::string error;
  std::string names;
  if (pmd.pick == MOD_PLACE_PICK_PATTERN) {
    const Vector<PlaceRow> rows = MOD_place_rows(pmd.source, pmd.collection, pmd.objects, ob);
    names = MOD_place_pattern_names(pmd.pattern, rows, error);
  }
  modifier_place_source_draw(layout, ptr, names, error);
  modifier_vgroup_ui(layout, ptr, &ob_ptr, "vertex_group", "invert_vertex_group", std::nullopt);

  if (pmd.mode == MOD_PLACE_EDGES) {
    layout.separator();
    layout.prop(ptr, "edges", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    layout.prop(ptr, "corner_angle", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    layout.prop(ptr, "along", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
    layout.prop(ptr, "gap", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    layout.prop(ptr, "fit", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    layout.prop(ptr, "use_restart_path", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    layout.prop(ptr, "corners", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    if (pmd.corners == MOD_PLACE_CORNERS_OBJECT) {
      layout.prop(ptr, "corner_object", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    }
  }
  else if (pmd.mode == MOD_PLACE_FACES) {
    layout.prop(ptr, "use_fit_face", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }

  if (pmd.count_num > 0) {
    char text[64];
    SNPRINTF(text, IFACE_("Objects: %d"), pmd.count_num);
    layout.label(text, ICON_NONE);
  }

  modifier_error_message_draw(layout, ptr);
}

static void align_panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);
  const PlaceModifierData &pmd = *static_cast<const PlaceModifierData *>(ptr->data);

  layout.use_property_split_set(true);
  if (pmd.mode != MOD_PLACE_EDGES) {
    layout.prop(ptr, "along", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
  }
  layout.prop(ptr, "up", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
  layout.prop(ptr, "base", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
  if (pmd.mode == MOD_PLACE_EDGES) {
    layout.prop(ptr, "side", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  layout.prop(ptr, "offset", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void random_panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);
  const PlaceModifierData &pmd = *static_cast<const PlaceModifierData *>(ptr->data);

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
  if (pmd.pick != MOD_PLACE_PICK_RANDOM) {
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
  PanelType *panel_type = modifier_panel_register(region_type, eModifierType_Place, panel_draw);
  modifier_subpanel_register(region_type, "align", "Align", nullptr, align_panel_draw, panel_type);
  modifier_subpanel_register(
      region_type, "random", "Random", nullptr, random_panel_draw, panel_type);
  modifier_subpanel_register(
      region_type, "output", "Output", nullptr, output_panel_draw, panel_type);
}

ModifierTypeInfo modifierType_Place = {
    /*idname*/ "Place",
    /*name*/ N_("Place"),
    /*struct_name*/ "PlaceModifierData",
    /*struct_size*/ sizeof(PlaceModifierData),
    /*srna*/ &RNA_PlaceModifier,
    /*type*/ ModifierTypeType::Constructive,
    /*flags*/ eModifierTypeFlag_AcceptsMesh,
    /*icon*/ ICON_MOD_PLACE,

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
