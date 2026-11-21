/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Texture Graph shape effects, all built on the distance to the edge of a mask: Extend Shape,
 * Shape Drop Shadow, Shape Glow and Shape Stroke.
 */

#include "texture_graph_filter.hh"

namespace blender::nodes::node_texture_graph_shape_fx_cc {

namespace tg = texture_graph;
using namespace tg::filter;

/* -------------------------------------------------------------------- */
/** \name Extend Shape
 * \{ */

namespace extend_shape {

static void node_declare(NodeDeclarationBuilder &b)
{
  color_out(b, "Image", "The shapes grown outward, keeping the colors of their edges");
  color_in(b, "Image", float4(1.0f), "Colors of the shapes");
  value_in(b, "Mask", 1.0f, "Where the shapes are, white is inside");
  float_in(b, "Distance", 0.05f, 0.0f, 1.0f, "How far to grow, next to the image width");
  bool_in(b, "Fill All", false, "Grow until the whole image is covered");
}

class Operation : public FilterOperation {
 public:
  using FilterOperation::FilterOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float4> image = this->colors("Image", size);
    const Array<float> mask = this->values("Mask", size);
    const bool fill_all = this->b("Fill All");
    const float distance = this->f("Distance") * float(math::max(size.x, size.y));
    Array<bool> seeds(mask.size());
    for (const int64_t k : mask.index_range()) {
      seeds[k] = mask[k] >= 0.5f;
    }
    const Array<int2> nearest = nearest_seeds(seeds, size, true);
    Array<float4> out(image.size());
    each_pixel(size, [&](const int2 p, const int64_t index) {
      if (seeds[index]) {
        out[index] = image[index];
        return;
      }
      const int2 n = nearest[index];
      if (n.x < 0) {
        out[index] = float4(0.0f);
        return;
      }
      const float d = math::length(tg::wrapped_delta(float2(n - p), float2(size)));
      out[index] = (fill_all || d <= distance) ? image[int64_t(n.y) * size.x + n.x] :
                                                 float4(0.0f);
    });
    this->out_colors("Image", domain, out);
  }
};

}  // namespace extend_shape

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shape Drop Shadow
 * \{ */

namespace drop_shadow {

static void node_declare(NodeDeclarationBuilder &b)
{
  color_out(b, "Image", "The shapes over their shadow, with alpha");
  value_out(b, "Shadow", "The shadow alone");
  value_in(b, "Shape", 0.0f, "Shapes that cast the shadow, white is inside");
  angle_in(b, "Angle", float(M_PI) * -0.25f, "Direction the shadow falls in");
  float_in(b, "Distance", 0.02f, 0.0f, 0.5f, "How far the shadow falls, next to the image width");
  float_in(b, "Spread", 0.02f, 0.0f, 0.5f, "How soft the shadow is, next to the image width");
  factor_in(b, "Opacity", 0.6f, "How dark the shadow is");
  color_in(b, "Shape Color", float4(1.0f), "Color of the shapes");
  color_in(b, "Shadow Color", float4(0.0f, 0.0f, 0.0f, 1.0f), "Color of the shadow");
}

class Operation : public FilterOperation {
 public:
  using FilterOperation::FilterOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float> shape = this->values("Shape", size);
    const float width = float(math::max(size.x, size.y));
    const float angle = this->f("Angle");
    const float2 offset = float2(math::cos(angle), math::sin(angle)) * this->f("Distance") * width;
    const float opacity = this->f("Opacity");
    const float4 shape_color = this->c("Shape Color");
    const float4 shadow_color = this->c("Shadow Color");
    Array<float> shadow(shape.size());
    each_pixel(size, [&](const int2 p, const int64_t index) {
      shadow[index] = sample_wrapped(shape.as_span(), size, float2(p) + 0.5f - offset);
    });
    tg::blur_wrapped(shadow.as_mutable_span(), size, this->f("Spread") * width);
    Array<float4> image(shape.size());
    for (const int64_t k : shape.index_range()) {
      const float s = math::clamp(shape[k], 0.0f, 1.0f);
      const float sh = math::clamp(shadow[k], 0.0f, 1.0f) * opacity * (1.0f - s);
      shadow[k] = sh;
      const float alpha = s + sh;
      const float3 rgb = alpha > 0.0f ?
                             (shape_color.xyz() * s + shadow_color.xyz() * sh) / alpha :
                             float3(0.0f);
      image[k] = float4(rgb, alpha);
    }
    this->out_colors("Image", domain, image);
    this->out_values("Shadow", domain, shadow);
  }
};

}  // namespace drop_shadow

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shape Glow
 * \{ */

namespace glow {

static void node_declare(NodeDeclarationBuilder &b)
{
  color_out(b, "Image", "The shapes with the glow, with alpha");
  value_out(b, "Glow", "The glow alone");
  value_in(b, "Shape", 0.0f, "Shapes that glow, white is inside");
  float_in(b, "Size", 0.05f, 0.0f, 0.5f, "How far the glow reaches, next to the image width");
  float_in(b, "Intensity", 1.0f, 0.0f, 4.0f, "How bright the glow is");
  bool_in(b, "Inner", false, "Glow inward from the edges instead of outward");
  color_in(b, "Shape Color", float4(1.0f), "Color of the shapes");
  color_in(b, "Glow Color", float4(1.0f, 0.8f, 0.3f, 1.0f), "Color of the glow");
}

class Operation : public FilterOperation {
 public:
  using FilterOperation::FilterOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float> shape = this->values("Shape", size);
    const float reach = math::max(this->f("Size") * float(math::max(size.x, size.y)), 1e-3f);
    const float intensity = this->f("Intensity");
    const bool inner = this->b("Inner");
    const float4 shape_color = this->c("Shape Color");
    const float4 glow_color = this->c("Glow Color");
    const Array<float> sdf = signed_distance(shape, size, true);
    Array<float> glow(shape.size());
    Array<float4> image(shape.size());
    for (const int64_t k : shape.index_range()) {
      const float d = inner ? -sdf[k] : sdf[k];
      float g = d > 0.0f ? math::max(0.0f, 1.0f - d / reach) : 0.0f;
      g = math::min(g * g * intensity, 1.0f);
      glow[k] = g;
      const float s = math::clamp(shape[k], 0.0f, 1.0f);
      if (inner) {
        const float a = s;
        image[k] = float4(math::interpolate(shape_color.xyz(), glow_color.xyz(), g), a);
      }
      else {
        const float a = s + g * (1.0f - s);
        const float3 rgb = a > 0.0f ? (shape_color.xyz() * s + glow_color.xyz() * g * (1.0f - s)) / a :
                                      float3(0.0f);
        image[k] = float4(rgb, a);
      }
    }
    this->out_colors("Image", domain, image);
    this->out_values("Glow", domain, glow);
  }
};

}  // namespace glow

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shape Stroke
 * \{ */

namespace stroke {

static const EnumPropertyItem position_items[] = {
    {0, "OUTSIDE", 0, N_("Outside"), N_("The stroke is around the shape")},
    {1, "CENTER", 0, N_("Center"), N_("The stroke is on the edge")},
    {2, "INSIDE", 0, N_("Inside"), N_("The stroke is inside the shape")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  value_out(b, "Stroke", "A line along the edges of the shapes");
  value_in(b, "Shape", 0.0f, "Shapes to outline, white is inside");
  float_in(b, "Width", 0.01f, 0.0f, 0.5f, "Width of the line, next to the image width");
  menu_in(b, "Position", position_items, 0, "Where the line lies against the edge");
  float_in(b, "Softness", 1.0f, 0.0f, 32.0f, "Softens the line, in pixels");
}

class Operation : public FilterOperation {
 public:
  using FilterOperation::FilterOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float> shape = this->values("Shape", size);
    const float width = this->f("Width") * float(math::max(size.x, size.y));
    const int position = this->m("Position");
    const float soft = math::max(this->f("Softness"), 0.5f);
    const Array<float> sdf = signed_distance(shape, size, true);
    float lo, hi;
    if (position == 0) {
      lo = 0.0f;
      hi = width;
    }
    else if (position == 1) {
      lo = -width * 0.5f;
      hi = width * 0.5f;
    }
    else {
      lo = -width;
      hi = 0.0f;
    }
    Array<float> out(shape.size());
    for (const int64_t k : shape.index_range()) {
      const float d = sdf[k];
      const float a = math::clamp((d - lo) / soft + 0.5f, 0.0f, 1.0f);
      const float b = math::clamp((hi - d) / soft + 0.5f, 0.0f, 1.0f);
      out[k] = math::min(a, b);
    }
    this->out_values("Stroke", domain, out);
  }
};

}  // namespace stroke

/** \} */

static void node_register()
{
  static bke::bNodeType types[4];
  register_filter(types[0], "TextureGraphNodeExtendShape", "Extend Shape",
                  "Grow shapes outward, keeping the colors of their edges",
                  "TEXTURE_GRAPH_EXTEND_SHAPE", NODE_CLASS_OP_FILTER,
                  extend_shape::node_declare, make_operation<extend_shape::Operation>);
  register_filter(types[1], "TextureGraphNodeShapeDropShadow", "Shape Drop Shadow",
                  "Put a soft shadow under shapes", "TEXTURE_GRAPH_SHAPE_DROP_SHADOW",
                  NODE_CLASS_OP_FILTER, drop_shadow::node_declare,
                  make_operation<drop_shadow::Operation>);
  register_filter(types[2], "TextureGraphNodeShapeGlow", "Shape Glow",
                  "Make shapes glow around or inside their edges", "TEXTURE_GRAPH_SHAPE_GLOW",
                  NODE_CLASS_OP_FILTER, glow::node_declare, make_operation<glow::Operation>);
  register_filter(types[3], "TextureGraphNodeShapeStroke", "Shape Stroke",
                  "Draw a line along the edges of shapes", "TEXTURE_GRAPH_SHAPE_STROKE",
                  NODE_CLASS_OP_FILTER, stroke::node_declare, make_operation<stroke::Operation>);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_shape_fx_cc
