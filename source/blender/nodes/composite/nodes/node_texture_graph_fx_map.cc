/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * FX-Map: scatters a pattern over the image. The image is split into a grid by halving it again
 * and again (a quadtree), and the pattern is placed in every cell of the chosen level, each copy
 * with its own random offset, size, rotation and brightness. Copies crossing an edge come back on
 * the other side, so the result tiles.
 */

#include "BLI_array.hh"
#include "BLI_math_vector.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"
#include "texture_graph_util.hh"

namespace blender::nodes::node_texture_graph_fx_map_cc {

enum class Shape : int {
  Square = 0,
  Disc = 1,
  SoftDisc = 2,
};

static const EnumPropertyItem shape_items[] = {
    {int(Shape::Square), "SQUARE", 0, N_("Square"), ""},
    {int(Shape::Disc), "DISC", 0, N_("Disc"), ""},
    {int(Shape::SoftDisc), "SOFT_DISC", 0, N_("Soft Disc"), N_("A disc that fades out to its rim")},
    {0, nullptr, 0, nullptr, nullptr},
};

enum class Blend : int {
  Add = 0,
  Max = 1,
  Over = 2,
};

static const EnumPropertyItem blend_items[] = {
    {int(Blend::Add), "ADD", 0, N_("Add"), N_("Copies add up where they overlap")},
    {int(Blend::Max), "MAX", 0, N_("Max"), N_("The brightest copy wins where they overlap")},
    {int(Blend::Over), "OVER", 0, N_("Over"), N_("Each copy is laid over the ones before it")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Color>("Image").structure_type(StructureType::Dynamic);
  b.add_output<decl::Float>("Mask")
      .structure_type(StructureType::Dynamic)
      .description("Where the copies are");

  b.add_input<decl::Color>("Pattern")
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .hide_value()
      .compositor_realization_mode(CompositorInputRealizationMode::None)
      .structure_type(StructureType::Dynamic)
      .description("Image to scatter, the shape below is used when nothing is connected");
  b.add_input<decl::Menu>("Shape")
      .default_value(MenuValue(Shape::Disc))
      .static_items(shape_items)
      .optional_label();
  b.add_input<decl::Int>("Level").default_value(3).min(0).max(8).description(
      "How many times the image is split in four: 0 is one cell, 3 is 8 x 8 cells");
  b.add_input<decl::Int>("Count").default_value(1).min(1).max(64).description(
      "Copies in every cell");
  b.add_input<decl::Float>("Size")
      .default_value(0.8f)
      .min(0.0f)
      .max(8.0f)
      .description("Size of a copy, as a part of its cell");
  b.add_input<decl::Float>("Size Random").default_value(0.0f).min(0.0f).max(1.0f).subtype(
      PROP_FACTOR);
  b.add_input<decl::Float>("Offset Random")
      .default_value(0.0f)
      .min(0.0f)
      .max(4.0f)
      .description("How far a copy can move from the middle of its cell, in cells");
  b.add_input<decl::Float>("Rotation").default_value(0.0f).subtype(PROP_ANGLE);
  b.add_input<decl::Float>("Rotation Random").default_value(0.0f).min(0.0f).max(1.0f).subtype(
      PROP_FACTOR);
  b.add_input<decl::Float>("Luminance").default_value(1.0f).min(0.0f).max(1.0f).subtype(
      PROP_FACTOR);
  b.add_input<decl::Float>("Luminance Random").default_value(0.0f).min(0.0f).max(1.0f).subtype(
      PROP_FACTOR);
  b.add_input<decl::Menu>("Blend")
      .default_value(MenuValue(Blend::Max))
      .static_items(blend_items)
      .optional_label();
  b.add_input<decl::Int>("Seed").default_value(0).description(
      "Change it to get a different scatter");
  b.add_input<decl::Menu>("Resolution")
      .default_value(MenuValue(0))
      .static_items(texture_graph::generator_size_items)
      .description("Size of the image this node makes");
}

using namespace blender::compositor;

struct Copy {
  float2 center;
  /* Width and height in pixels. */
  float2 size;
  /* Cosine and sine of the inverse rotation, to go from the image into the copy. */
  float cos;
  float sin;
  float luminance;
};

/* Pixels per side of the tiles copies are sorted into, so each tile only looks at the copies
 * that reach it. */
constexpr int tile_size = 32;
/* Upper bound on the number of copies, so a large level and count stay usable. */
constexpr int64_t max_copies = int64_t(1) << 20;

class FXMapOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &pattern = this->get_input("Pattern");
    const Shape shape = Shape(this->get_input("Shape").get_single_value_default<MenuValue>().value);
    const int level = math::clamp(this->get_input("Level").get_single_value_default<int>(), 0, 8);
    const int count = math::clamp(this->get_input("Count").get_single_value_default<int>(), 1, 64);
    const float size_factor = math::max(this->get_input("Size").get_single_value_default<float>(),
                                        0.0f);
    const float size_random = this->get_input("Size Random").get_single_value_default<float>();
    const float offset_random = this->get_input("Offset Random").get_single_value_default<float>();
    const float rotation = this->get_input("Rotation").get_single_value_default<float>();
    const float rotation_random =
        this->get_input("Rotation Random").get_single_value_default<float>();
    const float luminance = this->get_input("Luminance").get_single_value_default<float>();
    const float luminance_random =
        this->get_input("Luminance Random").get_single_value_default<float>();
    const Blend blend = Blend(this->get_input("Blend").get_single_value_default<MenuValue>().value);
    const uint32_t seed = uint32_t(this->get_input("Seed").get_single_value_default<int>());
    const int resolution = this->get_input("Resolution").get_single_value_default<MenuValue>().value;

    const Domain domain = texture_graph::generator_domain(this->context(), resolution);
    const int2 size = domain.data_size;

    /* Copy the pattern into a plain buffer, it is drawn with nothing around it. */
    int2 pattern_size(0);
    Array<float4> pattern_pixels;
    if (!pattern.is_single_value()) {
      pattern_size = pattern.domain().data_size;
      pattern_pixels = Array<float4>(int64_t(pattern_size.x) * pattern_size.y);
      parallel_for(pattern_size, [&](const int2 texel) {
        pattern_pixels[int64_t(texel.y) * pattern_size.x + texel.x] = float4(
            pattern.load_pixel<Color>(texel));
      });
    }
    const float4 pattern_tint = pattern.is_single_value() ?
                                    float4(pattern.get_single_value<Color>()) :
                                    float4(1.0f);

    const Vector<Copy> copies = this->make_copies(size,
                                                  level,
                                                  count,
                                                  size_factor,
                                                  size_random,
                                                  offset_random,
                                                  rotation,
                                                  rotation_random,
                                                  luminance,
                                                  luminance_random,
                                                  seed);

    /* Sort the copies into the tiles they reach. */
    const int2 tiles = math::divide_ceil(size, int2(tile_size));
    Array<Vector<int>> tile_copies(int64_t(tiles.x) * tiles.y);
    for (const int i : copies.index_range()) {
      this->add_to_tiles(copies[i], i, size, tiles, tile_copies);
    }

    Result &image = this->get_result("Image");
    Result &mask = this->get_result("Mask");
    if (image.should_compute()) {
      image.allocate_texture(domain);
    }
    if (mask.should_compute()) {
      mask.allocate_texture(domain);
    }

    threading::parallel_for(tile_copies.index_range(), 1, [&](const IndexRange range) {
      for (const int64_t tile_index : range) {
        const int2 tile(int(tile_index % tiles.x), int(tile_index / tiles.x));
        const Vector<int> &list = tile_copies[tile_index];
        const int2 start = tile * tile_size;
        const int2 end = math::min(start + tile_size, size);
        for (int y = start.y; y < end.y; y++) {
          for (int x = start.x; x < end.x; x++) {
            float4 sum(0.0f);
            for (const int i : list) {
              const float4 color = this->copy_color(
                  copies[i], int2(x, y), size, shape, pattern_pixels, pattern_size) *
                                   pattern_tint;
              switch (blend) {
                case Blend::Add:
                  sum += color;
                  break;
                case Blend::Max:
                  sum = math::max(sum, color);
                  break;
                case Blend::Over:
                  sum = color + sum * (1.0f - color.w);
                  break;
              }
            }
            if (image.should_compute()) {
              image.store_pixel(int2(x, y), Color(float4(sum.xyz(), 1.0f)));
            }
            if (mask.should_compute()) {
              mask.store_pixel(int2(x, y), math::clamp(sum.w, 0.0f, 1.0f));
            }
          }
        }
      }
    });
  }

