/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Channel Shuffle: builds a color by picking each of its channels from any channel of two
 * images, or a fixed 0 or 1. Swaps, copies and packing several masks into one image.
 */

#include "BLI_math_vector.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_texture_graph_channel_shuffle_cc {

/* 0-3: channels of A, 4-7: channels of B, then the constants. */
static const EnumPropertyItem source_items[] = {
    {0, "A_RED", 0, N_("A Red"), ""},
    {1, "A_GREEN", 0, N_("A Green"), ""},
    {2, "A_BLUE", 0, N_("A Blue"), ""},
    {3, "A_ALPHA", 0, N_("A Alpha"), ""},
    {4, "B_RED", 0, N_("B Red"), ""},
    {5, "B_GREEN", 0, N_("B Green"), ""},
    {6, "B_BLUE", 0, N_("B Blue"), ""},
    {7, "B_ALPHA", 0, N_("B Alpha"), ""},
    {8, "ZERO", 0, N_("0"), N_("Always 0")},
    {9, "ONE", 0, N_("1"), N_("Always 1")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Color>("Image").structure_type(StructureType::Dynamic);

  b.add_input<decl::Color>("A").default_value({0.0f, 0.0f, 0.0f, 1.0f}).structure_type(
      StructureType::Dynamic);
  b.add_input<decl::Color>("B").default_value({0.0f, 0.0f, 0.0f, 1.0f}).structure_type(
      StructureType::Dynamic);
  b.add_input<decl::Menu>("Red").default_value(MenuValue(0)).static_items(source_items);
  b.add_input<decl::Menu>("Green").default_value(MenuValue(1)).static_items(source_items);
  b.add_input<decl::Menu>("Blue").default_value(MenuValue(2)).static_items(source_items);
  b.add_input<decl::Menu>("Alpha").default_value(MenuValue(3)).static_items(source_items);
}

using namespace blender::compositor;

class ChannelShuffleOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &a = this->get_input("A");
    const Result &b = this->get_input("B");
    int4 sources;
    const char *names[4] = {"Red", "Green", "Blue", "Alpha"};
    for (int i = 0; i < 4; i++) {
      sources[i] = math::clamp(this->get_input(names[i]).get_single_value_default<MenuValue>().value,
                               0,
                               9);
    }

    const auto shuffle = [&](const float4 &ca, const float4 &cb) {
      float4 out;
      for (int i = 0; i < 4; i++) {
        const int s = sources[i];
        out[i] = s < 4 ? ca[s] : (s < 8 ? cb[s - 4] : (s == 8 ? 0.0f : 1.0f));
      }
      return out;
    };

    Result &output = this->get_result("Image");
    if (a.is_single_value() && b.is_single_value()) {
      output.allocate_single_value();
      output.set_single_value(
          Color(shuffle(float4(a.get_single_value<Color>()), float4(b.get_single_value<Color>()))));
      return;
    }

    const Domain domain = this->compute_domain();
    output.allocate_texture(domain);
    parallel_for(domain.data_size, [&](const int2 texel) {
      const float4 ca = float4(a.load_pixel<Color, true>(texel));
      const float4 cb = float4(b.load_pixel<Color, true>(texel));
      output.store_pixel(texel, Color(shuffle(ca, cb)));
    });
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new ChannelShuffleOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;

  tg_node_type_base(&ntype, "TextureGraphNodeChannelShuffle");
  ntype.ui_name = "Channel Shuffle";
  ntype.ui_description = "Pick each channel of the color from any channel of two images";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_CHANNEL_SHUFFLE";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_channel_shuffle_cc
