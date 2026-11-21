/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Slice. The work is done by shared code in the geometry module, which the matching nodes use too.
 */

#include <algorithm>
#include <array>

#include "BLI_array.hh"
#include "BLI_math_euler.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_string.h"
#include "BLT_translation.hh"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "BKE_attribute.hh"
#include "BKE_deform.hh"
#include "BKE_lib_query.hh"
#include "BKE_mesh.hh"
#include "BKE_modifier.hh"

#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "MOD_modifiertypes.hh"
#include "MOD_ui_common.hh"
#include "MOD_util.hh"
#include "BLI_bounds.hh"
#include "BKE_customdata.hh"
#include "BKE_mesh.h"
#include "bmesh.hh"
#include "BKE_attribute.h"
#include "BKE_geometry_set.hh"
#include "BLI_color.hh"
#include "BLI_task.hh"
#include "GEO_join_geometries.hh"
#include "GEO_mesh_clip.hh"
#include "GEO_randomize.hh"

namespace blender {

static void init_data_SliceModifierData(ModifierData *md)
{
  SliceModifierData *data = reinterpret_cast<SliceModifierData *>(md);
  INIT_DEFAULT_STRUCT_AFTER(data, modifier);
}

static Mesh *clip(const Mesh &mesh, const geometry::BMeshClipParams &params)
{
  BMeshCreateParams create_params{};
  create_params.use_toolflags = true;
  BMeshFromMeshParams convert_params{};
  convert_params.calc_face_normal = true;
  convert_params.calc_vert_normal = true;
  BMesh *bm = BKE_mesh_to_bmesh_ex(&mesh, &create_params, &convert_params);
  geometry::bmesh_clip(bm, params);
  Mesh *result = BKE_mesh_from_bmesh_for_eval_nomain(bm, nullptr, &mesh);
  BM_mesh_free(bm);
  return result;
}

/**
 * A face corner color by side of the plane. A face is on the side of its center, and a face with
 * every point on the plane is on the cut. With a blend the color changes softly by the distance of
 * each point instead.
 */
static void write_colors(Mesh &mesh,
                         const SliceModifierData &smd,
                         const float3 &plane_co,
                         const float3 &plane_no,
                         const float tolerance)
{
  const std::string name = smd.color_name[0] ? smd.color_name : "Color";
  const ColorGeometry4f above(smd.above_color);
  const ColorGeometry4f below(smd.below_color);
  const ColorGeometry4f cut(smd.cut_color);
  const bool use_cut = smd.flag & MOD_SLICE_CUT_COLOR;
  const float blend = std::max(smd.color_blend, 0.0f);

  const Span<float3> positions = mesh.vert_positions();
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  Array<float> distance(positions.size());
  threading::parallel_for(positions.index_range(), 4096, [&](const IndexRange range) {
    for (const int v : range) {
      distance[v] = math::dot(positions[v] - plane_co, plane_no);
    }
  });

  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  attributes.remove(name);
  bke::SpanAttributeWriter<ColorGeometry4f> colors =
      attributes.lookup_or_add_for_write_only_span<ColorGeometry4f>(name,
                                                                     bke::AttrDomain::Corner);
  if (!colors) {
    return;
  }
  threading::parallel_for(faces.index_range(), 1024, [&](const IndexRange range) {
    for (const int face : range) {
      const IndexRange corners = faces[face];
      float sum = 0.0f;
      bool on_cut = true;
      for (const int corner : corners) {
        const float d = distance[corner_verts[corner]];
        sum += d;
        on_cut &= std::abs(d) <= tolerance;
      }
      if (on_cut && use_cut) {
        colors.span.slice(corners).fill(cut);
        continue;
      }
      if (blend > 0.0f) {
        for (const int corner : corners) {
          const float t = std::clamp(
              (distance[corner_verts[corner]] + blend) / (2.0f * blend), 0.0f, 1.0f);
          const float s = t * t * (3.0f - 2.0f * t);
          const float4 mixed = math::interpolate(
              float4(smd.below_color), float4(smd.above_color), s);
          colors.span[corner] = ColorGeometry4f(mixed.x, mixed.y, mixed.z, mixed.w);
        }
        continue;
      }
      colors.span.slice(corners).fill(sum > 0.0f ? above : below);
    }
  });
  colors.finish();
  BKE_id_attributes_active_color_set(&mesh.id, name);
  BKE_id_attributes_default_color_set(&mesh.id, name);
}

static Mesh *modify_mesh(ModifierData *md, const ModifierEvalContext * /*ctx*/, Mesh *mesh)
{
  const SliceModifierData &smd = *reinterpret_cast<SliceModifierData *>(md);
  if (mesh->verts_num == 0) {
    return mesh;
  }
  const float3x3 rotation = math::from_rotation<float3x3>(math::EulerXYZ(float3(smd.rotation)));
  const float3 normal = math::normalize(rotation * float3(0.0f, 0.0f, 1.0f));

  geometry::BMeshClipParams params;
  params.plane_no = normal;
  params.plane_co = float3(smd.location) + normal * smd.distance;
  params.keep = geometry::BMeshClipKeep(smd.keep);
  params.fill = smd.flag & MOD_SLICE_FILL;
  if (const std::optional<Bounds<float3>> bounds = mesh->bounds_min_max()) {
    params.threshold = std::max(math::distance(bounds->min, bounds->max) * 1e-5f, 1e-7f);
  }

  Mesh *result = nullptr;
  if (smd.keep == 2 && (smd.flag & MOD_SLICE_SEPARATE)) {
    /* Both parts on their own, each clipped from the whole mesh, then joined. */
    geometry::BMeshClipParams side = params;
    side.keep = geometry::BMeshClipKeep::Above;
    Mesh *upper = clip(*mesh, side);
    side.keep = geometry::BMeshClipKeep::Below;
    Mesh *lower = clip(*mesh, side);
    const std::array<bke::GeometrySet, 2> parts = {bke::GeometrySet::from_mesh(upper),
                                                   bke::GeometrySet::from_mesh(lower)};
    bke::GeometrySet joined = geometry::join_geometries(parts, {});
    result = joined.get_component_for_write<bke::MeshComponent>().release();
    if (result == nullptr) {
      result = BKE_mesh_new_nomain(0, 0, 0, 0);
      BKE_mesh_copy_parameters_for_eval(result, mesh);
    }
  }
  else {
    result = clip(*mesh, params);
  }
  if (smd.flag & MOD_SLICE_COLOR) {
    write_colors(*result, smd, params.plane_co, normal, params.threshold * 10.0f);
  }
  geometry::debug_randomize_mesh_order(result);
  return result;
}

static void panel_draw_Slice(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);
  UNUSED_VARS(ob_ptr);

