/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Tiling Noise: noise that repeats exactly at the image edges, so it always tiles. The noise
 * lattice has a whole number of cells across the image and wraps around.
 */

#include <limits>

#include "BLI_math_vector.hh"
#include "BLI_task.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"
#include "texture_graph_util.hh"

namespace blender::nodes::node_texture_graph_tiling_noise_cc {

enum class NoiseType : int {
  Smooth = 0,
  Cells = 1,
};

static const EnumPropertyItem type_items[] = {
    {int(NoiseType::Smooth), "SMOOTH", 0, N_("Smooth"), N_("Soft clouds made of layers")},
    {int(NoiseType::Cells), "CELLS", 0, N_("Cells"), N_("Distance to scattered points")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Float>("Value").structure_type(StructureType::Dynamic);
  b.add_output<decl::Color>("Color")
      .structure_type(StructureType::Dynamic)
      .description("A random color per cell, or the value as gray for smooth noise");

  b.add_input<decl::Menu>("Type")
      .default_value(MenuValue(NoiseType::Smooth))
      .static_items(type_items)
      .optional_label();
  b.add_input<decl::Int>("Scale").default_value(4).min(1).max(256).description(
      "Number of noise cells across the image");
  b.add_input<decl::Int>("Detail")
      .default_value(5)
      .min(1)
      .max(12)
      .usage_by_menu("Type", int(NoiseType::Smooth))
      .description("Number of finer layers added on top");
  b.add_input<decl::Float>("Roughness")
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .usage_by_menu("Type", int(NoiseType::Smooth))
      .description("How strong each finer layer is");
  b.add_input<decl::Float>("Randomness")
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .usage_by_menu("Type", int(NoiseType::Cells))
      .description("How far the points move from a regular grid");
  b.add_input<decl::Int>("Seed").default_value(0).description(
      "Change it to get a different pattern");
  b.add_input<decl::Menu>("Resolution")
      .default_value(MenuValue(0))
      .static_items(texture_graph::generator_size_items)
      .description("Size of the image this node makes");
}

using namespace blender::compositor;

/* Gradient noise on a lattice that repeats every `period` cells, roughly in [-1, 1]. */
static float periodic_gradient_noise(const float2 p, const int period, const uint32_t seed)
{
  const int2 cell = int2(math::floor(p));
  const float2 f = p - float2(cell);
  const auto corner = [&](const int2 offset) {
    const int2 c = texture_graph::wrap_texel(cell + offset, int2(period));
    const float angle = texture_graph::hash_to_float(texture_graph::hash(c.x, c.y, seed)) *
                        float(M_PI * 2.0);
    const float2 gradient(math::cos(angle), math::sin(angle));
    return math::dot(gradient, f - float2(offset));
  };
  const float2 u = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f);
  const float bottom = math::interpolate(corner(int2(0, 0)), corner(int2(1, 0)), u.x);
  const float top = math::interpolate(corner(int2(0, 1)), corner(int2(1, 1)), u.x);
  return math::interpolate(bottom, top, u.y) * float(M_SQRT2);
}

class TilingNoiseOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const NoiseType type = NoiseType(
        this->get_input("Type").get_single_value_default<MenuValue>().value);
    const int scale = math::max(this->get_input("Scale").get_single_value_default<int>(), 1);
    const int detail = math::clamp(this->get_input("Detail").get_single_value_default<int>(), 1, 12);
    const float roughness = this->get_input("Roughness").get_single_value_default<float>();
    const float randomness = this->get_input("Randomness").get_single_value_default<float>();
    const uint32_t seed = uint32_t(this->get_input("Seed").get_single_value_default<int>());
    const int resolution = this->get_input("Resolution").get_single_value_default<MenuValue>().value;

    const Domain domain = texture_graph::generator_domain(this->context(), resolution);
    const int2 size = domain.data_size;

    Result &value_result = this->get_result("Value");
    Result &color_result = this->get_result("Color");
    if (value_result.should_compute()) {
      value_result.allocate_texture(domain);
    }
    if (color_result.should_compute()) {
      color_result.allocate_texture(domain);
    }

    parallel_for(size, [&](const int2 texel) {
      const float2 uv = (float2(texel) + 0.5f) / float2(size);
      float value = 0.0f;
      float4 color(0.0f);
      if (type == NoiseType::Smooth) {
        value = this->smooth(uv, scale, detail, roughness, seed);
        color = float4(float3(value), 1.0f);
      }
      else {
        this->cells(uv, scale, randomness, seed, value, color);
      }
      if (value_result.should_compute()) {
        value_result.store_pixel(texel, value);
      }
      if (color_result.should_compute()) {
        color_result.store_pixel(texel, Color(color));
      }
    });
  }

  static float smooth(const float2 uv,
                      const int scale,
                      const int detail,
                      const float roughness,
                      const uint32_t seed)
  {
    float sum = 0.0f;
    float amplitude = 1.0f;
    float total = 0.0f;
    int period = scale;
    for (int octave = 0; octave < detail; octave++) {
      sum += periodic_gradient_noise(uv * float(period), period, seed + uint32_t(octave) * 1013u) *
             amplitude;
      total += amplitude;
      amplitude *= roughness;
      period *= 2;
    }
    return math::clamp(sum / total * 0.5f + 0.5f, 0.0f, 1.0f);
  }

  static void cells(const float2 uv,
                    const int scale,
                    const float randomness,
                    const uint32_t seed,
                    float &r_value,
                    float4 &r_color)
  {
    const float2 p = uv * float2(scale);
    const int2 cell = int2(math::floor(p));
    float best = std::numeric_limits<float>::max();
    uint32_t best_hash = 0;
    for (int y = -1; y <= 1; y++) {
      for (int x = -1; x <= 1; x++) {
        const int2 neighbor = cell + int2(x, y);
        const int2 wrapped = texture_graph::wrap_texel(neighbor, int2(scale));
        const uint32_t h = texture_graph::hash(wrapped.x, wrapped.y, seed);
        const float2 jitter = float2(texture_graph::hash_to_float(h),
                                     texture_graph::hash_to_float(texture_graph::hash(h))) -
                              0.5f;
        const float2 point = float2(neighbor) + 0.5f + jitter * randomness;
        const float distance = math::distance(p, point);
        if (distance < best) {
          best = distance;
          best_hash = h;
        }
      }
    }
    r_value = math::clamp(best, 0.0f, 1.0f);
    const uint32_t h1 = texture_graph::hash(best_hash ^ 0x9e3779b9u);
    const uint32_t h2 = texture_graph::hash(h1);
    const uint32_t h3 = texture_graph::hash(h2);
    r_color = float4(texture_graph::hash_to_float(h1),
                     texture_graph::hash_to_float(h2),
                     texture_graph::hash_to_float(h3),
                     1.0f);
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new TilingNoiseOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;

  tg_node_type_base(&ntype, "TextureGraphNodeTilingNoise");
  ntype.ui_name = "Tiling Noise";
  ntype.ui_description = "Noise that repeats exactly at the image edges";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_TILING_NOISE";
  ntype.nclass = NODE_CLASS_TEXTURE;
  ntype.declare = node_declare;
  ntype.flag |= NODE_PREVIEW;
  ntype.get_compositor_operation = get_compositor_operation;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_tiling_noise_cc
