/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup geo
 *
 * Grows trees for games: tubes for the trunk and branches with bark UVs, flat cards for the leaves,
 * fronds and the smallest twigs, all in one mesh that keeps under a triangle budget.
 */

#include <array>
#include <functional>

#include "BLI_array.hh"
#include "BLI_function_ref.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

namespace blender {
struct Mesh;
}

namespace blender::geometry::tree {

enum class LevelType {
  Trunk,
  Branches,
  Leaves,
  Roots,
  Fronds,
  /** Branches along drawn curves. */
  Drawn,
  /** Vines that wind around the bark of their parent. */
  Ivy,
};

/** One drawn curve, in the space of the tree object, with its radius at each point. */
struct DrawnSpline {
  Vector<float3> points;
  Vector<float> radii;
};

/** Which way the normals of leaves and fronds point. */
enum class NormalMode {
  Flat = 0,
  Spherical = 1,
  Crown = 2,
  Up = 3,
  /** Out of the merged shape of the hidden clumps, so each clump lights like one soft ball. */
  Clumps = 4,
};

/** How many children grow on each parent. */
enum class CountMode {
  /** The same number on every parent. */
  Fixed = 0,
  /** A number per unit of the parent's length, longer parents get more. */
  ByLength = 1,
  /** One every so far along the parent. */
  Interval = 2,
};

/** How children are set around and along their parent. */
enum class Arrangement {
  /** One at a time, each turned by the spiral angle from the last. */
  Spiral = 0,
  /** One at a time, on opposite sides by turns. */
  Alternate = 1,
  /** Two at a time across from each other, each pair turned by the spiral angle. */
  Opposite = 2,
  /** Several at a time evenly around, each ring turned by the spiral angle. */
  Whorled = 3,
  /** One at a time, turned any way. */
  Random = 4,
};

enum class LeafShape {
  Leaf = 0,
  Square = 1,
  Mesh = 2,
};

/** What the nodes linked into a number can read about the branch, leaf or frond it is for. */
struct FieldContext {
  /** Where it sits on its parent, from the first to the last of its level (0 to 1). */
  float along_parent = 0.0f;
  /** Place along the branch itself, from its base to its tip (0 to 1). */
  float along_branch = 0.0f;
  /** Different for every branch, leaf and frond, the same every time it grows. */
  uint32_t id = 0;
  /** Random from 0 to 1, made from the id. */
  float random = 0.0f;
  /** The point being grown, in the space of the tree object. */
  float3 position = float3(0.0f);
  /** Where the tree stands, in the space of the tree object. */
  float3 origin = float3(0.0f);
  /** Length and radius of the parent, before the tree's scale. 0 for trunks. */
  float parent_length = 0.0f;
  float parent_radius = 0.0f;
  /** Levels down from the trunk, the trunk is 0. */
  int depth = 0;
  /** Number of this one among the children of its parent, and how many there are. */
  int index = 0;
  int count = 1;
  /** Turn around the parent from its level side. */
  float around = 0.0f;
  /** The tree's scale, to give lengths before it. */
  float scale = 1.0f;
};

/**
 * A number on a node: a fixed value, or what the nodes linked into it work out for every branch,
 * leaf and place along a branch.
 */
struct Param {
  float value = 0.0f;
  std::function<float(const FieldContext &)> fn;

  Param() = default;
  Param(const float value) : value(value) {}

