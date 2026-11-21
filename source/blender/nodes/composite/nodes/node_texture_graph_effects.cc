/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Texture Graph effect filters: ambient occlusion, bevel, cross section, curvature, diffusion,
 * glossy and lit embossing, mosaic, summed area table and cast shadows.
 */

#include "texture_graph_filter.hh"

namespace blender::nodes::node_texture_graph_effects_cc {

namespace tg = texture_graph;
using namespace tg::filter;

/* -------------------------------------------------------------------- */
/** \name Ambient Occlusion
 * \{ */

namespace ambient_occlusion {

static void node_declare(NodeDeclarationBuilder &b)
{
  value_out(b, "Ambient Occlusion", "White where the surface is open to the sky");
  value_in(b, "Height", 0.5f, "Height the occlusion is found on");
  float_in(b, "Height Depth", 0.1f, 0.0f, 1.0f, "How deep the height is, next to the image width");
  float_in(b, "Radius", 0.15f, 0.0f, 1.0f, "How far to look for occluders, next to the image width");
  int_in(b, "Quality", 8, 4, 32, "Number of directions to look in");
  float_in(b, "Strength", 1.0f, 0.0f, 4.0f, "How dark the occlusion gets");
}

class Operation : public FilterOperation {
 public:
  using FilterOperation::FilterOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float> height = this->values("Height", size);
    const float width = float(math::max(size.x, size.y));
    const float depth = this->f("Height Depth") * width;
    const float radius = math::max(this->f("Radius") * width, 1.0f);
    const int directions = math::max(this->i("Quality"), 1);
    const float strength = this->f("Strength");
    Array<float> ao(height.size());
    each_pixel(size, [&](const int2 p, const int64_t index) {
      float sum = 0.0f;
      horizons(height, size, p, depth, radius, directions, [&](const float2 /*dir*/, const float h) {
        sum += h;
      });
      ao[index] = math::clamp(1.0f - sum / float(directions) * strength, 0.0f, 1.0f);
    });
    this->out_values("Ambient Occlusion", domain, ao);
  }
};

}  // namespace ambient_occlusion

/** \} */

/* -------------------------------------------------------------------- */
/** \name Bevel
 * \{ */

namespace bevel {

static const EnumPropertyItem corner_items[] = {
    {0, "ROUND", 0, N_("Round"), N_("Rounded slope")},
    {1, "LINEAR", 0, N_("Linear"), N_("Straight slope")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  value_out(b, "Height", "Height of the beveled shape");
  color_out(b, "Normal", "Normal map of the bevel");
  value_in(b, "Shape", 0.0f, "Shapes to bevel, white is inside");
  float_in(b, "Distance", 0.1f, 0.0f, 1.0f, "Width of the bevel, next to the image width");
  float_in(b, "Smoothing", 0.0f, 0.0f, 32.0f, "Blurs the bevel, in pixels of a 256 image");
  menu_in(b, "Corner", corner_items, 0, "Shape of the slope");
  float_in(b, "Normal Intensity", 2.0f, 0.0f, 32.0f, "Strength of the normal map");
  normal_format_in(b);
}

class Operation : public FilterOperation {
 public:
  using FilterOperation::FilterOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float> shape = this->values("Shape", size);
    const float distance = math::max(this->f("Distance") * float(math::max(size.x, size.y)), 1e-3f);
    const bool round = this->m("Corner") == 0;
    const Array<float> sdf = signed_distance(shape, size, true);
    Array<float> height(shape.size());
    for (const int64_t k : shape.index_range()) {
      float t = math::clamp(-sdf[k] / distance, 0.0f, 1.0f);
      if (round) {
        t = math::sqrt(math::max(0.0f, 1.0f - (1.0f - t) * (1.0f - t)));
      }
      height[k] = t;
    }
    tg::blur_wrapped(height.as_mutable_span(), size, tg::scaled_radius(this->f("Smoothing"), size));
    this->out_values("Height", domain, height);
    if (this->wants("Normal")) {
      const float scale = this->f("Normal Intensity") * distance * 0.25f;
      const bool directx = this->directx();
      Array<float4> normal(height.size());
      each_pixel(size, [&](const int2 p, const int64_t index) {
        normal[index] = encode_normal(height_normal(height, size, p, scale), directx);
      });
      this->out_colors("Normal", domain, normal);
    }
  }
};

}  // namespace bevel

/** \} */

