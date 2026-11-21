/* SPDX-FileCopyrightText: 2006 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include <optional>

#include "BKE_node_runtime.hh"

#include "BLI_set.hh"

#include "NOD_socket_search_link.hh"

#include "node_composite_util.hh"

namespace blender {

/**
 * Compositor nodes that need a scene render, tracking data, masks or files. They make no sense in
 * a Texture Graph, which only generates images from its own nodes.
 */
static bool cmp_node_unsupported_in_texture_graph(const StringRef idname)
{
  static const Set<StringRef> unsupported = {
      "CompositorNodeRLayers",
      "CompositorNodeCryptomatte",
      "CompositorNodeCryptomatteV2",
      "CompositorNodeOutputFile",
      "CompositorNodeMovieClip",
      "CompositorNodeMovieDistortion",
      "CompositorNodeStabilize",
      "CompositorNodePlaneTrackDeform",
      "CompositorNodeTrackPos",
      "CompositorNodeKeyingScreen",
      "CompositorNodeMask",
      "CompositorNodeSequencerStripInfo",
      "CompositorNodeSwitchView",
      "CompositorNodeVecBlur",
      "CompositorNodeDefocus",
      "CompositorNodeZcombine",
      "CompositorNodeIDMask",
      "CompositorNodeLevels",
      "CompositorNodeConvertToDisplay",
  };
  return unsupported.contains(idname);
}

bool cmp_node_poll_default(const bke::bNodeType *ntype,
                           const bNodeTree *ntree,
                           const char **r_disabled_hint)
{
  if (STREQ(ntree->idname, "TextureGraphNodeTree")) {
    if (cmp_node_unsupported_in_texture_graph(ntype->idname)) {
      *r_disabled_hint = RPT_("Not available in a Texture Graph");
      return false;
    }
    return true;
  }
  if (!STREQ(ntree->idname, "CompositorNodeTree")) {
    *r_disabled_hint = RPT_("Not a compositor node tree");
    return false;
  }
  return true;
}

bool tg_node_poll_default(const bke::bNodeType * /*ntype*/,
                          const bNodeTree *ntree,
                          const char **r_disabled_hint)
{
  if (!STREQ(ntree->idname, "TextureGraphNodeTree")) {
    *r_disabled_hint = RPT_("Not a Texture Graph");
    return false;
  }
  return true;
}

bool tree_graph_node_poll_default(const bke::bNodeType * /*ntype*/,
                                  const bNodeTree *ntree,
                                  const char **r_disabled_hint)
{
  if (!STREQ(ntree->idname, "TreeGraphNodeTree")) {
    *r_disabled_hint = RPT_("Not a Tree Graph");
    return false;
  }
  return true;
}

void tree_graph_node_type_base(bke::bNodeType *ntype, std::string idname)
{
  bke::node_type_base(*ntype, idname, std::nullopt);

  ntype->poll = tree_graph_node_poll_default;
  ntype->insert_link = node_insert_link_default;
  ntype->gather_link_search_ops = nodes::search_link_ops_for_basic_node;
}

void tg_node_type_base(bke::bNodeType *ntype, std::string idname)
{
  bke::node_type_base(*ntype, idname, std::nullopt);

  ntype->poll = tg_node_poll_default;
  ntype->insert_link = node_insert_link_default;
  ntype->gather_link_search_ops = nodes::search_link_ops_for_basic_node;
}

void cmp_node_type_base(bke::bNodeType *ntype,
                        std::string idname,
                        const std::optional<int16_t> legacy_type)
{
  bke::node_type_base(*ntype, idname, legacy_type);

  ntype->poll = cmp_node_poll_default;
  ntype->insert_link = node_insert_link_default;
  ntype->gather_link_search_ops = nodes::search_link_ops_for_basic_node;
}

}  // namespace blender
