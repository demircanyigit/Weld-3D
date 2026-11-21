/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Height to Normal: turns a grayscale height image into a tangent-space normal map. The image is
 * sampled with wrap-around at its edges, so a tiling height map gives a tiling normal map.
 */

#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_task.hh"

#include "RNA_types.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_texture_graph_height_to_normal_cc {

enum class NormalFormat : int {
  OpenGL = 0,
  DirectX = 1,
};

static const EnumPropertyItem format_items[] = {
    {int(NormalFormat::OpenGL),
     "OPENGL",
     0,
     N_("OpenGL"),
     N_("Green points up, the format Blender uses")},
    {int(NormalFormat::DirectX),
     "DIRECTX",
     0,
     N_("DirectX"),
     N_("Green points down, used by some other programs")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Float>("Height")
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .hide_value()
      .structure_type(StructureType::Dynamic);
  b.add_output<decl::Color>("Normal").structure_type(StructureType::Dynamic).align_with_previous();

  b.add_input<decl::Float>("Strength")
      .default_value(1.0f)
      .min(0.0f)
      .max(100.0f)
      .description("How steep the height differences look");
  b.add_input<decl::Menu>("Format")
      .default_value(MenuValue(NormalFormat::OpenGL))
      .static_items(format_items)
      .optional_label();
}

using namespace blender::compositor;

class HeightToNormalOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &height = this->get_input("Height");
    Result &output = this->get_result("Normal");

    if (height.is_single_value()) {
      /* A flat surface faces straight out. */
      output.allocate_single_value();
      output.set_single_value(Color(float4(0.5f, 0.5f, 1.0f, 1.0f)));
      return;
    }

    const float strength = this->get_input("Strength").get_single_value_default<float>();
    const NormalFormat format = NormalFormat(
        this->get_input("Format").get_single_value_default<MenuValue>().value);
    const float green_sign = format == NormalFormat::DirectX ? -1.0f : 1.0f;

    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    output.allocate_texture(domain);

    /* Slopes are measured per pixel at a 1024 pixel reference size, so the look does not change
     * when the output size changes. */
    const float scale = strength * float(math::max(size.x, size.y)) / 1024.0f * 4.0f;

    const auto load = [&](int2 texel) -> float {
      texel.x = ((texel.x % size.x) + size.x) % size.x;
      texel.y = ((texel.y % size.y) + size.y) % size.y;
      return height.load_pixel<float>(texel);
    };

    parallel_for(size, [&](const int2 texel) {
      /* Sobel filter for smoother slopes than a plain central difference. */
      const float tl = load(texel + int2(-1, 1));
      const float t = load(texel + int2(0, 1));
      const float tr = load(texel + int2(1, 1));
      const float l = load(texel + int2(-1, 0));
      const float r = load(texel + int2(1, 0));
      const float bl = load(texel + int2(-1, -1));
      const float b = load(texel + int2(0, -1));
      const float br = load(texel + int2(1, -1));

      const float dx = (tr + 2.0f * r + br) - (tl + 2.0f * l + bl);
      const float dy = (tl + 2.0f * t + tr) - (bl + 2.0f * b + br);

      const float3 normal = math::normalize(float3(-dx * scale, -dy * scale * green_sign, 1.0f));
      output.store_pixel(texel, Color(float4(normal * 0.5f + 0.5f, 1.0f)));
    });
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new HeightToNormalOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;

  tg_node_type_base(&ntype, "TextureGraphNodeHeightToNormal");
  ntype.ui_name = "Height to Normal";
  ntype.ui_description = "Make a tangent-space normal map from a grayscale height image";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_HEIGHT_TO_NORMAL";
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.flag |= NODE_PREVIEW;
  ntype.get_compositor_operation = get_compositor_operation;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_height_to_normal_cc