/* -------------------------------------------------------------------- */
/** \name Cross Section
 * \{ */

namespace cross_section {

static void node_declare(NodeDeclarationBuilder &b)
{
  value_out(b, "Image", "The profile of one line of the height, white under it");
  value_in(b, "Height", 0.5f, "Height to cut through");
  factor_in(b, "Position", 0.5f, "Which row to cut along, bottom to top");
  bool_in(b, "Vertical", false, "Cut along a column instead of a row");
  float_in(b, "Smoothing", 1.0f, 0.0f, 16.0f, "Softens the edge of the profile, in pixels");
}

class Operation : public FilterOperation {
 public:
  using FilterOperation::FilterOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float> height = this->values("Height", size);
    const float position = this->f("Position");
    const bool vertical = this->b("Vertical");
    const float soft = math::max(this->f("Smoothing"), 1e-3f);
    Array<float> out(height.size());
    each_pixel(size, [&](const int2 p, const int64_t index) {
      const float2 uv = uv_of(p, size);
      const float along = vertical ? uv.y : uv.x;
      const float across = vertical ? uv.x : uv.y;
      const float2 at = vertical ? float2(position, along) : float2(along, position);
      const float h = sample_uv_clamped(height.as_span(), size, at);
      const float pixels = (h - across) * float(vertical ? size.x : size.y);
      out[index] = math::clamp(pixels / soft + 0.5f, 0.0f, 1.0f);
    });
    this->out_values("Image", domain, out);
  }
};

}  // namespace cross_section

/** \} */

/* -------------------------------------------------------------------- */
/** \name Curvature
 * \{ */

namespace curvature {

enum class Kind { Plain, Smooth, Sobel };

static void declare_common(NodeDeclarationBuilder &b)
{
  value_out(b, "Curvature", "Gray is flat, white is convex, black is concave");
  normal_in(b);
  float_in(b, "Intensity", 1.0f, 0.0f, 32.0f, "Strength of the curvature");
  normal_format_in(b);
}

static void declare_plain(NodeDeclarationBuilder &b)
{
  declare_common(b);
}

static void declare_smooth(NodeDeclarationBuilder &b)
{
  declare_common(b);
  float_in(b, "Radius", 8.0f, 1.0f, 64.0f, "Size of the bends to find, in pixels of a 256 image");
}

static void declare_sobel(NodeDeclarationBuilder &b)
{
  declare_common(b);
}

template<Kind kind> class Operation : public FilterOperation {
 public:
  using FilterOperation::FilterOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float4> colors = this->colors("Normal", size);
    const bool directx = this->directx();
    const float intensity = this->f("Intensity");
    Array<float2> n(colors.size());
    for (const int64_t k : colors.index_range()) {
      const float3 d = decode_normal(colors[k], directx);
      n[k] = float2(d.x, d.y);
    }
    if constexpr (kind == Kind::Smooth) {
      /* Sum of the curvature at a few sizes up to the radius. */
      const float radius = tg::scaled_radius(this->f("Radius"), size);
      Array<float> total(n.size(), 0.0f);
      float weight = 0.0f;
      for (float r = 1.0f; r <= math::max(radius, 1.0f); r *= 2.0f) {
        Array<float2> blurred = n;
        tg::blur_wrapped(blurred.as_mutable_span(), size, r);
        const int step = math::max(int(r * 0.5f), 1);
        each_pixel(size, [&](const int2 p, const int64_t index) {
          const auto at = [&](const int dx, const int dy) {
            const int2 w = tg::wrap_texel(p + int2(dx, dy), size);
            return blurred[int64_t(w.y) * size.x + w.x];
          };
          total[index] += (at(step, 0).x - at(-step, 0).x + at(0, step).y - at(0, -step).y) * 0.5f;
        });
        weight += 1.0f;
      }
      Array<float> out(n.size());
      for (const int64_t k : out.index_range()) {
        out[k] = math::clamp(0.5f + total[k] / weight * intensity * 0.5f, 0.0f, 1.0f);
      }
      this->out_values("Curvature", domain, out);
      return;
    }
    Array<float> out(n.size());
    each_pixel(size, [&](const int2 p, const int64_t index) {
      const auto at = [&](const int dx, const int dy) {
        const int2 w = tg::wrap_texel(p + int2(dx, dy), size);
        return n[int64_t(w.y) * size.x + w.x];
      };
      float c;
      if constexpr (kind == Kind::Sobel) {
        const float dx = ((at(1, 1).x + 2.0f * at(1, 0).x + at(1, -1).x) -
                          (at(-1, 1).x + 2.0f * at(-1, 0).x + at(-1, -1).x)) /
                         8.0f;
        const float dy = ((at(-1, 1).y + 2.0f * at(0, 1).y + at(1, 1).y) -
                          (at(-1, -1).y + 2.0f * at(0, -1).y + at(1, -1).y)) /
                         8.0f;
        c = dx + dy;
      }
      else {
        c = (at(1, 0).x - at(-1, 0).x + at(0, 1).y - at(0, -1).y) * 0.5f;
      }
      out[index] = math::clamp(0.5f + c * intensity * 0.5f, 0.0f, 1.0f);
    });
    this->out_values("Curvature", domain, out);
  }
};

}  // namespace curvature

