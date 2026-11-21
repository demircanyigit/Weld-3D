/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Emboss: shades an image as if a height image were lit from one side, wrapping at the edges.
 */

#include "BLI_math_vector.hh"
#include "BLI_task.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"
#include "texture_graph_util.hh"

namespace blender::nodes::node_texture_graph_emboss_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Color>("Image")
      .default_value({0.5f, 0.5f, 0.5f, 1.0f})
      .structure_type(StructureType::Dynamic);
  b.add_output<decl::Color>("Image").structure_type(StructureType::Dynamic).align_with_previous();

  b.add_input<decl::Float>("Height")
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .hide_value()
      .structure_type(StructureType::Dynamic);
  b.add_input<decl::Float>("Intensity")
      .default_value(1.0f)
      .min(0.0f)
      .max(100.0f)
      .description("How strong the light and shadow are");
  b.add_input<decl::Float>("Light Angle")
      .default_value(float(M_PI) * 0.75f)
      .subtype(PROP_ANGLE)
      .description("Direction the light comes from");
  b.add_input<decl::Color>("Highlight").default_value({1.0f, 1.0f, 1.0f, 1.0f});
  b.add_input<decl::Color>("Shadow").default_value({0.0f, 0.0f, 0.0f, 1.0f});
}

using namespace blender::compositor;

class EmbossOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &image = this->get_input("Image");
    const Result &height = this->get_input("Height");
    Result &output = this->get_result("Image");

    if (height.is_single_value()) {
      output.share_data(image);
      return;
    }

    const float intensity = this->get_input("Intensity").get_single_value_default<float>();
    const float angle = this->get_input("Light Angle").get_single_value_default<float>();
    const float4 highlight = float4(this->get_input("Highlight").get_single_value_default<Color>());
    const float4 shadow = float4(this->get_input("Shadow").get_single_value_default<Color>());

    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    output.allocate_texture(domain);

    const float2 light = float2(math::cos(angle), math::sin(angle));
    const float scale = intensity * float(math::max(size.x, size.y)) * 0.05f;

    parallel_for(size, [&](const int2 texel) {
      const float4 color = float4(image.load_pixel<Color, true>(texel));
      const float2 slope = texture_graph::sobel_slope(height, texel, size) * scale;
      /* A surface facing the light leans away from rising height, so its slope points away. */
      const float lit = math::dot(-slope, light);
      float4 result = lit > 0.0f ? math::interpolate(color, highlight, math::min(lit, 1.0f)) :
                                   math::interpolate(color, shadow, math::min(-lit, 1.0f));
      result.w = color.w;
      output.store_pixel(texel, Color(result));
    });
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new EmbossOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;

  tg_node_type_base(&ntype, "TextureGraphNodeEmboss");
  ntype.ui_name = "Emboss";
  ntype.ui_description = "Shade an image as if a height image were lit from one side";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_EMBOSS";
  ntype.nclass = NODE_CLASS_OP_FILTER;
  ntype.declare = node_declare;
  ntype.flag |= NODE_PREVIEW;
  ntype.get_compositor_operation = get_compositor_operation;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_emboss_cc
