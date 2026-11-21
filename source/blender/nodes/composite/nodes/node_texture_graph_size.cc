/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Size: changes the pixel size and bit depth that the following nodes work at. The image still
 * covers the same area, so a smaller size is stretched back to fit where it meets other images,
 * and the graph output is always stretched to the graph size.
 */

#include "BLI_math_half.hh"
#include "BLI_math_vector.hh"
#include "BLI_task.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"
#include "texture_graph_util.hh"

namespace blender::nodes::node_texture_graph_size_cc {

enum class SizeMode : int {
  Relative = 0,
  Absolute = 1,
};

enum class Depth : int {
  Full = 0,
  Half = 1,
  Bits16 = 2,
  Bits8 = 3,
};

static const EnumPropertyItem mode_items[] = {
    {int(SizeMode::Relative), "RELATIVE", 0, N_("Relative"), N_("Scale the incoming size")},
    {int(SizeMode::Absolute), "ABSOLUTE", 0, N_("Absolute"), N_("Set the size in pixels")},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem depth_items[] = {
    {int(Depth::Full), "FULL", 0, N_("32-bit Float"), N_("Keep full precision")},
    {int(Depth::Half), "HALF", 0, N_("16-bit Float"), N_("Round values to half floats")},
    {int(Depth::Bits16), "16BIT", 0, N_("16-bit"), N_("Round values to 65536 steps from 0 to 1")},
    {int(Depth::Bits8), "8BIT", 0, N_("8-bit"), N_("Round values to 256 steps from 0 to 1")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Color>("Image")
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .hide_value()
      .structure_type(StructureType::Dynamic);
  b.add_output<decl::Color>("Image").structure_type(StructureType::Dynamic).align_with_previous();

  b.add_input<decl::Menu>("Mode")
      .default_value(MenuValue(SizeMode::Relative))
      .static_items(mode_items)
      .optional_label();
  b.add_input<decl::Float>("Scale")
      .default_value(0.5f)
      .min(1.0f / 64.0f)
      .max(16.0f)
      .usage_by_menu("Mode", int(SizeMode::Relative))
      .description("Factor for the incoming size, 0.5 halves it");
  b.add_input<decl::Int>("Width")
      .default_value(512)
      .min(1)
      .max(16384)
      .usage_by_menu("Mode", int(SizeMode::Absolute));
  b.add_input<decl::Int>("Height")
      .default_value(512)
      .min(1)
      .max(16384)
      .usage_by_menu("Mode", int(SizeMode::Absolute));
  b.add_input<decl::Menu>("Bit Depth")
      .default_value(MenuValue(Depth::Full))
      .static_items(depth_items)
      .description("Round the values as if they were stored with fewer bits");
}

using namespace blender::compositor;

static float quantize(const float value, const Depth depth)
{
  switch (depth) {
    case Depth::Full:
      return value;
    case Depth::Half:
      return math::half_to_float(math::float_to_half(value));
    case Depth::Bits16:
      return math::round(math::clamp(value, 0.0f, 1.0f) * 65535.0f) / 65535.0f;
    case Depth::Bits8:
      return math::round(math::clamp(value, 0.0f, 1.0f) * 255.0f) / 255.0f;
  }
  return value;
}

class SizeOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &image = this->get_input("Image");
    Result &output = this->get_result("Image");
    const Depth depth = Depth(this->get_input("Bit Depth").get_single_value_default<MenuValue>().value);

    if (image.is_single_value()) {
      output.allocate_single_value();
      output.set_single_value(Color(this->quantize_color(float4(image.get_single_value<Color>()), depth)));
      return;
    }

    const Domain input_domain = image.domain();
    const int2 input_size = input_domain.data_size;
    int2 size = input_size;
    const SizeMode mode = SizeMode(this->get_input("Mode").get_single_value_default<MenuValue>().value);
    if (mode == SizeMode::Relative) {
      const float scale = this->get_input("Scale").get_single_value_default<float>();
      size = int2(math::round(float2(input_size) * scale));
    }
    else {
      size = int2(this->get_input("Width").get_single_value_default<int>(),
                  this->get_input("Height").get_single_value_default<int>());
      /* A draft run makes every image smaller by the same amount. */
      size = int2(math::round(float2(size) * this->context().draft_scale()));
    }
    size = math::clamp(size, int2(1), int2(16384));

    if (size == input_size && depth == Depth::Full) {
      output.share_data(image);
      return;
    }

    const Domain domain = texture_graph::resized_domain(input_domain, size);
    output.allocate_texture(domain);

    /* Source pixels covered by one output pixel. Shrinking averages all of them, growing
     * blends the nearest ones. */
    const float2 footprint = float2(input_size) / float2(size);
    const int2 box = math::max(int2(math::ceil(footprint)), int2(1));
    const bool shrink = footprint.x > 1.0f || footprint.y > 1.0f;

    parallel_for(size, [&](const int2 texel) {
      float4 color;
      if (shrink) {
        const int2 start = int2(math::floor(float2(texel) * footprint));
        float4 sum(0.0f);
        for (int y = 0; y < box.y; y++) {
          for (int x = 0; x < box.x; x++) {
            sum += float4(image.load_pixel<Color>(
                texture_graph::wrap_texel(start + int2(x, y), input_size)));
          }
        }
        color = sum / float(box.x * box.y);
      }
      else {
        const float2 coordinates = (float2(texel) + 0.5f) / float2(size);
        color = float4(image.sample<Color>(
            coordinates, Interpolation::Bilinear, Extension::Repeat, Extension::Repeat));
      }
      output.store_pixel(texel, Color(this->quantize_color(color, depth)));
    });
  }

  static float4 quantize_color(const float4 &color, const Depth depth)
  {
    return float4(quantize(color.x, depth),
                  quantize(color.y, depth),
                  quantize(color.z, depth),
                  quantize(color.w, depth));
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new SizeOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;

  tg_node_type_base(&ntype, "TextureGraphNodeSize");
  ntype.ui_name = "Size";
  ntype.ui_description =
      "Change the pixel size and bit depth that the following nodes work at";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_SIZE";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.flag |= NODE_PREVIEW;
  ntype.get_compositor_operation = get_compositor_operation;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_size_cc
