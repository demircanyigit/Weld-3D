/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Object Info: where an object is, for numbers and vectors on a Tree Graph, such as the point of
 * an Attract node or the center of a Crown Shape.
 */

#include "NOD_socket_declarations.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_tree_graph_object_info_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Vector>("Location")
      .subtype(PROP_TRANSLATION)
      .description("Where the object is, in the space of the tree object");
  b.add_output<decl::Vector>("Rotation")
      .subtype(PROP_EULER)
      .description("How the object is turned in the world");
  b.add_output<decl::Vector>("Scale").subtype(PROP_XYZ).description(
      "Size of the object in the world");

  b.add_input<decl::Object>("Object").description("The object to read");
}

static void node_register()
{
  static bke::bNodeType ntype;

  tree_graph_node_type_base(&ntype, "TreeGraphNodeObjectInfo");
  ntype.ui_name = "Object Info";
  ntype.ui_description = "Where an object is, how it is turned and its size";
  ntype.enum_name_legacy = "TREE_GRAPH_OBJECT_INFO";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_tree_graph_object_info_cc
