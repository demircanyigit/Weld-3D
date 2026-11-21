/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 */

#include <algorithm>

#include "BLI_math_matrix.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "DNA_collection_types.h"
#include "DNA_layer_types.h"
#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"

#include "BKE_collection.hh"
#include "BKE_geometry_set_instances.hh"
#include "BKE_instances.hh"
#include "BKE_mesh.hh"
#include "BKE_modifier.hh"

#include "DEG_depsgraph_build.hh"

#include "GEO_realize_instances.hh"

#include "MOD_place_util.hh"

namespace blender {

Vector<PlaceRow> MOD_place_rows(const int source,
                                Collection *collection,
                                const ListBaseT<ArraySourceObject> &objects,
                                const Object *self)
{
  Vector<PlaceRow> rows;
  if (source == MOD_PLACE_SOURCE_COLLECTION) {
    if (collection == nullptr) {
      return rows;
    }
    FOREACH_COLLECTION_OBJECT_RECURSIVE_BEGIN (collection, ob) {
      if (ob != self) {
        rows.append({ob, 1.0f, 1});
      }
    }
    FOREACH_COLLECTION_OBJECT_RECURSIVE_END;
    std::stable_sort(rows.begin(), rows.end(), [](const PlaceRow &a, const PlaceRow &b) {
      return BLI_strcasecmp_natural(a.object->id.name + 2, b.object->id.name + 2) < 0;
    });
    return rows;
  }
  for (const ArraySourceObject &item : objects) {
    PlaceRow row;
    row.object = item.object == self ? nullptr : item.object;
    row.weight = item.weight;
    row.count = item.count;
    rows.append(row);
  }
  return rows;
}

void MOD_place_add_object(PlaceObjects &r_objects, Object *ob, const float weight)
{
  geometry::PlaceItem item;
  item.weight = weight;
  item.valid = false;
  float3x3 shape = float3x3::identity();
  if (ob != nullptr) {
    bke::GeometrySet set = bke::object_get_evaluated_geometry_set(*ob);
    if (set.has_instances()) {
      set = geometry::realize_instances(set, geometry::RealizeInstancesOptions()).geometry;
    }
    const std::optional<Bounds<float3>> bounds = set.compute_boundbox_without_instances();
    if (bounds) {
      /* Turned and scaled as the object is, but not moved. */
      shape = float3x3(ob->object_to_world());
      float3 lo(1e30f), hi(-1e30f);
      for (int i = 0; i < 8; i++) {
        const float3 corner((i & 1) ? bounds->max.x : bounds->min.x,
                            (i & 2) ? bounds->max.y : bounds->min.y,
                            (i & 4) ? bounds->max.z : bounds->min.z);
        const float3 p = shape * corner;
        lo = math::min(lo, p);
        hi = math::max(hi, p);
      }
      item.min = lo;
      item.max = hi;
      item.valid = true;
    }
  }
  r_objects.items.append(item);
  r_objects.objects.append(ob);
  r_objects.shapes.append(shape);
}

PlaceObjects MOD_place_objects(const Span<PlaceRow> rows)
{
  PlaceObjects result;
  for (const PlaceRow &row : rows) {
    MOD_place_add_object(result, row.object, row.weight);
  }
  return result;
}

Vector<int> MOD_place_pattern(const StringRef text,
                              const Span<PlaceRow> rows,
                              std::string &r_error)
{
  bool typed = false;
  for (const char c : text) {
    if (!ELEM(c, ' ', '\t', ',', ';', '\n')) {
      typed = true;
      break;
    }
  }
  if (typed) {
    return geometry::place_parse_pattern(text, int(rows.size()), r_error);
  }
  Vector<int> result;
  for (const int i : rows.index_range()) {
    for (int t = 0; t < std::clamp(rows[i].count, 0, 1000); t++) {
      result.append(i);
    }
  }
  return result;
}

std::string MOD_place_pattern_names(const StringRef text,
                                    const Span<PlaceRow> rows,
                                    std::string &r_error)
{
  const Vector<int> pattern = MOD_place_pattern(text, rows, r_error);
  std::string result;
  for (const int i : pattern.index_range()) {
    if (i == 12) {
      result += ", ...";
      break;
    }
    if (i > 0) {
      result += ", ";
    }
    const Object *ob = rows[pattern[i]].object;
    result += ob ? ob->id.name + 2 : "-";
  }
  return result;
}

void MOD_place_update_depsgraph(const ModifierUpdateDepsgraphContext *ctx,
                                const int source,
                                Collection *collection,
                                const ListBaseT<ArraySourceObject> &objects,
                                Object *extra,
                                const char *description)
{
  if (source == MOD_PLACE_SOURCE_COLLECTION && collection != nullptr) {
    DEG_add_collection_geometry_relation(ctx->node, collection, description);
  }
  auto add = [&](Object *ob) {
    if (ob != nullptr && ob != ctx->object) {
      DEG_add_object_relation(ctx->node, ob, DEG_OB_COMP_GEOMETRY, description);
      DEG_add_object_relation(ctx->node, ob, DEG_OB_COMP_TRANSFORM, description);
    }
  };
  if (source == MOD_PLACE_SOURCE_OBJECTS) {
    for (const ArraySourceObject &item : objects) {
      add(item.object);
    }
  }
  add(extra);
}

bke::GeometrySet MOD_place_output(const PlaceObjects &objects,
                                  const Span<geometry::PlacedObject> placed,
                                  const Mesh *keep_mesh,
                                  const bool realize)
{
  std::unique_ptr<bke::Instances> instances = std::make_unique<bke::Instances>(placed.size());
  Array<int> handle_of(objects.objects.size(), -1);
  MutableSpan<int> handles = instances->reference_handles_for_write();
  MutableSpan<float4x4> transforms = instances->transforms_for_write();
  for (const int i : placed.index_range()) {
    const int item = placed[i].item;
    if (handle_of[item] < 0) {
      handle_of[item] = instances->add_reference(bke::InstanceReference(*objects.objects[item]));
    }
    handles[i] = handle_of[item];
    transforms[i] = placed[i].transform * float4x4(objects.shapes[item]);
  }

  bke::GeometrySet result;
  if (keep_mesh != nullptr) {
    result.replace_mesh(BKE_mesh_copy_for_eval(*keep_mesh));
  }
  result.replace_instances(instances.release());
  if (realize) {
    result = geometry::realize_instances(result, geometry::RealizeInstancesOptions()).geometry;
  }
  return result;
}

}  // namespace blender
