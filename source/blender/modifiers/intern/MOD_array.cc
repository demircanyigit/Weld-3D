/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Array modifier: duplicates the object multiple times along an axis.
 */

#include <algorithm>

#include "MEM_guardedalloc.h"

#include "BLI_utildefines.h"

#include "BLI_bounds.hh"
#include "BLI_index_mask.hh"
#include "BLI_listbase.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.h"
#include "BLI_span.hh"

#include "BLT_translation.hh"

#include "BLI_listbase_iterator.hh"

#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "BKE_anim_path.h"
#include "BKE_attribute.hh"
#include "BKE_attribute_legacy_convert.hh"
#include "BKE_curve.hh"
#include "BKE_customdata.hh"
#include "BKE_geometry_set.hh"
#include "BKE_instances.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_query.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_wrapper.hh"
#include "BKE_modifier.hh"
#include "BKE_object_deform.h"
#include "BKE_object_types.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "MOD_ui_common.hh"
#include "MOD_util.hh"

#include "BLO_read_write.hh"

#include "DEG_depsgraph.hh"

#include "GEO_mesh_merge_by_distance.hh"
#include "GEO_realize_instances.hh"

namespace blender {

static void init_data(ModifierData *md)
{
  ArrayModifierData *amd = reinterpret_cast<ArrayModifierData *>(md);
  INIT_DEFAULT_STRUCT_AFTER(amd, modifier);

  /* Open the first sub-panel by default,
   * it corresponds to Relative offset which is enabled too. */
  md->ui_expand_flag = UI_PANEL_DATA_EXPAND_ROOT | UI_SUBPANEL_DATA_EXPAND_1;
}

static void foreach_ID_link(ModifierData *md, Object *ob, IDWalkFunc walk, void *user_data)
{
  ArrayModifierData *amd = reinterpret_cast<ArrayModifierData *>(md);

  walk(user_data, ob, reinterpret_cast<ID **>(&amd->start_cap), IDWALK_CB_NOP);
  walk(user_data, ob, reinterpret_cast<ID **>(&amd->end_cap), IDWALK_CB_NOP);
  walk(user_data, ob, reinterpret_cast<ID **>(&amd->curve_ob), IDWALK_CB_NOP);
  walk(user_data, ob, reinterpret_cast<ID **>(&amd->offset_ob), IDWALK_CB_NOP);
  walk(user_data, ob, reinterpret_cast<ID **>(&amd->collection), IDWALK_CB_USER);
  for (ArraySourceObject &item : amd->objects) {
    walk(user_data, ob, reinterpret_cast<ID **>(&item.object), IDWALK_CB_NOP);
  }
}

static void free_data(ModifierData *md)
{
  ArrayModifierData *amd = reinterpret_cast<ArrayModifierData *>(md);
  BLI_freelistN(&amd->objects);
}

static void copy_data(const ModifierData *md, ModifierData *target, const int flag)
{
  BKE_modifier_copydata_generic(md, target, flag);
  const ArrayModifierData *src = reinterpret_cast<const ArrayModifierData *>(md);
  ArrayModifierData *dst = reinterpret_cast<ArrayModifierData *>(target);
  BLI_duplicatelist(&dst->objects, &src->objects);
}

static void blend_write(BlendWriter *writer, const ID * /*id_owner*/, const ModifierData *md)
{
  const ArrayModifierData *amd = reinterpret_cast<const ArrayModifierData *>(md);
  writer->write_struct(amd);
  writer->write_struct_list(&amd->objects);
}

static void blend_read(BlendDataReader *reader, ModifierData *md)
{
  ArrayModifierData *amd = reinterpret_cast<ArrayModifierData *>(md);
  BLO_read_struct_list(reader, ArraySourceObject, &amd->objects);
}

static void update_depsgraph(ModifierData *md, const ModifierUpdateDepsgraphContext *ctx)
{
  ArrayModifierData *amd = reinterpret_cast<ArrayModifierData *>(md);
  bool need_transform_dependency = false;
  if (amd->start_cap != nullptr) {
    DEG_add_object_relation(
        ctx->node, amd->start_cap, DEG_OB_COMP_GEOMETRY, "Array Modifier Start Cap");
  }
  if (amd->end_cap != nullptr) {
    DEG_add_object_relation(
        ctx->node, amd->end_cap, DEG_OB_COMP_GEOMETRY, "Array Modifier End Cap");
  }
  if (amd->curve_ob) {
    DEG_add_object_relation(
        ctx->node, amd->curve_ob, DEG_OB_COMP_GEOMETRY, "Array Modifier Curve");
    DEG_add_special_eval_flag(ctx->node, &amd->curve_ob->id, DAG_EVAL_NEED_CURVE_PATH);
  }
  if (amd->offset_ob != nullptr) {
    DEG_add_object_relation(
        ctx->node, amd->offset_ob, DEG_OB_COMP_TRANSFORM, "Array Modifier Offset");
    need_transform_dependency = true;
  }

  if (need_transform_dependency) {
    DEG_add_depends_on_transform_relation(ctx->node, "Array Modifier");
  }
  MOD_array_source_update_depsgraph(
      ctx, amd->source, amd->collection, amd->objects, "Array Modifier Source");
}

BLI_INLINE float sum_v3(const float v[3])
{
  return v[0] + v[1] + v[2];
}

/* Structure used for sorting vertices, when processing doubles */
struct SortVertsElem {
  int vertex_num; /* The original index of the vertex, prior to sorting */
  float co[3];    /* Its coordinates */
  float sum_co;   /* `sum_v3(co)`: just so we don't do the sum many times. */
};

static int svert_sum_cmp(const void *e1, const void *e2)
{
  const SortVertsElem *sv1 = static_cast<const SortVertsElem *>(e1);
  const SortVertsElem *sv2 = static_cast<const SortVertsElem *>(e2);

  if (sv1->sum_co > sv2->sum_co) {
    return 1;
  }
  if (sv1->sum_co < sv2->sum_co) {
    return -1;
  }

  return 0;
}

static void svert_from_mvert(SortVertsElem *sv,
                             const Span<float3> vert_positions,
                             const int i_begin,
                             const int i_end)
{
  int i;
  for (i = i_begin; i < i_end; i++, sv++) {
    sv->vertex_num = i;
    copy_v3_v3(sv->co, vert_positions[i]);
    sv->sum_co = sum_v3(vert_positions[i]);
  }
}

/**
 * Take as inputs two sets of verts, to be processed for detection of doubles and mapping.
 * Each set of verts is defined by its start within mverts array and its verts_num;
 * It builds a mapping for all vertices within source,
 * to vertices within target, or -1 if no double found.
 * The `int doubles_map[verts_source_num]` array must have been allocated by caller.
 */
static void dm_mvert_map_doubles(int *doubles_map,
                                 const Span<float3> vert_positions,
                                 const int target_start,
                                 const int target_verts_num,
                                 const int source_start,
                                 const int source_verts_num,
                                 const float dist)
{
  const float dist3 = (float(M_SQRT3) + 0.00005f) * dist; /* Just above `sqrt(3)`. */
  int i_source, i_target, i_target_low_bound, target_end, source_end;
  SortVertsElem *sve_source, *sve_target, *sve_target_low_bound;
  bool target_scan_completed;

  target_end = target_start + target_verts_num;
  source_end = source_start + source_verts_num;

  /* build array of MVerts to be tested for merging */
  SortVertsElem *sorted_verts_target = MEM_new_array_uninitialized<SortVertsElem>(
      size_t(target_verts_num), __func__);
  SortVertsElem *sorted_verts_source = MEM_new_array_uninitialized<SortVertsElem>(
      size_t(source_verts_num), __func__);

  /* Copy target vertices index and cos into SortVertsElem array */
  svert_from_mvert(sorted_verts_target, vert_positions, target_start, target_end);

  /* Copy source vertices index and cos into SortVertsElem array */
  svert_from_mvert(sorted_verts_source, vert_positions, source_start, source_end);

  /* sort arrays according to sum of vertex coordinates (sumco) */
  qsort(sorted_verts_target, target_verts_num, sizeof(SortVertsElem), svert_sum_cmp);
  qsort(sorted_verts_source, source_verts_num, sizeof(SortVertsElem), svert_sum_cmp);

  sve_target_low_bound = sorted_verts_target;
  i_target_low_bound = 0;
  target_scan_completed = false;

  /* Scan source vertices, in #SortVertsElem sorted array,
   * all the while maintaining the lower bound of possible doubles in target vertices. */
  for (i_source = 0, sve_source = sorted_verts_source; i_source < source_verts_num;
       i_source++, sve_source++)
  {
    int best_target_vertex = -1;
    float best_dist_sq = dist * dist;
    float sve_source_sumco;

    /* If source has already been assigned to a target (in an earlier call, with other chunks) */
    if (doubles_map[sve_source->vertex_num] != -1) {
      continue;
    }

    /* If target fully scanned already, then all remaining source vertices cannot have a double */
    if (target_scan_completed) {
      doubles_map[sve_source->vertex_num] = -1;
      continue;
    }

    sve_source_sumco = sum_v3(sve_source->co);

    /* Skip all target vertices that are more than dist3 lower in terms of sumco */
    /* and advance the overall lower bound, applicable to all remaining vertices as well. */
    while ((i_target_low_bound < target_verts_num) &&
           (sve_target_low_bound->sum_co < sve_source_sumco - dist3))
    {
      i_target_low_bound++;
      sve_target_low_bound++;
    }
    /* If end of target list reached, then no more possible doubles */
    if (i_target_low_bound >= target_verts_num) {
      doubles_map[sve_source->vertex_num] = -1;
      target_scan_completed = true;
      continue;
    }
    /* Test target candidates starting at the low bound of possible doubles,
     * ordered in terms of sumco. */
    i_target = i_target_low_bound;
    sve_target = sve_target_low_bound;

    /* i_target will scan vertices in the
     * [v_source_sumco - dist3;  v_source_sumco + dist3] range */

    while ((i_target < target_verts_num) && (sve_target->sum_co <= sve_source_sumco + dist3)) {
      /* Testing distance for candidate double in target */
      /* v_target is within dist3 of v_source in terms of sumco;  check real distance */
      const float dist_sq = len_squared_v3v3(sve_source->co, sve_target->co);
      if (dist_sq <= best_dist_sq) {
        /* Potential double found */
        best_dist_sq = dist_sq;
        best_target_vertex = sve_target->vertex_num;

        /* If target is already mapped, we only follow that mapping if final target remains
         * close enough from current vert (otherwise no mapping at all).
         * Note that if we later find another target closer than this one, then we check it.
         * But if other potential targets are farther,
         * then there will be no mapping at all for this source. */
        while (best_target_vertex != -1 &&
               !ELEM(doubles_map[best_target_vertex], -1, best_target_vertex))
        {
          if (compare_len_v3v3(vert_positions[sve_source->vertex_num],
                               vert_positions[doubles_map[best_target_vertex]],
                               dist))
          {
            best_target_vertex = doubles_map[best_target_vertex];
          }
          else {
            best_target_vertex = -1;
          }
        }
      }
      i_target++;
      sve_target++;
    }
    /* End of candidate scan: if none found then no doubles */
    doubles_map[sve_source->vertex_num] = best_target_vertex;
  }

  MEM_delete(sorted_verts_source);
  MEM_delete(sorted_verts_target);
}

static void mesh_merge_transform(Mesh *result,
                                 Mesh *cap_mesh,
                                 const float cap_offset[4][4],
                                 uint cap_verts_index,
                                 uint cap_edges_index,
                                 int cap_loops_index,
                                 int cap_faces_index,
                                 int cap_nverts,
                                 int cap_nedges,
                                 int cap_nloops,
                                 int cap_nfaces,
                                 int *remap,
                                 int remap_len,
                                 MutableSpan<float3> dst_vert_normals)
{
  int *index_orig;
  int i;
  int2 *edge;
  const Span<int> cap_face_offsets = cap_mesh->face_offsets();
  MutableSpan<float3> result_positions = result->vert_positions_for_write();
  MutableSpan<int2> result_edges = result->edges_for_write();
  MutableSpan<int> result_face_offsets = result->face_offsets_for_write();
  MutableSpan<int> result_corner_verts = result->corner_verts_for_write();
  MutableSpan<int> result_corner_edges = result->corner_edges_for_write();
  bke::MutableAttributeAccessor result_attributes = result->attributes_for_write();

  bke::LegacyMeshInterpolator vert_interp(*cap_mesh, *result, bke::AttrDomain::Point);
  bke::LegacyMeshInterpolator edge_interp(*cap_mesh, *result, bke::AttrDomain::Edge);
  bke::LegacyMeshInterpolator face_interp(*cap_mesh, *result, bke::AttrDomain::Face);
  bke::LegacyMeshInterpolator corner_interp(*cap_mesh, *result, bke::AttrDomain::Corner);

  vert_interp.copy(0, cap_verts_index, cap_nverts);
  edge_interp.copy(0, cap_edges_index, cap_nedges);
  face_interp.copy(0, cap_faces_index, cap_nfaces);
  corner_interp.copy(0, cap_loops_index, cap_nloops);

  for (i = 0; i < cap_nverts; i++) {
    mul_m4_v3(cap_offset, result_positions[cap_verts_index + i]);
  }

  /* We have to correct normals too, if we do not tag them as dirty later! */
  if (!dst_vert_normals.is_empty()) {
    for (i = 0; i < cap_nverts; i++) {
      mul_mat3_m4_v3(cap_offset, dst_vert_normals[cap_verts_index + i]);
      normalize_v3(dst_vert_normals[cap_verts_index + i]);
    }
  }

  /* remap the vertex groups if necessary */
  if (!result->deform_verts().is_empty()) {
    MDeformVert *dvert = result->deform_verts_for_write().data();
    BKE_object_defgroup_index_map_apply(&dvert[cap_verts_index], cap_nverts, remap, remap_len);
  }

  /* adjust cap edge vertex indices */
  edge = &result_edges[cap_edges_index];
  for (i = 0; i < cap_nedges; i++, edge++) {
    (*edge) += cap_verts_index;
  }

  /* Adjust cap face loop-start indices. */
  for (i = 0; i < cap_nfaces; i++) {
    result_face_offsets[cap_faces_index + i] = cap_face_offsets[i] + cap_loops_index;
  }

  /* adjust cap loop vertex and edge indices */
  for (i = 0; i < cap_nloops; i++) {
    result_corner_verts[cap_loops_index + i] += cap_verts_index;
    result_corner_edges[cap_loops_index + i] += cap_edges_index;
  }

  const bke::AttributeAccessor cap_attributes = cap_mesh->attributes();
  if (const VArray cap_material_indices = *cap_attributes.lookup<int>("material_index",
                                                                      bke::AttrDomain::Face))
  {
    bke::SpanAttributeWriter<int> result_material_indices =
        result_attributes.lookup_or_add_for_write_span<int>("material_index",
                                                            bke::AttrDomain::Face);
    cap_material_indices.materialize(
        result_material_indices.span.slice(cap_faces_index, cap_nfaces));
    result_material_indices.finish();
  }

  /* Set #CD_ORIGINDEX. */
  index_orig = static_cast<int *>(
      CustomData_get_layer_for_write(&result->vert_data, CD_ORIGINDEX, result->verts_num));
  if (index_orig) {
    copy_vn_i(index_orig + cap_verts_index, cap_nverts, ORIGINDEX_NONE);
  }

  index_orig = static_cast<int *>(
      CustomData_get_layer_for_write(&result->edge_data, CD_ORIGINDEX, result->edges_num));
  if (index_orig) {
    copy_vn_i(index_orig + cap_edges_index, cap_nedges, ORIGINDEX_NONE);
  }

  index_orig = static_cast<int *>(
      CustomData_get_layer_for_write(&result->face_data, CD_ORIGINDEX, result->faces_num));
  if (index_orig) {
    copy_vn_i(index_orig + cap_faces_index, cap_nfaces, ORIGINDEX_NONE);
  }

  index_orig = static_cast<int *>(
      CustomData_get_layer_for_write(&result->corner_data, CD_ORIGINDEX, result->corners_num));
  if (index_orig) {
    copy_vn_i(index_orig + cap_loops_index, cap_nloops, ORIGINDEX_NONE);
  }
}

/** The transform from one copy to the next, accumulating all offset settings. */
static void array_offset(const ArrayModifierData *amd,
                         const ModifierEvalContext *ctx,
                         const std::optional<Bounds<float3>> &bounds,
                         float r_offset[4][4])
{
  unit_m4(r_offset);

  if (amd->offset_type & MOD_ARR_OFF_CONST) {
    add_v3_v3(r_offset[3], amd->offset);
  }

  if ((amd->offset_type & MOD_ARR_OFF_RELATIVE) && bounds) {
    for (int j = 3; j--;) {
      r_offset[3][j] += amd->scale[j] * (bounds->max[j] - bounds->min[j]);
    }
  }

  if ((amd->offset_type & MOD_ARR_OFF_OBJ) && amd->offset_ob != nullptr) {
    float obinv[4][4];
    float result_mat[4][4];

    if (ctx->object) {
      invert_m4_m4(obinv, ctx->object->object_to_world().ptr());
    }
    else {
      unit_m4(obinv);
    }

    mul_m4_series(result_mat, r_offset, obinv, amd->offset_ob->object_to_world().ptr());
    copy_m4_m4(r_offset, result_mat);
  }
}

/**
 * The number of copies, from the fit type, limited to a sane amount of geometry.
 * \param chunk_nverts: The most points one copy can have.
 */
static int array_count(ArrayModifierData *amd,
                       const ModifierEvalContext *ctx,
                       const float offset[4][4],
                       const size_t chunk_nverts,
                       const size_t caps_nverts)
{
  int count = amd->count;
  float length = amd->length;

  if (amd->fit_type == MOD_ARR_FITCURVE && amd->curve_ob != nullptr) {
    Object *curve_ob = amd->curve_ob;
    CurveCache *curve_cache = curve_ob->runtime->curve_cache;
    if (curve_cache != nullptr && curve_cache->anim_path_accum_length != nullptr) {
      float scale_fac = mat4_to_scale(curve_ob->object_to_world().ptr());
      length = scale_fac * BKE_anim_path_get_length(curve_cache);
    }
  }

  /* About 67 million vertices max seems a decent limit for now. */
  const size_t max_verts_num = 1 << 26;

  /* calculate the maximum number of copies which will fit within the
   * prescribed length */
  if (ELEM(amd->fit_type, MOD_ARR_FITLENGTH, MOD_ARR_FITCURVE)) {
    const float float_epsilon = 1e-6f;
    bool offset_is_too_small = false;
    float dist = len_v3(offset[3]);

    if (dist > float_epsilon) {
      /* this gives length = first copy start to last copy end
       * add a tiny offset for floating point rounding errors */
      count = (length + float_epsilon) / dist + 1;

      /* Ensure we keep things to a reasonable level, in terms of rough total amount of generated
       * vertices.
       */
      if ((size_t(count) * chunk_nverts + caps_nverts) > max_verts_num) {
        count = 1;
        offset_is_too_small = true;
      }
    }
    else {
      /* if the offset has no translation, just make one copy */
      count = 1;
      offset_is_too_small = true;
    }

    if (offset_is_too_small) {
      BKE_modifier_set_error(
          ctx->object,
          &amd->modifier,
          "The offset is too small, we cannot generate the amount of geometry it would require");
    }
  }
  /* Ensure we keep things to a reasonable level, in terms of rough total amount of generated
   * vertices.
   */
  else if ((size_t(count) * chunk_nverts + caps_nverts) > max_verts_num) {
    count = 1;
    BKE_modifier_set_error(ctx->object,
                           &amd->modifier,
                           "The amount of copies is too high, we cannot generate the amount of "
                           "geometry it would require");
  }

  return std::max(count, 1);
}

static Mesh *arrayModifier_doArray(ArrayModifierData *amd,
                                   const ModifierEvalContext *ctx,
                                   Mesh *mesh)
{
  /* NOTE(@ideasman4): the `mesh` may be empty, the following code must account for this,
   * merging start/end caps into the empty mesh. Avoid an early return here as it can cause
   * problems if the expected custom-data layers don't exist in the resulting mesh,
   * see: #107353, #132991. */

  int2 *edge;
  int i, c, count;
  /* offset matrix */
  float offset[4][4];
  float scale[3];
  bool offset_has_scale;
  float current_offset[4][4];
  float final_offset[4][4];
  int *full_doubles_map = nullptr;
  int tot_doubles;

  const bool use_merge = (amd->flags & MOD_ARR_MERGE) != 0;
  const bool use_recalc_normals = BKE_mesh_vert_normals_are_dirty(mesh) || use_merge;

  int start_cap_nverts = 0, start_cap_nedges = 0, start_cap_nfaces = 0, start_cap_nloops = 0;
  int end_cap_nverts = 0, end_cap_nedges = 0, end_cap_nfaces = 0, end_cap_nloops = 0;
  int result_nverts = 0, result_nedges = 0, result_nfaces = 0, result_nloops = 0;
  int chunk_nverts, chunk_nedges, chunk_nloops, chunk_nfaces;
  int first_chunk_start, first_chunk_nverts, last_chunk_start, last_chunk_nverts;

  Mesh *result, *start_cap_mesh = nullptr, *end_cap_mesh = nullptr;

  int *vgroup_start_cap_remap = nullptr;
  int vgroup_start_cap_remap_len = 0;
  int *vgroup_end_cap_remap = nullptr;
  int vgroup_end_cap_remap_len = 0;

  chunk_nverts = mesh->verts_num;
  chunk_nedges = mesh->edges_num;
  chunk_nloops = mesh->corners_num;
  chunk_nfaces = mesh->faces_num;

  Object *start_cap_ob = amd->start_cap;
  if (start_cap_ob && start_cap_ob != ctx->object) {
    if (start_cap_ob->type == OB_MESH && ctx->object->type == OB_MESH) {
      vgroup_start_cap_remap = BKE_object_defgroup_index_map_create(
          start_cap_ob, ctx->object, &vgroup_start_cap_remap_len);
    }

    start_cap_mesh = BKE_modifier_get_evaluated_mesh_from_evaluated_object(start_cap_ob);
    if (start_cap_mesh) {
      BKE_mesh_wrapper_ensure_mdata(start_cap_mesh);
      start_cap_nverts = start_cap_mesh->verts_num;
      start_cap_nedges = start_cap_mesh->edges_num;
      start_cap_nloops = start_cap_mesh->corners_num;
      start_cap_nfaces = start_cap_mesh->faces_num;
    }
  }
  Object *end_cap_ob = amd->end_cap;
  if (end_cap_ob && end_cap_ob != ctx->object) {
    if (end_cap_ob->type == OB_MESH && ctx->object->type == OB_MESH) {
      vgroup_end_cap_remap = BKE_object_defgroup_index_map_create(
          end_cap_ob, ctx->object, &vgroup_end_cap_remap_len);
    }

    end_cap_mesh = BKE_modifier_get_evaluated_mesh_from_evaluated_object(end_cap_ob);
    if (end_cap_mesh) {
      BKE_mesh_wrapper_ensure_mdata(end_cap_mesh);
      end_cap_nverts = end_cap_mesh->verts_num;
      end_cap_nedges = end_cap_mesh->edges_num;
      end_cap_nloops = end_cap_mesh->corners_num;
      end_cap_nfaces = end_cap_mesh->faces_num;
    }
  }

  /* Build up offset array, accumulating all settings options. */
  array_offset(amd, ctx, mesh->bounds_min_max(), offset);

  /* Check if there is some scaling. If scaling, then we will not translate mapping */
  mat4_to_size(scale, offset);
  offset_has_scale = !is_one_v3(scale);

  count = array_count(amd, ctx, offset, chunk_nverts, start_cap_nverts + end_cap_nverts);

  /* The number of verts, edges, loops, faces, before eventually merging doubles */
  result_nverts = chunk_nverts * count + start_cap_nverts + end_cap_nverts;
  result_nedges = chunk_nedges * count + start_cap_nedges + end_cap_nedges;
  result_nloops = chunk_nloops * count + start_cap_nloops + end_cap_nloops;
  result_nfaces = chunk_nfaces * count + start_cap_nfaces + end_cap_nfaces;

  /* Initialize a result dm */
  result = BKE_mesh_new_nomain_from_template(
      mesh, result_nverts, result_nedges, result_nfaces, result_nloops);
  MutableSpan<float3> result_positions = result->vert_positions_for_write();
  MutableSpan<int2> result_edges = result->edges_for_write();
  MutableSpan<int> result_face_offsets = result->face_offsets_for_write();
  MutableSpan<int> result_corner_verts = result->corner_verts_for_write();
  MutableSpan<int> result_corner_edges = result->corner_edges_for_write();
  bke::MutableAttributeAccessor result_attributes = result->attributes_for_write();

  if (use_merge) {
    /* Will need full_doubles_map for handling merge */
    full_doubles_map = MEM_new_array_uninitialized<int>(size_t(result_nverts), __func__);
    copy_vn_i(full_doubles_map, result_nverts, -1);
  }

  /* copy customdata to original geometry */
  bke::LegacyMeshInterpolator vert_interp(*mesh, *result, bke::AttrDomain::Point);
  bke::LegacyMeshInterpolator edge_interp(*mesh, *result, bke::AttrDomain::Edge);
  bke::LegacyMeshInterpolator face_interp(*mesh, *result, bke::AttrDomain::Face);
  bke::LegacyMeshInterpolator corner_interp(*mesh, *result, bke::AttrDomain::Corner);
  vert_interp.copy(0, 0, chunk_nverts);
  edge_interp.copy(0, 0, chunk_nedges);
  face_interp.copy(0, 0, chunk_nfaces);
  corner_interp.copy(0, 0, chunk_nloops);

  result_face_offsets.take_front(mesh->faces_num).copy_from(mesh->face_offsets().drop_back(1));

  /* Remember first chunk, in case of cap merge */
  first_chunk_start = 0;
  first_chunk_nverts = chunk_nverts;

  unit_m4(current_offset);
  Span<float3> src_vert_normals;
  Vector<float3> dst_vert_normals;
  if (!use_recalc_normals) {
    src_vert_normals = mesh->vert_normals();
    dst_vert_normals.reinitialize(result->verts_num);
    dst_vert_normals.as_mutable_span()
        .take_front(src_vert_normals.size())
        .copy_from(src_vert_normals);
  }

  for (c = 1; c < count; c++) {
    /* copy customdata to new geometry */
    vert_interp.copy(0, c * chunk_nverts, chunk_nverts);
    edge_interp.copy(0, c * chunk_nedges, chunk_nedges);
    face_interp.copy(0, c * chunk_nfaces, chunk_nfaces);
    corner_interp.copy(0, c * chunk_nloops, chunk_nloops);

    /* recalculate cumulative offset here */
    mul_m4_m4m4(current_offset, current_offset, offset);

    if (chunk_nverts) {
      /* Apply offset to all new verts. */
      const int vert_offset = c * chunk_nverts;
      for (i = 0; i < chunk_nverts; i++) {
        const int i_dst = vert_offset + i;
        mul_m4_v3(current_offset, result_positions[i_dst]);

        /* We have to correct normals too, if we do not tag them as dirty! */
        if (!dst_vert_normals.is_empty()) {
          copy_v3_v3(dst_vert_normals[i_dst], src_vert_normals[i]);
          mul_mat3_m4_v3(current_offset, dst_vert_normals[i_dst]);
          normalize_v3(dst_vert_normals[i_dst]);
        }
      }
    }

    if (chunk_nedges) {
      /* Adjust edge vertex indices. */
      edge = &result_edges[c * chunk_nedges];
      for (i = 0; i < chunk_nedges; i++, edge++) {
        (*edge) += c * chunk_nverts;
      }
    }

    if (chunk_nfaces) {
      for (i = 0; i < chunk_nfaces; i++) {
        result_face_offsets[c * chunk_nfaces + i] = result_face_offsets[i] + c * chunk_nloops;
      }
    }

    if (chunk_nloops) {
      /* Adjust loop vertex and edge indices. */
      const int chunk_corner_start = c * chunk_nloops;
      for (i = 0; i < chunk_nloops; i++) {
        result_corner_verts[chunk_corner_start + i] += c * chunk_nverts;
        result_corner_edges[chunk_corner_start + i] += c * chunk_nedges;
      }
    }

    /* Handle merge between chunk n and n-1 */
    if (use_merge && (c >= 1)) {
      if (!offset_has_scale && (c >= 2)) {
        /* Mapping chunk 3 to chunk 2 is a translation of mapping 2 to 1
         * ... that is except if scaling makes the distance grow */
        int k;
        int this_chunk_index = c * chunk_nverts;
        int prev_chunk_index = (c - 1) * chunk_nverts;
        for (k = 0; k < chunk_nverts; k++, this_chunk_index++, prev_chunk_index++) {
          int target = full_doubles_map[prev_chunk_index];
          if (target != -1) {
            target += chunk_nverts; /* translate mapping */
            while (target != -1 && !ELEM(full_doubles_map[target], -1, target)) {
              /* If target is already mapped, we only follow that mapping if final target remains
               * close enough from current vert (otherwise no mapping at all). */
              if (compare_len_v3v3(result_positions[this_chunk_index],
                                   result_positions[full_doubles_map[target]],
                                   amd->merge_dist))
              {
                target = full_doubles_map[target];
              }
              else {
                target = -1;
              }
            }
          }
          full_doubles_map[this_chunk_index] = target;
        }
      }
      else {
        dm_mvert_map_doubles(full_doubles_map,
                             result_positions,
                             (c - 1) * chunk_nverts,
                             chunk_nverts,
                             c * chunk_nverts,
                             chunk_nverts,
                             amd->merge_dist);
      }
    }
  }

  /* handle UVs */
  if (chunk_nloops > 0 && is_zero_v2(amd->uv_offset) == false) {
    const VectorSet<StringRefNull> uv_map_names = result->uv_map_names();
    for (i = 0; i < uv_map_names.size(); i++) {
      bke::SpanAttributeWriter uv_map_attr = result_attributes.lookup_for_write_span<float2>(
          uv_map_names[i]);
      float2 *uv_map = uv_map_attr.span.data();
      uv_map += chunk_nloops;
      for (c = 1; c < count; c++) {
        const float uv_offset[2] = {
            amd->uv_offset[0] * float(c),
            amd->uv_offset[1] * float(c),
        };
        int l_index = chunk_nloops;
        for (; l_index-- != 0; uv_map++) {
          (*uv_map)[0] += uv_offset[0];
          (*uv_map)[1] += uv_offset[1];
        }
      }
      uv_map_attr.finish();
    }
  }

  if (!use_merge && !mesh->runtime->subsurf_optimal_display_edges.is_empty()) {
    const BoundedBitSpan src = mesh->runtime->subsurf_optimal_display_edges;

    result->runtime->subsurf_optimal_display_edges.resize(result->edges_num);
    MutableBoundedBitSpan dst = result->runtime->subsurf_optimal_display_edges;
    for (const int i : IndexRange(count)) {
      dst.slice({i * mesh->edges_num, mesh->edges_num}).copy_from(src);
    }

    if (start_cap_mesh) {
      MutableBitSpan cap_bits = dst.slice(
          {result_nedges - start_cap_nedges - end_cap_nedges, start_cap_mesh->edges_num});
      if (start_cap_mesh->runtime->subsurf_optimal_display_edges.is_empty()) {
        cap_bits.set_all(true);
      }
      else {
        cap_bits.copy_from(start_cap_mesh->runtime->subsurf_optimal_display_edges);
      }
    }
    if (end_cap_mesh) {
      MutableBitSpan cap_bits = dst.slice(
          {result_nedges - end_cap_nedges, end_cap_mesh->edges_num});
      if (end_cap_mesh->runtime->subsurf_optimal_display_edges.is_empty()) {
        cap_bits.set_all(true);
      }
      else {
        cap_bits.copy_from(end_cap_mesh->runtime->subsurf_optimal_display_edges);
      }
    }
  }

  last_chunk_start = (count - 1) * chunk_nverts;
  last_chunk_nverts = chunk_nverts;

  copy_m4_m4(final_offset, current_offset);

  if (use_merge && (amd->flags & MOD_ARR_MERGEFINAL) && (count > 1)) {
    /* Merge first and last copies */
    dm_mvert_map_doubles(full_doubles_map,
                         result_positions,
                         last_chunk_start,
                         last_chunk_nverts,
                         first_chunk_start,
                         first_chunk_nverts,
                         amd->merge_dist);
  }

  /* start capping */
  if (start_cap_mesh) {
    float start_offset[4][4];
    int start_cap_start = result_nverts - start_cap_nverts - end_cap_nverts;
    invert_m4_m4(start_offset, offset);
    mesh_merge_transform(result,
                         start_cap_mesh,
                         start_offset,
                         result_nverts - start_cap_nverts - end_cap_nverts,
                         result_nedges - start_cap_nedges - end_cap_nedges,
                         result_nloops - start_cap_nloops - end_cap_nloops,
                         result_nfaces - start_cap_nfaces - end_cap_nfaces,
                         start_cap_nverts,
                         start_cap_nedges,
                         start_cap_nloops,
                         start_cap_nfaces,
                         vgroup_start_cap_remap,
                         vgroup_start_cap_remap_len,
                         dst_vert_normals);
    /* Identify doubles with first chunk */
    if (use_merge) {
      dm_mvert_map_doubles(full_doubles_map,
                           result_positions,
                           first_chunk_start,
                           first_chunk_nverts,
                           start_cap_start,
                           start_cap_nverts,
                           amd->merge_dist);
    }
  }

  if (end_cap_mesh) {
    float end_offset[4][4];
    int end_cap_start = result_nverts - end_cap_nverts;
    mul_m4_m4m4(end_offset, current_offset, offset);
    mesh_merge_transform(result,
                         end_cap_mesh,
                         end_offset,
                         result_nverts - end_cap_nverts,
                         result_nedges - end_cap_nedges,
                         result_nloops - end_cap_nloops,
                         result_nfaces - end_cap_nfaces,
                         end_cap_nverts,
                         end_cap_nedges,
                         end_cap_nloops,
                         end_cap_nfaces,
                         vgroup_end_cap_remap,
                         vgroup_end_cap_remap_len,
                         dst_vert_normals);
    /* Identify doubles with last chunk */
    if (use_merge) {
      dm_mvert_map_doubles(full_doubles_map,
                           result_positions,
                           last_chunk_start,
                           last_chunk_nverts,
                           end_cap_start,
                           end_cap_nverts,
                           amd->merge_dist);
    }
  }
  /* done capping */

  if (!dst_vert_normals.is_empty()) {
    bke::mesh_vert_normals_assign(*result, std::move(dst_vert_normals));
  }

  /* Handle merging */
  tot_doubles = 0;
  if (use_merge) {
    for (i = 0; i < result_nverts; i++) {
      int new_i = full_doubles_map[i];
      if (new_i != -1) {
        /* We have to follow chains of doubles
         * (merge start/end especially is likely to create some),
         * those are not supported at all by `geometry::mesh_merge_verts`! */
        while (!ELEM(full_doubles_map[new_i], -1, new_i)) {
          new_i = full_doubles_map[new_i];
        }
        if (i == new_i) {
          full_doubles_map[i] = -1;
        }
        else {
          full_doubles_map[i] = new_i;
          tot_doubles++;
        }
      }
    }
    if (tot_doubles > 0) {
      Mesh *tmp = result;
      result = geometry::mesh_merge_verts(
          *tmp, MutableSpan<int>{full_doubles_map, result->verts_num}, tot_doubles, false);
      BKE_id_free(nullptr, tmp);
    }
    MEM_delete(full_doubles_map);
  }

  if (vgroup_start_cap_remap) {
    MEM_delete(vgroup_start_cap_remap);
  }
  if (vgroup_end_cap_remap) {
    MEM_delete(vgroup_end_cap_remap);
  }

  return result;
}

/**
 * Copies made of other objects' meshes, one picked for each copy by #ArrayModifierData.pick.
 * The meshes are placed as they are in their own object, without the object's transform.
 * Merging joins close points anywhere in the result, not only between neighboring copies.
 */
static Mesh *array_from_meshes(ArrayModifierData *amd,
                               const ModifierEvalContext *ctx,
                               Mesh *mesh,
                               const Span<const Mesh *> meshes)
{
  /* The copies are spaced by the size of all meshes they can be made of. */
  std::optional<Bounds<float3>> size;
  size_t chunk_nverts = 0;
  auto add_size = [&](const Mesh &copy) {
    size = bounds::merge(size, copy.bounds_min_max());
    chunk_nverts = std::max(chunk_nverts, size_t(copy.verts_num));
  };
  for (const Mesh *copy : meshes) {
    add_size(*copy);
  }
  if (amd->pick == MOD_ARRAY_PICK_ALTERNATE) {
    add_size(*mesh);
  }

  auto cap_mesh = [&](Object *cap_ob) -> const Mesh * {
    if (cap_ob == nullptr || cap_ob == ctx->object) {
      return nullptr;
    }
    Mesh *cap = BKE_modifier_get_evaluated_mesh_from_evaluated_object(cap_ob);
    if (cap != nullptr) {
      BKE_mesh_wrapper_ensure_mdata(cap);
    }
    return cap;
  };
  const Mesh *start_cap = cap_mesh(amd->start_cap);
  const Mesh *end_cap = cap_mesh(amd->end_cap);
  const size_t caps_nverts = (start_cap ? size_t(start_cap->verts_num) : 0) +
                             (end_cap ? size_t(end_cap->verts_num) : 0);

  float offset[4][4];
  array_offset(amd, ctx, size, offset);
  const int count = array_count(amd, ctx, offset, chunk_nverts, caps_nverts);
  const Array<int> picks = MOD_array_source_pick(amd->pick, amd->seed, count, int(meshes.size()));

  const int caps_num = (start_cap ? 1 : 0) + (end_cap ? 1 : 0);
  std::unique_ptr<bke::Instances> instances = std::make_unique<bke::Instances>(count + caps_num);
  /* The meshes are only read. */
  auto add_reference = [&](const Mesh &copy) {
    return instances->add_reference(bke::InstanceReference{bke::GeometrySet::from_mesh(
        const_cast<Mesh *>(&copy), bke::GeometryOwnershipType::ReadOnly)});
  };
  const int own_handle = add_reference(*mesh);
  Array<int> mesh_handles(meshes.size());
  for (const int i : meshes.index_range()) {
    mesh_handles[i] = add_reference(*meshes[i]);
  }

  MutableSpan<int> handles = instances->reference_handles_for_write();
  MutableSpan<float4x4> transforms = instances->transforms_for_write();
  const float4x4 step(offset);
  float4x4 current = float4x4::identity();
  for (const int c : IndexRange(count)) {
    handles[c] = picks[c] < 0 ? own_handle : mesh_handles[picks[c]];
    transforms[c] = current;
    current = current * step;
  }
  /* The caps go last, like in the copies of the mesh itself. */
  int cap_index = count;
  if (start_cap) {
    handles[cap_index] = add_reference(*start_cap);
    transforms[cap_index] = math::invert(step);
    cap_index++;
  }
  if (end_cap) {
    handles[cap_index] = add_reference(*end_cap);
    transforms[cap_index] = current;
  }

  geometry::RealizeInstancesOptions options;
  options.keep_original_ids = false;
  options.realize_instance_attributes = false;
  bke::GeometrySet realized =
      geometry::realize_instances(bke::GeometrySet::from_instances(std::move(instances)), options)
          .geometry;
  Mesh *result = realized.get_component_for_write<bke::MeshComponent>().release();
  if (result == nullptr) {
    return mesh;
  }

  /* The corners of the copies come one copy after another, in order. */
  if (!is_zero_v2(amd->uv_offset)) {
    bke::MutableAttributeAccessor attributes = result->attributes_for_write();
    for (const StringRefNull name : result->uv_map_names()) {
      bke::SpanAttributeWriter<float2> uv_map = attributes.lookup_for_write_span<float2>(name);
      if (!uv_map) {
        continue;
      }
      int corner_start = 0;
      for (const int c : IndexRange(count)) {
        const Mesh &copy = picks[c] < 0 ? *mesh : *meshes[picks[c]];
        const float2 uv_offset = float2(amd->uv_offset) * float(c);
        for (float2 &uv : uv_map.span.slice(corner_start, copy.corners_num)) {
          uv += uv_offset;
        }
        corner_start += copy.corners_num;
      }
      uv_map.finish();
    }
  }

  if ((amd->flags & MOD_ARR_MERGE) && count + caps_num > 1) {
    if (std::optional<Mesh *> merged = geometry::mesh_merge_by_distance_all(
            *result, IndexMask(result->verts_num), amd->merge_dist))
    {
      BKE_id_free(nullptr, result);
      result = *merged;
    }
  }

  result->runtime->edit_mesh = mesh->runtime->edit_mesh;
  return result;
}

static Mesh *modify_mesh(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  ArrayModifierData *amd = reinterpret_cast<ArrayModifierData *>(md);
  /* Other objects in place of the mesh, when there are any. */
  Vector<bke::GeometrySet> keep;
  const Vector<const Mesh *> meshes = MOD_array_source_meshes(
      amd->source, amd->collection, amd->objects, ctx->object, keep);
  if (!meshes.is_empty()) {
    return array_from_meshes(amd, ctx, mesh, meshes);
  }
  return arrayModifier_doArray(amd, ctx, mesh);
}

static bool is_disabled(const Scene * /*scene*/, ModifierData *md, bool /*use_render_params*/)
{
  ArrayModifierData *amd = reinterpret_cast<ArrayModifierData *>(md);

  /* The object type check is only needed here in case we have a placeholder
   * object assigned (because the library containing the curve/mesh is missing).
   *
   * In other cases it should be impossible to have a type mismatch.
   */

  if (amd->curve_ob && amd->curve_ob->type != OB_CURVES_LEGACY) {
    return true;
  }
  if (amd->start_cap && amd->start_cap->type != OB_MESH) {
    return true;
  }
  if (amd->end_cap && amd->end_cap->type != OB_MESH) {
    return true;
  }

  return false;
}

static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  layout.use_property_split_set(true);