  float at(const FieldContext &context) const
  {
    return fn ? fn(context) : value;
  }
};

enum class ForceType {
  /** Turns the stems toward a direction, away from it with a negative strength. */
  Direction,
  /** Turns the stems toward a point. */
  Toward,
  /** Turns the stems away from a point. */
  Away,
  /** Keeps the stems off a mesh. */
  Avoid,
};

/** Bends the stems of the levels it is linked into as they grow, thin wood more than thick. */
struct Force {
  ForceType type = ForceType::Direction;
  /** Turn per section, in radians for a stem as thick as a quarter of the tree's scale. */
  Param strength = 0.02f;
  /** Direction: the way to turn. */
  float3 direction = float3(0.0f, 0.0f, 1.0f);
  /** Toward and Away: the point, in the space of the tree. */
  float3 point = float3(0.0f);
  /** Avoid: how far from the mesh to keep. */
  Param distance = 0.3f;
  /** Avoid: lean toward open space as they grow, instead of only turning away close by. */
  bool lean = false;
  /** Lean: how far ahead the stems look, in the space of the tree. */
  float reach = 3.0f;
  /** Lean: rays per section, and how much open space up counts over open space to the side. */
  int rays = 8;
  float up_bias = 0.5f;
  /** Avoid: the mesh, and its place in the space of the tree. */
  const Mesh *mesh = nullptr;
  float4x4 mesh_transform = float4x4::identity();
};

enum class PruneMode {
  /** A share of them, any of them. */
  Random = 0,
  /** The ones growing down. */
  Down = 1,
  /** The ones too close to others of their level. */
  Crowded = 2,
};

/** Removes branches and leaves once they are grown. */
struct Prune {
  PruneMode mode = PruneMode::Random;
  Param amount = 0.3f;
  /** Crowded: the room each one keeps, before the tree's scale. */
  float spacing = 0.3f;
  int seed = 0;
};

/** Changes the surface of trunks, branches and roots once they are meshed. */
struct Displace {
  /** Noise, or the gray of #image. */
  bool use_image = false;
  /** Gray values of the image, row by row from the bottom, #image_size wide and high. */
  Array<float> image;
  int2 image_size = int2(0);
  /** Out from the middle, before the tree's scale. */
  Param radial = 0.05f;
  /** Stretches the noise along the branch, for ridges. */
  Param along = 0.0f;
  /** Size of the noise, before the tree's scale. */
  Param scale = 1.0f;
  int seed = 0;
  /** Small detail on top, as a share of #radial, and its size. */
  Param fine_amount = 0.75f;
  Param fine_scale = 1.0f;
  /** Big lumps, as a share of the radius, their twist around the stem and their size. */
  Param shape_amount = 0.0f;
  Param shape_twist = 0.0f;
  Param shape_scale = 2.0f;
  /** Buttresses at the foot of trunks. */
  int flare_count = 0;
  /** How far out they reach, as a share of the radius. */
  Param flare_radius = 0.5f;
  /** How far up they go, as a share of the trunk's length. */
  Param flare_height = 0.25f;
  /** How narrow they are, 0 wide and round, 1 thin blades. */
  Param flare_pinch = 0.5f;
  Param flare_angle = 0.0f;
};

/** One mesh leaves can be, and how often. */
struct LeafChoice {
  const Mesh *mesh = nullptr;
  float weight = 1.0f;
};

enum class ShapeType {
  Sphere = 0,
  Dome = 1,
  Cone = 2,
  Cylinder = 3,
};

/** Keeps the branches of the levels it is linked into inside a shape. */
struct CrownShape {
  ShapeType type = ShapeType::Sphere;
  /** Cut the branches where they leave the shape, or make them shorter so they end inside. */
  bool cut = true;
  /** From the space of the tree to the shape, where the shape fills -1 to 1. */
  float4x4 to_shape = float4x4::identity();
  /** Branches may reach this much further out, more or less at random. */
  float softness = 0.0f;
};

/** One level of the tree, one node of a Tree Graph. Distances are before the tree's scale. */
struct Level {
  LevelType type = LevelType::Branches;
  /** Changes the randomness of this level, the node's identifier. */
  int id = 0;
  /** Index of the object's material slot for the faces of this level. */
  int material = 0;
  /** Spines make no mesh, they are only lines for their children to grow along. */
  bool make_mesh = true;

