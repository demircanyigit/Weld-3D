/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Cable: hanging cables, between two objects or along each loose edge of the mesh, sagging down.
 */

#include <algorithm>
#include <array>
#include <cmath>

#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.hh"
#include "BLI_rand.hh"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "BKE_attribute.hh"
#include "BKE_geometry_set.hh"
#include "BKE_lib_query.hh"
#include "BKE_mesh.hh"
#include "BKE_modifier.hh"

#include "DEG_depsgraph_build.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "GEO_join_geometries.hh"

#include "MOD_modifiertypes.hh"
#include "MOD_ui_common.hh"
#include "MOD_util.hh"

namespace blender {

static void init_data(ModifierData *md)
{
  CableModifierData *data = reinterpret_cast<CableModifierData *>(md);
  INIT_DEFAULT_STRUCT_AFTER(data, modifier);
}

static void foreach_ID_link(ModifierData *md, Object *ob, IDWalkFunc walk, void *user_data)
{
  CableModifierData *data = reinterpret_cast<CableModifierData *>(md);
  walk(user_data, ob, reinterpret_cast<ID **>(&data->start_object), IDWALK_CB_NOP);
  walk(user_data, ob, reinterpret_cast<ID **>(&data->end_object), IDWALK_CB_NOP);
  CableModifierData *pick_data = reinterpret_cast<CableModifierData *>(md);
  walk(user_data, ob, reinterpret_cast<ID **>(&pick_data->material_index_pick), IDWALK_CB_USER);
}

static void update_depsgraph(ModifierData *md, const ModifierUpdateDepsgraphContext *ctx)
{
  const CableModifierData &data = *reinterpret_cast<CableModifierData *>(md);
  /* Down is the world's down, so turning the object changes the sag. */
  DEG_add_depends_on_transform_relation(ctx->node, "Cable Modifier");
  if (data.source != MOD_CABLE_OBJECTS) {
    return;
  }
  for (Object *object : {data.start_object, data.end_object}) {
    if (object != nullptr && object != ctx->object) {
      DEG_add_object_relation(ctx->node, object, DEG_OB_COMP_TRANSFORM, "Cable Modifier");
    }
  }
}

/** Faces and their points, made into a mesh at the end. */
struct MeshBuilder {
  Vector<float3> positions;
  Vector<int> face_sizes;
  Vector<int> corner_verts;
  Vector<float2> corner_uvs;

