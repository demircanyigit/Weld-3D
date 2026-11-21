/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Grayscale value nodes of the Texture Graph. Histogram Range, Histogram Scan, Non-Uniform
 * Histogram Scan, Histogram Select, Histogram Shift, ID to Mask, Quantize Grayscale and Quantize
 * Color (Simple) work per pixel. Auto Levels, Min Max, Histogram Compute, Histogram Equalize and
 * Histogram Render need the whole image first.
 */

#include <cfloat>

#include "BLI_math_vector.hh"

#include "FN_multi_function_builder.hh"

#include "NOD_multi_function.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"
#include "texture_graph_util.hh"

namespace blender::nodes::node_texture_graph_histogram_cc {

/* Number of bins a histogram node counts values in. */
inline const EnumPropertyItem resolution_items[] = {
    {256, "256", 0, N_("256"), ""},
    {512, "512", 0, N_("512"), ""},
    {1024, "1024", 0, N_("1024"), ""},
    {2048, "2048", 0, N_("2048"), ""},
    {4096, "4096", 0, N_("4096"), ""},
    {0, nullptr, 0, nullptr, nullptr},
};

/* Shared part of the two scan nodes: a soft step that moves from all black at position 0 to all
 * white at position 1, hard at contrast 1. */
inline float scan(const float value, const float position, const float contrast, const bool invert)
{
  const float width = math::clamp(1.0f - contrast, 0.0f, 1.0f);
  const float low = (1.0f - position) * (1.0f + width) - width;
  float result;
  if (width <= 1e-6f) {
    result = value > low ? 1.0f : 0.0f;
  }
  else {
    result = math::clamp((value - low) / width, 0.0f, 1.0f);
  }
  return invert ? 1.0f - result : result;
}

/* The domain a whole-image node works on: the input's own, or the graph's for a single value. */
inline compositor::Domain image_domain(const compositor::Context &context,
                                       const compositor::Result &input)
{
  return input.is_single_value() ? context.get_compositing_domain() : input.domain();
}

}  // namespace blender::nodes::node_texture_graph_histogram_cc

/* -------------------------------------------------------------------- */
/** \name Histogram Range
 * \{ */

namespace blender::nodes::node_texture_graph_histogram_range_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_output<decl::Float>("Value");
  b.add_input<decl::Float>("Value").default_value(0.5f).hide_value();
  b.add_input<decl::Float>("Range").default_value(0.5f).min(0.0f).max(1.0f).subtype(
      PROP_FACTOR).description("How much of the full range the values keep");
  b.add_input<decl::Float>("Position").default_value(0.5f).min(0.0f).max(1.0f).subtype(
      PROP_FACTOR).description("Where the middle of the squeezed range lands");
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  static auto function = mf::build::SI3_SO<float, float, float, float>(
      "Histogram Range",
      [](const float value, const float range, const float position) -> float {
        return position + (value - 0.5f) * range;
      },
      mf::build::exec_presets::SomeSpanOrSingle<0>());
  builder.set_matching_fn(function);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeHistogramRange");
  ntype.ui_name = "Histogram Range";
  ntype.ui_description = "Squeeze the values into a smaller range and move it";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_HISTOGRAM_RANGE";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  ntype.declare = node_declare;
  ntype.build_multi_function = node_build_multi_function;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_histogram_range_cc

/** \} */

/* -------------------------------------------------------------------- */
/** \name Histogram Scan
 * \{ */

