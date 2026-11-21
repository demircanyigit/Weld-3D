/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Directional Warp: pushes an image in one direction by an amount taken from a grayscale image,
 * wrapping at the edges.
 */

#include "BLI_math_vector.hh"
#include "BLI_task.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"
#include "texture_graph_util.hh"

namespace blender::nodes::node_texture_graph_directional_warp_cc {

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
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .structure_type(StructureType::Dynamic)
      .description("Grayscale image that sets how far each pixel is pushed");
  b.add_input<decl::Float>("Distance")
      .default_value(0.1f)
      .min(-1.0f)
      .max(1.0f)
      .description("How far white pixels in the intensity image are pushed, as a part of the "
                   "image size");
  b.add_input<decl::Float>("Angle").default_value(0.0f).subtype(PROP_ANGLE).description(
      "Direction to push in");
}

using namespace blender::compositor;

class DirectionalWarpOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &image = this->get_input("Image");
    const Result &intensity = this->get_input("Intensity");
    Result &output = this->get_result("Image");
    const float distance = this->get_input("Distance").get_single_value_default<float>();
    const float angle = this->get_input("Angle").get_single_value_default<float>();

    if (image.is_single_value() || distance == 0.0f) {
      output.share_data(image);
      return;
    }

    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    output.allocate_texture(domain);

    /* Push the image the other way, so the image moves in the given direction. */
    const float2 direction = float2(math::cos(angle), math::sin(angle)) * distance;

    parallel_for(size, [&](const int2 texel) {
      const float amount = intensity.load_pixel<float, true>(texel);
      const float2 coordinates = (float2(texel) + 0.5f) / float2(size) - direction * amount;
      output.store_pixel(
          texel,
          image.sample<Color>(
              coordinates, Interpolation::Bilinear, Extension::Repeat, Extension::Repeat));
    });
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new DirectionalWarpOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;

  tg_node_type_base(&ntype, "TextureGraphNodeDirectionalWarp");
  ntype.ui_name = "Directional Warp";
  ntype.ui_description =
      "Push an image in one direction, by an amount taken from a grayscale image";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_DIRECTIONAL_WARP";
  ntype.nclass = NODE_CLASS_DISTORT;
  ntype.declare = node_declare;
  ntype.flag |= NODE_PREVIEW;
  ntype.get_compositor_operation = get_compositor_operation;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_directional_warp_cc