  Mesh *build(const Mesh *material_template, const int material_index) const
  {
    Mesh *mesh = BKE_mesh_new_nomain(
        positions.size(), 0, face_sizes.size(), corner_verts.size());
    if (material_template != nullptr) {
      BKE_mesh_copy_parameters_for_eval(mesh, material_template);
    }
    mesh->vert_positions_for_write().copy_from(positions);
    MutableSpan<int> offsets = mesh->face_offsets_for_write();
    int start = 0;
    for (const int i : face_sizes.index_range()) {
      offsets[i] = start;
      start += face_sizes[i];
    }
    offsets.last() = start;
    mesh->corner_verts_for_write().copy_from(corner_verts);
    bke::mesh_calc_edges(*mesh, false, false);
    bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
    bke::SpanAttributeWriter<float2> uvs = attributes.lookup_or_add_for_write_only_span<float2>(
        "UVMap", bke::AttrDomain::Corner);
    if (uvs) {
      uvs.span.copy_from(corner_uvs);
      uvs.finish();
    }
    if (material_index > 0) {
      bke::SpanAttributeWriter<int> materials =
          attributes.lookup_or_add_for_write_only_span<int>("material_index",
                                                            bke::AttrDomain::Face);
      if (materials) {
        materials.span.fill(material_index);
        materials.finish();
      }
    }
    bke::mesh_smooth_set(*mesh, true);
    mesh->tag_positions_changed();
    return mesh;
  }
};

/** A tube along the points, with rings turned by parallel transport so they do not twist. */
static void add_tube(MeshBuilder &builder,
                     const Span<float3> points,
                     const float radius,
                     const int sides,
                     const bool caps)
{
  const int rings = points.size();
  if (rings < 2 || sides < 3) {
    return;
  }
  const int first = builder.positions.size();
  float3 tangent = math::normalize(points[1] - points[0]);
  float3 helper = std::abs(tangent.z) < 0.9f ? float3(0, 0, 1) : float3(1, 0, 0);
  float3 side = math::normalize(math::cross(tangent, helper));
  float length = 0.0f;
  Vector<float> along;
  for (const int r : IndexRange(rings)) {
    if (r > 0) {
      length += math::distance(points[r], points[r - 1]);
    }
    along.append(length);
    const float3 next_tangent = r + 1 < rings ? math::normalize(points[r + 1] - points[r]) :
                                                tangent;
    const float3 ring_tangent = math::normalize(tangent + next_tangent);
    /* Parallel transport: keep the side at right angles to the tangent. */
    side = math::normalize(side - ring_tangent * math::dot(side, ring_tangent));
    const float3 up = math::cross(ring_tangent, side);
    for (const int s : IndexRange(sides)) {
      const float angle = float(s) / float(sides) * float(M_PI) * 2.0f;
      builder.positions.append(points[r] +
                               (side * std::cos(angle) + up * std::sin(angle)) * radius);
    }
    tangent = next_tangent;
  }
  const float circumference = 2.0f * float(M_PI) * radius;
  for (const int r : IndexRange(rings - 1)) {
    for (const int s : IndexRange(sides)) {
      const int s1 = (s + 1) % sides;
      const std::array<int, 4> verts = {first + r * sides + s,
                                        first + r * sides + s1,
                                        first + (r + 1) * sides + s1,
                                        first + (r + 1) * sides + s};
      /* U runs along the cable in units of the distance around it, so a texture is not
       * stretched. V goes once around. */
      const float u0 = along[r] / std::max(circumference, 1e-6f);
      const float u1 = along[r + 1] / std::max(circumference, 1e-6f);
      const std::array<float2, 4> uvs = {float2(u0, float(s) / sides),
                                         float2(u0, float(s + 1) / sides),
                                         float2(u1, float(s + 1) / sides),
                                         float2(u1, float(s) / sides)};
      builder.face_sizes.append(4);
      for (const int k : IndexRange(4)) {
        builder.corner_verts.append(verts[k]);
        builder.corner_uvs.append(uvs[k]);
      }
    }
  }
  if (!caps) {
    return;
  }
  for (const int end : {0, rings - 1}) {
    builder.face_sizes.append(sides);
    for (const int s : IndexRange(sides)) {
      /* The start cap faces back, the end cap forward. */
      const int k = end == 0 ? sides - 1 - s : s;
      builder.corner_verts.append(first + end * sides + k);
      const float angle = float(k) / float(sides) * float(M_PI) * 2.0f;
      builder.corner_uvs.append(float2(0.5f + 0.5f * std::cos(angle),
                                       0.5f + 0.5f * std::sin(angle)));
    }
  }
}

/** Points of a cable hanging from a to b, dropping by a share of its span in the middle. */
static Vector<float3> hang(const float3 &a,
                           const float3 &b,
                           const float3 &down,
                           const float sag,
                           const int segments)
{
  Vector<float3> points;
  const float span = math::distance(a, b);
  for (const int i : IndexRange(segments + 1)) {
    const float t = float(i) / float(segments);
    /* A parabola is close to the real curve of a hanging cable for small sags, and it keeps the
     * lowest point in the middle. */
    points.append(a + (b - a) * t + down * (4.0f * t * (1.0f - t) * sag * span));
  }
  return points;
}

static Mesh *modify_mesh(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  const CableModifierData &cmd = *reinterpret_cast<CableModifierData *>(md);
  const float4x4 &world_to_object = ctx->object->world_to_object();
  float3 down = math::transform_direction(world_to_object, float3(0, 0, -1));
  down = math::length(down) > 0.0f ? math::normalize(down) : float3(0, 0, -1);
  const int segments = std::max(cmd.segments, 1);
  const int sides = std::max(cmd.sides, 3);
  const bool caps = cmd.flag & MOD_CABLE_CAPS;

  Vector<std::array<float3, 2>> spans;
  if (cmd.source == MOD_CABLE_OBJECTS) {
    const auto location = [&](const Object *object) {
      return object == nullptr ? float3(0.0f) :
                                 math::transform_point(world_to_object,
                                                       object->object_to_world().location());
    };
    if (cmd.end_object == nullptr) {
      BKE_modifier_set_error(ctx->object, md, "Choose an end object");
      return mesh;
    }
    spans.append({location(cmd.start_object), location(cmd.end_object)});
  }
  else {
    /* Loose edges: edges of no face. */
    const bke::LooseEdgeCache &loose = mesh->loose_edges();
    const Span<int2> edges = mesh->edges();
    const Span<float3> positions = mesh->vert_positions();
    if (loose.count > 0) {
      for (const int e : edges.index_range()) {
        if (loose.is_loose_bits[e]) {
          spans.append({positions[edges[e][0]], positions[edges[e][1]]});
        }
      }
    }
    if (spans.is_empty()) {
      BKE_modifier_set_error(
          ctx->object, md, "Draw loose edges between the points the cables hang from");
      return mesh;
    }
  }

  MeshBuilder builder;
  RandomNumberGenerator rng(uint32_t(cmd.seed) * 2654435761u + 5u);
  for (const std::array<float3, 2> &span : spans) {
    const float sag = cmd.sag *
                      (1.0f + std::clamp(cmd.sag_random, 0.0f, 1.0f) *
                                  (rng.get_float() * 2.0f - 1.0f));
    add_tube(builder, hang(span[0], span[1], down, sag, segments), cmd.radius, sides, caps);
  }
  Mesh *cables = builder.build(
      mesh, std::max(MOD_material_slot(*mesh, cmd.material_index_pick, cmd.material_index), 0));
  if (!(cmd.flag & MOD_CABLE_KEEP_MESH)) {
    return cables;
  }
  const std::array<bke::GeometrySet, 2> parts = {
      bke::GeometrySet::from_mesh(mesh, bke::GeometryOwnershipType::ReadOnly),
      bke::GeometrySet::from_mesh(cables)};
  bke::GeometrySet joined = geometry::join_geometries(parts, {});
  Mesh *result = joined.get_component_for_write<bke::MeshComponent>().release();
  return result ? result : mesh;
}

static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);
  layout.prop(ptr, "source", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
  layout.use_property_split_set(true);
  if (RNA_enum_get(ptr, "source") == MOD_CABLE_OBJECTS) {
    layout.prop(ptr, "start_object", UI_ITEM_NONE, IFACE_("Start"), ICON_NONE);
    layout.prop(ptr, "end_object", UI_ITEM_NONE, IFACE_("End"), ICON_NONE);
  }
  layout.prop(ptr, "sag", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  if (RNA_enum_get(ptr, "source") == MOD_CABLE_EDGES) {
    layout.prop(ptr, "sag_random", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    layout.prop(ptr, "seed", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  layout.prop(ptr, "radius", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "segments", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "sides", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "use_caps", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "use_keep_mesh", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  modifier_material_ui(layout, ptr, "material_index_pick", "material_index", std::nullopt);
  modifier_error_message_draw(layout, ptr);
}

static void panel_register(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_Cable, panel_draw);
}

ModifierTypeInfo modifierType_Cable = {
    /*idname*/ "Cable",
    /*name*/ N_("Cable"),
    /*struct_name*/ "CableModifierData",
    /*struct_size*/ sizeof(CableModifierData),
    /*srna*/ &RNA_CableModifier,
    /*type*/ ModifierTypeType::Constructive,
    /*flags*/ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsEditmode |
        eModifierTypeFlag_EnableInEditmode,
    /*icon*/ ICON_MOD_CABLE,

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
