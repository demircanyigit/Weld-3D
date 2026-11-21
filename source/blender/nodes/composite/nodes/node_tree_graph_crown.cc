/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Crown: keeps the stems inside a shape, shortening the ones that reach out of it.
 */

#include "BLT_translation.hh"

#include "NOD_socket_declarations.hh"
#include "NOD_socket_declarations_geometry.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_tree_graph_crown_cc {

static const EnumPropertyItem shape_items[] = {
    {0, "SPHERE", 0, N_("Sphere"), N_("A round crown")},
    {1, "DOME", 0, N_("Dome"), N_("The top half of a sphere, flat underneath")},
    {2, "CONE", 0, N_("Cone"), N_("Wide at the bottom and pointed at the top")},
    {3, "CYLINDER", 0, N_("Cylinder"), N_("Straight up and down")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Geometry>("Crown").description(
      "Link into the Forces of a Branches or Leaves node");

  b.add_input<decl::Menu>("Shape")
      .default_value(MenuValue(0))
      .static_items(shape_items)
      .description("The shape the stems stay in");
  b.add_input<decl::Vector>("Size")
      .default_value(float3(4.0f, 4.0f, 5.0f))
      .subtype(PROP_XYZ)
      .description("Half the width, depth and height of the shape");
  b.add_input<decl::Vector>("Offset")
      .default_value(float3(0.0f, 0.0f, 6.0f))
      .subtype(PROP_TRANSLATION)
      .description("Middle of the shape, from the foot of the tree");
  b.add_input<decl::Object>("Object").description(
      "Places and sizes the shape instead, a scale of 1 fills -1 to 1");
  b.add_input<decl::Float>("Softness")
      .default_value(0.3f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Lets some stems reach further out than others");
}

static void node_register()
{
  static bke::bNodeType ntype;

  tree_graph_node_type_base(&ntype, "TreeGraphNodeCrown");
  ntype.ui_name = "Crown";
  ntype.ui_description = "Keeps stems inside a shape";
  ntype.enum_name_legacy = "TREE_GRAPH_CROWN";
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_tree_graph_crown_cc
