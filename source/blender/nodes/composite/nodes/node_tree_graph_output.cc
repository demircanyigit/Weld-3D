/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Tree Output: the end of a Tree Graph, the trunks linked into it are what the Tree modifier
 * grows.
 */

#include "BLT_translation.hh"

#include "NOD_socket_declarations.hh"
#include "NOD_socket_declarations_geometry.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_tree_graph_output_cc {

/** By the order of #VertexValue in the generator. */
static const EnumPropertyItem value_items[] = {
    {0, "NONE", 0, N_("None"), N_("Nothing")},
    {1, "BRANCH_RANDOM", 0, N_("Branch Random"), N_("0 to 1, the same for a whole branch")},
    {2, "LEAF_RANDOM", 0, N_("Leaf Random"), N_("0 to 1, the same for a whole leaf")},
    {3, "LEVEL", 0, N_("Level"), N_("Trunk, branches, leaves, roots or fronds")},
    {4, "DEPTH", 0, N_("Depth"), N_("Levels down from the trunk")},
    {5, "ALONG_BRANCH", 0, N_("Along Branch"), N_("0 at the base of a stem, 1 at its tip")},
    {6, "HEIGHT", 0, N_("Height"), N_("Above the foot of the tree")},
    {7, "RADIUS", 0, N_("Radius"), N_("Thickness of the stem")},
    {8, "OCCLUSION", 0, N_("Occlusion"), N_("How open to the sky, 1 for nothing in the way")},
    {9, "WELD", 0, N_("Weld"), N_("1 where a branch grows out of its parent")},
    {10, "LEAF_FACING", 0, N_("Leaf Facing"), N_("1 where leaves face out of the crown")},
    {11, "CLUMP_RANDOM", 0, N_("Clump Random"), N_("0 to 1, the same for a whole leaf clump")},
    {12, "CLUMP_DEPTH", 0, N_("Clump Depth"), N_("How far inside the merged clumps, 0 on the outside")},
    {13, "CLUMP_EDGE", 0, N_("Clump Edge"), N_("1 at the sides of a clump, 0 at its top and bottom")},
    {14,
     "GRADIENT_HEIGHT",
     0,
     N_("Gradient Height"),
     N_("0 at the lowest leaf, 1 at the highest")},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem form_items[] = {
    {0,
     "BROADLEAF",
     0,
     N_("Broadleaf"),
     N_("Each stem carries on from the tip of its parent, only the last ones run to a point")},
    {1,
     "CONIFER",
     0,
     N_("Conifer"),
     N_("Every stem runs to a point, branches get shorter toward the top")},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem layout_items[] = {
    {0, "CUSTOM", 0, N_("Custom"), N_("Pick a value for each channel")},
    {1,
     "VARIATION",
     0,
     N_("Variation"),
     N_("Branch Random, Leaf Random, Along Branch and Height")},
    {2, "SHADING", 0, N_("Shading"), N_("Occlusion, Weld, Leaf Facing and Depth")},
    {3,
     "STYLIZED",
     0,
     N_("Stylized"),
     N_("Clump Depth, Clump Edge, Clump Random and Gradient Height")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();

  b.add_input<decl::Geometry>("Tree").multi_input().description(
      "The trunks of the tree, with everything that grows on them");
  b.add_input<decl::Menu>("Form")
      .default_value(MenuValue(0))
      .static_items(form_items)
      .description("How the stems grow on each other");
  b.add_input<decl::Int>("Seed").default_value(0).description("Another tree of the same kind");
  b.add_input<decl::Float>("Detail")
      .default_value(1.0f)
      .min(0.02f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Scales the sides, sections and leaves of the whole tree");

  PanelDeclarationBuilder &data = b.add_panel("Vertex Data").default_closed(true);
  for (const EnumPropertyItem *item = value_items + 1; item->identifier; item++) {
    data.add_input<decl::Bool>(item->name).default_value(false).description(item->description);
  }
  data.add_input<decl::Int>("Rays").default_value(16).min(1).max(256).description(
      "Occlusion: rays per vertex, more for smoother shading");

  PanelDeclarationBuilder &color = b.add_panel("Color").default_closed(true);
  color.add_input<decl::Bool>("Use Color").default_value(false).description(
      "Pack four values into the \"Tree Color\" attribute");
  color.add_input<decl::Menu>("Layout")
      .default_value(MenuValue(1))
      .static_items(layout_items)
      .description("Which values go into the color");
  for (const char *channel : {"Red", "Green", "Blue", "Alpha"}) {
    color.add_input<decl::Menu>(channel)
        .default_value(MenuValue(0))
        .static_items(value_items)
        .description("Custom: the value in this channel");
  }
}

static void node_register()
{
  static bke::bNodeType ntype;

  tree_graph_node_type_base(&ntype, "TreeGraphNodeOutput");
  ntype.ui_name = "Tree Output";
  ntype.ui_description = "The tree the Tree modifier grows";
  ntype.enum_name_legacy = "TREE_GRAPH_OUTPUT";
  ntype.nclass = NODE_CLASS_OUTPUT;
  ntype.declare = node_declare;
  ntype.no_muting = true;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_tree_graph_output_cc
