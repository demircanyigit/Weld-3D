/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Per-pixel color adjustments of the Texture Graph: Channel Mixer, Chrominance Extract, Color To
 * Mask, Contrast/Luminosity, Grayscale Conversion, HDR Range Viewer, Replace Color, Replace Color
 * Range and Threshold. Each one only looks at the pixel it computes, so they run as functions
 * and work on single values as well as images.
 */

#include "BLI_math_vector.hh"

#include "FN_multi_function_builder.hh"

#include "NOD_multi_function.hh"

#include "COM_result.hh"

#include "node_composite_util.hh"
#include "texture_graph_util.hh"

/* -------------------------------------------------------------------- */
/** \name Channel Mixer
 * \{ */

namespace blender::nodes::node_texture_graph_channel_mixer_cc {

using compositor::Color;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_output<decl::Color>("Image");
  b.add_input<decl::Color>("Image").default_value({1.0f, 1.0f, 1.0f, 1.0f}).hide_value();
  b.add_input<decl::Vector>("Red")
      .default_value({100.0f, 0.0f, 0.0f})
      .min(-200.0f)
      .max(200.0f)
      .description("Percent of the red, green and blue going into the red channel");
  b.add_input<decl::Vector>("Green")
      .default_value({0.0f, 100.0f, 0.0f})
      .min(-200.0f)
      .max(200.0f)
      .description("Percent of the red, green and blue going into the green channel");
  b.add_input<decl::Vector>("Blue")
      .default_value({0.0f, 0.0f, 100.0f})
      .min(-200.0f)
      .max(200.0f)
      .description("Percent of the red, green and blue going into the blue channel");
  b.add_input<decl::Bool>("Monochrome").default_value(false).description(
      "Use the red mix for all three channels, giving a gray image");
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  static auto function = mf::build::SI5_SO<Color, float3, float3, float3, bool, Color>(
      "Channel Mixer",
      [](const Color &color, const float3 &red, const float3 &green, const float3 &blue,
         const bool mono) -> Color {
        const float4 c = float4(color);
        const float3 rgb = c.xyz();
        const float r = math::dot(rgb, red) / 100.0f;
        if (mono) {
          return Color(float4(r, r, r, c.w));
        }
        return Color(float4(r, math::dot(rgb, green) / 100.0f, math::dot(rgb, blue) / 100.0f, c.w));
      },
      mf::build::exec_presets::SomeSpanOrSingle<0>());
  builder.set_matching_fn(function);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeChannelMixer");
  ntype.ui_name = "Channel Mixer";
  ntype.ui_description = "Build each channel from a mix of the red, green and blue channels";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_CHANNEL_MIXER";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  ntype.declare = node_declare;
  ntype.build_multi_function = node_build_multi_function;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_channel_mixer_cc

/** \} */

/* -------------------------------------------------------------------- */
/** \name Chrominance Extract
 * \{ */

namespace blender::nodes::node_texture_graph_chrominance_extract_cc {

using compositor::Color;
namespace tg = texture_graph;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_output<decl::Color>("Image").description(
      "The color with its brightness flattened to middle gray");
  b.add_input<decl::Color>("Image").default_value({1.0f, 1.0f, 1.0f, 1.0f}).hide_value();
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  static auto function = mf::build::SI1_SO<Color, Color>(
      "Chrominance Extract",
      [](const Color &color) -> Color {
        const float4 c = float4(color);
        return Color(float4(c.xyz() - tg::luminance(c.xyz()) + 0.5f, c.w));
      },
      mf::build::exec_presets::AllSpanOrSingle());
  builder.set_matching_fn(function);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeChrominanceExtract");
  ntype.ui_name = "Chrominance Extract";
  ntype.ui_description = "Keep only the color of an image, without its brightness";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_CHROMINANCE_EXTRACT";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  ntype.declare = node_declare;
  ntype.build_multi_function = node_build_multi_function;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_chrominance_extract_cc

/** \} */

/* -------------------------------------------------------------------- */
/** \name Color To Mask
 * \{ */

namespace blender::nodes::node_texture_graph_color_to_mask_cc {

using compositor::Color;
namespace tg = texture_graph;

enum class Space : int {
  RGB = 0,
  Luminance = 1,
  Lab = 2,
  Angle = 3,
};

static const EnumPropertyItem space_items[] = {
    {int(Space::RGB), "RGB", 0, N_("RGB"), N_("Straight red, green and blue, for data images")},
    {int(Space::Luminance), "LUMINANCE", 0, N_("Luminance"), N_("Compare brightness only")},
    {int(Space::Lab), "LAB", 0, N_("Lab"), N_("Colors that look alike are close, for color images")},
    {int(Space::Angle), "ANGLE", 0, N_("Angle"), N_("Compare directions, for normal maps")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_output<decl::Float>("Mask");
  b.add_input<decl::Color>("Image").default_value({1.0f, 1.0f, 1.0f, 1.0f}).hide_value();
  b.add_input<decl::Color>("Color")
      .default_value({1.0f, 0.0f, 0.0f, 1.0f})
      .description("Color to select, connect an image to select a different color per pixel");
  b.add_input<decl::Float>("Threshold").default_value(0.1f).min(0.0f).max(1.0f).subtype(
      PROP_FACTOR);
  b.add_input<decl::Float>("Fade").default_value(0.05f).min(0.0f).max(1.0f).subtype(
      PROP_FACTOR);
  b.add_input<decl::Menu>("Space")
      .default_value(MenuValue(Space::Lab))
      .static_items(space_items)
      .description("How the distance between colors is measured");
  b.add_input<decl::Vector>("Weights")
      .default_value({0.5f, 1.0f, 1.0f})
      .min(0.0f)
      .max(4.0f)
      .usage_by_menu("Space", int(Space::Lab))
      .description("How much lightness, chroma and hue differences count");
}

static float distance(const float3 &a, const float3 &b, const Space space, const float3 &weights)
{
  switch (space) {
    case Space::RGB:
      return math::distance(a, b) / math::sqrt(3.0f);
    case Space::Luminance:
      return math::abs(tg::luminance(a) - tg::luminance(b));
    case Space::Lab:
      return tg::lab_distance(a, b, weights);
    case Space::Angle: {
      const float3 na = math::normalize(a * 2.0f - 1.0f);
      const float3 nb = math::normalize(b * 2.0f - 1.0f);
      return math::safe_acos(math::dot(na, nb)) / float(M_PI);
    }
  }
  return 0.0f;
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  static auto function = mf::build::SI6_SO<Color, Color, float, float, MenuValue, float3, float>(
      "Color To Mask",
      [](const Color &color, const Color &reference, const float threshold, const float fade,
         const MenuValue space, const float3 &weights) -> float {
        const float d = distance(
            float4(color).xyz(), float4(reference).xyz(), Space(space.value), weights);
        if (fade <= 1e-6f) {
          return d <= threshold ? 1.0f : 0.0f;
        }
        return math::clamp(1.0f - (d - threshold) / fade, 0.0f, 1.0f);
      },
      mf::build::exec_presets::SomeSpanOrSingle<0>());
  builder.set_matching_fn(function);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeColorToMask");
  ntype.ui_name = "Color To Mask";
  ntype.ui_description = "White where the image is close to a color";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_COLOR_TO_MASK";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  ntype.declare = node_declare;
  ntype.build_multi_function = node_build_multi_function;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_color_to_mask_cc

/** \} */

/* -------------------------------------------------------------------- */
/** \name Contrast/Luminosity
 * \{ */

namespace blender::nodes::node_texture_graph_contrast_luminosity_cc {

using compositor::Color;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_output<decl::Color>("Image");
  b.add_input<decl::Color>("Image").default_value({1.0f, 1.0f, 1.0f, 1.0f}).hide_value();
  b.add_input<decl::Float>("Contrast").default_value(0.0f).min(-1.0f).max(1.0f).description(
      "More contrast around middle gray above zero, less below");
  b.add_input<decl::Float>("Luminosity").default_value(0.0f).min(-1.0f).max(1.0f).description(
      "Brighter above zero, darker below");
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  static auto function = mf::build::SI3_SO<Color, float, float, Color>(
      "Contrast/Luminosity",
      [](const Color &color, const float contrast, const float luminosity) -> Color {
        const float4 c = float4(color);
        const float k = contrast >= 0.0f ? 1.0f / math::max(1.0f - contrast, 0.01f) :
                                           1.0f + contrast;
        return Color(float4((c.xyz() - 0.5f) * k + 0.5f + luminosity, c.w));
      },
      mf::build::exec_presets::SomeSpanOrSingle<0>());
  builder.set_matching_fn(function);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeContrastLuminosity");
  ntype.ui_name = "Contrast/Luminosity";
  ntype.ui_description = "Change the contrast and brightness";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_CONTRAST_LUMINOSITY";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  ntype.declare = node_declare;
  ntype.build_multi_function = node_build_multi_function;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_contrast_luminosity_cc

/** \} */

/* -------------------------------------------------------------------- */
/** \name Grayscale Conversion
 * \{ */

namespace blender::nodes::node_texture_graph_grayscale_conversion_cc {

using compositor::Color;
namespace tg = texture_graph;

enum class Type : int {
  Desaturation = 0,
  Luma = 1,
  Average = 2,
  Max = 3,
  Min = 4,
};

static const EnumPropertyItem type_items[] = {
    {int(Type::Desaturation), "DESATURATION", 0, N_("Desaturation"),
     N_("Halfway between the brightest and darkest channel")},
    {int(Type::Luma), "LUMA", 0, N_("Luma"), N_("Weighted the way the eye sees brightness")},
    {int(Type::Average), "AVERAGE", 0, N_("Average"), N_("Average of the three channels")},
    {int(Type::Max), "MAX", 0, N_("Max"), N_("The brightest channel")},
    {int(Type::Min), "MIN", 0, N_("Min"), N_("The darkest channel")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_output<decl::Float>("Value");
  b.add_input<decl::Color>("Image").default_value({1.0f, 1.0f, 1.0f, 1.0f}).hide_value();
  b.add_input<decl::Menu>("Type")
      .default_value(MenuValue(Type::Luma))
      .static_items(type_items)
      .optional_label();
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  static auto function = mf::build::SI2_SO<Color, MenuValue, float>(
      "Grayscale Conversion",
      [](const Color &color, const MenuValue type) -> float {
        const float3 c = float4(color).xyz();
        const float max = math::max(c.x, math::max(c.y, c.z));
        const float min = math::min(c.x, math::min(c.y, c.z));
        switch (Type(type.value)) {
          case Type::Desaturation:
            return (max + min) * 0.5f;
          case Type::Luma:
            return tg::luminance(c);
          case Type::Average:
            return (c.x + c.y + c.z) / 3.0f;
          case Type::Max:
            return max;
          case Type::Min:
            return min;
        }
        return 0.0f;
      },
      mf::build::exec_presets::SomeSpanOrSingle<0>());
  builder.set_matching_fn(function);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeGrayscaleConversion");
  ntype.ui_name = "Grayscale Conversion";
  ntype.ui_description = "Turn a color image gray, choosing how";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_GRAYSCALE_CONVERSION";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.build_multi_function = node_build_multi_function;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_grayscale_conversion_cc

/** \} */

/* -------------------------------------------------------------------- */
/** \name HDR Range Viewer
 * \{ */

namespace blender::nodes::node_texture_graph_hdr_range_viewer_cc {

using compositor::Color;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_output<decl::Color>("Image").description(
      "Gray where the values are between 0 and 1, red above 1, blue below 0");
  b.add_input<decl::Color>("Image").default_value({1.0f, 1.0f, 1.0f, 1.0f}).hide_value();
  b.add_input<decl::Float>("Range Min").default_value(-1.0f).min(-2.0f).max(0.0f).description(
      "Values this low show at full blue");
  b.add_input<decl::Float>("Range Max").default_value(2.0f).min(1.0f).max(3.0f).description(
      "Values this high show at full red");
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  static auto function = mf::build::SI3_SO<Color, float, float, Color>(
      "HDR Range Viewer",
      [](const Color &color, const float range_min, const float range_max) -> Color {
        const float3 c = float4(color).xyz();
        const float high = math::max(c.x, math::max(c.y, c.z));
        const float low = math::min(c.x, math::min(c.y, c.z));
        if (high > 1.0f) {
          const float t = math::clamp((high - 1.0f) / math::max(range_max - 1.0f, 1e-4f), 0.0f, 1.0f);
          return Color(float4(0.5f + 0.5f * t, 0.15f * (1.0f - t), 0.15f * (1.0f - t), 1.0f));
        }
        if (low < 0.0f) {
          const float t = math::clamp(low / math::min(range_min, -1e-4f), 0.0f, 1.0f);
          return Color(float4(0.15f * (1.0f - t), 0.15f * (1.0f - t), 0.5f + 0.5f * t, 1.0f));
        }
        const float v = (c.x + c.y + c.z) / 3.0f * 0.8f;
        return Color(float4(v, v, v, 1.0f));
      },
      mf::build::exec_presets::SomeSpanOrSingle<0>());
  builder.set_matching_fn(function);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeHDRRangeViewer");
  ntype.ui_name = "HDR Range Viewer";
  ntype.ui_description = "Show where values go below 0 or above 1";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_HDR_RANGE_VIEWER";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  ntype.declare = node_declare;
  ntype.build_multi_function = node_build_multi_function;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_hdr_range_viewer_cc

/** \} */

/* -------------------------------------------------------------------- */
/** \name Replace Color
 * \{ */

namespace blender::nodes::node_texture_graph_replace_color_cc {

using compositor::Color;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_output<decl::Color>("Image");
  b.add_input<decl::Color>("Image").default_value({1.0f, 1.0f, 1.0f, 1.0f}).hide_value();
  b.add_input<decl::Color>("Source").default_value({0.5f, 0.5f, 0.5f, 1.0f}).description(
      "Color the shift is measured from");
  b.add_input<decl::Color>("Target").default_value({0.5f, 0.5f, 0.5f, 1.0f}).description(
      "Color the source turns into, the rest of the image shifts the same way");
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  static auto function = mf::build::SI3_SO<Color, Color, Color, Color>(
      "Replace Color",
      [](const Color &color, const Color &source, const Color &target) -> Color {
        const float4 c = float4(color);
        return Color(float4(texture_graph::shift_color(
                                c.xyz(), float4(source).xyz(), float4(target).xyz()),
                            c.w));
      },
      mf::build::exec_presets::SomeSpanOrSingle<0>());
  builder.set_matching_fn(function);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeReplaceColor");
  ntype.ui_name = "Replace Color";
  ntype.ui_description = "Shift all colors so one color becomes another";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_REPLACE_COLOR";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  ntype.declare = node_declare;
  ntype.build_multi_function = node_build_multi_function;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_replace_color_cc

/** \} */

/* -------------------------------------------------------------------- */
/** \name Replace Color Range
 * \{ */

namespace blender::nodes::node_texture_graph_replace_color_range_cc {

using compositor::Color;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_output<decl::Color>("Image");
  b.add_input<decl::Color>("Image").default_value({1.0f, 1.0f, 1.0f, 1.0f}).hide_value();
  b.add_input<decl::Color>("Source").default_value({0.5f, 0.5f, 0.5f, 1.0f}).description(
      "Color to replace");
  b.add_input<decl::Color>("Target").default_value({0.5f, 0.5f, 0.5f, 1.0f}).description(
      "Color it becomes");
  b.add_input<decl::Float>("Range").default_value(0.1f).min(0.0f).max(1.0f).subtype(PROP_FACTOR)
      .description("How different a color can be from the source and still be fully replaced");
  b.add_input<decl::Float>("Falloff").default_value(0.1f).min(0.0f).max(1.0f).subtype(
      PROP_FACTOR).description("How softly the replacement fades out past the range");
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  static auto function = mf::build::SI5_SO<Color, Color, Color, float, float, Color>(
      "Replace Color Range",
      [](const Color &color, const Color &source, const Color &target, const float range,
         const float falloff) -> Color {
        const float4 c = float4(color);
        const float3 src = float4(source).xyz();
        const float d = math::distance(c.xyz(), src) / math::sqrt(3.0f);
        float w;
        if (falloff <= 1e-6f) {
          w = d <= range ? 1.0f : 0.0f;
        }
        else {
          w = math::clamp(1.0f - (d - range) / falloff, 0.0f, 1.0f);
        }
        const float3 replaced = texture_graph::shift_color(
            c.xyz(), src, float4(target).xyz());
        return Color(float4(math::interpolate(c.xyz(), replaced, w), c.w));
      },
      mf::build::exec_presets::SomeSpanOrSingle<0>());
  builder.set_matching_fn(function);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeReplaceColorRange");
  ntype.ui_name = "Replace Color Range";
  ntype.ui_description = "Replace one color and the colors close to it";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_REPLACE_COLOR_RANGE";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  ntype.declare = node_declare;
  ntype.build_multi_function = node_build_multi_function;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_replace_color_range_cc

/** \} */

/* -------------------------------------------------------------------- */
/** \name Threshold
 * \{ */

namespace blender::nodes::node_texture_graph_threshold_cc {

enum class Mode : int {
  Greater = 0,
  GreaterEqual = 1,
  Lower = 2,
  LowerEqual = 3,
};

static const EnumPropertyItem mode_items[] = {
    {int(Mode::Greater), "GREATER", 0, N_("Greater"), ""},
    {int(Mode::GreaterEqual), "GREATER_EQUAL", 0, N_("Greater or Equal"), ""},
    {int(Mode::Lower), "LOWER", 0, N_("Lower"), ""},
    {int(Mode::LowerEqual), "LOWER_EQUAL", 0, N_("Lower or Equal"), ""},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_output<decl::Float>("Mask");
  b.add_input<decl::Float>("Value").default_value(0.0f).hide_value();
  b.add_input<decl::Float>("Threshold").default_value(0.5f).min(0.0f).max(1.0f).subtype(
      PROP_FACTOR);
  b.add_input<decl::Menu>("Mode")
      .default_value(MenuValue(Mode::Greater))
      .static_items(mode_items)
      .optional_label()
      .description("Pixels passing this comparison turn white");
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  static auto function = mf::build::SI3_SO<float, float, MenuValue, float>(
      "Threshold",
      [](const float value, const float threshold, const MenuValue mode) -> float {
        switch (Mode(mode.value)) {
          case Mode::Greater:
            return value > threshold ? 1.0f : 0.0f;
          case Mode::GreaterEqual:
            return value >= threshold ? 1.0f : 0.0f;
          case Mode::Lower:
            return value < threshold ? 1.0f : 0.0f;
          case Mode::LowerEqual:
            return value <= threshold ? 1.0f : 0.0f;
        }
        return 0.0f;
      },
      mf::build::exec_presets::SomeSpanOrSingle<0>());
  builder.set_matching_fn(function);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeThreshold");
  ntype.ui_name = "Threshold";
  ntype.ui_description = "Turn values black or white by comparing them to a threshold";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_THRESHOLD";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.build_multi_function = node_build_multi_function;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_threshold_cc

/** \} */