  int count = 1;
  /** Children per parent when nodes are linked into the count, else #count. */
  Param count_field = 1.0f;
  CountMode count_mode = CountMode::Fixed;
  /** By length: children per unit of the parent's length, before the tree's scale. */
  Param density = 4.0f;
  /** Interval: length between children, before the tree's scale. */
  Param interval = 0.5f;
  Arrangement arrangement = Arrangement::Spiral;
  /** Whorled: children in each ring. */
  int per_node = 3;
  /** How far each child, or ring of children, moves along the parent from even spacing. */
  Param jitter = 0.3f;
  /** How far around the parent the children spread, the full turn is all the way around. */
  Param sweep = 2.0f * 3.14159265f;
  /** Share of the range at each end that gets no children. */
  Param clip = 0.0f;
  /** How far the base goes into the parent, as a share of the parent's radius. */
  Param sink = 0.0f;
  /** Scales length and thickness of the whole level. */
  Param size = 1.0f;
  /** Moves children toward the place their parent forks. */
  Param favor_forks = 0.0f;
  /** Branches: one more child that goes on from the parent's tip. */
  bool extend_parent = false;
  /** Children only grow where this is at least 0.5. */
  Param mask = 1.0f;
  /** Range on the parent the children grow along. */
  Param start = 0.0f;
  Param end = 1.0f;
  /** Height of the trunk, length of branches and roots, size of leaves. */
  Param length = 1.0f;
  /** Angle to the parent. */
  Param angle = 0.0f;
  /** Turn around the parent from one child to the next. */
  Param spiral = 2.39996f;
  /** Turn of the whole arrangement around the parent, from its level side. */
  Param rotation = 0.0f;
  /** How far the children are pulled to the parent's two level sides, like a fan. */
  Param flatten = 0.0f;
  /** Trunk: radius at the bottom. Others: share of the parent radius. */
  Param radius = 0.1f;
  /** Radius at the tip as a share of the base radius. */
  Param taper = 0.1f;
  Param droop = 0.0f;
  Param wiggle = 0.0f;
  int segments = 8;
  /** Branches and roots: how far the base spreads out over the parent it grows from. */
  Param weld = 0.0f;
  /** Branches and roots: length of the weld as a share of the parent's radius. */
  Param weld_length = 1.0f;
  /** Branches and roots: how much more the weld spreads toward the parent's tip and base. */
  Param weld_upper = 1.0f;
  Param weld_lower = 1.5f;
  /** Branches and roots: rings across the weld, from the parent's surface into the branch. */
  int weld_rings = 6;
  /** Branches and roots: never thicker than the parent where they grow. */
  bool clamp_to_parent = true;
  /** Longer branches get thicker, shorter ones thinner, by this power of their length. */
  float length_radius = 0.0f;
  /** Turn of the rings around the branch. */
  float roll = 0.0f;
  /** Evens out the line of the branch before it is meshed (0 to 1). */
  float path_smooth = 0.0f;
  /** The texture seam is turned to the back, instead of anywhere. */
  bool seam_fix = true;

  /* Mesh of trunks, branches and roots. */
  /** Sides around, 0 for fewer on thinner branches, as many as the trunk at its base. */
  int mesh_sides = 0;
  /** How much detail this level keeps when the triangle budget takes some away. */
  float mesh_detail = 1.0f;
  /** Times the bark texture goes around, and how stretched it is along. */
  int u_tiles = 1;
  float v_tiles = 1.0f;
  float u_offset = 0.0f;
  float v_offset = 0.0f;
  /** 0: the texture keeps its aspect on every branch. 1: the same length per tile everywhere. */
  int v_mode = 0;
  /** Turns of the texture around the branch from its base to its tip. */
  float uv_twist = 0.0f;
  /** Half of the branches get the texture mirrored. */
  bool random_flip = false;
  /** The whole branch is one tile along. */
  bool fit_one_tile = false;

  /* Trunk. */
  Param flare = 0.0f;
  Param lean = 0.0f;
  Param spread = 0.0f;
  int sides = 8;
  /**
   * The trunks stand on this mesh: they drop straight down onto it, and their roots stay out of
   * it. Its place in the space of the tree.
   */
  const Mesh *ground = nullptr;
  float4x4 ground_transform = float4x4::identity();
  /** How far the trunks turn from straight up to the slope they stand on. */
  Param align = 0.0f;

