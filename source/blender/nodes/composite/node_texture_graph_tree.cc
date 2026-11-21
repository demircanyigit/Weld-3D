/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Texture Graph node tree type: node graphs that generate texture images. They are evaluated by
 * the compositor engine, so they can use the compositor's image nodes and the shared texture,
 * color and math nodes. Each output of the graph becomes an image data-block.
 */

#include "DNA_node_types.h"

#include "BKE_node.hh"

#include "BLT_translation.hh"

#include "NOD_common.hh"
#include "NOD_composite.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "UI_resources.hh"

#include "node_common.h"
#include "node_composite_util.hh"
#include "node_util.hh"

namespace blender {

static void texture_graph_foreach_nodeclass(void *calldata, bke::bNodeClassCallback func)
{
  func(calldata, NODE_CLASS_INPUT, N_("Input"));
  func(calldata, NODE_CLASS_OUTPUT, N_("Output"));
  func(calldata, NODE_CLASS_OP_COLOR, N_("Color"));
  func(calldata, NODE_CLASS_OP_VECTOR, N_("Vector"));
  func(calldata, NODE_CLASS_OP_FILTER, N_("Filter"));
  func(calldata, NODE_CLASS_CONVERTER, N_("Converter"));
  func(calldata, NODE_CLASS_DISTORT, N_("Distort"));
  func(calldata, NODE_CLASS_TEXTURE, N_("Texture"));
  func(calldata, NODE_CLASS_GROUP, N_("Group"));
  func(calldata, NODE_CLASS_INTERFACE, N_("Interface"));
  func(calldata, NODE_CLASS_LAYOUT, N_("Layout"));
}

static void texture_graph_update(bNodeTree *ntree)
{
  bke::node_tree_set_output(*ntree);
  ntree_update_reroute_nodes(ntree);
}

static bool texture_graph_socket_type_valid(bke::bNodeTreeType * /*ntreetype*/,
                                            bke::bNodeSocketType *socket_type)
{
  return bke::node_is_static_socket_type(*socket_type) && ELEM(socket_type->type,
                                                               SOCK_FLOAT,
                                                               SOCK_INT,
                                                               SOCK_BOOLEAN,
                                                               SOCK_VECTOR,
                                                               SOCK_RGBA,
                                                               SOCK_MENU,
                                                               SOCK_STRING);
}

/** Same rules as the compositor: grayscale, color and vector values convert into each other. */
static bool texture_graph_validate_link(eNodeSocketDatatype from_type, eNodeSocketDatatype to_type)
{
  if (ELEM(from_type, SOCK_FLOAT, SOCK_VECTOR, SOCK_RGBA, SOCK_BOOLEAN, SOCK_INT) &&
      ELEM(to_type, SOCK_FLOAT, SOCK_VECTOR, SOCK_RGBA, SOCK_BOOLEAN, SOCK_INT))
  {
    return true;
  }
  return from_type == to_type;
}

bke::bNodeTreeType *ntreeType_TextureGraph;

void register_node_tree_type_texture_graph()
{
  bke::bNodeTreeType *tt = ntreeType_TextureGraph = MEM_new<bke::bNodeTreeType>(__func__);

  tt->type = NTREE_TEXTURE_GRAPH;
  tt->idname = "TextureGraphNodeTree";
  tt->group_idname = "TextureGraphNodeGroup";
  tt->ui_name = N_("Texture Graph");
  tt->ui_icon = ICON_NODE_TEXTURE_GRAPH;
  tt->ui_description = N_("Build texture images from generator and filter nodes");

  tt->foreach_nodeclass = texture_graph_foreach_nodeclass;
  tt->update = texture_graph_update;
  /* No `get_from_context`: the edited graph is picked in the editor header and kept there. */
  tt->validate_link = texture_graph_validate_link;
  tt->valid_socket_type = texture_graph_socket_type_valid;

  tt->rna_ext.srna = RNA_TextureGraphNodeTree;

  bke::node_tree_type_add(*tt);
}

}  // namespace blender

namespace blender::nodes::node_texture_graph_group_cc {

static void node_register()
{
  static bke::bNodeType ntype;

  /* Like the other group nodes, this maps to the shared NODE_GROUP integer type. */
  bke::node_type_base_custom(
      ntype, "TextureGraphNodeGroup", "Group", "GROUP", NODE_CLASS_GROUP);
  ntype.enum_name_legacy = "GROUP";
  ntype.type_legacy = NODE_GROUP;
  ntype.poll = tg_node_poll_default;
  ntype.poll_instance = node_group_poll_instance;
  ntype.insert_link = node_insert_link_default;
  ntype.ui_class = node_group_ui_class;
  ntype.ui_description_fn = node_group_ui_description;
  ntype.rna_ext.srna = RNA_struct_find("TextureGraphNodeGroup");
  BLI_assert(ntype.rna_ext.srna != nullptr);
  RNA_struct_blender_type_set(ntype.rna_ext.srna, &ntype);

  bke::node_type_size(ntype, GROUP_NODE_DEFAULT_WIDTH, GROUP_NODE_MIN_WIDTH, GROUP_NODE_MAX_WIDTH);
  ntype.labelfunc = node_group_label;
  ntype.declare = nodes::node_group_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_group_cc
