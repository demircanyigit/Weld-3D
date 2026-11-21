/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Texture Graph Blend, with its blend modes, alpha handling and crop area, and Height Blend, which
 * puts one height on top of another where it stands higher.
 */

#include "BLI_math_vector.hh"

#include "BKE_node_runtime.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"
#include "texture_graph_util.hh"

/* -------------------------------------------------------------------- */
/** \name Blend
 * \{ */

namespace blender::nodes::node_texture_graph_blend_modes_cc {

using namespace blender::compositor;

enum class Mode : int {
  Normal = 0,
  Add = 1,
  Subtract = 2,
  Multiply = 3,
  AddSub = 4,
  Lighten = 5,
  Darken = 6,
  Switch = 7,
  Divide = 8,
  Overlay = 9,
  Screen = 10,
  SoftLight = 11,
};

static const EnumPropertyItem mode_items[] = {
    {int(Mode::Normal), "NORMAL", 0, N_("Normal"), N_("The foreground over the background")},
    {int(Mode::Add), "ADD", 0, N_("Add"), N_("Add the foreground to the background")},
    {int(Mode::Subtract),
     "SUBTRACT",
     0,
     N_("Subtract"),
     N_("Take the foreground away from the background")},
    {int(Mode::Multiply), "MULTIPLY", 0, N_("Multiply"), N_("Multiply the two, darkens")},
    {int(Mode::AddSub),
     "ADD_SUB",
     0,
     N_("Add Sub"),
     N_("Foreground above half gray is added, below half gray is taken away")},
    {int(Mode::Lighten), "LIGHTEN", 0, N_("Lighten"), N_("The lighter of the two")},
    {int(Mode::Darken), "DARKEN", 0, N_("Darken"), N_("The darker of the two")},
    {int(Mode::Switch),
     "SWITCH",
     0,
     N_("Switch"),
     N_("Go from background to foreground with the opacity, alpha included")},
    {int(Mode::Divide), "DIVIDE", 0, N_("Divide"), N_("Divide the background by the foreground")},
    {int(Mode::Overlay),
     "OVERLAY",
     0,
     N_("Overlay"),
     N_("Multiply dark parts of the background and screen light parts")},
    {int(Mode::Screen), "SCREEN", 0, N_("Screen"), N_("The opposite of multiply, lightens")},
    {int(Mode::SoftLight),
     "SOFT_LIGHT",
     0,
     N_("Soft Light"),
     N_("A softer overlay, lightens or darkens with the foreground")},
    {0, nullptr, 0, nullptr, nullptr},
};

enum class AlphaMode : int { UseAlpha = 0, IgnoreAlpha = 1, StraightAlpha = 2 };

static const EnumPropertyItem alpha_items[] = {
    {int(AlphaMode::UseAlpha),
     "USE_ALPHA",
     0,
     N_("Use Alpha"),
     N_("The foreground's alpha is part of its opacity, and the alphas are laid over each other")},
    {int(AlphaMode::IgnoreAlpha),
     "IGNORE_ALPHA",
     0,
     N_("Ignore Alpha"),
     N_("Only colors are blended, the background keeps its alpha")},
    {int(AlphaMode::StraightAlpha),
     "STRAIGHT_ALPHA",
     0,
     N_("Straight Alpha"),
     N_("Alpha is blended like a color channel, with the same mode")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Color>("Image").structure_type(StructureType::Dynamic);
  /* Transparent by default: with nothing connected, the background comes through unchanged. */
  b.add_input<decl::Color>("Foreground")
      .default_value({0.0f, 0.0f, 0.0f, 0.0f})
      .structure_type(StructureType::Dynamic)
      .description("The image laid on top, nothing when not connected");
  b.add_input<decl::Color>("Background")
      .default_value({0.0f, 0.0f, 0.0f, 1.0f})
      .structure_type(StructureType::Dynamic)
      .description("The image underneath");
  b.add_input<decl::Float>("Mask")
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .structure_type(StructureType::Dynamic)
      .description("Where the blend applies, white is full");
  b.add_input<decl::Menu>("Mode")
      .default_value(MenuValue(int(Mode::Normal)))
      .static_items(mode_items)
      .optional_label()
      .description("How the foreground is combined with the background");
  b.add_input<decl::Float>("Opacity")
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("How much of the blend shows");
  b.add_input<decl::Menu>("Alpha")
      .default_value(MenuValue(int(AlphaMode::UseAlpha)))
      .static_items(alpha_items)
      .description("What is done with the alpha of the images");
  b.add_input<decl::Bool>("Clamp").default_value(true).description(
      "Keep the result between 0 and 1");
  PanelDeclarationBuilder &crop = b.add_panel("Crop").default_closed(true);
  crop.add_input<decl::Float>("Min X")
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Left edge of the area that is blended, the background shows outside");
  crop.add_input<decl::Float>("Max X")
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Right edge of the area that is blended");
  crop.add_input<decl::Float>("Min Y")
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Bottom edge of the area that is blended");
  crop.add_input<decl::Float>("Max Y")
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Top edge of the area that is blended");
}

static float blend_channel(const Mode mode, const float b, const float f)
{
  switch (mode) {
    case Mode::Normal:
    case Mode::Switch:
      return f;
    case Mode::Add:
      return b + f;
    case Mode::Subtract:
      return b - f;
    case Mode::Multiply:
      return b * f;
    case Mode::AddSub:
      return b + 2.0f * f - 1.0f;
    case Mode::Lighten:
      return math::max(b, f);
    case Mode::Darken:
      return math::min(b, f);
    case Mode::Divide:
      if (f != 0.0f) {
        return b / f;
      }
      return b > 0.0f ? 1.0f : 0.0f;
    case Mode::Overlay:
      return b < 0.5f ? 2.0f * b * f : 1.0f - 2.0f * (1.0f - b) * (1.0f - f);
    case Mode::Screen:
      return 1.0f - (1.0f - b) * (1.0f - f);
    case Mode::SoftLight:
      return (1.0f - 2.0f * f) * b * b + 2.0f * f * b;
  }
  return f;
}

struct BlendSettings {
  Mode mode;
  AlphaMode alpha;
  float opacity;
  bool clamp;
};

static float4 blend_pixel(const BlendSettings &s, const float4 fg, const float4 bg, float mask)
{
  const float amount = s.opacity * math::clamp(mask, 0.0f, 1.0f);
  float4 result;
  if (s.mode == Mode::Switch) {
    /* A plain switch between the two, alpha included. */
    result = math::interpolate(bg, fg, amount);
  }
  else if (s.alpha == AlphaMode::StraightAlpha) {
    float4 blended;
    for (int i = 0; i < 4; i++) {
      blended[i] = blend_channel(s.mode, bg[i], fg[i]);
    }
    result = math::interpolate(bg, blended, amount);
  }
  else {
    const float3 blended(blend_channel(s.mode, bg.x, fg.x),
                         blend_channel(s.mode, bg.y, fg.y),
                         blend_channel(s.mode, bg.z, fg.z));
    if (s.alpha == AlphaMode::UseAlpha) {
      const float a = amount * math::clamp(fg.w, 0.0f, 1.0f);
      result = float4(math::interpolate(bg.xyz(), blended, a), a + bg.w * (1.0f - a));
    }
    else {
      result = float4(math::interpolate(bg.xyz(), blended, amount), bg.w);
    }
  }
  if (s.clamp) {
    result = math::clamp(result, float4(0.0f), float4(1.0f));
  }
  return result;
}

class BlendOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &foreground = this->get_input("Foreground");
    const Result &background = this->get_input("Background");
    const Result &mask = this->get_input("Mask");
    BlendSettings s;
    s.mode = Mode(this->get_input("Mode").get_single_value_default<MenuValue>().value);
    s.alpha = AlphaMode(this->get_input("Alpha").get_single_value_default<MenuValue>().value);
    s.opacity = this->get_input("Opacity").get_single_value_default<float>();
    s.clamp = this->get_input("Clamp").get_single_value_default<bool>();
    const float2 crop_min(this->get_input("Min X").get_single_value_default<float>(),
                          this->get_input("Min Y").get_single_value_default<float>());
    const float2 crop_max(this->get_input("Max X").get_single_value_default<float>(),
                          this->get_input("Max Y").get_single_value_default<float>());
    const bool cropped = crop_min.x > 0.0f || crop_min.y > 0.0f || crop_max.x < 1.0f ||
                         crop_max.y < 1.0f;

    Result &output = this->get_result("Image");

    /* Nothing laid on top (not connected and left transparent): the background comes through as
     * it is, whatever the alpha setting. A color typed in is still blended. */
    const bNodeSocket *foreground_socket = this->node().input_by_identifier("Foreground");
    if (foreground_socket && !foreground_socket->is_directly_linked() &&
        foreground.is_single_value() && float4(foreground.get_single_value<Color>()).w <= 0.0f)
    {
      if (background.is_single_value()) {
        output.allocate_single_value();
        output.set_single_value(background.get_single_value<Color>());
      }
      else {
        output.allocate_texture(background.domain());
        parallel_for(background.domain().data_size, [&](const int2 texel) {
          output.store_pixel(texel, background.load_pixel<Color>(texel));
        });
      }
      return;
    }
    const bool all_single = foreground.is_single_value() && background.is_single_value() &&
                            mask.is_single_value();
    if (all_single && !cropped) {
      output.allocate_single_value();
      output.set_single_value(Color(blend_pixel(s,
                                                float4(foreground.get_single_value<Color>()),
                                                float4(background.get_single_value<Color>()),
                                                mask.get_single_value<float>())));
      return;
    }

    /* The crop area needs pixels, so single values still give an image of the graph size. */
    const Domain domain = all_single ? this->context().get_compositing_domain() :
                                       this->compute_domain();
    const int2 size = domain.data_size;
    output.allocate_texture(domain);
    parallel_for(size, [&](const int2 texel) {
      const float4 bg = float4(background.load_pixel<Color, true>(texel));
      if (cropped) {
        const float2 uv = (float2(texel) + 0.5f) / float2(size);
        if (uv.x < crop_min.x || uv.x > crop_max.x || uv.y < crop_min.y || uv.y > crop_max.y) {
          output.store_pixel(texel, Color(bg));
          return;
        }
      }
      const float4 fg = float4(foreground.load_pixel<Color, true>(texel));
      output.store_pixel(
          texel, Color(blend_pixel(s, fg, bg, mask.load_pixel<float, true>(texel))));
    });
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new BlendOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeBlend");
  ntype.ui_name = "Blend";
  ntype.ui_description =
      "Combine a foreground with a background, with a blend mode, opacity, mask and crop area";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_BLEND";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_blend_modes_cc

/** \} */

/* -------------------------------------------------------------------- */
/** \name Height Blend
 * \{ */

namespace blender::nodes::node_texture_graph_height_blend_cc {

using namespace blender::compositor;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Float>("Height")
      .structure_type(StructureType::Dynamic)
      .description("The two heights, the higher one on top");
  b.add_output<decl::Float>("Mask").structure_type(StructureType::Dynamic).description(
      "White where the top height shows, soft where they meet");
  b.add_input<decl::Float>("Top")
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .structure_type(StructureType::Dynamic)
      .description("Height laid on top, it shows where it stands higher");
  b.add_input<decl::Float>("Bottom")
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .structure_type(StructureType::Dynamic)
      .description("Height underneath");
  b.add_input<decl::Float>("Mask")
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .structure_type(StructureType::Dynamic)
      .description("Where the top height may show");
  b.add_input<decl::Float>("Height Position")
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Raises or lowers the top height, above half it covers more");
  b.add_input<decl::Float>("Contrast")
      .default_value(0.9f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("How sharp the change from bottom to top is");
  b.add_input<decl::Float>("Opacity")
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("How much of the top height shows");
}

struct HeightBlendSettings {
  float offset;
  float width;
  float opacity;
};

static void height_blend(const HeightBlendSettings &s,
                         const float top,
                         const float bottom,
                         const float mask,
                         float &r_height,
                         float &r_mask)
{
  const float raised = top + s.offset;
  const float t = math::clamp((raised - bottom) / s.width + 0.5f, 0.0f, 1.0f);
  const float m = t * t * (3.0f - 2.0f * t) * math::clamp(mask, 0.0f, 1.0f) * s.opacity;
  r_mask = m;
  r_height = math::interpolate(bottom, raised, m);
}

class HeightBlendOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &top = this->get_input("Top");
    const Result &bottom = this->get_input("Bottom");
    const Result &mask = this->get_input("Mask");
    HeightBlendSettings s;
    s.offset = (this->get_input("Height Position").get_single_value_default<float>() - 0.5f) *
               2.0f;
    s.width = math::max(
        1.0f - this->get_input("Contrast").get_single_value_default<float>(), 1e-3f);
    s.opacity = math::clamp(
        this->get_input("Opacity").get_single_value_default<float>(), 0.0f, 1.0f);

    Result &height_out = this->get_result("Height");
    Result &mask_out = this->get_result("Mask");
    if (top.is_single_value() && bottom.is_single_value() && mask.is_single_value()) {
      float h, m;
      height_blend(s,
                   top.get_single_value<float>(),
                   bottom.get_single_value<float>(),
                   mask.get_single_value<float>(),
                   h,
                   m);
      if (height_out.should_compute()) {
        height_out.allocate_single_value();
        height_out.set_single_value(h);
      }
      if (mask_out.should_compute()) {
        mask_out.allocate_single_value();
        mask_out.set_single_value(m);
      }
      return;
    }

    const Domain domain = this->compute_domain();
    if (height_out.should_compute()) {
      height_out.allocate_texture(domain);
    }
    if (mask_out.should_compute()) {
      mask_out.allocate_texture(domain);
    }
    parallel_for(domain.data_size, [&](const int2 texel) {
      float h, m;
      height_blend(s,
                   top.load_pixel<float, true>(texel),
                   bottom.load_pixel<float, true>(texel),
                   mask.load_pixel<float, true>(texel),
                   h,
                   m);
      if (height_out.should_compute()) {
        height_out.store_pixel(texel, h);
      }
      if (mask_out.should_compute()) {
        mask_out.store_pixel(texel, m);
      }
    });
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new HeightBlendOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeHeightBlend");
  ntype.ui_name = "Height Blend";
  ntype.ui_description = "Lay one height on another, the top shows where it stands higher";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_HEIGHT_BLEND";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_height_blend_cc

/** \} */