  Vector<Copy> make_copies(const int2 size,
                           const int level,
                           const int count,
                           const float size_factor,
                           const float size_random,
                           const float offset_random,
                           const float rotation,
                           const float rotation_random,
                           const float luminance,
                           const float luminance_random,
                           const uint32_t seed)
  {
    const int cells = 1 << level;
    const float2 cell_size = float2(size) / float(cells);
    const int64_t total = math::min(int64_t(cells) * cells * count, max_copies);
    Vector<Copy> copies;
    copies.reserve(total);
    for (int cy = 0; cy < cells; cy++) {
      for (int cx = 0; cx < cells; cx++) {
        for (int k = 0; k < count; k++) {
          if (copies.size() >= total) {
            return copies;
          }
          const uint32_t h = texture_graph::hash(cx, cy, texture_graph::hash(seed) + uint32_t(k));
          const auto random = [&](const uint32_t channel) {
            return texture_graph::hash_to_float(texture_graph::hash(h ^ (channel * 0x9e3779b9U)));
          };
          const float2 jitter = float2(random(1) - 0.5f, random(2) - 0.5f) * offset_random;
          const float scale = size_factor * (1.0f - size_random * random(3));
          const float angle = rotation + rotation_random * (random(4) - 0.5f) * float(M_PI * 2.0);
          Copy copy;
          copy.center = (float2(cx, cy) + 0.5f + jitter) * cell_size;
          copy.size = cell_size * scale;
          copy.cos = math::cos(-angle);
          copy.sin = math::sin(-angle);
          copy.luminance = luminance * (1.0f - luminance_random * random(5));
          copies.append(copy);
        }
      }
    }
    return copies;
  }