  /* Roots. */
  Param rise = 0.0f;
  Param depth = 0.0f;

  /* Leaves, the size is #length. Fronds use the width, fold and soft normals too. */
  LeafShape shape = LeafShape::Leaf;
  /** How much the size of each leaf differs from #length, 0 to 1. */
  Param size_random = 0.0f;
  /** Mesh leaves: placed with +Y along the leaf and +Z to the side it faces. */
  const Mesh *leaf_mesh = nullptr;
  Param width = 0.6f;
  Param fold = 0.0f;
  Param face_up = 0.0f;
  NormalMode normal_mode = NormalMode::Spherical;
  /** How far the normals turn from the cards toward the normal mode. */
  Param soft_normals = 0.0f;
  /** Mesh leaves from a collection, picked by their weight. Empty for #leaf_mesh. */
  Vector<LeafChoice> leaf_choices;
  int leaf_seed = 0;
  /** Mesh leaves keep the size of the mesh instead of #length. */
  bool real_size = false;
  /** Stretch across, along and out of the leaf. */
  float3 leaf_scale = float3(1.0f);
  /** The base of the leaf sits on the branch's surface instead of its middle line. */
  bool leaf_weld = true;
  /** A second card at a right angle. */
  bool cross = false;
  /** Half of the leaves mirrored. */
  bool flip = false;
  /** Tip up or down, sides rolled in, turn from base to tip. */
  Param bend = 0.0f;
  Param curl = 0.0f;
  Param twist = 0.0f;
  /** Bumps on the leaf, as a share of its length, and their size. */
  Param leaf_noise = 0.0f;
  Param leaf_noise_scale = 1.0f;
  /** Rows of vertices along leaf and square cards, more to bend smoothly. */
  int leaf_detail = 1;
  /**
   * Leaves: clumps along each parent, the leaves sit on their merged surface. The clumps place the
   * leaves and give the Clumps normals, and with #show_clumps they are in the mesh too.
   */
  bool use_clumps = false;
  int clump_count = 3;
  /** Radius of a clump, before the tree's scale. Nodes linked into it vary it per clump. */
  Param clump_size = 0.8f;
  /** The clumps are smooth blobs in the mesh, with the material of the leaves. */
  bool show_clumps = false;
  /** Height of a clump as a share of its width. */
  float clump_squash = 0.8f;
  /** How softly clumps that touch run into each other, 0 to 1. */
  float clump_merge = 0.5f;

  /* Fronds, they follow the line of their parent. #width, #fold, #curl and #twist shape them. */
  /** Ribbons on each parent, turned evenly around it. */
  int planes = 1;
  /** Share of the length that narrows to the tip, and whether it closes in a curve. */
  float frond_tip = 0.3f;
  bool frond_round_tip = false;
  /** Columns across each frond. */
  int frond_across = 2;
  /** Random turn of each frond around its stem, 0 to 1. */
  float frond_roll_random = 0.0f;

  /* Ivy: #angle is the climb, #wiggle its wander, #droop the share that hangs free. */
  /** Space between the vines and the bark, before the tree's scale. */
  float ivy_gap = 0.005f;

  /** Length by place on the parent, sampled evenly from 0 to 1, empty for the full length. */
  Array<float> length_profile;
  /** Branches: angle, radius and droop by place on the parent, the same way. */
  Array<float> angle_profile;
  Array<float> radius_profile;
  Array<float> droop_profile;

