/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Warp: pushes an image along the slopes of a grayscale gradient image, wrapping at the edges.
 */

#include "BLI_math_vector.hh"
#include "BLI_task.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"
#include "texture_graph_util.hh"

namespace blender::nodes::node_texture_graph_warp_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Color>("Image")
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .hide_value()
      .structure_type(StructureType::Dynamic);
  b.add_output<decl::Color>("Image").structure_type(StructureType::Dynamic).align_with_previous();

  b.add_input<decl::Float>("Gradient")
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .hide_value()
      .structure_type(StructureType::Dynamic)
      .description("Grayscale image whose slopes push the pixels");
  b.add_input<decl::Float>("Intensity")
      .default_value(1.0f)
      .min(-100.0f)
      .max(100.0f)
      .description("How far the pixels are pushed");
}

using namespace blender::compositor;

class WarpOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &image = this->get_input("Image");
    const Result &gradient = this->get_input("Gradient");
    Result &output = this->get_result("Image");
    const float intensity = this->get_input("Intensity").get_single_value_default<float>();

    if (image.is_single_value() || gradient.is_single_value() || intensity == 0.0f) {
      output.share_data(image);
      return;
    }

    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    output.allocate_texture(domain);

    /* The slope is per pixel, multiplying by the image size makes it per image, so the look does
     * not change with the resolution. */
    const float scale = intensity * float(math::max(size.x, size.y)) * 0.01f;

    parallel_for(size, [&](const int2 texel) {
      const float2 slope = texture_graph::sobel_slope(gradient, texel, size);
      const float2 coordinates = (float2(texel) + 0.5f) / float2(size) + slope * scale;
      output.store_pixel(
          texel,
          image.sample<Color>(
              coordinates, Interpolation::Bilinear, Extension::Repeat, Extension::Repeat));
    });
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new WarpOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;

  tg_node_type_base(&ntype, "TextureGraphNodeWarp");
  ntype.ui_name = "Warp";
  ntype.ui_description = "Push an image along the slopes of a grayscale gradient image";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_WARP";
  ntype.nclass = NODE_CLASS_DISTORT;
  ntype.declare = node_declare;
  ntype.flag |= NODE_PREVIEW;
  ntype.get_compositor_operation = get_compositor_operation;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_warp_cc