/** \} */

/* -------------------------------------------------------------------- */
/** \name Diffusion
 * \{ */

namespace diffusion {

enum class Kind { Color, Grayscale, UV };

/* Fills the pixels whose weight is zero from the ones around them, pulling known values down a
 * pyramid of half sizes and pushing them back up. Wraps at the edges. */
static void push_pull(MutableSpan<float4> data, MutableSpan<float> weight, const int2 size)
{
  if (size.x <= 1 && size.y <= 1) {
    return;
  }
  const int2 half = math::max((size + 1) / 2, int2(1));
  Array<float4> coarse(int64_t(half.x) * half.y, float4(0.0f));
  Array<float> coarse_w(coarse.size(), 0.0f);
  for (int y = 0; y < size.y; y++) {
    for (int x = 0; x < size.x; x++) {
      const int64_t i = int64_t(y) * size.x + x;
      const int64_t j = int64_t(y / 2) * half.x + x / 2;
      coarse[j] += data[i] * weight[i];
      coarse_w[j] += weight[i];
    }
  }
  for (const int64_t j : coarse.index_range()) {
    if (coarse_w[j] > 0.0f) {
      coarse[j] /= coarse_w[j];
      coarse_w[j] = math::min(coarse_w[j], 1.0f);
    }
  }
  push_pull(coarse, coarse_w, half);
  for (int y = 0; y < size.y; y++) {
    for (int x = 0; x < size.x; x++) {
      const int64_t i = int64_t(y) * size.x + x;
      if (weight[i] >= 1.0f) {
        continue;
      }
      const float2 at = (float2(x, y) + 0.5f) * float2(half) / float2(size);
      const float4 fill = sample_wrapped(coarse.as_span(), half, at);
      data[i] = data[i] * weight[i] + fill * (1.0f - weight[i]);
      weight[i] = 1.0f;
    }
  }
}

static void declare_common(NodeDeclarationBuilder &b, const Kind kind)
{
  if (kind == Kind::Grayscale) {
    value_out(b, "Image", "The kept values spread over the rest of the image");
    value_in(b, "Image", 0.5f, "Values to spread");
  }
  else {
    color_out(b, "Image", "The kept colors spread over the rest of the image");
    if (kind == Kind::UV) {
      color_in(b, "Image", float4(0.5f, 0.5f, 0.0f, 1.0f), "UV map to spread");
    }
    else {
      color_in(b, "Image", float4(0.5f, 0.5f, 0.5f, 1.0f), "Colors to spread");
    }
  }
  value_in(b, "Mask", 1.0f, "Where the values are kept, they spread into the black parts");
  float_in(b, "Smoothing", 0.0f, 0.0f, 32.0f, "Blurs the spread part, in pixels of a 256 image");
}

static void declare_color(NodeDeclarationBuilder &b)
{
  declare_common(b, Kind::Color);
}
static void declare_grayscale(NodeDeclarationBuilder &b)
{
  declare_common(b, Kind::Grayscale);
}
static void declare_uv(NodeDeclarationBuilder &b)
{
  declare_common(b, Kind::UV);
}

template<Kind kind> class Operation : public FilterOperation {
 public:
  using FilterOperation::FilterOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    Array<float4> data = this->colors("Image", size);
    const Array<float> mask = this->values("Mask", size);
    Array<float> weight(mask.size());
    for (const int64_t k : mask.index_range()) {
      weight[k] = mask[k] >= 0.5f ? 1.0f : 0.0f;
    }
    if (kind == Kind::UV) {
      /* Spread positions as angles so the seam at 1 to 0 does not average to the middle. */
      for (float4 &c : data) {
        const float a = c.x * float(M_PI * 2.0);
        const float b = c.y * float(M_PI * 2.0);
        c = float4(math::cos(a), math::sin(a), math::cos(b), math::sin(b));
      }
    }
    const Array<float> original = weight;
    push_pull(data, weight, size);
    const float radius = tg::scaled_radius(this->f("Smoothing"), size);
    if (radius > 0.0f) {
      Array<float4> blurred = data;
      tg::blur_wrapped(blurred.as_mutable_span(), size, radius);
      for (const int64_t k : data.index_range()) {
        if (original[k] < 1.0f) {
          data[k] = blurred[k];
        }
      }
    }
    if (kind == Kind::UV) {
      for (float4 &c : data) {
        const float u = std::atan2(c.y, c.x) / float(M_PI * 2.0);
        const float v = std::atan2(c.w, c.z) / float(M_PI * 2.0);
        c = float4(u - math::floor(u), v - math::floor(v), 0.0f, 1.0f);
      }
    }
    if (kind == Kind::Grayscale) {
      Array<float> out(data.size());
      for (const int64_t k : data.index_range()) {
        out[k] = data[k].x;
      }
      this->out_values("Image", domain, out);
    }
    else {
      this->out_colors("Image", domain, data);
    }
  }
};

}  // namespace diffusion

