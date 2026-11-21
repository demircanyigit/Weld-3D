/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Trunk: the stem a tree stands on, the first level of a Tree Graph.
 */

#include "BLT_translation.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "NOD_socket_declarations.hh"
#include "NOD_socket_declarations_geometry.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_tree_graph_trunk_cc {

static const EnumPropertyItem skin_items[] = {
    {0, "MESH", 0, N_("Mesh"), N_("A tube with bark")},
    {1,
     "SPINE",
     0,
     N_("Spine Only"),
     N_("No mesh, only a line for its branches, leaves and fronds to grow along")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();

  b.add_output<decl::Geometry>("Trunk");
  b.add_input<decl::Geometry>("Children").multi_input().description(
      "Branches, roots and leaves that grow on the trunk");
  b.add_input<decl::Geometry>("Forces").multi_input().description(
      "Force, Avoid, Crown and Prune nodes for the trunk");
  b.add_input<decl::Geometry>("Surface").multi_input().description("A Bark node");
  b.add_input<decl::Menu>("Skin")
      .default_value(MenuValue(0))
      .static_items(skin_items)
      .description("A mesh, or only a hidden line for others to grow along");

  b.add_input<decl::Float>("Length")
      .default_value(10.0f)
      .min(0.0f)
      .subtype(PROP_DISTANCE)
      .description("Height of the trunk")
      .supports_field();
  b.add_input<decl::Float>("Radius")
      .default_value(0.4f)
      .min(0.0f)
      .subtype(PROP_DISTANCE)
      .description("Radius at the foot")
      .supports_field();
  b.add_input<decl::Float>("Taper")
      .default_value(0.7f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Share of the radius lost up to the tip. Without branches it runs to a point")
      .supports_field();
  b.add_input<decl::Float>("Gnarl")
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .description("Random turn of each section, more where the wood is thin")
      .supports_field();
  b.add_input<decl::Float>("Twist")
      .default_value(0.0f)
      .subtype(PROP_ANGLE)
      .description("Turn of each section around the stem")
      .supports_field();
  b.add_input<decl::Int>("Count").default_value(1).min(1).max(64).description(
      "Stems from one foot");

  PanelDeclarationBuilder &normals = b.add_panel("Normals").default_closed(true);
  normals.add_input<decl::Bool>("Smooth").default_value(true).description(
      "Smooth shading, flat faces when off");

  PanelDeclarationBuilder &mesh = b.add_panel("Mesh").default_closed(true);
  mesh.add_input<decl::Int>("Sections").default_value(12).min(1).max(256).description(
      "Sections along the trunk");
  mesh.add_input<decl::Int>("Sides").default_value(10).min(3).max(64).description(
      "Sides around the trunk, thinner branches get fewer");
  mesh.add_input<decl::Int>("U Tiles").default_value(1).min(1).max(16).description(
      "Times the bark texture goes around");
  mesh.add_input<decl::Float>("V Scale").default_value(1.0f).min(0.01f).max(16.0f).description(
      "How many times the bark texture repeats along, 1 keeps it square");
  mesh.add_input<decl::Int>("Material Index").default_value(0).min(0).max(32766).description(
      "Material slot of the trunk, counted from 0");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA * /*ptr*/)
{
  layout.menu("NODE_MT_tree_graph_node_presets", IFACE_("Presets"), ICON_PRESET);
}

static void node_register()
{
  static bke::bNodeType ntype;

  tree_graph_node_type_base(&ntype, "TreeGraphNodeTrunk");
  ntype.ui_name = "Trunk";
  ntype.ui_description = "The stem a tree stands on";
  ntype.enum_name_legacy = "TREE_GRAPH_TRUNK";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_tree_graph_trunk_cc