  modifier_array_source_draw(layout, ptr);

  layout.prop(ptr, "fit_type", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  int fit_type = RNA_enum_get(ptr, "fit_type");
  if (fit_type == MOD_ARR_FIXEDCOUNT) {
    layout.prop(ptr, "count", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  else if (fit_type == MOD_ARR_FITLENGTH) {
    layout.prop(ptr, "fit_length", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  else if (fit_type == MOD_ARR_FITCURVE) {
    layout.prop(ptr, "curve", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }

  modifier_error_message_draw(layout, ptr);
}

static void relative_offset_header_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);

  layout.prop(ptr, "use_relative_offset", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void relative_offset_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);

  layout.use_property_split_set(true);

  ui::Layout &col = layout.column(false);

  col.active_set(RNA_boolean_get(ptr, "use_relative_offset"));
  col.prop(ptr, "relative_offset_displace", UI_ITEM_NONE, IFACE_("Factor"), ICON_NONE);
}

static void constant_offset_header_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);

  layout.prop(ptr, "use_constant_offset", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void constant_offset_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);

  layout.use_property_split_set(true);

  ui::Layout &col = layout.column(false);

  col.active_set(RNA_boolean_get(ptr, "use_constant_offset"));
  col.prop(ptr, "constant_offset_displace", UI_ITEM_NONE, IFACE_("Distance"), ICON_NONE);
}

/**
 * Object offset in a subpanel for consistency with the other offset types.
 */
static void object_offset_header_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);

