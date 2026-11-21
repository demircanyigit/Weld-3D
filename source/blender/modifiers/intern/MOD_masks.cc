/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Masks: works out a gray value for every point (occlusion, curvature, thickness, edges,
 * direction, height, proximity, parts or noise) and blends it over what an attribute or vertex
 * group already holds, so several Masks modifiers can be stacked into one mask.
 */

#include <algorithm>
#include <queue>

#include "BLI_array.hh"
#include "BLI_color.hh"
#include "BLI_disjoint_set.hh"
#include "BLI_kdopbvh.hh"
#include "BLI_math_base.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_noise.hh"
#include "BLI_task.hh"

#include "BLT_translation.hh"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "BKE_attribute.hh"
#include "BKE_bvhutils.hh"
#include "BKE_deform.hh"
#include "BKE_lib_query.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_mapping.hh"
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

static void init_data(ModifierData *md)
{
  MasksModifierData *mmd = reinterpret_cast<MasksModifierData *>(md);
  INIT_DEFAULT_STRUCT_AFTER(mmd, modifier);
}

static void required_data_mask(ModifierData *md, CustomData_MeshMasks *r_cddata_masks)
{
  const MasksModifierData &mmd = *reinterpret_cast<MasksModifierData *>(md);
  if (mmd.vertex_group[0] != '\0') {
    r_cddata_masks->vmask |= CD_MASK_MDEFORMVERT;
  }
}

static bool is_disabled(const Scene * /*scene*/, ModifierData *md, bool /*use_render_params*/)
{
  const MasksModifierData &mmd = *reinterpret_cast<MasksModifierData *>(md);
  if (mmd.attribute_name[0] == '\0' && mmd.vertex_group[0] == '\0') {
    return true;
  }
  return mmd.mask_type == MOD_MASKS_PROXIMITY &&
         (mmd.object == nullptr || mmd.object->type != OB_MESH);
}

static void foreach_ID_link(ModifierData *md, Object *ob, IDWalkFunc walk, void *user_data)
{
  MasksModifierData *mmd = reinterpret_cast<MasksModifierData *>(md);
  walk(user_data, ob, reinterpret_cast<ID **>(&mmd->object), IDWALK_CB_NOP);
}

static bool uses_object(const MasksModifierData &mmd)
{
  return mmd.object != nullptr &&
         ELEM(mmd.mask_type, MOD_MASKS_OCCLUSION, MOD_MASKS_THICKNESS, MOD_MASKS_PROXIMITY);
}

static void update_depsgraph(ModifierData *md, const ModifierUpdateDepsgraphContext *ctx)
{
  MasksModifierData *mmd = reinterpret_cast<MasksModifierData *>(md);
  if (uses_object(*mmd)) {
    DEG_add_object_relation(ctx->node, mmd->object, DEG_OB_COMP_GEOMETRY, "Masks");
    DEG_add_object_relation(ctx->node, mmd->object, DEG_OB_COMP_TRANSFORM, "Masks");
  }
  DEG_add_depends_on_transform_relation(ctx->node, "Masks");
}

/* -------------------------------------------------------------------- */
/** \name Helpers
 * \{ */

