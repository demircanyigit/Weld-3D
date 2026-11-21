/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 */

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>

#include "BLI_bitmap.h"
#include "BLI_hash.h"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_memory_cache.hh"
#include "BLI_memory_counter.hh"

#include "BLI_listbase_iterator.hh"

#include "DNA_collection_types.h"
#include "DNA_layer_types.h"
#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_texture_types.h"

#include "BKE_action.hh" /* BKE_pose_channel_find_name */
#include "BKE_attribute.hh"
#include "BKE_collection.hh"
#include "BKE_customdata.hh"
#include "BKE_deform.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_wrapper.hh"
#include "BKE_geometry_set.hh"
#include "BKE_geometry_set_instances.hh"
#include "BKE_lattice.hh"

#include "BKE_modifier.hh"

#include "DEG_depsgraph_query.hh"

#include "GEO_mesh_sim_cache.hh"
#include "GEO_realize_instances.hh"

#include "MOD_modifiertypes.hh"
#include "MOD_util.hh"

#include "MEM_guardedalloc.h"

namespace blender {

/** The objects in the order they are picked, each once, without \a self. */
static Vector<Object *> array_source_objects(const int source,
                                             Collection *collection,
                                             const ListBaseT<ArraySourceObject> &objects,
                                             const Object *self)
{
  Vector<Object *> result;
  auto add = [&](Object *ob) {
    if (ob == nullptr || ob == self || result.contains(ob)) {
      return;
    }
    /* Meshes, and objects that give a mesh once their instances are realized. */
    const bool instances_collection = ob->type == OB_EMPTY && ob->instance_collection != nullptr;
    if (ELEM(ob->type, OB_MESH, OB_CURVES_LEGACY, OB_FONT, OB_SURF) || instances_collection) {
      result.append(ob);
    }
  };
  if (source == MOD_ARRAY_SOURCE_COLLECTION && collection != nullptr) {
    FOREACH_COLLECTION_OBJECT_RECURSIVE_BEGIN (collection, ob) {
      add(ob);
    }
    FOREACH_COLLECTION_OBJECT_RECURSIVE_END;
  }
  else if (source == MOD_ARRAY_SOURCE_OBJECTS) {
    for (const ArraySourceObject &item : objects) {
      add(item.object);
    }
  }
  return result;
}

Vector<const Mesh *> MOD_array_source_meshes(const int source,
                                             Collection *collection,
                                             const ListBaseT<ArraySourceObject> &objects,
                                             const Object *self,
                                             Vector<bke::GeometrySet> &r_keep)
{
  Vector<const Mesh *> meshes;
  for (Object *ob : array_source_objects(source, collection, objects, self)) {
    /* Everything the object gives, its instances realized into one mesh. */
    bke::GeometrySet geometry = bke::object_get_evaluated_geometry_set(*ob);
    if (geometry.has_instances()) {
      geometry::RealizeInstancesOptions options;
      options.keep_original_ids = false;
      options.realize_instance_attributes = false;
      geometry = geometry::realize_instances(geometry, options).geometry;
    }
    const Mesh *mesh = geometry.get_mesh();
    if (mesh == nullptr || mesh->verts_num == 0) {
      continue;
    }
    meshes.append(mesh);
    r_keep.append(std::move(geometry));
  }
  return meshes;
}

void MOD_array_source_update_depsgraph(const ModifierUpdateDepsgraphContext *ctx,
                                       const int source,
                                       Collection *collection,
                                       const ListBaseT<ArraySourceObject> &objects,
                                       const char *description)
{
  if (source == MOD_ARRAY_SOURCE_COLLECTION && collection != nullptr) {
    DEG_add_collection_geometry_relation(ctx->node, collection, description);
  }
  for (Object *ob : array_source_objects(source, collection, objects, ctx->object)) {
    if (ob->type == OB_EMPTY) {
      DEG_add_object_relation(ctx->node, ob, DEG_OB_COMP_TRANSFORM, description);
      DEG_add_collection_geometry_relation(ctx->node, ob->instance_collection, description);
    }
    else {
      DEG_add_object_relation(ctx->node, ob, DEG_OB_COMP_GEOMETRY, description);
    }
  }
}

Array<int> MOD_array_source_pick(const int pick,
                                 const int seed,
                                 const int copies_num,
                                 const int meshes_num)
{
  Array<int> result(std::max(copies_num, 0), -1);
  if (meshes_num <= 0) {
    return result;
  }
  for (const int i : result.index_range()) {
    switch (pick) {
      case MOD_ARRAY_PICK_IN_ORDER:
        result[i] = i % meshes_num;
        break;
      case MOD_ARRAY_PICK_ALTERNATE:
        result[i] = (i % 2 == 0) ? -1 : (i / 2) % meshes_num;
        break;
      case MOD_ARRAY_PICK_RANDOM:
      default:
        /* Hashed per copy, so more copies keep the picks of the ones before. */
        result[i] = int(BLI_hash_int_2d(uint(i), uint(seed)) % uint(meshes_num));
        break;
    }
  }
  return result;
}

void MOD_init_texture(MappingInfoModifierData *dmd, const ModifierEvalContext *ctx)
{
  Tex *tex = dmd->texture;

  if (tex == nullptr) {
    return;
  }

  if (tex->ima && BKE_image_is_animated(tex->ima)) {
    BKE_image_user_frame_calc(tex->ima, &tex->iuser, DEG_get_ctime(ctx->depsgraph));
  }
}

void MOD_get_texture_coords(MappingInfoModifierData *dmd,
                            const ModifierEvalContext * /*ctx*/,
                            Object *ob,
                            Mesh *mesh,
                            float (*cos)[3],
                            float (*r_texco)[3])
{
  /* TODO: to be renamed to `get_texture_coords` once we are done with moving modifiers to Mesh. */

  const int verts_num = mesh->verts_num;
  int i;
  int texmapping = dmd->texmapping;
  float mapref_imat[4][4];

  if (texmapping == MOD_DISP_MAP_OBJECT) {
    if (dmd->map_object != nullptr) {
      Object *map_object = dmd->map_object;
      if (dmd->map_bone[0] != '\0') {
        bPoseChannel *pchan = BKE_pose_channel_find_name(map_object->pose, dmd->map_bone);
        if (pchan) {
          float mat_bone_world[4][4];
          mul_m4_m4m4(mat_bone_world, map_object->object_to_world().ptr(), pchan->pose_mat);
          invert_m4_m4(mapref_imat, mat_bone_world);
        }
        else {
          invert_m4_m4(mapref_imat, map_object->object_to_world().ptr());
        }
      }
      else {
        invert_m4_m4(mapref_imat, map_object->object_to_world().ptr());
      }
    }
    else { /* if there is no map object, default to local */
      texmapping = MOD_DISP_MAP_LOCAL;
    }
  }

  /* UVs need special handling, since they come from faces */
  if (texmapping == MOD_DISP_MAP_UV) {
    const VectorSet<StringRefNull> uv_map_names = mesh->uv_map_names();
    if (!uv_map_names.is_empty()) {
      const OffsetIndices faces = mesh->faces();
      const Span<int> corner_verts = mesh->corner_verts();
      BLI_bitmap *done = BLI_BITMAP_NEW(verts_num, __func__);
      const StringRef uvname = uv_map_names.contains(dmd->uvlayer_name) ?
                                   dmd->uvlayer_name :
                                   mesh->active_uv_map_name();
      const bke::AttributeAccessor attributes = mesh->attributes();
      const VArraySpan uv_map = *attributes.lookup_or_default<float2>(
          uvname, bke::AttrDomain::Corner, float2(0));

      /* verts are given the UV from the first face that uses them */
      for (const int i : faces.index_range()) {
        const IndexRange face = faces[i];
        for (const int corner : face) {
          const int vert = corner_verts[corner];
          if (!BLI_BITMAP_TEST(done, vert)) {
            /* remap UVs from [0, 1] to [-1, 1] */
            r_texco[vert][0] = (uv_map[corner][0] * 2.0f) - 1.0f;
            r_texco[vert][1] = (uv_map[corner][1] * 2.0f) - 1.0f;
            BLI_BITMAP_ENABLE(done, vert);
          }
        }
      }

      MEM_delete(done);
      return;
    }

    /* if there are no UVs, default to local */
    texmapping = MOD_DISP_MAP_LOCAL;
  }

  const Span<float3> positions = mesh->vert_positions();
  for (i = 0; i < verts_num; i++, r_texco++) {
    switch (texmapping) {
      case MOD_DISP_MAP_LOCAL:
        copy_v3_v3(*r_texco, cos != nullptr ? *cos : positions[i]);
        break;
      case MOD_DISP_MAP_GLOBAL:
        mul_v3_m4v3(*r_texco, ob->object_to_world().ptr(), cos != nullptr ? *cos : positions[i]);
        break;
      case MOD_DISP_MAP_OBJECT:
        mul_v3_m4v3(*r_texco, ob->object_to_world().ptr(), cos != nullptr ? *cos : positions[i]);
        mul_m4_v3(mapref_imat, *r_texco);
        break;
    }
    if (cos != nullptr) {
      cos++;
    }
  }
}

void MOD_previous_vcos_store(ModifierData *md, const float (*vert_coords)[3])
{
  while ((md = md->next) && md->type == eModifierType_Armature) {
    ArmatureModifierData *amd = reinterpret_cast<ArmatureModifierData *>(md);
    if (amd->multi && amd->vert_coords_prev == nullptr) {
      amd->vert_coords_prev = MEM_dupalloc(vert_coords);
    }
    else {
      break;
    }
  }
  /* lattice/mesh modifier too */
}

void MOD_get_vgroup(const Object *ob,
                    const Mesh *mesh,
                    const char *name,
                    const MDeformVert **dvert,
                    int *defgrp_index)
{
  if (mesh) {
    *defgrp_index = BKE_id_defgroup_name_index(&mesh->id, name);
    if (*defgrp_index != -1) {
      *dvert = mesh->deform_verts().data();
    }
    else {
      *dvert = nullptr;
    }
  }
  else if (OB_TYPE_SUPPORT_VGROUP(ob->type)) {
    *defgrp_index = BKE_object_defgroup_name_index(ob, name);
    if (*defgrp_index != -1 && ob->type == OB_LATTICE) {
      *dvert = BKE_lattice_deform_verts_get(ob);
    }
    else {
      *dvert = nullptr;
    }
  }
  else {
    *defgrp_index = -1;
    *dvert = nullptr;
  }
}

void MOD_depsgraph_update_object_bone_relation(DepsNodeHandle *node,
                                               Object *object,
                                               const char *bonename,
                                               const char *description)
{
  if (object == nullptr) {
    return;
  }
  if (bonename[0] != '\0' && object->type == OB_ARMATURE) {
    DEG_add_object_relation(node, object, DEG_OB_COMP_EVAL_POSE, description);
  }
  else {
    DEG_add_object_relation(node, object, DEG_OB_COMP_TRANSFORM, description);
  }
}

void modifier_type_init(ModifierTypeInfo *types[])
{
#define INIT_TYPE(typeName) (types[eModifierType_##typeName] = &modifierType_##typeName)
  INIT_TYPE(None);
  INIT_TYPE(Curve);
  INIT_TYPE(Lattice);
  INIT_TYPE(Subsurf);
  INIT_TYPE(Build);
  INIT_TYPE(Array);
  INIT_TYPE(Mirror);
  INIT_TYPE(EdgeSplit);
  INIT_TYPE(Bevel);
  INIT_TYPE(Displace);
  INIT_TYPE(UVProject);
  INIT_TYPE(Decimate);
  INIT_TYPE(Smooth);
  INIT_TYPE(Cast);
  INIT_TYPE(Wave);
  INIT_TYPE(Armature);
  INIT_TYPE(Hook);
  INIT_TYPE(Softbody);
  INIT_TYPE(Cloth);
  INIT_TYPE(Collision);
  INIT_TYPE(Boolean);
  INIT_TYPE(MeshDeform);
  INIT_TYPE(Ocean);
  INIT_TYPE(ParticleSystem);
  INIT_TYPE(ParticleInstance);
  INIT_TYPE(Explode);
  INIT_TYPE(Shrinkwrap);
  INIT_TYPE(Mask);
  INIT_TYPE(SimpleDeform);
  INIT_TYPE(Multires);
  INIT_TYPE(Surface);
  INIT_TYPE(Fluid);
  INIT_TYPE(ShapeKey);
  INIT_TYPE(Solidify);
  INIT_TYPE(Screw);
  INIT_TYPE(Warp);
  INIT_TYPE(WeightVGEdit);
  INIT_TYPE(WeightVGMix);
  INIT_TYPE(WeightVGProximity);
  INIT_TYPE(DynamicPaint);
  INIT_TYPE(Remesh);
  INIT_TYPE(Skin);
  INIT_TYPE(LaplacianSmooth);
  INIT_TYPE(Triangulate);
  INIT_TYPE(UVWarp);
  INIT_TYPE(MeshCache);
  INIT_TYPE(LaplacianDeform);
  INIT_TYPE(Wireframe);
  INIT_TYPE(Weld);
  INIT_TYPE(DataTransfer);
  INIT_TYPE(NormalEdit);
  INIT_TYPE(CorrectiveSmooth);
  INIT_TYPE(MeshSequenceCache);
  INIT_TYPE(SurfaceDeform);
  INIT_TYPE(WeightedNormal);
  INIT_TYPE(MeshToVolume);
  INIT_TYPE(VolumeDisplace);
  INIT_TYPE(VolumeToMesh);
  INIT_TYPE(Nodes);
  INIT_TYPE(GreasePencilOpacity);
  INIT_TYPE(GreasePencilSubdiv);
  INIT_TYPE(GreasePencilColor);
  INIT_TYPE(GreasePencilTint);
  INIT_TYPE(GreasePencilSmooth);
  INIT_TYPE(GreasePencilOffset);
  INIT_TYPE(GreasePencilNoise);
  INIT_TYPE(GreasePencilMirror);
  INIT_TYPE(GreasePencilThickness);
  INIT_TYPE(GreasePencilLattice);
  INIT_TYPE(GreasePencilDash);
  INIT_TYPE(GreasePencilMultiply);
  INIT_TYPE(GreasePencilLength);
  INIT_TYPE(GreasePencilWeightAngle);
  INIT_TYPE(GreasePencilArray);
  INIT_TYPE(GreasePencilWeightProximity);
  INIT_TYPE(GreasePencilHook);
  INIT_TYPE(GreasePencilLineart);
  INIT_TYPE(GreasePencilArmature);
  INIT_TYPE(GreasePencilTime);
  INIT_TYPE(GreasePencilSimplify);
  INIT_TYPE(GreasePencilEnvelope);
  INIT_TYPE(GreasePencilOutline);
  INIT_TYPE(GreasePencilShrinkwrap);
  INIT_TYPE(GreasePencilBuild);
  INIT_TYPE(GreasePencilTexture);
  INIT_TYPE(IsotropicRemesh);
  INIT_TYPE(QuadriflowRemesh);
  INIT_TYPE(Slice);
  INIT_TYPE(CapHoles);
  INIT_TYPE(Quadrangulate);
  INIT_TYPE(RandomizeElements);
  INIT_TYPE(MarkByAngle);
  INIT_TYPE(ExplodedView);
  INIT_TYPE(Noise);
  INIT_TYPE(FFD2);
  INIT_TYPE(FFD3);
  INIT_TYPE(FFD4);
  INIT_TYPE(Panelize);
  INIT_TYPE(ElementOrder);
  INIT_TYPE(ElementPivot);
  INIT_TYPE(Inset);
  INIT_TYPE(Poke);
  INIT_TYPE(BeautifyFaces);
  INIT_TYPE(RotateEdges);
  INIT_TYPE(Symmetrize);
  INIT_TYPE(Push);
  INIT_TYPE(Melt);
  INIT_TYPE(Sweep);
  INIT_TYPE(SmartUVProject);
  INIT_TYPE(VoronoiFracture);
  INIT_TYPE(Relax);
  INIT_TYPE(PolyEdit);
  INIT_TYPE(Dice);
  INIT_TYPE(EditPoly);
  INIT_TYPE(Collapse);
  INIT_TYPE(Drape);
  INIT_TYPE(Shatter);
  INIT_TYPE(Masonry);
  INIT_TYPE(Dissolve);
  INIT_TYPE(UVScale);
  INIT_TYPE(EdgeDamage);
  INIT_TYPE(Divide);
  INIT_TYPE(Join);
  INIT_TYPE(Blast);
  INIT_TYPE(Planks);
  INIT_TYPE(Roof);
  INIT_TYPE(Carve);
  INIT_TYPE(Materialize);
  INIT_TYPE(Cleanup);
  INIT_TYPE(Stonework);
  INIT_TYPE(Distribute);
  INIT_TYPE(PieceId);
  INIT_TYPE(UVUnwrap);
  INIT_TYPE(UVPack);
  INIT_TYPE(Conform);
  INIT_TYPE(NormalTransfer);
  INIT_TYPE(NormalSmooth);
  INIT_TYPE(NormalFaceSets);
  INIT_TYPE(NormalShape);
  INIT_TYPE(Trim);
  INIT_TYPE(Tile);
  INIT_TYPE(Patch);
  INIT_TYPE(AutoWeights);
  INIT_TYPE(SmoothWeights);
  INIT_TYPE(TransferWeights);
  INIT_TYPE(BoneColors);
  INIT_TYPE(Pack);
  INIT_TYPE(Rock);
  INIT_TYPE(Masks);
  INIT_TYPE(Snow);
  INIT_TYPE(Tree);
  INIT_TYPE(Resample);
  INIT_TYPE(Lean);
  INIT_TYPE(Chisel);
  INIT_TYPE(Faceset);
  INIT_TYPE(Wrap);
  INIT_TYPE(Place);
  INIT_TYPE(Scatter);
  INIT_TYPE(Dent);
  INIT_TYPE(Contact);
  INIT_TYPE(Cable);
  INIT_TYPE(Cracks);
  INIT_TYPE(CollisionHull);
  INIT_TYPE(Bricks);
  INIT_TYPE(UVRotate);
  INIT_TYPE(UVWMap);
  INIT_TYPE(Bend);
  INIT_TYPE(Twist);
  INIT_TYPE(Taper);
  INIT_TYPE(Stretch);
  INIT_TYPE(Tension);
  INIT_TYPE(RadialArray);
  INIT_TYPE(VolumeSelect);
  INIT_TYPE(MaterialByElement);
  INIT_TYPE(DataChannel);
  INIT_TYPE(FaceExtrude);
  INIT_TYPE(DeleteMesh);
  INIT_TYPE(NormalizeSpline);
  INIT_TYPE(RelaxSpline);
  INIT_TYPE(ChamferSpline);
  INIT_TYPE(SplineMorph);
  INIT_TYPE(ShapeKeyInbetween);
  INIT_TYPE(PolySelect);
#undef INIT_TYPE
}

namespace {

class ModifierCachedValue : public memory_cache::CachedValue {
 public:
  Mesh *mesh = nullptr;
  /** The modifier's error or warning, empty for none. */
  std::string message;

