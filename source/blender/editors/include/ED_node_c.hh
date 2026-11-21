/* SPDX-FileCopyrightText: 2009 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 */

#pragma once

#include <string>

#include "BLI_map.hh"
#include "BLI_timeit.hh"

#include "DNA_node_types.h"

namespace blender {

struct ARegion;
struct ID;
struct Main;
struct Scene;
struct SpaceNode;
struct Tex;
struct View2D;
struct bContext;
struct bNode;
struct bNodeTree;
namespace bke {
struct bNodeTreeType;
struct bNodeType;
struct bNodeSocketType;
}  // namespace bke

#define NODE_GRID_STEP_SIZE (20.0f * UI_SCALE_FAC) /* Based on the grid nodes snap to. */
#define NODE_EDGE_PAN_INSIDE_PAD 2
#define NODE_EDGE_PAN_OUTSIDE_PAD 0 /* Disable clamping for node panning, use whole screen. */
#define NODE_EDGE_PAN_SPEED_RAMP 1
#define NODE_EDGE_PAN_MAX_SPEED 26 /* In UI units per second, slower than default. */
#define NODE_EDGE_PAN_DELAY 0.5f
#define NODE_EDGE_PAN_ZOOM_INFLUENCE 0.5f

/* `space_node.cc` */

void ED_node_cursor_location_get(const SpaceNode *snode, float value[2]);
void ED_node_cursor_location_set(SpaceNode *snode, const float value[2]);

int ED_node_tree_path_length(SpaceNode *snode);
/**
 * \param value: The path output at least the size of `ED_node_tree_path_length(snode) + 1`.
 */
void ED_node_tree_path_get(SpaceNode *snode, char *value);

void ED_node_tree_start(ARegion *region, SpaceNode *snode, bNodeTree *ntree, ID *id, ID *from);
void ED_node_tree_push(ARegion *region, SpaceNode *snode, bNodeTree *ntree, bNode *gnode);
void ED_node_tree_pop(ARegion *region, SpaceNode *snode);
int ED_node_tree_depth(SpaceNode *snode);
bNodeTree *ED_node_tree_get(SpaceNode *snode, int level);

void ED_node_set_active_viewer_key(SpaceNode *snode);

/* `drawnode.cc` */

void ED_node_init_butfuncs();
void ED_init_custom_node_type(bke::bNodeType *ntype);
void ED_init_custom_node_socket_type(bke::bNodeSocketType *stype);
void ED_init_standard_node_socket_type(bke::bNodeSocketType *stype);
void ED_init_node_socket_type_virtual(bke::bNodeSocketType *stype);
void ED_node_sample_set(const float col[4]);
void ED_node_type_draw_color(const char *idname, float *r_color);

/* `node_edit.cc` */

void ED_node_set_tree_type(SpaceNode *snode, bke::bNodeTreeType *typeinfo);
bool ED_node_is_compositor(const SpaceNode *snode);
bool ED_node_is_shader(SpaceNode *snode);
bool ED_node_is_texture(SpaceNode *snode);
bool ED_node_is_geometry(const SpaceNode *snode);
bool ED_node_is_texture_graph(const SpaceNode *snode);
bool ED_node_is_tree_graph(const SpaceNode *snode);
bool ED_node_supports_preview(SpaceNode *snode);

/**
 * Assumes nothing being done in ntree yet, sets the default in/out node.
 * Called from shading buttons or header.
 */
void ED_node_texture_default(const bContext *C, Tex *tex);
void ED_node_post_apply_transform(bContext *C, bNodeTree *ntree);
void ED_node_set_active(
    Main *bmain, SpaceNode *snode, bNodeTree *ntree, bNode *node, bool *r_active_texture_changed);

/**
 * Launch a compositor job for the scene of the given context.
 */
void ED_node_compositor_job(const bContext *C);

/* `node_texture_graph.cc` */

/**
 * Compute a Texture Graph on the CPU and write each output of the graph into its image
 * data-block, creating the images when needed. With \a with_previews, the node thumbnails are
 * refreshed too.
 */
void ED_texture_graph_evaluate(Main *bmain, Scene *scene, bNodeTree *ntree, bool with_previews);
/**
 * Compute a Texture Graph in the background and write its images when done. A new call while
 * one runs stops it and starts again with the graph as it is then.
 */
void ED_texture_graph_job(const bContext *C, bNodeTree *ntree);
/** How long each node of a Texture Graph took the last time it ran, null when it has not run. */
const Map<bNodeInstanceKey, timeit::Nanoseconds> *ED_texture_graph_node_times(
    const bNodeTree *ntree);
/** A hash of everything that changes what a Texture Graph makes, but not where its nodes are. */
uint64_t ED_texture_graph_signature(const bNodeTree *ntree);
/** Forget the node results kept between runs of the Texture Graphs. */
void ED_texture_graph_clear_cache();

/** The image a Texture Graph writes for the output with the given name. */
std::string ED_texture_graph_image_name(const bNodeTree *ntree, const char *output_name);

/* `node_ops.cc` */

void ED_operatormacros_node();

/* `node_view.cc` */

/**
 * Returns mouse position in image space.
 */
bool ED_space_node_get_position(
    Main *bmain, SpaceNode *snode, ARegion *region, const int mval[2], float fpos[2]);
/**
 * Returns color in linear space, matching #ED_space_image_color_sample().
 * And here we've got recursion in the comments tips...
 */
bool ED_space_node_color_sample(
    Main *bmain, SpaceNode *snode, ARegion *region, const int mval[2], float r_col[3]);

}  // namespace blender
