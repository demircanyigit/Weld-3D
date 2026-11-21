/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Sharpen: boosts the difference between each pixel and its neighbors, wrapping at the edges.
 */

#include "BLI_math_vector.hh"
#include "BLI_task.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"
#include "texture_graph_util.hh"

namespace blender::nodes::node_texture_graph_sharpen_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Color>("Image")
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .hide_value()
      .structure_type(StructureType::Dynamic);
  b.add_output<decl::Color>("Image").structure_type(StructureType::Dynamic).align_with_previous();

  b.add_input<decl::Float>("Intensity")
      .default_value(0.5f)
      .min(0.0f)
      .max(10.0f)
      .description("How strongly edges are sharpened");
}

using namespace blender::compositor;

class SharpenOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &image = this->get_input("Image");
    Result &output = this->get_result("Image");
    const float intensity = this->get_input("Intensity").get_single_value_default<float>();

    if (image.is_single_value() || intensity == 0.0f) {
      output.share_data(image);
      return;
    }

    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    output.allocate_texture(domain);

    parallel_for(size, [&](const int2 texel) {
      const auto load = [&](const int2 offset) {
        return float4(image.load_pixel<Color>(texture_graph::wrap_texel(texel + offset, size)));
      };
      const float4 center = load(int2(0));
      const float4 neighbors = load(int2(1, 0)) + load(int2(-1, 0)) + load(int2(0, 1)) +
                               load(int2(0, -1));
      float4 sharpened = center + (center * 4.0f - neighbors) * intensity;
      sharpened.w = center.w;
      output.store_pixel(texel, Color(sharpened));
    });
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new SharpenOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;

  tg_node_type_base(&ntype, "TextureGraphNodeSharpen");
  ntype.ui_name = "Sharpen";
  ntype.ui_description = "Make edges in an image crisper";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_SHARPEN";
  ntype.nclass = NODE_CLASS_OP_FILTER;
  ntype.declare = node_declare;
  ntype.flag |= NODE_PREVIEW;
  ntype.get_compositor_operation = get_compositor_operation;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_sharpen_cc
