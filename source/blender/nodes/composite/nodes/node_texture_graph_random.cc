/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Random: a random number from a seed, either one for the whole image or a different one for
 * every pixel. The same seed always gives the same numbers.
 */

#include "BLI_math_vector.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"
#include "texture_graph_util.hh"

namespace blender::nodes::node_texture_graph_random_cc {

enum class Mode : int {
  Single = 0,
  PerPixel = 1,
};

static const EnumPropertyItem mode_items[] = {
    {int(Mode::Single), "SINGLE", 0, N_("Single"), N_("One number for the whole image")},
    {int(Mode::PerPixel), "PER_PIXEL", 0, N_("Per Pixel"), N_("A different number per pixel")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Float>("Value").structure_type(StructureType::Dynamic);
  b.add_output<decl::Color>("Color")
      .structure_type(StructureType::Dynamic)
      .description("Three random numbers as red, green and blue");

  b.add_input<decl::Menu>("Mode")
      .default_value(MenuValue(Mode::Single))
      .static_items(mode_items)
      .optional_label();
  b.add_input<decl::Float>("Min").default_value(0.0f);
  b.add_input<decl::Float>("Max").default_value(1.0f);
  b.add_input<decl::Int>("Seed").default_value(0).description(
      "Change it to get different numbers");
  b.add_input<decl::Menu>("Resolution")
      .default_value(MenuValue(0))
      .static_items(texture_graph::generator_size_items)
      .usage_by_menu("Mode", int(Mode::PerPixel))
      .description("Size of the image this node makes");
}

using namespace blender::compositor;

class RandomOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Mode mode = Mode(this->get_input("Mode").get_single_value_default<MenuValue>().value);
    const float min = this->get_input("Min").get_single_value_default<float>();
    const float max = this->get_input("Max").get_single_value_default<float>();
    const uint32_t seed = uint32_t(this->get_input("Seed").get_single_value_default<int>());
    Result &value = this->get_result("Value");
    Result &color = this->get_result("Color");

    const auto number = [&](const int x, const int y, const uint32_t channel) {
      const float f = texture_graph::hash_to_float(
          texture_graph::hash(x, y, texture_graph::hash(seed) ^ channel));
      return min + (max - min) * f;
    };

    if (mode == Mode::Single) {
      if (value.should_compute()) {
        value.allocate_single_value();
        value.set_single_value(number(0, 0, 0u));
      }
      if (color.should_compute()) {
        color.allocate_single_value();
        color.set_single_value(Color(float4(number(0, 0, 1u), number(0, 0, 2u), number(0, 0, 3u), 1.0f)));
      }
      return;
    }

    const int resolution = this->get_input("Resolution").get_single_value_default<MenuValue>().value;
    const Domain domain = texture_graph::generator_domain(this->context(), resolution);
    if (value.should_compute()) {
      value.allocate_texture(domain);
    }
    if (color.should_compute()) {
      color.allocate_texture(domain);
    }
    parallel_for(domain.data_size, [&](const int2 texel) {
      if (value.should_compute()) {
        value.store_pixel(texel, number(texel.x, texel.y, 0u));
      }
      if (color.should_compute()) {
        color.store_pixel(texel,
                          Color(float4(number(texel.x, texel.y, 1u),
                                       number(texel.x, texel.y, 2u),
                                       number(texel.x, texel.y, 3u),
                                       1.0f)));
      }
    });
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new RandomOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;

  tg_node_type_base(&ntype, "TextureGraphNodeRandom");
  ntype.ui_name = "Random";
  ntype.ui_description = "A random number from a seed, once or for every pixel";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_RANDOM";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_random_cc
