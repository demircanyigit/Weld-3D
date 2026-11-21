/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Snow: lays a soft layer of snow on the parts of the mesh that face up and are open to the sky.
 *
 * Points are scattered over the faces that hold snow. Each gets a depth from the slope, the mask,
 * how much sky it sees and some drift noise, and becomes a round blob as wide as the overhang
 * asks and as high as its depth. The blobs are melted into one soft volume, which is turned back
 * into a mesh and added to the object with its own material.
 */

#include <algorithm>

#include "BLI_array.hh"
#include "BLI_math_base.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_noise.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "DNA_collection_types.h"
#include "DNA_layer_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "BKE_attribute.hh"
#include "BKE_bvhutils.hh"
#include "BKE_collection.hh"
#include "BKE_lib_id.hh"
#include "BKE_geometry_set.hh"
#include "BKE_lib_query.hh"
#include "BKE_mesh.hh"
#include "BKE_modifier.hh"
#include "BKE_volume_to_mesh.hh"

#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"

#include "GEO_join_geometries.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "MOD_modifiertypes.hh"
#include "MOD_ui_common.hh"
#include "MOD_util.hh"

#ifdef WITH_OPENVDB
#  include <openvdb/openvdb.h>
#  include <openvdb/tools/LevelSetFilter.h>
#  include <openvdb/tools/ParticlesToLevelSet.h>
#endif

namespace blender {

static void init_data(ModifierData *md)
{
  SnowModifierData *smd = reinterpret_cast<SnowModifierData *>(md);
  INIT_DEFAULT_STRUCT_AFTER(smd, modifier);
}

static void required_data_mask(ModifierData *md, CustomData_MeshMasks *r_cddata_masks)
{
  const SnowModifierData &smd = *reinterpret_cast<SnowModifierData *>(md);
  if (smd.mask_name[0] != '\0') {
    r_cddata_masks->vmask |= CD_MASK_MDEFORMVERT;
  }
}

static void foreach_ID_link(ModifierData *md, Object *ob, IDWalkFunc walk, void *user_data)
{
  SnowModifierData *smd = reinterpret_cast<SnowModifierData *>(md);
  walk(user_data, ob, reinterpret_cast<ID **>(&smd->blockers), IDWALK_CB_USER);
  SnowModifierData *pick_data = reinterpret_cast<SnowModifierData *>(md);
  walk(user_data, ob, reinterpret_cast<ID **>(&pick_data->material_pick), IDWALK_CB_USER);
}

/** The mesh objects of the blocking collection, without the object itself. */
static Vector<Object *> blocker_objects(const SnowModifierData &smd, Object *owner)
{
  Vector<Object *> objects;
  if (smd.blockers == nullptr) {
    return objects;
  }
  Collection *collection = DEG_get_original(smd.blockers);
  Object *owner_orig = DEG_get_original(owner);
  FOREACH_COLLECTION_OBJECT_RECURSIVE_BEGIN (collection, object) {
    if (object != owner_orig && object->type == OB_MESH && object->data != nullptr) {
      objects.append_non_duplicates(object);
    }
  }
  FOREACH_COLLECTION_OBJECT_RECURSIVE_END;
  return objects;
}

static void update_depsgraph(ModifierData *md, const ModifierUpdateDepsgraphContext *ctx)
{
  SnowModifierData *smd = reinterpret_cast<SnowModifierData *>(md);
  /* Blockers are read before their modifiers, so snow on two objects that block each other does
   * not make a loop. */
  for (Object *object : blocker_objects(*smd, ctx->object)) {
    DEG_add_object_relation(ctx->node, object, DEG_OB_COMP_TRANSFORM, "Snow blocker");
    DEG_add_generic_id_relation(ctx->node, static_cast<ID *>(object->data), "Snow blocker");
  }
  DEG_add_depends_on_transform_relation(ctx->node, "Snow");
}

#ifdef WITH_OPENVDB

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

/** A surface that can keep snow off what is under it, in its own space. */
struct Blocker {
  const bke::BVHTreeFromMesh *tree;
  float4x4 to_local;
};

static bool blocked(const Span<Blocker> blockers, const float3 &origin, const float3 &dir)
{
  for (const Blocker &blocker : blockers) {
    const float3 o = math::transform_point(blocker.to_local, origin);
    float3 d = math::transform_direction(blocker.to_local, dir);
    const float len = math::length(d);
    if (len < 1e-12f) {
      continue;
    }
    d /= len;
    BVHTreeRayHit hit;
    hit.index = -1;
    hit.dist = FLT_MAX;
    if (BLI_bvhtree_ray_cast(blocker.tree->tree,
                             o,
                             d,
                             0.0f,
                             &hit,
                             blocker.tree->raycast_callback,
                             (void *)blocker.tree) != -1)
    {
      return true;
    }
  }
  return false;
}

/** Blobs for #openvdb::tools::ParticlesToLevelSet, in the grid's own space. */
class Blobs {
 public:
  using PosType = openvdb::Vec3R;
  Span<float3> centers;
  Span<float> radii;

