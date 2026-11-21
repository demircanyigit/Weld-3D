/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Texture Graph nodes that lay out many copies of a shape or image: Tile Generator, Tile Sampler,
 * Tile Random, Tile Random 2, Splatter, Splatter Circular, Arc Pavement, Shape Mapper and
 * Scratches Generator. All but Tile Random 2 and Shape Mapper build a list of copies and draw
 * them with the stamper from texture_graph_shapes.hh.
 */

#include <string>

#include "BLI_math_vector.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"
#include "texture_graph_shapes.hh"
#include "texture_graph_util.hh"

namespace blender::nodes::node_texture_graph_tile_cc {

namespace tg = texture_graph;
using namespace tg::shapes;
using namespace blender::compositor;

/* -------------------------------------------------------------------- */
/** \name Shared settings
 * \{ */

static constexpr int max_images = 6;

static const EnumPropertyItem distribution_items[] = {
    {0, "RANDOM", 0, N_("Random"), N_("Every copy picks an image at random")},
    {1, "IN_ORDER", 0, N_("In Order"), N_("The images take turns")},
    {2, "MAP", 0, N_("Map"), N_("The Distribution Map picks the image")},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem step_rotation_items[] = {
    {0, "0", 0, N_("0"), N_("Not turned")},
    {1, "90", 0, N_("90"), N_("A quarter turn")},
    {2, "180", 0, N_("180"), N_("A half turn")},
    {3, "270", 0, N_("270"), N_("Three quarter turns")},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem symmetry_items[] = {
    {0, "BOTH", 0, N_("Both"), N_("Mirror left to right and top to bottom")},
    {1, "HORIZONTAL", 0, N_("Horizontal"), N_("Mirror left to right")},
    {2, "VERTICAL", 0, N_("Vertical"), N_("Mirror top to bottom")},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem combine_items[] = {
    {int(Combine::Max), "MAX", 0, N_("Max"), N_("The brightest copy wins where copies overlap")},
    {int(Combine::Add), "ADD", 0, N_("Add"), N_("Overlapping copies add up")},
    {int(Combine::Over), "OVER", 0, N_("Over"), N_("Later copies cover earlier ones")},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem offset_items[] = {
    {0, "ROWS", 0, N_("Rows"), N_("Every row shifts sideways from the one below")},
    {1, "COLUMNS", 0, N_("Columns"), N_("Every column shifts up from the one to its left")},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem size_mode_items[] = {
    {0, "GAP", 0, N_("Gap"), N_("Tiles fill their cells, minus the gap")},
    {1, "SIZE", 0, N_("Size"), N_("Tiles have the size set below, as a part of their cell")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void declare_output(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Float>("Image").structure_type(StructureType::Dynamic);
}

/* The shape menu and the image inputs it can switch to. */
static void declare_pattern(NodeDeclarationBuilder &b, const int images, const int default_shape)
{
  b.add_input<decl::Menu>("Pattern")
      .default_value(MenuValue(default_shape))
      .static_items(shape_items)
      .description("The shape every copy draws, or the connected images");
  for (int i = 0; i < images; i++) {
    const std::string name = images == 1 ? std::string("Pattern Image") :
                                           "Pattern " + std::to_string(i + 1);
    b.add_input<decl::Float>(name)
        .default_value(1.0f)
        .hide_value()
        .compositor_realization_mode(CompositorInputRealizationMode::None)
        .structure_type(StructureType::Dynamic)
        .usage_by_menu("Pattern", int(Shape::Image))
        .description("An image every copy can show, drawn to fill the copy");
  }
  if (images > 1) {
    b.add_input<decl::Int>("Pattern Count")
        .default_value(1)
        .min(1)
        .max(images)
        .usage_by_menu("Pattern", int(Shape::Image))
        .description("How many of the images are used");
  }
  b.add_input<decl::Float>("Pattern Specific")
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Changes the shape a little, each shape in its own way");
}

static void declare_map(NodeDeclarationBuilder &b, const char *name, const char *description)
{
  b.add_input<decl::Float>(name)
      .default_value(1.0f)
      .hide_value()
      .compositor_realization_mode(CompositorInputRealizationMode::None)
      .structure_type(StructureType::Dynamic)
      .description(description);
}

static void declare_factor(NodeDeclarationBuilder &b,
                           const char *name,
                           const float d,
                           const char *description)
{
  b.add_input<decl::Float>(name).default_value(d).min(0.0f).max(1.0f).subtype(PROP_FACTOR).description(
      description);
}

static void declare_float(NodeDeclarationBuilder &b,
                          const char *name,
                          const float d,
                          const float min,
                          const float max,
                          const char *description)
{
  b.add_input<decl::Float>(name).default_value(d).min(min).max(max).description(description);
}

static void declare_int(NodeDeclarationBuilder &b,
                        const char *name,
                        const int d,
                        const int min,
                        const int max,
                        const char *description)
{
  b.add_input<decl::Int>(name).default_value(d).min(min).max(max).description(description);
}

static void declare_bool(NodeDeclarationBuilder &b,
                         const char *name,
                         const bool d,
                         const char *description)
{
  b.add_input<decl::Bool>(name).default_value(d).description(description);
}

static void declare_vector(NodeDeclarationBuilder &b,
                           const char *name,
                           const float2 d,
                           const float min,
                           const float max,
                           const char *description)
{
  b.add_input<decl::Vector>(name)
      .subtype(PROP_FACTOR)
      .dimensions(2)
      .default_value({d.x, d.y})
      .min(min)
      .max(max)
      .description(description);
}

static void declare_menu(NodeDeclarationBuilder &b,
                         const char *name,
                         const int d,
                         const EnumPropertyItem *items,
                         const char *description)
{
  b.add_input<decl::Menu>(name).default_value(MenuValue(d)).static_items(items).description(
      description);
}

static void declare_tail(NodeDeclarationBuilder &b, const bool background = true)
{
  if (background) {
    b.add_input<decl::Float>("Background")
        .default_value(0.0f)
        .min(0.0f)
        .max(1.0f)
        .compositor_realization_mode(CompositorInputRealizationMode::None)
        .structure_type(StructureType::Dynamic)
        .description("What shows between the copies");
  }
  b.add_input<decl::Int>("Seed").default_value(0).description(
      "Change it to get a different layout");
  b.add_input<decl::Menu>("Resolution")
      .default_value(MenuValue(0))
      .static_items(tg::generator_size_items)
      .description("Size of the image this node makes");
}

/* Shared reading of settings and drawing of copies. */
class CopiesOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

 protected:
  float f(const char *name)
  {
    return this->get_input(name).get_single_value_default<float>();
  }
  int i(const char *name)
  {
    return this->get_input(name).get_single_value_default<int>();
  }
  bool b(const char *name)
  {
    return this->get_input(name).get_single_value_default<bool>();
  }
  int m(const char *name)
  {
    return this->get_input(name).get_single_value_default<MenuValue>().value;
  }
  float2 v(const char *name)
  {
    return this->get_input(name).get_single_value_default<float2>();
  }
  uint32_t seed()
  {
    return uint32_t(this->i("Seed"));
  }
  Domain output_domain()
  {
    return tg::generator_domain(this->context(), this->m("Resolution"));
  }

  /* The connected images, when the pattern is set to show them. */
  Vector<Picture> read_images(const int images)
  {
    Vector<Picture> pictures;
    if (Shape(this->m("Pattern")) != Shape::Image) {
      return pictures;
    }
    if (images == 1) {
      pictures.append(read_picture(this->get_input("Pattern Image")));
      return pictures;
    }
    const int count = math::clamp(this->i("Pattern Count"), 1, images);
    for (int k = 0; k < count; k++) {
      pictures.append(read_picture(this->get_input("Pattern " + std::to_string(k + 1))));
    }
    return pictures;
  }

  /* Draws the copies over the background into the output. */
  void draw(const Span<Copy> copies,
            const Span<Picture> images,
            const Combine combine,
            const Picture &background,
            const float level_min = 0.0f,
            const float level_max = 1.0f)
  {
    const Domain domain = this->output_domain();
    const int2 size = domain.data_size;
    const Stamper stamper(copies, images);
    Result &output = this->get_result("Image");
    output.allocate_texture(domain);
    parallel_for(size, [&](const int2 texel) {
      const float2 uv = (float2(texel) + 0.5f) / float2(size);
      const Hit hit = stamper.at(uv, combine);
      const float bg = background.sample_wrapped(uv);
      float value = bg;
      if (hit.copy >= 0) {
        switch (combine) {
          case Combine::Add:
            value = bg + hit.value;
            break;
          case Combine::Max:
            value = math::max(bg, hit.value);
            break;
          case Combine::Over: {
            /* The top copy covers the background, softened at its very edge. */
            const float coverage = math::clamp(
                stamper.shape_of(copies[hit.copy], hit.local) * 8.0f, 0.0f, 1.0f);
            value = math::interpolate(bg, hit.value, coverage);
            break;
          }
        }
      }
      value = math::interpolate(level_min, level_max, value);
      output.store_pixel(texel, std::isfinite(value) ? value : 0.0f);
    });
  }

  Picture background()
  {
    return read_picture(this->get_input("Background"));
  }
};

/* Picks the image of a copy. */
static int pick_image(const int distribution,
                      const int count,
                      const int index,
                      const float random,
                      const float map_value)
{
  if (count <= 1) {
    return 0;
  }
  switch (distribution) {
    case 1:
      return index % count;
    case 2:
      return math::clamp(int(map_value * float(count)), 0, count - 1);
    default:
      return math::clamp(int(random * float(count)), 0, count - 1);
  }
}

static void apply_symmetry(Copy &copy, const float amount, const int mode, uint32_t &state)
{
  if (random_float(state) >= amount) {
    return;
  }
  const bool flip_x = mode != 2;
  const bool flip_y = mode != 1;
  if (flip_x && flip_y) {
    const float r = random_float(state);
    copy.flip_x = r < 0.66f;
    copy.flip_y = r >= 0.33f;
  }
  else {
    copy.flip_x = flip_x;
    copy.flip_y = flip_y;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Grid layout shared by Tile Generator and Tile Sampler
 * \{ */

struct GridSettings {
  int2 amount = int2(8);
  Shape shape = Shape::Square;
  int image_count = 0;
  int distribution = 0;
  float specific = 0.0f;
  float specific_random = 0.0f;
  int step_rotation = 0;
  float step_rotation_random = 0.0f;
  bool checker_turn = false;
  float symmetry_random = 0.0f;
  int symmetry_mode = 0;
  int size_mode = 0;
  float2 gap = float2(0.1f);
  float2 size = float2(0.8f);
  float2 size_random = float2(0.0f);
  float2 middle_size = float2(0.5f);
  float scale = 1.0f;
  float scale_random = 0.0f;
  float offset = 0.0f;
  float offset_random = 0.0f;
  int offset_mode = 0;
  float2 position_random = float2(0.0f);
  float2 global_offset = float2(0.0f);
  float rotation = 0.0f;
  float rotation_random = 0.0f;
  float value = 1.0f;
  float value_random = 0.0f;
  uint32_t seed = 0;

  /* Tile Sampler maps, sampled at the center of each tile. */
  const Picture *scale_map = nullptr;
  float scale_map_amount = 0.0f;
  const Picture *rotation_map = nullptr;
  float rotation_map_amount = 0.0f;
  const Picture *displacement_map = nullptr;
  float displacement_amount = 0.0f;
  float displacement_angle = 0.0f;
  const Picture *value_map = nullptr;
  float value_map_amount = 0.0f;
  const Picture *mask_map = nullptr;
  float mask_threshold = 0.0f;
  float mask_random = 0.0f;
  const Picture *distribution_map = nullptr;
};

static Vector<Copy> grid_copies(const GridSettings &g)
{
  const int2 amount = math::max(g.amount, int2(1));
  const float2 cell = 1.0f / float2(amount);
  Vector<Copy> copies;
  copies.reserve(int64_t(amount.x) * amount.y);
  for (int y = 0; y < amount.y; y++) {
    for (int x = 0; x < amount.x; x++) {
      const int index = y * amount.x + x;
      uint32_t state = tg::hash(x, y, g.seed);
      /* Random numbers in a fixed order, so one setting does not change the others. */
      const float r_offset = random_float(state);
      const float r_scale = random_float(state);
      const float r_rotation = random_float(state);
      const float r_step = random_float(state);
      const float r_value = random_float(state);
      const float r_mask = random_float(state);
      const float r_image = random_float(state);
      const float r_specific = random_float(state);
      const float2 r_position(random_float(state) - 0.5f, random_float(state) - 0.5f);
      const float2 r_size(random_float(state), random_float(state));

      /* Rows shift sideways, or columns shift up. */
      float2 center = (float2(x, y) + 0.5f) * cell;
      if (g.offset_mode == 0) {
        center.x += (float(y) * g.offset + (r_offset - 0.5f) * 2.0f * g.offset_random) * cell.x;
      }
      else {
        center.y += (float(x) * g.offset + (r_offset - 0.5f) * 2.0f * g.offset_random) * cell.y;
      }
      center += r_position * g.position_random * cell * 2.0f + g.global_offset;
      center = fract_vec(center);

      Copy copy;
      copy.center = center;
      /* Every other row and column can be a different size. */
      const float2 middle(x & 1 ? g.middle_size.x * 2.0f : 1.0f, y & 1 ? g.middle_size.y * 2.0f : 1.0f);
      float2 half = g.size_mode == 0 ? (1.0f - g.gap) * 0.5f * cell : g.size * 0.5f * cell;
      half *= math::min(middle, float2(2.0f));
      half *= 1.0f - g.size_random * r_size;
      float scale = g.scale * (1.0f - g.scale_random * r_scale);
      if (g.scale_map) {
        scale *= math::interpolate(1.0f, g.scale_map->sample_wrapped(center), g.scale_map_amount);
      }
      copy.half_size = half * scale;

      int steps = g.step_rotation;
      if (r_step < g.step_rotation_random) {
        steps += int(r_step / math::max(g.step_rotation_random, 1e-6f) * 4.0f);
      }
      if (g.checker_turn && ((x + y) & 1)) {
        steps += 1;
      }
      float turns = float(steps) * 0.25f + g.rotation + (r_rotation - 0.5f) * g.rotation_random;
      if (g.rotation_map) {
        turns += g.rotation_map->sample_wrapped(center) * g.rotation_map_amount;
      }
      copy.angle = turns * float(M_PI * 2.0);
      /* A quarter turn swaps width and height, swap them back so the tile still fits its cell. */
      if (steps & 1) {
        copy.half_size = float2(copy.half_size.y, copy.half_size.x);
      }
      if (g.displacement_map) {
        const float d = (g.displacement_map->sample_wrapped(center) - 0.5f) * 2.0f *
                        g.displacement_amount;
        const float a = g.displacement_angle * float(M_PI * 2.0);
        copy.center = fract_vec(copy.center + float2(math::cos(a), math::sin(a)) * d * cell);
      }

      copy.shape = g.shape;
      copy.specific = math::clamp(g.specific + (r_specific - 0.5f) * g.specific_random, 0.0f, 1.0f);
      copy.image = pick_image(g.distribution,
                              g.image_count,
                              index,
                              r_image,
                              g.distribution_map ? g.distribution_map->sample_wrapped(center) : 0.0f);
      copy.value = g.value * (1.0f - g.value_random * r_value);
      if (g.value_map) {
        copy.value *= math::interpolate(1.0f, g.value_map->sample_wrapped(center), g.value_map_amount);
      }
      copy.random = r_value;
      uint32_t symmetry_state = state;
      apply_symmetry(copy, g.symmetry_random, g.symmetry_mode, symmetry_state);
      if (r_mask < g.mask_random) {
        continue;
      }
      if (g.mask_map && g.mask_map->sample_wrapped(center) < g.mask_threshold) {
        continue;
      }
      copies.append(copy);
    }
  }
  return copies;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tile Generator
 * \{ */

namespace tile_generator {

static void node_declare(NodeDeclarationBuilder &b)
{
  declare_output(b);
  declare_int(b, "X Amount", 10, 1, 64, "Tiles across");
  declare_int(b, "Y Amount", 10, 1, 64, "Tiles down");
  declare_pattern(b, max_images, int(Shape::Square));
  declare_menu(b, "Distribution", 0, distribution_items, "How the copies pick an image");
  declare_menu(b, "Step Rotation", 0, step_rotation_items, "Turn of every tile in quarter turns");
  declare_factor(b, "Step Rotation Random", 0.0f, "Share of tiles turned by random quarter turns");
  declare_bool(b, "Checker Turn", false, "Turn every other tile by a quarter turn");
  declare_factor(b, "Symmetry Random", 0.0f, "Share of tiles mirrored");
  declare_menu(b, "Symmetry Mode", 0, symmetry_items, "Which way tiles are mirrored");
  declare_menu(b, "Size Mode", 0, size_mode_items, "How the tile size is set");
  declare_vector(b, "Gap", float2(0.1f), 0.0f, 1.0f, "Space between tiles across and down");
  declare_vector(b, "Size", float2(0.8f), 0.0f, 1.0f, "Tile width and height, as a part of the cell");
  declare_vector(b, "Middle Size", float2(0.5f), 0.0f, 1.0f, "Size of every other column and row, 0.5 keeps them equal");
  declare_float(b, "Scale", 1.0f, 0.0f, 2.0f, "Size of every tile");
  declare_factor(b, "Scale Random", 0.0f, "Random size per tile");
  declare_factor(b, "Offset", 0.0f, "How far each row shifts from the one below, 0.5 gives a brick bond");
  declare_factor(b, "Offset Random", 0.0f, "Random shift per row");
  declare_menu(b, "Offset Mode", 0, offset_items, "Shift rows or columns");
  declare_vector(b, "Position Random", float2(0.0f), 0.0f, 1.0f, "Random move per tile");
  declare_vector(b, "Global Offset", float2(0.0f), -1.0f, 1.0f, "Moves the whole pattern");
  declare_factor(b, "Rotation", 0.0f, "Turn of every tile");
  declare_factor(b, "Rotation Random", 0.0f, "Random turn per tile");
  declare_factor(b, "Luminance", 1.0f, "Brightness of the tiles");
  declare_factor(b, "Luminance Random", 0.0f, "Random brightness per tile");
  declare_menu(b, "Blend", int(Combine::Max), combine_items, "How overlapping tiles combine");
  declare_tail(b);
}

class Operation : public CopiesOperation {
 public:
  using CopiesOperation::CopiesOperation;

  void execute() override
  {
    const Vector<Picture> images = this->read_images(max_images);
    GridSettings g;
    g.amount = int2(this->i("X Amount"), this->i("Y Amount"));
    g.shape = Shape(this->m("Pattern"));
    g.image_count = int(images.size());
    g.distribution = math::min(this->m("Distribution"), 1);
    g.specific = this->f("Pattern Specific");
    g.step_rotation = this->m("Step Rotation");
    g.step_rotation_random = this->f("Step Rotation Random");
    g.checker_turn = this->b("Checker Turn");
    g.symmetry_random = this->f("Symmetry Random");
    g.symmetry_mode = this->m("Symmetry Mode");
    g.size_mode = this->m("Size Mode");
    g.gap = this->v("Gap");
    g.size = this->v("Size");
    g.middle_size = this->v("Middle Size");
    g.scale = this->f("Scale");
    g.scale_random = this->f("Scale Random");
    g.offset = this->f("Offset");
    g.offset_random = this->f("Offset Random");
    g.offset_mode = this->m("Offset Mode");
    g.position_random = this->v("Position Random");
    g.global_offset = this->v("Global Offset");
    g.rotation = this->f("Rotation");
    g.rotation_random = this->f("Rotation Random");
    g.value = this->f("Luminance");
    g.value_random = this->f("Luminance Random");
    g.seed = this->seed();
    const Vector<Copy> copies = grid_copies(g);
    this->draw(copies, images, Combine(this->m("Blend")), this->background());
  }
};

}  // namespace tile_generator

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tile Sampler
 * \{ */

namespace tile_sampler {

static void node_declare(NodeDeclarationBuilder &b)
{
  declare_output(b);
  declare_int(b, "X Amount", 16, 1, 64, "Tiles across");
  declare_int(b, "Y Amount", 16, 1, 64, "Tiles down");
  declare_pattern(b, max_images, int(Shape::Square));
  declare_menu(b, "Distribution", 0, distribution_items, "How the copies pick an image");
  declare_factor(b, "Pattern Specific Random", 0.0f, "Random change of the shape per tile");
  declare_menu(b, "Step Rotation", 0, step_rotation_items, "Turn of every tile in quarter turns");
  declare_factor(b, "Symmetry Random", 0.0f, "Share of tiles mirrored");
  declare_menu(b, "Symmetry Mode", 0, symmetry_items, "Which way tiles are mirrored");
  declare_vector(b, "Size", float2(0.8f), 0.0f, 1.0f, "Tile width and height, as a part of the cell");
  declare_vector(b, "Size Random", float2(0.0f), 0.0f, 1.0f, "Random width and height per tile");
  declare_float(b, "Scale", 1.0f, 0.0f, 10.0f, "Size of every tile");
  declare_factor(b, "Scale Random", 0.0f, "Random size per tile");
  declare_factor(b, "Scale Map Amount", 1.0f, "How much the Scale Map changes the size");
  declare_float(b, "Position Random", 0.0f, 0.0f, 10.0f, "Random move per tile, in cells");
  declare_factor(b, "Offset", 0.5f, "How far each row shifts from the one below");
  declare_menu(b, "Offset Mode", 0, offset_items, "Shift rows or columns");
  declare_float(b, "Displacement", 0.0f, 0.0f, 4.0f, "How far the Displacement Map moves tiles, in cells");
  declare_factor(b, "Displacement Angle", 0.0f, "Which way the Displacement Map moves tiles");
  declare_factor(b, "Rotation", 0.0f, "Turn of every tile");
  declare_factor(b, "Rotation Random", 0.0f, "Random turn per tile");
  declare_factor(b, "Rotation Map Amount", 1.0f, "How much the Rotation Map turns tiles");
  declare_factor(b, "Luminance", 1.0f, "Brightness of the tiles");
  declare_factor(b, "Luminance Random", 0.0f, "Random brightness per tile");
  declare_factor(b, "Luminance Map Amount", 1.0f, "How much the Luminance Map darkens tiles");
  declare_factor(b, "Mask Threshold", 0.0f, "Tiles where the Mask Map is below this are left out");
  declare_factor(b, "Mask Random", 0.0f, "Share of tiles left out at random");
  declare_menu(b, "Blend", int(Combine::Max), combine_items, "How overlapping tiles combine");
  declare_map(b, "Scale Map", "Changes the size of each tile");
  declare_map(b, "Displacement Map", "Moves each tile");
  declare_map(b, "Rotation Map", "Turns each tile, white is a full turn");
  declare_map(b, "Luminance Map", "Brightness of each tile");
  declare_map(b, "Mask Map", "Leaves out tiles where it is dark");
  declare_map(b, "Distribution Map", "Picks the image of each tile when Distribution is Map");
  declare_tail(b);
}

class Operation : public CopiesOperation {
 public:
  using CopiesOperation::CopiesOperation;

  /* A map that is not connected does nothing. */
  const Picture *map(const char *name, Picture &storage)
  {
    const Result &input = this->get_input(name);
    if (input.is_single_value()) {
      return nullptr;
    }
    storage = read_picture(input);
    return &storage;
  }

  void execute() override
  {
    const Vector<Picture> images = this->read_images(max_images);
    Picture scale_map, displacement_map, rotation_map, value_map, mask_map, distribution_map;
    GridSettings g;
    g.amount = int2(this->i("X Amount"), this->i("Y Amount"));
    g.shape = Shape(this->m("Pattern"));
    g.image_count = int(images.size());
    g.distribution = this->m("Distribution");
    g.specific = this->f("Pattern Specific");
    g.specific_random = this->f("Pattern Specific Random");
    g.step_rotation = this->m("Step Rotation");
    g.symmetry_random = this->f("Symmetry Random");
    g.symmetry_mode = this->m("Symmetry Mode");
    g.size_mode = 1;
    g.size = this->v("Size");
    g.size_random = this->v("Size Random");
    g.scale = this->f("Scale");
    g.scale_random = this->f("Scale Random");
    g.scale_map = this->map("Scale Map", scale_map);
    g.scale_map_amount = this->f("Scale Map Amount");
    g.position_random = float2(this->f("Position Random"));
    g.offset = this->f("Offset");
    g.offset_mode = this->m("Offset Mode");
    g.displacement_map = this->map("Displacement Map", displacement_map);
    g.displacement_amount = this->f("Displacement");
    g.displacement_angle = this->f("Displacement Angle");
    g.rotation = this->f("Rotation");
    g.rotation_random = this->f("Rotation Random");
    g.rotation_map = this->map("Rotation Map", rotation_map);
    g.rotation_map_amount = this->f("Rotation Map Amount");
    g.value = this->f("Luminance");
    g.value_random = this->f("Luminance Random");
    g.value_map = this->map("Luminance Map", value_map);
    g.value_map_amount = this->f("Luminance Map Amount");
    g.mask_map = this->map("Mask Map", mask_map);
    g.mask_threshold = this->f("Mask Threshold");
    g.mask_random = this->f("Mask Random");
    g.distribution_map = this->map("Distribution Map", distribution_map);
    g.seed = this->seed();
    const Vector<Copy> copies = grid_copies(g);
    this->draw(copies, images, Combine(this->m("Blend")), this->background());
  }
};

}  // namespace tile_sampler

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tile Random
 * \{ */

namespace tile_random {

static const EnumPropertyItem split_items[] = {
    {0, "NONE", 0, N_("None"), N_("Tiles are not split")},
    {1, "AUTO", 0, N_("Auto"), N_("Split across the longer side")},
    {2, "HORIZONTAL", 0, N_("Horizontal"), N_("Split into a left and a right half")},
    {3, "VERTICAL", 0, N_("Vertical"), N_("Split into a bottom and a top half")},
    {4, "RANDOM", 0, N_("Random"), N_("Split either way at random")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  declare_output(b);
  declare_int(b, "X Amount", 4, 1, 64, "Tiles across before splitting");
  declare_int(b, "Y Amount", 8, 1, 64, "Tiles down before splitting");
  declare_pattern(b, 1, int(Shape::Square));
  declare_factor(b, "Pattern Specific Random", 0.0f, "Random change of the shape per tile");
  declare_menu(b, "Split", 1, split_items, "How tiles are split into smaller ones");
  declare_factor(b, "Split Chance", 0.5f, "How likely each tile is to be split");
  declare_int(b, "Split Levels", 2, 0, 10, "How many times a tile can be split again");
  declare_factor(b, "Size Random X", 0.0f, "Random width per tile");
  declare_factor(b, "Size Random Y", 0.0f, "Random height per tile");
  declare_factor(b, "Gap", 0.1f, "Space between tiles, as a part of the smallest tile");
  declare_factor(b, "Scale", 1.0f, "Size of every tile");
  declare_factor(b, "Scale Random", 0.0f, "Random size per tile");
  declare_factor(b, "Rotation", 0.0f, "Turn of every tile");
  declare_factor(b, "Rotation Random", 0.0f, "Random turn per tile");
  declare_factor(b, "Symmetry Random", 0.0f, "Share of tiles mirrored");
  declare_factor(b, "Offset", 0.0f, "How far each row shifts from the one below");
  declare_factor(b, "Offset Random", 0.0f, "Random sideways shift per tile");
  declare_factor(b, "Position Random", 0.0f, "Random move per tile");
  declare_factor(b, "Luminance Random", 0.5f, "Random brightness per tile");
  declare_tail(b);
}

class Operation : public CopiesOperation {
 public:
  using CopiesOperation::CopiesOperation;

  void execute() override
  {
    const Vector<Picture> images = this->read_images(1);
    const int2 amount = math::max(int2(this->i("X Amount"), this->i("Y Amount")), int2(1));
    const Shape shape = Shape(this->m("Pattern"));
    const float specific = this->f("Pattern Specific");
    const float specific_random = this->f("Pattern Specific Random");
    const int split = this->m("Split");
    const float split_chance = this->f("Split Chance");
    const int split_levels = math::clamp(this->i("Split Levels"), 0, 10);
    const float2 size_random(this->f("Size Random X"), this->f("Size Random Y"));
    const float gap = this->f("Gap");
    const float scale = this->f("Scale");
    const float scale_random = this->f("Scale Random");
    const float rotation = this->f("Rotation");
    const float rotation_random = this->f("Rotation Random");
    const float symmetry_random = this->f("Symmetry Random");
    const float offset = this->f("Offset");
    const float offset_random = this->f("Offset Random");
    const float position_random = this->f("Position Random");
    const float value_random = this->f("Luminance Random");
    const uint32_t seed = this->seed();

    struct Rect {
      float2 min;
      float2 max;
      int level;
      uint32_t state;
    };
    Vector<Rect> rects;
    const float2 cell = 1.0f / float2(amount);
    for (int y = 0; y < amount.y; y++) {
      const float shift = float(y) * offset * cell.x;
      for (int x = 0; x < amount.x; x++) {
        const float2 lo = float2(float(x) * cell.x + shift, float(y) * cell.y);
        rects.append({lo, lo + cell, 0, tg::hash(x, y, seed)});
      }
    }
    /* Split tiles until no tile wants to split any more. */
    Vector<Rect> tiles;
    while (!rects.is_empty()) {
      Rect r = rects.pop_last();
      const float2 extent = r.max - r.min;
      if (split != 0 && r.level < split_levels && random_float(r.state) < split_chance) {
        bool across_x;
        switch (split) {
          case 2:
            across_x = true;
            break;
          case 3:
            across_x = false;
            break;
          case 4:
            across_x = random_float(r.state) < 0.5f;
            break;
          default:
            across_x = extent.x >= extent.y;
            break;
        }
        Rect first = r;
        Rect second = r;
        first.level = second.level = r.level + 1;
        first.state = tg::hash(r.state ^ 0x68e31da4u);
        second.state = tg::hash(r.state ^ 0xb5297a4du);
        if (across_x) {
          first.max.x = second.min.x = (r.min.x + r.max.x) * 0.5f;
        }
        else {
          first.max.y = second.min.y = (r.min.y + r.max.y) * 0.5f;
        }
        rects.append(first);
        rects.append(second);
        continue;
      }
      tiles.append(r);
    }

    float smallest = 1.0f;
    for (const Rect &r : tiles) {
      smallest = math::min(smallest, math::min(r.max.x - r.min.x, r.max.y - r.min.y));
    }
    const float gap_uv = gap * smallest * 0.5f;

    Vector<Copy> copies;
    copies.reserve(tiles.size());
    for (const int k : tiles.index_range()) {
      Rect r = tiles[k];
      uint32_t state = r.state;
      const float2 extent = r.max - r.min;
      Copy copy;
      copy.center = (r.min + r.max) * 0.5f;
      copy.center.x += (random_float(state) - 0.5f) * offset_random * extent.x;
      copy.center += float2(random_float(state) - 0.5f, random_float(state) - 0.5f) *
                     position_random * extent;
      copy.center = fract_vec(copy.center);
      float2 half = math::max(extent * 0.5f - gap_uv, float2(0.0f));
      half *= 1.0f - size_random * float2(random_float(state), random_float(state));
      half *= scale * (1.0f - scale_random * random_float(state));
      copy.half_size = half;
      copy.angle = (rotation + (random_float(state) - 0.5f) * rotation_random) *
                   float(M_PI * 2.0);
      copy.shape = shape;
      copy.specific = math::clamp(specific + (random_float(state) - 0.5f) * specific_random,
                                  0.0f,
                                  1.0f);
      copy.random = random_float(state);
      copy.value = 1.0f - value_random * copy.random;
      apply_symmetry(copy, symmetry_random, 0, state);
      copies.append(copy);
    }
    this->draw(copies, images, Combine::Max, this->background());
  }
};

}  // namespace tile_random

/** \} */

/* -------------------------------------------------------------------- */
/** \name Splatter
 * \{ */

namespace splatter {

static void node_declare(NodeDeclarationBuilder &b)
{
  declare_output(b);
  b.add_input<decl::Float>("Pattern Image")
      .default_value(1.0f)
      .hide_value()
      .compositor_realization_mode(CompositorInputRealizationMode::None)
      .structure_type(StructureType::Dynamic)
      .description("The image to stamp, a soft disc is used when nothing is connected");
  declare_int(b, "X Amount", 8, 1, 256, "Stamps across");
  declare_int(b, "Y Amount", 8, 1, 256, "Stamps down");
  declare_float(b, "Size", 1.0f, 0.0f, 4.0f, "Size of the stamps, one fills a cell");
  declare_factor(b, "Size Random", 0.0f, "Random size per stamp");
  declare_factor(b, "Rotation", 0.0f, "Turn of every stamp");
  declare_factor(b, "Rotation Random", 0.0f, "Random turn per stamp");
  declare_float(b, "Disorder", 0.0f, 0.0f, 4.0f, "How far stamps move from the grid, in cells");
  declare_factor(b, "Disorder Angle", 0.0f, "Which way stamps move");
  declare_bool(b, "Disorder Random", true, "Every stamp moves its own way");
  declare_float(b, "Gain", 1.0f, 0.0f, 10.0f, "Brightness of every stamp, overlaps add up");
  declare_factor(b, "Luminance Random", 0.0f, "Random brightness per stamp");
  declare_factor(b, "Level Min", 0.0f, "Darkest output value");
  declare_factor(b, "Level Max", 1.0f, "Brightest output value");
  declare_tail(b);
}

class Operation : public CopiesOperation {
 public:
  using CopiesOperation::CopiesOperation;

  void execute() override
  {
    Vector<Picture> images;
    const Result &image = this->get_input("Pattern Image");
    const bool has_image = !image.is_single_value();
    if (has_image) {
      images.append(read_picture(image));
    }
    const int2 amount = math::max(int2(this->i("X Amount"), this->i("Y Amount")), int2(1));
    const float size = this->f("Size");
    const float size_random = this->f("Size Random");
    const float rotation = this->f("Rotation");
    const float rotation_random = this->f("Rotation Random");
    const float disorder = this->f("Disorder");
    const float disorder_angle = this->f("Disorder Angle");
    const bool disorder_random = this->b("Disorder Random");
    const float gain = this->f("Gain");
    const float value_random = this->f("Luminance Random");
    const uint32_t seed = this->seed();
    const float2 cell = 1.0f / float2(amount);

    Vector<Copy> copies;
    copies.reserve(int64_t(amount.x) * amount.y);
    for (int y = 0; y < amount.y; y++) {
      for (int x = 0; x < amount.x; x++) {
        uint32_t state = tg::hash(x, y, seed);
        Copy copy;
        const float angle = (disorder_random ? random_float(state) : disorder_angle) *
                            float(M_PI * 2.0);
        const float distance = disorder * (disorder_random ? random_float(state) : 1.0f);
        copy.center = fract_vec((float2(x, y) + 0.5f + float2(math::cos(angle), math::sin(angle)) *
                                                          distance) *
                                cell);
        copy.half_size = cell * 0.5f * size * (1.0f - size_random * random_float(state));
        copy.angle = (rotation + (random_float(state) - 0.5f) * rotation_random) *
                     float(M_PI * 2.0);
        copy.shape = has_image ? Shape::Image : Shape::Gaussian;
        copy.value = gain * (1.0f - value_random * random_float(state));
        copies.append(copy);
      }
    }
    this->draw(copies,
               images,
               Combine::Add,
               this->background(),
               this->f("Level Min"),
               this->f("Level Max"));
  }
};

}  // namespace splatter

/** \} */

/* -------------------------------------------------------------------- */
/** \name Splatter Circular
 * \{ */

namespace splatter_circular {

static const EnumPropertyItem circular_distribution_items[] = {
    {0, "RANDOM", 0, N_("Random"), N_("Every copy picks an image at random")},
    {1, "IN_ORDER", 0, N_("In Order"), N_("The images take turns along a ring")},
    {2, "BY_RING", 0, N_("By Ring"), N_("Every ring uses its own image")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  declare_output(b);
  declare_int(b, "Pattern Amount", 12, 1, 64, "Copies on a ring");
  declare_factor(b, "Pattern Amount Random", 0.0f, "Random number of copies per ring");
  declare_int(b, "Pattern Amount Min", 3, 1, 10, "Fewest copies on a ring");
  declare_int(b, "Rings", 1, 1, 10, "Number of rings");
  declare_pattern(b, max_images, int(Shape::Disc));
  declare_menu(b, "Distribution", 0, circular_distribution_items, "How the copies pick an image");
  declare_factor(b, "Symmetry Random", 0.0f, "Share of copies mirrored");
  declare_factor(b, "Radius", 0.35f, "Distance of the outer ring from the center");
  declare_factor(b, "Radius Random", 0.0f, "Random distance per copy");
  declare_factor(b, "Ring Spacing", 0.5f, "How close inner rings are to the outer one");
  declare_factor(b, "Angle Random", 0.0f, "Random place of each copy along its ring");
  declare_factor(b, "Spiral", 0.0f, "Turns the rings into spirals");
  declare_float(b, "Spread", 1.0f, 0.0f, 2.0f, "How many turns a ring covers");
  declare_vector(b, "Global Offset", float2(0.0f), -1.0f, 1.0f, "Moves the whole result");
  declare_bool(b, "Connect", false, "Make copies long enough to touch their neighbors");
  declare_vector(b, "Size", float2(0.08f), 0.0f, 1.0f, "Width and height of the copies");
  declare_factor(b, "Size Random", 0.0f, "Random width and height per copy");
  declare_float(b, "Scale", 1.0f, 0.0f, 2.0f, "Size of every copy");
  declare_factor(b, "Scale Random", 0.0f, "Random size per copy");
  declare_factor(b, "Scale by Number", 0.0f, "Copies get smaller along each ring");
  declare_bool(b, "Invert Number", false, "Copies get bigger along each ring instead");
  declare_factor(b, "Scale by Ring", 0.0f, "Inner rings get smaller copies");
  declare_bool(b, "Invert Ring", false, "Inner rings get bigger copies instead");
  declare_bool(b, "Follow Ring", true, "Turn copies to follow the ring");
  declare_factor(b, "Rotation", 0.0f, "Turn of every copy");
  declare_factor(b, "Rotation Random", 0.0f, "Random turn per copy");
  declare_factor(b, "Luminance", 1.0f, "Brightness of the copies");
  declare_factor(b, "Luminance Random", 0.0f, "Random brightness per copy");
  declare_menu(b, "Blend", int(Combine::Max), combine_items, "How overlapping copies combine");
  declare_tail(b);
}

class Operation : public CopiesOperation {
 public:
  using CopiesOperation::CopiesOperation;

  void execute() override
  {
    const Vector<Picture> images = this->read_images(max_images);
    const int amount = math::max(this->i("Pattern Amount"), 1);
    const float amount_random = this->f("Pattern Amount Random");
    const int amount_min = math::clamp(this->i("Pattern Amount Min"), 1, amount);
    const int rings = math::clamp(this->i("Rings"), 1, 10);
    const Shape shape = Shape(this->m("Pattern"));
    const float specific = this->f("Pattern Specific");
    const int distribution = this->m("Distribution");
    const float symmetry_random = this->f("Symmetry Random");
    const float radius = this->f("Radius");
    const float radius_random = this->f("Radius Random");
    const float ring_spacing = this->f("Ring Spacing");
    const float angle_random = this->f("Angle Random");
    const float spiral = this->f("Spiral");
    const float spread = this->f("Spread");
    const float2 global_offset = this->v("Global Offset");
    const bool connect = this->b("Connect");
    const float2 size = this->v("Size");
    const float size_random = this->f("Size Random");
    const float scale = this->f("Scale");
    const float scale_random = this->f("Scale Random");
    const float scale_by_number = this->f("Scale by Number");
    const bool invert_number = this->b("Invert Number");
    const float scale_by_ring = this->f("Scale by Ring");
    const bool invert_ring = this->b("Invert Ring");
    const bool follow = this->b("Follow Ring");
    const float rotation = this->f("Rotation");
    const float rotation_random = this->f("Rotation Random");
    const float value = this->f("Luminance");
    const float value_random = this->f("Luminance Random");
    const uint32_t seed = this->seed();

    Vector<Copy> copies;
    for (int ring = 0; ring < rings; ring++) {
      uint32_t ring_state = tg::hash(ring, 7, seed);
      /* Ring 0 is the outer one. */
      const float ring_t = rings > 1 ? float(ring) / float(rings - 1) : 0.0f;
      const float ring_radius = radius * math::interpolate(1.0f, ring_spacing, ring_t);
      int count = amount;
      if (amount_random > 0.0f) {
        count = math::max(amount_min,
                          int(std::round(float(amount) *
                                         (1.0f - amount_random * random_float(ring_state)))));
      }
      for (int k = 0; k < count; k++) {
        uint32_t state = tg::hash(ring, k, seed);
        const float number_t = count > 1 ? float(k) / float(count - 1) : 0.0f;
        const float turn = (float(k) + (random_float(state) - 0.5f) * angle_random) /
                           float(count) * spread;
        const float a = turn * float(M_PI * 2.0);
        const float r = ring_radius * (1.0f + spiral * number_t) *
                        (1.0f - radius_random * random_float(state));
        Copy copy;
        copy.center = fract_vec(float2(0.5f) + global_offset +
                                float2(math::cos(a), math::sin(a)) * r);
        float2 half = size * 0.5f;
        if (connect) {
          half.x = float(M_PI) * r * spread / float(count);
        }
        half *= 1.0f - size_random * float2(random_float(state), random_float(state));
        float s = scale * (1.0f - scale_random * random_float(state));
        s *= 1.0f - scale_by_number * (invert_number ? 1.0f - number_t : number_t);
        s *= 1.0f - scale_by_ring * (invert_ring ? 1.0f - ring_t : ring_t);
        copy.half_size = half * s;
        copy.angle = (follow ? a + float(M_PI) * 0.5f : 0.0f) +
                     (rotation + (random_float(state) - 0.5f) * rotation_random) *
                         float(M_PI * 2.0);
        copy.shape = shape;
        copy.specific = specific;
        const int index = distribution == 2 ? ring : k;
        copy.image = pick_image(math::min(distribution, 1),
                                int(images.size()),
                                index,
                                random_float(state),
                                0.0f);
        copy.value = value * (1.0f - value_random * random_float(state));
        apply_symmetry(copy, symmetry_random, 0, state);
        copies.append(copy);
      }
    }
    this->draw(copies, images, Combine(this->m("Blend")), this->background());
  }
};

}  // namespace splatter_circular

/** \} */

/* -------------------------------------------------------------------- */
/** \name Arc Pavement
 * \{ */

namespace arc_pavement {

static void node_declare(NodeDeclarationBuilder &b)
{
  declare_output(b);
  declare_int(b, "Scale", 2, 1, 8, "Fans across the image");
  declare_int(b, "Stones", 8, 1, 32, "Stones on the outer arc of a fan");
  declare_factor(b, "Stones Random", 0.0f, "Random number of stones per arc");
  declare_int(b, "Stones Min", 3, 1, 10, "Fewest stones on an arc");
  declare_int(b, "Arcs", 4, 1, 20, "Arcs in every fan");
  declare_pattern(b, 1, int(Shape::Paraboloid));
  declare_factor(b, "Pattern Scale", 0.9f, "Size of every stone");
  declare_factor(b, "Pattern Width", 1.0f, "Width of every stone");
  declare_factor(b, "Pattern Height", 1.0f, "Height of every stone");
  declare_factor(b, "Width Random", 0.0f, "Random width per stone");
  declare_factor(b, "Height Random", 0.0f, "Random height per stone");
  declare_factor(b, "Height Decrease", 0.0f, "Stones get flatter toward the ends of each arc");
  declare_factor(b, "Luminance Random", 0.3f, "Random brightness per stone");
  declare_tail(b);
}

class Operation : public CopiesOperation {
 public:
  using CopiesOperation::CopiesOperation;

  void execute() override
  {
    const Vector<Picture> images = this->read_images(1);
    const int scale = math::clamp(this->i("Scale"), 1, 8);
    const int stones = math::max(this->i("Stones"), 1);
    const float stones_random = this->f("Stones Random");
    const int stones_min = math::clamp(this->i("Stones Min"), 1, stones);
    const int arcs = math::clamp(this->i("Arcs"), 1, 20);
    const Shape shape = Shape(this->m("Pattern"));
    const float specific = this->f("Pattern Specific");
    const float pattern_scale = this->f("Pattern Scale");
    const float width = this->f("Pattern Width");
    const float height = this->f("Pattern Height");
    const float width_random = this->f("Width Random");
    const float height_random = this->f("Height Random");
    const float height_decrease = this->f("Height Decrease");
    const float value_random = this->f("Luminance Random");
    const uint32_t seed = this->seed();

    /* Fans are half discs laid in rows like fish scales: radius R, 2R apart, rows R apart and
     * every other row shifted by R. Later rows cover earlier ones. */
    const float R = 0.5f / float(scale);
    const int columns = scale;
    const int rows = scale * 2;
    Vector<Copy> copies;
    for (int row = rows - 1; row >= 0; row--) {
      for (int column = 0; column < columns; column++) {
        const float2 base((float(column) + 0.5f * float(row & 1)) * 2.0f * R + R,
                          float(row) * R);
        for (int arc = 0; arc < arcs; arc++) {
          uint32_t arc_state = tg::hash(column * 64 + arc, row, seed);
          const float r0 = R * float(arc) / float(arcs);
          const float r1 = R * float(arc + 1) / float(arcs);
          const float r_mid = (r0 + r1) * 0.5f;
          /* Inner arcs are shorter, so they get fewer stones. */
          int count = math::max(1, int(std::round(float(stones) * float(arc + 1) / float(arcs))));
          if (stones_random > 0.0f) {
            count = math::max(math::min(stones_min, count),
                              int(std::round(float(count) *
                                             (1.0f - stones_random * random_float(arc_state)))));
          }
          for (int k = 0; k < count; k++) {
            uint32_t state = tg::hash(int(arc_state), k, seed);
            const float t = (float(k) + 0.5f) / float(count);
            const float a = float(M_PI) * t;
            Copy copy;
            copy.center = fract_vec(base + float2(math::cos(a), math::sin(a)) * r_mid);
            const float along = float(M_PI) * r_mid / float(count) * 0.5f;
            const float across = (r1 - r0) * 0.5f;
            const float end_flat = 1.0f - height_decrease * math::abs(t - 0.5f) * 2.0f;
            copy.half_size = float2(along * width * (1.0f - width_random * random_float(state)),
                                    across * height * end_flat *
                                        (1.0f - height_random * random_float(state))) *
                             pattern_scale;
            copy.angle = a + float(M_PI) * 0.5f;
            copy.shape = shape;
            copy.specific = specific;
            copy.value = 1.0f - value_random * random_float(state);
            copies.append(copy);
          }
        }
      }
    }
    this->draw(copies, images, Combine::Over, this->background());
  }
};

}  // namespace arc_pavement

/** \} */

/* -------------------------------------------------------------------- */
/** \name Scratches Generator
 * \{ */

namespace scratches_generator {

static const EnumPropertyItem fade_items[] = {
    {0, "NONE", 0, N_("None"), N_("Scratches keep their brightness")},
    {1, "START", 0, N_("Start"), N_("Scratches fade in at their start")},
    {2, "END", 0, N_("End"), N_("Scratches fade out at their end")},
    {3, "BOTH", 0, N_("Both"), N_("Scratches fade at both ends")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  declare_output(b);
  declare_int(b, "Count", 64, 1, 512, "Number of scratches");
  declare_int(b, "Segments", 16, 2, 256, "Pieces every scratch is made of, more gives smoother curves");
  declare_factor(b, "Rotation", 0.0f, "Direction of every scratch");
  declare_factor(b, "Rotation Random", 1.0f, "Random direction per scratch");
  declare_factor(b, "Length", 0.3f, "Length of the scratches");
  declare_factor(b, "Length Random", 0.5f, "Random length per scratch");
  declare_factor(b, "Waviness", 0.2f, "How much the scratches bend");
  declare_factor(b, "Waviness Random", 0.5f, "Random bending per scratch");
  declare_factor(b, "Wave Frequency", 0.3f, "How tight the bends are");
  declare_float(b, "Width", 1.0f, 0.0f, 8.0f, "Thickness, in pixels of a 1024 image");
  declare_factor(b, "Width Random", 0.5f, "Random thickness per scratch");
  declare_factor(b, "Position Random", 1.0f, "Spread over the image, low values gather them in the middle");
  declare_factor(b, "Luminance Random", 0.5f, "Random brightness per scratch");
  declare_menu(b, "Fade", 3, fade_items, "Where the scratches fade");
  declare_factor(b, "Fade Length", 0.3f, "How long the fade is");
  declare_tail(b, false);
}

class Operation : public CopiesOperation {
 public:
  using CopiesOperation::CopiesOperation;

  void execute() override
  {
    const int count = math::clamp(this->i("Count"), 1, 512);
    const int segments = math::clamp(this->i("Segments"), 2, 256);
    const float rotation = this->f("Rotation");
    const float rotation_random = this->f("Rotation Random");
    const float length = this->f("Length");
    const float length_random = this->f("Length Random");
    const float waviness = this->f("Waviness");
    const float waviness_random = this->f("Waviness Random");
    const float frequency = this->f("Wave Frequency");
    const float width = this->f("Width");
    const float width_random = this->f("Width Random");
    const float position_random = this->f("Position Random");
    const float value_random = this->f("Luminance Random");
    const int fade = this->m("Fade");
    const float fade_length = math::max(this->f("Fade Length"), 1e-3f);
    const uint32_t seed = this->seed();

    Vector<Copy> copies;
    copies.reserve(int64_t(count) * segments);
    for (int k = 0; k < count; k++) {
      uint32_t state = tg::hash(k, 3, seed);
      const float2 start = float2(0.5f) + (float2(random_float(state), random_float(state)) -
                                           0.5f) *
                                              position_random;
      const float angle = (rotation + (random_float(state) - 0.5f) * rotation_random) *
                          float(M_PI * 2.0);
      const float len = length * (1.0f - length_random * random_float(state));
      const float wave = waviness * (1.0f - waviness_random * random_float(state)) * len * 0.15f;
      const float wave_count = 0.5f + frequency * 6.0f;
      const float phase = random_float(state) * float(M_PI * 2.0);
      const float half_width = width * (1.0f - width_random * random_float(state)) * 0.5f /
                               1024.0f;
      const float value = 1.0f - value_random * random_float(state);
      const float2 dir(math::cos(angle), math::sin(angle));
      const float2 side(-dir.y, dir.x);
      const auto point_at = [&](const float t) {
        return start + dir * (t - 0.5f) * len +
               side * wave * math::sin(t * wave_count * float(M_PI * 2.0) + phase);
      };
      for (int s = 0; s < segments; s++) {
        const float t0 = float(s) / float(segments);
        const float t1 = float(s + 1) / float(segments);
        const float2 a = point_at(t0);
        const float2 b = point_at(t1);
        const float t_mid = (t0 + t1) * 0.5f;
        float fade_value = 1.0f;
        if (fade == 1 || fade == 3) {
          fade_value *= math::min(t_mid / fade_length, 1.0f);
        }
        if (fade == 2 || fade == 3) {
          fade_value *= math::min((1.0f - t_mid) / fade_length, 1.0f);
        }
        Copy copy;
        copy.center = fract_vec((a + b) * 0.5f);
        copy.half_size = float2(math::distance(a, b) * 0.5f + half_width, half_width);
        copy.angle = std::atan2(b.y - a.y, b.x - a.x);
        copy.shape = Shape::Capsule;
        copy.specific = 0.0f;
        copy.value = value * fade_value;
        copies.append(copy);
      }
    }
    Picture black;
    black.single = 0.0f;
    this->draw(copies, {}, Combine::Max, black);
  }
};

}  // namespace scratches_generator

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shape Splatter and its helpers
 *
 * Shape Splatter writes what it placed into two data images, so the other three nodes can make
 * maps that line up with its height exactly:
 * - Splatter Data 1: position inside the shape in red and green, (pattern + 1) / 8 in blue,
 *   zero where there is no shape.
 * - Splatter Data 2: the shape's random number in red, its turn in green, its height in blue.
 * \{ */

static constexpr int splatter_images = 8;

static int data_pattern(const float4 &data_1)
{
  return int(std::round(data_1.z * float(splatter_images))) - 1;
}

namespace shape_splatter {

static const EnumPropertyItem splatter_distribution_items[] = {
    {0, "RANDOM", 0, N_("Random"), N_("Every shape picks a pattern at random")},
    {1, "IN_ORDER", 0, N_("In Order"), N_("The patterns take turns")},
    {2, "BY_ROW", 0, N_("By Row"), N_("Every row uses the next pattern")},
    {3, "BY_COLUMN", 0, N_("By Column"), N_("Every column uses the next pattern")},
    {4, "MAP", 0, N_("Map"), N_("The Distribution Map picks the pattern")},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem height_blend_items[] = {
    {0, "MAX", 0, N_("Max"), N_("Shapes rise out of the background")},
    {1, "ADD", 0, N_("Add"), N_("Shapes add to the background")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Float>("Height").structure_type(StructureType::Dynamic);
  b.add_output<decl::Color>("Splatter Data 1")
      .structure_type(StructureType::Dynamic)
      .description("Where every pixel is inside its shape and which pattern the shape uses");
  b.add_output<decl::Color>("Splatter Data 2")
      .structure_type(StructureType::Dynamic)
      .description("Random number, turn and height of the shape at every pixel");

  b.add_input<decl::Float>("Background Height")
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .compositor_realization_mode(CompositorInputRealizationMode::None)
      .structure_type(StructureType::Dynamic)
      .description("The surface the shapes lie on");
  declare_int(b, "X Amount", 8, 1, 64, "Shapes across");
  declare_int(b, "Y Amount", 8, 1, 64, "Shapes down");
  declare_pattern(b, splatter_images, int(Shape::Paraboloid));
  declare_menu(b, "Distribution", 0, splatter_distribution_items, "How shapes pick a pattern");
  declare_menu(b, "Step Rotation", 0, step_rotation_items, "Turn of every shape in quarter turns");
  declare_factor(
      b, "Step Rotation Random", 0.0f, "Share of shapes turned by random quarter turns");
  declare_float(b, "Scale", 1.0f, 0.0f, 5.0f, "Size of every shape");
  declare_factor(b, "Scale Random", 0.0f, "Random size per shape");
  declare_vector(
      b, "Size", float2(0.8f), 0.0f, 1.0f, "Width and height of the shapes, as a part of a cell");
  declare_factor(b, "Scale Map Amount", 1.0f, "How much the Scale Map changes the size");
  declare_float(b, "Position Random", 0.0f, 0.0f, 2.0f, "Random move per shape, in cells");
  declare_vector(b, "Offset", float2(0.0f), -2.0f, 2.0f, "Moves all shapes, in cells");
  declare_factor(b, "Vector Map Amount", 1.0f, "How far the Vector Map moves shapes");
  declare_factor(b, "Rotation", 0.0f, "Turn of every shape");
  declare_factor(b, "Rotation Random", 0.0f, "Random turn per shape");
  declare_factor(b, "Rotation Map Amount", 1.0f, "How much the Rotation Map turns shapes");
  declare_factor(b, "Height", 0.5f, "Height of the shapes");
  declare_factor(b, "Height Random", 0.0f, "Random height per shape");
  declare_float(b, "Height Offset", 0.0f, -1.0f, 1.0f, "Raises or lowers every shape");
  declare_factor(b, "Height Offset Random", 0.0f, "Random raise per shape");
  declare_factor(
      b, "Conform", 1.0f, "How much shapes sit on the background instead of at zero");
  declare_menu(b, "Height Blend", 0, height_blend_items, "How shapes meet the background");
  declare_factor(b, "Mask Random", 0.0f, "Share of shapes left out at random");
  declare_map(b, "Distribution Map", "Picks the pattern of each shape when Distribution is Map");
  declare_map(b, "Scale Map", "Changes the size of each shape");
  declare_map(b, "Rotation Map", "Turns each shape, white is a full turn");
  declare_map(b, "Height Map", "Changes the height of each shape");
  declare_map(b, "Mask Map", "Leaves out shapes where it is dark");
  b.add_input<decl::Color>("Vector Map")
      .default_value({0.5f, 0.5f, 0.5f, 1.0f})
      .hide_value()
      .compositor_realization_mode(CompositorInputRealizationMode::None)
      .structure_type(StructureType::Dynamic)
      .description("Moves each shape: red across and green down, gray stays");
  declare_tail(b, false);
}

class Operation : public CopiesOperation {
 public:
  using CopiesOperation::CopiesOperation;

  const Picture *map(const char *name, Picture &storage)
  {
    const Result &input = this->get_input(name);
    if (input.is_single_value()) {
      return nullptr;
    }
    storage = read_picture(input);
    return &storage;
  }

  void execute() override
  {
    const Vector<Picture> images = this->read_images(splatter_images);
    const Picture background = read_picture(this->get_input("Background Height"));
    Picture distribution_storage, scale_storage, rotation_storage, height_storage, mask_storage;
    const Picture *distribution_map = this->map("Distribution Map", distribution_storage);
    const Picture *scale_map = this->map("Scale Map", scale_storage);
    const Picture *rotation_map = this->map("Rotation Map", rotation_storage);
    const Picture *height_map = this->map("Height Map", height_storage);
    const Picture *mask_map = this->map("Mask Map", mask_storage);
    const Result &vector_input = this->get_input("Vector Map");
    int2 vector_size(0);
    Array<float4> vectors;
    if (!vector_input.is_single_value()) {
      vector_size = vector_input.domain().data_size;
      vectors = tg::read_colors(vector_input, vector_size);
    }

    const int2 amount = math::max(int2(this->i("X Amount"), this->i("Y Amount")), int2(1));
    const Shape shape = Shape(this->m("Pattern"));
    const float specific = this->f("Pattern Specific");
    const int distribution = this->m("Distribution");
    const int step_rotation = this->m("Step Rotation");
    const float step_rotation_random = this->f("Step Rotation Random");
    const float scale = this->f("Scale");
    const float scale_random = this->f("Scale Random");
    const float2 size = this->v("Size");
    const float scale_map_amount = this->f("Scale Map Amount");
    const float position_random = this->f("Position Random");
    const float2 offset = this->v("Offset");
    const float vector_amount = this->f("Vector Map Amount");
    const float rotation = this->f("Rotation");
    const float rotation_random = this->f("Rotation Random");
    const float rotation_map_amount = this->f("Rotation Map Amount");
    const float height = this->f("Height");
    const float height_random = this->f("Height Random");
    const float height_offset = this->f("Height Offset");
    const float height_offset_random = this->f("Height Offset Random");
    const float conform = this->f("Conform");
    const bool add = this->m("Height Blend") == 1;
    const float mask_random = this->f("Mask Random");
    const uint32_t seed = this->seed();
    const int pattern_count = shape == Shape::Image ? math::max(int(images.size()), 1) : 1;

    const float2 cell = 1.0f / float2(amount);
    Vector<Copy> copies;
    Vector<int> patterns;
    for (int y = 0; y < amount.y; y++) {
      for (int x = 0; x < amount.x; x++) {
        uint32_t state = tg::hash(x, y, seed);
        const float r_position_x = random_float(state) - 0.5f;
        const float r_position_y = random_float(state) - 0.5f;
        const float r_scale = random_float(state);
        const float r_rotation = random_float(state);
        const float r_step = random_float(state);
        const float r_height = random_float(state);
        const float r_raise = random_float(state);
        const float r_mask = random_float(state);
        const float r_pattern = random_float(state);
        const float r_id = random_float(state);

        float2 center = (float2(x, y) + 0.5f + offset +
                         float2(r_position_x, r_position_y) * position_random * 2.0f) *
                        cell;
        if (vector_size.x > 0) {
          const float4 vec = tg::sample_wrapped(
              vectors.as_span(), vector_size, fract_vec(center) * float2(vector_size));
          center += (float2(vec.x, vec.y) - 0.5f) * 2.0f * vector_amount * cell;
        }
        center = fract_vec(center);
        if (r_mask < mask_random || (mask_map && mask_map->sample_wrapped(center) < 0.5f)) {
          continue;
        }

        int pattern = 0;
        switch (distribution) {
          case 1:
            pattern = (y * amount.x + x) % pattern_count;
            break;
          case 2:
            pattern = y % pattern_count;
            break;
          case 3:
            pattern = x % pattern_count;
            break;
          case 4:
            pattern = distribution_map ?
                          math::clamp(int(distribution_map->sample_wrapped(center) *
                                          float(pattern_count)),
                                      0,
                                      pattern_count - 1) :
                          0;
            break;
          default:
            pattern = math::clamp(int(r_pattern * float(pattern_count)), 0, pattern_count - 1);
            break;
        }

        Copy copy;
        copy.center = center;
        float s = scale * (1.0f - scale_random * r_scale);
        if (scale_map) {
          s *= math::interpolate(1.0f, scale_map->sample_wrapped(center), scale_map_amount);
        }
        copy.half_size = size * 0.5f * cell * s;
        int steps = step_rotation;
        if (r_step < step_rotation_random) {
          steps += int(r_step / math::max(step_rotation_random, 1e-6f) * 4.0f);
        }
        float turns = float(steps) * 0.25f + rotation + (r_rotation - 0.5f) * rotation_random;
        if (rotation_map) {
          turns += rotation_map->sample_wrapped(center) * rotation_map_amount;
        }
        copy.angle = turns * float(M_PI * 2.0);
        copy.shape = shape;
        copy.image = pattern;
        copy.specific = specific;
        float h = height * (1.0f - height_random * r_height);
        if (height_map) {
          h *= height_map->sample_wrapped(center);
        }
        copy.value = h;
        copy.offset = height_offset + height_offset_random * r_raise +
                      (add ? 0.0f : conform * background.sample_wrapped(center));
        copy.random = r_id;
        copies.append(copy);
        patterns.append(pattern);
      }
    }

    const Domain domain = this->output_domain();
    const int2 out_size = domain.data_size;
    const Stamper stamper(copies, images);
    Result &height_result = this->get_result("Height");
    Result &data_1 = this->get_result("Splatter Data 1");
    Result &data_2 = this->get_result("Splatter Data 2");
    for (Result *r : {&height_result, &data_1, &data_2}) {
      if (r->should_compute()) {
        r->allocate_texture(domain);
      }
    }
    parallel_for(out_size, [&](const int2 texel) {
      const float2 uv = (float2(texel) + 0.5f) / float2(out_size);
      const float bg = background.sample_wrapped(uv);
      const Hit hit = stamper.at(uv, Combine::Max);
      float value = bg;
      bool on_shape = false;
      if (hit.copy >= 0) {
        const Copy &copy = copies[hit.copy];
        const float coverage = stamper.shape_of(copy, hit.local);
        if (add) {
          value = bg + coverage * copy.value + copy.offset;
          on_shape = coverage > 0.0f;
        }
        else if (hit.value > bg) {
          value = hit.value;
          on_shape = true;
        }
      }
      if (height_result.should_compute()) {
        height_result.store_pixel(texel, value);
      }
      float4 d1(0.0f, 0.0f, 0.0f, 1.0f);
      float4 d2(0.0f, 0.0f, 0.0f, 1.0f);
      if (on_shape) {
        const Copy &copy = copies[hit.copy];
        d1 = float4(hit.local.x * 0.5f + 0.5f,
                    hit.local.y * 0.5f + 0.5f,
                    float(patterns[hit.copy] + 1) / float(splatter_images),
                    1.0f);
        d2 = float4(copy.random, fract_value(copy.angle / float(M_PI * 2.0)), value, 1.0f);
      }
      if (data_1.should_compute()) {
        data_1.store_pixel(texel, Color(d1));
      }
      if (data_2.should_compute()) {
        data_2.store_pixel(texel, Color(d2));
      }
    });
  }
};

}  // namespace shape_splatter

static void declare_data_inputs(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>("Splatter Data 1")
      .default_value({0.0f, 0.0f, 0.0f, 1.0f})
      .hide_value()
      .structure_type(StructureType::Dynamic)
      .description("Splatter Data 1 of a Shape Splatter node");
  b.add_input<decl::Color>("Splatter Data 2")
      .default_value({0.0f, 0.0f, 0.0f, 1.0f})
      .hide_value()
      .structure_type(StructureType::Dynamic)
      .description("Splatter Data 2 of a Shape Splatter node");
}

/* A random number from the shape's own random number. */
static uint32_t data_state(const float4 &data_2, const uint32_t salt)
{
  return tg::hash(uint32_t(data_2.x * 16777216.0f) ^ salt);
}

namespace shape_splatter_blend {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Color>("Image").structure_type(StructureType::Dynamic);
  declare_data_inputs(b);
  b.add_input<decl::Color>("Background")
      .default_value({0.0f, 0.0f, 0.0f, 1.0f})
      .structure_type(StructureType::Dynamic)
      .description("What shows where there is no shape");
  for (int k = 0; k < splatter_images; k++) {
    b.add_input<decl::Color>("Pattern " + std::to_string(k + 1))
        .default_value({1.0f, 1.0f, 1.0f, 1.0f})
        .compositor_realization_mode(CompositorInputRealizationMode::None)
        .structure_type(StructureType::Dynamic)
        .description("Color or image of the shapes using this pattern, drawn to fill each shape");
  }
  declare_int(b, "Pattern Count", 1, 1, splatter_images, "How many pattern inputs are used");
  declare_factor(b, "Random Pattern", 0.0f, "Share of shapes that use a random pattern input");
  declare_float(b, "Hue Shift", 0.0f, -1.0f, 1.0f, "Shifts the hue of all shapes");
  declare_float(b, "Saturation", 0.0f, -1.0f, 1.0f, "Changes the saturation of all shapes");
  declare_float(b, "Value", 0.0f, -1.0f, 1.0f, "Brightens or darkens all shapes");
  declare_factor(b, "Hue Random", 0.0f, "Random hue shift per shape");
  declare_factor(b, "Saturation Random", 0.0f, "Random saturation change per shape");
  declare_factor(b, "Value Random", 0.0f, "Random brightness change per shape");
  b.add_input<decl::Color>("Overlay")
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .structure_type(StructureType::Dynamic)
      .description("A color or image laid over the shapes");
  declare_factor(b, "Overlay Opacity", 0.0f, "How much the overlay covers the shapes");
  declare_bool(
      b, "Normal Map", false, "The patterns are normal maps, turn their normals with each shape");
}

class Operation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &data_1 = this->get_input("Splatter Data 1");
    const Result &data_2 = this->get_input("Splatter Data 2");
    const Result &background = this->get_input("Background");
    const Result &overlay = this->get_input("Overlay");
    const int count = math::clamp(
        this->get_input("Pattern Count").get_single_value_default<int>(), 1, splatter_images);
    struct Pattern {
      Array<float4> pixels;
      int2 size = int2(0);
      float4 single = float4(1.0f);
    };
    Array<Pattern> patterns(count);
    for (int k = 0; k < count; k++) {
      const Result &input = this->get_input("Pattern " + std::to_string(k + 1));
      if (input.is_single_value()) {
        patterns[k].single = float4(input.get_single_value<Color>());
      }
      else {
        patterns[k].size = input.domain().data_size;
        patterns[k].pixels = tg::read_colors(input, patterns[k].size);
      }
    }
    const auto f = [&](const char *name) {
      return this->get_input(name).get_single_value_default<float>();
    };
    const float random_pattern = f("Random Pattern");
    const float3 shift(f("Hue Shift"), f("Saturation"), f("Value"));
    const float3 shift_random(f("Hue Random"), f("Saturation Random"), f("Value Random"));
    const float overlay_opacity = f("Overlay Opacity");
    const bool normal_map = this->get_input("Normal Map").get_single_value_default<bool>();

    const Domain domain = this->compute_domain();
    Result &output = this->get_result("Image");
    output.allocate_texture(domain);
    parallel_for(domain.data_size, [&](const int2 texel) {
      const float4 d1 = float4(data_1.load_pixel<Color, true>(texel));
      const float4 d2 = float4(data_2.load_pixel<Color, true>(texel));
      const float4 bg = float4(background.load_pixel<Color, true>(texel));
      int id = data_pattern(d1);
      if (id < 0) {
        output.store_pixel(texel, Color(bg));
        return;
      }
      uint32_t state = data_state(d2, 0x9e3779b9u);
      if (random_float(state) < random_pattern) {
        id = int(random_float(state) * float(count));
      }
      const Pattern &pattern = patterns[math::clamp(id, 0, count - 1)];
      float4 color = pattern.single;
      if (pattern.size.x > 0) {
        color = tg::sample_wrapped(pattern.pixels.as_span(),
                                   pattern.size,
                                   math::clamp(float2(d1.x, d1.y), float2(0.0f), float2(1.0f)) *
                                       float2(pattern.size));
      }
      if (normal_map) {
        /* Turn the sideways part of the normal with the shape. */
        const float a = d2.y * float(M_PI * 2.0);
        const float2 n = rotate(float2(color.x, color.y) * 2.0f - 1.0f, a);
        color.x = n.x * 0.5f + 0.5f;
        color.y = n.y * 0.5f + 0.5f;
      }
      else {
        float3 hsv = tg::rgb_to_hsv(color.xyz());
        const float3 r(random_float(state), random_float(state), random_float(state));
        hsv.x = fract_value(hsv.x + shift.x + (r.x - 0.5f) * shift_random.x);
        hsv.y = math::clamp(hsv.y + shift.y + (r.y - 0.5f) * 2.0f * shift_random.y, 0.0f, 1.0f);
        hsv.z = math::max(hsv.z + shift.z + (r.z - 0.5f) * 2.0f * shift_random.z, 0.0f);
        color = float4(tg::hsv_to_rgb(hsv), color.w);
      }
      const float4 over = float4(overlay.load_pixel<Color, true>(texel));
      color = math::interpolate(color, over, overlay_opacity);
      output.store_pixel(texel, Color(color));
    });
  }
};

}  // namespace shape_splatter_blend

namespace shape_splatter_data_extract {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Float>("Mask").structure_type(StructureType::Dynamic).description(
      "White where there is a shape");
  b.add_output<decl::Float>("Pattern").structure_type(StructureType::Dynamic).description(
      "Which pattern the shape uses, starting at 1");
  b.add_output<decl::Float>("Random").structure_type(StructureType::Dynamic).description(
      "A random number per shape");
  b.add_output<decl::Vector>("Shape UV")
      .structure_type(StructureType::Dynamic)
      .description("Position inside the shape, 0 to 1");
  b.add_output<decl::Float>("Rotation").structure_type(StructureType::Dynamic).description(
      "Turn of the shape, a full turn is 1");
  b.add_output<decl::Float>("Height").structure_type(StructureType::Dynamic).description(
      "Height at the pixel");
  declare_data_inputs(b);
}

class Operation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &data_1 = this->get_input("Splatter Data 1");
    const Result &data_2 = this->get_input("Splatter Data 2");
    const Domain domain = this->compute_domain();
    Result &mask = this->get_result("Mask");
    Result &pattern = this->get_result("Pattern");
    Result &random = this->get_result("Random");
    Result &shape_uv = this->get_result("Shape UV");
    Result &rotation = this->get_result("Rotation");
    Result &height = this->get_result("Height");
    for (Result *r : {&mask, &pattern, &random, &shape_uv, &rotation, &height}) {
      if (r->should_compute()) {
        r->allocate_texture(domain);
      }
    }
    parallel_for(domain.data_size, [&](const int2 texel) {
      const float4 d1 = float4(data_1.load_pixel<Color, true>(texel));
      const float4 d2 = float4(data_2.load_pixel<Color, true>(texel));
      const int id = data_pattern(d1);
      const bool on = id >= 0;
      if (mask.should_compute()) {
        mask.store_pixel(texel, on ? 1.0f : 0.0f);
      }
      if (pattern.should_compute()) {
        pattern.store_pixel(texel, float(id + 1));
      }
      if (random.should_compute()) {
        random.store_pixel(texel, on ? d2.x : 0.0f);
      }
      if (shape_uv.should_compute()) {
        shape_uv.store_pixel(texel, on ? float3(d1.x, d1.y, 0.0f) : float3(0.0f));
      }
      if (rotation.should_compute()) {
        rotation.store_pixel(texel, on ? d2.y : 0.0f);
      }
      if (height.should_compute()) {
        height.store_pixel(texel, on ? d2.z : 0.0f);
      }
    });
  }
};

}  // namespace shape_splatter_data_extract

namespace shape_splatter_to_mask {

static const EnumPropertyItem mask_output_items[] = {
    {0, "BINARY", 0, N_("Binary"), N_("White where a chosen shape is")},
    {1, "PATTERN", 0, N_("Pattern Number"), N_("The pattern number of the chosen shapes")},
    {2, "RANDOM", 0, N_("Random Gray"), N_("A random gray per chosen shape")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Float>("Mask").structure_type(StructureType::Dynamic);
  declare_data_inputs(b);
  declare_int(b, "Pattern Start", 1, 1, splatter_images, "First pattern number kept");
  declare_int(
      b, "Pattern End", splatter_images, 1, splatter_images, "Last pattern number kept");
  declare_factor(b, "Random Mask", 0.0f, "Share of the kept shapes left out at random");
  declare_menu(b, "Output", 0, mask_output_items, "What the mask holds");
}

class Operation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &data_1 = this->get_input("Splatter Data 1");
    const Result &data_2 = this->get_input("Splatter Data 2");
    const int start = this->get_input("Pattern Start").get_single_value_default<int>();
    const int end = this->get_input("Pattern End").get_single_value_default<int>();
    const float random_mask = this->get_input("Random Mask").get_single_value_default<float>();
    const int mode = this->get_input("Output").get_single_value_default<MenuValue>().value;
    const Domain domain = this->compute_domain();
    Result &output = this->get_result("Mask");
    output.allocate_texture(domain);
    parallel_for(domain.data_size, [&](const int2 texel) {
      const float4 d1 = float4(data_1.load_pixel<Color, true>(texel));
      const float4 d2 = float4(data_2.load_pixel<Color, true>(texel));
      const int number = data_pattern(d1) + 1;
      float value = 0.0f;
      if (number >= 1 && number >= start && number <= end) {
        uint32_t state = data_state(d2, 0x3c6ef372u);
        if (random_float(state) >= random_mask) {
          value = mode == 1 ? float(number) : (mode == 2 ? d2.x : 1.0f);
        }
      }
      output.store_pixel(texel, value);
    });
  }
};

}  // namespace shape_splatter_to_mask

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tile Random 2
 * \{ */

namespace tile_random_2 {

static void node_declare(NodeDeclarationBuilder &b)
{
  declare_output(b);
  b.add_output<decl::Color>("Shape UV")
      .structure_type(StructureType::Dynamic)
      .description("Position inside each shape in red and green, a random value per shape in blue");
  declare_int(b, "Amount X", 6, 1, 64, "Columns of shapes");
  declare_int(b, "Amount Y", 6, 1, 64, "Rows of shapes");
  declare_factor(b, "Size Random X", 0.5f, "How much the widths vary");
  declare_factor(b, "Size Random Y", 0.5f, "How much the row heights vary");
  declare_factor(b, "Slant Random", 0.0f, "Random lean of the lines between shapes");
  declare_factor(b, "Gap", 0.05f, "Space between shapes, as a part of a cell");
  declare_factor(b, "Corner Radius", 0.0f, "Rounding of the corners, as a part of a cell");
  declare_factor(b, "Bevel", 0.0f, "Width of the slope at the edges, as a part of a cell");
  declare_float(b, "Bevel Curve", 0.0f, -1.0f, 1.0f, "Rounder or sharper bevel");
  declare_factor(b, "Scale", 1.0f, "Size of the shapes in their cells");
  declare_factor(b, "Scale Random", 0.0f, "Random size per shape");
  declare_factor(b, "Luminance Random", 0.5f, "Random brightness per shape");
  declare_tail(b, false);
}

class Operation : public CopiesOperation {
 public:
  using CopiesOperation::CopiesOperation;

  void execute() override
  {
    const int2 amount = math::max(int2(this->i("Amount X"), this->i("Amount Y")), int2(1));
    const float2 size_random(this->f("Size Random X"), this->f("Size Random Y"));
    const float slant_random = this->f("Slant Random");
    const float gap = this->f("Gap");
    const float corner = this->f("Corner Radius");
    const float bevel = this->f("Bevel");
    const float bevel_curve = this->f("Bevel Curve");
    const float scale = this->f("Scale");
    const float scale_random = this->f("Scale Random");
    const float value_random = this->f("Luminance Random");
    const uint32_t seed = this->seed();

    /* Line positions move by at most 0.4 cells so they never cross. */
    const auto row_line = [&](const int j) {
      const int w = ((j % amount.y) + amount.y) % amount.y;
      const float jitter = (tg::hash_to_float(tg::hash(w, 0, seed + 1)) - 0.5f) * 0.8f *
                           size_random.y;
      return (float(j) + jitter) / float(amount.y);
    };
    const auto column_line = [&](const int i, const int row, const float y_in_row) {
      const int w = ((i % amount.x) + amount.x) % amount.x;
      const int r = ((row % amount.y) + amount.y) % amount.y;
      const uint32_t h = tg::hash(w, r, seed + 2);
      const float jitter = (tg::hash_to_float(h) - 0.5f) * 0.8f * size_random.x;
      const float slant = (tg::hash_to_float(tg::hash(h)) - 0.5f) * 0.8f * slant_random;
      return (float(i) + jitter + slant * (y_in_row - 0.5f)) / float(amount.x);
    };

    const Domain domain = this->output_domain();
    const int2 size = domain.data_size;
    Result &output = this->get_result("Image");
    Result &uv_output = this->get_result("Shape UV");
    if (output.should_compute()) {
      output.allocate_texture(domain);
    }
    if (uv_output.should_compute()) {
      uv_output.allocate_texture(domain);
    }
    const float cell = 1.0f / float(math::max(amount.x, amount.y));
    const float pixel = 1.0f / float(math::max(size.x, size.y));
    parallel_for(size, [&](const int2 texel) {
      const float2 uv = (float2(texel) + 0.5f) / float2(size);
      /* Find the row, then the column inside it. */
      int row = int(std::floor(uv.y * float(amount.y)));
      while (uv.y < row_line(row)) {
        row--;
      }
      while (uv.y >= row_line(row + 1)) {
        row++;
      }
      const float bottom = row_line(row);
      const float top = row_line(row + 1);
      const float y_in_row = (uv.y - bottom) / math::max(top - bottom, 1e-6f);
      int column = int(std::floor(uv.x * float(amount.x)));
      while (uv.x < column_line(column, row, y_in_row)) {
        column--;
      }
      while (uv.x >= column_line(column + 1, row, y_in_row)) {
        column++;
      }
      const float left = column_line(column, row, y_in_row);
      const float right = column_line(column + 1, row, y_in_row);

      const int2 id = tg::wrap_texel(int2(column, row), amount);
      uint32_t state = tg::hash(id.x, id.y, seed + 3);
      const float random = random_float(state);
      const float shrink = (1.0f - scale * (1.0f - scale_random * random_float(state))) * 0.5f;
      const float2 extent(right - left, top - bottom);
      const float2 inset = shrink * extent + gap * cell * 0.5f;
      /* Distances to the edges, positive inside. */
      const float2 d(math::min(uv.x - left, right - uv.x) - inset.x,
                     math::min(uv.y - bottom, top - uv.y) - inset.y);
      const float r = corner * cell;
      float sd;
      if (d.x < r && d.y < r) {
        sd = r - math::length(float2(r) - d);
      }
      else {
        sd = math::min(d.x, d.y);
      }
      float v;
      if (bevel > 1e-4f) {
        v = math::clamp(sd / (bevel * cell), 0.0f, 1.0f);
        v = std::pow(v, std::exp2(-bevel_curve * 2.0f));
      }
      else {
        v = math::clamp(sd / pixel + 0.5f, 0.0f, 1.0f);
      }
      v *= 1.0f - value_random * random;
      if (output.should_compute()) {
        output.store_pixel(texel, v);
      }
      if (uv_output.should_compute()) {
        const float2 local = (uv - float2(left, bottom)) / math::max(extent, float2(1e-6f));
        uv_output.store_pixel(
            texel,
            Color(sd > 0.0f ? float4(local.x, local.y, random, 1.0f) : float4(0.0f, 0.0f, 0.0f, 1.0f)));
      }
    });
  }
};

}  // namespace tile_random_2

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shape Mapper
 * \{ */

namespace shape_mapper {

static const EnumPropertyItem path_items[] = {
    {0, "CIRCLE", 0, N_("Circle"), N_("Wrap the image around a circle")},
    {1, "POLYGON", 0, N_("Polygon"), N_("Wrap the image around a polygon")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  declare_output(b);
  b.add_input<decl::Float>("Pattern Image")
      .default_value(1.0f)
      .hide_value()
      .compositor_realization_mode(CompositorInputRealizationMode::None)
      .structure_type(StructureType::Dynamic)
      .description("The image wrapped around the shape");
  declare_menu(b, "Shape", 0, path_items, "What the image is wrapped around");
  declare_int(b, "Pattern Amount", 6, 1, 64, "Copies of the image around the shape");
  b.add_input<decl::Bool>("Side per Copy")
      .default_value(true)
      .usage_by_menu("Shape", 1)
      .description("Give the polygon one side for every copy");
  b.add_input<decl::Int>("Sides")
      .default_value(6)
      .min(3)
      .max(64)
      .usage_by_menu("Shape", 1)
      .description("Number of polygon sides when not one per copy");
  declare_float(b, "Radius", 0.6f, 0.0f, 2.0f, "Size of the shape, one reaches the image edge");
  declare_float(b, "Width", 0.3f, 0.0f, 2.0f, "Width of the band the image is drawn in");
  declare_factor(b, "Rotation", 0.0f, "Turn of the shape");
  declare_bool(b, "Flip Alternate", false, "Turn every other copy upside down");
  b.add_input<decl::Menu>("Resolution")
      .default_value(MenuValue(0))
      .static_items(tg::generator_size_items)
      .description("Size of the image this node makes");
}

class Operation : public CopiesOperation {
 public:
  using CopiesOperation::CopiesOperation;

  void execute() override
  {
    const Picture image = read_picture(this->get_input("Pattern Image"));
    const bool polygon = this->m("Shape") == 1;
    const int amount = math::max(this->i("Pattern Amount"), 1);
    const int sides = this->b("Side per Copy") ? math::max(amount, 3) :
                                                 math::clamp(this->i("Sides"), 3, 64);
    const float radius = this->f("Radius");
    const float width = math::max(this->f("Width"), 1e-4f);
    const float rotation = this->f("Rotation");
    const bool flip_alternate = this->b("Flip Alternate");

    const Domain domain = this->output_domain();
    const int2 size = domain.data_size;
    Result &output = this->get_result("Image");
    output.allocate_texture(domain);
    parallel_for(size, [&](const int2 texel) {
      const float2 p = ((float2(texel) + 0.5f) / float2(size) - 0.5f) * 2.0f;
      /* Turns clockwise from the right. */
      const float turn = fract_value(-std::atan2(p.y, p.x) / float(M_PI * 2.0) - rotation);
      float around;
      float distance;
      if (polygon) {
        const float side_turn = 1.0f / float(sides);
        const int side = math::min(int(turn / side_turn), sides - 1);
        const float a0 = -(float(side) * side_turn + rotation) * float(M_PI * 2.0);
        const float a1 = -(float(side + 1) * side_turn + rotation) * float(M_PI * 2.0);
        const float2 v0(math::cos(a0), math::sin(a0));
        const float2 v1(math::cos(a1), math::sin(a1));
        const float2 normal = math::normalize(v0 + v1);
        const float apothem = math::cos(float(M_PI) / float(sides));
        distance = math::dot(p, normal) / apothem;
        /* Where along the side the pixel is, measured on the side itself. */
        const float2 on_side = p / math::max(distance, 1e-6f);
        const float t = math::clamp(math::dot(on_side - v0, v1 - v0) / math::length_squared(v1 - v0),
                                    0.0f,
                                    1.0f);
        around = (float(side) + t) / float(sides);
      }
      else {
        around = turn;
        distance = math::length(p);
      }
      const float u_all = around * float(amount);
      const int copy = int(std::floor(u_all));
      float v = (distance - radius) / width + 0.5f;
      if (flip_alternate && (copy & 1)) {
        v = 1.0f - v;
      }
      const float value = (v < 0.0f || v > 1.0f) ? 0.0f :
                                                   image.sample_clamped(float2(fract_value(u_all), v));
      output.store_pixel(texel, value);
    });
  }
};

}  // namespace shape_mapper

/** \} */

/* -------------------------------------------------------------------- */
/** \name Registration
 * \{ */

template<typename T> static NodeOperation *make_operation(Context &context, const bNode &node)
{
  return new T(context, node);
}

static void register_one(bke::bNodeType &ntype,
                         const char *idname,
                         const char *name,
                         const char *description,
                         const char *enum_name,
                         bke::NodeDeclareFunction declare,
                         bke::NodeGetCompositorOperationFunction operation)
{
  tg_node_type_base(&ntype, idname);
  ntype.ui_name = name;
  ntype.ui_description = description;
  ntype.enum_name_legacy = enum_name;
  ntype.nclass = NODE_CLASS_TEXTURE;
  ntype.declare = declare;
  ntype.flag |= NODE_PREVIEW;
  ntype.get_compositor_operation = operation;
  bke::node_register_type(ntype);
}

static void node_register()
{
  static bke::bNodeType types[13];
  register_one(types[0],
               "TextureGraphNodeTileGenerator",
               "Tile Generator",
               "Lay out a grid of shapes or images",
               "TEXTURE_GRAPH_TILE_GENERATOR",
               tile_generator::node_declare,
               make_operation<tile_generator::Operation>);
  register_one(types[1],
               "TextureGraphNodeTileSampler",
               "Tile Sampler",
               "Lay out a grid of shapes or images, with maps that change every tile",
               "TEXTURE_GRAPH_TILE_SAMPLER",
               tile_sampler::node_declare,
               make_operation<tile_sampler::Operation>);
  register_one(types[2],
               "TextureGraphNodeTileRandom",
               "Tile Random",
               "Tiles split at random into smaller tiles",
               "TEXTURE_GRAPH_TILE_RANDOM",
               tile_random::node_declare,
               make_operation<tile_random::Operation>);
  register_one(types[3],
               "TextureGraphNodeTileRandom2",
               "Tile Random 2",
               "Rows of shapes of random width and height, touching each other",
               "TEXTURE_GRAPH_TILE_RANDOM_2",
               tile_random_2::node_declare,
               make_operation<tile_random_2::Operation>);
  register_one(types[4],
               "TextureGraphNodeSplatter",
               "Splatter",
               "Stamp an image many times, scattered and turned",
               "TEXTURE_GRAPH_SPLATTER",
               splatter::node_declare,
               make_operation<splatter::Operation>);
  register_one(types[5],
               "TextureGraphNodeSplatterCircular",
               "Splatter Circular",
               "Place shapes or images on rings",
               "TEXTURE_GRAPH_SPLATTER_CIRCULAR",
               splatter_circular::node_declare,
               make_operation<splatter_circular::Operation>);
  register_one(types[6],
               "TextureGraphNodeArcPavement",
               "Arc Pavement",
               "Stones laid in fans of arcs",
               "TEXTURE_GRAPH_ARC_PAVEMENT",
               arc_pavement::node_declare,
               make_operation<arc_pavement::Operation>);
  register_one(types[7],
               "TextureGraphNodeShapeMapper",
               "Shape Mapper",
               "Wrap an image around a circle or polygon",
               "TEXTURE_GRAPH_SHAPE_MAPPER",
               shape_mapper::node_declare,
               make_operation<shape_mapper::Operation>);
  register_one(types[8],
               "TextureGraphNodeScratchesGenerator",
               "Scratches Generator",
               "Scatter wavy scratches",
               "TEXTURE_GRAPH_SCRATCHES_GENERATOR",
               scratches_generator::node_declare,
               make_operation<scratches_generator::Operation>);
  register_one(types[9],
               "TextureGraphNodeShapeSplatter",
               "Shape Splatter",
               "Scatter shapes over a height, with data to make matching maps",
               "TEXTURE_GRAPH_SHAPE_SPLATTER",
               shape_splatter::node_declare,
               make_operation<shape_splatter::Operation>);
  register_one(types[10],
               "TextureGraphNodeShapeSplatterBlend",
               "Shape Splatter Blend",
               "Color the shapes of a Shape Splatter, each with its own pattern",
               "TEXTURE_GRAPH_SHAPE_SPLATTER_BLEND",
               shape_splatter_blend::node_declare,
               make_operation<shape_splatter_blend::Operation>);
  register_one(types[11],
               "TextureGraphNodeShapeSplatterDataExtract",
               "Shape Splatter Data Extract",
               "Split the data of a Shape Splatter into separate maps",
               "TEXTURE_GRAPH_SHAPE_SPLATTER_DATA_EXTRACT",
               shape_splatter_data_extract::node_declare,
               make_operation<shape_splatter_data_extract::Operation>);
  register_one(types[12],
               "TextureGraphNodeShapeSplatterToMask",
               "Shape Splatter to Mask",
               "A mask of chosen shapes of a Shape Splatter",
               "TEXTURE_GRAPH_SHAPE_SPLATTER_TO_MASK",
               shape_splatter_to_mask::node_declare,
               make_operation<shape_splatter_to_mask::Operation>);
}
NOD_REGISTER_NODE(node_register)

/** \} */

}  // namespace blender::nodes::node_texture_graph_tile_cc
