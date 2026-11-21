/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Sample: reads an image at any position. Positions past an edge come from the other side, so
 * sampling keeps the result tiling. With a single position it gives a single value, which is how
 * a function reads one pixel of an image.
 */

#include "BLI_math_vector.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"
#include "texture_graph_util.hh"

namespace blender::nodes::node_texture_graph_sample_cc {

enum class Filter : int {
  Linear = 0,
  Nearest = 1,
};

static const EnumPropertyItem filter_items[] = {
    {int(Filter::Linear), "LINEAR", 0, N_("Linear"), N_("Blend the four nearest pixels")},
    {int(Filter::Nearest), "NEAREST", 0, N_("Nearest"), N_("Take the closest pixel as it is")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Color>("Color").structure_type(StructureType::Dynamic);
  b.add_output<decl::Float>("Value")
      .structure_type(StructureType::Dynamic)
      .description("Average of the red, green and blue of the sampled color");

  b.add_input<decl::Color>("Image")
      .default_value({0.0f, 0.0f, 0.0f, 1.0f})
      .hide_value()
      .compositor_realization_mode(CompositorInputRealizationMode::None)
      .structure_type(StructureType::Dynamic);
  b.add_input<decl::Vector>("Position")
      .dimensions(2)
      .default_value({0.5f, 0.5f})
      .structure_type(StructureType::Dynamic)
      .description("Where to read the image, from (0, 0) at the bottom left to (1, 1) at the "
                   "top right");
  b.add_input<decl::Menu>("Filter")
      .default_value(MenuValue(Filter::Linear))
      .static_items(filter_items)
      .optional_label();
}

using namespace blender::compositor;

class SampleOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &image = this->get_input("Image");
    const Result &position = this->get_input("Position");
    Result &color_result = this->get_result("Color");
    Result &value_result = this->get_result("Value");
    const Interpolation interpolation =
        Filter(this->get_input("Filter").get_single_value_default<MenuValue>().value) ==
                Filter::Nearest ?
            Interpolation::Nearest :
            Interpolation::Bilinear;

    const auto sample = [&](const float2 uv) -> float4 {
      if (image.is_single_value()) {
        return float4(image.get_single_value<Color>());
      }
      /* Wrap here as well, so the sample tiles even outside of a Texture Graph. */
      const float2 wrapped = uv - math::floor(uv);
      return float4(image.sample<Color>(wrapped, interpolation, Extension::Repeat, Extension::Repeat));
    };
    const auto value_of = [](const float4 &color) { return (color.x + color.y + color.z) / 3.0f; };

    if (position.is_single_value()) {
      const float4 color = sample(position.get_single_value<float2>());
      if (color_result.should_compute()) {
        color_result.allocate_single_value();
        color_result.set_single_value(Color(color));
      }
      if (value_result.should_compute()) {
        value_result.allocate_single_value();
        value_result.set_single_value(value_of(color));
      }
      return;
    }

    const Domain domain = position.domain();
    if (color_result.should_compute()) {
      color_result.allocate_texture(domain);
    }
    if (value_result.should_compute()) {
      value_result.allocate_texture(domain);
    }
    parallel_for(domain.data_size, [&](const int2 texel) {
      const float4 color = sample(position.load_pixel<float2>(texel));
      if (color_result.should_compute()) {
        color_result.store_pixel(texel, Color(color));
      }
      if (value_result.should_compute()) {
        value_result.store_pixel(texel, value_of(color));
      }
    });
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new SampleOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;

  tg_node_type_base(&ntype, "TextureGraphNodeSample");
  ntype.ui_name = "Sample";
  ntype.ui_description = "Read an image at any position, wrapping around its edges";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_SAMPLE";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_sample_cc