namespace blender::nodes::node_texture_graph_histogram_scan_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_output<decl::Float>("Value");
  b.add_input<decl::Float>("Value").default_value(0.5f).hide_value();
  b.add_input<decl::Float>("Position").default_value(0.5f).min(0.0f).max(1.0f).subtype(
      PROP_FACTOR).description("All black at 0, all white at 1");
  b.add_input<decl::Float>("Contrast").default_value(0.5f).min(0.0f).max(1.0f).subtype(
      PROP_FACTOR).description("How hard the step from black to white is");
  b.add_input<decl::Bool>("Invert").default_value(false);
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  static auto function = mf::build::SI4_SO<float, float, float, bool, float>(
      "Histogram Scan",
      [](const float value, const float position, const float contrast, const bool invert) {
        return node_texture_graph_histogram_cc::scan(value, position, contrast, invert);
      },
      mf::build::exec_presets::SomeSpanOrSingle<0>());
  builder.set_matching_fn(function);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeHistogramScan");
  ntype.ui_name = "Histogram Scan";
  ntype.ui_description = "Turn values into a mask by sliding a soft step through them";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_HISTOGRAM_SCAN";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  ntype.declare = node_declare;
  ntype.build_multi_function = node_build_multi_function;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_histogram_scan_cc

/** \} */

/* -------------------------------------------------------------------- */
/** \name Non-Uniform Histogram Scan
 * \{ */

namespace blender::nodes::node_texture_graph_histogram_scan_non_uniform_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_output<decl::Float>("Value");
  b.add_input<decl::Float>("Value").default_value(0.5f).hide_value();
  b.add_input<decl::Float>("Position Map").default_value(1.0f).hide_value().description(
      "Scales the position per pixel");
  b.add_input<decl::Float>("Contrast Map").default_value(1.0f).hide_value().description(
      "Scales the contrast per pixel");
  b.add_input<decl::Float>("Position").default_value(0.5f).min(0.0f).max(1.0f).subtype(
      PROP_FACTOR);
  b.add_input<decl::Float>("Contrast").default_value(0.5f).min(0.0f).max(1.0f).subtype(
      PROP_FACTOR);
  b.add_input<decl::Bool>("Invert").default_value(false);
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  static auto function = mf::build::SI6_SO<float, float, float, float, float, bool, float>(
      "Non-Uniform Histogram Scan",
      [](const float value, const float position_map, const float contrast_map,
         const float position, const float contrast, const bool invert) {
        return node_texture_graph_histogram_cc::scan(
            value, position * position_map, contrast * contrast_map, invert);
      },
      mf::build::exec_presets::SomeSpanOrSingle<0, 1, 2>());
  builder.set_matching_fn(function);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeHistogramScanNonUniform");
  ntype.ui_name = "Non-Uniform Histogram Scan";
  ntype.ui_description = "Histogram Scan with a position and contrast that change per pixel";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_HISTOGRAM_SCAN_NON_UNIFORM";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  ntype.declare = node_declare;
  ntype.build_multi_function = node_build_multi_function;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_histogram_scan_non_uniform_cc

/** \} */

/* -------------------------------------------------------------------- */
/** \name Histogram Select
 * \{ */

namespace blender::nodes::node_texture_graph_histogram_select_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_output<decl::Float>("Value");
  b.add_input<decl::Float>("Value").default_value(0.5f).hide_value();
  b.add_input<decl::Float>("Position").default_value(0.5f).min(0.0f).max(1.0f).subtype(
      PROP_FACTOR).description("Middle of the band of values to keep");
  b.add_input<decl::Float>("Range").default_value(0.25f).min(0.0f).max(1.0f).subtype(
      PROP_FACTOR).description("Width of the band");
  b.add_input<decl::Float>("Contrast").default_value(0.5f).min(0.0f).max(1.0f).subtype(
      PROP_FACTOR).description("How hard the band's edges are");
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  static auto function = mf::build::SI4_SO<float, float, float, float, float>(
      "Histogram Select",
      [](const float value, const float position, const float range, const float contrast) {
        const float distance = math::abs(value - position) - range * 0.5f;
        const float soft = (1.0f - math::clamp(contrast, 0.0f, 1.0f)) * 0.5f;
        if (soft <= 1e-6f) {
          return distance <= 0.0f ? 1.0f : 0.0f;
        }
        return math::clamp(1.0f - distance / soft, 0.0f, 1.0f);
      },
      mf::build::exec_presets::SomeSpanOrSingle<0>());
  builder.set_matching_fn(function);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeHistogramSelect");
  ntype.ui_name = "Histogram Select";
  ntype.ui_description = "White where values are within a band, fading out around it";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_HISTOGRAM_SELECT";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  ntype.declare = node_declare;
  ntype.build_multi_function = node_build_multi_function;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_histogram_select_cc

