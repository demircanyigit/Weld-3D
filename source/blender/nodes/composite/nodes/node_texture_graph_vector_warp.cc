/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Texture Graph warps driven by maps: Multi Directional Warp, Non Uniform Directional Warp,
 * Swirl, Vector Morph and Vector Warp. All of them wrap at the edges.
 */

#include "texture_graph_filter.hh"

namespace blender::nodes::node_texture_graph_vector_warp_cc {

namespace tg = texture_graph;
using namespace tg::filter;

/* -------------------------------------------------------------------- */
/** \name Multi Directional Warp
 * \{ */

namespace multi_directional {

static const EnumPropertyItem mode_items[] = {
    {0, "AVERAGE", 0, N_("Average"), N_("Average of the warped copies")},
    {1, "MAX", 0, N_("Max"), N_("Brightest of the warped copies")},
    {2, "MIN", 0, N_("Min"), N_("Darkest of the warped copies")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  color_out(b, "Image");
  color_in(b, "Image", float4(0.5f, 0.5f, 0.5f, 1.0f), "Image to warp");
  value_in(b, "Intensity Map", 1.0f, "How far each pixel is pushed");
  float_in(b, "Intensity", 10.0f, 0.0f, 256.0f, "Push distance, in pixels of a 256 image");
  int_in(b, "Directions", 4, 1, 16, "Number of directions to push in, spread evenly");
  angle_in(b, "Angle", 0.0f, "Turns all the directions");
  menu_in(b, "Mode", mode_items, 0, "How the warped copies are put together");
}

class Operation : public FilterOperation {
 public:
  using FilterOperation::FilterOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float4> image = this->colors("Image", size);
    const Array<float> map = this->values("Intensity Map", size);
    const float intensity = tg::scaled_radius(this->f("Intensity"), size);
    const int directions = math::max(this->i("Directions"), 1);
    const float angle = this->f("Angle");
    const int mode = this->m("Mode");
    Array<float4> out(image.size());
    each_pixel(size, [&](const int2 p, const int64_t index) {
      const float push = map[index] * intensity;
      float4 result = mode == 0 ? float4(0.0f) : (mode == 1 ? float4(-1e30f) : float4(1e30f));
      for (int d = 0; d < directions; d++) {
        const float a = angle + float(d) / float(directions) * float(M_PI * 2.0);
        const float4 s = sample_wrapped(
            image.as_span(), size, float2(p) + 0.5f + float2(math::cos(a), math::sin(a)) * push);
        if (mode == 0) {
          result += s;
        }
        else if (mode == 1) {
          result = math::max(result, s);
        }
        else {
          result = math::min(result, s);
        }
      }
      out[index] = mode == 0 ? result / float(directions) : result;
    });
    this->out_colors("Image", domain, out);
  }
};

}  // namespace multi_directional

/** \} */

/* -------------------------------------------------------------------- */
/** \name Non Uniform Directional Warp
 * \{ */

namespace non_uniform {

static void node_declare(NodeDeclarationBuilder &b)
{
  color_out(b, "Image");
  color_in(b, "Image", float4(0.5f, 0.5f, 0.5f, 1.0f), "Image to warp");
  value_in(b, "Intensity Map", 1.0f, "How far each pixel is pushed");
  value_in(b, "Angle Map", 0.0f, "Direction each pixel is pushed in, one turn from 0 to 1");
  float_in(b, "Intensity", 10.0f, 0.0f, 256.0f, "Push distance, in pixels of a 256 image");
  angle_in(b, "Angle", 0.0f, "Added to the direction of every pixel");
  int_in(b, "Steps", 1, 1, 32, "Push in small steps that follow the angle map");
}

class Operation : public FilterOperation {
 public:
  using FilterOperation::FilterOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float4> image = this->colors("Image", size);
    const Array<float> intensity_map = this->values("Intensity Map", size);
    const Array<float> angle_map = this->values("Angle Map", size);
    const float intensity = tg::scaled_radius(this->f("Intensity"), size);
    const float angle = this->f("Angle");
    const int steps = math::max(this->i("Steps"), 1);
    Array<float4> out(image.size());
    each_pixel(size, [&](const int2 p, const int64_t index) {
      float2 at = float2(p) + 0.5f;
      for (int s = 0; s < steps; s++) {
        const float push = sample_wrapped(intensity_map.as_span(), size, at) * intensity /
                           float(steps);
        const float a = sample_wrapped(angle_map.as_span(), size, at) * float(M_PI * 2.0) + angle;
        at += float2(math::cos(a), math::sin(a)) * push;
      }
      out[index] = sample_wrapped(image.as_span(), size, at);
    });
    this->out_colors("Image", domain, out);
  }
};

}  // namespace non_uniform

/** \} */

/* -------------------------------------------------------------------- */
/** \name Swirl
 * \{ */

