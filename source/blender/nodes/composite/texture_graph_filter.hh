/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Shared parts of the Texture Graph filter nodes: socket declarations, a base operation with short
 * input getters, whole image buffers, normal map encoding and nearest-pixel search.
 */

#pragma once

#include "BLI_array.hh"
#include "BLI_math_vector.hh"
#include "BLI_task.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"
#include "texture_graph_util.hh"

namespace blender::nodes::texture_graph::filter {

using namespace blender::compositor;
/* The shared helpers above this header, for the files using this namespace. */
using namespace blender::nodes::texture_graph;

/* -------------------------------------------------------------------- */
/** \name Declarations
 * \{ */

inline const EnumPropertyItem normal_format_items[] = {
    {0, "OPENGL", 0, N_("OpenGL"), N_("Green points up")},
    {1, "DIRECTX", 0, N_("DirectX"), N_("Green points down")},
    {0, nullptr, 0, nullptr, nullptr},
};

inline void color_in(NodeDeclarationBuilder &b,
                     const char *name,
                     const float4 d = float4(0.5f, 0.5f, 0.5f, 1.0f),
                     const char *description = "")
{
  b.add_input<decl::Color>(name)
      .default_value({d.x, d.y, d.z, d.w})
      .structure_type(StructureType::Dynamic)
      .description(description);
}

/* A normal map input, flat by default. */
inline void normal_in(NodeDeclarationBuilder &b,
                      const char *name = "Normal",
                      const char *description = "Normal map")
{
  color_in(b, name, float4(0.5f, 0.5f, 1.0f, 1.0f), description);
}

inline void value_in(NodeDeclarationBuilder &b,
                     const char *name,
                     const float d = 0.5f,
                     const char *description = "")
{
  b.add_input<decl::Float>(name)
      .default_value(d)
      .min(0.0f)
      .max(1.0f)
      .structure_type(StructureType::Dynamic)
      .description(description);
}

inline void factor_in(NodeDeclarationBuilder &b,
                      const char *name,
                      const float d,
                      const char *description = "")
{
  b.add_input<decl::Float>(name).default_value(d).min(0.0f).max(1.0f).subtype(PROP_FACTOR).description(
      description);
}

inline void float_in(NodeDeclarationBuilder &b,
                     const char *name,
                     const float d,
                     const float min,
                     const float max,
                     const char *description = "")
{
  b.add_input<decl::Float>(name).default_value(d).min(min).max(max).description(description);
}

inline void angle_in(NodeDeclarationBuilder &b,
                     const char *name,
                     const float d,
                     const char *description = "")
{
  b.add_input<decl::Float>(name).default_value(d).subtype(PROP_ANGLE).description(description);
}

inline void int_in(NodeDeclarationBuilder &b,
                   const char *name,
                   const int d,
                   const int min,
                   const int max,
                   const char *description = "")
{
  b.add_input<decl::Int>(name).default_value(d).min(min).max(max).description(description);
}

inline void bool_in(NodeDeclarationBuilder &b,
                    const char *name,
                    const bool d,
                    const char *description = "")
{
  b.add_input<decl::Bool>(name).default_value(d).description(description);
}

inline void vector2_in(NodeDeclarationBuilder &b,
                       const char *name,
                       const float2 d,
                       const float min,
                       const float max,
                       const char *description = "")
{
  b.add_input<decl::Vector>(name)
      .dimensions(2)
      .default_value({d.x, d.y})
      .min(min)
      .max(max)
      .description(description);
}

inline void menu_in(NodeDeclarationBuilder &b,
                    const char *name,
                    const EnumPropertyItem *items,
                    const int d = 0,
                    const char *description = "")
{
  b.add_input<decl::Menu>(name).default_value(MenuValue(d)).static_items(items).description(
      description);
}

inline void normal_format_in(NodeDeclarationBuilder &b)
{
  menu_in(b, "Normal Format", normal_format_items, 0, "Which way green points in the normal map");
}

inline void seed_in(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Int>("Seed").default_value(0).description("Changes the random values");
}

inline void color_out(NodeDeclarationBuilder &b, const char *name, const char *description = "")
{
  b.add_output<decl::Color>(name).structure_type(StructureType::Dynamic).description(description);
}

inline void value_out(NodeDeclarationBuilder &b, const char *name, const char *description = "")
{
  b.add_output<decl::Float>(name).structure_type(StructureType::Dynamic).description(description);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Normal maps
 * \{ */

/* A normal map color to a unit vector, green up. */
inline float3 decode_normal(const float4 color, const bool directx)
{
  float3 n = float3(color.x, color.y, color.z) * 2.0f - 1.0f;
  if (directx) {
    n.y = -n.y;
  }
  const float length = math::length(n);
  return length > 1e-6f ? n / length : float3(0.0f, 0.0f, 1.0f);
}

inline float4 encode_normal(float3 n, const bool directx)
{
  const float length = math::length(n);
  n = length > 1e-6f ? n / length : float3(0.0f, 0.0f, 1.0f);
  if (directx) {
    n.y = -n.y;
  }
  return float4(n * 0.5f + 0.5f, 1.0f);
}

/* Normal of a height buffer, `scale` is how many pixels one unit of height rises. */
inline float3 height_normal(const Span<float> height, const int2 size, const int2 p, const float scale)
{
  const auto v = [&](const int dx, const int dy) {
    const int2 w = wrap_texel(p + int2(dx, dy), size);
    return height[int64_t(w.y) * size.x + w.x];
  };
  const float dx = ((v(1, 1) + 2.0f * v(1, 0) + v(1, -1)) - (v(-1, 1) + 2.0f * v(-1, 0) + v(-1, -1))) /
                   8.0f;
  const float dy = ((v(-1, 1) + 2.0f * v(0, 1) + v(1, 1)) - (v(-1, -1) + 2.0f * v(0, -1) + v(1, -1))) /
                   8.0f;
  return math::normalize(float3(-dx * scale, -dy * scale, 1.0f));
}

/* Rotation taking straight up to `n`, applied to `v` (used to lay one normal map on another). */
inline float3 rotate_from_up(const float3 n, const float3 v)
{
  if (n.z < -0.9999f) {
    return float3(-v.x, v.y, -v.z);
  }
  const float a = 1.0f / (1.0f + n.z);
  const float b = -n.x * n.y * a;
  const float3 t(1.0f - n.x * n.x * a, b, -n.x);
  const float3 bt(b, 1.0f - n.y * n.y * a, -n.y);
  return t * v.x + bt * v.y + n * v.z;
}

/* The inverse of rotate_from_up. */
inline float3 rotate_to_up(const float3 n, const float3 v)
{
  if (n.z < -0.9999f) {
    return float3(-v.x, v.y, -v.z);
  }
  const float a = 1.0f / (1.0f + n.z);
  const float b = -n.x * n.y * a;
  const float3 t(1.0f - n.x * n.x * a, b, -n.x);
  const float3 bt(b, 1.0f - n.y * n.y * a, -n.y);
  return float3(math::dot(t, v), math::dot(bt, v), math::dot(n, v));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Nearest pixels
 * \{ */

/* For every pixel, the nearest pixel that is set in `seeds` (jump flooding), or -1 when there
 * are none. With `wrap` the distance is measured across the edges too. */
inline Array<int2> nearest_seeds(const Span<bool> seeds, const int2 size, const bool wrap)
{
  const int64_t count = int64_t(size.x) * size.y;
  Array<int2> nearest(count, int2(-1));
  for (int64_t i = 0; i < count; i++) {
    if (seeds[i]) {
      nearest[i] = int2(int(i % size.x), int(i / size.x));
    }
  }
  const auto distance2 = [&](const int2 a, const int2 b) {
    float2 d = float2(b - a);
    if (wrap) {
      d = wrapped_delta(d, float2(size));
    }
    return math::length_squared(d);
  };
  Array<int2> next(count);
  int step = 1;
  while (step < math::max(size.x, size.y)) {
    step *= 2;
  }
  /* A few extra passes at step 1 fix most of the rare jump flooding misses. */
  Vector<int> steps;
  for (int s = step / 2; s >= 1; s /= 2) {
    steps.append(s);
  }
  steps.append(2);
  steps.append(1);
  for (const int s : steps) {
    threading::parallel_for(IndexRange(size.y), 16, [&](const IndexRange rows) {
      for (const int y : rows) {
        for (int x = 0; x < size.x; x++) {
          const int2 p(x, y);
          int2 best = nearest[int64_t(y) * size.x + x];
          float best_d = best.x >= 0 ? distance2(p, best) : 1e30f;
          for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
              int2 q = p + int2(dx, dy) * s;
              if (wrap) {
                q = wrap_texel(q, size);
              }
              else if (q.x < 0 || q.y < 0 || q.x >= size.x || q.y >= size.y) {
                continue;
              }
              const int2 candidate = nearest[int64_t(q.y) * size.x + q.x];
              if (candidate.x < 0) {
                continue;
              }
              const float d = distance2(p, candidate);
              if (d < best_d) {
                best_d = d;
                best = candidate;
              }
            }
          }
          next[int64_t(y) * size.x + x] = best;
        }
      }
    });
    std::swap(nearest, next);
  }
  return nearest;
}

/* Distance in pixels from every pixel to the nearest seed, a large value when there are none. */
inline Array<float> seed_distance(const Span<bool> seeds, const int2 size, const bool wrap)
{
  const Array<int2> nearest = nearest_seeds(seeds, size, wrap);
  Array<float> distance(nearest.size());
  threading::parallel_for(nearest.index_range(), 4096, [&](const IndexRange range) {
    for (const int64_t i : range) {
      const int2 n = nearest[i];
      if (n.x < 0) {
        distance[i] = 1e9f;
        continue;
      }
      float2 d = float2(n) - float2(int(i % size.x), int(i / size.x));
      if (wrap) {
        d = wrapped_delta(d, float2(size));
      }
      distance[i] = math::length(d);
    }
  });
  return distance;
}

/* Signed distance to the 0.5 edge of a mask: below zero inside, above zero outside. */
inline Array<float> signed_distance(const Span<float> mask, const int2 size, const bool wrap)
{
  Array<bool> inside(mask.size());
  Array<bool> outside(mask.size());
  for (const int64_t i : mask.index_range()) {
    inside[i] = mask[i] >= 0.5f;
    outside[i] = !inside[i];
  }
  const Array<float> to_inside = seed_distance(inside, size, wrap);
  const Array<float> to_outside = seed_distance(outside, size, wrap);
  Array<float> result(mask.size());
  for (const int64_t i : mask.index_range()) {
    result[i] = inside[i] ? -(to_outside[i] - 0.5f) : to_inside[i] - 0.5f;
  }
  return result;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Horizons
 * \{ */

/* Horizon search over a height buffer: for each pixel, how far up the sky is blocked in each of
 * `directions` directions. `fn(direction, sin_of_horizon)` is called per direction. */
template<typename Fn>
inline void horizons(const Span<float> height,
                     const int2 size,
                     const int2 p,
                     const float depth_pixels,
                     const float radius_pixels,
                     const int directions,
                     const Fn &fn)
{
  const float h0 = height[int64_t(p.y) * size.x + p.x] * depth_pixels;
  const int steps = math::clamp(int(radius_pixels), 4, 24);
  for (int d = 0; d < directions; d++) {
    const float angle = (float(d) + 0.5f) / float(directions) * float(M_PI * 2.0);
    const float2 dir(math::cos(angle), math::sin(angle));
    float best = 0.0f;
    for (int s = 1; s <= steps; s++) {
      const float dist = radius_pixels * float(s) / float(steps);
      const float2 q = float2(p) + 0.5f + dir * dist;
      const float h = sample_wrapped(height, size, q) * depth_pixels;
      const float rise = h - h0;
      if (rise > 0.0f) {
        const float sin_h = rise / math::sqrt(rise * rise + dist * dist);
        /* Far occluders count less. */
        best = math::max(best, sin_h * (1.0f - float(s - 1) / float(steps)));
      }
    }
    fn(dir, best);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operation
 * \{ */

inline float smooth01(const float e0, const float e1, const float x)
{
  const float t = math::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

class FilterOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

 protected:
  float f(const char *name)
  {
    return this->get_input(name).get_single_value_default<float>();
  }
  int i(const char *name)
  {
    return this->get_input(name).get_single_value_default<int>();
  }
  bool b(const char *name)
  {
    return this->get_input(name).get_single_value_default<bool>();
  }
  int m(const char *name)
  {
    return this->get_input(name).get_single_value_default<MenuValue>().value;
  }
  float2 v(const char *name)
  {
    return this->get_input(name).get_single_value_default<float2>();
  }
  float4 c(const char *name)
  {
    return float4(this->get_input(name).get_single_value_default<Color>());
  }
  uint32_t seed()
  {
    return uint32_t(this->i("Seed"));
  }
  bool directx()
  {
    return this->m("Normal Format") == 1;
  }

  /* The domain of the node: the one of its image inputs, or one pixel when they are all single
   * values. */
  int2 begin(Domain &r_domain)
  {
    r_domain = this->compute_domain();
    return r_domain.data_size;
  }

  Array<float4> colors(const char *name, const int2 size)
  {
    return read_colors(this->get_input(name), size);
  }
  Array<float> values(const char *name, const int2 size)
  {
    const Result &input = this->get_input(name);
    if (input.type() == ResultType::Float) {
      return read_values(input, size);
    }
    const Array<float4> c = read_colors(input, size);
    Array<float> v(c.size());
    for (const int64_t k : c.index_range()) {
      v[k] = luminance(c[k].xyz());
    }
    return v;
  }
  bool is_image(const char *name)
  {
    return !this->get_input(name).is_single_value();
  }

  void out_colors(const char *name, const Domain &domain, const Span<float4> pixels)
  {
    Result &result = this->get_result(name);
    if (!result.should_compute()) {
      return;
    }
    result.allocate_texture(domain);
    write_colors(result, pixels, domain.data_size);
  }
  void out_values(const char *name, const Domain &domain, const Span<float> pixels)
  {
    Result &result = this->get_result(name);
    if (!result.should_compute()) {
      return;
    }
    result.allocate_texture(domain);
    write_values(result, pixels, domain.data_size);
  }
  bool wants(const char *name)
  {
    return this->get_result(name).should_compute();
  }
};

/* Runs fn(pixel, index) for every pixel. */
template<typename Fn> inline void each_pixel(const int2 size, const Fn &fn)
{
  threading::parallel_for(IndexRange(size.y), 8, [&](const IndexRange rows) {
    for (const int y : rows) {
      for (int x = 0; x < size.x; x++) {
        fn(int2(x, y), int64_t(y) * size.x + x);
      }
    }
  });
}

/* Position of a pixel center in 0 to 1. */
inline float2 uv_of(const int2 p, const int2 size)
{
  return (float2(p) + 0.5f) / float2(size);
}

/* Wrapped bilinear sample at a position in 0 to 1. */
template<typename T> inline T sample_uv(const Span<T> data, const int2 size, const float2 uv)
{
  return sample_wrapped(data, size, uv * float2(size));
}

/* Bilinear sample at a position in 0 to 1, clamped to the edges. */
template<typename T> inline T sample_uv_clamped(const Span<T> data, const int2 size, const float2 uv)
{
  const float2 p = math::clamp(uv * float2(size), float2(0.5f), float2(size) - 0.5f);
  return sample_wrapped(data, size, p);
}

inline float2 rotate2(const float2 v, const float angle)
{
  const float c = math::cos(angle);
  const float s = math::sin(angle);
  return float2(v.x * c - v.y * s, v.x * s + v.y * c);
}

template<typename T> static NodeOperation *make_operation(Context &context, const bNode &node)
{
  return new T(context, node);
}

inline void register_filter(bke::bNodeType &ntype,
                            const char *idname,
                            const char *name,
                            const char *description,
                            const char *enum_name,
                            const short nclass,
                            bke::NodeDeclareFunction declare,
                            bke::NodeGetCompositorOperationFunction operation)
{
  tg_node_type_base(&ntype, idname);
  ntype.ui_name = name;
  ntype.ui_description = description;
  ntype.enum_name_legacy = enum_name;
  ntype.nclass = nclass;
  ntype.declare = declare;
  ntype.flag |= NODE_PREVIEW;
  ntype.get_compositor_operation = operation;
  bke::node_register_type(ntype);
}

/** \} */

}  // namespace blender::nodes::texture_graph::filter
