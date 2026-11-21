/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Force: turns the stems a little at every section, toward a direction or a point, or away from
 * a point. Thin wood turns more than thick.
 */

#include "BLT_translation.hh"

#include "NOD_socket_declarations.hh"
#include "NOD_socket_declarations_geometry.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_tree_graph_force_cc {

static const EnumPropertyItem mode_items[] = {
    {0, "DIRECTION", 0, N_("Direction"), N_("Toward a direction, down for gravity")},
    {1, "TOWARD", 0, N_("Toward"), N_("Toward the object, or the point in Direction")},
    {2, "AWAY", 0, N_("Away"), N_("Away from the object, or the point in Direction")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Geometry>("Force").description(
      "Link into the Forces of a Trunk, Branches or Roots node");

  b.add_input<decl::Menu>("Mode")
      .default_value(MenuValue(0))
      .static_items(mode_items)
      .description("Which way the stems turn");
  b.add_input<decl::Vector>("Direction")
      .default_value(float3(0.0f, 0.0f, 1.0f))
      .description("Direction: the way to turn. Toward and Away: the point, without an object");
  b.add_input<decl::Object>("Object").description("Toward and Away: the object to turn to");
  b.add_input<decl::Float>("Strength")
      .default_value(0.02f)
      .min(-1.0f)
      .max(1.0f)
      .description("Turn per section, below zero the other way")
      .supports_field();
}

static void node_register()
{
  static bke::bNodeType ntype;

  tree_graph_node_type_base(&ntype, "TreeGraphNodeForce");
  ntype.ui_name = "Force";
  ntype.ui_description = "Turns stems toward a direction or a point as they grow";
  ntype.enum_name_legacy = "TREE_GRAPH_FORCE";
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_tree_graph_force_cc
