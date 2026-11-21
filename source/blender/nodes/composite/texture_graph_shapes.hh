/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Shapes and stamping for the Texture Graph pattern nodes.
 *
 * The shape library draws one of a set of built-in shapes in a square from -1 to 1. The stamper
 * draws many rotated, scaled and mirrored copies of shapes or images over a tiling image: the
 * copies are sorted into a grid of bins first, so every pixel only looks at the copies near it.
 */

#pragma once

#include <cmath>
#include <cstdint>
#include <utility>

#include "BLI_array.hh"
#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "RNA_types.hh"

#include "texture_graph_util.hh"

namespace blender::nodes::texture_graph::shapes {

/* -------------------------------------------------------------------- */
/** \name Shape library
 * \{ */

enum class Shape : int {
  /* Not a shape: the copies show an image connected to the node. */
  Image = 0,
  Square = 1,
  Disc = 2,
  Paraboloid = 3,
  Bell = 4,
  Gaussian = 5,
  Thorn = 6,
  Pyramid = 7,
  Brick = 8,
  Gradation = 9,
  Waves = 10,
  HalfBell = 11,
  RidgedBell = 12,
  Crescent = 13,
  Capsule = 14,
  Cone = 15,
  Hemisphere = 16,
};

/* The shape menu, with the image entry first for nodes that take images. */
inline const EnumPropertyItem shape_items[] = {
    {int(Shape::Image), "IMAGE", 0, N_("Image"), N_("The image connected to the node")},
    {int(Shape::Square), "SQUARE", 0, N_("Square"), N_("A flat square")},
    {int(Shape::Disc), "DISC", 0, N_("Disc"), N_("A flat disc")},
    {int(Shape::Paraboloid), "PARABOLOID", 0, N_("Paraboloid"), N_("A rounded dome")},
    {int(Shape::Bell), "BELL", 0, N_("Bell"), N_("A smooth bump")},
    {int(Shape::Gaussian), "GAUSSIAN", 0, N_("Gaussian"), N_("A soft blob")},
    {int(Shape::Thorn), "THORN", 0, N_("Thorn"), N_("A sharp spike")},
    {int(Shape::Pyramid), "PYRAMID", 0, N_("Pyramid"), N_("Four slopes meeting at a point")},
    {int(Shape::Brick), "BRICK", 0, N_("Brick"), N_("A bevelled brick, twice as wide as high")},
    {int(Shape::Gradation), "GRADATION", 0, N_("Gradation"), N_("A ramp from left to right")},
    {int(Shape::Waves), "WAVES", 0, N_("Waves"), N_("Soft parallel waves")},
    {int(Shape::HalfBell), "HALF_BELL", 0, N_("Half Bell"), N_("A bump cut in half")},
    {int(Shape::RidgedBell), "RIDGED_BELL", 0, N_("Ridged Bell"), N_("A bump with rings")},
    {int(Shape::Crescent), "CRESCENT", 0, N_("Crescent"), N_("A disc with a bite taken out")},
    {int(Shape::Capsule), "CAPSULE", 0, N_("Capsule"), N_("A rounded bar")},
    {int(Shape::Cone), "CONE", 0, N_("Cone"), N_("Straight slopes up to a point")},
    {int(Shape::Hemisphere), "HEMISPHERE", 0, N_("Hemisphere"), N_("Half a ball")},
    {0, nullptr, 0, nullptr, nullptr},
};

/* The same menu without the image entry, for nodes that only draw built-in shapes. */
inline const EnumPropertyItem *builtin_shape_items = shape_items + 1;

inline float smooth01(const float a, const float b, const float x)
{
  const float t = math::clamp((x - a) / (b - a), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

/* A hard edge at `edge` softened by `softness`, one inside and zero outside. */
inline float soft_edge(const float distance, const float edge, const float softness)
{
  if (softness <= 1e-4f) {
    return distance <= edge ? 1.0f : 0.0f;
  }
  return math::clamp((edge - distance) / softness, 0.0f, 1.0f);
}

inline float bell(const float r)
{
  return r >= 1.0f ? 0.0f : 0.5f + 0.5f * math::cos(r * float(M_PI));
}

/* A shape drawn in the square from -1 to 1, zero outside it. `specific` changes each shape a
 * little: softness, sharpness or how far the bite goes. */
inline float shape_value(const Shape shape, const float2 p, const float specific)
{
  const float ax = math::abs(p.x);
  const float ay = math::abs(p.y);
  if (ax > 1.0f || ay > 1.0f) {
    return 0.0f;
  }
  const float r = math::length(p);
  const float box = math::max(ax, ay);
  switch (shape) {
    case Shape::Image:
    case Shape::Square:
      return soft_edge(box, 1.0f, specific * 0.5f);
    case Shape::Disc:
      return soft_edge(r, 1.0f, specific * 0.5f);
    case Shape::Paraboloid:
      return std::pow(math::max(0.0f, 1.0f - r * r), 1.0f + specific * 3.0f);
    case Shape::Bell:
      return bell(r / (1.0f - specific * 0.5f));
    case Shape::Gaussian: {
      const float g = std::exp(-r * r * (4.0f + specific * 12.0f));
      return g * smooth01(1.0f, 0.85f, r);
    }
    case Shape::Thorn:
      return std::pow(math::max(0.0f, 1.0f - r), 2.0f + specific * 6.0f);
    case Shape::Pyramid:
      return math::clamp((1.0f - box) / math::max(1.0f - specific * 0.95f, 1e-3f), 0.0f, 1.0f);
    case Shape::Brick: {
      const float bevel = 0.05f + specific * 0.4f;
      const float bx = math::clamp((1.0f - ax) / bevel, 0.0f, 1.0f);
      const float by = math::clamp((0.5f - ay) / (bevel * 0.5f), 0.0f, 1.0f);
      return math::min(bx, by);
    }
    case Shape::Gradation:
      return std::pow((p.x + 1.0f) * 0.5f, 1.0f + specific * 3.0f);
    case Shape::Waves:
      return 0.5f + 0.5f * math::cos(p.y * float(M_PI) * (2.0f + std::floor(specific * 8.0f)));
    case Shape::HalfBell: {
      const float2 q(p.x, (p.y + 1.0f) * 0.5f);
      return bell(math::length(q) / (1.0f - specific * 0.5f));
    }
    case Shape::RidgedBell: {
      const float rings = 0.5f + 0.5f * math::cos(r * float(M_PI) * (4.0f + specific * 8.0f));
      return bell(r) * (0.55f + 0.45f * rings);
    }
    case Shape::Crescent: {
      const float bite = math::length(p - float2(0.35f + specific * 0.5f, 0.0f)) / 0.85f;
      return soft_edge(r, 1.0f, 0.04f) * (1.0f - soft_edge(bite, 1.0f, 0.04f));
    }
    case Shape::Capsule: {
      const float t = math::clamp(p.x, -0.5f, 0.5f);
      const float d = math::length(float2(p.x - t, p.y)) / 0.5f;
      const float dome = std::sqrt(math::max(0.0f, 1.0f - d * d));
      return math::interpolate(soft_edge(d, 1.0f, 0.05f), dome, 1.0f - specific);
    }
    case Shape::Cone:
      return math::max(0.0f, 1.0f - r / (1.0f - specific * 0.5f));
    case Shape::Hemisphere: {
      const float s = r / (1.0f - specific * 0.5f);
      return std::sqrt(math::max(0.0f, 1.0f - s * s));
    }
  }
  return 0.0f;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Images used as shapes
 * \{ */

/* A grayscale image read into a plain buffer, used as a shape or a map. */
struct Picture {
  Array<float> values;
  int2 size = int2(0);
  /* The value everywhere when nothing is connected. */
  float single = 1.0f;

  bool is_image() const
  {
    return size.x > 0;
  }

  /* Bilinear sample, uv from 0 to 1 over the image, black outside. */
  float sample_clamped(const float2 uv) const
  {
    if (!this->is_image()) {
      return single;
    }
    if (uv.x < 0.0f || uv.y < 0.0f || uv.x > 1.0f || uv.y > 1.0f) {
      return 0.0f;
    }
    const float2 p = uv * float2(size) - 0.5f;
    const int2 i = int2(math::floor(p));
    const float2 f = p - float2(i);
    const auto at = [&](const int2 t) {
      const int2 c = math::clamp(t, int2(0), size - 1);
      return values[int64_t(c.y) * size.x + c.x];
    };
    return math::interpolate(math::interpolate(at(i), at(i + int2(1, 0)), f.x),
                             math::interpolate(at(i + int2(0, 1)), at(i + int2(1, 1)), f.x),
                             f.y);
  }

  /* Bilinear sample that wraps around, for maps laid over the whole image. */
  float sample_wrapped(const float2 uv) const
  {
    if (!this->is_image()) {
      return single;
    }
    return texture_graph::sample_wrapped(values.as_span(), size, uv * float2(size));
  }
};

/* Reads a grayscale input at its own size. */
inline Picture read_picture(const compositor::Result &result)
{
  Picture picture;
  if (result.is_single_value()) {
    picture.single = result.get_single_value<float>();
    return picture;
  }
  picture.size = result.domain().data_size;
  picture.values = read_values(result, picture.size);
  return picture;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Stamper
 * \{ */

enum class Combine : int {
  /* The brightest copy wins. */
  Max = 0,
  /* Copies add up. */
  Add = 1,
  /* Later copies cover earlier ones. */
  Over = 2,
};

struct Copy {
  /* Center in UV space, 0 to 1 over the image. */
  float2 center = float2(0.5f);
  /* Half the width and height, in UV space, before turning. */
  float2 half_size = float2(0.1f);
  /* Turn in radians, counterclockwise. */
  float angle = 0.0f;
  bool flip_x = false;
  bool flip_y = false;
  /* Brightness the shape is multiplied by. */
  float value = 1.0f;
  /* Which shape or image this copy draws. */
  Shape shape = Shape::Square;
  int image = 0;
  float specific = 0.0f;
  /* Added on top where the copy is drawn, for copies that sit on a height. */
  float offset = 0.0f;
  /* Random number of the copy, for nodes that color copies later. */
  float random = 0.0f;
  /* How the copy fades along its length, for strokes: 0 no fade, otherwise along local x. */
  float fade_start = 0.0f;
  float fade_end = 0.0f;
};

/* What a pixel got from the copies. */
struct Hit {
  float value = 0.0f;
  /* The copy on top, -1 when none covers the pixel. */
  int copy = -1;
  /* Where the pixel is inside the copy on top, -1 to 1. */
  float2 local = float2(0.0f);
};

class Stamper {
 private:
  Span<Copy> copies_;
  Span<Picture> images_;
  int bins_ = 1;
  /* For every bin, the copies that reach into it and the whole-image shift to draw them with. */
  Array<Vector<std::pair<int, float2>>> bin_items_;

 public:
  Stamper(const Span<Copy> copies, const Span<Picture> images) : copies_(copies), images_(images)
  {
    bins_ = math::clamp(int(std::sqrt(float(copies.size()))), 1, 256);
    bin_items_ = Array<Vector<std::pair<int, float2>>>(int64_t(bins_) * bins_);
    for (const int i : copies.index_range()) {
      const Copy &copy = copies[i];
      const float radius = math::length(copy.half_size);
      if (radius <= 0.0f || copy.value == 0.0f) {
        continue;
      }
      const int2 lo = int2(math::floor((copy.center - radius) * float(bins_)));
      const int2 hi = int2(math::floor((copy.center + radius) * float(bins_)));
      /* Copies bigger than the image would be added to every bin many times, cap the range. */
      const int2 hi_capped = math::min(hi, lo + bins_ * 2);
      for (int by = lo.y; by <= hi_capped.y; by++) {
        for (int bx = lo.x; bx <= hi_capped.x; bx++) {
          const int2 wrapped = wrap_texel(int2(bx, by), int2(bins_));
          /* The shift that takes the copy into the image where this bin is. */
          const float2 shift = float2(wrapped - int2(bx, by)) / float(bins_);
          bin_items_[int64_t(wrapped.y) * bins_ + wrapped.x].append({i, shift});
        }
      }
    }
  }

  float shape_of(const Copy &copy, const float2 local) const
  {
    float2 q = local;
    if (copy.flip_x) {
      q.x = -q.x;
    }
    if (copy.flip_y) {
      q.y = -q.y;
    }
    float v;
    if (copy.shape == Shape::Image) {
      if (math::abs(q.x) > 1.0f || math::abs(q.y) > 1.0f) {
        return 0.0f;
      }
      v = images_.is_empty() ? 1.0f :
                               images_[math::clamp(copy.image, 0, int(images_.size()) - 1)]
                                   .sample_clamped(q * 0.5f + 0.5f);
    }
    else {
      v = shape_value(copy.shape, q, copy.specific);
    }
    if (copy.fade_start > 0.0f) {
      v *= math::clamp((local.x + 1.0f) / (2.0f * copy.fade_start), 0.0f, 1.0f);
    }
    if (copy.fade_end > 0.0f) {
      v *= math::clamp((1.0f - local.x) / (2.0f * copy.fade_end), 0.0f, 1.0f);
    }
    return v;
  }

  Hit at(const float2 uv, const Combine combine) const
  {
    Hit hit;
    const int2 bin = wrap_texel(int2(math::floor(uv * float(bins_))), int2(bins_));
    float best = -1.0f;
    for (const std::pair<int, float2> &item : bin_items_[int64_t(bin.y) * bins_ + bin.x]) {
      const Copy &copy = copies_[item.first];
      const float2 d = uv - (copy.center + item.second);
      const float c = math::cos(-copy.angle);
      const float s = math::sin(-copy.angle);
      const float2 local = float2(d.x * c - d.y * s, d.x * s + d.y * c) /
                           math::max(copy.half_size, float2(1e-6f));
      if (math::abs(local.x) > 1.0f || math::abs(local.y) > 1.0f) {
        continue;
      }
      const float shape = this->shape_of(copy, local);
      if (shape <= 0.0f) {
        continue;
      }
      const float v = shape * copy.value + copy.offset;
      switch (combine) {
        case Combine::Max:
          if (v > best) {
            best = v;
            hit.value = v;
            hit.copy = item.first;
            hit.local = local;
          }
          break;
        case Combine::Add:
          hit.value += shape * copy.value;
          if (v > best) {
            best = v;
            hit.copy = item.first;
            hit.local = local;
          }
          break;
        case Combine::Over:
          if (item.first > hit.copy) {
            hit.value = v;
            hit.copy = item.first;
            hit.local = local;
          }
          break;
      }
    }
    return hit;
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Helpers
 * \{ */

inline float random_float(uint32_t &state)
{
  state = hash(state + 0x9e3779b9u);
  return hash_to_float(state);
}

/* Roughly normal random numbers, mean zero and about unit spread. */
inline float random_gaussian(uint32_t &state)
{
  return (random_float(state) + random_float(state) + random_float(state) - 1.5f) * 2.0f;
}

inline float fract_value(const float v)
{
  return v - std::floor(v);
}

/* Wraps a UV position back into the image. */
inline float2 fract_vec(const float2 v)
{
  return v - math::floor(v);
}

inline float2 rotate(const float2 p, const float angle)
{
  const float c = math::cos(angle);
  const float s = math::sin(angle);
  return float2(p.x * c - p.y * s, p.x * s + p.y * c);
}

/** \} */

}  // namespace blender::nodes::texture_graph::shapes
