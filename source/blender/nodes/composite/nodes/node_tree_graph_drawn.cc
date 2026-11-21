/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Drawn: branches along the splines of a curve object. A spline that starts on another grows
 * from it, the others grow from the nearest parent, or stand as trunks.
 */

#include "BLT_translation.hh"

#include "NOD_socket_declarations.hh"
#include "NOD_socket_declarations_geometry.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_tree_graph_drawn_cc {

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

  b.add_output<decl::Geometry>("Drawn");
  b.add_input<decl::Geometry>("Children").multi_input().description(
      "Branches and leaves that grow on these");
  b.add_input<decl::Geometry>("Surface").multi_input().description("A Bark node");
  b.add_input<decl::Menu>("Skin")
      .default_value(MenuValue(0))
      .static_items(skin_items)
      .description("A mesh, or only a hidden line for others to grow along");

  b.add_input<decl::Object>("Curve").description(
      "The curve object, its splines' radius sets the thickness");
  b.add_input<decl::Float>("Radius")
      .default_value(0.1f)
      .min(0.0f)
      .subtype(PROP_DISTANCE)
      .description("Radius at a spline radius of 1")
      .supports_field();
  b.add_input<decl::Float>("Taper")
      .default_value(0.8f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Share of the radius lost up to the tip")
      .supports_field();
  b.add_input<decl::Float>("Snap")
      .default_value(0.1f)
      .min(0.0f)
      .subtype(PROP_DISTANCE)
      .description("How close a spline's start must be to another to grow from it");

  PanelDeclarationBuilder &weld = b.add_panel("Weld").default_closed(true);
  weld.add_input<decl::Float>("Weld")
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("How much the base spreads out over the parent, 0 for none")
      .supports_field();
  weld.add_input<decl::Float>("Weld Length")
      .default_value(1.0f)
      .min(0.0f)
      .max(4.0f)
      .description("How far up the branch the weld goes, as a share of its radius")
      .supports_field();
  weld.add_input<decl::Int>("Weld Rings").default_value(6).min(2).max(32).description(
      "Rings across the weld, more for a smoother curve onto the parent");

  PanelDeclarationBuilder &normals = b.add_panel("Normals").default_closed(true);
  normals.add_input<decl::Bool>("From Parent").default_value(true).description(
      "The base takes the normals of the parent's bark, so the joint shades smoothly");
  normals.add_input<decl::Float>("Blend Length")
      .default_value(1.0f)
      .min(0.0f)
      .max(8.0f)
      .description("How far up the branch the normals turn to its own, as a share of its radius")
      .supports_field();
  normals.add_input<decl::Bool>("Smooth").default_value(true).description(
      "Smooth shading, flat faces when off");

  PanelDeclarationBuilder &mesh = b.add_panel("Mesh").default_closed(true);
  mesh.add_input<decl::Int>("Sections").default_value(16).min(2).max(256).description(
      "Sections along each spline");
  mesh.add_input<decl::Int>("Sides").default_value(8).min(3).max(64).description(
      "Sides around the thickest spline, thinner ones get fewer");
  mesh.add_input<decl::Int>("U Tiles").default_value(1).min(1).max(16).description(
      "Times the bark texture goes around");
  mesh.add_input<decl::Float>("V Scale").default_value(1.0f).min(0.01f).max(16.0f).description(
      "How many times the bark texture repeats along, 1 keeps it square");
  mesh.add_input<decl::Int>("Material Index").default_value(0).min(0).max(32766).description(
      "Material slot of the branches, counted from 0");
}

static void node_register()
{
  static bke::bNodeType ntype;

  tree_graph_node_type_base(&ntype, "TreeGraphNodeDrawn");
  ntype.ui_name = "Drawn";
  ntype.ui_description = "Branches along the splines of a curve object";
  ntype.enum_name_legacy = "TREE_GRAPH_DRAWN";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_tree_graph_drawn_cc
