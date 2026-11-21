/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Transfer Weights: each point takes the weights of the nearest point on the surface of another
 * mesh, blended across that face. Points farther than the limit take the weights of the points
 * around them instead, so loose ends follow the part they hang from.
 */

#include <utility>

#include "BLI_array.hh"
#include "BLI_kdopbvh.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "BKE_bvhutils.hh"
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
#include "MOD_weights_util.hh"

namespace blender {

using WeightList = Vector<std::pair<int, float>, 4>;

static void init_data(ModifierData *md)
{
  TransferWeightsModifierData *tmd = reinterpret_cast<TransferWeightsModifierData *>(md);
  INIT_DEFAULT_STRUCT_AFTER(tmd, modifier);
}

static void required_data_mask(ModifierData * /*md*/, CustomData_MeshMasks *r_cddata_masks)
{
  r_cddata_masks->vmask |= CD_MASK_MDEFORMVERT;
}

static bool is_disabled(const Scene * /*scene*/, ModifierData *md, bool /*use_render_params*/)
{
  const TransferWeightsModifierData &tmd = *reinterpret_cast<TransferWeightsModifierData *>(md);
  return tmd.object == nullptr || tmd.object->type != OB_MESH;
}

static void foreach_ID_link(ModifierData *md, Object *ob, IDWalkFunc walk, void *user_data)
{
  TransferWeightsModifierData *tmd = reinterpret_cast<TransferWeightsModifierData *>(md);
  walk(user_data, ob, reinterpret_cast<ID **>(&tmd->object), IDWALK_CB_NOP);
}

static void update_depsgraph(ModifierData *md, const ModifierUpdateDepsgraphContext *ctx)
{
  TransferWeightsModifierData *tmd = reinterpret_cast<TransferWeightsModifierData *>(md);
  if (tmd->object != nullptr) {
    DEG_add_object_relation(ctx->node, tmd->object, DEG_OB_COMP_GEOMETRY, "Transfer Weights");
    DEG_add_object_relation(ctx->node, tmd->object, DEG_OB_COMP_TRANSFORM, "Transfer Weights");
    DEG_add_depends_on_transform_relation(ctx->node, "Transfer Weights");
  }
}

/** Gives each point not yet set the average weights of its set neighbors, over and over, until
 * every connected point is set. */
static void fill_unset(const Mesh &mesh, Array<WeightList> &weights, Array<bool> &set)
{
  const Span<int2> edges = mesh.edges();
  Vector<int> todo;
  for (const int v : set.index_range()) {
    if (!set[v]) {
      todo.append(v);
    }
  }
  if (todo.is_empty()) {
    return;
  }
  Array<Vector<int, 6>> neighbors(mesh.verts_num);
  for (const int2 &edge : edges) {
    neighbors[edge[0]].append(edge[1]);
    neighbors[edge[1]].append(edge[0]);
  }
  while (!todo.is_empty()) {
    Vector<std::pair<int, WeightList>> filled;
    Vector<int> left;
    for (const int v : todo) {
      WeightList sum;
      int count = 0;
      for (const int n : neighbors[v]) {
        if (!set[n]) {
          continue;
        }
        count++;
        for (const auto &[g, w] : weights[n]) {
          bool found = false;
          for (auto &[sg, sw] : sum) {
            if (sg == g) {
              sw += w;
              found = true;
              break;
            }
          }
          if (!found) {
            sum.append({g, w});
          }
        }
      }
      if (count == 0) {
        left.append(v);
        continue;
      }
      for (auto &[g, w] : sum) {
        w /= float(count);
      }
      filled.append({v, std::move(sum)});
    }
    if (filled.is_empty()) {
      break;
    }
    for (auto &[v, list] : filled) {
      weights[v] = std::move(list);
      set[v] = true;
    }
    todo = std::move(left);
  }
}

static Mesh *modify_mesh(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  const TransferWeightsModifierData &tmd =
      *reinterpret_cast<const TransferWeightsModifierData *>(md);
  if (tmd.object == nullptr || mesh->verts_num == 0) {
    return mesh;
  }
  const Mesh *src = BKE_modifier_get_evaluated_mesh_from_evaluated_object(tmd.object);
  if (src == nullptr || src->corner_tris().is_empty()) {
    return mesh;
  }
  const Span<MDeformVert> src_dverts = src->deform_verts();
  if (src_dverts.is_empty() || weights::groups_num(*src) == 0) {
    BKE_modifier_set_error(ctx->object, md, "The source has no vertex groups");
    return mesh;
  }

  Object *armature = weights::find_armature(*ctx->object);
  if (armature == nullptr) {
    armature = weights::find_armature(*tmd.object);
  }
  if (tmd.groups == MOD_WEIGHT_GROUPS_SELECTED_BONES && armature == nullptr) {
    BKE_modifier_set_error(ctx->object, md, "No armature found for the selected bones");
    return mesh;
  }
  const Array<bool> src_mask = weights::group_mask(*src, armature, tmd.groups);

  /* Each used source group's group on this mesh. */
  Array<int> target_group(src_mask.size(), -1);
  {
    int i = 0;
    for (const bDeformGroup &dg : src->vertex_group_names) {
      if (src_mask[i]) {
        target_group[i] = weights::ensure_group(*mesh, dg.name);
      }
      i++;
    }
  }

  const bke::BVHTreeFromMesh tree = src->bvh_corner_tris();
  const Span<float3> src_positions = src->vert_positions();
  const Span<int> src_corner_verts = src->corner_verts();
  const Span<int3> src_tris = src->corner_tris();
  const float4x4 to_world = ctx->object->object_to_world();
  const float4x4 src_to_world = tmd.object->object_to_world();
  const float4x4 to_src = tmd.object->world_to_object() * to_world;
  const Span<float3> positions = mesh->vert_positions();

  Array<WeightList> weights(mesh->verts_num);
  Array<bool> set(mesh->verts_num, false);
  threading::parallel_for(positions.index_range(), 256, [&](const IndexRange range) {
    for (const int v : range) {
      const float3 co = math::transform_point(to_src, positions[v]);
      BVHTreeNearest nearest;
      nearest.index = -1;
      nearest.dist_sq = FLT_MAX;
      BLI_bvhtree_find_nearest(tree.tree, co, &nearest, tree.nearest_callback, (void *)&tree);
      if (nearest.index == -1) {
        continue;
      }
      if (tmd.max_distance > 0.0f &&
          math::distance(math::transform_point(to_world, positions[v]),
                         math::transform_point(src_to_world, float3(nearest.co))) >
              tmd.max_distance)
      {
        continue;
      }
      const int3 &tri = src_tris[nearest.index];
      const int corners[3] = {
          src_corner_verts[tri[0]], src_corner_verts[tri[1]], src_corner_verts[tri[2]]};
      float3 bary;
      interp_weights_tri_v3(bary,
                            src_positions[corners[0]],
                            src_positions[corners[1]],
                            src_positions[corners[2]],
                            nearest.co);
      WeightList &list = weights[v];
      for (const int k : IndexRange(3)) {
        const MDeformVert &dv = src_dverts[corners[k]];
        for (const MDeformWeight &dw : Span(dv.dw, dv.totweight)) {
          if (dw.def_nr >= src_mask.size() || !src_mask[dw.def_nr] || bary[k] <= 0.0f) {
            continue;
          }
          const int g = target_group[dw.def_nr];
          const float w = dw.weight * bary[k];
          bool found = false;
          for (auto &[lg, lw] : list) {
            if (lg == g) {
              lw += w;
              found = true;
              break;
            }
          }
          if (!found) {
            list.append({g, w});
          }
        }
      }
      set[v] = true;
    }
  });
  fill_unset(*mesh, weights, set);

  Array<bool> mask(weights::groups_num(*mesh), false);
  for (const int g : target_group) {
    if (g != -1) {
      mask[g] = true;
    }
  }
  MutableSpan<MDeformVert> dverts = mesh->deform_verts_for_write();
  threading::parallel_for(dverts.index_range(), 1024, [&](const IndexRange range) {
    for (const int v : range) {
      MDeformVert &dv = dverts[v];
      for (int j = int(dv.totweight) - 1; j >= 0; j--) {
        if (dv.dw[j].def_nr < mask.size() && mask[dv.dw[j].def_nr]) {
          BKE_defvert_remove_group(&dv, &dv.dw[j]);
        }
      }
      for (const auto &[g, w] : weights[v]) {
        if (w > 0.0f) {
          BKE_defvert_add_index_notest(&dv, g, std::min(w, 1.0f));
        }
      }
    }
  });

  weights::smooth_groups(*mesh, dverts, mask, tmd.smooth, 0.5f, {});
  weights::limit_and_normalize(
      dverts, mask, tmd.max_bones, tmd.flag & MOD_TRANSFERWEIGHTS_NORMALIZE);
  return mesh;
}

static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  layout.use_property_split_set(true);
  layout.prop(ptr, "object", UI_ITEM_NONE, IFACE_("Source"), ICON_NONE);
  layout.prop(ptr, "groups", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "max_distance", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "smooth", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "max_bones", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "use_normalize", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  modifier_error_message_draw(layout, ptr);
}

static void panel_register(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_TransferWeights, panel_draw);
}

ModifierTypeInfo modifierType_TransferWeights = {
    /*idname*/ "TransferWeights",
    /*name*/ N_("Transfer Weights"),
    /*struct_name*/ "TransferWeightsModifierData",
    /*struct_size*/ sizeof(TransferWeightsModifierData),
    /*srna*/ &RNA_TransferWeightsModifier,
    /*type*/ ModifierTypeType::NonGeometrical,
    /*flags*/ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsMapping |
        eModifierTypeFlag_SupportsEditmode,
    /*icon*/ ICON_MOD_TRANSFER_WEIGHTS,
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
    /*is_disabled*/ is_disabled,
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