  void add_to_tiles(const Copy &copy,
                    const int index,
                    const int2 size,
                    const int2 tiles,
                    Array<Vector<int>> &tile_copies)
  {
    if (copy.size.x <= 0.0f || copy.size.y <= 0.0f) {
      return;
    }
    /* Half the diagonal reaches every corner however the copy is turned, plus a pixel for the
     * soft edge. */
    const float radius = math::length(copy.size) * 0.5f + 1.0f;
    const auto tile_range = [&](const float center, const int length, const int tile_count) {
      Vector<int> result;
      if (radius * 2.0f >= float(length)) {
        for (int t = 0; t < tile_count; t++) {
          result.append(t);
        }
        return result;
      }
      const int first = int(math::floor(center - radius));
      const int last = int(math::ceil(center + radius));
      int previous = -1;
      for (int p = first; p <= last;) {
        const int wrapped = ((p % length) + length) % length;
        const int t = wrapped / tile_size;
        if (t != previous && !result.contains(t)) {
          result.append(t);
        }
        previous = t;
        /* Jump to the start of the next tile, or the wrap point, whichever comes first. */
        const int to_next_tile = tile_size - wrapped % tile_size;
        const int to_wrap = length - wrapped;
        p += math::max(1, math::min(to_next_tile, to_wrap));
      }
      return result;
    };
    const Vector<int> xs = tile_range(copy.center.x, size.x, tiles.x);
    const Vector<int> ys = tile_range(copy.center.y, size.y, tiles.y);
    for (const int ty : ys) {
      for (const int tx : xs) {
        tile_copies[int64_t(ty) * tiles.x + tx].append(index);
      }
    }
  }

  float4 copy_color(const Copy &copy,
                    const int2 texel,
                    const int2 size,
                    const Shape shape,
                    const Array<float4> &pattern_pixels,
                    const int2 pattern_size) const
  {
    const float2 delta = texture_graph::wrapped_delta(float2(texel) + 0.5f - copy.center,
                                                      float2(size));
    const float2 turned(delta.x * copy.cos - delta.y * copy.sin,
                        delta.x * copy.sin + delta.y * copy.cos);
    const float2 uv = turned / copy.size + 0.5f;
    if (uv.x < -0.01f || uv.y < -0.01f || uv.x > 1.01f || uv.y > 1.01f) {
      return float4(0.0f);
    }

    float4 color;
    if (pattern_size.x > 0) {
      color = texture_graph::sample_buffer_zero(
          pattern_pixels.data(), pattern_size, uv * float2(pattern_size));
    }
    else {
      /* One pixel of soft edge, measured in the copy's own units. */
      const float edge = 1.0f / math::max(math::min(copy.size.x, copy.size.y), 1.0f);
      const float2 p = uv - 0.5f;
      float coverage = 0.0f;
      switch (shape) {
        case Shape::Square:
          coverage = (0.5f - math::max(math::abs(p.x), math::abs(p.y))) / edge + 0.5f;
          break;
        case Shape::Disc:
          coverage = (0.5f - math::length(p)) / edge + 0.5f;
          break;
        case Shape::SoftDisc: {
          const float t = math::clamp(1.0f - math::length(p) * 2.0f, 0.0f, 1.0f);
          coverage = t * t * (3.0f - 2.0f * t);
          break;
        }
      }
      color = float4(math::clamp(coverage, 0.0f, 1.0f));
    }
    return float4(color.xyz() * copy.luminance, color.w);
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new FXMapOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;

  tg_node_type_base(&ntype, "TextureGraphNodeFXMap");
  ntype.ui_name = "FX-Map";
  ntype.ui_description =
      "Scatter a pattern over a grid of cells, each copy with its own random offset, size, "
      "rotation and brightness";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_FX_MAP";
  ntype.nclass = NODE_CLASS_TEXTURE;
  ntype.declare = node_declare;
  ntype.flag |= NODE_PREVIEW;
  ntype.get_compositor_operation = get_compositor_operation;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_fx_map_cc
