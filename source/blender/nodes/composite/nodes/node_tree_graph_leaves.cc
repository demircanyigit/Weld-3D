/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Leaves: cards or meshes along the stems they are linked into, and one at each tip.
 */

#include "BLT_translation.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "NOD_socket_declarations.hh"
#include "NOD_socket_declarations_geometry.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_tree_graph_leaves_cc {

/** Values by #NormalMode in the generator. */
static const EnumPropertyItem normal_items[] = {
    {0, "FLAT", 0, N_("Flat"), N_("The normals of the cards themselves")},
    {1, "SPHERICAL", 0, N_("Spherical"), N_("Out from the middle of the foliage")},
    {2, "CROWN", 0, N_("Crown"), N_("Out from an ellipsoid fitted around all the foliage")},
    {3, "UP", 0, N_("Up"), N_("Straight up, for grass and low plants")},
    {4, "CLUMPS", 0, N_("Clumps"), N_("Out of the merged clumps, each one lights like a soft ball")},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem cards_items[] = {
    {0, "SINGLE", 0, N_("Single"), N_("One card per leaf")},
    {1, "CROSS", 0, N_("Cross"), N_("Two cards at a right angle, full from every side")},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem shape_items[] = {
    {0, "CARD", 0, N_("Card"), N_("A square card for a leaf texture")},
    {1, "MESH", 0, N_("Mesh"), N_("The mesh of an object, or of the objects in a collection")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();

  b.add_output<decl::Geometry>("Leaves");
  b.add_input<decl::Geometry>("Forces").multi_input().description(
      "Crown and Prune nodes for these leaves");

  b.add_input<decl::Int>("Count")
      .default_value(16)
      .min(0)
      .max(1000)
      .description("Leaves along each parent")
      .supports_field();
  b.add_input<decl::Float>("Start")
      .default_value(0.2f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Where on the parent the first one grows")
      .supports_field();
  b.add_input<decl::Float>("Angle")
      .default_value(0.698f)
      .min(0.0f)
      .max(3.14159f)
      .subtype(PROP_ANGLE)
      .description("Angle to the parent")
      .supports_field();
  b.add_input<decl::Float>("Size")
      .default_value(0.25f)
      .min(0.0f)
      .subtype(PROP_DISTANCE)
      .description("Size of each leaf")
      .supports_field();
  b.add_input<decl::Float>("Size Random")
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("How much the size differs from leaf to leaf")
      .supports_field();
  b.add_input<decl::Bool>("Tip Leaf").default_value(true).description(
      "One more at the tip of every stem that doesn't carry on");
  b.add_input<decl::Menu>("Cards")
      .default_value(MenuValue(0))
      .static_items(cards_items)
      .description("One card per leaf, or two crossed");

  PanelDeclarationBuilder &normals = b.add_panel("Normals").default_closed(true);
  normals.add_input<decl::Menu>("Mode")
      .default_value(MenuValue(1))
      .static_items(normal_items)
      .description("Which way the normals point, for soft lighting of the crown");
  normals.add_input<decl::Float>("Blend")
      .default_value(0.7f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("How far the normals turn from the cards toward the mode")
      .supports_field();
  normals.add_input<decl::Float>("Face Up")
      .default_value(0.6f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("How much the leaves turn to face the sky")
      .supports_field();

  PanelDeclarationBuilder &clumps = b.add_panel("Clumps").default_closed(true);
  clumps.add_input<decl::Bool>("Use Clumps").default_value(false).description(
      "Hidden clumps along each parent, the leaves sit on their merged surface");
  clumps.add_input<decl::Int>("Clumps").default_value(3).min(1).max(64).description(
      "Clumps on each parent, from Start to the tip");
  clumps.add_input<decl::Bool>("Show Clumps").default_value(false).description(
      "The clumps are smooth blobs in the mesh, with the material of the leaves");
  clumps.add_input<decl::Float>("Clump Size")
      .default_value(0.8f)
      .min(0.001f)
      .subtype(PROP_DISTANCE)
      .description("Radius of each clump, link a Random Value for clumps of different sizes")
      .supports_field();
  clumps.add_input<decl::Float>("Squash")
      .default_value(0.8f)
      .min(0.05f)
      .max(4.0f)
      .description("Height of a clump as a share of its width");
  clumps.add_input<decl::Float>("Merge")
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("How softly touching clumps run into each other, for the Clumps normals");

  PanelDeclarationBuilder &source = b.add_panel("Source").default_closed(true);
  source.add_input<decl::Menu>("Shape")
      .default_value(MenuValue(0))
      .static_items(shape_items)
      .description("What each leaf is");
  source.add_input<decl::Object>("Object").description(
      "Mesh: the mesh, with +Y along the leaf and +Z the side it faces");
  source.add_input<decl::Collection>("Collection")
      .description("Mesh: a mesh picked from these for each leaf, by their Weight property");
  source.add_input<decl::Int>("Material Index").default_value(1).min(0).max(32766).description(
      "Material slot of the leaves, counted from 0");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA * /*ptr*/)
{
  layout.menu("NODE_MT_tree_graph_node_presets", IFACE_("Presets"), ICON_PRESET);
}

static void node_register()
{
  static bke::bNodeType ntype;

  tree_graph_node_type_base(&ntype, "TreeGraphNodeLeaves");
  ntype.ui_name = "Leaves";
  ntype.ui_description = "Leaves along their parent";
  ntype.enum_name_legacy = "TREE_GRAPH_LEAVES";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_tree_graph_leaves_cc