  layout.prop(ptr, "use_object_offset", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void object_offset_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);

  layout.use_property_split_set(true);

  ui::Layout &col = layout.column(false);

  col.active_set(RNA_boolean_get(ptr, "use_object_offset"));
  col.prop(ptr, "offset_object", UI_ITEM_NONE, IFACE_("Object"), ICON_NONE);
}

static void symmetry_panel_header_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);

  layout.prop(ptr, "use_merge_vertices", UI_ITEM_NONE, IFACE_("Merge"), ICON_NONE);
}

static void symmetry_panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);

  layout.use_property_split_set(true);

  ui::Layout &col = layout.column(false);
  col.active_set(RNA_boolean_get(ptr, "use_merge_vertices"));
  col.prop(ptr, "merge_threshold", UI_ITEM_NONE, IFACE_("Distance"), ICON_NONE);
  col.prop(
      ptr, "use_merge_vertices_cap", UI_ITEM_NONE, IFACE_("First and Last Copies"), ICON_NONE);
}

static void uv_panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);

  layout.use_property_split_set(true);

  ui::Layout &col = layout.column(true);
  col.prop(ptr, "offset_u", ui::ITEM_R_EXPAND, IFACE_("Offset U"), ICON_NONE);
  col.prop(ptr, "offset_v", ui::ITEM_R_EXPAND, IFACE_("V"), ICON_NONE);
}

