/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Color grading nodes of the Texture Graph: Hald CLUT applies a color look-up image, Color Match
 * shifts all colors so a source color becomes a target color.
 */

#include "BLI_math_vector.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"
#include "texture_graph_util.hh"

/* -------------------------------------------------------------------- */
/** \name Hald CLUT
 * \{ */

namespace blender::nodes::node_texture_graph_hald_clut_cc {

namespace tg = texture_graph;
using namespace blender::compositor;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Color>("Image").structure_type(StructureType::Dynamic);
  b.add_input<decl::Color>("Image")
      .default_value({0.5f, 0.5f, 0.5f, 1.0f})
      .hide_value()
      .structure_type(StructureType::Dynamic);
  b.add_input<decl::Color>("LUT")
      .default_value({0.0f, 0.0f, 0.0f, 1.0f})
      .hide_value()
      .compositor_realization_mode(CompositorInputRealizationMode::None)
      .structure_type(StructureType::Dynamic)
      .description("Hald look-up image, a graded copy of an identity Hald image, such as 512 x "
                   "512 (level 8) or 4096 x 4096 (level 16)");
  b.add_input<decl::Bool>("Intensity by Alpha").default_value(false).description(
      "Apply the look-up only as much as the image's alpha");
}

class HaldClutOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &input = this->get_input("Image");
    const Result &lut = this->get_input("LUT");
    Result &output = this->get_result("Image");
    const bool by_alpha = this->get_input("Intensity by Alpha").get_single_value_default<bool>();

    /* A Hald image of level L is L^3 pixels wide and high and holds a cube of L^2 steps. */
    int cube = 0;
    int2 lut_size(0);
    if (!lut.is_single_value()) {
      lut_size = lut.domain().data_size;
      const int level = int(math::round(std::cbrt(double(lut_size.x))));
      if (level >= 2 && level * level * level == lut_size.x && lut_size.x == lut_size.y) {
        cube = level * level;
      }
    }
    if (cube == 0) {
      /* No usable look-up image, pass the image through. */
      output.share_data(input);
      return;
    }

    const auto entry = [&](const int r, const int g, const int b) {
      const int64_t index = int64_t(r) + int64_t(g) * cube + int64_t(b) * cube * cube;
      const int x = int(index % lut_size.x);
      const int y = int(index / lut_size.x);
      /* Hald images start at the top row. */
      return float4(lut.load_pixel<Color>(int2(x, lut_size.y - 1 - y))).xyz();
    };
    const auto apply = [&](const float4 color) {
      const float3 p = math::clamp(color.xyz(), float3(0.0f), float3(1.0f)) * float(cube - 1);
      const int3 base = math::min(int3(math::floor(p)), int3(cube - 2));
      const float3 t = p - float3(base);
      const auto lerp_x = [&](const int g, const int b) {
        return math::interpolate(entry(base.x, g, b), entry(base.x + 1, g, b), t.x);
      };
      const float3 c00 = lerp_x(base.y, base.z);
      const float3 c10 = lerp_x(base.y + 1, base.z);
      const float3 c01 = lerp_x(base.y, base.z + 1);
      const float3 c11 = lerp_x(base.y + 1, base.z + 1);
      const float3 graded = math::interpolate(
          math::interpolate(c00, c10, t.y), math::interpolate(c01, c11, t.y), t.z);
      const float amount = by_alpha ? math::clamp(color.w, 0.0f, 1.0f) : 1.0f;
      return float4(math::interpolate(color.xyz(), graded, amount), color.w);
    };

    if (input.is_single_value()) {
      output.allocate_single_value();
      output.set_single_value(Color(apply(float4(input.get_single_value<Color>()))));
      return;
    }
    const Domain domain = input.domain();
    output.allocate_texture(domain);
    parallel_for(domain.data_size, [&](const int2 texel) {
      output.store_pixel(texel, Color(apply(float4(input.load_pixel<Color>(texel)))));
    });
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new HaldClutOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeHaldCLUT");
  ntype.ui_name = "Hald CLUT";
  ntype.ui_description = "Grade the colors with a Hald look-up image";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_HALD_CLUT";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_hald_clut_cc

/** \} */

/* -------------------------------------------------------------------- */
/** \name Color Match
 * \{ */