/** \} */

/* -------------------------------------------------------------------- */
/** \name Histogram Shift
 * \{ */

namespace blender::nodes::node_texture_graph_histogram_shift_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_output<decl::Float>("Value");
  b.add_input<decl::Float>("Value").default_value(0.5f).hide_value();
  b.add_input<decl::Float>("Position").default_value(0.5f).min(0.0f).max(1.0f).subtype(
      PROP_FACTOR).description("How far to shift, values past 1 come back from 0");
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  static auto function = mf::build::SI2_SO<float, float, float>(
      "Histogram Shift",
      [](const float value, const float position) {
        const float v = value + position;
        return v - math::floor(v);
      },
      mf::build::exec_presets::SomeSpanOrSingle<0>());
  builder.set_matching_fn(function);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeHistogramShift");
  ntype.ui_name = "Histogram Shift";
  ntype.ui_description = "Shift all values, wrapping the ones past 1 back to 0";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_HISTOGRAM_SHIFT";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  ntype.declare = node_declare;
  ntype.build_multi_function = node_build_multi_function;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_histogram_shift_cc

/** \} */

/* -------------------------------------------------------------------- */
/** \name ID to Mask
 * \{ */

namespace blender::nodes::node_texture_graph_id_to_mask_cc {

namespace tg = texture_graph;

enum class Mode : int {
  Solo = 0,
  Range = 1,
};

static const EnumPropertyItem mode_items[] = {
    {int(Mode::Solo), "SOLO", 0, N_("Solo"), N_("One ID turns white")},
    {int(Mode::Range), "RANGE", 0, N_("Range"), N_("All IDs from the first to the last")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_output<decl::Float>("Mask");
  b.add_input<decl::Float>("ID").default_value(0.0f).hide_value().description(
      "ID map, such as the one Quantize Color makes");
  b.add_input<decl::Menu>("Mode")
      .default_value(MenuValue(Mode::Solo))
      .static_items(mode_items)
      .optional_label();
  b.add_input<decl::Int>("ID Index").default_value(0).min(0).max(255).usage_by_menu(
      "Mode", int(Mode::Solo));
  b.add_input<decl::Int>("First").default_value(0).min(0).max(255).usage_by_menu(
      "Mode", int(Mode::Range));
  b.add_input<decl::Int>("Last").default_value(1).min(0).max(255).usage_by_menu(
      "Mode", int(Mode::Range));
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  static auto function = mf::build::SI5_SO<float, MenuValue, int, int, int, float>(
      "ID to Mask",
      [](const float id_value, const MenuValue mode, const int index, const int first,
         const int last) {
        const int id = tg::id_from_value(id_value);
        if (Mode(mode.value) == Mode::Solo) {
          return id == index ? 1.0f : 0.0f;
        }
        return id >= math::min(first, last) && id <= math::max(first, last) ? 1.0f : 0.0f;
      },
      mf::build::exec_presets::SomeSpanOrSingle<0>());
  builder.set_matching_fn(function);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeIDToMask");
  ntype.ui_name = "ID to Mask";
  ntype.ui_description = "White where an ID map has the chosen IDs";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_ID_TO_MASK";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.build_multi_function = node_build_multi_function;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_id_to_mask_cc

/** \} */

/* -------------------------------------------------------------------- */
/** \name Quantize Grayscale
 * \{ */

namespace blender::nodes::node_texture_graph_quantize_grayscale_cc {

enum class Curve : int {
  Linear = 0,
  Smooth = 1,
};

static const EnumPropertyItem curve_items[] = {
    {int(Curve::Linear), "LINEAR", 0, N_("Linear"), N_("Straight slopes between steps")},
    {int(Curve::Smooth), "SMOOTH", 0, N_("Smooth"), N_("Slopes that ease in and out")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_output<decl::Float>("Value");
  b.add_input<decl::Float>("Value").default_value(0.5f).hide_value();
  b.add_input<decl::Int>("Steps").default_value(4).min(2).max(256).description(
      "Number of different values left");
  b.add_input<decl::Float>("Offset").default_value(0.0f).min(0.0f).max(1.0f).subtype(
      PROP_FACTOR).description("Moves where the steps are");
  b.add_input<decl::Float>("Slope").default_value(0.0f).min(0.0f).max(1.0f).subtype(
      PROP_FACTOR).description("Part of each step that ramps up to the next one");
  b.add_input<decl::Menu>("Curve")
      .default_value(MenuValue(Curve::Linear))
      .static_items(curve_items)
      .optional_label();
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  static auto function = mf::build::SI5_SO<float, int, float, float, MenuValue, float>(
      "Quantize Grayscale",
      [](const float value, const int steps_in, const float offset, const float slope,
         const MenuValue curve) {
        const int steps = math::max(steps_in, 2);
        const float t = value * float(steps) - offset;
        float base = math::floor(t);
        const float fraction = t - base;
        const float width = math::clamp(slope, 0.0f, 1.0f);
        float ramp = 0.0f;
        if (width > 1e-6f && fraction > 1.0f - width) {
          ramp = (fraction - (1.0f - width)) / width;
          if (Curve(curve.value) == Curve::Smooth) {
            ramp = ramp * ramp * (3.0f - 2.0f * ramp);
          }
        }
        base = math::clamp(base, 0.0f, float(steps - 1));
        return math::clamp((base + ramp) / float(steps - 1), 0.0f, 1.0f);
      },
      mf::build::exec_presets::SomeSpanOrSingle<0>());
  builder.set_matching_fn(function);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeQuantizeGrayscale");
  ntype.ui_name = "Quantize Grayscale";
  ntype.ui_description = "Snap values to a few steps, like terraces";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_QUANTIZE_GRAYSCALE";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  ntype.declare = node_declare;
  ntype.build_multi_function = node_build_multi_function;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_quantize_grayscale_cc

/** \} */

/* -------------------------------------------------------------------- */
/** \name Quantize Color (Simple)
 * \{ */

namespace blender::nodes::node_texture_graph_quantize_color_simple_cc {

using compositor::Color;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_output<decl::Color>("Image");
  b.add_input<decl::Color>("Image").default_value({1.0f, 1.0f, 1.0f, 1.0f}).hide_value();
  b.add_input<decl::Int>("Red").default_value(4).min(2).max(256).description(
      "Number of steps of the red channel");
  b.add_input<decl::Int>("Green").default_value(4).min(2).max(256);
  b.add_input<decl::Int>("Blue").default_value(4).min(2).max(256);
  b.add_input<decl::Int>("Alpha").default_value(256).min(2).max(256);
}

static float quantize(const float value, const int steps)
{
  const float n = float(math::max(steps, 2) - 1);
  return math::round(math::clamp(value, 0.0f, 1.0f) * n) / n;
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  static auto function = mf::build::SI5_SO<Color, int, int, int, int, Color>(
      "Quantize Color (Simple)",
      [](const Color &color, const int r, const int g, const int b, const int a) -> Color {
        const float4 c = float4(color);
        return Color(float4(quantize(c.x, r), quantize(c.y, g), quantize(c.z, b), quantize(c.w, a)));
      },
      mf::build::exec_presets::SomeSpanOrSingle<0>());
  builder.set_matching_fn(function);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeQuantizeColorSimple");
  ntype.ui_name = "Quantize Color (Simple)";
  ntype.ui_description = "Snap each channel to a few even steps";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_QUANTIZE_COLOR_SIMPLE";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  ntype.declare = node_declare;
  ntype.build_multi_function = node_build_multi_function;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_quantize_color_simple_cc

/** \} */

/* -------------------------------------------------------------------- */
/** \name Auto Levels
 * \{ */

namespace blender::nodes::node_texture_graph_auto_levels_cc {

namespace tg = texture_graph;
using namespace blender::compositor;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Float>("Value").structure_type(StructureType::Dynamic);
  b.add_input<decl::Float>("Value").default_value(0.5f).hide_value().structure_type(
      StructureType::Dynamic);
}

class AutoLevelsOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &input = this->get_input("Value");
    Result &output = this->get_result("Value");
    if (input.is_single_value()) {
      output.allocate_single_value();
      output.set_single_value(input.get_single_value<float>());
      return;
    }
    const Domain domain = input.domain();
    const int2 size = domain.data_size;
    Array<float> values = tg::read_values(input, size);
    float low = FLT_MAX;
    float high = -FLT_MAX;
    for (const float v : values) {
      low = math::min(low, v);
      high = math::max(high, v);
    }
    const float span = high - low;
    for (float &v : values) {
      v = span > 1e-8f ? (v - low) / span : 0.0f;
    }
    output.allocate_texture(domain);
    tg::write_values(output, values, size);
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new AutoLevelsOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeAutoLevels");
  ntype.ui_name = "Auto Levels";
  ntype.ui_description = "Stretch the values so the darkest is black and the brightest white";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_AUTO_LEVELS";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_auto_levels_cc

/** \} */

/* -------------------------------------------------------------------- */
/** \name Min Max
 * \{ */

namespace blender::nodes::node_texture_graph_min_max_cc {

namespace tg = texture_graph;
using namespace blender::compositor;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Float>("Min").description("Lowest value in the image");
  b.add_output<decl::Float>("Max").description("Highest value in the image");
  b.add_input<decl::Float>("Value").default_value(0.5f).hide_value().structure_type(
      StructureType::Dynamic);
}

class MinMaxOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &input = this->get_input("Value");
    float low;
    float high;
    if (input.is_single_value()) {
      low = high = input.get_single_value<float>();
    }
    else {
      const Array<float> values = tg::read_values(input, input.domain().data_size);
      low = FLT_MAX;
      high = -FLT_MAX;
      for (const float v : values) {
        low = math::min(low, v);
        high = math::max(high, v);
      }
    }
    Result &min_result = this->get_result("Min");
    Result &max_result = this->get_result("Max");
    if (min_result.should_compute()) {
      min_result.allocate_single_value();
      min_result.set_single_value(low);
    }
    if (max_result.should_compute()) {
      max_result.allocate_single_value();
      max_result.set_single_value(high);
    }
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new MinMaxOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeMinMax");
  ntype.ui_name = "Min Max";
  ntype.ui_description = "Find the lowest and highest value of an image";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_MIN_MAX";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_min_max_cc

/** \} */

/* -------------------------------------------------------------------- */
/** \name Histogram Compute
 * \{ */

namespace blender::nodes::node_texture_graph_histogram_compute_cc {

namespace tg = texture_graph;
using namespace blender::compositor;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Float>("Histogram").description(
      "One row, each pixel holds how many pixels have that value");
  b.add_output<decl::Float>("CDF").description(
      "One row, each pixel holds the share of pixels at or below that value");
  b.add_input<decl::Float>("Value").default_value(0.5f).hide_value().structure_type(
      StructureType::Dynamic);
  b.add_input<decl::Menu>("Resolution")
      .default_value(MenuValue(256))
      .static_items(node_texture_graph_histogram_cc::resolution_items)
      .description("Number of value steps counted");
}

class HistogramComputeOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &input = this->get_input("Value");
    const int bins = math::max(
        this->get_input("Resolution").get_single_value_default<MenuValue>().value, 2);
    const int2 size =
        node_texture_graph_histogram_cc::image_domain(this->context(), input).data_size;
    const Array<float> counts = tg::histogram(tg::read_values(input, size), bins);
    const float total = float(int64_t(size.x) * size.y);
    const Domain domain(int2(bins, 1));

    Result &histogram = this->get_result("Histogram");
    if (histogram.should_compute()) {
      histogram.allocate_texture(domain);
      for (int i = 0; i < bins; i++) {
        histogram.store_pixel(int2(i, 0), counts[i]);
      }
    }
    Result &cdf = this->get_result("CDF");
    if (cdf.should_compute()) {
      cdf.allocate_texture(domain);
      float sum = 0.0f;
      for (int i = 0; i < bins; i++) {
        sum += counts[i];
        cdf.store_pixel(int2(i, 0), sum / total);
      }
    }
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new HistogramComputeOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeHistogramCompute");
  ntype.ui_name = "Histogram Compute";
  ntype.ui_description = "Count the values of an image into a row of pixels";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_HISTOGRAM_COMPUTE";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_histogram_compute_cc

/** \} */

/* -------------------------------------------------------------------- */
/** \name Histogram Equalize
 * \{ */

namespace blender::nodes::node_texture_graph_histogram_equalize_cc {

namespace tg = texture_graph;
using namespace blender::compositor;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Float>("Value").structure_type(StructureType::Dynamic);
  b.add_input<decl::Float>("Value").default_value(0.5f).hide_value().structure_type(
      StructureType::Dynamic);
  b.add_input<decl::Menu>("Resolution")
      .default_value(MenuValue(256))
      .static_items(node_texture_graph_histogram_cc::resolution_items)
      .description("Number of value steps counted");
  b.add_input<decl::Float>("Smoothing").default_value(0.0f).min(0.0f).max(1.0f).subtype(
      PROP_FACTOR).description("Blurs the counts so the spread is less exact and smoother");
}

class HistogramEqualizeOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &input = this->get_input("Value");
    Result &output = this->get_result("Value");
    if (input.is_single_value()) {
      output.allocate_single_value();
      output.set_single_value(input.get_single_value<float>());
      return;
    }
    const int bins = math::max(
        this->get_input("Resolution").get_single_value_default<MenuValue>().value, 2);
    const float smoothing = this->get_input("Smoothing").get_single_value_default<float>();
    const Domain domain = input.domain();
    const int2 size = domain.data_size;
    Array<float> values = tg::read_values(input, size);
    Array<float> counts = tg::histogram(values, bins);

    /* Blur the counts along their row, without wrapping 0 into 1. */
    const int radius = int(math::round(smoothing * float(bins) * 0.05f));
    if (radius > 0) {
      Array<float> blurred(bins);
      for (int i = 0; i < bins; i++) {
        float sum = 0.0f;
        int n = 0;
        for (int j = math::max(i - radius, 0); j <= math::min(i + radius, bins - 1); j++) {
          sum += counts[j];
          n++;
        }
        blurred[i] = sum / float(n);
      }
      counts = blurred;
    }

    /* Value at the start of each bin. */
    Array<float> cdf(bins + 1);
    cdf[0] = 0.0f;
    for (int i = 0; i < bins; i++) {
      cdf[i + 1] = cdf[i] + counts[i];
    }
    const float total = math::max(cdf[bins], 1e-8f);
    threading::parallel_for(values.index_range(), 4096, [&](const IndexRange range) {
      for (const int64_t i : range) {
        const float t = math::clamp(values[i], 0.0f, 1.0f) * float(bins);
        const int bin = math::min(int(t), bins - 1);
        const float fraction = t - float(bin);
        values[i] = (cdf[bin] + counts[bin] * fraction) / total;
      }
    });
    output.allocate_texture(domain);
    tg::write_values(output, values, size);
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new HistogramEqualizeOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeHistogramEqualize");
  ntype.ui_name = "Histogram Equalize";
  ntype.ui_description = "Spread the values evenly over the whole range";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_HISTOGRAM_EQUALIZE";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_histogram_equalize_cc

/** \} */

/* -------------------------------------------------------------------- */
/** \name Histogram Render
 * \{ */

namespace blender::nodes::node_texture_graph_histogram_render_cc {

namespace tg = texture_graph;
using namespace blender::compositor;

enum class Filter : int {
  Linear = 0,
  Nearest = 1,
};

static const EnumPropertyItem filter_items[] = {
    {int(Filter::Linear), "LINEAR", 0, N_("Linear"), N_("Blend between neighboring columns")},
    {int(Filter::Nearest), "NEAREST", 0, N_("Nearest"), N_("Flat columns")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Float>("Image");
  b.add_input<decl::Float>("Value").default_value(0.5f).hide_value().structure_type(
      StructureType::Dynamic);
  b.add_input<decl::Menu>("Resolution")
      .default_value(MenuValue(256))
      .static_items(node_texture_graph_histogram_cc::resolution_items)
      .description("Number of value steps counted");
  b.add_input<decl::Bool>("Auto Scale").default_value(true).description(
      "Make the tallest column reach the top");
  b.add_input<decl::Float>("Scale").default_value(1.0f).min(0.0f).max(10.0f).description(
      "Height of the columns");
  b.add_input<decl::Menu>("Filter")
      .default_value(MenuValue(Filter::Linear))
      .static_items(filter_items)
      .optional_label();
  b.add_input<decl::Bool>("Flip").default_value(false).description(
      "Draw the columns from the top down");
}

class HistogramRenderOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &input = this->get_input("Value");
    const int bins = math::max(
        this->get_input("Resolution").get_single_value_default<MenuValue>().value, 2);
    const bool auto_scale = this->get_input("Auto Scale").get_single_value_default<bool>();
    const float scale = this->get_input("Scale").get_single_value_default<float>();
    const bool nearest = Filter(this->get_input("Filter")
                                    .get_single_value_default<MenuValue>()
                                    .value) == Filter::Nearest;
    const bool flip = this->get_input("Flip").get_single_value_default<bool>();

    const Domain domain = node_texture_graph_histogram_cc::image_domain(this->context(), input);
    const int2 size = domain.data_size;
    Array<float> heights = tg::histogram(tg::read_values(input, size), bins);
    float norm;
    if (auto_scale) {
      norm = 0.0f;
      for (const float h : heights) {
        norm = math::max(norm, h);
      }
    }
    else {
      /* An even spread of values reaches the top at scale 1. */
      norm = float(int64_t(size.x) * size.y) / float(bins);
    }
    for (float &h : heights) {
      h = h / math::max(norm, 1e-8f) * scale;
    }

    Result &output = this->get_result("Image");
    output.allocate_texture(domain);
    parallel_for(size, [&](const int2 texel) {
      const float u = (float(texel.x) + 0.5f) / float(size.x) * float(bins);
      float height;
      if (nearest) {
        height = heights[math::clamp(int(u), 0, bins - 1)];
      }
      else {
        const float p = u - 0.5f;
        const int a = math::clamp(int(math::floor(p)), 0, bins - 1);
        const int b = math::clamp(a + 1, 0, bins - 1);
        height = math::interpolate(heights[a], heights[b], math::clamp(p - math::floor(p), 0.0f, 1.0f));
      }
      float v = (float(texel.y) + 0.5f) / float(size.y);
      if (flip) {
        v = 1.0f - v;
      }
      output.store_pixel(texel, v < height ? 1.0f : 0.0f);
    });
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new HistogramRenderOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeHistogramRender");
  ntype.ui_name = "Histogram Render";
  ntype.ui_description = "Draw the histogram of an image as a chart";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_HISTOGRAM_RENDER";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_histogram_render_cc

/** \} */
