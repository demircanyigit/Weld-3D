/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Faceset: new face sets for the mesh, one per region. The regions are split by islands,
 * materials, angles, seams, sharp edges or UV islands.
 */

#include <cmath>

#include "BLI_array.hh"
#include "BLI_disjoint_set.hh"
#include "BLI_function_ref.hh"
#include "BLI_math_vector.hh"

#include "BLT_translation.hh"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "BKE_attribute.hh"
#include "BKE_mesh.hh"
#include "BKE_modifier.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "MOD_modifiertypes.hh"
#include "MOD_ui_common.hh"

namespace blender {

static void init_data(ModifierData *md)
{
  FacesetModifierData *fmd = reinterpret_cast<FacesetModifierData *>(md);
  INIT_DEFAULT_STRUCT_AFTER(fmd, modifier);
}

/** Per edge, whether faces on both sides of it may be in the same region. */
static Array<bool> edge_joins(const FacesetModifierData &fmd, const Mesh &mesh)
{
  const int edges_num = mesh.edges_num;
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_edges = mesh.corner_edges();
  const Span<int> corner_verts = mesh.corner_verts();
  const bke::AttributeAccessor attributes = mesh.attributes();
  Array<bool> joins(edges_num, true);

  /* The first face of every edge and the corner of it that starts the edge, to compare with the
   * other faces of the edge. */
  Array<int> first_face(edges_num, -1);
  Array<int> first_corner(edges_num, -1);
  auto compare_faces = [&](FunctionRef<bool(int corner_a, int face_a, int corner_b, int face_b)>
                               same) {
    for (const int face : faces.index_range()) {
      for (const int corner : faces[face]) {
        const int edge = corner_edges[corner];
        if (first_face[edge] == -1) {
          first_face[edge] = face;
          first_corner[edge] = corner;
        }
        else if (!same(first_corner[edge], first_face[edge], corner, face)) {
          joins[edge] = false;
        }
      }
    }
  };

  switch (fmd.mode) {
    case MOD_FACESET_ISLANDS:
      break;
    case MOD_FACESET_MATERIALS: {
      const VArraySpan<int> materials = *attributes.lookup_or_default<int>(
          "material_index", bke::AttrDomain::Face, 0);
      compare_faces([&](int, const int a, int, const int b) { return materials[a] == materials[b]; });
      break;
    }
    case MOD_FACESET_ANGLE: {
      const Span<float3> normals = mesh.face_normals();
      const float cos_angle = std::cos(fmd.angle);
      compare_faces([&](int, const int a, int, const int b) {
        return math::dot(normals[a], normals[b]) >= cos_angle;
      });
      break;
    }
    case MOD_FACESET_SEAMS:
    case MOD_FACESET_SHARP: {
      const VArraySpan<bool> marked = *attributes.lookup_or_default<bool>(
          fmd.mode == MOD_FACESET_SEAMS ? "uv_seam" : "sharp_edge", bke::AttrDomain::Edge, false);
      for (const int e : IndexRange(edges_num)) {
        joins[e] = !marked[e];
      }
      break;
    }
    case MOD_FACESET_UV_ISLANDS: {
      const StringRef uv_name = mesh.active_or_default_uv_map_name();
      const VArraySpan<float2> uvs = *attributes.lookup_or_default<float2>(
          uv_name, bke::AttrDomain::Corner, float2(0.0f));
      /* The UV of a point of the edge, from the corner of the face that is at that point. */
      auto uv_at = [&](const int face, const int vert) {
        for (const int corner : faces[face]) {
          if (corner_verts[corner] == vert) {
            return uvs[corner];
          }
        }
        return float2(0.0f);
      };
      const Span<int2> edges = mesh.edges();
      compare_faces([&](const int corner_a, const int a, int, const int b) {
        const int2 edge = edges[corner_edges[corner_a]];
        constexpr float limit = 1e-5f;
        return math::distance_squared(uv_at(a, edge[0]), uv_at(b, edge[0])) < limit &&
               math::distance_squared(uv_at(a, edge[1]), uv_at(b, edge[1])) < limit;
      });
      break;
    }
  }

  if (fmd.flag & MOD_FACESET_SPLIT_EXISTING) {
    if (const VArray<int> old_sets = *attributes.lookup<int>(".sculpt_face_set",
                                                            bke::AttrDomain::Face))
    {
      const VArraySpan<int> sets(old_sets);
      first_face.fill(-1);
      compare_faces([&](int, const int a, int, const int b) { return sets[a] == sets[b]; });
    }
  }
  return joins;
}

static Mesh *modify_mesh(ModifierData *md, const ModifierEvalContext * /*ctx*/, Mesh *mesh)
{
  const FacesetModifierData &fmd = *reinterpret_cast<FacesetModifierData *>(md);
  if (mesh->faces_num == 0) {
    return mesh;
  }

  const Array<bool> joins = edge_joins(fmd, *mesh);

  /* Faces joined across the edges that allow it. */
  const OffsetIndices<int> faces = mesh->faces();
  const Span<int> corner_edges = mesh->corner_edges();
  DisjointSet<int> regions(mesh->faces_num);
  Array<int> edge_face(mesh->edges_num, -1);
  for (const int face : faces.index_range()) {
    for (const int edge : corner_edges.slice(faces[face])) {
      if (!joins[edge]) {
        continue;
      }
      if (edge_face[edge] == -1) {
        edge_face[edge] = face;
      }
      else {
        regions.join(edge_face[edge], face);
      }
    }
  }

  /* Face sets numbered from 1, in the order the regions first appear. */
  Array<int> set_of_root(mesh->faces_num, 0);
  Array<int> face_sets(mesh->faces_num);
  int sets_num = 0;
  for (const int face : faces.index_range()) {
    const int root = regions.find_root(face);
    if (set_of_root[root] == 0) {
      set_of_root[root] = ++sets_num;
    }
    face_sets[face] = set_of_root[root];
  }

  Mesh *result = BKE_mesh_copy_for_eval(*mesh);
  bke::MutableAttributeAccessor attributes = result->attributes_for_write();
  attributes.remove(".sculpt_face_set");
  attributes.add<int>(".sculpt_face_set",
                      bke::AttrDomain::Face,
                      bke::AttributeInitVArray(VArray<int>::from_span(face_sets)));
  result->face_sets_color_seed = fmd.seed;
  result->face_sets_color_default = 1;
  return result;
}

static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);

  layout.use_property_split_set(true);
  layout.prop(ptr, "mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  if (RNA_enum_get(ptr, "mode") == MOD_FACESET_ANGLE) {
    layout.prop(ptr, "angle", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  layout.prop(ptr, "use_split_existing", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "seed", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  modifier_error_message_draw(layout, ptr);
}

static void panel_register(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_Faceset, panel_draw);
}

ModifierTypeInfo modifierType_Faceset = {
    /*idname*/ "Faceset",
    /*name*/ N_("Faceset"),
    /*struct_name*/ "FacesetModifierData",
    /*struct_size*/ sizeof(FacesetModifierData),
    /*srna*/ &RNA_FacesetModifier,
    /*type*/ ModifierTypeType::Constructive,
    /*flags*/ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsMapping |
        eModifierTypeFlag_SupportsEditmode | eModifierTypeFlag_EnableInEditmode,
    /*icon*/ ICON_MOD_FACESET,

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