namespace blender::nodes::node_texture_graph_color_match_cc {

namespace tg = texture_graph;
using namespace blender::compositor;

enum class Source : int {
  Average = 0,
  Color = 1,
};

static const EnumPropertyItem source_items[] = {
    {int(Source::Average), "AVERAGE", 0, N_("Average"), N_("The average color of the image")},
    {int(Source::Color), "COLOR", 0, N_("Color"),
     N_("The Source Color, connect an image to use a different source per pixel")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Color>("Image");
  b.add_output<decl::Float>("Mask").description("How much each pixel changed");

  b.add_input<decl::Color>("Image").default_value({0.5f, 0.5f, 0.5f, 1.0f}).hide_value();
  b.add_input<decl::Menu>("Source")
      .default_value(MenuValue(Source::Average))
      .static_items(source_items)
      .description("Color the shift is measured from");
  b.add_input<decl::Color>("Source Color")
      .default_value({0.5f, 0.5f, 0.5f, 1.0f})
      .usage_by_menu("Source", int(Source::Color));
  b.add_input<decl::Color>("Target Color")
      .default_value({0.8f, 0.3f, 0.2f, 1.0f})
      .description("Color the source turns into, connect an image for a target per pixel");
  b.add_input<decl::Float>("Hue").default_value(0.0f).min(-0.5f).max(0.5f).description(
      "Extra hue turn after matching");
  b.add_input<decl::Float>("Saturation").default_value(0.0f).min(-1.0f).max(1.0f).description(
      "Extra saturation after matching");
  b.add_input<decl::Float>("Brightness").default_value(0.0f).min(-1.0f).max(1.0f).description(
      "Extra brightness after matching");
  b.add_input<decl::Float>("Mask")
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("How strongly each pixel is matched, connect a mask to limit it");
  b.add_input<decl::Float>("Mask Blur").default_value(0.0f).min(0.0f).max(64.0f).description(
      "Softens the output mask");
}

class ColorMatchOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &input = this->get_input("Image");
    const Result &source_input = this->get_input("Source Color");
    const Result &target_input = this->get_input("Target Color");
    const Result &mask_input = this->get_input("Mask");
    const Source source_mode = Source(
        this->get_input("Source").get_single_value_default<MenuValue>().value);
    const float hue = this->get_input("Hue").get_single_value_default<float>();
    const float saturation = this->get_input("Saturation").get_single_value_default<float>();
    const float brightness = this->get_input("Brightness").get_single_value_default<float>();
    const float mask_blur = this->get_input("Mask Blur").get_single_value_default<float>();

    const bool single = input.is_single_value() && source_input.is_single_value() &&
                        target_input.is_single_value() && mask_input.is_single_value();
    const Domain domain = single ? Domain(int2(1)) : this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float4> pixels = tg::read_colors(input, size);

    float3 average(0.0f);
    if (source_mode == Source::Average) {
      double3 sum(0.0);
      for (const float4 &p : pixels) {
        sum += double3(p.xyz());
      }
      average = float3(sum / double(pixels.size()));
    }

    Array<float4> result(pixels.size());
    Array<float> change(pixels.size());
    threading::parallel_for(IndexRange(size.y), 16, [&](const IndexRange rows) {
      for (const int y : rows) {
        for (int x = 0; x < size.x; x++) {
          const int2 texel(x, y);
          const int64_t i = int64_t(y) * size.x + x;
          const float4 color = pixels[i];
          const float3 source = source_mode == Source::Average ?
                                    average :
                                    float4(source_input.load_pixel<Color, true>(texel)).xyz();
          const float3 target = float4(target_input.load_pixel<Color, true>(texel)).xyz();
          float3 hsv = tg::rgb_to_hsv(tg::shift_color(color.xyz(), source, target));
          hsv.x += hue;
          hsv.y = math::clamp(hsv.y + saturation, 0.0f, 1.0f);
          hsv.z = math::max(hsv.z + brightness, 0.0f);
          const float amount = math::clamp(mask_input.load_pixel<float, true>(texel), 0.0f, 1.0f);
          const float3 rgb = math::interpolate(color.xyz(), tg::hsv_to_rgb(hsv), amount);
          result[i] = float4(rgb, color.w);
          change[i] = math::distance(rgb, color.xyz());
        }
      }
    });

    Result &image_result = this->get_result("Image");
    Result &mask_result = this->get_result("Mask");
    if (single) {
      if (image_result.should_compute()) {
        image_result.allocate_single_value();
        image_result.set_single_value(Color(result[0]));
      }
      if (mask_result.should_compute()) {
        mask_result.allocate_single_value();
        mask_result.set_single_value(change[0] > 1e-5f ? 1.0f : 0.0f);
      }
      return;
    }
    if (image_result.should_compute()) {
      image_result.allocate_texture(domain);
      tg::write_colors(image_result, result, size);
    }
    if (mask_result.should_compute()) {
      float largest = 0.0f;
      for (const float c : change) {
        largest = math::max(largest, c);
      }
      for (float &c : change) {
        c = largest > 1e-6f ? c / largest : 0.0f;
      }
      tg::blur_wrapped<float>(change, size, tg::scaled_radius(mask_blur, size));
      mask_result.allocate_texture(domain);
      tg::write_values(mask_result, change, size);
    }
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new ColorMatchOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeColorMatch");
  ntype.ui_name = "Color Match";
  ntype.ui_description = "Shift the colors so a source color becomes a target color";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_COLOR_MATCH";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_color_match_cc

/** \} */
