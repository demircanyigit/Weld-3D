/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Turns the nodes linked into the numbers of a Tree Graph into functions the tree can call for
 * every branch, leaf and place along a branch. Math, Vector Math, Map Range, Random Value and the
 * other value nodes work as they do in geometry nodes, with Branch Info telling them about the
 * branch being grown.
 */

#pragma once

#include <functional>

#include "BLI_map.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"

#include "GEO_mesh_tree.hh"

struct bNodeSocket;
struct bNodeTree;
struct IDProperty;
struct Object;

namespace blender::tree_fields {

using Context = geometry::tree::FieldContext;
/** Floats, integers and booleans fill all three components, vectors and colors one each. */
using Fn = std::function<float3(const Context &)>;

/** The value of a modifier input, with up to four components. */
float4 property_value(const IDProperty &property);
void property_value_set(IDProperty &property, const float4 &value);

class FieldCompiler {
 public:
  /**
   * \param self: The object with the modifier, other objects are read in its space.
   * \param tree: The graph, for the defaults of its inputs.
   * \param inputs: The values the modifier gives the graph's inputs, by socket identifier.
   */
  FieldCompiler(const Object *self,
                const float4x4 &world_to_tree,
                const bNodeTree *tree = nullptr,
                const IDProperty *inputs = nullptr);

  /** A number on a level: fixed, or a function when nodes are linked into it. */
  geometry::tree::Param param(const bNodeSocket &input);
  /** Inputs that are worked out once for the whole tree. */
  float number(const bNodeSocket &input);
  int integer(const bNodeSocket &input);
  float3 vector(const bNodeSocket &input);

  const Object *self() const
  {
    return self_;
  }
  const float4x4 &world_to_tree() const
  {
    return world_to_tree_;
  }

 private:
  Fn input(const bNodeSocket &input, int depth);
  bool is_linked(const bNodeSocket &input) const;
  Fn output(const bNodeSocket &output, int depth);
  Fn build_output(const bNodeSocket &output, int depth);
  float3 group_input(StringRef identifier) const;

  const Object *self_;
  float4x4 world_to_tree_;
  const bNodeTree *tree_;
  const IDProperty *inputs_;
  Map<const bNodeSocket *, Fn> outputs_;
};

}  // namespace blender::tree_fields
