/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup cmpnodes
 *
 * Fronds of a Tree Graph: ribbons that follow the line of their parent stem, for grass blades,
 * palm and fern leaves and reeds. Their parent is often a stem with Skin set to Spine Only.
 */

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "NOD_socket_declarations.hh"
#include "NOD_socket_declarations_geometry.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_tree_graph_fronds_cc {

/** Values by #NormalMode in the generator. */
static const EnumPropertyItem normal_items[] = {
    {0, "FLAT", 0, N_("Flat"), N_("The normals of the ribbons themselves")},
    {1, "SPHERICAL", 0, N_("Spherical"), N_("Out from the middle of the foliage")},
    {2, "CROWN", 0, N_("Crown"), N_("Out from an ellipsoid fitted around all the foliage")},
    {3, "UP", 0, N_("Up"), N_("Straight up, for grass and low plants")},
    {4, "CLUMPS", 0, N_("Clumps"), N_("Out of the merged leaf clumps of the tree")},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem tip_items[] = {
    {0, "POINTED", 0, N_("Pointed"), N_("The sides run straight to a point")},
    {1, "ROUND", 0, N_("Round"), N_("The sides close in a curve")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();

  b.add_output<decl::Geometry>("Fronds");
  b.add_input<decl::Geometry>("Forces").multi_input().description(
      "Crown and Prune nodes for these fronds");

  b.add_input<decl::Int>("Count").default_value(1).min(1).max(8).description(
      "Ribbons on each parent stem, turned evenly around it");
  b.add_input<decl::Float>("Start")
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Where on the parent the frond begins")
      .supports_field();
  b.add_input<decl::Float>("End")
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Where on the parent the frond ends")
      .supports_field();
  b.add_input<decl::Float>("Width")
      .default_value(0.1f)
      .min(0.0f)
      .subtype(PROP_DISTANCE)
      .description("Width across, link a Profile for a width that changes along the frond")
      .supports_field();
  b.add_input<decl::Float>("Width Random")
      .default_value(0.2f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("How much the width differs from frond to frond");
  b.add_input<decl::Float>("Tip")
      .default_value(0.3f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Share of the length that narrows to the tip, 0 for a square end");
  b.add_input<decl::Menu>("Tip Shape")
      .default_value(MenuValue(0))
      .static_items(tip_items)
      .description("How the sides close at the tip");

  PanelDeclarationBuilder &shape = b.add_panel("Shape").default_closed(true);
  shape.add_input<decl::Float>("Fold")
      .default_value(0.0f)
      .min(-1.5708f)
      .max(1.5708f)
      .subtype(PROP_ANGLE)
      .description("Crease down the middle, both halves turn up by this angle")
      .supports_field();
  shape.add_input<decl::Float>("Curl")
      .default_value(0.0f)
      .min(-3.14159f)
      .max(3.14159f)
      .subtype(PROP_ANGLE)
      .description("How much the sides roll in, across the whole width")
      .supports_field();
  shape.add_input<decl::Float>("Twist")
      .default_value(0.0f)
      .subtype(PROP_ANGLE)
      .description("Turn around the stem from the start to the tip");
  shape.add_input<decl::Float>("Roll Random")
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Random turn of each frond around its stem");

  PanelDeclarationBuilder &normals = b.add_panel("Normals").default_closed(true);
  normals.add_input<decl::Menu>("Mode")
      .default_value(MenuValue(3))
      .static_items(normal_items)
      .description("Which way the normals point, for soft lighting");
  normals.add_input<decl::Float>("Blend")
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("How far the normals turn from the ribbons toward the mode")
      .supports_field();

  PanelDeclarationBuilder &mesh = b.add_panel("Mesh").default_closed(true);
  mesh.add_input<decl::Int>("Sections").default_value(8).min(1).max(128).description(
      "Rows along each frond");
  mesh.add_input<decl::Int>("Across").default_value(2).min(1).max(16).description(
      "Columns across each frond, more for a smooth curl");
  mesh.add_input<decl::Int>("Material Index").default_value(1).min(0).max(32766).description(
      "Material slot of the fronds, counted from 0");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA * /*ptr*/)
{
  layout.menu("NODE_MT_tree_graph_node_presets", IFACE_("Presets"), ICON_PRESET);
}

static void node_register()
{
  static bke::bNodeType ntype;

  tree_graph_node_type_base(&ntype, "TreeGraphNodeFronds");
  ntype.ui_name = "Fronds";
  ntype.ui_description = "Ribbons along their parent stem, for grass, palm and fern leaves";
  ntype.enum_name_legacy = "TREE_GRAPH_FRONDS";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_tree_graph_fronds_cc
