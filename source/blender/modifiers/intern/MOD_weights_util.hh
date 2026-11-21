/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Helpers shared by the weight modifiers: Auto Weights, Smooth Weights, Transfer Weights and
 * Bone Colors.
 */

#pragma once

#include <algorithm>

#include "BLI_array.hh"
#include "BLI_listbase.h"
#include "BLI_math_vector.hh"
#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "DNA_action_types.h"
#include "DNA_armature_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"

#include "BKE_action.hh"
#include "BKE_armature.hh"
#include "BKE_deform.hh"
#include "BKE_mesh.hh"

#include "BKE_modifier.hh"

#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"

#include "MEM_guardedalloc.h"

namespace blender::weights {

/** The armature that deforms the object: the one of its first Armature modifier, else its
 * parent. */
inline Object *find_armature(const Object &ob)
{
  for (ModifierData *md = static_cast<ModifierData *>(ob.modifiers.first); md; md = md->next) {
    if (md->type == eModifierType_Armature) {
      Object *arm = reinterpret_cast<ArmatureModifierData *>(md)->object;
      if (arm && arm->type == OB_ARMATURE) {
        return arm;
      }
    }
  }
  if (ob.parent && ob.parent->type == OB_ARMATURE) {
    return ob.parent;
  }
  return nullptr;
}

/** Re-run the modifier when the armature's bones change or are selected, for modifiers that work
 * on bone groups or selected bones. The flag makes selecting bones tag the armature for an
 * update, as for the Mask modifier. */
inline void add_armature_relations(const ModifierUpdateDepsgraphContext *ctx, const char *name)
{
  Object *armature = find_armature(*ctx->object);
  if (armature == nullptr) {
    return;
  }
  DEG_add_object_relation(ctx->node, armature, DEG_OB_COMP_TRANSFORM, name);
  DEG_add_object_relation(ctx->node, armature, DEG_OB_COMP_PARAMETERS, name);
  id_cast<bArmature *>(armature->data)->flag |= ARM_HAS_VIZ_DEPS;
}

/** A deform bone of the armature with this name, or null. */
inline Bone *find_deform_bone(Object *armature, const char *name)
{
  if (armature == nullptr || armature->data == nullptr) {
    return nullptr;
  }
  Bone *bone = BKE_armature_find_bone_name(id_cast<bArmature *>(armature->data), name);
  if (bone == nullptr || (bone->flag & BONE_NO_DEFORM)) {
    return nullptr;
  }
  return bone;
}

/** Whether the bone is selected in the armature's pose. Reads the original armature, since
 * selecting doesn't update the evaluated one. */
inline bool bone_selected(Object *armature, const char *name)
{
  Object *orig = DEG_get_original(armature);
  if (orig == nullptr || orig->pose == nullptr) {
    return false;
  }
  const bPoseChannel *pchan = BKE_pose_channel_find_name(orig->pose, name);
  return pchan && (pchan->flag & POSE_SELECTED);
}

inline int groups_num(const Mesh &mesh)
{
  return BLI_listbase_count(&mesh.vertex_group_names);
}

/** Which vertex groups of the mesh to work on, for a #WeightGroupsMode. Without an armature,
 * "bone groups" are all groups. */
inline Array<bool> group_mask(const Mesh &mesh,
                              Object *armature,
                              const int mode,
                              const StringRef single = "")
{
  Array<bool> mask(groups_num(mesh), false);
  int i = 0;
  for (const bDeformGroup &dg : mesh.vertex_group_names) {
    switch (mode) {
      case MOD_WEIGHT_GROUPS_ALL:
        mask[i] = true;
        break;
      case MOD_WEIGHT_GROUPS_BONES:
        mask[i] = armature == nullptr || find_deform_bone(armature, dg.name) != nullptr;
        break;
      case MOD_WEIGHT_GROUPS_SELECTED_BONES:
        mask[i] = armature != nullptr && find_deform_bone(armature, dg.name) != nullptr &&
                  bone_selected(armature, dg.name);
        break;
      case MOD_WEIGHT_GROUPS_SINGLE:
        mask[i] = !single.is_empty() && single == dg.name;
        break;
    }
    i++;
  }
  return mask;
}

/** The index of the group with this name, added to the mesh when it has none. */
inline int ensure_group(Mesh &mesh, const StringRef name)
{
  const int index = BKE_defgroup_name_index(&mesh.vertex_group_names, name);
  if (index != -1) {
    return index;
  }
  bDeformGroup *dg = MEM_new<bDeformGroup>(__func__);
  name.copy_utf8_truncated(dg->name);
  BLI_addtail(&mesh.vertex_group_names, dg);
  return groups_num(mesh) - 1;
}

/** Removes the weights of the masked groups that are at or below the threshold. */
inline void remove_small(MDeformVert &dv, const Span<bool> mask, const float threshold)
{
  for (int j = int(dv.totweight) - 1; j >= 0; j--) {
    const MDeformWeight &dw = dv.dw[j];
    if (dw.def_nr < mask.size() && mask[dw.def_nr] && dw.weight <= threshold) {
      BKE_defvert_remove_group(&dv, &dv.dw[j]);
    }
  }
}

/** Keeps the strongest #max_bones weights of the masked groups (all when zero), then scales them
 * to add up to one when #normalize is set. */
inline void limit_and_normalize(MutableSpan<MDeformVert> dverts,
                                const Span<bool> mask,
                                const int max_bones,
                                const bool normalize)
{
  threading::parallel_for(dverts.index_range(), 1024, [&](const IndexRange range) {
    Vector<int> order;
    for (const int v : range) {
      MDeformVert &dv = dverts[v];
      remove_small(dv, mask, 0.0f);
      if (max_bones > 0) {
        order.clear();
        for (const int j : IndexRange(dv.totweight)) {
          if (dv.dw[j].def_nr < mask.size() && mask[dv.dw[j].def_nr]) {
            order.append(j);
          }
        }
        if (order.size() > max_bones) {
          /* Drop all but the strongest ones. */
          std::sort(order.begin(), order.end(), [&](const int a, const int b) {
            return dv.dw[a].weight > dv.dw[b].weight;
          });
          for (const int j : order.as_span().drop_front(max_bones)) {
            dv.dw[j].weight = 0.0f;
          }
          remove_small(dv, mask, 0.0f);
        }
      }
      if (normalize) {
        float sum = 0.0f;
        for (const MDeformWeight &dw : Span(dv.dw, dv.totweight)) {
          if (dw.def_nr < mask.size() && mask[dw.def_nr]) {
            sum += dw.weight;
          }
        }
        if (sum > 0.0f) {
          for (MDeformWeight &dw : MutableSpan(dv.dw, dv.totweight)) {
            if (dw.def_nr < mask.size() && mask[dw.def_nr]) {
              dw.weight /= sum;
            }
          }
        }
      }
    }
  });
}

/** Blurs the weights of the masked groups along the edges of the mesh, so they don't jump across
 * gaps. #influence, when not empty, scales the blur per point. */
inline void smooth_groups(const Mesh &mesh,
                          MutableSpan<MDeformVert> dverts,
                          const Span<bool> mask,
                          const int iterations,
                          const float factor,
                          const Span<float> influence)
{
  if (iterations <= 0 || factor <= 0.0f) {
    return;
  }
  const int verts_num = mesh.verts_num;
  const Span<int2> edges = mesh.edges();
  Array<int> degree(verts_num, 0);
  for (const int2 &edge : edges) {
    degree[edge[0]]++;
    degree[edge[1]]++;
  }
  /* The groups that are used at all. */
  Array<bool> used(mask.size(), false);
  for (const MDeformVert &dv : dverts) {
    for (const MDeformWeight &dw : Span(dv.dw, dv.totweight)) {
      if (dw.def_nr < mask.size() && mask[dw.def_nr]) {
        used[dw.def_nr] = true;
      }
    }
  }
  Array<float> w(verts_num);
  Array<float> sums(verts_num);
  for (const int group : mask.index_range()) {
    if (!used[group]) {
      continue;
    }
    threading::parallel_for(IndexRange(verts_num), 4096, [&](const IndexRange range) {
      for (const int v : range) {
        w[v] = BKE_defvert_find_weight(&dverts[v], group);
      }
    });
    for ([[maybe_unused]] const int step : IndexRange(iterations)) {
      sums.fill(0.0f);
      for (const int2 &edge : edges) {
        sums[edge[0]] += w[edge[1]];
        sums[edge[1]] += w[edge[0]];
      }
      threading::parallel_for(IndexRange(verts_num), 4096, [&](const IndexRange range) {
        for (const int v : range) {
          if (degree[v] == 0) {
            continue;
          }
          const float f = factor * (influence.is_empty() ? 1.0f : influence[v]);
          w[v] += (sums[v] / float(degree[v]) - w[v]) * f;
        }
      });
    }
    threading::parallel_for(IndexRange(verts_num), 1024, [&](const IndexRange range) {
      for (const int v : range) {
        MDeformVert &dv = dverts[v];
        if (w[v] > 0.001f) {
          BKE_defvert_ensure_index(&dv, group)->weight = std::min(w[v], 1.0f);
        }
        else if (MDeformWeight *dw = BKE_defvert_find_index(&dv, group)) {
          BKE_defvert_remove_group(&dv, dw);
        }
      }
    });
  }
}

}  // namespace blender::weights