  layout.prop(ptr, "keep", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
  layout.use_property_split_set(true);
  layout.prop(ptr, "plane_location", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "plane_rotation", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "distance", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  const bool both = RNA_enum_get(ptr, "keep") == 2;
  if (both) {
    layout.prop(ptr, "use_separate", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  if (!both || RNA_boolean_get(ptr, "use_separate")) {
    layout.prop(ptr, "use_fill", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }

  layout.prop(ptr, "use_color", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  if (RNA_boolean_get(ptr, "use_color")) {
    ui::Layout &col = layout.column(false);
    col.prop(ptr, "above_color", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col.prop(ptr, "below_color", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    ui::Layout &row = col.row(true, IFACE_("Cut"));
    row.prop(ptr, "use_cut_color", UI_ITEM_NONE, "", ICON_NONE);
    ui::Layout &sub = row.row(true);
    sub.active_set(RNA_boolean_get(ptr, "use_cut_color"));
    sub.prop(ptr, "cut_color", UI_ITEM_NONE, "", ICON_NONE);
    col.prop(ptr, "color_blend", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col.prop(ptr, "color_name", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  modifier_error_message_draw(layout, ptr);
}

static void panel_register_Slice(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_Slice, panel_draw_Slice);
}

ModifierTypeInfo modifierType_Slice = {
    /*idname*/ "Slice",
    /*name*/ N_("Slice"),
    /*struct_name*/ "SliceModifierData",
    /*struct_size*/ sizeof(SliceModifierData),
    /*srna*/ &RNA_SliceModifier,
    /*type*/ ModifierTypeType::Constructive,
    /*flags*/ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsEditmode | eModifierTypeFlag_EnableInEditmode,
    /*icon*/ ICON_MOD_SLICE,

    /*copy_data*/ BKE_modifier_copydata_generic,

    /*deform_verts*/ nullptr,
    /*deform_matrices*/ nullptr,
    /*deform_verts_EM*/ nullptr,
    /*deform_matrices_EM*/ nullptr,
    /*modify_mesh*/ modify_mesh,
    /*modify_geometry_set*/ nullptr,

    /*init_data*/ init_data_SliceModifierData,
    /*required_data_mask*/ nullptr,
    /*free_data*/ nullptr,
    /*is_disabled*/ nullptr,
    /*update_depsgraph*/ nullptr,
    /*depends_on_time*/ nullptr,
    /*depends_on_normals*/ nullptr,
    /*foreach_ID_link*/ nullptr,
    /*foreach_tex_link*/ nullptr,
    /*free_runtime_data*/ nullptr,
    /*panel_register*/ panel_register_Slice,
    /*blend_write*/ nullptr,
    /*blend_read*/ nullptr,
    /*foreach_cache*/ nullptr,
    /*foreach_working_space_color*/ nullptr,
};

}  // namespace blender
