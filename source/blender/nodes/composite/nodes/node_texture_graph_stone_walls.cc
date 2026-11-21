/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Texture Graph stone walls: the wall patterns of the Carve modifier as tiling textures. Stone
 * Wall has the stones laid on their own, Fitted Wall the stones fitted to their neighbors, and
 * Packed Wall the stones stacked one by one.
 */

#include "GEO_mesh_carve.hh"

#include "texture_graph_filter.hh"

namespace blender::nodes::node_texture_graph_stone_walls_cc {

namespace tg = texture_graph;
using namespace tg::filter;
using geometry::CarvePattern;

static const EnumPropertyItem wall_items[] = {
    {int(CarvePattern::RandomStone),
     "RANDOM_STONE",
     0,
     N_("Random Stone"),
     N_("Rows of uneven stones with mortar between them")},
    {int(CarvePattern::CoursedBlocks),
     "COURSED_BLOCKS",
     0,
     N_("Coursed Blocks"),
     N_("Squared blocks in even courses")},
    {int(CarvePattern::Slate), "SLATE", 0, N_("Slate"), N_("Thin, long stones laid flat")},
    {int(CarvePattern::DryStone),
     "DRY_STONE",
     0,
     N_("Dry Stone"),
     N_("Thin stones stacked tight, with small gaps")},
    {int(CarvePattern::Banded),
     "BANDED",
     0,
     N_("Banded"),
     N_("Small stones with a course of long stones every few courses")},
    {int(CarvePattern::Fieldstone),
     "FIELDSTONE",
     0,
     N_("Fieldstone"),
     N_("Round stones with wide joints")},
    {int(CarvePattern::Rubble), "RUBBLE", 0, N_("Rubble"), N_("Large, irregular stones")},
    {int(CarvePattern::Cobblestone),
     "COBBLESTONE",
     0,
     N_("Cobblestone"),
     N_("Rounded stones of about the same size")},
    {int(CarvePattern::Ashlar),
     "ASHLAR",
     0,
     N_("Ashlar"),
     N_("Square and long blocks of mixed sizes")},
    {int(CarvePattern::Bricks), "BRICKS", 0, N_("Bricks"), N_("Even rows of bricks")},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem fitted_items[] = {
    {int(CarvePattern::FittedRubble),
     "FITTED_RUBBLE",
     0,
     N_("Fitted Rubble"),
     N_("Irregular stones that fit their neighbors, some split in pieces")},
    {int(CarvePattern::RoughCourses),
     "ROUGH_COURSES",
     0,
     N_("Rough Courses"),
     N_("Rough stones in wandering courses, fitted to each other")},
    {int(CarvePattern::SquaredStone),
     "SQUARED_STONE",
     0,
     N_("Squared Stone"),
     N_("Squared stones in even courses, fitted tight")},
    {int(CarvePattern::MixedRubble),
     "MIXED_RUBBLE",
     0,
     N_("Mixed Rubble"),
     N_("Big stones broken into pieces of mixed sizes")},
    {int(CarvePattern::WornStone),
     "WORN_STONE",
     0,
     N_("Worn Stone"),
     N_("Fitted stones with well worn, round corners")},
    {int(CarvePattern::ThinCourses),
     "THIN_COURSES",
     0,
     N_("Thin Courses"),
     N_("Thin, long stones fitted in many courses")},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem packed_items[] = {
    {int(CarvePattern::PackedRound),
     "ROUND_STONES",
     0,
     N_("Round Stones"),
     N_("Round stones stacked one by one")},
    {int(CarvePattern::PackedAngular),
     "ANGULAR_STONES",
     0,
     N_("Angular Stones"),
     N_("Angular stones stacked one by one")},
    {int(CarvePattern::PackedBlocks),
     "BLOCKS",
     0,
     N_("Blocks"),
     N_("Rough blocks stacked one by one")},
    {int(CarvePattern::PackedSlabs),
     "SLABS",
     0,
     N_("Slabs"),
     N_("Long, thin slabs stacked one by one")},
    {int(CarvePattern::PackedWedges),
     "WEDGES",
     0,
     N_("Wedges"),
     N_("Broken, wedge shaped stones stacked one by one")},
    {int(CarvePattern::PackedMixed),
     "MIXED_STONES",
     0,
     N_("Mixed Stones"),
     N_("Stones of every shape stacked one by one")},
    {int(CarvePattern::DryWall),
     "DRY_WALL",
     0,
     N_("Dry Wall"),
     N_("Blocks and slabs stacked flat, with wedges in the gaps")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void declare_wall(NodeDeclarationBuilder &b,
                         const EnumPropertyItem *items,
                         const int default_style,
                         const bool rows)
{
  value_out(b, "Height", "Height of the stones, black in the joints");
  value_out(b, "Random", "A random gray for each stone");
  value_out(b, "Stones", "White on the stones, black in the joints");
  menu_in(b, "Style", items, default_style, "Kind of wall");
  float_in(b, "Scale", 2.0f, 0.1f, 50.0f, "How much wall fits across the image, higher is smaller");
  float_in(b, "Stone Length", 0.8f, 0.01f, 10.0f, "Length of the stones, compared to their height");
  if (rows) {
    float_in(b, "Row Height", 0.25f, 0.01f, 10.0f, "Height of the rows of stones");
  }
  float_in(b, "Mortar", 0.02f, 0.0f, 0.5f, "Width of the joints between stones");
  factor_in(b, "Roundness", 0.5f, "How rounded the edges of the stones are");
  factor_in(b, "Variation", 0.5f, "How much the stones differ in size and height");
  factor_in(b, "Roughness", 0.5f, "How rough the faces of the stones are");
  seed_in(b);
  b.add_input<decl::Menu>("Resolution")
      .default_value(MenuValue(0))
      .static_items(tg::generator_size_items)
      .description("Size of the image this node makes");
}

static void declare_stone_wall(NodeDeclarationBuilder &b)
{
  declare_wall(b, wall_items, int(CarvePattern::RandomStone), true);
}

static void declare_fitted_wall(NodeDeclarationBuilder &b)
{
  declare_wall(b, fitted_items, int(CarvePattern::FittedRubble), true);
}

static void declare_packed_wall(NodeDeclarationBuilder &b)
{
  declare_wall(b, packed_items, int(CarvePattern::PackedRound), false);
}

class Operation : public FilterOperation {
 public:
  using FilterOperation::FilterOperation;

  void execute() override
  {
    geometry::CarvePatternParams params;
    params.pattern = CarvePattern(this->m("Style"));
    params.scale = math::max(this->f("Scale"), 0.1f);
    params.stone_length = math::max(this->f("Stone Length"), 0.01f);
    if (!STREQ(this->node().idname, "TextureGraphNodePackedWall")) {
      params.row_height = math::max(this->f("Row Height"), 0.01f);
    }
    params.mortar = math::max(this->f("Mortar"), 0.0f);
    params.roundness = this->f("Roundness");
    params.variation = this->f("Variation");
    params.roughness = this->f("Roughness");
    params.seed = this->i("Seed");

    const int resolution = this->m("Resolution");
    const Domain domain = tg::generator_domain(this->context(), resolution);
    const int2 size = domain.data_size;

    Array<float> height(int64_t(size.x) * size.y);
    Array<float> random(height.size());
    Array<float> stones(height.size());
    each_pixel(size, [&](const int2 p, const int64_t index) {
      const geometry::CarvePatternSample sample = geometry::carve_pattern_tiled(
          params, (float2(p) + 0.5f) / float2(size));
      height[index] = sample.height;
      random[index] = sample.height > 0.0f ? sample.id : 0.0f;
      stones[index] = math::clamp(sample.height * 8.0f, 0.0f, 1.0f);
    });
    this->out_values("Height", domain, height);
    this->out_values("Random", domain, random);
    this->out_values("Stones", domain, stones);
  }
};

static void node_register()
{
  static bke::bNodeType types[3];
  register_filter(types[0], "TextureGraphNodeStoneWall", "Stone Wall",
                  "Walls of stones laid in courses or cells, with mortar between them",
                  "TEXTURE_GRAPH_STONE_WALL", NODE_CLASS_TEXTURE, declare_stone_wall,
                  make_operation<Operation>);
  register_filter(types[1], "TextureGraphNodeFittedWall", "Fitted Wall",
                  "Walls of stones cut to fit their neighbors", "TEXTURE_GRAPH_FITTED_WALL",
                  NODE_CLASS_TEXTURE, declare_fitted_wall, make_operation<Operation>);
  register_filter(types[2], "TextureGraphNodePackedWall", "Packed Wall",
                  "Walls of stones stacked one by one", "TEXTURE_GRAPH_PACKED_WALL",
                  NODE_CLASS_TEXTURE, declare_packed_wall, make_operation<Operation>);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_stone_walls_cc
