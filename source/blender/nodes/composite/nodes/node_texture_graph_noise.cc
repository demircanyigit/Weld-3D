/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * The noise library of the Texture Graph: 83 ready-made noises and grunge masks. They all share
 * one operation and differ only by a recipe in the table below, built on texture_graph_noise.hh.
 * Every noise tiles and changes with its Seed.
 */

#include <array>
#include <cctype>
#include <string>
#include <utility>

#include "BLI_math_vector.hh"
#include "BLI_task.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"
#include "texture_graph_noise.hh"
#include "texture_graph_util.hh"

namespace blender::nodes::node_texture_graph_noise_cc {

namespace tg = texture_graph;
using namespace tg::noise;

/* -------------------------------------------------------------------- */
/** \name Settings
 * \{ */

enum class ParamKind : int {
  Float = 0,
  Int = 1,
  Menu = 2,
};

struct Param {
  const char *name = nullptr;
  ParamKind kind = ParamKind::Float;
  float default_value = 0.0f;
  float min = 0.0f;
  float max = 1.0f;
  const char *description = nullptr;
  bool factor = false;
};

static constexpr Param roughness(const float d)
{
  return {"Roughness", ParamKind::Float, d, 0.0f, 1.0f, "How strong each finer layer is", true};
}
static constexpr Param warp_amount(const float d)
{
  return {"Warp", ParamKind::Float, d, 0.0f, 2.0f, "How much the noise is pushed around"};
}
static constexpr Param bands(const float d)
{
  return {"Bands", ParamKind::Float, d, 0.5f, 10.0f, "Number of light and dark bands"};
}
static constexpr Param stretch(const int d)
{
  return {"Stretch", ParamKind::Int, float(d), 1.0f, 64.0f, "How many times longer than wide"};
}
static constexpr Param jitter(const float d)
{
  return {"Jitter", ParamKind::Float, d, 0.0f, 1.0f, "How far the points move from a grid", true};
}
static constexpr Param angle(const float d)
{
  return {"Angle", ParamKind::Float, d, 0.0f, 1.0f, "Direction, as a part of a full turn", true};
}
static constexpr Param length(const float d, const float max = 1.0f)
{
  return {"Length", ParamKind::Float, d, 0.0f, max, "How long the shapes are"};
}
static constexpr Param curl(const float d)
{
  return {"Curl", ParamKind::Float, d, 0.0f, 1.0f, "How much the strands bend", true};
}
static constexpr Param amount()
{
  return {"Amount", ParamKind::Float, 0.5f, 0.0f, 1.0f, "How much of the image is covered", true};
}
static constexpr Param contrast_param()
{
  return {"Contrast", ParamKind::Float, 0.5f, 0.0f, 1.0f, "How sharp the edges are", true};
}
static constexpr Param grain(const float d)
{
  return {"Grain", ParamKind::Float, d, 0.0f, 1.0f, "Fine noise over the surface", true};
}
static constexpr Param spot_size(const float d)
{
  return {"Size", ParamKind::Float, d, 0.0f, 2.0f, "Size of the spots, in cells"};
}
static constexpr Param size_random(const float d)
{
  return {"Size Random", ParamKind::Float, d, 0.0f, 1.0f, "How much the spot sizes vary", true};
}

static const EnumPropertyItem voronoi_mode_items[] = {
    {0, "DISTANCE", 0, N_("Distance"), N_("Distance to the nearest point")},
    {1, "EDGES", 0, N_("Edges"), N_("Dark lines where cells meet")},
    {2, "CELLS", 0, N_("Cells"), N_("A flat random gray per cell")},
    {0, nullptr, 0, nullptr, nullptr},
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Recipes
 *
 * Kept in step with `icon_work/noise_lab.py`.
 * \{ */

static int half(const int scale)
{
  return math::max(1, scale / 2);
}

static float perlin_noise(const Sample &s)
{
  const float2 uv = s.p[0] != 0.0f ? warp(s.uv, s.scale, s.p[0], s.seed + 3) : s.uv;
  return fbm(uv, s.scale, 1, 0.5f, s.seed);
}

static float fractal_sum_base(const Sample &s)
{
  const int first = int(s.p[0]);
  const int octaves = math::max(1, int(s.p[1]) - first);
  return fbm(s.uv, int2(s.scale), octaves, s.p[2], s.seed, Kind::Value, first);
}

static float fractal_sum_1(const Sample &s)
{
  return fbm(s.uv, s.scale, 4, s.p[0], s.seed, Kind::Value);
}
static float fractal_sum_2(const Sample &s)
{
  return fbm(s.uv, s.scale, 7, s.p[0], s.seed, Kind::Value);
}
static float fractal_sum_3(const Sample &s)
{
  return fbm(s.uv, s.scale, 6, s.p[0], s.seed, Kind::Ridged);
}
static float fractal_sum_4(const Sample &s)
{
  return fbm(s.uv, s.scale, 6, s.p[0], s.seed, Kind::Turbulence);
}
static float clouds_1(const Sample &s)
{
  return fbm(s.uv, s.scale, 5, s.p[0], s.seed);
}
static float clouds_2(const Sample &s)
{
  return contrast(fbm(s.uv, s.scale, 7, s.p[0], s.seed), 1.8f);
}
static float clouds_3(const Sample &s)
{
  return fbm(warp(s.uv, s.scale, s.p[1], s.seed + 1), s.scale, 8, s.p[0], s.seed);
}
static float plasma(const Sample &s)
{
  return 0.5f + 0.5f * math::sin(fbm(s.uv, s.scale, 5, 0.7f, s.seed) * float(M_PI * 2.0) * s.p[0]);
}
static float creased(const Sample &s)
{
  const float v = fbm(warp(s.uv, s.scale, s.p[0], s.seed + 2), s.scale, 6, 0.5f, s.seed, Kind::Ridged);
  return v * v;
}
static float fluid(const Sample &s)
{
  float2 uv = warp(s.uv, s.scale, s.p[0], s.seed + 1);
  uv = warp(uv, s.scale, s.p[0] * 0.7f, s.seed + 2);
  return fbm(uv, s.scale, 4, 0.5f, s.seed);
}
static float liquid(const Sample &s)
{
  const float n = fbm(warp(s.uv, s.scale, 0.5f, s.seed + 1), s.scale, 3, 0.5f, s.seed);
  return 0.5f + 0.5f * math::sin(n * float(M_PI * 2.0) * s.p[0]);
}
static float gaussian_noise(const Sample &s)
{
  float total = 0.0f;
  for (int k = 0; k < 4; k++) {
    total += perlin(s.uv * float(s.scale), int2(s.scale), s.seed + uint32_t(k));
  }
  return math::clamp(0.5f + total / 4.0f * 1.4f * s.p[0], 0.0f, 1.0f);
}
static float white_noise(const Sample &s)
{
  return white(s.texel, s.seed);
}
static float white_noise_fast(const Sample &s)
{
  return value(math::floor(s.uv * float(s.scale)) + 0.5f, int2(s.scale), s.seed);
}
static float blue_noise_fast(const Sample &s)
{
  /* White noise with its blurred self taken away, which leaves only the fine detail. */
  float sum = 0.0f;
  for (int dy = -2; dy <= 2; dy++) {
    for (int dx = -2; dx <= 2; dx++) {
      if (dx != 0 || dy != 0) {
        sum += white(tg::wrap_texel(s.texel + int2(dx, dy), s.size), s.seed);
      }
    }
  }
  const float h = white(s.texel, s.seed) - sum / 24.0f;
  return math::clamp(0.5f + h * 1.6f, 0.0f, 1.0f);
}
static float anisotropic_noise(const Sample &s)
{
  const float v = fbm(s.uv, int2(s.scale * int(s.p[0]), s.scale), 2, 0.35f, s.seed, Kind::Perlin);
  return contrast(v, 1.5f + (1.0f - s.p[1]) * 4.0f);
}
static int2 directional_scale(const Sample &s)
{
  return int2(half(s.scale), s.scale * int(s.p[0]));
}
static float directional_noise_1(const Sample &s)
{
  return fbm(s.uv, directional_scale(s), 4, 0.5f, s.seed, Kind::Value);
}
static float directional_noise_2(const Sample &s)
{
  return fbm(s.uv, directional_scale(s), 6, 0.6f, s.seed, Kind::Perlin);
}
static float directional_noise_3(const Sample &s)
{
  return fbm(warp(s.uv, s.scale, s.p[1], s.seed + 1), directional_scale(s), 5, 0.55f, s.seed);
}
static float directional_noise_4(const Sample &s)
{
  const float v = fbm(s.uv, directional_scale(s), 5, 0.55f, s.seed, Kind::Ridged);
  return v * v * v;
}
static float directional_scratches(const Sample &s)
{
  return strokes(s.uv, int2(s.scale), s.seed, 3, 1.0f, 0.02f, s.p[0], s.p[1]);
}
static float cells_1(const Sample &s)
{
  return math::clamp(1.0f - worley(s.uv, s.scale, s.seed, s.p[0]).f1, 0.0f, 1.0f);
}
static float cells_2(const Sample &s)
{
  const Cells c = worley(s.uv, s.scale, s.seed, s.p[0]);
  return math::clamp((c.f2 - c.f1) * 2.5f, 0.0f, 1.0f);
}
static float cells_3(const Sample &s)
{
  return worley(s.uv, s.scale, s.seed, s.p[0]).id;
}
static float cells_4(const Sample &s)
{
  float total = 0.0f;
  for (int o = 0; o < 3; o++) {
    total += (1.0f - worley(s.uv, s.scale << o, s.seed + uint32_t(o), s.p[0]).f1) /
             float(1 << o);
  }
  return math::clamp(total / 1.75f, 0.0f, 1.0f);
}
static float voronoi(const Sample &s)
{
  const Cells c = worley(s.uv, s.scale, s.seed, s.p[1]);
  switch (int(s.p[0])) {
    case 0:
      return math::clamp(c.f1, 0.0f, 1.0f);
    case 1:
      return math::clamp((c.f2 - c.f1) * 2.0f, 0.0f, 1.0f);
    default:
      return c.id;
  }
}
static float voronoi_fractal(const Sample &s)
{
  float total = 0.0f;
  float norm = 0.0f;
  float amplitude = 1.0f;
  for (int o = 0; o < math::max(1, int(s.p[0])); o++) {
    const Cells c = worley(s.uv, s.scale << o, s.seed + uint32_t(o) * 1013u);
    total += math::clamp(c.f1, 0.0f, 1.0f) * amplitude;
    norm += amplitude;
    amplitude *= s.p[1];
  }
  return total / norm;
}
static float crystal(const Sample &s, const int scale, const uint32_t seed)
{
  const Cells c = worley(s.uv, scale, seed);
  const float a = c.id * float(M_PI * 2.0);
  const float facet = 0.5f + 0.8f * (c.delta.x * math::cos(a) + c.delta.y * math::sin(a));
  return math::clamp(0.5f * c.id + 0.5f * facet, 0.0f, 1.0f);
}
static float crystal_1(const Sample &s)
{
  return crystal(s, s.scale, s.seed);
}
static float crystal_2(const Sample &s)
{
  return math::max(crystal(s, s.scale, s.seed), crystal(s, s.scale * 3, s.seed + 1) * 0.8f);
}
static float caustics(const Sample &s)
{
  const float2 uv = warp(s.uv, s.scale, 0.35f * s.p[0], s.seed + 2);
  const Cells c = worley(uv, s.scale * 2, s.seed);
  const float v = math::clamp(1.0f - (c.f2 - c.f1) * 4.0f, 0.0f, 1.0f);
  return v * v * v;
}
static float microscope_view(const Sample &s)
{
  const float2 uv = warp(s.uv, s.scale, 0.25f, s.seed + 4);
  const Cells c = worley(uv, s.scale, s.seed);
  const float wall = smoothstep(0.0f, 0.12f, c.f2 - c.f1);
  const float inner = fbm(uv, s.scale * 4, 4, 0.55f, s.seed + 8);
  return math::clamp(wall * (0.35f + 0.4f * inner + 0.25f * smoothstep(0.5f, 0.0f, c.f1)),
                     0.0f,
                     1.0f);
}
static float bnw_spots_1(const Sample &s)
{
  return smoothstep(0.47f, 0.53f, fbm(s.uv, s.scale, 4, s.p[0], s.seed));
}
static float bnw_spots_2(const Sample &s)
{
  return smoothstep(0.42f, 0.48f, fbm(s.uv, s.scale, 6, s.p[0], s.seed, Kind::Turbulence) * 1.6f);
}
static float bnw_spots_3(const Sample &s)
{
  return smoothstep(0.5f, 0.56f, fbm(s.uv, s.scale, 5, s.p[0], s.seed)) *
         smoothstep(0.45f, 0.5f, fbm(s.uv, half(s.scale), 3, 0.5f, s.seed + 9));
}
static float gaussian_spots_1(const Sample &s)
{
  return spots(s.uv, int2(s.scale), s.seed, s.p[0], s.p[1], 0.6f);
}
static float gaussian_spots_2(const Sample &s)
{
  return spots(s.uv, int2(s.scale), s.seed, s.p[0], s.p[1]) * 0.6f +
         spots(s.uv, int2(s.scale * 2), s.seed + 1, s.p[0], s.p[1]) * 0.4f;
}
static float galvanic(const Sample &s, const int cell_factor)
{
  const int S = s.scale * cell_factor;
  const Cells c = worley(s.uv, S, s.seed);
  const float a = c.id * float(M_PI * 2.0);
  const float facet = 0.5f + 0.5f * (c.delta.x * math::cos(a) + c.delta.y * math::sin(a)) * 1.4f;
  float v = 0.35f + 0.4f * c.id + 0.25f * facet;
  v *= 0.75f + 0.25f * smoothstep(0.0f, 0.06f, c.f2 - c.f1);
  return math::clamp(v + (fbm(s.uv, S * 4, 3, 0.5f, s.seed + 3) - 0.5f) * s.p[0], 0.0f, 1.0f);
}
static float grunge_galvanic_large(const Sample &s)
{
  return galvanic(s, 1);
}
static float grunge_galvanic_small(const Sample &s)
{
  return galvanic(s, 3);
}
static float grunge_leaks(const Sample &s)
{
  const int S = s.scale;
  const float v = fbm(s.uv, int2(S * 6, half(S)), 5, 0.55f, s.seed, Kind::Value);
  const float start = fbm(s.uv, int2(S * 6, 1), 2, 0.5f, s.seed + 9, Kind::Value);
  const float along = start - s.uv.y - math::floor(start - s.uv.y);
  const float fall = smoothstep(s.p[0], 0.0f, along);
  return math::clamp(smoothstep(0.45f, 0.75f, v) * fall, 0.0f, 1.0f);
}
static float grunge_leaky_paint(const Sample &s)
{
  return math::max(grunge_leaks(s),
                   smoothstep(0.55f, 0.6f, fbm(s.uv, s.scale, 5, 0.55f, s.seed + 4)));
}
static float scratches(
    const Sample &s, const int count, const float width, const float spread, const float dirt)
{
  float v = strokes(s.uv, int2(s.scale), s.seed, count, 1.0f, width, s.p[0], spread);
  if (dirt != 0.0f) {
    v = math::clamp(v * (0.6f + 0.8f * fbm(s.uv, s.scale, 5, 0.55f, s.seed + 3)) +
                        dirt * (fbm(s.uv, s.scale * 2, 6, 0.6f, s.seed + 5) - 0.6f),
                    0.0f,
                    1.0f);
  }
  return v;
}
static float grunge_scratches_fine(const Sample &s)
{
  return scratches(s, 3, 0.012f, 1.0f, 0.0f);
}
static float grunge_scratches_rough(const Sample &s)
{
  return scratches(s, 3, 0.04f, 1.0f, 0.0f);
}
static float grunge_scratches_dirty(const Sample &s)
{
  return scratches(s, 2, 0.025f, 1.0f, 0.5f);
}
static float grunge_shavings(const Sample &s)
{
  const float2 uv = warp(s.uv, s.scale, s.p[0] * 0.3f, s.seed + 2);
  return strokes(uv, int2(s.scale), s.seed, 3, 0.8f, 0.05f, 0.0f, 1.0f, 0.6f);
}
static float fibers(const Sample &s, const int count, const float width)
{
  const float2 uv = warp(s.uv, s.scale, s.p[0], s.seed + 1);
  return strokes(uv, int2(s.scale), s.seed, count, 1.1f, width, 0.0f, 1.0f, 0.25f);
}
static float messy_fibers_1(const Sample &s)
{
  return fibers(s, 3, 0.012f);
}
static float messy_fibers_2(const Sample &s)
{
  return fibers(s, 4, 0.018f);
}
static float messy_fibers_3(const Sample &s)
{
  return fibers(s, 2, 0.03f);
}
static float fur(const Sample &s, const float curl_amount, const int count)
{
  const float2 uv = curl_amount != 0.0f ? warp(s.uv, s.scale, curl_amount, s.seed + 6) : s.uv;
  return strokes(uv, int2(s.scale), s.seed, count, 1.4f * s.p[0], 0.035f, 0.25f, 0.12f);
}
static float fur_1(const Sample &s)
{
  return fur(s, 0.0f, 6);
}
static float fur_2(const Sample &s)
{
  return fur(s, 0.12f, 6);
}
static float fur_3(const Sample &s)
{
  return fur(s, 0.04f, 4);
}
static float waveform_1(const Sample &s)
{
  const int S = math::max(1, s.scale);
  const float x = s.uv.x * float(S);
  const int column = int(math::floor(x));
  const float height = tg::noise::random(tg::wrap_texel(int2(column, 0), int2(S, 1)).x, 0, s.seed) * s.p[0] +
                       (1.0f - s.p[0]) * 0.5f;
  const bool inside = math::abs(s.uv.y - 0.5f) < height * 0.5f;
  const bool bar = x - math::floor(x) < 0.75f;
  return (inside && bar) ? 1.0f : 0.0f;
}
static float dirt_gradient(const Sample &s)
{
  const float g = smoothstep(s.p[0] + 0.25f, s.p[0] - 0.25f, s.uv.y);
  const float n = fbm(s.uv, s.scale, 6, 0.55f, s.seed);
  return math::clamp(g + (n - 0.5f) * s.p[1] * 2.0f, 0.0f, 1.0f);
}

/* Generated from the Python twin, do not tune here alone. */
static const Grunge grunge_maps[] = {
    Grunge{.octaves = 7, .roughness = 0.6f, .specks = 0.4f, .threshold = 0.52f, .softness = 0.12f},
    Grunge{.kind = Kind::Ridged, .warp = 0.15f, .threshold = 0.62f, .softness = 0.08f},
    Grunge{.octaves = 5,
           .roughness = 0.5f,
           .spots = 0.9f,
           .spot_size = 0.3f,
           .spot_scale = 3,
           .threshold = 0.55f,
           .softness = 0.1f},
    Grunge{.kind = Kind::Turbulence,
           .octaves = 7,
           .roughness = 0.6f,
           .threshold = 0.28f,
           .softness = 0.1f,
           .invert = true},
    Grunge{.roughness = 0.65f, .cells = 0.8f, .cell_scale = 2, .threshold = 0.45f, .softness = 0.12f},
    Grunge{.roughness = 0.5f, .streaks = 0.9f, .softness = 0.1f},
    Grunge{.scratches = 0.8f, .threshold = 0.55f, .fine = 0.3f},
    Grunge{.kind = Kind::Value, .octaves = 8, .roughness = 0.7f, .warp = 0.3f, .softness = 0.05f},
    Grunge{.octaves = 4, .roughness = 0.45f, .warp = 0.6f, .softness = 0.2f, .fine = 0.5f},
    Grunge{.kind = Kind::Ridged,
           .octaves = 8,
           .roughness = 0.65f,
           .specks = 0.6f,
           .speck_density = 0.05f,
           .threshold = 0.7f,
           .softness = 0.06f},
    Grunge{.spots = 1.2f, .spot_size = 0.2f, .spot_scale = 6, .threshold = 0.6f, .softness = 0.08f},
    Grunge{.kind = Kind::Turbulence,
           .octaves = 5,
           .roughness = 0.5f,
           .warp = 0.4f,
           .cells = 0.5f,
           .threshold = 0.35f,
           .softness = 0.12f},
    Grunge{.octaves = 7,
           .roughness = 0.62f,
           .streaks = 0.6f,
           .scratches = 0.5f,
           .threshold = 0.52f,
           .softness = 0.12f},
    Grunge{.kind = Kind::Value,
           .roughness = 0.5f,
           .spots = 0.7f,
           .spot_size = 0.5f,
           .threshold = 0.45f,
           .softness = 0.25f},
    Grunge{.octaves = 8,
           .roughness = 0.7f,
           .cells = 0.6f,
           .cell_scale = 4,
           .softness = 0.1f,
           .invert = true,
           .fine = 0.4f},
};

static const Grunge grunge_concrete = Grunge{.octaves = 7,
                                             .roughness = 0.6f,
                                             .cells = 0.35f,
                                             .cell_scale = 3,
                                             .specks = 0.5f,
                                             .speck_density = 0.02f,
                                             .softness = 0.35f,
                                             .fine = 0.6f};
static const Grunge grunge_damas = Grunge{.kind = Kind::Ridged,
                                          .octaves = 5,
                                          .roughness = 0.5f,
                                          .warp = 0.5f,
                                          .cells = 0.6f,
                                          .threshold = 0.6f,
                                          .softness = 0.12f};
static const Grunge grunge_rough_dirty = Grunge{.kind = Kind::Ridged,
                                                .octaves = 7,
                                                .roughness = 0.65f,
                                                .specks = 0.5f,
                                                .speck_density = 0.06f,
                                                .threshold = 0.55f,
                                                .softness = 0.2f,
                                                .fine = 0.4f};
static const Grunge grunge_rust_fine = Grunge{.kind = Kind::Turbulence,
                                              .roughness = 0.6f,
                                              .scale_factor = 3,
                                              .threshold = 0.22f,
                                              .softness = 0.08f,
                                              .invert = true,
                                              .fine = 0.3f};
static const Grunge grunge_spots = Grunge{.octaves = 3,
                                          .roughness = 0.5f,
                                          .spots = 1.4f,
                                          .spot_size = 0.3f,
                                          .spot_scale = 4,
                                          .threshold = 0.65f,
                                          .softness = 0.06f};
static const Grunge grunge_spots_dirty = Grunge{
    .roughness = 0.6f, .spots = 1.0f, .spot_scale = 3, .threshold = 0.55f, .fine = 0.5f};
static const Grunge grunge_splashes_dusty = Grunge{.octaves = 5,
                                                   .roughness = 0.6f,
                                                   .warp = 0.3f,
                                                   .spots = 1.3f,
                                                   .spot_size = 0.25f,
                                                   .spot_scale = 5,
                                                   .specks = 0.7f,
                                                   .speck_density = 0.04f,
                                                   .threshold = 0.62f,
                                                   .softness = 0.05f};
static const Grunge moisture_noise_1 = Grunge{
    .octaves = 5, .roughness = 0.5f, .warp = 0.35f, .softness = 0.3f};
static const Grunge moisture_noise_2 = Grunge{
    .warp = 0.5f, .specks = 0.3f, .threshold = 0.55f, .softness = 0.18f, .fine = 0.4f};
static const Grunge dirt_1 = Grunge{.spots = 0.6f, .spot_size = 0.4f, .softness = 0.3f};
static const Grunge dirt_2 = Grunge{.octaves = 4,
                                    .roughness = 0.5f,
                                    .specks = 1.0f,
                                    .speck_density = 0.08f,
                                    .threshold = 0.6f,
                                    .softness = 0.2f};
static const Grunge dirt_3 = Grunge{
    .kind = Kind::Ridged, .roughness = 0.6f, .warp = 0.2f, .threshold = 0.55f, .softness = 0.25f};
static const Grunge dirt_4 = Grunge{.octaves = 5,
                                    .warp = 0.25f,
                                    .cells = 0.9f,
                                    .cell_scale = 2,
                                    .threshold = 0.45f,
                                    .softness = 0.25f};
static const Grunge dirt_5 = Grunge{.roughness = 0.6f,
                                    .warp = 0.45f,
                                    .spots = 0.8f,
                                    .spot_size = 0.25f,
                                    .spot_scale = 4,
                                    .softness = 0.3f};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Table
 * \{ */

using NoiseFunction = float (*)(const Sample &s);

struct Recipe {
  /* The UI name. The node id is "TextureGraphNode" with the spaces taken out. */
  const char *name;
  const char *description;
  /* Cells across the image by default, zero when the noise works per pixel and has no Scale. */
  int scale;
  Param params[4];
  NoiseFunction function = nullptr;
  const Grunge *grunge = nullptr;
};

#define GRUNGE_PARAMS {amount(), contrast_param()}

static const Recipe recipes[] = {
    /* Smooth noises. */
    {"Perlin Noise",
     "Soft gradient noise in one layer",
     4,
     {Param{"Disorder", ParamKind::Float, 0.0f, 0.0f, 2.0f, "Pushes the noise around itself"}},
     perlin_noise},
    {"Fractal Sum Base",
     "Value noise summed over a chosen range of layers",
     2,
     {Param{"Min Level", ParamKind::Int, 2.0f, 0.0f, 10.0f, "The coarsest layer"},
      Param{"Max Level", ParamKind::Int, 8.0f, 1.0f, 12.0f, "The finest layer"},
      roughness(0.5f)},
     fractal_sum_base},
    {"Fractal Sum 1", "Soft value noise in four layers", 2, {roughness(0.5f)}, fractal_sum_1},
    {"Fractal Sum 2", "Detailed value noise in seven layers", 2, {roughness(0.65f)}, fractal_sum_2},
    {"Fractal Sum 3", "Ridges with sharp bright crests", 2, {roughness(0.55f)}, fractal_sum_3},
    {"Fractal Sum 4", "Billowy noise with sharp dark creases", 2, {roughness(0.55f)}, fractal_sum_4},
    {"Clouds 1", "Soft clouds", 2, {roughness(0.5f)}, clouds_1},
    {"Clouds 2", "Clouds with more contrast and detail", 3, {roughness(0.6f)}, clouds_2},
    {"Clouds 3", "Swirled detailed clouds", 2, {roughness(0.5f), warp_amount(0.25f)}, clouds_3},
    {"Plasma", "Clouds folded into bands", 2, {bands(2.0f)}, plasma},
    {"Creased", "Thin bright folds, like crumpled paper", 2, {warp_amount(0.3f)}, creased},
    {"Fluid", "Clouds swirled twice, like stirred liquid", 2, {warp_amount(0.6f)}, fluid},
    {"Liquid", "Swirled bands, like marbled paint", 2, {bands(3.0f)}, liquid},
    {"Gaussian Noise",
     "Fine noise that is mostly mid gray",
     16,
     {Param{"Spread", ParamKind::Float, 1.0f, 0.0f, 2.0f, "How far the values stray from gray"}},
     gaussian_noise},
    {"White Noise", "A random gray for every pixel", 0, {}, white_noise},
    {"White Noise Fast", "A random gray for every square of a grid", 64, {}, white_noise_fast},
    {"Blue Noise Fast", "Pixel noise with no clumps", 0, {}, blue_noise_fast},
    /* Stretched noises. */
    {"Anisotropic Noise",
     "Noise stretched into long streaks",
     4,
     {stretch(16),
      Param{"Smoothness", ParamKind::Float, 1.0f, 0.0f, 1.0f, "Softer or sharper streaks", true}},
     anisotropic_noise},
    {"Directional Noise 1", "Smooth vertical streaks", 4, {stretch(12)}, directional_noise_1},
    {"Directional Noise 2", "Detailed vertical streaks", 4, {stretch(8)}, directional_noise_2},
    {"Directional Noise 3",
     "Wavy vertical streaks",
     4,
     {stretch(8), warp_amount(0.1f)},
     directional_noise_3},
    {"Directional Noise 4", "Thin bright vertical lines", 4, {stretch(12)}, directional_noise_4},
    {"Directional Scratches",
     "Short scratches that mostly point one way",
     6,
     {angle(0.1f),
      Param{"Angle Random", ParamKind::Float, 0.1f, 0.0f, 1.0f, "How much the scratches turn", true}},
     directional_scratches},
    /* Cells. */
    {"Cells 1", "Bright cell centers fading out", 6, {jitter(1.0f)}, cells_1},
    {"Cells 2", "Cells with dark edges", 6, {jitter(1.0f)}, cells_2},
    {"Cells 3", "A flat random gray per cell", 6, {jitter(1.0f)}, cells_3},
    {"Cells 4", "Cells of three sizes on top of each other", 3, {jitter(1.0f)}, cells_4},
    {"Voronoi",
     "Cells around scattered points",
     6,
     {Param{"Mode", ParamKind::Menu, 0.0f, 0.0f, 2.0f, "What to show of the cells"},
      jitter(1.0f)},
     voronoi},
    {"Voronoi Fractal",
     "Cell distances over several sizes",
     3,
     {Param{"Octaves", ParamKind::Int, 4.0f, 1.0f, 8.0f, "Number of cell sizes"}, roughness(0.5f)},
     voronoi_fractal},
    {"Crystal 1", "Shaded facets, like broken crystal", 6, {}, crystal_1},
    {"Crystal 2", "Facets of two sizes", 3, {}, crystal_2},
    {"Caustics", "Bright wavy lines, like light under water", 3, {warp_amount(1.0f)}, caustics},
    {"Microscope View", "Cells with soft walls, like tissue", 5, {}, microscope_view},
    /* Spots. */
    {"BnW Spots 1", "Black and white blobs", 2, {roughness(0.5f)}, bnw_spots_1},
    {"BnW Spots 2", "Black and white blobs with rough edges", 3, {roughness(0.55f)}, bnw_spots_2},
    {"BnW Spots 3", "Scattered black and white blobs", 4, {roughness(0.5f)}, bnw_spots_3},
    {"Gaussian Spots 1",
     "Soft round spots",
     6,
     {spot_size(0.3f), size_random(0.5f)},
     gaussian_spots_1},
    {"Gaussian Spots 2",
     "Soft round spots of two sizes",
     8,
     {spot_size(0.45f), size_random(0.7f)},
     gaussian_spots_2},
    /* Grunge with its own shape. */
    {"Grunge Galvanic Large",
     "Large shaded flakes, like galvanized metal",
     3,
     {grain(0.3f)},
     grunge_galvanic_large},
    {"Grunge Galvanic Small", "Small shaded flakes", 3, {grain(0.3f)}, grunge_galvanic_small},
    {"Grunge Leaks", "Streaks running down", 4, {length(0.6f)}, grunge_leaks},
    {"Grunge Leaky Paint", "Paint patches with drips", 4, {length(0.4f)}, grunge_leaky_paint},
    {"Grunge Scratches Fine", "Thin scratches", 8, {angle(0.0f)}, grunge_scratches_fine},
    {"Grunge Scratches Rough", "Wide scratches", 5, {angle(0.0f)}, grunge_scratches_rough},
    {"Grunge Scratches Dirty", "Scratches with dirt", 6, {angle(0.0f)}, grunge_scratches_dirty},
    {"Grunge Shavings", "Curled shavings", 6, {curl(0.4f)}, grunge_shavings},
    {"Messy Fibers 1", "Thin tangled fibers", 4, {curl(0.15f)}, messy_fibers_1},
    {"Messy Fibers 2", "Many tangled fibers", 6, {curl(0.3f)}, messy_fibers_2},
    {"Messy Fibers 3", "Thick tangled fibers", 3, {curl(0.5f)}, messy_fibers_3},
    {"Fur 1", "Straight hairs", 8, {length(1.0f, 3.0f)}, fur_1},
    {"Fur 2", "Wavy hairs", 8, {length(1.0f, 3.0f)}, fur_2},
    {"Fur 3", "Short fine hairs", 12, {length(0.6f, 3.0f)}, fur_3},
    {"Waveform 1",
     "Bars of random height, like a sound wave",
     32,
     {Param{"Height Random", ParamKind::Float, 0.9f, 0.0f, 1.0f, "How much the bars vary", true}},
     waveform_1},
    {"Dirt Gradient",
     "Dirt that gathers toward the bottom",
     3,
     {Param{"Height", ParamKind::Float, 0.5f, 0.0f, 1.0f, "Where the dirt fades out", true},
      Param{"Noise", ParamKind::Float, 0.4f, 0.0f, 1.0f, "How ragged the edge is", true}},
     dirt_gradient},
    /* Grunge masks made by the mixer. */
    {"Grunge Concrete", "Pitted concrete", 3, GRUNGE_PARAMS, nullptr, &grunge_concrete},
    {"Grunge Damas", "Swirled worn patches", 3, GRUNGE_PARAMS, nullptr, &grunge_damas},
    {"Grunge Rough Dirty", "Rough dirt with specks", 3, GRUNGE_PARAMS, nullptr, &grunge_rough_dirty},
    {"Grunge Rust Fine", "Fine rust", 3, GRUNGE_PARAMS, nullptr, &grunge_rust_fine},
    {"Grunge Spots", "Scattered spots", 3, GRUNGE_PARAMS, nullptr, &grunge_spots},
    {"Grunge Spots Dirty", "Spots with dirt", 3, GRUNGE_PARAMS, nullptr, &grunge_spots_dirty},
    {"Grunge Splashes Dusty",
     "Splashes and dust",
     3,
     GRUNGE_PARAMS,
     nullptr,
     &grunge_splashes_dusty},
    {"Moisture Noise 1", "Soft damp patches", 3, GRUNGE_PARAMS, nullptr, &moisture_noise_1},
    {"Moisture Noise 2", "Damp patches with grain", 3, GRUNGE_PARAMS, nullptr, &moisture_noise_2},
    {"Dirt 1", "Soft dirt", 3, GRUNGE_PARAMS, nullptr, &dirt_1},
    {"Dirt 2", "Specks of dirt", 3, GRUNGE_PARAMS, nullptr, &dirt_2},
    {"Dirt 3", "Streaky dirt", 3, GRUNGE_PARAMS, nullptr, &dirt_3},
    {"Dirt 4", "Dirt in cracks", 3, GRUNGE_PARAMS, nullptr, &dirt_4},
    {"Dirt 5", "Swirled dirt with spots", 3, GRUNGE_PARAMS, nullptr, &dirt_5},
#define GRUNGE_MAP(number, index) \
  {"Grunge Map " number, "A ready-made grunge mask", 3, GRUNGE_PARAMS, nullptr, &grunge_maps[index]}
    GRUNGE_MAP("001", 0),
    GRUNGE_MAP("002", 1),
    GRUNGE_MAP("003", 2),
    GRUNGE_MAP("004", 3),
    GRUNGE_MAP("005", 4),
    GRUNGE_MAP("006", 5),
    GRUNGE_MAP("007", 6),
    GRUNGE_MAP("008", 7),
    GRUNGE_MAP("009", 8),
    GRUNGE_MAP("010", 9),
    GRUNGE_MAP("011", 10),
    GRUNGE_MAP("012", 11),
    GRUNGE_MAP("013", 12),
    GRUNGE_MAP("014", 13),
    GRUNGE_MAP("015", 14),
#undef GRUNGE_MAP
};

#undef GRUNGE_PARAMS

static constexpr int recipes_num = 83;
static_assert(sizeof(recipes) / sizeof(recipes[0]) == recipes_num);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Node
 * \{ */

static void declare_recipe(const Recipe &recipe, NodeDeclarationBuilder &b)
{
  b.add_output<decl::Float>("Image").structure_type(StructureType::Dynamic);

  if (recipe.scale > 0) {
    b.add_input<decl::Int>("Scale").default_value(recipe.scale).min(1).max(256).description(
        "Number of noise cells across the image");
  }
  for (const Param &param : recipe.params) {
    if (param.name == nullptr) {
      break;
    }
    switch (param.kind) {
      case ParamKind::Float: {
        auto &socket = b.add_input<decl::Float>(param.name)
                           .default_value(param.default_value)
                           .min(param.min)
                           .max(param.max);
        socket.description(param.description);
        if (param.factor) {
          socket.subtype(PROP_FACTOR);
        }
        break;
      }
      case ParamKind::Int:
        b.add_input<decl::Int>(param.name)
            .default_value(int(param.default_value))
            .min(int(param.min))
            .max(int(param.max))
            .description(param.description);
        break;
      case ParamKind::Menu:
        b.add_input<decl::Menu>(param.name)
            .default_value(MenuValue(int(param.default_value)))
            .static_items(voronoi_mode_items)
            .description(param.description);
        break;
    }
  }
  b.add_input<decl::Int>("Seed").default_value(0).description(
      "Change it to get a different pattern");
  b.add_input<decl::Menu>("Resolution")
      .default_value(MenuValue(0))
      .static_items(tg::generator_size_items)
      .description("Size of the image this node makes");
}

/* A declare function per recipe, since a declaration does not know which node type it is for. */
template<size_t I> static void declare_recipe(NodeDeclarationBuilder &b)
{
  declare_recipe(recipes[I], b);
}

template<size_t... I>
static constexpr std::array<bke::NodeDeclareFunction, sizeof...(I)> make_declares(
    std::index_sequence<I...> /*indices*/)
{
  return {&declare_recipe<I>...};
}

static constexpr std::array<bke::NodeDeclareFunction, recipes_num> declares = make_declares(
    std::make_index_sequence<recipes_num>());

static bke::bNodeType node_types[recipes_num];

using namespace blender::compositor;

class NoiseOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Recipe &recipe = recipes[this->node().typeinfo - node_types];

    Sample sample;
    sample.scale = recipe.scale > 0 ?
                       math::max(this->get_input("Scale").get_single_value_default<int>(), 1) :
                       1;
    sample.seed = uint32_t(this->get_input("Seed").get_single_value_default<int>());
    for (int i = 0; i < 4; i++) {
      const Param &param = recipe.params[i];
      sample.p[i] = 0.0f;
      if (param.name == nullptr) {
        continue;
      }
      const Result &input = this->get_input(param.name);
      switch (param.kind) {
        case ParamKind::Float:
          sample.p[i] = input.get_single_value_default<float>();
          break;
        case ParamKind::Int:
          sample.p[i] = float(math::clamp(
              input.get_single_value_default<int>(), int(param.min), int(param.max)));
          break;
        case ParamKind::Menu:
          sample.p[i] = float(input.get_single_value_default<MenuValue>().value);
          break;
      }
    }

    const int resolution = this->get_input("Resolution").get_single_value_default<MenuValue>().value;
    const Domain domain = tg::generator_domain(this->context(), resolution);
    const int2 size = domain.data_size;
    sample.size = size;

    Result &output = this->get_result("Image");
    output.allocate_texture(domain);
    parallel_for(size, [&](const int2 texel) {
      Sample s = sample;
      s.texel = texel;
      s.uv = (float2(texel) + 0.5f) / float2(size);
      const float v = recipe.grunge ? grunge(s, *recipe.grunge) : recipe.function(s);
      output.store_pixel(texel, std::isfinite(v) ? math::clamp(v, 0.0f, 1.0f) : 0.0f);
    });
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new NoiseOperation(context, node);
}

static std::string without_spaces(const char *name)
{
  std::string result;
  for (const char *c = name; *c; c++) {
    if (*c != ' ') {
      result += *c;
    }
  }
  return result;
}

static std::string upper_snake(const char *name)
{
  std::string result;
  for (const char *c = name; *c; c++) {
    result += *c == ' ' ? '_' : char(toupper(*c));
  }
  return result;
}

static void node_register()
{
  /* Kept alive for the legacy enum names, which are plain pointers. */
  static std::array<std::string, recipes_num> enum_names;
  for (int i = 0; i < recipes_num; i++) {
    const Recipe &recipe = recipes[i];
    bke::bNodeType &ntype = node_types[i];
    enum_names[i] = "TEXTURE_GRAPH_" + upper_snake(recipe.name);

    tg_node_type_base(&ntype, "TextureGraphNode" + without_spaces(recipe.name));
    ntype.ui_name = recipe.name;
    ntype.ui_description = recipe.description;
    ntype.enum_name_legacy = enum_names[i].c_str();
    ntype.nclass = NODE_CLASS_TEXTURE;
    ntype.declare = declares[i];
    ntype.flag |= NODE_PREVIEW;
    ntype.get_compositor_operation = get_compositor_operation;
    bke::node_register_type(ntype);
  }
}
NOD_REGISTER_NODE(node_register)

/** \} */

}  // namespace blender::nodes::node_texture_graph_noise_cc
