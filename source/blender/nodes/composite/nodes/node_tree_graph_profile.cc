/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Profile: a value that changes along a curve, linked into a number on a tree level. The curve
 * goes from left to right either by where a branch sits on its parent, or along each branch from
 * its base to its tip. The number used is the Value times the curve.
 */

#include "BKE_colortools.hh"

#include "BLT_translation.hh"

#include "DNA_color_types.h"

#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "NOD_socket_declarations.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_tree_graph_profile_cc {

static const EnumPropertyItem along_items[] = {
    {0,
     "PARENT",
     0,
     N_("Parent"),
     N_("By where each branch or leaf sits on its parent, from start to end")},
    {1,
     "BRANCH",
     0,
     N_("Branch"),
     N_("Along each branch from its base to its tip, for Gnarl, Twist and forces")},
    {2, "HEIGHT", 0, N_("Height"), N_("By height, from the foot of the tree up to Top")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Float>("Value").description(
      "Link into a number on a Trunk, Branches, Roots, Leaves or Drawn node");

  b.add_input<decl::Menu>("Along")
      .default_value(MenuValue(0))
      .static_items(along_items)
      .description("What the curve goes along, from left to right");
  b.add_input<decl::Float>("Min").default_value(0.0f).description(
      "The number where the curve is at the bottom");
  b.add_input<decl::Float>("Max").default_value(1.0f).description(
      "The number where the curve is at the top");
  b.add_input<decl::Float>("Top")
      .default_value(10.0f)
      .min(0.01f)
      .subtype(PROP_DISTANCE)
      .description("Height: the height at the right end of the curve");
}

static void node_init(bNodeTree * /*ntree*/, bNode *node)
{
  /* Full at the start, falling off toward the end. */
  CurveMapping *cumap = BKE_curvemapping_add(1, 0.0f, 0.0f, 1.0f, 1.0f);
  CurveMap &cuma = cumap->cm[0];
  cuma.curve[0].x = 0.0f;
  cuma.curve[0].y = 1.0f;
  cuma.curve[1].x = 1.0f;
  cuma.curve[1].y = 0.3f;
  BKE_curvemapping_init(cumap);
  node->storage = cumap;
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  ui::template_curve_mapping(&layout, ptr, "mapping", 0, false, false, false, false, false);
}

/* The curve points are saved with the curve, the generic code only saves the curve itself. */
static void node_blend_write(const bNodeTree & /*tree*/, const bNode &node, BlendWriter &writer)
{
  BKE_curvemapping_curves_blend_write(&writer, static_cast<const CurveMapping *>(node.storage));
}

static void node_blend_read(bNodeTree & /*tree*/, bNode &node, BlendDataReader &reader)
{
  BKE_curvemapping_blend_read(&reader, static_cast<CurveMapping *>(node.storage));
}

static void node_register()
{
  static bke::bNodeType ntype;

  tree_graph_node_type_base(&ntype, "TreeGraphNodeProfile");
  ntype.ui_name = "Profile";
  ntype.ui_description = "A number that changes along a curve, for any number on a tree level";
  ntype.enum_name_legacy = "TREE_GRAPH_PROFILE";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  ntype.draw_buttons = node_layout;
  bke::node_type_size_preset(ntype, bke::eNodeSizePreset::Large);
  bke::node_type_storage(ntype, "CurveMapping", node_free_curves, node_copy_curves);
  ntype.blend_write_storage_content = node_blend_write;
  ntype.blend_data_read_storage_content = node_blend_read;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_tree_graph_profile_cc