  ~ModifierCachedValue() override
  {
    if (mesh != nullptr) {
      BKE_id_free(nullptr, mesh);
    }
  }

  void count_memory(MemoryCounter &memory) const override
  {
    if (mesh != nullptr) {
      mesh->count_memory(memory);
    }
  }
};

}  // namespace

int MOD_material_slot(Mesh &mesh, Material *material, const int index)
{
  if (material == nullptr) {
    return index;
  }
  if (mesh.totcol == 0) {
    /* Faces that get no material keep pointing at an empty slot. */
    BKE_id_material_eval_ensure_default_slot(&mesh.id);
  }
  const int materials_num = mesh.totcol;
  if (mesh.mat && materials_num > 0) {
    const int found = Span(mesh.mat, materials_num).first_index_try(material);
    if (found != -1) {
      return found;
    }
  }
  BKE_id_material_eval_assign(&mesh.id, materials_num + 1, material);
  return materials_num;
}

void MOD_material_slots_from(Mesh &result, const Mesh &mesh)
{
  if (&result == &mesh) {
    return;
  }
  for (int i = result.totcol; i < mesh.totcol; i++) {
    BKE_id_material_eval_assign(&result.id, i + 1, mesh.mat[i]);
  }
}

Mesh *MOD_cached_modify_mesh(
    ModifierData *md,
    const ModifierEvalContext *ctx,
    Mesh *mesh,
    const FunctionRef<Mesh *(ModifierData *, const ModifierEvalContext *, Mesh *)> compute,
    const FunctionRef<void(geometry::SimCacheHasher &)> extra)
{
  const ModifierTypeInfo *info = BKE_modifier_get_info(ModifierType(md->type));
  geometry::SimCacheHasher hasher("modifier");
  hasher.add(md->type);
  hasher.add(ctx->flag);
  /* The settings: everything after the part all modifiers share. */
  hasher.add_bytes(reinterpret_cast<const char *>(md) + sizeof(ModifierData),
                   int64_t(info->struct_size) - int64_t(sizeof(ModifierData)));
  hasher.add_mesh(*mesh);
  if (extra) {
    extra(hasher);
  }

  bool computed = false;
  Mesh *own_result = nullptr;
  const std::shared_ptr<const ModifierCachedValue> value =
      std::dynamic_pointer_cast<const ModifierCachedValue>(
          geometry::sim_cache_get_value(hasher.finish(), [&]() {
            computed = true;
            auto value = std::make_unique<ModifierCachedValue>();
            if (md->error) {
              MEM_delete(md->error);
              md->error = nullptr;
            }
            Mesh *result = compute(md, ctx, mesh);
            if (md->error) {
              value->message = md->error;
            }
            if (result != nullptr) {
              /* The cache keeps its own copy. The arrays are shared, so it is cheap. */
              value->mesh = BKE_mesh_copy_for_eval(*result);
              geometry::sim_cache_mesh_prepare(*value->mesh);
            }
            own_result = result;
            return value;
          }));
  if (computed) {
    /* The modifier ran: its result and message are as it made them. */
    return own_result;
  }
  if (!value) {
    return compute(md, ctx, mesh);
  }
  if (!value->message.empty()) {
    BKE_modifier_set_warning(ctx->object, md, "%s", value->message.c_str());
  }
  if (value->mesh == nullptr) {
    return nullptr;
  }
  Mesh *result = geometry::sim_cache_mesh_copy(*value->mesh, *mesh);
  /* Like a result made now: linked to the Edit Mode data of the input, if any. */
  result->runtime->edit_mesh = mesh->runtime->edit_mesh;
  return result;
}

}  // namespace blender
