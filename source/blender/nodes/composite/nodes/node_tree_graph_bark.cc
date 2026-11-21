/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Bark: the shape of the surface of trunks, branches and roots: ridges, lumps, displacement by
 * noise or an image, and buttresses at the foot.
 */

#include "BLT_translation.hh"

#include "NOD_socket_declarations.hh"
#include "NOD_socket_declarations_geometry.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_tree_graph_bark_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();

  b.add_output<decl::Geometry>("Bark").description(
      "Link into the Surface of a Trunk, Branches, Roots or Drawn node");

  b.add_input<decl::Int>("Flutes").default_value(0).min(0).max(32).description(
      "Ridges around the stem, 0 for a round stem. Give it enough sides to show them");
  b.add_input<decl::Float>("Flute Depth")
      .default_value(0.25f)
      .min(0.0f)
      .max(0.9f)
      .subtype(PROP_FACTOR)
      .description("How deep the grooves between the ridges are, as a share of the radius")
      .supports_field();
  b.add_input<decl::Float>("Bumps")
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Lumps on the bark, as a share of the radius")
      .supports_field();
  b.add_input<decl::Float>("Bump Size")
      .default_value(0.3f)
      .min(0.001f)
      .subtype(PROP_DISTANCE)
      .description("Size of the lumps")
      .supports_field();
  b.add_input<decl::Float>("Displace")
      .default_value(0.0f)
      .subtype(PROP_DISTANCE)
      .description("How far the noise or image moves the surface out")
      .supports_field();
  b.add_input<decl::Float>("Scale")
      .default_value(1.0f)
      .min(0.001f)
      .description("Size of the noise")
      .supports_field();
  b.add_input<decl::Image>("Image").description(
      "Displaces by the gray of this image instead of noise, white the most");

  PanelDeclarationBuilder &flares = b.add_panel("Flares").default_closed(true);
  flares.add_input<decl::Int>("Count").default_value(0).min(0).max(16).description(
      "Buttresses at the foot of trunks");
  flares.add_input<decl::Float>("Reach")
      .default_value(0.5f)
      .min(0.0f)
      .description("How far out they reach, as a share of the radius")
      .supports_field();
  flares.add_input<decl::Float>("Height")
      .default_value(0.15f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("How far up they go, as a share of the trunk's length")
      .supports_field();
}

static void node_register()
{
  static bke::bNodeType ntype;

  tree_graph_node_type_base(&ntype, "TreeGraphNodeBark");
  ntype.ui_name = "Bark";
  ntype.ui_description = "Shapes the surface of stems";
  ntype.enum_name_legacy = "TREE_GRAPH_BARK";
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_tree_graph_bark_cc
