/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 */

#pragma once

/* so modifier types match their defines */
#include "MOD_modifiertypes.hh"

#include "BLI_array.hh"
#include "BLI_function_ref.hh"
#include "BLI_vector.hh"

#include "DNA_listBase.h"

#include "BKE_geometry_set.hh"

#include "DEG_depsgraph_build.hh"

namespace blender {

struct ArraySourceObject;
struct Collection;
struct MDeformVert;
struct Material;
struct Mesh;
struct ModifierData;
struct ModifierEvalContext;
struct ModifierUpdateDepsgraphContext;
struct Object;

/**
 * The evaluated meshes Array and Radial Array place for #ArraySource, without \a self and
 * without objects that have no mesh. Instances are realized,  r_keep owns the meshes made
 * for them. Empty for #MOD_ARRAY_SOURCE_MESH.
 */
Vector<const Mesh *> MOD_array_source_meshes(int source,
                                             Collection *collection,
                                             const ListBaseT<ArraySourceObject> &objects,
                                             const Object *self,
                                             Vector<bke::GeometrySet> &r_keep);

/** Relations to the objects that #MOD_array_source_meshes reads. */
void MOD_array_source_update_depsgraph(const ModifierUpdateDepsgraphContext *ctx,
                                       int source,
                                       Collection *collection,
                                       const ListBaseT<ArraySourceObject> &objects,
                                       const char *description);

/**
 * For each copy, the index of the placed mesh in a list of \a meshes_num meshes, by #ArrayPick,
 * or -1 for the modifier's own mesh.
 */
Array<int> MOD_array_source_pick(int pick, int seed, int copies_num, int meshes_num);

namespace geometry {
class SimCacheHasher;
}

void MOD_init_texture(MappingInfoModifierData *dmd, const ModifierEvalContext *ctx);
/**
 * \param cos: may be null, in which case we use directly mesh vertices' coordinates.
 */
void MOD_get_texture_coords(MappingInfoModifierData *dmd,
                            const ModifierEvalContext *ctx,
                            Object *ob,
                            Mesh *mesh,
                            float (*cos)[3],
                            float (*r_texco)[3]);

void MOD_previous_vcos_store(ModifierData *md, const float (*vert_coords)[3]);

void MOD_get_vgroup(const Object *ob,
                    const Mesh *mesh,
                    const char *name,
                    const MDeformVert **dvert,
                    int *defgrp_index);

void MOD_depsgraph_update_object_bone_relation(DepsNodeHandle *node,
                                               Object *object,
                                               const char *bonename,
                                               const char *description);

/**
 * The slot of  material in the mesh, added at the end when it is not there yet. When
 *  material is null,  index is returned as it is. Cached modifiers call this on their input
 * mesh before the cache too, so a result taken from it has the slot as well.
 */
int MOD_material_slot(Mesh &mesh, Material *material, int index);

/** Adds the slots of  mesh that  result is missing, for a result made from it. */
void MOD_material_slots_from(Mesh &result, const Mesh &mesh);

/**
 * The result of a slow modifier, from the cache when the mesh, the modifier's settings and what
 * \a extra adds are the same as before. Objects are evaluated again for undo and for changes to
 * other objects, and this keeps that from computing everything again. The modifier's message is
 * kept with the result and shown again.
 *
 * \param compute: The modifier's own #modify_mesh.
 * \param extra: Adds what else the result depends on, like the placement of other objects.
 */
Mesh *MOD_cached_modify_mesh(
    ModifierData *md,
    const ModifierEvalContext *ctx,
    Mesh *mesh,
    FunctionRef<Mesh *(ModifierData *, const ModifierEvalContext *, Mesh *)> compute,
    FunctionRef<void(geometry::SimCacheHasher &)> extra = nullptr);

}  // namespace blender
