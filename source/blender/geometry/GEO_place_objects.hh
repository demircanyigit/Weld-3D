/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup geo
 *
 * Place and Scatter: objects along the edges, on the faces or on the vertices of a mesh, or at
 * random spots on its faces. The objects are picked by a pattern, in order or at random by weight,
 * and packed by their measured sides.
 */

#include <string>

#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

struct Mesh;

namespace blender::geometry {

/** One object to place: its bounding box in its own (turned and scaled) space. */
struct PlaceItem {
  float3 min = float3(-0.5f);
  float3 max = float3(0.5f);
  /** How often it is picked at random, next to the others. */
  float weight = 1.0f;
  /** False for an empty list row: its turns are skipped. */
  bool valid = true;
};

enum class PlacePick : int8_t {
  Pattern = 0,
  Random = 1,
  InOrder = 2,
};

/**
 * Reads a pattern of 1-based list numbers, like "1 1 1 2", "1x3 2" or "1, 2". Returns 0-based
 * numbers. On a bad word, \a r_error names it and the words before it are kept.
 */
Vector<int> place_parse_pattern(StringRef text, int rows_num, std::string &r_error);

/** Chooses the item for each turn ("slot"). */
class PlacePicker {
  PlacePick pick_;
  Vector<int> sequence_;
  Vector<float> cumulative_;
  int items_num_ = 0;
  int seed_ = 0;

 public:
  /**
   * \param sequence: the pattern, used for #PlacePick::Pattern.
   * \param items: the list rows, for their weights.
   */
  PlacePicker(PlacePick pick, Span<int> sequence, Span<PlaceItem> items, int seed);
  /** The item for a turn, or -1 for none. */
  int pick(int slot) const;
  bool is_empty() const;
};

enum class PlaceUp : int8_t {
  Z = 0,
  Normal = 1,
};

enum class PlaceBase : int8_t {
  Bottom = 0,
  Center = 1,
};

enum class PlaceSide : int8_t {
  Center = 0,
  Inside = 1,
  Outside = 2,
};

enum class PlaceEdges : int8_t {
  All = 0,
  Boundary = 1,
  Loose = 2,
};

enum class PlaceFit : int8_t {
  Stop = 0,
  Spread = 1,
  Stretch = 2,
  Overhang = 3,
};

enum class PlaceCorners : int8_t {
  Touch = 0,
  PullBack = 1,
  CornerObject = 2,
};

/** How each object stands, and how much it varies. */
struct PlaceLook {
  /** The object axis that follows the edge or the forward direction: 0 is X, 1 is Y. Z is up. */
  int along = 0;
  PlaceUp up = PlaceUp::Z;
  PlaceBase base = PlaceBase::Bottom;
  PlaceSide side = PlaceSide::Center;
  /** Moves the objects along their up direction. */
  float offset = 0.0f;
  /** Largest random turn around the up direction, in radians. */
  float rotation_random = 0.0f;
  /** Largest random lean to the side, in radians. */
  float tilt_random = 0.0f;
  float scale_min = 1.0f;
  float scale_max = 1.0f;
  /** Random scale of the up axis only, on top of the scale. */
  float height_min = 1.0f;
  float height_max = 1.0f;
  int seed = 0;
};

struct PlacedObject {
  int item = 0;
  /** From the item's own space to the mesh's space. */
  float4x4 transform;
};

struct PlaceEdgesParams {
  /** The list rows, followed by the corner object when there is one. */
  Span<PlaceItem> items;
  const PlacePicker *picker = nullptr;
  /** Per point, which points are used. Empty for all. */
  Span<bool> vert_selection;
  PlaceEdges edges = PlaceEdges::All;
  /** Paths break where they turn more than this, in radians. */
  float corner_angle = 0.523599f;
  float gap = 0.0f;
  PlaceFit fit = PlaceFit::Stop;
  PlaceCorners corners = PlaceCorners::Touch;
  /** Index in #items of the corner object, or -1. */
  int corner_item = -1;
  bool restart_per_path = false;
  PlaceLook look;
};

/** Objects packed along chains of edges, end to end by their sides. */
Vector<PlacedObject> place_along_edges(const Mesh &mesh, const PlaceEdgesParams &params);

/** One object on each used point, facing along its first edge. */
Vector<PlacedObject> place_on_vertices(const Mesh &mesh,
                                       Span<PlaceItem> items,
                                       const PlacePicker &picker,
                                       Span<bool> vert_selection,
                                       const PlaceLook &look);

/**
 * One object on the center of each face whose points are all used, facing along its longest
 * edge. With \a fit_to_face, the object is scaled to the face.
 */
Vector<PlacedObject> place_on_faces(const Mesh &mesh,
                                    Span<PlaceItem> items,
                                    const PlacePicker &picker,
                                    Span<bool> vert_selection,
                                    const PlaceLook &look,
                                    bool fit_to_face);

struct ScatterParams {
  Span<PlaceItem> items;
  const PlacePicker *picker = nullptr;
  /** Per point, the chance of an object nearby. Empty for everywhere. */
  Span<float> vert_weights;
  /** A fixed number of spots, instead of a number per square meter. */
  bool use_count = false;
  float density = 10.0f;
  int count = 100;
  int max_count = 10000;
  /** Least distance between objects. */
  float spacing = 0.0f;
  /** Objects keep apart by their measured size. */
  bool no_overlap = false;
  PlaceLook look;
};

/** Objects at random spots on the faces. */
Vector<PlacedObject> scatter_on_faces(const Mesh &mesh, const ScatterParams &params);

}  // namespace blender::geometry