  /* Forks: trunks and branches split into several stems on the way up. */
  /** Times each stem splits, 0 for none. */
  int fork_count = 0;
  /** Stems each split makes. */
  int fork_stems = 2;
  /** Where on the stem the first split is (0 to 1), the others share what is above. */
  Param fork_start = 0.5f;
  /** Angle between a new stem and the stem it splits from. */
  Param fork_angle = 0.45f;
  /** How likely each split is. */
  Param fork_chance = 1.0f;
  /** 0.5 for stems as thick as each other, more for a thicker first stem. */
  Param fork_balance = 0.5f;
  /** How much the angles of the stems differ from each other. */
  Param fork_spread = 0.0f;
  /** How gently the stems turn out of the fork. */
  Param fork_smooth = 0.25f;

  /* Growth of trunks, branches and roots. */
  /** Random turn of each section, more where the wood is thin. */
  Param gnarl = 0.0f;
  /** Turn of each section around the stem. */
  Param stem_twist = 0.0f;
  /** Leaves: one more at the tip of every branch that doesn't carry on. */
  bool tip_leaf = true;
  /** The base takes the normals of the parent's bark and turns to its own over this length, as
   * a share of its radius. */
  bool parent_normals = true;
  Param normal_blend = 1.0f;
  /** Smooth shading of the stems, flat faces when off. */
  bool smooth = true;

  /* Bark shape of trunks, branches and roots. */
  /** Ridges around the stem, 0 for a round stem. */
  int flutes = 0;
  /** How deep the grooves between the ridges are, as a share of the radius. */
  Param flute_depth = 0.0f;
  /** How far up the stem the flutes go before they fade out (0 to 1). */
  Param flute_height = 1.0f;
  /** Lumps on the bark, as a share of the radius. */
  Param bumps = 0.0f;
  /** Size of the lumps, before the tree's scale. */
  Param bump_size = 0.3f;

  Vector<Force> forces;
  Vector<CrownShape> shapes;
  Vector<Prune> prunes;
  Vector<Displace> displaces;
  /** Drawn: the curves, and how close a curve's start must be to another to grow from it. */
  Vector<DrawnSpline> splines;
  float snap = 0.1f;

  /** Indices of the levels that grow on this one. */
  Vector<int> children;
};

/** What can be stored on every vertex. */
enum class VertexValue {
  None = 0,
  BranchRandom = 1,
  LeafRandom = 2,
  Level = 3,
  Depth = 4,
  AlongBranch = 5,
  Height = 6,
  Radius = 7,
  Occlusion = 8,
  Weld = 9,
  LeafFacing = 10,
  ClumpRandom = 11,
  ClumpDepth = 12,
  ClumpEdge = 13,
  GradientHeight = 14,
};
static constexpr int vertex_value_count = 15;

/** Which values the mesh gets, each as its own attribute, and four of them in one color. */
struct VertexData {
  /** By #VertexValue. */
  std::array<bool, vertex_value_count> write{};
  /** Rays per vertex for the occlusion. */
  int rays = 16;
  /** The color attribute, red to alpha, None for no color. */
  std::array<VertexValue, 4> color{};
  bool use_color = false;
};

struct TreeParams {
  Vector<Level> levels;
  /** Levels that stand on the ground, they should be trunks. */
  Vector<int> roots;
  int seed = 0;
  float scale = 1.0f;
  /** Most triangles for one tree, 0 for no limit. */
  int triangle_budget = 0;
  /** Leaves and fronds. */
  bool leaves = true;
  /** Conifer: branches get shorter toward the top and every stem runs to a point. Broadleaf:
   * stems carry on from the tip of their parent as the next level. */
  bool conifer = false;
  /** Scales the sides and sections of every stem, 0 to 1. */
  float detail = 1.0f;
  VertexData vertex_data;
};

/**
 * Grows one tree at every position, or one at the origin when there are none. Every tree gets its
 * own seed. Faces get the material index of their level, "UVMap" coordinates and custom normals.
 * Returns null when there is nothing to grow or when \a stop says so.
 *
 * \param r_triangles: Triangles of the first tree.
 */
Mesh *mesh_trees(const TreeParams &params,
                 Span<float3> positions,
                 int *r_triangles,
                 FunctionRef<bool()> stop = {});

}  // namespace blender::geometry::tree
