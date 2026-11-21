/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Avoid: keeps the branches out of an object's mesh, so they grow around walls and rocks.
 */

#include "BLT_translation.hh"

#include "NOD_socket_declarations.hh"
#include "NOD_socket_declarations_geometry.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_tree_graph_avoid_cc {

static const EnumPropertyItem mode_items[] = {
    {0, "PUSH", 0, N_("Push"), N_("Turn away when they come close to the mesh")},
    {1,
     "LEAN",
     0,
     N_("Lean"),
     N_("Look ahead and lean toward open space, starting long before they reach the mesh")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Geometry>("Force").description(
      "Link into the Forces of a stem node, everything that grows on it avoids the mesh too");

  b.add_input<decl::Object>("Object").description("The mesh object to grow around");
  b.add_input<decl::Menu>("Mode")
      .default_value(MenuValue(0))
      .static_items(mode_items)
      .description("Turn away close by, or lean toward open space early");
  b.add_input<decl::Float>("Strength")
      .default_value(1.0f)
      .min(-10.0f)
      .max(10.0f)
      .description("How hard the branches are pushed away");
  b.add_input<decl::Float>("Distance")
      .default_value(0.3f)
      .min(0.0f)
      .subtype(PROP_DISTANCE)
      .description("Push: how far from the mesh the branches start to turn away")
      .supports_field();
  b.add_input<decl::Float>("Reach")
      .default_value(3.0f)
      .min(0.0f)
      .subtype(PROP_DISTANCE)
      .description("Lean: how far ahead the stems look for the mesh");
  b.add_input<decl::Int>("Rays").default_value(8).min(1).max(64).description(
      "Lean: rays each section looks with, more for smoother leaning");
  b.add_input<decl::Float>("Up Bias")
      .default_value(0.5f)
      .min(0.0f)
      .max(4.0f)
      .description("Lean: how much open space above counts over open space to the side");
}

static void node_register()
{
  static bke::bNodeType ntype;

  tree_graph_node_type_base(&ntype, "TreeGraphNodeAvoid");
  ntype.ui_name = "Avoid";
  ntype.ui_description = "Keeps branches out of an object as they grow";
  ntype.enum_name_legacy = "TREE_GRAPH_AVOID";
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_tree_graph_avoid_cc