/** \} */

/* -------------------------------------------------------------------- */
/** \name Emboss With Gloss and Uber Emboss
 * \{ */

namespace emboss {

static void declare_gloss(NodeDeclarationBuilder &b)
{
  color_out(b, "Image");
  color_in(b, "Image", float4(0.5f, 0.5f, 0.5f, 1.0f), "Image to shade");
  value_in(b, "Height", 0.5f, "Height that catches the light");
  float_in(b, "Intensity", 1.0f, 0.0f, 32.0f, "How strong the light and shadow are");
  angle_in(b, "Light Angle", float(M_PI) * 0.75f, "Direction the light comes from");
  factor_in(b, "Gloss", 0.5f, "How sharp and bright the shine is");
  color_in(b, "Highlight", float4(1.0f), "Color of the lit side");
  color_in(b, "Shadow", float4(0.0f, 0.0f, 0.0f, 1.0f), "Color of the side in shadow");
}

static void declare_uber(NodeDeclarationBuilder &b)
{
  color_out(b, "Image");
  color_in(b, "Image", float4(0.5f, 0.5f, 0.5f, 1.0f), "Image to light");
  value_in(b, "Height", 0.5f, "Height that catches the light");
  float_in(b, "Intensity", 1.0f, 0.0f, 32.0f, "How tall the height looks");
  angle_in(b, "Light Angle", float(M_PI) * 0.75f, "Direction the light comes from");
  angle_in(b, "Light Elevation", float(M_PI) * 0.25f, "How high the light is above the surface");
  factor_in(b, "Ambient", 0.3f, "Light reaching the parts in shadow");
  factor_in(b, "Specular", 0.3f, "Strength of the shine");
  float_in(b, "Glossiness", 20.0f, 1.0f, 256.0f, "Sharpness of the shine");
  color_in(b, "Light Color", float4(1.0f), "Color of the light");
}

static float4 shade(const float4 color, const float3 normal, const float3 light, const float4 tint,
                    const float ambient, const float specular, const float glossiness)
{
  const float diffuse = math::max(math::dot(normal, light), 0.0f);
  const float3 half = math::normalize(light + float3(0.0f, 0.0f, 1.0f));
  const float spec = std::pow(math::max(math::dot(normal, half), 0.0f), glossiness) * specular;
  const float3 lit = color.xyz() * tint.xyz() * (ambient + (1.0f - ambient) * diffuse) +
                     tint.xyz() * spec;
  return float4(lit, color.w);
}

class GlossOperation : public FilterOperation {
 public:
  using FilterOperation::FilterOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float4> image = this->colors("Image", size);
    const Array<float> height = this->values("Height", size);
    const float scale = this->f("Intensity") * float(math::max(size.x, size.y)) * 0.05f;
    const float angle = this->f("Light Angle");
    const float gloss = this->f("Gloss");
    const float4 highlight = this->c("Highlight");
    const float4 shadow = this->c("Shadow");
    const float3 light = math::normalize(float3(math::cos(angle), math::sin(angle), 1.0f));
    const float glossiness = 2.0f + gloss * 120.0f;
    Array<float4> out(image.size());
    each_pixel(size, [&](const int2 p, const int64_t index) {
      const float3 n = height_normal(height, size, p, scale);
      const float lit = math::dot(n, light) - light.z;
      const float4 color = image[index];
      float4 result = lit > 0.0f ? math::interpolate(color, highlight, math::min(lit * 2.0f, 1.0f)) :
                                   math::interpolate(color, shadow, math::min(-lit * 2.0f, 1.0f));
      const float3 half = math::normalize(light + float3(0.0f, 0.0f, 1.0f));
      const float spec = std::pow(math::max(math::dot(n, half), 0.0f), glossiness) * gloss;
      result = float4(result.xyz() + highlight.xyz() * spec, color.w);
      out[index] = result;
    });
    this->out_colors("Image", domain, out);
  }
};

