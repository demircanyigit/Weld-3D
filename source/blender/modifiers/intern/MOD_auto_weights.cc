/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Auto Weights: gives each deform bone of an armature its own vertex group. The weights are
 * worked out once, when binding, and stored in the modifier.
 *
 * The Heat method spreads heat from each bone over the surface, as in "Automatic Rigging and
 * Animation of 3D Characters" (Baran and Popović, 2007). Every point is heated by the bones it
 * can see. A loose part that sees no bone is heated by its nearest bones instead, so the solve
 * doesn't fail on meshes with holes, separate parts or parts inside others.
 */

#include <algorithm>
#include <string>

#include "BLI_array.hh"
#include "BLI_kdopbvh.hh"
#include "BLI_map.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_ordered_edge.hh"
#include "BLI_string.h"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "DNA_armature_types.h"
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

#include "BLO_read_write.hh"

#include "DEG_depsgraph_query.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "MEM_guardedalloc.h"

#include "eigen_capi.h"

#include "MOD_modifiertypes.hh"
#include "MOD_ui_common.hh"
#include "MOD_util.hh"
#include "MOD_weights_util.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Bind Data
 * \{ */

static void bind_free(AutoWeightsModifierData &amd)
{
  MEM_SAFE_DELETE(amd.bind_bones);
  MEM_SAFE_DELETE(amd.bind_offsets);
  MEM_SAFE_DELETE(amd.bind_groups);
  MEM_SAFE_DELETE(amd.bind_weights);
  amd.bind_verts_num = 0;
  amd.bind_bones_num = 0;
  amd.bind_weights_num = 0;
}

static void bind_copy(const AutoWeightsModifierData &src, AutoWeightsModifierData &dst)
{
  dst.bind_bones = MEM_dupalloc(src.bind_bones);
  dst.bind_offsets = MEM_dupalloc(src.bind_offsets);
  dst.bind_groups = MEM_dupalloc(src.bind_groups);
  dst.bind_weights = MEM_dupalloc(src.bind_weights);
  dst.bind_verts_num = src.bind_verts_num;
  dst.bind_bones_num = src.bind_bones_num;
  dst.bind_weights_num = src.bind_weights_num;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Heat
 * \{ */

struct BoneSegment {
  std::string name;
  float3 head;
  float3 tail;
  bool solve;
};

/** From the paper: how much a point's nearest bones heat it, and the cut-off of tiny weights. */
constexpr float HEAT_WEIGHT = 1.0f;
constexpr float WEIGHT_LIMIT_START = 0.05f;
constexpr float WEIGHT_LIMIT_END = 0.025f;
constexpr float DISTANCE_EPSILON = 1e-4f;

static float limit_weight(const float weight)
{
  if (weight < WEIGHT_LIMIT_END) {
    return 0.0f;
  }
  if (weight < WEIGHT_LIMIT_START) {
    return (weight - WEIGHT_LIMIT_END) / (WEIGHT_LIMIT_START - WEIGHT_LIMIT_END) *
           WEIGHT_LIMIT_START;
  }
  return std::min(weight, 1.0f);
}

static float3 closest_on_bone(const BoneSegment &bone, const float3 &p)
{
  float3 r;
  closest_to_line_segment_v3(r, p, bone.head, bone.tail);
  return r;
}

/** The distance to the bone, longer when the point's normal faces away from it. */
static float heat_distance(const BoneSegment &bone, const float3 &p, const float3 &normal)
{
  float3 d = p - closest_on_bone(bone, p);
  const float dist = math::length(d);
  if (dist < 1e-12f) {
    return 0.0f;
  }
  d /= dist;
  const float cosine = math::dot(d, normal);
  return dist / (0.5f * (cosine + 1.001f));
}

struct RayData {
  Span<float3> positions;
  Span<int> corner_verts;
  Span<int3> tris;
  float3 start;
  float3 dir;
};

/** Only faces turned towards the point block the view. */
static void ray_callback(void *userdata, int index, const BVHTreeRay * /*ray*/, BVHTreeRayHit *hit)
{
  const RayData &data = *static_cast<const RayData *>(userdata);
  const int3 &tri = data.tris[index];
  const float3 &v1 = data.positions[data.corner_verts[tri[0]]];
  const float3 &v2 = data.positions[data.corner_verts[tri[1]]];
  const float3 &v3 = data.positions[data.corner_verts[tri[2]]];
  float dist;
  if (isect_ray_tri_v3(data.start, data.dir, v1, v2, v3, &dist, nullptr)) {
    if (dist < hit->dist) {
      const float3 n = math::normalize(math::cross(v1 - v2, v2 - v3));
      if (math::dot(n, data.dir) < -1e-5f) {
        hit->index = index;
        hit->dist = dist;
      }
    }
  }
}

static bool bone_visible(const bke::BVHTreeFromMesh &tree,
                         const Span<float3> positions,
                         const Span<int> corner_verts,
                         const Span<int3> tris,
                         const float3 &p,
                         const BoneSegment &bone)
{
  RayData data{positions, corner_verts, tris, p, float3(0.0f)};
  float3 vec = closest_on_bone(bone, p) - p;
  data.start = p + vec * 1e-5f;
  vec *= 1.0f - 2e-5f;
  const float len = math::length(vec);
  if (len <= 0.0f) {
    return true;
  }
  BVHTreeRayHit hit;
  hit.index = -1;
  hit.dist = len;
  data.dir = vec / len;
  return BLI_bvhtree_ray_cast(tree.tree, data.start, data.dir, 0.0f, &hit, ray_callback, &data) ==
         -1;
}

static int find_root(Array<int> &parent, int i)
{
  while (parent[i] != i) {
    parent[i] = parent[parent[i]];
    i = parent[i];
  }
  return i;
}

/** The weights of each point for the solved bones, as (bone, weight) lists. Returns false when
 * the solver fails. */
static bool heat_weights(const Mesh &mesh,
                         const Span<BoneSegment> bones,
                         Array<Vector<std::pair<int, float>>> &r_weights)
{
  const int verts_num = mesh.verts_num;
  const Span<float3> positions = mesh.vert_positions();
  const Span<float3> normals = mesh.vert_normals();
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<int3> tris = mesh.corner_tris();
  const bke::BVHTreeFromMesh tree = mesh.bvh_corner_tris();

  Array<bool> on_face(verts_num, false);
  for (const int v : corner_verts) {
    on_face[v] = true;
  }

  /* The bones that heat each point: the nearest ones it can see. */
  Array<float> mindist(verts_num);
  Array<Vector<int, 2>> sources(verts_num);
  threading::parallel_for(IndexRange(verts_num), 256, [&](const IndexRange range) {
    for (const int v : range) {
      float best = FLT_MAX;
      for (const BoneSegment &bone : bones) {
        best = std::min(best, heat_distance(bone, positions[v], normals[v]));
      }
      mindist[v] = best;
      for (const int b : bones.index_range()) {
        if (heat_distance(bones[b], positions[v], normals[v]) <= best * (1.0f + DISTANCE_EPSILON))
        {
          if (!on_face[v] ||
              bone_visible(tree, positions, corner_verts, tris, positions[v], bones[b]))
          {
            sources[v].append(b);
          }
        }
      }
    }
  });

  /* A loose part where no point sees a bone is heated by its nearest bones. */
  Array<int> parent(verts_num);
  for (const int v : IndexRange(verts_num)) {
    parent[v] = v;
  }
  for (const int3 &tri : tris) {
    const int a = find_root(parent, corner_verts[tri[0]]);
    const int b = find_root(parent, corner_verts[tri[1]]);
    const int c = find_root(parent, corner_verts[tri[2]]);
    parent[b] = a;
    parent[find_root(parent, c)] = a;
  }
  Array<bool> part_heated(verts_num, false);
  for (const int v : IndexRange(verts_num)) {
    if (!sources[v].is_empty()) {
      part_heated[find_root(parent, v)] = true;
    }
  }
  for (const int v : IndexRange(verts_num)) {
    if (!part_heated[find_root(parent, v)]) {
      for (const int b : bones.index_range()) {
        if (heat_distance(bones[b], positions[v], normals[v]) <=
            mindist[v] * (1.0f + DISTANCE_EPSILON))
        {
          sources[v].append(b);
        }
      }
    }
  }

  /* Area of each point (mixed Voronoi). */
  Array<float> area(verts_num, 0.0f);
  Map<OrderedEdge, int> edge_faces;
  for (const int3 &tri : tris) {
    const int i[3] = {corner_verts[tri[0]], corner_verts[tri[1]], corner_verts[tri[2]]};
    for (const int k : IndexRange(3)) {
      edge_faces.add_or_modify(
          {i[k], i[(k + 1) % 3]}, [](int *n) { *n = 1; }, [](int *n) { (*n)++; });
    }
    const float3 &v1 = positions[i[0]];
    const float3 &v2 = positions[i[1]];
    const float3 &v3 = positions[i[2]];
    int obtuse = 0;
    if (angle_v3v3v3(v2, v1, v3) > DEG2RADF(90.0f)) {
      obtuse = 1;
    }
    else if (angle_v3v3v3(v1, v2, v3) > DEG2RADF(90.0f)) {
      obtuse = 2;
    }
    else if (angle_v3v3v3(v1, v3, v2) > DEG2RADF(90.0f)) {
      obtuse = 3;
    }
    if (obtuse > 0) {
      const float a = area_tri_v3(v1, v2, v3);
      area[i[0]] += (obtuse == 1) ? a : a * 0.5f;
      area[i[1]] += (obtuse == 2) ? a : a * 0.5f;
      area[i[2]] += (obtuse == 3) ? a : a * 0.5f;
    }
    else {
      const float t1 = cotangent_tri_weight_v3(v1, v2, v3) * math::distance_squared(v2, v3);
      const float t2 = cotangent_tri_weight_v3(v2, v3, v1) * math::distance_squared(v1, v3);
      const float t3 = cotangent_tri_weight_v3(v3, v1, v2) * math::distance_squared(v1, v2);
      area[i[0]] += (t2 + t3) * 0.25f;
      area[i[1]] += (t1 + t3) * 0.25f;
      area[i[2]] += (t1 + t2) * 0.25f;
    }
  }

  /* Heat of each point, scaled by its area so the system stays symmetric. */
  Array<float> heat(verts_num, 0.0f);
  for (const int v : IndexRange(verts_num)) {
    if (sources[v].is_empty()) {
      continue;
    }
    const float d = std::max(mindist[v], 1e-4f);
    const float scale = area[v] > 0.0f ? 2.0f * area[v] : 1.0f;
    heat[v] = float(sources[v].size()) * HEAT_WEIGHT / (d * d) * scale;
  }

  LinearSolver *solver = EIG_linear_solver_new(verts_num, verts_num, 1);
  for (const int v : IndexRange(verts_num)) {
    /* A point on no face and with no heat keeps a weight of zero. */
    const float diagonal = heat[v] > 0.0f ? heat[v] : (area[v] > 0.0f ? 0.0f : 1.0f);
    EIG_linear_solver_matrix_add(solver, v, v, diagonal);
  }
  for (const int3 &tri : tris) {
    const int i[3] = {corner_verts[tri[0]], corner_verts[tri[1]], corner_verts[tri[2]]};
    const float3 &v1 = positions[i[0]];
    const float3 &v2 = positions[i[1]];
    const float3 &v3 = positions[i[2]];
    const float t1 = cotangent_tri_weight_v3(v1, v2, v3) /
                     float(edge_faces.lookup({i[1], i[2]}));
    const float t2 = cotangent_tri_weight_v3(v2, v3, v1) /
                     float(edge_faces.lookup({i[2], i[0]}));
    const float t3 = cotangent_tri_weight_v3(v3, v1, v2) /
                     float(edge_faces.lookup({i[0], i[1]}));
    EIG_linear_solver_matrix_add(solver, i[0], i[0], t2 + t3);
    EIG_linear_solver_matrix_add(solver, i[1], i[1], t1 + t3);
    EIG_linear_solver_matrix_add(solver, i[2], i[2], t1 + t2);
    EIG_linear_solver_matrix_add(solver, i[0], i[1], -t3);
    EIG_linear_solver_matrix_add(solver, i[1], i[0], -t3);
    EIG_linear_solver_matrix_add(solver, i[1], i[2], -t1);
    EIG_linear_solver_matrix_add(solver, i[2], i[1], -t1);
    EIG_linear_solver_matrix_add(solver, i[2], i[0], -t2);
    EIG_linear_solver_matrix_add(solver, i[0], i[2], -t2);
  }

  /* Points heated by each bone. */
  Array<Vector<int>> heated_by(bones.size());
  for (const int v : IndexRange(verts_num)) {
    for (const int b : sources[v]) {
      heated_by[b].append(v);
    }
  }

  r_weights.reinitialize(verts_num);
  bool ok = true;
  for (const int b : bones.index_range()) {
    if (!bones[b].solve || heated_by[b].is_empty()) {
      continue;
    }
    for (const int v : heated_by[b]) {
      EIG_linear_solver_right_hand_side_add(solver, 0, v, heat[v] / float(sources[v].size()));
    }
    if (!EIG_linear_solver_solve(solver)) {
      ok = false;
      break;
    }
    for (const int v : IndexRange(verts_num)) {
      const float w = limit_weight(float(EIG_linear_solver_variable_get(solver, 0, v)));
      if (w > 0.0f) {
        r_weights[v].append({b, w});
      }
    }
  }
  EIG_linear_solver_delete(solver);
  return ok;
}

/** Weights by distance alone: nearer bones weigh more. */
static void distance_weights(const Mesh &mesh,
                             const Span<BoneSegment> bones,
                             Array<Vector<std::pair<int, float>>> &r_weights)
{
  const Span<float3> positions = mesh.vert_positions();
  r_weights.reinitialize(mesh.verts_num);
  threading::parallel_for(positions.index_range(), 512, [&](const IndexRange range) {
    Array<float> w(bones.size());
    for (const int v : range) {
      float sum = 0.0f;
      for (const int b : bones.index_range()) {
        const float d = std::max(math::distance(positions[v], closest_on_bone(bones[b], positions[v])),
                                 1e-5f);
        w[b] = 1.0f / (d * d * d * d);
        sum += w[b];
      }
      for (const int b : bones.index_range()) {
        const float weight = w[b] / sum;
        if (bones[b].solve && weight > 0.01f) {
          r_weights[v].append({b, weight});
        }
      }
    }
  });
}

static void collect_bones(const Bone &bone,
                          const float4x4 &to_mesh,
                          Object *armature,
                          const bool only_selected,
                          Vector<BoneSegment> &r_bones)
{
  if (!(bone.flag & BONE_NO_DEFORM) &&
      math::distance_squared(float3(bone.arm_head), float3(bone.arm_tail)) > 0.0f)
  {
    BoneSegment seg;
    seg.name = bone.name;
    seg.head = math::transform_point(to_mesh, float3(bone.arm_head));
    seg.tail = math::transform_point(to_mesh, float3(bone.arm_tail));
    seg.solve = !only_selected || weights::bone_selected(armature, bone.name);
    r_bones.append(std::move(seg));
  }
  for (const Bone &child : bone.childbase) {
    collect_bones(child, to_mesh, armature, only_selected, r_bones);
  }
}

static void bind(AutoWeightsModifierData &amd, const Object &ob, const Mesh &mesh)
{
  Object *armature = amd.object;
  const bArmature *arm = id_cast<const bArmature *>(armature->data);
  const float4x4 to_mesh = ob.world_to_object() * armature->object_to_world();
  Vector<BoneSegment> bones;
  for (const Bone &bone : arm->bonebase) {
    collect_bones(bone, to_mesh, armature, amd.flag & MOD_AUTOWEIGHTS_SELECTED, bones);
  }
  bind_free(amd);
  if (bones.is_empty()) {
    BKE_modifier_set_error(&ob, &amd.modifier, "The armature has no deform bones");
    return;
  }
  if (!std::any_of(bones.begin(), bones.end(), [](const BoneSegment &b) { return b.solve; })) {
    BKE_modifier_set_error(&ob, &amd.modifier, "No bones are selected");
    return;
  }

  Array<Vector<std::pair<int, float>>> weights;
  if (amd.method == MOD_AUTOWEIGHTS_DISTANCE) {
    distance_weights(mesh, bones, weights);
  }
  else if (!heat_weights(mesh, bones, weights)) {
    BKE_modifier_set_error(&ob, &amd.modifier, "Heat failed, try Distance");
    return;
  }

  /* Store only the bones that got weights. */
  Array<int> bone_slot(bones.size(), -1);
  int bones_used = 0;
  int weights_num = 0;
  for (const Vector<std::pair<int, float>> &list : weights) {
    for (const auto &[b, w] : list) {
      if (bone_slot[b] == -1) {
        bone_slot[b] = bones_used++;
      }
      weights_num++;
    }
  }
  amd.bind_verts_num = mesh.verts_num;
  amd.bind_bones_num = bones_used;
  amd.bind_weights_num = weights_num;
  amd.bind_bones = MEM_new_array_zeroed<AutoWeightsBone>(std::max(bones_used, 1), __func__);
  for (const int b : bones.index_range()) {
    if (bone_slot[b] != -1) {
      STRNCPY(amd.bind_bones[bone_slot[b]].name, bones[b].name.c_str());
    }
  }
  amd.bind_offsets = MEM_new_array_zeroed<int>(mesh.verts_num + 1, __func__);
  amd.bind_groups = MEM_new_array_zeroed<int>(std::max(weights_num, 1), __func__);
  amd.bind_weights = MEM_new_array_zeroed<float>(std::max(weights_num, 1), __func__);
  int n = 0;
  for (const int v : weights.index_range()) {
    amd.bind_offsets[v] = n;
    for (const auto &[b, w] : weights[v]) {
      amd.bind_groups[n] = bone_slot[b];
      amd.bind_weights[n] = w;
      n++;
    }
  }
  amd.bind_offsets[mesh.verts_num] = n;
}

/** \} */

static void init_data(ModifierData *md)
{
  AutoWeightsModifierData *amd = reinterpret_cast<AutoWeightsModifierData *>(md);
  INIT_DEFAULT_STRUCT_AFTER(amd, modifier);
}

static void copy_data(const ModifierData *md, ModifierData *target, const int flag)
{
  const AutoWeightsModifierData &amd = *reinterpret_cast<const AutoWeightsModifierData *>(md);
  AutoWeightsModifierData &tamd = *reinterpret_cast<AutoWeightsModifierData *>(target);
  BKE_modifier_copydata_generic(md, target, flag);
  bind_copy(amd, tamd);
}

static void free_data(ModifierData *md)
{
  bind_free(*reinterpret_cast<AutoWeightsModifierData *>(md));
}

static void required_data_mask(ModifierData * /*md*/, CustomData_MeshMasks *r_cddata_masks)
{
  r_cddata_masks->vmask |= CD_MASK_MDEFORMVERT;
}

static bool is_disabled(const Scene * /*scene*/, ModifierData *md, bool /*use_render_params*/)
{
  const AutoWeightsModifierData &amd = *reinterpret_cast<AutoWeightsModifierData *>(md);
  return amd.object == nullptr || amd.object->type != OB_ARMATURE;
}

static void foreach_ID_link(ModifierData *md, Object *ob, IDWalkFunc walk, void *user_data)
{
  AutoWeightsModifierData *amd = reinterpret_cast<AutoWeightsModifierData *>(md);
  walk(user_data, ob, reinterpret_cast<ID **>(&amd->object), IDWALK_CB_NOP);
}

static void update_depsgraph(ModifierData *md, const ModifierUpdateDepsgraphContext *ctx)
{
  AutoWeightsModifierData *amd = reinterpret_cast<AutoWeightsModifierData *>(md);
  if (amd->object != nullptr) {
    /* The bone names decide which groups are bone groups. */
    DEG_add_object_relation(ctx->node, amd->object, DEG_OB_COMP_PARAMETERS, "Auto Weights");
    /* Binding places the bones by where both objects are, so their transforms must be ready. */
    DEG_add_object_relation(ctx->node, amd->object, DEG_OB_COMP_TRANSFORM, "Auto Weights");
    DEG_add_depends_on_transform_relation(ctx->node, "Auto Weights");
  }
}

static Mesh *modify_mesh(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  AutoWeightsModifierData &amd = *reinterpret_cast<AutoWeightsModifierData *>(md);
  if (amd.object == nullptr) {
    return mesh;
  }

  if (amd.flag & MOD_AUTOWEIGHTS_BIND_REQUEST) {
    amd.flag &= ~MOD_AUTOWEIGHTS_BIND_REQUEST;
    if (DEG_is_active(ctx->depsgraph)) {
      bind(amd, *ctx->object, *mesh);
      AutoWeightsModifierData *amd_orig = reinterpret_cast<AutoWeightsModifierData *>(
          BKE_modifier_get_original(ctx->object, md));
      if (amd_orig) {
        bind_free(*amd_orig);
        bind_copy(amd, *amd_orig);
      }
    }
  }

  if (amd.bind_verts_num == 0) {
    return mesh;
  }
  if (amd.bind_verts_num != mesh->verts_num) {
    BKE_modifier_set_error(ctx->object,
                           md,
                           "The point count changed from %d to %d, bind again",
                           amd.bind_verts_num,
                           mesh->verts_num);
    return mesh;
  }

  Array<int> group_of(amd.bind_bones_num);
  for (const int b : group_of.index_range()) {
    group_of[b] = weights::ensure_group(*mesh, amd.bind_bones[b].name);
  }
  Array<bool> bound(weights::groups_num(*mesh), false);
  for (const int g : group_of) {
    bound[g] = true;
  }
  MutableSpan<MDeformVert> dverts = mesh->deform_verts_for_write();
  threading::parallel_for(dverts.index_range(), 1024, [&](const IndexRange range) {
    for (const int v : range) {
      MDeformVert &dv = dverts[v];
      for (int j = int(dv.totweight) - 1; j >= 0; j--) {
        if (dv.dw[j].def_nr < bound.size() && bound[dv.dw[j].def_nr]) {
          BKE_defvert_remove_group(&dv, &dv.dw[j]);
        }
      }
      for (int i = amd.bind_offsets[v]; i < amd.bind_offsets[v + 1]; i++) {
        BKE_defvert_add_index_notest(&dv, group_of[amd.bind_groups[i]], amd.bind_weights[i]);
      }
    }
  });

  Array<bool> mask = weights::group_mask(*mesh, amd.object, MOD_WEIGHT_GROUPS_BONES);
  for (const int g : mask.index_range()) {
    mask[g] = mask[g] || bound[g];
  }
  weights::limit_and_normalize(
      dverts, mask, amd.max_bones, amd.flag & MOD_AUTOWEIGHTS_NORMALIZE);
  return mesh;
}

static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);
  const bool is_bound = RNA_boolean_get(ptr, "is_bound");

  layout.use_property_split_set(true);
  ui::Layout *col = &layout.column(false);
  col->active_set(!is_bound);
  col->prop(ptr, "object", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  col->prop(ptr, "method", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  col->prop(ptr, "use_selected_bones", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  layout.prop(ptr, "max_bones", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "use_normalize", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  layout.separator();
  col = &layout.column(false);
  if (is_bound) {
    col->op("OBJECT_OT_auto_weights_bind", IFACE_("Unbind"), ICON_NONE);
  }
  else {
    PointerRNA arm_ptr = RNA_pointer_get(ptr, "object");
    col->active_set(!RNA_pointer_is_null(&arm_ptr));
    col->op("OBJECT_OT_auto_weights_bind", IFACE_("Bind"), ICON_NONE);
  }
  modifier_error_message_draw(layout, ptr);
}

static void panel_register(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_AutoWeights, panel_draw);
}

static void blend_write(BlendWriter *writer, const ID * /*id_owner*/, const ModifierData *md)
{
  const AutoWeightsModifierData &amd = *reinterpret_cast<const AutoWeightsModifierData *>(md);
  writer->write_struct(&amd);
  if (amd.bind_verts_num > 0) {
    writer->write_struct_array(amd.bind_bones_num, amd.bind_bones);
    BLO_write_int32_array(writer, amd.bind_verts_num + 1, amd.bind_offsets);
    BLO_write_int32_array(writer, amd.bind_weights_num, amd.bind_groups);
    BLO_write_float_array(writer, amd.bind_weights_num, amd.bind_weights);
  }
}

static void blend_read(BlendDataReader *reader, ModifierData *md)
{
  AutoWeightsModifierData &amd = *reinterpret_cast<AutoWeightsModifierData *>(md);
  amd.flag &= ~MOD_AUTOWEIGHTS_BIND_REQUEST;
  if (amd.bind_verts_num > 0) {
    BLO_read_struct_array(reader, AutoWeightsBone, amd.bind_bones_num, &amd.bind_bones);
    BLO_read_int32_array(reader, amd.bind_verts_num + 1, &amd.bind_offsets);
    BLO_read_int32_array(reader, amd.bind_weights_num, &amd.bind_groups);
    BLO_read_float_array(reader, amd.bind_weights_num, &amd.bind_weights);
    if (!amd.bind_offsets || (amd.bind_weights_num > 0 && (!amd.bind_groups || !amd.bind_weights)))
    {
      bind_free(amd);
    }
  }
  else {
    amd.bind_bones = nullptr;
    amd.bind_offsets = nullptr;
    amd.bind_groups = nullptr;
    amd.bind_weights = nullptr;
  }
}

ModifierTypeInfo modifierType_AutoWeights = {
    /*idname*/ "AutoWeights",
    /*name*/ N_("Auto Weights"),
    /*struct_name*/ "AutoWeightsModifierData",
    /*struct_size*/ sizeof(AutoWeightsModifierData),
    /*srna*/ &RNA_AutoWeightsModifier,
    /*type*/ ModifierTypeType::NonGeometrical,
    /*flags*/ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsMapping |
        eModifierTypeFlag_SupportsEditmode,
    /*icon*/ ICON_MOD_AUTO_WEIGHTS,
    /*copy_data*/ copy_data,
    /*deform_verts*/ nullptr,
    /*deform_matrices*/ nullptr,
    /*deform_verts_EM*/ nullptr,
    /*deform_matrices_EM*/ nullptr,
    /*modify_mesh*/ modify_mesh,
    /*modify_geometry_set*/ nullptr,
    /*init_data*/ init_data,
    /*required_data_mask*/ required_data_mask,
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
