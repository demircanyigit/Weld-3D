/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Blend nodes of the Texture Graph that Mix Color does not cover: Dissolve and Linear Burn.
 */

#include "BLI_math_vector.hh"

#include "FN_multi_function_builder.hh"

#include "NOD_multi_function.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"
#include "texture_graph_util.hh"

namespace blender::nodes::node_texture_graph_blend_cc {

/* The inputs every blend node shares. */
static void declare_blend_inputs(NodeDeclarationBuilder &b)
{
  /* Images, so they are read per pixel and not made into one value. */
  b.add_input<decl::Color>("Foreground")
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .structure_type(StructureType::Dynamic);
  b.add_input<decl::Color>("Background")
      .default_value({0.0f, 0.0f, 0.0f, 1.0f})
      .structure_type(StructureType::Dynamic);
  b.add_input<decl::Float>("Mask")
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .structure_type(StructureType::Dynamic)
      .description("Where the blend applies, connect a mask to limit it");
  b.add_input<decl::Float>("Opacity").default_value(1.0f).min(0.0f).max(1.0f).subtype(
      PROP_FACTOR);
  b.add_input<decl::Bool>("Alpha Blending")
      .default_value(true)
      .description("Use the foreground's alpha, off treats the foreground as opaque");
}

}  // namespace blender::nodes::node_texture_graph_blend_cc

/* -------------------------------------------------------------------- */
/** \name Dissolve
 * \{ */

namespace blender::nodes::node_texture_graph_dissolve_cc {

namespace tg = texture_graph;
using namespace blender::compositor;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Color>("Image").structure_type(StructureType::Dynamic);
  node_texture_graph_blend_cc::declare_blend_inputs(b);
  b.add_input<decl::Int>("Seed").default_value(0).description(
      "Changes which pixels come from the foreground");
}

class DissolveOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &foreground = this->get_input("Foreground");
    const Result &background = this->get_input("Background");
    const Result &mask = this->get_input("Mask");
    const float opacity = this->get_input("Opacity").get_single_value_default<float>();
    const bool alpha_blending = this->get_input("Alpha Blending").get_single_value_default<bool>();
    const uint32_t seed = uint32_t(this->get_input("Seed").get_single_value_default<int>());

    /* The noise needs pixels, so single values still give an image of the graph size. */
    const bool all_single = foreground.is_single_value() && background.is_single_value() &&
                            mask.is_single_value();
    const Domain domain = all_single ? this->context().get_compositing_domain() :
                                       this->compute_domain();
    Result &output = this->get_result("Image");
    output.allocate_texture(domain);
    parallel_for(domain.data_size, [&](const int2 texel) {
      const float4 fg = float4(foreground.load_pixel<Color, true>(texel));
      const float4 bg = float4(background.load_pixel<Color, true>(texel));
      float amount = opacity * math::clamp(mask.load_pixel<float, true>(texel), 0.0f, 1.0f);
      if (alpha_blending) {
        amount *= fg.w;
      }
      const bool take_foreground = tg::hash_to_float(tg::hash(texel.x, texel.y, seed)) < amount;
      float4 result = take_foreground ? fg : bg;
      if (!alpha_blending || !take_foreground) {
        result.w = bg.w;
      }
      output.store_pixel(texel, Color(result));
    });
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new DissolveOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeDissolve");
  ntype.ui_name = "Dissolve";
  ntype.ui_description = "Blend two images by taking random pixels from the foreground";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_DISSOLVE";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_dissolve_cc

/** \} */

/* -------------------------------------------------------------------- */
/** \name Linear Burn
 * \{ */

namespace blender::nodes::node_texture_graph_linear_burn_cc {

using compositor::Color;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_output<decl::Color>("Image").structure_type(StructureType::Dynamic);
  node_texture_graph_blend_cc::declare_blend_inputs(b);
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  static auto function = mf::build::SI5_SO<Color, Color, float, float, bool, Color>(
      "Linear Burn",
      [](const Color &foreground, const Color &background, const float mask, const float opacity,
         const bool alpha_blending) -> Color {
        const float4 fg = float4(foreground);
        const float4 bg = float4(background);
        float amount = opacity * math::clamp(mask, 0.0f, 1.0f);
        if (alpha_blending) {
          amount *= fg.w;
        }
        const float3 burned = math::max(fg.xyz() + bg.xyz() - 1.0f, float3(0.0f));
        const float3 rgb = math::interpolate(bg.xyz(), burned, amount);
        const float alpha = alpha_blending ? bg.w + (1.0f - bg.w) * amount : bg.w;
        return Color(float4(rgb, alpha));
      },
      mf::build::exec_presets::SomeSpanOrSingle<0, 1>());
  builder.set_matching_fn(function);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeLinearBurn");
  ntype.ui_name = "Linear Burn";
  ntype.ui_description = "Darken by adding both images and taking away white";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_LINEAR_BURN";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  ntype.declare = node_declare;
  ntype.build_multi_function = node_build_multi_function;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_linear_burn_cc

/** \} */
