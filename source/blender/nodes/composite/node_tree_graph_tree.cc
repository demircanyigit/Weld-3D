/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Tree Graph node tree type: node graphs that describe a tree. Every node is one level of the
 * tree (trunk, branches, leaves, fronds, roots), and a link means "grows on". Forces and
 * shapes link into the levels they bend, profiles into the numbers they change. The graph is not evaluated
 * here: the Tree modifier reads it and grows the mesh.
 */

#include "BLI_utildefines.h"

#include "DNA_node_types.h"

#include "BKE_node.hh"

#include "BLT_translation.hh"

#include "NOD_composite.hh"

#include "RNA_prototypes.hh"

#include "UI_resources.hh"

#include "node_common.h"
#include "node_util.hh"

namespace blender {

static void tree_graph_foreach_nodeclass(void *calldata, bke::bNodeClassCallback func)
{
  func(calldata, NODE_CLASS_INPUT, N_("Input"));
  func(calldata, NODE_CLASS_OUTPUT, N_("Output"));
  func(calldata, NODE_CLASS_GEOMETRY, N_("Tree"));
  func(calldata, NODE_CLASS_OP_VECTOR, N_("Forces"));
  func(calldata, NODE_CLASS_CONVERTER, N_("Utilities"));
  func(calldata, NODE_CLASS_TEXTURE, N_("Texture"));
  func(calldata, NODE_CLASS_LAYOUT, N_("Layout"));
}

static void tree_graph_update(bNodeTree *ntree)
{
  bke::node_tree_set_output(*ntree);
  ntree_update_reroute_nodes(ntree);
}

static bool is_value_socket(const eNodeSocketDatatype type)
{
  return ELEM(type, SOCK_FLOAT, SOCK_INT, SOCK_BOOLEAN, SOCK_VECTOR, SOCK_RGBA);
}

/** Inputs of the graph are numbers set on the Tree modifier, one set for every object. */
static bool tree_graph_socket_type_valid(bke::bNodeTreeType * /*ntreetype*/,
                                         bke::bNodeSocketType *socket_type)
{
  if (!bke::node_is_static_socket_type(*socket_type)) {
    return false;
  }
  return socket_type->type == SOCK_GEOMETRY || is_value_socket(socket_type->type);
}

/** Levels, forces and shapes link to levels, value nodes link to the numbers on them. */
static bool tree_graph_validate_link(eNodeSocketDatatype from_type, eNodeSocketDatatype to_type)
{
  if (from_type == SOCK_GEOMETRY || to_type == SOCK_GEOMETRY) {
    return from_type == to_type;
  }
  if (from_type == SOCK_OBJECT || to_type == SOCK_OBJECT) {
    return from_type == to_type;
  }
  return is_value_socket(from_type) && is_value_socket(to_type);
}

bke::bNodeTreeType *ntreeType_TreeGraph;

void register_node_tree_type_tree_graph()
{
  bke::bNodeTreeType *tt = ntreeType_TreeGraph = MEM_new<bke::bNodeTreeType>(__func__);

  tt->type = NTREE_TREE_GRAPH;
  tt->idname = "TreeGraphNodeTree";
  tt->ui_name = N_("Tree Graph");
  tt->ui_icon = ICON_NODE_TREE_GRAPH;
  tt->ui_description = N_("Describe a tree level by level, for the Tree modifier");

  tt->foreach_nodeclass = tree_graph_foreach_nodeclass;
  tt->update = tree_graph_update;
  tt->validate_link = tree_graph_validate_link;
  tt->valid_socket_type = tree_graph_socket_type_valid;

  tt->rna_ext.srna = RNA_TreeGraphNodeTree;

  bke::node_tree_type_add(*tt);
}

}  // namespace blender