class UberOperation : public FilterOperation {
 public:
  using FilterOperation::FilterOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float4> image = this->colors("Image", size);
    const Array<float> height = this->values("Height", size);
    const float scale = this->f("Intensity") * float(math::max(size.x, size.y)) * 0.05f;
    const float angle = this->f("Light Angle");
    const float elevation = this->f("Light Elevation");
    const float3 light = math::normalize(float3(math::cos(angle) * math::cos(elevation),
                                                math::sin(angle) * math::cos(elevation),
                                                math::sin(elevation)));
    const float ambient = this->f("Ambient");
    const float specular = this->f("Specular");
    const float glossiness = this->f("Glossiness");
    const float4 tint = this->c("Light Color");
    /* Flat ground keeps the image as it is. */
    const float flat = ambient + (1.0f - ambient) * math::max(light.z, 1e-3f);
    Array<float4> out(image.size());
    each_pixel(size, [&](const int2 p, const int64_t index) {
      const float3 n = height_normal(height, size, p, scale);
      float4 lit = shade(image[index], n, light, tint, ambient, specular, glossiness);
      out[index] = float4(lit.xyz() / flat, lit.w);
    });
    this->out_colors("Image", domain, out);
  }
};

}  // namespace emboss

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mosaic
 * \{ */

namespace mosaic {

static void node_declare(NodeDeclarationBuilder &b)
{
  color_out(b, "Image");
  color_in(b, "Image", float4(0.5f, 0.5f, 0.5f, 1.0f), "Image to break into tiles");
  int_in(b, "Cells", 16, 1, 256, "Number of tiles across the image");
  factor_in(b, "Jitter", 1.0f, "How far the tiles move from a regular grid");
  float_in(b, "Grout", 0.0f, 0.0f, 0.5f, "Width of the lines between tiles");
  color_in(b, "Grout Color", float4(0.1f, 0.1f, 0.1f, 1.0f), "Color of the lines between tiles");
  seed_in(b);
}

class Operation : public FilterOperation {
 public:
  using FilterOperation::FilterOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float4> image = this->colors("Image", size);
    const int cells = math::max(this->i("Cells"), 1);
    const float jitter = this->f("Jitter");
    const float grout = this->f("Grout");
    const float4 grout_color = this->c("Grout Color");
    const uint32_t seed = this->seed();
    const auto point = [&](const int2 cell) {
      const int2 w = tg::wrap_texel(cell, int2(cells));
      const uint32_t h = tg::hash(w.x, w.y, seed);
      return float2(cell) + 0.5f +
             (float2(tg::hash_to_float(h), tg::hash_to_float(tg::hash(h))) - 0.5f) * jitter;
    };
    Array<float4> out(image.size());
    each_pixel(size, [&](const int2 p, const int64_t index) {
      const float2 g = uv_of(p, size) * float(cells);
      const int2 base = int2(math::floor(g));
      float d1 = 1e9f, d2 = 1e9f;
      float2 best(0.0f);
      for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
          const float2 q = point(base + int2(dx, dy));
          const float d = math::distance(q, g);
          if (d < d1) {
            d2 = d1;
            d1 = d;
            best = q;
          }
          else if (d < d2) {
            d2 = d;
          }
        }
      }
      float4 color = sample_uv(image.as_span(), size, best / float(cells));
      if (grout > 0.0f) {
        const float edge = (d2 - d1) * 0.5f;
        color = math::interpolate(grout_color, color, smooth01(grout * 0.5f, grout * 0.5f + 0.03f, edge));
      }
      out[index] = color;
    });
    this->out_colors("Image", domain, out);
  }
};

}  // namespace mosaic

