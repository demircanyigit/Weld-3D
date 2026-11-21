/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Levels: remaps the input range to the output range with a gamma curve in between.
 */

#include <cmath>

#include "BLI_math_base.hh"
#include "BLI_math_vector_types.hh"

#include "FN_multi_function_builder.hh"

#include "NOD_multi_function.hh"

#include "COM_result.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_texture_graph_levels_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.is_function_node();
  b.add_input<decl::Color>("Image").default_value({0.5f, 0.5f, 0.5f, 1.0f}).hide_value();
  b.add_output<decl::Color>("Image").align_with_previous();

  b.add_input<decl::Float>("Input Low")
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Input value that becomes the output low value");
  b.add_input<decl::Float>("Input High")
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Input value that becomes the output high value");
  b.add_input<decl::Float>("Gamma")
      .default_value(1.0f)
      .min(0.01f)
      .max(10.0f)
      .description("Brightens the midtones above 1, darkens them below 1");
  b.add_input<decl::Float>("Output Low")
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
  b.add_input<decl::Float>("Output High")
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
}

static float levels(const float value,
                    const float in_low,
                    const float in_high,
                    const float gamma,
                    const float out_low,
                    const float out_high)
{
  const float range = in_high - in_low;
  float t;
  if (std::abs(range) < 1e-6f) {
    t = value >= in_high ? 1.0f : 0.0f;
  }
  else {
    t = math::clamp((value - in_low) / range, 0.0f, 1.0f);
  }
  t = std::pow(t, 1.0f / math::max(gamma, 0.01f));
  return out_low + t * (out_high - out_low);
}

using compositor::Color;

static void node_build_multi_function(nodes::NodeMultiFunctionBuilder &builder)
{
  static auto function = mf::build::SI6_SO<Color, float, float, float, float, float, Color>(
      "Levels",
      [](const Color &color,
         const float in_low,
         const float in_high,
         const float gamma,
         const float out_low,
         const float out_high) -> Color {
        const float4 c = float4(color);
        return Color(float4(levels(c.x, in_low, in_high, gamma, out_low, out_high),
                            levels(c.y, in_low, in_high, gamma, out_low, out_high),
                            levels(c.z, in_low, in_high, gamma, out_low, out_high),
                            c.w));
      },
      mf::build::exec_presets::SomeSpanOrSingle<0>());
  builder.set_matching_fn(function);
}

static void node_register()
{
  static bke::bNodeType ntype;

  tg_node_type_base(&ntype, "TextureGraphNodeLevels");
  ntype.ui_name = "Levels";
  ntype.ui_description =
      "Remap the value range of an image, with a gamma curve for the midtones";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_LEVELS";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  ntype.declare = node_declare;
  ntype.flag |= NODE_PREVIEW;
  ntype.build_multi_function = node_build_multi_function;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_levels_cc
