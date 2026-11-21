/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Bone Colors: colors the mesh by the bones it follows. Each bone gets a color from its name, and
 * the colors blend by weight. Left and right bones (".L" and ".R", "_L" and "_R", "Left" and
 * "Right" and so on) share a color. Solid shading shows the colors without any other setting.
 */

#include <algorithm>
#include <cstring>

#include "BLI_array.hh"
#include "BLI_color.hh"
#include "BLI_hash.h"
#include "BLI_math_color.h"
#include "BLI_math_vector.hh"
#include "BLI_string.h"
#include "BLI_string_utils.hh"
#include "BLI_task.hh"

#include "BLT_translation.hh"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "BKE_attribute.h"
#include "BKE_attribute.hh"
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
#include "MOD_weights_util.hh"

namespace blender {

/** The color attribute the modifier writes, also what Solid shading looks for. */
static constexpr const char *BONE_COLORS_ATTRIBUTE = "bone_colors";

static void init_data(ModifierData *md)
{
  BoneColorsModifierData *bmd = reinterpret_cast<BoneColorsModifierData *>(md);
  INIT_DEFAULT_STRUCT_AFTER(bmd, modifier);
  /* A preview: shown in the viewport, not in renders. */
  md->mode &= ~eModifierMode_Render;
}

static void required_data_mask(ModifierData * /*md*/, CustomData_MeshMasks *r_cddata_masks)
{
  r_cddata_masks->vmask |= CD_MASK_MDEFORMVERT;
}

/** The color of a bone. With #mirror, a left bone and its right one get the same color. */
static float3 bone_color(const char *name, const int seed, const bool mirror)
{
  char key[MAX_VGROUP_NAME];
  STRNCPY(key, name);
  if (mirror) {
    char flipped[MAX_VGROUP_NAME];
    BLI_string_flip_side_name(flipped, name, false, sizeof(flipped));
    if (strcmp(flipped, key) < 0) {
      STRNCPY(key, flipped);
    }
  }
  const uint hash = BLI_hash_int_2d(BLI_hash_string(key), uint(seed));
  const float hsv[3] = {float(hash & 0xFFFF) / 65536.0f,
                        0.5f + float((hash >> 16) & 0xFF) / 255.0f * 0.25f,
                        0.5f + float((hash >> 24) & 0xFF) / 255.0f * 0.25f};
  float3 srgb;
  hsv_to_rgb_v(hsv, srgb);
  float3 linear;
  srgb_to_linearrgb_v3_v3(linear, srgb);
  return linear;
}

static void update_depsgraph(ModifierData * /*md*/, const ModifierUpdateDepsgraphContext *ctx)
{
  weights::add_armature_relations(ctx, "Bone Colors");
}

static Mesh *modify_mesh(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  const BoneColorsModifierData &bmd = *reinterpret_cast<const BoneColorsModifierData *>(md);
  if (mesh->verts_num == 0) {
    return mesh;
  }
  Object *armature = weights::find_armature(*ctx->object);
  const Array<bool> mask = weights::group_mask(*mesh, armature, MOD_WEIGHT_GROUPS_BONES);

  Array<float3> colors(mask.size(), float3(0.0f));
  {
    int i = 0;
    for (const bDeformGroup &dg : mesh->vertex_group_names) {
      if (mask[i]) {
        colors[i] = bone_color(dg.name, bmd.seed, bmd.flag & MOD_BONECOLORS_MIRROR);
      }
      i++;
    }
  }

  const Span<MDeformVert> dverts = mesh->deform_verts();
  /* Points that follow no bone stay grey. */
  const float3 none(0.2f);

  bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
  attributes.remove(BONE_COLORS_ATTRIBUTE);
  bke::SpanAttributeWriter<ColorGeometry4f> out =
      attributes.lookup_or_add_for_write_only_span<ColorGeometry4f>(BONE_COLORS_ATTRIBUTE,
                                                                     bke::AttrDomain::Point);
  if (!out) {
    return mesh;
  }
  threading::parallel_for(out.span.index_range(), 4096, [&](const IndexRange range) {
    for (const int v : range) {
      float3 sum(0.0f);
      float total = 0.0f;
      if (!dverts.is_empty()) {
        for (const MDeformWeight &dw : Span(dverts[v].dw, dverts[v].totweight)) {
          if (dw.def_nr < mask.size() && mask[dw.def_nr] && dw.weight > 0.0f) {
            sum += colors[dw.def_nr] * dw.weight;
            total += dw.weight;
          }
        }
      }
      float3 color = none;
      if (total > 0.0f) {
        /* Weights that add up to less than one fade towards grey. */
        color = math::interpolate(none, sum / total, std::min(total, 1.0f));
      }
      out.span[v] = ColorGeometry4f(color.x, color.y, color.z, 1.0f);
    }
  });
  out.finish();
  BKE_id_attributes_active_color_set(&mesh->id, BONE_COLORS_ATTRIBUTE);
  BKE_id_attributes_default_color_set(&mesh->id, BONE_COLORS_ATTRIBUTE);
  return mesh;
}

static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  layout.use_property_split_set(true);
  layout.prop(ptr, "seed", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "use_mirror", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  modifier_error_message_draw(layout, ptr);
}

static void panel_register(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_BoneColors, panel_draw);
}

ModifierTypeInfo modifierType_BoneColors = {
    /*idname*/ "BoneColors",
    /*name*/ N_("Bone Colors"),
    /*struct_name*/ "BoneColorsModifierData",
    /*struct_size*/ sizeof(BoneColorsModifierData),
    /*srna*/ &RNA_BoneColorsModifier,
    /*type*/ ModifierTypeType::NonGeometrical,
    /*flags*/ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsMapping |
        eModifierTypeFlag_SupportsEditmode | eModifierTypeFlag_EnableInEditmode,
    /*icon*/ ICON_MOD_BONE_COLORS,
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