static uint32_t hash_u32(uint32_t x)
{
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

static float hash_float(const uint32_t a, const uint32_t b)
{
  return float(hash_u32(a ^ hash_u32(b)) >> 8) * (1.0f / 16777216.0f);
}

static float smoothstep(const float x)
{
  const float t = std::clamp(x, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

/** A surface rays can hit, in its own space. */
struct Blocker {
  const bke::BVHTreeFromMesh *tree;
  float4x4 to_local;
};

/** How far along \a dir the first hit is, as a share of \a max_dist, or -1 for none. */
static float first_hit(const Span<Blocker> blockers,
                       const float3 &origin,
                       const float3 &dir,
                       const float max_dist)
{
  float best = -1.0f;
  for (const Blocker &blocker : blockers) {
    float3 o = math::transform_point(blocker.to_local, origin);
    float3 d = math::transform_direction(blocker.to_local, dir);
    const float scale = math::length(d);
    if (scale < 1e-12f) {
      continue;
    }
    d /= scale;
    BVHTreeRayHit hit;
    hit.index = -1;
    hit.dist = max_dist * scale;
    if (BLI_bvhtree_ray_cast(blocker.tree->tree,
                             o,
                             d,
                             0.0f,
                             &hit,
                             blocker.tree->raycast_callback,
                             (void *)blocker.tree) != -1)
    {
      const float t = hit.dist / (max_dist * scale);
      if (best < 0.0f || t < best) {
        best = t;
      }
    }
  }
  return best;
}

/**
 * Rays over the half sphere around \a normal, spread evenly and turned by a random angle per
 * point. Returns per point the occlusion (share of rays blocked) or, for thickness, how close
 * the hits are on average.
 */
static Array<float> cast_rays(const Mesh &mesh,
                              const MasksModifierData &mmd,
                              const Span<Blocker> blockers,
                              const bool inward,
                              const ModifierEvalContext *ctx)
{
  const Span<float3> positions = mesh.vert_positions();
  const Span<float3> normals = mesh.vert_normals();
  const int rays = std::max<int>(mmd.rays, 1);
  const float max_dist = std::max(mmd.distance, 1e-6f);
  const std::optional<Bounds<float3>> bounds = mesh.bounds_min_max();
  const float size = bounds ? math::distance(bounds->min, bounds->max) : 1.0f;
  const float eps = std::max(size * 1e-4f, 1e-6f);
  const float max_sin = math::sin(mmd.spread * float(M_PI_2));
  const float golden = float(M_PI) * (3.0f - math::sqrt(5.0f));

  Array<float> out(mesh.verts_num, 0.0f);
  threading::parallel_for(positions.index_range(), 64, [&](const IndexRange range) {
    /* Esc stops it, the modifier stack then turns it off. */
    if (BKE_modifier_stop_requested(ctx)) {
      return;
    }
    for (const int v : range) {
      float3 n = normals[v];
      if (inward) {
        n = -n;
      }
      float3 t1, t2;
      ortho_basis_v3v3_v3(t1, t2, n);
      const float3 origin = positions[v] + n * eps;
      const float turn = hash_float(uint32_t(v), uint32_t(mmd.seed)) * float(M_PI * 2.0);
      float sum = 0.0f;
      for (int i = 0; i < rays; i++) {
        /* Cosine weighted: more rays near the normal, as light falls. */
        const float r = math::sqrt((float(i) + 0.5f) / float(rays)) * max_sin;
        const float phi = float(i) * golden + turn;
        const float z = math::sqrt(std::max(0.0f, 1.0f - r * r));
        const float3 dir = t1 * (r * math::cos(phi)) + t2 * (r * math::sin(phi)) + n * z;
        const float t = first_hit(blockers, origin, dir, max_dist);
        if (t >= 0.0f) {
          sum += inward ? 1.0f - t : 1.0f;
        }
      }
      out[v] = sum / float(rays);
    }
  });
  return out;
}

static Array<float> curvature(const Mesh &mesh, const MasksModifierData &mmd)
{
  const Span<float3> positions = mesh.vert_positions();
  const Span<float3> normals = mesh.vert_normals();
  Array<float> sum(mesh.verts_num, 0.0f);
  Array<int> count(mesh.verts_num, 0);
  for (const int2 &edge : mesh.edges()) {
    const float3 d = positions[edge[1]] - positions[edge[0]];
    const float len = math::length(d);
    if (len < 1e-12f) {
      continue;
    }
    /* The neighbor lies below the tangent plane on an outer edge. */
    sum[edge[0]] += -math::dot(normals[edge[0]], d) / len;
    sum[edge[1]] += math::dot(normals[edge[1]], d) / len;
    count[edge[0]]++;
    count[edge[1]]++;
  }
  Array<float> out(mesh.verts_num);
  for (const int v : out.index_range()) {
    const float c = count[v] ? sum[v] / float(count[v]) * mmd.scale : 0.0f;
    switch (mmd.curvature) {
      case MOD_MASKS_CURVATURE_CONCAVE:
        out[v] = std::clamp(-c, 0.0f, 1.0f);
        break;
      case MOD_MASKS_CURVATURE_BOTH:
        out[v] = std::clamp(0.5f + c * 0.5f, 0.0f, 1.0f);
        break;
      default:
        out[v] = std::clamp(c, 0.0f, 1.0f);
        break;
    }
  }
  return out;
}

/** 1 on sharp edges and open borders, falling to 0 at the distance, along the surface. */
static Array<float> edges(const Mesh &mesh, const MasksModifierData &mmd)
{
  const Span<float3> positions = mesh.vert_positions();
  const Span<int2> mesh_edges = mesh.edges();
  const Span<float3> face_normals = mesh.face_normals();
  Array<int> e2f_offsets, e2f_indices;
  const GroupedSpan<int> edge_faces = bke::mesh::build_edge_to_face_map(
      mesh.faces(), mesh.corner_edges(), mesh.edges_num, e2f_offsets, e2f_indices);
  const float cos_angle = math::cos(mmd.angle);

  Array<float> dist(mesh.verts_num, FLT_MAX);
  using Item = std::pair<float, int>;
  std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
  for (const int e : mesh_edges.index_range()) {
    const Span<int> faces = edge_faces[e];
    bool seed = false;
    if (faces.size() == 1) {
      seed = mmd.flag & MOD_MASKS_BORDERS;
    }
    else if (faces.size() == 2) {
      seed = math::dot(face_normals[faces[0]], face_normals[faces[1]]) < cos_angle;
    }
    else if (faces.size() > 2) {
      seed = true;
    }
    if (seed) {
      for (const int v : {mesh_edges[e][0], mesh_edges[e][1]}) {
        if (dist[v] > 0.0f) {
          dist[v] = 0.0f;
          queue.push({0.0f, v});
        }
      }
    }
  }

  Array<int> v2e_offsets, v2e_indices;
  const GroupedSpan<int> vert_edges = bke::mesh::build_vert_to_edge_map(
      mesh_edges, mesh.verts_num, v2e_offsets, v2e_indices);
  const float max_dist = std::max(mmd.distance, 1e-6f);
  while (!queue.empty()) {
    const auto [d, v] = queue.top();
    queue.pop();
    if (d > dist[v] || d > max_dist) {
      continue;
    }
    for (const int e : vert_edges[v]) {
      const int u = bke::mesh::edge_other_vert(mesh_edges[e], v);
      const float nd = d + math::distance(positions[v], positions[u]);
      if (nd < dist[u]) {
        dist[u] = nd;
        queue.push({nd, u});
      }
    }
  }
  Array<float> out(mesh.verts_num);
  for (const int v : out.index_range()) {
    out[v] = 1.0f - std::clamp(dist[v] / max_dist, 0.0f, 1.0f);
  }
  return out;
}

static Array<float> direction(const Mesh &mesh, const MasksModifierData &mmd, const Object &ob)
{
  const Span<float3> normals = mesh.vert_normals();
  const float3x3 to_world = math::transpose(math::invert(float3x3(ob.object_to_world())));
  const float3 dir = math::normalize(float3(mmd.direction));
  const float threshold = 1.0f - 2.0f * mmd.coverage;
  const float soft = mmd.softness;
  Array<float> out(mesh.verts_num);
  threading::parallel_for(out.index_range(), 4096, [&](const IndexRange range) {
    for (const int v : range) {
      const float d = math::dot(math::normalize(to_world * normals[v]), dir);
      if (soft < 1e-4f) {
        out[v] = d >= threshold ? 1.0f : 0.0f;
      }
      else {
        out[v] = smoothstep((d - (threshold - soft)) / (2.0f * soft));
      }
    }
  });
  return out;
}

static Array<float> height(const Mesh &mesh, const MasksModifierData &mmd)
{
  const Span<float3> positions = mesh.vert_positions();
  const int axis = std::clamp(int(mmd.axis), 0, 2);
  const std::optional<Bounds<float3>> bounds = mesh.bounds_min_max();
  const float lo = bounds ? bounds->min[axis] : 0.0f;
  const float span = bounds ? std::max(bounds->max[axis] - lo, 1e-12f) : 1.0f;
  Array<float> out(mesh.verts_num);
  for (const int v : out.index_range()) {
    out[v] = std::clamp((positions[v][axis] - lo) / span, 0.0f, 1.0f);
  }
  return out;
}

static Array<float> proximity(const Mesh &mesh,
                              const MasksModifierData &mmd,
                              const Object &ob,
                              const Mesh &target)
{
  const Span<float3> positions = mesh.vert_positions();
  const bke::BVHTreeFromMesh tree = target.bvh_corner_tris();
  const float4x4 to_world = ob.object_to_world();
  const float4x4 target_to_world = mmd.object->object_to_world();
  const float4x4 to_target = mmd.object->world_to_object() * to_world;
  const float max_dist = mmd.distance;
  Array<float> out(mesh.verts_num, 0.0f);
  threading::parallel_for(out.index_range(), 256, [&](const IndexRange range) {
    for (const int v : range) {
      BVHTreeNearest nearest;
      nearest.index = -1;
      nearest.dist_sq = FLT_MAX;
      BLI_bvhtree_find_nearest(tree.tree,
                               math::transform_point(to_target, positions[v]),
                               &nearest,
                               tree.nearest_callback,
                               (void *)&tree);
      if (nearest.index == -1) {
        continue;
      }
      const float d = math::distance(math::transform_point(to_world, positions[v]),
                                     math::transform_point(target_to_world, float3(nearest.co)));
      out[v] = max_dist > 0.0f ? 1.0f - std::clamp(d / max_dist, 0.0f, 1.0f) :
                                 float(d < 1e-5f);
    }
  });
  return out;
}

static Array<float> parts(const Mesh &mesh, const MasksModifierData &mmd)
{
  DisjointSet<int> sets(mesh.verts_num);
  for (const int2 &edge : mesh.edges()) {
    sets.join(edge[0], edge[1]);
  }
  /* Each part is known by its lowest point index, which does not change with the joins. */
  Array<int> first(mesh.verts_num, INT_MAX);
  for (const int v : IndexRange(mesh.verts_num)) {
    int &f = first[sets.find_root(v)];
    f = std::min(f, v);
  }
  Array<float> out(mesh.verts_num);
  for (const int v : out.index_range()) {
    out[v] = hash_float(uint32_t(first[sets.find_root(v)]), uint32_t(mmd.seed) ^ 0x9e3779b9u);
  }
  return out;
}

static Array<float> noise_mask(const Mesh &mesh, const MasksModifierData &mmd)
{
  const Span<float3> positions = mesh.vert_positions();
  const float3 offset(hash_float(uint32_t(mmd.seed), 1u) * 1000.0f,
                      hash_float(uint32_t(mmd.seed), 2u) * 1000.0f,
                      hash_float(uint32_t(mmd.seed), 3u) * 1000.0f);
  Array<float> out(mesh.verts_num);
  threading::parallel_for(out.index_range(), 1024, [&](const IndexRange range) {
    for (const int v : range) {
      out[v] = noise::perlin_fbm<float3>(
          positions[v] * mmd.scale + offset, float(mmd.detail), 0.5f, 2.0f, true);
    }
  });
  return out;
}

static void smooth(const Mesh &mesh, MutableSpan<float> values, const int iterations)
{
  if (iterations <= 0) {
    return;
  }
  const Span<int2> mesh_edges = mesh.edges();
  Array<int> count(mesh.verts_num, 0);
  for (const int2 &edge : mesh_edges) {
    count[edge[0]]++;
    count[edge[1]]++;
  }
  Array<float> sum(mesh.verts_num);
  for (int i = 0; i < iterations; i++) {
    sum.fill(0.0f);
    for (const int2 &edge : mesh_edges) {
      sum[edge[0]] += values[edge[1]];
      sum[edge[1]] += values[edge[0]];
    }
    for (const int v : values.index_range()) {
      if (count[v]) {
        values[v] = values[v] * 0.5f + sum[v] / float(count[v]) * 0.5f;
      }
    }
  }
}

static float blend(const int mode, const float base, const float mask)
{
  switch (mode) {
    case MOD_MASKS_BLEND_ADD:
      return base + mask;
    case MOD_MASKS_BLEND_SUBTRACT:
      return base - mask;
    case MOD_MASKS_BLEND_MULTIPLY:
      return base * mask;
    case MOD_MASKS_BLEND_SCREEN:
      return 1.0f - (1.0f - base) * (1.0f - mask);
    case MOD_MASKS_BLEND_OVERLAY:
      return base < 0.5f ? 2.0f * base * mask : 1.0f - 2.0f * (1.0f - base) * (1.0f - mask);
    case MOD_MASKS_BLEND_LIGHTEN:
      return std::max(base, mask);
    case MOD_MASKS_BLEND_DARKEN:
      return std::min(base, mask);
    case MOD_MASKS_BLEND_DIFFERENCE:
      return math::abs(base - mask);
    default:
      return mask;
  }
}

/** \} */

static Mesh *modify_mesh(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  const MasksModifierData &mmd = *reinterpret_cast<const MasksModifierData *>(md);
  if (mesh->verts_num == 0) {
    return mesh;
  }

  const Mesh *target = nullptr;
  if (uses_object(mmd) && mmd.object->type == OB_MESH && mmd.object != ctx->object) {
    target = BKE_modifier_get_evaluated_mesh_from_evaluated_object(mmd.object);
    if (target != nullptr && target->corner_tris().is_empty()) {
      target = nullptr;
    }
  }

  Array<float> mask;
  switch (mmd.mask_type) {
    case MOD_MASKS_OCCLUSION:
    case MOD_MASKS_THICKNESS: {
      if (mesh->corner_tris().is_empty()) {
        return mesh;
      }
      const bke::BVHTreeFromMesh self_tree = mesh->bvh_corner_tris();
      std::optional<bke::BVHTreeFromMesh> target_tree;
      Vector<Blocker> blockers;
      blockers.append({&self_tree, float4x4::identity()});
      if (target && mmd.mask_type == MOD_MASKS_OCCLUSION) {
        target_tree.emplace(target->bvh_corner_tris());
        blockers.append(
            {&*target_tree, mmd.object->world_to_object() * ctx->object->object_to_world()});
      }
      const bool thickness = mmd.mask_type == MOD_MASKS_THICKNESS;
      mask = cast_rays(*mesh, mmd, blockers, thickness, ctx);
      if (BKE_modifier_stop_requested(ctx)) {
        return mesh;
      }
      if (!thickness) {
        /* Bright where open, dark where blocked. */
        for (float &value : mask) {
          value = 1.0f - value;
        }
      }
      break;
    }
    case MOD_MASKS_CURVATURE:
      mask = curvature(*mesh, mmd);
      break;
    case MOD_MASKS_EDGES:
      mask = edges(*mesh, mmd);
      break;
    case MOD_MASKS_DIRECTION:
      mask = direction(*mesh, mmd, *ctx->object);
      break;
    case MOD_MASKS_HEIGHT:
      mask = height(*mesh, mmd);
      break;
    case MOD_MASKS_PROXIMITY:
      if (target == nullptr) {
        BKE_modifier_set_error(ctx->object, md, "Needs another mesh object");
        return mesh;
      }
      mask = proximity(*mesh, mmd, *ctx->object, *target);
      break;
    case MOD_MASKS_PARTS:
      mask = parts(*mesh, mmd);
      break;
    case MOD_MASKS_NOISE:
      mask = noise_mask(*mesh, mmd);
      break;
    default:
      return mesh;
  }

  smooth(*mesh, mask, mmd.smooth);
  const bool invert = mmd.flag & MOD_MASKS_INVERT;
  const float low = mmd.low;
  const float range = std::max(mmd.high - mmd.low, 1e-6f);
  for (float &value : mask) {
    if (invert) {
      value = 1.0f - value;
    }
    value = std::clamp((value - low) / range, 0.0f, 1.0f);
  }

  /* What the output holds now: the attribute, else the vertex group, else black. */
  bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
  const bool has_attribute = mmd.attribute_name[0] != '\0';
  const bool has_group = mmd.vertex_group[0] != '\0';
  const int group = has_group ? BKE_defgroup_name_index(&mesh->vertex_group_names,
                                                        mmd.vertex_group) :
                                -1;
  Array<float> base(mesh->verts_num, 0.0f);
  bool base_found = false;
  if (has_attribute) {
    if (const VArray<float> old = *attributes.lookup<float>(mmd.attribute_name,
                                                            bke::AttrDomain::Point))
    {
      old.materialize(base);
      base_found = true;
    }
  }
  if (!base_found && group != -1) {
    const Span<MDeformVert> dverts = mesh->deform_verts();
    if (!dverts.is_empty()) {
      for (const int v : base.index_range()) {
        base[v] = BKE_defvert_find_weight(&dverts[v], group);
      }
    }
  }

  const float factor = mmd.factor;
  for (const int v : mask.index_range()) {
    const float mixed = std::clamp(blend(mmd.blend, base[v], mask[v]), 0.0f, 1.0f);
    mask[v] = base[v] + (mixed - base[v]) * factor;
  }

  if (has_attribute) {
    bool as_color = mmd.flag & MOD_MASKS_COLOR;
    if (const std::optional<bke::AttributeMetaData> meta = attributes.lookup_meta_data(
            mmd.attribute_name))
    {
      as_color |= ELEM(meta->data_type, bke::AttrType::ColorFloat, bke::AttrType::ColorByte);
      attributes.remove(mmd.attribute_name);
    }
    if (as_color) {
      Array<ColorGeometry4f> colors(mesh->verts_num);
      for (const int v : colors.index_range()) {
        colors[v] = ColorGeometry4f(mask[v], mask[v], mask[v], 1.0f);
      }
      attributes.add<ColorGeometry4f>(
          mmd.attribute_name,
          bke::AttrDomain::Point,
          bke::AttributeInitVArray(VArray<ColorGeometry4f>::from_container(std::move(colors))));
    }
    else {
      attributes.add<float>(
          mmd.attribute_name,
          bke::AttrDomain::Point,
          bke::AttributeInitVArray(VArray<float>::from_span(mask.as_span())));
    }
  }

  if (has_group) {
    const int index = weights::ensure_group(*mesh, mmd.vertex_group);
    MutableSpan<MDeformVert> dverts = mesh->deform_verts_for_write();
    threading::parallel_for(dverts.index_range(), 1024, [&](const IndexRange range) {
      for (const int v : range) {
        MDeformVert &dv = dverts[v];
        if (mask[v] > 0.0f) {
          BKE_defvert_ensure_index(&dv, index)->weight = mask[v];
        }
        else if (MDeformWeight *dw = BKE_defvert_find_index(&dv, index)) {
          BKE_defvert_remove_group(&dv, dw);
        }
      }
    });
  }
  return mesh;
}

static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);
  const int type = RNA_enum_get(ptr, "mask_type");

  layout.use_property_split_set(true);
  layout.prop(ptr, "mask_type", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  switch (type) {
    case MOD_MASKS_OCCLUSION:
    case MOD_MASKS_THICKNESS:
      layout.prop(ptr, "rays", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      layout.prop(ptr, "distance", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      layout.prop(ptr, "spread", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      if (type == MOD_MASKS_OCCLUSION) {
        layout.prop(ptr, "object", UI_ITEM_NONE, IFACE_("Also Blocked By"), ICON_NONE);
      }
      layout.prop(ptr, "seed", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;
    case MOD_MASKS_CURVATURE:
      layout.prop(ptr, "curvature", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      layout.prop(ptr, "scale", UI_ITEM_NONE, IFACE_("Strength"), ICON_NONE);
      break;
    case MOD_MASKS_EDGES:
      layout.prop(ptr, "angle", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      layout.prop(ptr, "distance", UI_ITEM_NONE, IFACE_("Width"), ICON_NONE);
      layout.prop(ptr, "use_borders", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;
    case MOD_MASKS_DIRECTION:
      layout.prop(ptr, "direction", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      layout.prop(ptr, "coverage", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      layout.prop(ptr, "softness", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;
    case MOD_MASKS_HEIGHT:
      layout.prop(ptr, "axis", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
      break;
    case MOD_MASKS_PROXIMITY:
      layout.prop(ptr, "object", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      layout.prop(ptr, "distance", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;
    case MOD_MASKS_PARTS:
      layout.prop(ptr, "seed", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;
    case MOD_MASKS_NOISE:
      layout.prop(ptr, "scale", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      layout.prop(ptr, "detail", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      layout.prop(ptr, "seed", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;
    default:
      break;
  }

  layout.separator();
  layout.prop(ptr, "smooth", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  ui::Layout &col = layout.column(true);
  col.prop(ptr, "low", UI_ITEM_NONE, IFACE_("Levels Low"), ICON_NONE);
  col.prop(ptr, "high", UI_ITEM_NONE, IFACE_("High"), ICON_NONE);
  layout.prop(ptr, "invert", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  layout.separator();
  layout.prop(ptr, "blend", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "factor", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "attribute_name", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "use_color", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop_search(
      ptr, "vertex_group", &ob_ptr, "vertex_groups", std::nullopt, ICON_GROUP_VERTEX);
  modifier_error_message_draw(layout, ptr);
}

static void panel_register(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_Masks, panel_draw);
}

ModifierTypeInfo modifierType_Masks = {
    /*idname*/ "Masks",
    /*name*/ N_("Masks"),
    /*struct_name*/ "MasksModifierData",
    /*struct_size*/ sizeof(MasksModifierData),
    /*srna*/ &RNA_MasksModifier,
    /*type*/ ModifierTypeType::Nonconstructive,
    /*flags*/ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsEditmode |
        eModifierTypeFlag_EnableInEditmode,
    /*icon*/ ICON_MOD_MASKS,

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
