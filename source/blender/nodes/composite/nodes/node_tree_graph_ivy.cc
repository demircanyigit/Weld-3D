/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Ivy: thin vines that wind around the bark of the stems they are linked into as they climb, the
 * end of each hanging free. Link Leaves into its Children for the ivy leaves.
 */

#include "BLT_translation.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "NOD_socket_declarations.hh"
#include "NOD_socket_declarations_geometry.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_tree_graph_ivy_cc {

static const EnumPropertyItem skin_items[] = {
    {0, "MESH", 0, N_("Mesh"), N_("A thin tube")},
    {1, "SPINE", 0, N_("Spine Only"), N_("No mesh, only a line for its leaves to grow along")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();

  b.add_output<decl::Geometry>("Ivy");
  b.add_input<decl::Geometry>("Children").multi_input().description(
      "Leaves and branches that grow on the vines");
  b.add_input<decl::Geometry>("Forces").multi_input().description(
      "Crown and Prune nodes for these vines");
  b.add_input<decl::Menu>("Skin")
      .default_value(MenuValue(0))
      .static_items(skin_items)
      .description("A mesh, or only a hidden line for the leaves to grow along");

  b.add_input<decl::Int>("Count")
      .default_value(3)
      .min(0)
      .max(64)
      .description("Vines on each parent")
      .supports_field();
  b.add_input<decl::Float>("Start")
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Where on the parent the vines begin")
      .supports_field();
  b.add_input<decl::Float>("End")
      .default_value(0.8f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("How high on the parent they climb before they hang free or stop")
      .supports_field();
  b.add_input<decl::Float>("Length")
      .default_value(6.0f)
      .min(0.0f)
      .subtype(PROP_DISTANCE)
      .description("Length of each vine")
      .supports_field();
  b.add_input<decl::Float>("Radius")
      .default_value(0.015f)
      .min(0.0f)
      .subtype(PROP_DISTANCE)
      .description("Thickness of the vines at their base")
      .supports_field();
  b.add_input<decl::Float>("Taper")
      .default_value(0.6f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Share of the radius lost up to the tip")
      .supports_field();
  b.add_input<decl::Float>("Climb")
      .default_value(1.1f)
      .min(-1.5f)
      .max(1.5f)
      .subtype(PROP_ANGLE)
      .description("Angle up from the ring around the parent, less winds the vines more closely")
      .supports_field();
  b.add_input<decl::Float>("Wiggle")
      .default_value(0.4f)
      .min(0.0f)
      .max(1.5f)
      .subtype(PROP_ANGLE)
      .description("How much the climb wanders up and down")
      .supports_field();
  b.add_input<decl::Float>("Hang")
      .default_value(0.2f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Share of each vine at its end that leaves the bark and hangs down")
      .supports_field();
  b.add_input<decl::Float>("Gap")
      .default_value(0.005f)
      .min(0.0f)
      .subtype(PROP_DISTANCE)
      .description("Space between the vines and the bark");

  PanelDeclarationBuilder &mesh = b.add_panel("Mesh").default_closed(true);
  mesh.add_input<decl::Int>("Sections").default_value(48).min(2).max(512).description(
      "Sections along each vine");
  mesh.add_input<decl::Int>("Sides").default_value(4).min(0).max(32).description(
      "Sides around each vine, 0 for fewer on thinner ones");
  mesh.add_input<decl::Int>("Material Index").default_value(0).min(0).max(32766).description(
      "Material slot of the vines, counted from 0");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA * /*ptr*/)
{
  layout.menu("NODE_MT_tree_graph_node_presets", IFACE_("Presets"), ICON_PRESET);
}

static void node_register()
{
  static bke::bNodeType ntype;

  tree_graph_node_type_base(&ntype, "TreeGraphNodeIvy");
  ntype.ui_name = "Ivy";
  ntype.ui_description = "Vines that wind around the bark of their parent as they climb";
  ntype.enum_name_legacy = "TREE_GRAPH_IVY";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_tree_graph_ivy_cc
