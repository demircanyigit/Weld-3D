/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Prune: removes or shortens branches and leaves once they are grown.
 */

#include "BLT_translation.hh"

#include "NOD_socket_declarations.hh"
#include "NOD_socket_declarations_geometry.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_tree_graph_prune_cc {

static const EnumPropertyItem mode_items[] = {
    {0, "RANDOM", 0, N_("Random"), N_("A share of them, any of them")},
    {1, "DOWN", 0, N_("Facing Down"), N_("The ones growing down")},
    {2, "CROWDED", 0, N_("Crowded"), N_("The ones too close to others of their level")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Geometry>("Prune").description(
      "Link into the Forces of a Trunk, Branches, Roots or Leaves node");

  b.add_input<decl::Menu>("Mode")
      .default_value(MenuValue(0))
      .static_items(mode_items)
      .description("Which ones are taken away");
  b.add_input<decl::Float>("Amount")
      .default_value(0.3f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("How many are taken away")
      .supports_field();
  b.add_input<decl::Float>("Spacing")
      .default_value(0.3f)
      .min(0.0f)
      .subtype(PROP_DISTANCE)
      .description("Crowded: the room each one keeps from the others");
  b.add_input<decl::Int>("Seed").default_value(0).description("Another pick");
}

static void node_register()
{
  static bke::bNodeType ntype;

  tree_graph_node_type_base(&ntype, "TreeGraphNodePrune");
  ntype.ui_name = "Prune";
  ntype.ui_description = "Removes branches or leaves";
  ntype.enum_name_legacy = "TREE_GRAPH_PRUNE";
  ntype.nclass = NODE_CLASS_OP_FILTER;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_tree_graph_prune_cc