namespace swirl {

static void node_declare(NodeDeclarationBuilder &b)
{
  color_out(b, "Image");
  color_in(b, "Image", float4(0.5f, 0.5f, 0.5f, 1.0f), "Image to twist");
  vector2_in(b, "Center", float2(0.5f), 0.0f, 1.0f, "Middle of the twist");
  float_in(b, "Radius", 0.4f, 0.0f, 2.0f, "How far the twist reaches, next to the image width");
  float_in(b, "Amount", 1.0f, -8.0f, 8.0f, "How many turns the middle twists");
}

class Operation : public FilterOperation {
 public:
  using FilterOperation::FilterOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float4> image = this->colors("Image", size);
    const float2 center = this->v("Center");
    const float radius = math::max(this->f("Radius"), 1e-4f);
    const float amount = this->f("Amount") * float(M_PI * 2.0);
    const float aspect = float(size.x) / float(math::max(size.y, 1));
    Array<float4> out(image.size());
    each_pixel(size, [&](const int2 p, const int64_t index) {
      float2 d = uv_of(p, size) - center;
      d.x *= aspect;
      const float r = math::length(d);
      const float t = math::max(0.0f, 1.0f - r / radius);
      d = rotate2(d, -amount * t * t);
      d.x /= aspect;
      out[index] = sample_uv(image.as_span(), size, center + d);
    });
    this->out_colors("Image", domain, out);
  }
};

}  // namespace swirl

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vector Morph and Vector Warp
 * \{ */

namespace vector_map {

static const EnumPropertyItem format_items[] = {
    {0, "SIGNED", 0, N_("Signed"), N_("Gray 0.5 is no push, 0 and 1 push in either direction")},
    {1, "POSITIVE", 0, N_("Positive"), N_("0 is no push, 1 pushes the most")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void declare_common(NodeDeclarationBuilder &b, const char *intensity_description)
{
  color_out(b, "Image");
  color_in(b, "Image", float4(0.5f, 0.5f, 0.5f, 1.0f), "Image to warp");
  color_in(b, "Vector Map", float4(0.5f, 0.5f, 0.5f, 1.0f), "Push direction in red and green");
  float_in(b, "Intensity", 10.0f, 0.0f, 256.0f, intensity_description);
  menu_in(b, "Vector Format", format_items, 0, "How the vector map's values are read");
}

static void declare_morph(NodeDeclarationBuilder &b)
{
  declare_common(b, "Distance along the vector map, in pixels of a 256 image");
  int_in(b, "Steps", 16, 1, 64, "Number of small steps that follow the vector map");
}

static void declare_warp(NodeDeclarationBuilder &b)
{
  declare_common(b, "Push distance, in pixels of a 256 image");
}

template<bool morph> class Operation : public FilterOperation {
 public:
  using FilterOperation::FilterOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float4> image = this->colors("Image", size);
    const Array<float4> raw = this->colors("Vector Map", size);
    const bool signed_format = this->m("Vector Format") == 0;
    Array<float2> field(raw.size());
    for (const int64_t k : raw.index_range()) {
      const float2 v(raw[k].x, raw[k].y);
      field[k] = signed_format ? v * 2.0f - 1.0f : v;
    }
    const float intensity = tg::scaled_radius(this->f("Intensity"), size);
    const int steps = morph ? math::max(this->i("Steps"), 1) : 1;
    Array<float4> out(image.size());
    each_pixel(size, [&](const int2 p, const int64_t index) {
      float2 at = float2(p) + 0.5f;
      if (morph) {
        for (int s = 0; s < steps; s++) {
          at -= sample_wrapped(field.as_span(), size, at) * intensity / float(steps);
        }
      }
      else {
        at -= field[index] * intensity;
      }
      out[index] = sample_wrapped(image.as_span(), size, at);
    });
    this->out_colors("Image", domain, out);
  }
};

}  // namespace vector_map

/** \} */

static void node_register()
{
  static bke::bNodeType types[5];
  register_filter(types[0], "TextureGraphNodeMultiDirectionalWarp", "Multi Directional Warp",
                  "Push an image in several directions at once and put the copies together",
                  "TEXTURE_GRAPH_MULTI_DIRECTIONAL_WARP", NODE_CLASS_DISTORT,
                  multi_directional::node_declare, make_operation<multi_directional::Operation>);
  register_filter(types[1], "TextureGraphNodeNonUniformDirectionalWarp",
                  "Non Uniform Directional Warp",
                  "Push each pixel of an image in a direction read from an angle map",
                  "TEXTURE_GRAPH_NON_UNIFORM_DIRECTIONAL_WARP", NODE_CLASS_DISTORT,
                  non_uniform::node_declare, make_operation<non_uniform::Operation>);
  register_filter(types[2], "TextureGraphNodeSwirl", "Swirl", "Twist an image around a point",
                  "TEXTURE_GRAPH_SWIRL", NODE_CLASS_DISTORT, swirl::node_declare,
                  make_operation<swirl::Operation>);
  register_filter(types[3], "TextureGraphNodeVectorMorph", "Vector Morph",
                  "Smear an image along the flow of a vector map", "TEXTURE_GRAPH_VECTOR_MORPH",
                  NODE_CLASS_DISTORT, vector_map::declare_morph, make_operation<vector_map::Operation<true>>);
  register_filter(types[4], "TextureGraphNodeVectorWarp", "Vector Warp",
                  "Push each pixel of an image by a vector map", "TEXTURE_GRAPH_VECTOR_WARP",
                  NODE_CLASS_DISTORT, vector_map::declare_warp, make_operation<vector_map::Operation<false>>);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_vector_warp_cc
