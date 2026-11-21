/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Branch Info: what the Tree modifier knows about the branch, leaf or frond a number is worked
 * out for. Linked through Math, Map Range or Random Value nodes into a number on a level, it lets
 * that number change from branch to branch and along each branch.
 */

#include "NOD_socket_declarations.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_tree_graph_branch_info_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Float>("Along Parent")
      .description(
          "Where the branch, leaf or frond sits on its parent, from the first of its level (0) to "
          "the last (1)");
  b.add_output<decl::Float>("Along Branch")
      .description(
          "Place along the branch from its base (0) to its tip (1), for numbers that change "
          "along it: Radius, Taper, Flare, Droop, Wiggle and forces");
  b.add_output<decl::Float>("Random").description(
      "A different number from 0 to 1 for every branch, leaf and frond");
  b.add_output<decl::Int>("ID").description(
      "A different number for every branch, leaf and frond, for the ID of a Random Value node");
  b.add_output<decl::Vector>("Position")
      .subtype(PROP_TRANSLATION)
      .description("The point being grown, in the space of the tree object");
  b.add_output<decl::Float>("Parent Length")
      .description("Length of the parent, before the tree's scale, 0 for trunks");
  b.add_output<decl::Float>("Parent Radius")
      .description("Radius of the parent at its base, before the tree's scale");
  b.add_output<decl::Int>("Depth").description("Levels down from the trunk, the trunk is 0");
  b.add_output<decl::Int>("Index").description("Number of this one among its parent's children");
  b.add_output<decl::Int>("Count").description("How many children the parent has of this level");
  b.add_output<decl::Float>("Height").description(
      "Height above the foot of the tree, before the tree's scale");
  b.add_output<decl::Float>("Distance to Trunk")
      .description("Distance from the trunk's line, flat, before the tree's scale");
}

static void node_register()
{
  static bke::bNodeType ntype;

  tree_graph_node_type_base(&ntype, "TreeGraphNodeBranchInfo");
  ntype.ui_name = "Branch Info";
  ntype.ui_description = "What is known about the branch, leaf or frond being grown";
  ntype.enum_name_legacy = "TREE_GRAPH_BRANCH_INFO";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_tree_graph_branch_info_cc
