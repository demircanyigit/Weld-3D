/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Position: where each pixel is, and how big the image is. This is what a function made of math
 * nodes works from, the same function then runs for every pixel.
 */

#include "BLI_math_vector.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"
#include "texture_graph_util.hh"

namespace blender::nodes::node_texture_graph_position_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Vector>("Position")
      .dimensions(2)
      .structure_type(StructureType::Dynamic)
      .description("Position of the pixel center, from (0, 0) at the bottom left to (1, 1) at "
                   "the top right");
  b.add_output<decl::Vector>("Pixel")
      .dimensions(2)
      .structure_type(StructureType::Dynamic)
      .description("Whole pixel number, counting from 0 at the bottom left");
  b.add_output<decl::Vector>("Size")
      .dimensions(2)
      .description("Width and height of the image in pixels");

  b.add_input<decl::Menu>("Resolution")
      .default_value(MenuValue(0))
      .static_items(texture_graph::generator_size_items)
      .description("Size of the image the positions are made for");
}

using namespace blender::compositor;

class PositionOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const int resolution = this->get_input("Resolution").get_single_value_default<MenuValue>().value;
    const Domain domain = texture_graph::generator_domain(this->context(), resolution);
    const int2 size = domain.data_size;

    Result &position = this->get_result("Position");
    Result &pixel = this->get_result("Pixel");
    Result &size_result = this->get_result("Size");
    if (size_result.should_compute()) {
      size_result.allocate_single_value();
      size_result.set_single_value(float2(size));
    }
    if (position.should_compute()) {
      position.allocate_texture(domain);
    }
    if (pixel.should_compute()) {
      pixel.allocate_texture(domain);
    }
    if (!position.should_compute() && !pixel.should_compute()) {
      return;
    }
    parallel_for(size, [&](const int2 texel) {
      if (position.should_compute()) {
        position.store_pixel(texel, (float2(texel) + 0.5f) / float2(size));
      }
      if (pixel.should_compute()) {
        pixel.store_pixel(texel, float2(texel));
      }
    });
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new PositionOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;

  tg_node_type_base(&ntype, "TextureGraphNodePosition");
  ntype.ui_name = "Position";
  ntype.ui_description = "Where each pixel is and how big the image is, to build functions from";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_POSITION";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_position_cc