static void caps_panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);

  layout.use_property_split_set(true);

  ui::Layout &col = layout.column(false);
  col.prop(ptr, "start_cap", UI_ITEM_NONE, IFACE_("Cap Start"), ICON_NONE);
  col.prop(ptr, "end_cap", UI_ITEM_NONE, IFACE_("End"), ICON_NONE);
}

static void panel_register(ARegionType *region_type)
{
  PanelType *panel_type = modifier_panel_register(region_type, eModifierType_Array, panel_draw);
  modifier_subpanel_register(region_type,
                             "relative_offset",
                             "",
                             relative_offset_header_draw,
                             relative_offset_draw,
                             panel_type);
  modifier_subpanel_register(region_type,
                             "constant_offset",
                             "",
                             constant_offset_header_draw,
                             constant_offset_draw,
                             panel_type);
  modifier_subpanel_register(
      region_type, "object_offset", "", object_offset_header_draw, object_offset_draw, panel_type);
  modifier_subpanel_register(
      region_type, "merge", "", symmetry_panel_header_draw, symmetry_panel_draw, panel_type);
  modifier_subpanel_register(region_type, "uv", "UVs", nullptr, uv_panel_draw, panel_type);
  modifier_subpanel_register(region_type, "caps", "Caps", nullptr, caps_panel_draw, panel_type);
}

ModifierTypeInfo modifierType_Array = {
    /*idname*/ "Array",
    /*name*/ N_("Array"),
    /*struct_name*/ "ArrayModifierData",
    /*struct_size*/ sizeof(ArrayModifierData),
    /*srna*/ &RNA_ArrayModifier,
    /*type*/ ModifierTypeType::Constructive,
    /*flags*/ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsMapping |
        eModifierTypeFlag_SupportsEditmode | eModifierTypeFlag_EnableInEditmode |
        eModifierTypeFlag_AcceptsCVs,
    /*icon*/ ICON_MOD_ARRAY,

    /*copy_data*/ copy_data,

    /*deform_verts*/ nullptr,
    /*deform_matrices*/ nullptr,
    /*deform_verts_EM*/ nullptr,
    /*deform_matrices_EM*/ nullptr,
    /*modify_mesh*/ modify_mesh,
    /*modify_geometry_set*/ nullptr,

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