/** \} */

/* -------------------------------------------------------------------- */
/** \name Summed Area Table
 * \{ */

namespace summed_area {

static void node_declare(NodeDeclarationBuilder &b)
{
  value_out(b, "Image", "Sum of the pixels below and left of each pixel, divided by the pixel count");
  value_in(b, "Image", 0.5f, "Values to sum");
}

class Operation : public FilterOperation {
 public:
  using FilterOperation::FilterOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float> image = this->values("Image", size);
    Array<double> sums(image.size());
    for (int y = 0; y < size.y; y++) {
      double row = 0.0;
      for (int x = 0; x < size.x; x++) {
        const int64_t i = int64_t(y) * size.x + x;
        row += double(image[i]);
        sums[i] = row + (y > 0 ? sums[i - size.x] : 0.0);
      }
    }
    const double count = double(size.x) * double(size.y);
    Array<float> out(image.size());
    for (const int64_t k : out.index_range()) {
      out[k] = float(sums[k] / count);
    }
    this->out_values("Image", domain, out);
  }
};

}  // namespace summed_area

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shadows
 * \{ */

namespace shadows {

static void node_declare(NodeDeclarationBuilder &b)
{
  value_out(b, "Shadows", "Black where the height is in shadow");
  value_in(b, "Height", 0.5f, "Height that casts the shadows");
  float_in(b, "Height Depth", 0.1f, 0.0f, 1.0f, "How tall the height is, next to the image width");
  angle_in(b, "Light Angle", float(M_PI) * 0.75f, "Direction the light comes from");
  angle_in(b, "Light Elevation", float(M_PI) * 0.15f, "How high the light is above the surface");
  float_in(b, "Softness", 2.0f, 0.0f, 32.0f, "Blurs the shadow, in pixels of a 256 image");
  factor_in(b, "Opacity", 1.0f, "How dark the shadow is");
}

class Operation : public FilterOperation {
 public:
  using FilterOperation::FilterOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float> height = this->values("Height", size);
    const float width = float(math::max(size.x, size.y));
    const float depth = this->f("Height Depth") * width;
    const float angle = this->f("Light Angle");
    const float rise = math::tan(math::clamp(this->f("Light Elevation"), 0.01f, 1.55f));
    const float2 dir(math::cos(angle), math::sin(angle));
    const float opacity = this->f("Opacity");
    /* The ray can not climb above the highest point, so it stops there. */
    const float reach = math::min(depth / math::max(rise, 1e-3f), width);
    Array<float> lit(height.size());
    each_pixel(size, [&](const int2 p, const int64_t index) {
      const float h0 = height[index] * depth;
      float shadow = 0.0f;
      for (float t = 1.0f; t <= reach; t += 1.0f) {
        const float h = sample_wrapped(height.as_span(), size, float2(p) + 0.5f + dir * t) * depth;
        if (h > h0 + t * rise) {
          shadow = 1.0f;
          break;
        }
      }
      lit[index] = shadow;
    });
    tg::blur_wrapped(lit.as_mutable_span(), size, tg::scaled_radius(this->f("Softness"), size));
    for (float &v : lit) {
      v = 1.0f - v * opacity;
    }
    this->out_values("Shadows", domain, lit);
  }
};

}  // namespace shadows

/** \} */

/* -------------------------------------------------------------------- */
/** \name Registration
 * \{ */