  size_t size() const
  {
    return size_t(centers.size());
  }
  void getPos(const size_t n, openvdb::Vec3R &xyz) const
  {
    xyz = openvdb::Vec3R(centers[n].x, centers[n].y, centers[n].z);
  }
  void getPosRad(const size_t n, openvdb::Vec3R &xyz, openvdb::Real &radius) const
  {
    this->getPos(n, xyz);
    radius = radii[n];
  }
};

/** A point on a face that holds snow. */
struct Sample {
  float3 position;
  float3 normal;
  float mask;
  float slope;
};

static Mesh *make_snow(const SnowModifierData &smd,
                       const ModifierEvalContext *ctx,
                       ModifierData *md,
                       const Mesh &mesh,
                       const int material)
{
  /* Esc stops it between steps, the modifier stack then turns it off. */
  const auto stop = [&]() { return BKE_modifier_stop_requested(ctx); };
  const float voxel = std::max(smd.voxel_size, 5e-4f);
  const float thickness = smd.thickness;
  if (thickness <= 0.0f || mesh.faces_num == 0) {
    return nullptr;
  }

  /* A frame with "up" against the snowfall. The grid is squashed along up so a round blob in the
   * grid is as wide as the overhang asks and as high as the snow is deep. */
  float3 up = -float3(smd.direction);
  up = math::length(up) > 1e-6f ? math::normalize(up) : float3(0.0f, 0.0f, 1.0f);
  float3 t1, t2_unused;
  ortho_basis_v3v3_v3(t1, t2_unused, up);
  t1 = math::normalize(t1);
  const float3 t2 = math::cross(up, t1);
  const float wide = std::max(smd.overhang, 0.05f);
  auto to_grid = [&](const float3 &p) {
    return float3(math::dot(p, t1), math::dot(p, t2), math::dot(p, up) * wide);
  };
  auto from_grid = [&](const float3 &q) { return t1 * q.x + t2 * q.y + up * (q.z / wide); };

  const float4x4 to_world = ctx->object->object_to_world();
  const float4x4 world_to_object = ctx->object->world_to_object();
  const Span<float3> positions = mesh.vert_positions();
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<int3> tris = mesh.corner_tris();

  VArray<float> mask;
  if (smd.mask_name[0] != '\0') {
    mask = *mesh.attributes().lookup<float>(smd.mask_name, bke::AttrDomain::Point);
  }

  /* Faces that can hold snow, with how much their slope lets them hold. */
  const float cos_slope = math::cos(smd.max_slope);
  const float fade = std::max((1.0f - cos_slope) * 0.25f, 1e-4f);
  Array<float> tri_slope(tris.size(), 0.0f);
  Array<float> tri_area(tris.size(), 0.0f);
  threading::parallel_for(tris.index_range(), 2048, [&](const IndexRange range) {
    for (const int t : range) {
      const float3 a = math::transform_point(to_world, positions[corner_verts[tris[t][0]]]);
      const float3 b = math::transform_point(to_world, positions[corner_verts[tris[t][1]]]);
      const float3 c = math::transform_point(to_world, positions[corner_verts[tris[t][2]]]);
      const float3 cross = math::cross(b - a, c - a);
      const float len = math::length(cross);
      if (len < 1e-12f) {
        continue;
      }
      const float facing = math::dot(cross / len, up);
      tri_slope[t] = smoothstep((facing - cos_slope) / fade);
      if (tri_slope[t] > 0.0f) {
        tri_area[t] = len * 0.5f;
      }
    }
  });

  /* Spacing of the samples: close enough for the blobs to melt together, but not too many. */
  double total_area = 0.0;
  for (const float area : tri_area) {
    total_area += area;
  }
  if (total_area <= 0.0) {
    return nullptr;
  }
  constexpr double max_samples = 400000.0;
  float spacing = std::max(voxel, thickness * wide * 0.4f);
  if (total_area / (double(spacing) * spacing) > max_samples) {
    spacing = float(std::sqrt(total_area / max_samples));
  }
  const float inv_cell = 1.0f / (spacing * spacing);

  Vector<Sample> samples;
  for (const int t : tris.index_range()) {
    if (tri_area[t] <= 0.0f) {
      continue;
    }
    const float exact = tri_area[t] * inv_cell;
    const int count = int(exact + hash_float(uint32_t(t), 0x51ed27u));
    if (count == 0) {
      continue;
    }
    const int3 v(corner_verts[tris[t][0]], corner_verts[tris[t][1]], corner_verts[tris[t][2]]);
    const float3 a = math::transform_point(to_world, positions[v[0]]);
    const float3 b = math::transform_point(to_world, positions[v[1]]);
    const float3 c = math::transform_point(to_world, positions[v[2]]);
    const float3 normal = math::normalize(math::cross(b - a, c - a));
    for (int i = 0; i < count; i++) {
      float u = hash_float(uint32_t(t), uint32_t(i * 2 + 1));
      float w = hash_float(uint32_t(t), uint32_t(i * 2 + 2));
      if (u + w > 1.0f) {
        u = 1.0f - u;
        w = 1.0f - w;
      }
      Sample s;
      s.position = a + (b - a) * u + (c - a) * w;
      s.normal = normal;
      s.slope = tri_slope[t];
      s.mask = 1.0f;
      if (mask) {
        s.mask = std::max(
            0.0f, mask[v[0]] * (1.0f - u - w) + mask[v[1]] * u + mask[v[2]] * w);
      }
      samples.append(s);
    }
  }

  /* What can keep snow off: the mesh itself and the blocking objects, before their modifiers. */
  const std::optional<Bounds<float3>> bounds = mesh.bounds_min_max();
  const float size = bounds ? math::distance(bounds->min, bounds->max) : 1.0f;
  const float eps = std::max(size * 1e-4f, 1e-5f);
  const bke::BVHTreeFromMesh self_tree = mesh.bvh_corner_tris();
  Vector<Blocker> blockers;
  blockers.append({&self_tree, world_to_object});
  Vector<std::unique_ptr<bke::BVHTreeFromMesh>> blocker_trees;
  if (smd.sky_rays > 0) {
    for (Object *object : blocker_objects(smd, ctx->object)) {
      Object *evaluated = DEG_get_evaluated(ctx->depsgraph, object);
      const Mesh *blocker_mesh = DEG_get_evaluated(ctx->depsgraph,
                                                   reinterpret_cast<Mesh *>(object->data));
      if (evaluated == nullptr || blocker_mesh == nullptr || blocker_mesh->faces_num == 0) {
        continue;
      }
      blocker_trees.append(
          std::make_unique<bke::BVHTreeFromMesh>(blocker_mesh->bvh_corner_tris()));
      blockers.append({blocker_trees.last().get(), evaluated->world_to_object()});
    }
  }

  const int rays = std::max(smd.sky_rays, 0);
  const float cone = math::sin(float(M_PI) / 5.0f);
  const float golden = float(M_PI) * (3.0f - math::sqrt(5.0f));
  const float3 noise_offset(hash_float(uint32_t(smd.seed), 1u) * 1000.0f,
                            hash_float(uint32_t(smd.seed), 2u) * 1000.0f,
                            hash_float(uint32_t(smd.seed), 3u) * 1000.0f);

  /* The blobs, in the grid's space. */
  Array<float3> centers(samples.size());
  Array<float> radii(samples.size(), 0.0f);
  const float embed = voxel * 0.75f / wide;
  threading::parallel_for(samples.index_range(), 64, [&](const IndexRange range) {
    if (stop()) {
      return;
    }
    for (const int i : range) {
      const Sample &s = samples[i];
      float depth = s.slope * s.mask;
      if (depth <= 0.0f) {
        continue;
      }
      if (rays > 0) {
        const float3 origin = s.position + s.normal * eps;
        const float turn = hash_float(uint32_t(i), 0x9e37u) * float(M_PI * 2.0);
        int open = 0, cast = 0;
        for (int r = 0; r < rays; r++) {
          const float rad = math::sqrt((float(r) + 0.5f) / float(rays)) * cone;
          const float phi = float(r) * golden + turn;
          const float z = math::sqrt(std::max(0.0f, 1.0f - rad * rad));
          const float3 dir = t1 * (rad * math::cos(phi)) + t2 * (rad * math::sin(phi)) + up * z;
          if (math::dot(dir, s.normal) <= 0.0f) {
            continue;
          }
          cast++;
          if (!blocked(blockers, origin, dir)) {
            open++;
          }
        }
        depth *= cast ? float(open) / float(cast) : 1.0f;
      }
      if (smd.drift > 0.0f) {
        const float n = noise::perlin_fbm<float3>(
            s.position * smd.drift_scale + noise_offset, 2.0f, 0.5f, 2.0f, true);
        depth *= std::max(0.0f, 1.0f + smd.drift * 2.0f * (n - 0.5f));
      }
      const float height = thickness * depth;
      if (height < voxel * 0.5f) {
        continue;
      }
      centers[i] = to_grid(s.position + up * (height - embed));
      radii[i] = height * wide;
    }
  });

  if (stop()) {
    return nullptr;
  }
  Vector<float3> kept_centers;
  Vector<float> kept_radii;
  float3 lo(FLT_MAX), hi(-FLT_MAX);
  for (const int i : radii.index_range()) {
    if (radii[i] > 0.0f) {
      kept_centers.append(centers[i]);
      kept_radii.append(radii[i]);
      lo = math::min(lo, centers[i] - radii[i]);
      hi = math::max(hi, centers[i] + radii[i]);
    }
  }
  if (kept_centers.is_empty()) {
    return nullptr;
  }
  const float3 extent = (hi - lo) / voxel;
  if (std::max({extent.x, extent.y, extent.z}) > 4000.0f) {
    BKE_modifier_set_error(ctx->object, md, "Voxel Size is too small for this much snow");
    return nullptr;
  }

  /* Melt the blobs into one volume, then fill the dips between them and soften it. */
  openvdb::FloatGrid::Ptr grid = openvdb::FloatGrid::create(3.0f * voxel);
  grid->setTransform(openvdb::math::Transform::createLinearTransform(voxel));
  grid->setGridClass(openvdb::GRID_LEVEL_SET);
  try {
    openvdb::tools::ParticlesToLevelSet<openvdb::FloatGrid> raster(*grid);
    raster.setRmin(0.5f);
    raster.setRmax(std::numeric_limits<float>::max());
    Blobs blobs;
    blobs.centers = kept_centers;
    blobs.radii = kept_radii;
    raster.rasterizeSpheres(blobs);
    raster.finalize(true);
    if (stop()) {
      return nullptr;
    }

    openvdb::tools::LevelSetFilter<openvdb::FloatGrid> filter(*grid);
    const float close = spacing * (1.0f + 2.0f * smd.smooth);
    filter.offset(-close);
    filter.offset(close);
    const int iterations = int(smd.smooth * 6.0f + 0.5f);
    for (int i = 0; i < iterations && !stop(); i++) {
      filter.meanCurvature();
    }
  }
  catch (const std::exception &error) {
    BKE_modifier_set_error(ctx->object, md, "Snow: %s", error.what());
    return nullptr;
  }

  if (stop()) {
    return nullptr;
  }
  const bool faceted = smd.flag & MOD_SNOW_FACETED;
  Mesh *snow = bke::volume_grid_to_mesh(*grid, 0.0f, faceted ? 0.5f : 0.0f);
  if (snow == nullptr || snow->verts_num == 0) {
    return snow;
  }

  /* Back to the object's space, with how deep the snow is above the surface at each point. */
  MutableSpan<float3> snow_positions = snow->vert_positions_for_write();
  Array<float> depth(snow->verts_num, 0.0f);
  threading::parallel_for(snow_positions.index_range(), 512, [&](const IndexRange range) {
    for (const int v : range) {
      const float3 world = from_grid(snow_positions[v]);
      const float3 local = math::transform_point(world_to_object, world);
      snow_positions[v] = local;
      BVHTreeNearest nearest;
      nearest.index = -1;
      nearest.dist_sq = FLT_MAX;
      BLI_bvhtree_find_nearest(
          self_tree.tree, local, &nearest, self_tree.nearest_callback, (void *)&self_tree);
      if (nearest.index != -1) {
        depth[v] = math::distance(world, math::transform_point(to_world, float3(nearest.co)));
      }
    }
  });
  snow->tag_positions_changed();
  bke::mesh_smooth_set(*snow, !faceted);

  bke::MutableAttributeAccessor attributes = snow->attributes_for_write();
  attributes.add<float>("snow_depth",
                        bke::AttrDomain::Point,
                        bke::AttributeInitVArray(VArray<float>::from_span(depth.as_span())));
  attributes.add<int>("material_index",
                      bke::AttrDomain::Face,
                      bke::AttributeInitVArray(
                          VArray<int>::from_single(material, snow->faces_num)));
  return snow;
}

#endif /* WITH_OPENVDB */

static Mesh *modify_mesh(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
#ifdef WITH_OPENVDB
  const SnowModifierData &smd = *reinterpret_cast<const SnowModifierData *>(md);
  const int material = MOD_material_slot(*mesh, smd.material_pick, smd.material);
  Mesh *snow = make_snow(smd, ctx, md, *mesh, material);
  if (BKE_modifier_stop_requested(ctx)) {
    /* Stopped with Esc: leave the mesh as it was. */
    if (snow) {
      BKE_id_free(nullptr, snow);
    }
    return mesh;
  }
  const bool only = smd.flag & MOD_SNOW_ONLY;
  if (snow == nullptr) {
    return only ? BKE_mesh_new_nomain(0, 0, 0, 0) : mesh;
  }
  if (only) {
    BKE_mesh_copy_parameters_for_eval(snow, mesh);
    return snow;
  }
  /* The snow's material numbers are slots of this mesh, the join needs them to be there. */
  BKE_mesh_copy_parameters_for_eval(snow, mesh);
  Vector<bke::GeometrySet> pieces;
  pieces.append(bke::GeometrySet::from_mesh(mesh, bke::GeometryOwnershipType::ReadOnly));
  pieces.append(bke::GeometrySet::from_mesh(snow));
  bke::GeometrySet joined = geometry::join_geometries(pieces, {});
  if (!joined.has_mesh()) {
    return mesh;
  }
  Mesh *result = joined.get_component_for_write<bke::MeshComponent>().release();
  BKE_mesh_copy_parameters_for_eval(result, mesh);
  return result;
#else
  BKE_modifier_set_error(ctx->object, md, "Built without OpenVDB");
  return mesh;
#endif
}

static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);

  layout.use_property_split_set(true);
  layout.prop(ptr, "thickness", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "max_slope", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "overhang", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "direction", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  layout.separator();
  layout.prop(ptr, "drift", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "drift_scale", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "seed", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  layout.separator();
  layout.prop(ptr, "sky_rays", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "blocked_by", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "mask", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  layout.separator();
  layout.prop(ptr, "voxel_size", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "smooth", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "use_faceted", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  modifier_material_ui(layout, ptr, "material_pick", "material", std::nullopt);
  layout.prop(ptr, "use_snow_only", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  modifier_error_message_draw(layout, ptr);
}

static void panel_register(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_Snow, panel_draw);
}

ModifierTypeInfo modifierType_Snow = {
    /*idname*/ "Snow",
    /*name*/ N_("Snow"),
    /*struct_name*/ "SnowModifierData",
    /*struct_size*/ sizeof(SnowModifierData),
    /*srna*/ &RNA_SnowModifier,
    /*type*/ ModifierTypeType::Constructive,
    /*flags*/ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsEditmode,
    /*icon*/ ICON_MOD_SNOW,

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
