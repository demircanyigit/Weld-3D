/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * The noise engine behind the Texture Graph noise library. Every noise works in UV space on
 * lattices with a whole number of cells across the image that wrap around, so all of them tile.
 *
 * `icon_work/noise_lab.py` is a Python twin of this file used to look at the noises without
 * building, keep the two in step.
 */

#pragma once

#include <cmath>
#include <cstdint>

#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"

#include "texture_graph_util.hh"

namespace blender::nodes::texture_graph::noise {

/* What a noise gets for one pixel. */
struct Sample {
  /* Pixel center in [0, 1). */
  float2 uv;
  int2 texel;
  int2 size;
  /* Number of cells across the image. */
  int scale;
  uint32_t seed;
  /* The node's own settings, in the order the node lists them. */
  float p[4];
};

enum class Kind : int {
  Value = 0,
  Perlin = 1,
  Ridged = 2,
  Turbulence = 3,
};

inline float smoothstep(const float a, const float b, const float x)
{
  const float t = math::clamp((x - a) / (b - a), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

inline float contrast(const float x, const float amount)
{
  return math::clamp((x - 0.5f) * amount + 0.5f, 0.0f, 1.0f);
}

inline float random(const int x, const int y, const uint32_t seed)
{
  return hash_to_float(hash(x, y, seed));
}

inline float random_at(const int2 cell, const int2 period, const uint32_t seed)
{
  const int2 c = wrap_texel(cell, period);
  return random(c.x, c.y, seed);
}

/* Smooth value noise in [0, 1], p is in cells. */
inline float value(const float2 p, const int2 period, const uint32_t seed)
{
  const int2 cell = int2(math::floor(p));
  const float2 f = p - float2(cell);
  const float2 u = f * f * (3.0f - 2.0f * f);
  const float c00 = random_at(cell, period, seed);
  const float c10 = random_at(cell + int2(1, 0), period, seed);
  const float c01 = random_at(cell + int2(0, 1), period, seed);
  const float c11 = random_at(cell + int2(1, 1), period, seed);
  return math::interpolate(math::interpolate(c00, c10, u.x), math::interpolate(c01, c11, u.x), u.y);
}

/* Signed gradient noise, about -1..1, p is in cells. */
inline float perlin(const float2 p, const int2 period, const uint32_t seed)
{
  const int2 cell = int2(math::floor(p));
  const float2 f = p - float2(cell);
  const auto corner = [&](const int2 offset) {
    const float angle = random_at(cell + offset, period, seed) * float(M_PI * 2.0);
    return math::cos(angle) * (f.x - float(offset.x)) + math::sin(angle) * (f.y - float(offset.y));
  };
  const float2 u = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f);
  const float bottom = math::interpolate(corner(int2(0, 0)), corner(int2(1, 0)), u.x);
  const float top = math::interpolate(corner(int2(0, 1)), corner(int2(1, 1)), u.x);
  return math::clamp(math::interpolate(bottom, top, u.y) * 1.41f, -1.0f, 1.0f);
}

/* Layers of noise, each twice as fine as the one before. Result in [0, 1]. */
inline float fbm(const float2 uv,
                 const int2 scale,
                 const int octaves,
                 const float roughness,
                 const uint32_t seed,
                 const Kind kind = Kind::Perlin,
                 const int first = 0)
{
  float total = 0.0f;
  float norm = 0.0f;
  float amplitude = 1.0f;
  for (int octave = first; octave < first + octaves; octave++) {
    const int2 period = scale * (1 << octave);
    const float2 p = uv * float2(period);
    const uint32_t s = seed + uint32_t(octave) * 1013u;
    float n = kind == Kind::Value ? value(p, period, s) * 2.0f - 1.0f : perlin(p, period, s);
    if (kind == Kind::Ridged) {
      n = 1.0f - math::abs(n);
    }
    else if (kind == Kind::Turbulence) {
      n = math::abs(n);
    }
    total += n * amplitude;
    norm += amplitude;
    amplitude *= roughness;
  }
  total /= math::max(norm, 1e-6f);
  if (kind == Kind::Value || kind == Kind::Perlin) {
    /* Sums of octaves bunch up around the middle, spread them back out. */
    total *= 1.0f + 0.6f * float(math::min(octaves - 1, 3)) / 3.0f;
    return math::clamp(0.5f + 0.5f * total, 0.0f, 1.0f);
  }
  return math::clamp(total, 0.0f, 1.0f);
}

inline float fbm(const float2 uv,
                 const int scale,
                 const int octaves,
                 const float roughness,
                 const uint32_t seed,
                 const Kind kind = Kind::Perlin)
{
  return fbm(uv, int2(scale), octaves, roughness, seed, kind);
}

/* Push the position around by smooth noise. */
inline float2 warp(const float2 uv,
                   const int2 scale,
                   const float amount,
                   const uint32_t seed,
                   const int octaves = 3)
{
  const float dx = fbm(uv, scale, octaves, 0.5f, seed + 71) - 0.5f;
  const float dy = fbm(uv, scale, octaves, 0.5f, seed + 137) - 0.5f;
  return uv + amount * float2(dx, dy);
}

inline float2 warp(const float2 uv, const int scale, const float amount, const uint32_t seed)
{
  return warp(uv, int2(scale), amount, seed);
}

struct Cells {
  /* Distance to the nearest and second nearest point, in cells. */
  float f1 = 9.0f;
  float f2 = 9.0f;
  /* A random value of the nearest cell. */
  float id = 0.0f;
  /* From the pixel to the nearest point. */
  float2 delta = float2(0.0f);
};

inline Cells worley(const float2 uv, const int2 scale, const uint32_t seed, const float jitter = 1.0f)
{
  const float2 p = uv * float2(scale);
  const int2 cell = int2(math::floor(p));
  Cells result;
  for (int dy = -1; dy <= 1; dy++) {
    for (int dx = -1; dx <= 1; dx++) {
      const int2 neighbor = cell + int2(dx, dy);
      const int2 c = wrap_texel(neighbor, scale);
      const uint32_t h = hash(c.x, c.y, seed);
      const float2 offset(0.5f + (hash_to_float(h) - 0.5f) * jitter,
                          0.5f + (hash_to_float(hash(h)) - 0.5f) * jitter);
      const float2 d = float2(neighbor) + offset - p;
      const float distance = math::length(d);
      if (distance < result.f1) {
        result.f2 = result.f1;
        result.f1 = distance;
        result.id = hash_to_float(hash(h ^ 0x5bd1e995u));
        result.delta = d;
      }
      else {
        result.f2 = math::min(result.f2, distance);
      }
    }
  }
  return result;
}

inline Cells worley(const float2 uv, const int scale, const uint32_t seed, const float jitter = 1.0f)
{
  return worley(uv, int2(scale), seed, jitter);
}

/* Short straight or bent strokes scattered per cell. Length and width are in cells, the angle in
 * turns. */
inline float strokes(const float2 uv,
                     const int2 scale,
                     const uint32_t seed,
                     const int count,
                     const float length,
                     const float width,
                     const float angle,
                     const float spread,
                     const float curve = 0.0f)
{
  const float2 p = uv * float2(scale);
  const int2 cell = int2(math::floor(p));
  float result = 0.0f;
  for (int dy = -1; dy <= 1; dy++) {
    for (int dx = -1; dx <= 1; dx++) {
      const int2 neighbor = cell + int2(dx, dy);
      const int2 c = wrap_texel(neighbor, scale);
      const uint32_t base = hash(c.x, c.y, seed);
      for (int k = 0; k < count; k++) {
        uint32_t h = hash(base + uint32_t(k) * 7919u);
        const float cx = float(neighbor.x) + hash_to_float(h);
        h = hash(h);
        const float cy = float(neighbor.y) + hash_to_float(h);
        h = hash(h);
        const float a = (angle + (hash_to_float(h) - 0.5f) * spread) * float(M_PI * 2.0);
        h = hash(h);
        const float brightness = 0.4f + 0.6f * hash_to_float(h);
        h = hash(h);
        const float stroke_length = length * (0.5f + 0.5f * hash_to_float(h));
        const float rx = p.x - cx;
        const float ry = p.y - cy;
        const float u = rx * math::cos(a) + ry * math::sin(a);
        float v = -rx * math::sin(a) + ry * math::cos(a);
        if (curve != 0.0f) {
          v -= curve * (u * u) / math::max(stroke_length, 1e-3f);
        }
        const float t = math::clamp(u, -stroke_length * 0.5f, stroke_length * 0.5f);
        const float d = std::sqrt((u - t) * (u - t) + v * v);
        result = math::max(result, brightness * smoothstep(width, width * 0.3f, d));
      }
    }
  }
  return result;
}

/* Soft round spots, one chance per cell. The size is in cells. */
inline float spots(const float2 uv,
                   const int2 scale,
                   const uint32_t seed,
                   const float size,
                   const float size_random,
                   const float density = 1.0f)
{
  const float2 p = uv * float2(scale);
  const int2 cell = int2(math::floor(p));
  float result = 0.0f;
  for (int dy = -1; dy <= 1; dy++) {
    for (int dx = -1; dx <= 1; dx++) {
      const int2 neighbor = cell + int2(dx, dy);
      const int2 c = wrap_texel(neighbor, scale);
      uint32_t h = hash(c.x, c.y, seed);
      const float cx = float(neighbor.x) + hash_to_float(h);
      h = hash(h);
      const float cy = float(neighbor.y) + hash_to_float(h);
      h = hash(h);
      const float radius = size * (1.0f - size_random * hash_to_float(h));
      h = hash(h);
      const bool keep = hash_to_float(h) < density;
      h = hash(h);
      const float brightness = 0.5f + 0.5f * hash_to_float(h);
      if (keep) {
        const float d2 = (p.x - cx) * (p.x - cx) + (p.y - cy) * (p.y - cy);
        result += brightness * std::exp(-d2 / math::max(radius * radius, 1e-4f));
      }
    }
  }
  return math::clamp(result, 0.0f, 1.0f);
}

/* One random value per pixel. */
inline float white(const int2 texel, const uint32_t seed)
{
  return random(texel.x, texel.y, seed);
}

/* -------------------------------------------------------------------- */
/** \name Grunge mixer
 *
 * One function with many looks: a base noise, optionally pushed around, with spots, cell walls,
 * streaks, scratches, fine grain and specks added, then cut into a mask.
 * \{ */

struct Grunge {
  Kind kind = Kind::Perlin;
  int octaves = 6;
  float roughness = 0.55f;
  float warp = 0.0f;
  int scale_factor = 1;
  float spots = 0.0f;
  float spot_size = 0.35f;
  int spot_scale = 2;
  float cells = 0.0f;
  int cell_scale = 1;
  float specks = 0.0f;
  float speck_density = 0.03f;
  float streaks = 0.0f;
  float scratches = 0.0f;
  float threshold = 0.5f;
  float softness = 0.15f;
  bool invert = false;
  float fine = 0.0f;
};

/* The first two settings are Amount and Contrast. */
inline float grunge(const Sample &s, const Grunge &g)
{
  const int S = s.scale * g.scale_factor;
  float2 uv = s.uv;
  if (g.warp != 0.0f) {
    uv = warp(uv, S, g.warp, s.seed + 5);
  }
  float v = fbm(uv, int2(S), g.octaves, g.roughness, s.seed, g.kind);
  if (g.spots != 0.0f) {
    v += g.spots * (spots(s.uv, int2(S * g.spot_scale), s.seed + 11, g.spot_size, 0.6f) - 0.3f);
  }
  if (g.cells != 0.0f) {
    const Cells c = worley(uv, S * g.cell_scale, s.seed + 17);
    v -= g.cells * (1.0f - smoothstep(0.0f, 0.25f, c.f2 - c.f1)) * 0.5f;
  }
  if (g.streaks != 0.0f) {
    v += g.streaks *
         (fbm(s.uv, int2(S * 8, math::max(1, S / 2)), 4, 0.5f, s.seed + 23, Kind::Value) - 0.5f);
  }
  if (g.scratches != 0.0f) {
    v += g.scratches * strokes(s.uv, int2(S * 2), s.seed + 29, 2, 0.9f, 0.025f, 0.1f, 0.25f);
  }
  if (g.fine != 0.0f) {
    v += g.fine * (fbm(s.uv, S * 16, 2, 0.5f, s.seed + 31) - 0.5f);
  }
  if (g.specks != 0.0f && white(s.texel, s.seed + 37) < g.speck_density) {
    v += g.specks;
  }
  const float t = g.threshold - (s.p[0] - 0.5f);
  const float soft = math::max(g.softness * (1.0f + (0.5f - s.p[1]) * 1.8f), 1e-4f);
  const float result = smoothstep(t - soft, t + soft, v);
  return g.invert ? 1.0f - result : result;
}

/** \} */

}  // namespace blender::nodes::texture_graph::noise