static void node_register()
{
  static bke::bNodeType types[15];
  int k = 0;
  register_filter(types[k++], "TextureGraphNodeAmbientOcclusion", "Ambient Occlusion",
                  "Darken the parts of a height that the sky can not reach",
                  "TEXTURE_GRAPH_AMBIENT_OCCLUSION", NODE_CLASS_OP_FILTER,
                  ambient_occlusion::node_declare,
                  make_operation<ambient_occlusion::Operation>);
  register_filter(types[k++], "TextureGraphNodeBevel", "Bevel",
                  "Slope the edges of shapes into a height and normal map",
                  "TEXTURE_GRAPH_BEVEL", NODE_CLASS_OP_FILTER, bevel::node_declare,
                  make_operation<bevel::Operation>);
  register_filter(types[k++], "TextureGraphNodeCrossSection", "Cross Section",
                  "Show the profile of one line through a height",
                  "TEXTURE_GRAPH_CROSS_SECTION", NODE_CLASS_OP_FILTER,
                  cross_section::node_declare, make_operation<cross_section::Operation>);
  register_filter(types[k++], "TextureGraphNodeCurvature", "Curvature",
                  "Find the bends of a normal map", "TEXTURE_GRAPH_CURVATURE",
                  NODE_CLASS_OP_FILTER, curvature::declare_plain,
                  make_operation<curvature::Operation<curvature::Kind::Plain>>);
  register_filter(types[k++], "TextureGraphNodeCurvatureSmooth", "Curvature Smooth",
                  "Find the bends of a normal map at several sizes",
                  "TEXTURE_GRAPH_CURVATURE_SMOOTH", NODE_CLASS_OP_FILTER,
                  curvature::declare_smooth,
                  make_operation<curvature::Operation<curvature::Kind::Smooth>>);
  register_filter(types[k++], "TextureGraphNodeCurvatureSobel", "Curvature Sobel",
                  "Find the bends of a normal map with a Sobel filter",
                  "TEXTURE_GRAPH_CURVATURE_SOBEL", NODE_CLASS_OP_FILTER,
                  curvature::declare_sobel,
                  make_operation<curvature::Operation<curvature::Kind::Sobel>>);
  register_filter(types[k++], "TextureGraphNodeDiffusionColor", "Diffusion Color",
                  "Spread the colors under a mask over the rest of the image",
                  "TEXTURE_GRAPH_DIFFUSION_COLOR", NODE_CLASS_OP_FILTER,
                  diffusion::declare_color,
                  make_operation<diffusion::Operation<diffusion::Kind::Color>>);
  register_filter(types[k++], "TextureGraphNodeDiffusionGrayscale", "Diffusion Grayscale",
                  "Spread the values under a mask over the rest of the image",
                  "TEXTURE_GRAPH_DIFFUSION_GRAYSCALE", NODE_CLASS_OP_FILTER,
                  diffusion::declare_grayscale,
                  make_operation<diffusion::Operation<diffusion::Kind::Grayscale>>);
  register_filter(types[k++], "TextureGraphNodeDiffusionUV", "Diffusion UV",
                  "Spread a UV map under a mask over the rest of the image, across its seams",
                  "TEXTURE_GRAPH_DIFFUSION_UV", NODE_CLASS_OP_FILTER, diffusion::declare_uv,
                  make_operation<diffusion::Operation<diffusion::Kind::UV>>);
  register_filter(types[k++], "TextureGraphNodeEmbossWithGloss", "Emboss With Gloss",
                  "Shade an image by a height, with a shine on the lit side",
                  "TEXTURE_GRAPH_EMBOSS_WITH_GLOSS", NODE_CLASS_OP_FILTER,
                  emboss::declare_gloss, make_operation<emboss::GlossOperation>);
  register_filter(types[k++], "TextureGraphNodeUberEmboss", "Uber Emboss",
                  "Light an image as a surface shaped by a height",
                  "TEXTURE_GRAPH_UBER_EMBOSS", NODE_CLASS_OP_FILTER, emboss::declare_uber,
                  make_operation<emboss::UberOperation>);
  register_filter(types[k++], "TextureGraphNodeMosaic", "Mosaic",
                  "Break an image into flat colored tiles", "TEXTURE_GRAPH_MOSAIC",
                  NODE_CLASS_OP_FILTER, mosaic::node_declare, make_operation<mosaic::Operation>);
  register_filter(types[k++], "TextureGraphNodeSummedAreaTable", "Summed Area Table",
                  "Sum the pixels below and left of each pixel",
                  "TEXTURE_GRAPH_SUMMED_AREA_TABLE", NODE_CLASS_OP_FILTER,
                  summed_area::node_declare, make_operation<summed_area::Operation>);
  register_filter(types[k++], "TextureGraphNodeShadows", "Shadows",
                  "Cast the shadows of a height lit from one side", "TEXTURE_GRAPH_SHADOWS",
                  NODE_CLASS_OP_FILTER, shadows::node_declare, make_operation<shadows::Operation>);
  BLI_assert(k <= 15);
}
NOD_REGISTER_NODE(node_register)

/** \} */

}  // namespace blender::nodes::node_texture_graph_effects_cc
