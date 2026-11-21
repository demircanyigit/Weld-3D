/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Small helpers shared by the Texture Graph nodes.
 */

#pragma once

#include <cmath>
#include <cstdint>

#include "BLI_array.hh"
#include "BLI_math_base.hh"
#include "BLI_span.hh"
#include "BLI_task.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"

#include "RNA_types.hh"

#include "COM_context.hh"
#include "COM_domain.hh"
#include "COM_result.hh"
#include "COM_utilities.hh"

namespace blender::nodes::texture_graph {

/* Sizes a generator can make its image at. Zero means the size set on the graph. */
inline const EnumPropertyItem generator_size_items[] = {
    {0, "GRAPH", 0, N_("Graph Size"), N_("Use the size set on the graph")},
    {256, "256", 0, N_("256"), N_("256 x 256 pixels")},
    {512, "512", 0, N_("512"), N_("512 x 512 pixels")},
    {1024, "1024", 0, N_("1024"), N_("1024 x 1024 pixels")},
    {2048, "2048", 0, N_("2048"), N_("2048 x 2048 pixels")},
    {4096, "4096", 0, N_("4096"), N_("4096 x 4096 pixels")},
    {0, nullptr, 0, nullptr, nullptr},
};

/* A domain of the given pixel size that covers the same area as the reference domain, so it gets
 * stretched to fit when it meets results of other sizes. */
inline compositor::Domain resized_domain(const compositor::Domain &reference, const int2 size)
{
  const float2 scale = float2(reference.data_size) / float2(math::max(size, int2(1)));
  return compositor::Domain(math::max(size, int2(1)),
                            reference.transformation * math::from_scale<float3x3>(scale));
}

/* The domain a generator writes into, given a value from #generator_size_items. */
inline compositor::Domain generator_domain(const compositor::Context &context, const int size)
{
  const compositor::Domain graph_domain = context.get_compositing_domain();
  if (size <= 0) {
    return graph_domain;
  }
  /* A draft run makes every image smaller by the same amount. */
  const int scaled = math::max(int(std::round(float(size) * context.draft_scale())), 1);
  return resized_domain(graph_domain, int2(scaled));
}

/* Wraps a texel around the edges of an image of the given size. */
inline int2 wrap_texel(const int2 texel, const int2 size)
{
  return int2(((texel.x % size.x) + size.x) % size.x, ((texel.y % size.y) + size.y) % size.y);
}

/* Shortest offset between two points on a tiling image of the given size. */
inline float2 wrapped_delta(float2 delta, const float2 size)
{
  delta.x -= size.x * math::round(delta.x / size.x);
  delta.y -= size.y * math::round(delta.y / size.y);
  return delta;
}

/* Slope of a grayscale image at a texel using a Sobel filter, wrapping at the edges. The result
 * is the change in value per pixel. */
inline float2 sobel_slope(const compositor::Result &image, const int2 texel, const int2 size)
{
  const auto load = [&](const int2 offset) {
    return image.load_pixel<float>(wrap_texel(texel + offset, size));
  };
  const float tl = load(int2(-1, 1));
  const float t = load(int2(0, 1));
  const float tr = load(int2(1, 1));
  const float l = load(int2(-1, 0));
  const float r = load(int2(1, 0));
  const float bl = load(int2(-1, -1));
  const float b = load(int2(0, -1));
  const float br = load(int2(1, -1));
  const float dx = (tr + 2.0f * r + br) - (tl + 2.0f * l + bl);
  const float dy = (tl + 2.0f * t + tr) - (bl + 2.0f * b + br);
  return float2(dx, dy) / 8.0f;
}

/* Bilinear sample of an RGBA pixel buffer at a position in pixels, zero outside of it. */
inline float4 sample_buffer_zero(const float4 *pixels, const int2 size, const float2 position)
{
  const float2 p = position - 0.5f;
  const int2 base = int2(math::floor(p));
  const float2 t = p - float2(base);
  const auto load = [&](const int2 texel) {
    if (texel.x < 0 || texel.y < 0 || texel.x >= size.x || texel.y >= size.y) {
      return float4(0.0f);
    }
    return pixels[int64_t(texel.y) * size.x + texel.x];
  };
  const float4 bottom = math::interpolate(load(base), load(base + int2(1, 0)), t.x);
  const float4 top = math::interpolate(load(base + int2(0, 1)), load(base + int2(1, 1)), t.x);
  return math::interpolate(bottom, top, t.y);
}

/* Draws a picture made in its own buffer onto a tiling image: the picture's anchor point lands at
 * the given pixel position, turned by the rotation. Copies one image size away are drawn too, so
 * a picture crossing an edge comes back on the other side. Returns the premultiplied color. */
inline float4 place_on_tile(const float4 *pixels,
                            const int2 picture_size,
                            const float2 anchor,
                            const float2 position,
                            const float rotation,
                            const int2 texel,
                            const int2 image_size)
{
  const float c = math::cos(-rotation);
  const float s = math::sin(-rotation);
  const float2 delta = float2(texel) + 0.5f - position;
  float4 result(0.0f);
  for (int y = -1; y <= 1; y++) {
    for (int x = -1; x <= 1; x++) {
      const float2 d = delta + float2(x, y) * float2(image_size);
      const float2 local = float2(d.x * c - d.y * s, d.x * s + d.y * c) + anchor;
      const float4 color = sample_buffer_zero(pixels, picture_size, local);
      /* Copies only overlap when the picture is bigger than the image, lay them over each
       * other. */
      result = color + result * (1.0f - color.w);
    }
  }
  return result;
}

/* -------------------------------------------------------------------- */
/** \name Colors
 * \{ */

/* Brightness of a linear color, with the Rec. 709 weights. */
inline float luminance(const float3 &color)
{
  return color.x * 0.2126f + color.y * 0.7152f + color.z * 0.0722f;
}

inline float3 rgb_to_hsv(const float3 &rgb)
{
  const float max = math::max(rgb.x, math::max(rgb.y, rgb.z));
  const float min = math::min(rgb.x, math::min(rgb.y, rgb.z));
  const float delta = max - min;
  float h = 0.0f;
  if (delta > 1e-8f) {
    if (max == rgb.x) {
      h = (rgb.y - rgb.z) / delta;
    }
    else if (max == rgb.y) {
      h = 2.0f + (rgb.z - rgb.x) / delta;
    }
    else {
      h = 4.0f + (rgb.x - rgb.y) / delta;
    }
    h /= 6.0f;
    h -= math::floor(h);
  }
  const float s = max > 1e-8f ? delta / max : 0.0f;
  return float3(h, s, max);
}

inline float3 hsv_to_rgb(const float3 &hsv)
{
  const float h = (hsv.x - math::floor(hsv.x)) * 6.0f;
  const float s = hsv.y;
  const float v = hsv.z;
  const float c = v * s;
  const float x = c * (1.0f - math::abs(std::fmod(h, 2.0f) - 1.0f));
  float3 rgb;
  if (h < 1.0f) {
    rgb = float3(c, x, 0.0f);
  }
  else if (h < 2.0f) {
    rgb = float3(x, c, 0.0f);
  }
  else if (h < 3.0f) {
    rgb = float3(0.0f, c, x);
  }
  else if (h < 4.0f) {
    rgb = float3(0.0f, x, c);
  }
  else if (h < 5.0f) {
    rgb = float3(x, 0.0f, c);
  }
  else {
    rgb = float3(c, 0.0f, x);
  }
  return rgb + (v - c);
}

/* Moves a color by the hue and saturation difference between two colors, and scales its
 * brightness by their ratio, so the source color turns into the target color. */
inline float3 shift_color(const float3 &color, const float3 &source, const float3 &target)
{
  const float3 hsv = rgb_to_hsv(color);
  const float3 source_hsv = rgb_to_hsv(source);
  const float3 target_hsv = rgb_to_hsv(target);
  float3 result = hsv;
  result.x = hsv.x + (target_hsv.x - source_hsv.x);
  /* Shift the saturation rather than scale it, so a gray source can still turn colorful. */
  result.y = math::clamp(hsv.y + (target_hsv.y - source_hsv.y), 0.0f, 1.0f);
  result.z = math::max(hsv.z * (target_hsv.z + 1e-4f) / (source_hsv.z + 1e-4f), 0.0f);
  return hsv_to_rgb(result);
}

/* Linear sRGB to CIE Lab (D65 white), L from 0 to 100. */
inline float3 rgb_to_lab(const float3 &rgb)
{
  const float3 xyz(rgb.x * 0.4124f + rgb.y * 0.3576f + rgb.z * 0.1805f,
                   rgb.x * 0.2126f + rgb.y * 0.7152f + rgb.z * 0.0722f,
                   rgb.x * 0.0193f + rgb.y * 0.1192f + rgb.z * 0.9505f);
  const float3 white(0.95047f, 1.0f, 1.08883f);
  const auto f = [](const float t) {
    return t > 0.008856f ? math::pow(math::max(t, 0.0f), 1.0f / 3.0f) : 7.787f * t + 16.0f / 116.0f;
  };
  const float fx = f(xyz.x / white.x);
  const float fy = f(xyz.y / white.y);
  const float fz = f(xyz.z / white.z);
  return float3(116.0f * fy - 16.0f, 500.0f * (fx - fy), 200.0f * (fy - fz));
}

/* Difference between two linear colors in Lab, scaled so that black to white is about 1. The
 * weights scale the lightness, chroma and hue parts. */
inline float lab_distance(const float3 &a, const float3 &b, const float3 &weights = float3(1.0f))
{
  const float3 la = rgb_to_lab(a);
  const float3 lb = rgb_to_lab(b);
  const float ca = math::length(float2(la.y, la.z));
  const float cb = math::length(float2(lb.y, lb.z));
  const float dl = la.x - lb.x;
  const float dc = ca - cb;
  const float da = la.y - lb.y;
  const float db = la.z - lb.z;
  const float dh = math::sqrt(math::max(da * da + db * db - dc * dc, 0.0f));
  return math::sqrt(math::square(dl * weights.x) + math::square(dc * weights.y) +
                    math::square(dh * weights.z)) /
         100.0f;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Whole images
 * \{ */

/* Copies an image result into a plain buffer of colors, grayscale values fill RGB. */
inline Array<float4> read_colors(const compositor::Result &result, const int2 size)
{
  Array<float4> pixels(int64_t(size.x) * size.y);
  const bool is_float = result.type() == compositor::ResultType::Float;
  threading::parallel_for(IndexRange(size.y), 16, [&](const IndexRange rows) {
    for (const int y : rows) {
      for (int x = 0; x < size.x; x++) {
        float4 value;
        if (result.is_single_value()) {
          value = is_float ? float4(float3(result.get_single_value<float>()), 1.0f) :
                             float4(result.get_single_value<compositor::Color>());
        }
        else if (is_float) {
          value = float4(float3(result.load_pixel<float>(int2(x, y))), 1.0f);
        }
        else {
          value = float4(result.load_pixel<compositor::Color>(int2(x, y)));
        }
        pixels[int64_t(y) * size.x + x] = value;
      }
    }
  });
  return pixels;
}

inline Array<float> read_values(const compositor::Result &result, const int2 size)
{
  Array<float> values(int64_t(size.x) * size.y);
  threading::parallel_for(IndexRange(size.y), 16, [&](const IndexRange rows) {
    for (const int y : rows) {
      for (int x = 0; x < size.x; x++) {
        values[int64_t(y) * size.x + x] = result.is_single_value() ?
                                              result.get_single_value<float>() :
                                              result.load_pixel<float>(int2(x, y));
      }
    }
  });
  return values;
}

/* Blur radius in pixels for a radius given in pixels of a 256 wide image, so a node looks the
 * same at any resolution. */
inline float scaled_radius(const float radius, const int2 size)
{
  return radius * float(math::max(size.x, size.y)) / 256.0f;
}

/* One box blur pass along one axis, wrapping at the edges. */
template<typename T>
inline void box_blur_axis(MutableSpan<T> data, const int2 size, const int radius, const int axis)
{
  const int length = size[axis];
  const int lines = size[1 - axis];
  const int r = math::min(radius, (length - 1) / 2);
  if (r <= 0) {
    return;
  }
  const float scale = 1.0f / float(2 * r + 1);
  threading::parallel_for(IndexRange(lines), 8, [&](const IndexRange range) {
    Array<T> line(length);
    for (const int l : range) {
      const auto index = [&](const int i) {
        const int w = ((i % length) + length) % length;
        return axis == 0 ? int64_t(l) * size.x + w : int64_t(w) * size.x + l;
      };
      for (int i = 0; i < length; i++) {
        line[i] = data[index(i)];
      }
      T sum = T(0);
      for (int i = -r; i <= r; i++) {
        sum += line[((i % length) + length) % length];
      }
      for (int i = 0; i < length; i++) {
        data[index(i)] = sum * scale;
        sum += line[(i + r + 1) % length] - line[((i - r) % length + length) % length];
      }
    }
  });
}

/* Smooth blur of about the given radius in pixels (three box passes per axis), wrapping at the
 * edges so tiling images stay tiling. */
template<typename T>
inline void blur_wrapped(MutableSpan<T> data, const int2 size, const float radius)
{
  const int box = int(math::round(radius * 0.5f));
  if (box <= 0) {
    return;
  }
  for (int pass = 0; pass < 3; pass++) {
    box_blur_axis(data, size, box, 0);
    box_blur_axis(data, size, box, 1);
  }
}

/* Bilinear sample of a buffer at a position in pixels (pixel centers at +0.5), wrapping around
 * the edges so tiling images stay tiling. */
template<typename T>
inline T sample_wrapped(const Span<T> data, const int2 size, const float2 position)
{
  const float2 p = position - 0.5f;
  const float2 base_f = math::floor(p);
  const float2 t = p - base_f;
  const int2 base = int2(base_f);
  const auto load = [&](const int2 texel) {
    const int2 w = wrap_texel(texel, size);
    return data[int64_t(w.y) * size.x + w.x];
  };
  const T bottom = math::interpolate(load(base), load(base + int2(1, 0)), t.x);
  const T top = math::interpolate(load(base + int2(0, 1)), load(base + int2(1, 1)), t.x);
  return math::interpolate(bottom, top, t.y);
}

/* Slope of a buffer of values per pixel, with a Sobel filter that wraps at the edges. */
inline Array<float2> slope_field(const Span<float> values, const int2 size)
{
  Array<float2> slopes(values.size());
  threading::parallel_for(IndexRange(size.y), 16, [&](const IndexRange rows) {
    for (const int y : rows) {
      for (int x = 0; x < size.x; x++) {
        const auto v = [&](const int dx, const int dy) {
          const int2 w = wrap_texel(int2(x + dx, y + dy), size);
          return values[int64_t(w.y) * size.x + w.x];
        };
        const float dx = (v(1, 1) + 2.0f * v(1, 0) + v(1, -1)) - (v(-1, 1) + 2.0f * v(-1, 0) + v(-1, -1));
        const float dy = (v(-1, 1) + 2.0f * v(0, 1) + v(1, 1)) - (v(-1, -1) + 2.0f * v(0, -1) + v(1, -1));
        slopes[int64_t(y) * size.x + x] = float2(dx, dy) / 8.0f;
      }
    }
  });
  return slopes;
}

/* Writes a buffer of colors or values into a result allocated on the given domain. */
inline void write_colors(compositor::Result &result, const Span<float4> pixels, const int2 size)
{
  compositor::parallel_for(size, [&](const int2 texel) {
    result.store_pixel(texel, compositor::Color(pixels[int64_t(texel.y) * size.x + texel.x]));
  });
}

inline void write_values(compositor::Result &result, const Span<float> values, const int2 size)
{
  compositor::parallel_for(size, [&](const int2 texel) {
    result.store_pixel(texel, values[int64_t(texel.y) * size.x + texel.x]);
  });
}

/* Counts how many values fall in each of `bins` equal steps between 0 and 1, values outside
 * are clamped. */
inline Array<float> histogram(const Span<float> values, const int bins)
{
  Array<float> counts(bins, 0.0f);
  for (const float v : values) {
    const int bin = math::clamp(int(math::clamp(v, 0.0f, 1.0f) * float(bins)), 0, bins - 1);
    counts[bin] += 1.0f;
  }
  return counts;
}

/* Values of the ID maps made by the palette nodes: index N is stored as N / 255. */
inline int id_from_value(const float value)
{
  return math::clamp(int(math::round(value * 255.0f)), 0, 255);
}

inline float id_to_value(const int id)
{
  return float(id) / 255.0f;
}

/** \} */

/* Fast integer hash, good enough for noise. */
inline uint32_t hash(uint32_t x)
{
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

inline uint32_t hash(const int x, const int y, const uint32_t seed)
{
  return hash(uint32_t(x) ^ hash(uint32_t(y) ^ hash(seed)));
}

/* Random float in [0, 1). */
inline float hash_to_float(const uint32_t h)
{
  return float(h >> 8) * (1.0f / 16777216.0f);
}

}  // namespace blender::nodes::texture_graph
