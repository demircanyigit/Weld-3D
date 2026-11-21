/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * What the Place and Scatter modifiers share: the list of objects, their measured boxes, the
 * pattern, and the placed objects as instances.
 */

#pragma once

#include <string>

#include "BLI_math_matrix_types.hh"
#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "DNA_listBase.h"

#include "BKE_geometry_set.hh"

#include "GEO_place_objects.hh"

namespace blender {

struct ArraySourceObject;
struct Collection;
struct Mesh;
struct ModifierUpdateDepsgraphContext;
struct Object;

/** One row of the list, or one object of the collection. */
struct PlaceRow {
  Object *object = nullptr;
  float weight = 1.0f;
  int count = 1;
};

/**
 * The rows of the list for #MOD_PLACE_SOURCE_OBJECTS, empty rows included so the numbers match
 * the list. For #MOD_PLACE_SOURCE_COLLECTION, the collection's objects sorted by name. \a self is
 * never placed: its row stays, empty.
 */
Vector<PlaceRow> MOD_place_rows(int source,
                                Collection *collection,
                                const ListBaseT<ArraySourceObject> &objects,
                                const Object *self);

/** The objects to place, with their measured boxes. */
struct PlaceObjects {
  Vector<geometry::PlaceItem> items;
  Vector<Object *> objects;
  /** Each object's turn and scale, without its place. */
  Vector<float3x3> shapes;
};

/** Measures \a ob and adds it. An object without geometry is added as unusable. */
void MOD_place_add_object(PlaceObjects &r_objects, Object *ob, float weight);

/** The objects of all \a rows, in order. */
PlaceObjects MOD_place_objects(Span<PlaceRow> rows);

/** The typed pattern, or the rows' counts in order when the text is empty. */
Vector<int> MOD_place_pattern(StringRef text, Span<PlaceRow> rows, std::string &r_error);

/** The pattern with the object names, like "Log, Log, Log, Post", for the panel. */
std::string MOD_place_pattern_names(StringRef text, Span<PlaceRow> rows, std::string &r_error);

void MOD_place_update_depsgraph(const ModifierUpdateDepsgraphContext *ctx,
                                int source,
                                Collection *collection,
                                const ListBaseT<ArraySourceObject> &objects,
                                Object *extra,
                                const char *description);

/**
 * The placed objects as instances, next to a copy of \a keep_mesh when it is given.
 * With \a realize, everything is made into one mesh.
 */
bke::GeometrySet MOD_place_output(const PlaceObjects &objects,
                                  Span<geometry::PlacedObject> placed,
                                  const Mesh *keep_mesh,
                                  bool realize);

}  // namespace blender
