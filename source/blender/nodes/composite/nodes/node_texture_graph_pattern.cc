/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * The pattern library of the Texture Graph: bricks, weaves, gradients, polygons and other plain
 * patterns made from nothing. Like the noise library, they share one operation and differ by a
 * recipe in the table below.
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
#include "texture_graph_shapes.hh"
#include "texture_graph_util.hh"

namespace blender::nodes::node_texture_graph_pattern_cc {

namespace tg = texture_graph;
using tg::noise::smoothstep;
using tg::shapes::Shape;

/* -------------------------------------------------------------------- */
/** \name Settings
 * \{ */

enum class ParamKind : int {
  Float = 0,
  Int = 1,
  Menu = 2,
  Bool = 3,
  /* Two numbers, taking two slots. */
  Vector2 = 4,
};

struct Param {
  const char *name = nullptr;
  ParamKind kind = ParamKind::Float;
  float default_value = 0.0f;
  float min = 0.0f;
  float max = 1.0f;
  const char *description = nullptr;
  bool factor = false;
  const EnumPropertyItem *items = nullptr;
  /* Second default of a Vector2. */
  float default_y = 0.0f;
};

static constexpr Param tiling(const int d)
{
  return {"Tiling", ParamKind::Int, float(d), 1.0f, 16.0f, "How many times the pattern repeats"};
}
static constexpr Param boolean(const char *name, const bool d, const char *description)
{
  return {name, ParamKind::Bool, d ? 1.0f : 0.0f, 0.0f, 1.0f, description};
}
static constexpr Param factor(const char *name, const float d, const char *description)
{
  return {name, ParamKind::Float, d, 0.0f, 1.0f, description, true};
}
static constexpr Param number(
    const char *name, const float d, const float min, const float max, const char *description)
{
  return {name, ParamKind::Float, d, min, max, description};
}
static constexpr Param integer(
    const char *name, const int d, const int min, const int max, const char *description)
{
  return {name, ParamKind::Int, float(d), float(min), float(max), description};
}
static constexpr Param menu(const char *name,
                            const int d,
                            const EnumPropertyItem *items,
                            const char *description)
{
  return {name, ParamKind::Menu, float(d), 0.0f, 0.0f, description, false, items};
}
static constexpr Param point(const char *name,
                             const float x,
                             const float y,
                             const char *description)
{
  return {name, ParamKind::Vector2, x, -1.0f, 2.0f, description, true, nullptr, y};
}
static constexpr Param rotate_45()
{
  return boolean("Rotate 45", false, "Turn the pattern by 45 degrees");
}

static const EnumPropertyItem rotation_4_items[] = {
    {0, "0", 0, N_("0"), N_("Left to right")},
    {1, "90", 0, N_("90"), N_("Bottom to top")},
    {2, "180", 0, N_("180"), N_("Right to left")},
    {3, "270", 0, N_("270"), N_("Top to bottom")},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem rotation_2_items[] = {
    {0, "HORIZONTAL", 0, N_("Horizontal"), N_("Changes from left to right")},
    {1, "VERTICAL", 0, N_("Vertical"), N_("Changes from bottom to top")},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem radial_items[] = {
    {0, "CONE", 0, N_("Cone"), N_("A straight fade")},
    {1, "HEMISPHERE", 0, N_("Hemisphere"), N_("A rounded fade")},
    {0, nullptr, 0, nullptr, nullptr},
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Recipes
 * \{ */

struct Sample {
  float2 uv;
  /* One pixel, in UV. */
  float pixel;
  uint32_t seed;
  /* The settings in slot order, a Vector2 takes two slots. */
  float p[12];
};

static int int_of(const float v)
{
  return int(std::round(v));
}

static bool bool_of(const float v)
{
  return v > 0.5f;
}

/* One where the signed distance is positive, with a one pixel soft edge. */
static float fill(const float distance, const float pixel)
{
  return math::clamp(distance / math::max(pixel, 1e-6f) + 0.5f, 0.0f, 1.0f);
}

static float fract(const float v)
{
  return v - std::floor(v);
}

static float2 fract(const float2 v)
{
  return v - math::floor(v);
}

/* The position in pattern cells, turned by 45 degrees when asked. Turning keeps it tiling. */
static float2 cells(const Sample &s, const int tiles, const bool turn)
{
  const float2 q = turn ? float2(s.uv.x + s.uv.y, s.uv.y - s.uv.x) : s.uv;
  return q * float(tiles);
}

static float cell_pixel(const Sample &s, const int tiles, const bool turn)
{
  return s.pixel * float(tiles) * (turn ? 1.41421f : 1.0f);
}

static float alveolus(const Sample &s)
{
  const int columns = math::max(1, int_of(s.p[0]));
  const bool gradient = bool_of(s.p[1]);
  const float gap = s.p[2];
  /* An even number of rows, as close to a regular hexagon grid as tiling allows. */
  const int rows = math::max(2, 2 * int_of(float(columns) / 1.7320508f));
  const float2 grid(columns, rows);
  const float2 q = s.uv * grid;
  const int2 base = int2(math::floor(q));
  float f1 = 1e9f;
  float f2 = 1e9f;
  for (int dy = -1; dy <= 1; dy++) {
    for (int dx = -2; dx <= 2; dx++) {
      const int2 c = base + int2(dx, dy);
      const int row_parity = ((c.y % 2) + 2) % 2;
      const float2 center(float(c.x) + 0.25f + 0.5f * float(row_parity), float(c.y) + 0.5f);
      /* Distances in column widths. */
      const float d = math::length((q - center) / grid * float(columns));
      if (d < f1) {
        f2 = f1;
        f1 = d;
      }
      else {
        f2 = math::min(f2, d);
      }
    }
  }
  if (gradient) {
    return std::sqrt(math::clamp((f2 - f1) / (f2 + f1), 0.0f, 1.0f));
  }
  return fill((f2 - f1) * 0.5f - gap * 0.25f, s.pixel * float(columns));
}

/* A brick with its mortar gap and soft edges, distances in brick heights. */
static float brick_profile(const float distance, const float gap, const float smoothness, const float pixel)
{
  const float d = distance - gap * 0.5f;
  if (smoothness <= 1e-3f) {
    return fill(d, pixel);
  }
  return smoothstep(0.0f, smoothness * 0.5f, d);
}

static float running_bond(const Sample &s, const int width_in_heights, const float row_shift)
{
  const int tiles = math::max(1, int_of(s.p[0]));
  const int rows = tiles * width_in_heights;
  float2 q = s.uv * float2(tiles, rows);
  const float row = std::floor(q.y);
  q.x += row * row_shift;
  const float2 f = fract(q);
  const float dx = math::min(f.x, 1.0f - f.x) * float(width_in_heights);
  const float dy = math::min(f.y, 1.0f - f.y);
  return brick_profile(math::min(dx, dy), s.p[2] * 0.5f, s.p[1], s.pixel * float(rows));
}

static float brick_1(const Sample &s)
{
  return running_bond(s, 2, 0.5f);
}

static float brick_2(const Sample &s)
{
  return running_bond(s, 4, 0.25f);
}

static float brick_generator(const Sample &s)
{
  const int columns = math::max(1, int_of(s.p[0]));
  const int pairs = math::max(1, (int_of(s.p[1]) + 1) / 2);
  const int rows = pairs * 2;
  const float bevel = s.p[2];
  const float gap = s.p[3];
  const float middle = math::clamp(s.p[4], 0.05f, 0.95f);
  const float height_random = s.p[5];
  const float slope_random = s.p[6];
  /* Rounded so the rows still meet at the top and bottom edges. */
  const float offset = std::round(s.p[7] * float(rows)) / float(rows);

  const float pair_position = s.uv.y * float(pairs);
  const float pair = std::floor(pair_position);
  const float t = pair_position - pair;
  int row;
  float fy;
  float row_height;
  if (t < middle) {
    row = int(pair) * 2;
    fy = t / middle;
    row_height = middle * 2.0f;
  }
  else {
    row = int(pair) * 2 + 1;
    fy = (t - middle) / (1.0f - middle);
    row_height = (1.0f - middle) * 2.0f;
  }
  const float x = s.uv.x * float(columns) + float(row) * offset * float(columns);
  const int column = int(std::floor(x));
  const float fx = x - float(column);

  /* Distances in brick heights of an even row. */
  const float dx = math::min(fx, 1.0f - fx) * float(rows) / float(columns);
  const float dy = math::min(fy, 1.0f - fy) * row_height;
  const float d = math::min(dx, dy) - gap * 0.5f;
  float v = bevel <= 1e-3f ? fill(d, s.pixel * float(rows)) : smoothstep(0.0f, bevel * 0.5f, d);

  const int2 id = tg::wrap_texel(int2(column, row), int2(columns, rows));
  const uint32_t h = tg::hash(id.x, id.y, s.seed);
  const float r1 = tg::hash_to_float(h);
  const float r2 = tg::hash_to_float(tg::hash(h));
  const float height = 1.0f - height_random * r1;
  const float slope = slope_random * (r2 - 0.5f) * 2.0f * (fx - 0.5f);
  return math::clamp(v * (height + slope), 0.0f, 1.0f);
}

static float checker_1(const Sample &s)
{
  const float2 q = cells(s, math::max(1, int_of(s.p[0])), bool_of(s.p[1]));
  return (int(std::floor(q.x)) + int(std::floor(q.y))) & 1 ? 1.0f : 0.0f;
}

/* One cell of a weave: threads on top run one way, threads below the other way and show
 * between them. Positions in the cell from 0 to 1. */
static float weave_cell(const float2 f,
                        const bool horizontal_on_top,
                        const int threads,
                        const float width,
                        const float roundness,
                        const float arch,
                        const float pixel)
{
  const auto layer = [&](const float along, const float across) {
    const float t = fract(across * float(threads));
    const float d = math::abs(t - 0.5f) * 2.0f;
    const float bar = fill(width - d, pixel * float(threads) * 2.0f);
    const float dome = std::sqrt(math::max(0.0f, 1.0f - math::min(d / math::max(width, 1e-3f), 1.0f) *
                                                            math::min(d / math::max(width, 1e-3f), 1.0f)));
    const float profile = math::interpolate(1.0f, dome, roundness);
    const float bend = 1.0f - arch + arch * math::sin(along * float(M_PI));
    return std::pair<float, float>(bar, bar * profile * bend);
  };
  const auto top = horizontal_on_top ? layer(f.x, f.y) : layer(f.y, f.x);
  const auto below = horizontal_on_top ? layer(f.y, f.x) : layer(f.x, f.y);
  return math::max(top.second, below.second * 0.5f * (1.0f - top.first));
}

static float weave_pattern(const Sample &s,
                           const int tiles,
                           const bool turn,
                           const int threads,
                           const float width,
                           const float roundness,
                           const float arch,
                           const float disorder = 0.0f)
{
  const float2 q = cells(s, tiles, turn);
  const int2 c = int2(math::floor(q));
  bool horizontal = ((c.x + c.y) & 1) == 0;
  if (disorder > 0.0f) {
    int2 id = tg::wrap_texel(c, int2(tiles * (turn ? 2 : 1)));
    if (turn && id.y >= tiles) {
      /* Turned cells repeat every (tiles, tiles) too, fold them onto one copy. */
      id = int2((id.x + tiles) % (tiles * 2), id.y - tiles);
    }
    if (tg::hash_to_float(tg::hash(id.x, id.y, s.seed)) < disorder * 0.5f) {
      horizontal = !horizontal;
    }
  }
  return weave_cell(fract(q), horizontal, threads, width, roundness, arch, cell_pixel(s, tiles, turn));
}

static float fibers_1(const Sample &s)
{
  return weave_pattern(s, math::max(1, int_of(s.p[0])), false, 3, 0.55f, 1.0f, 0.6f);
}

static float fibers_2(const Sample &s)
{
  return weave_pattern(s, math::max(1, int_of(s.p[0])), false, 2, 0.75f, 1.0f, 0.5f);
}

static float mesh_1(const Sample &s)
{
  return weave_pattern(s, math::max(1, int_of(s.p[0])), bool_of(s.p[1]), 1, 0.35f, 0.0f, 0.0f);
}

static float mesh_2(const Sample &s)
{
  return weave_pattern(s, math::max(1, int_of(s.p[0])), bool_of(s.p[1]), 1, 0.8f, 0.0f, 0.0f);
}

static float weave_1(const Sample &s)
{
  return weave_pattern(s, math::max(1, int_of(s.p[0])), bool_of(s.p[1]), 1, 0.85f, 1.0f, 0.7f);
}

static float weave_2(const Sample &s)
{
  return weave_pattern(
      s, math::max(1, int_of(s.p[0])), bool_of(s.p[2]), 1, 0.85f, 1.0f, 0.7f, s.p[1]);
}

static float weave_generator(const Sample &s)
{
  const int tiles_x = math::max(1, int_of(s.p[0]));
  const int tiles_y = math::max(1, int_of(s.p[1]));
  const float shape = s.p[2];
  const int threads = math::max(1, int_of(s.p[3]));
  const float2 gap(s.p[4], s.p[5]);
  /* Every tile is two by two blocks, the threads of each block run the other way from its
   * neighbors. */
  const float2 q = s.uv * float2(tiles_x * 2, tiles_y * 2);
  const int2 c = int2(math::floor(q));
  const float2 f = fract(q);
  const bool horizontal = ((c.x + c.y) & 1) == 0;
  const float across = horizontal ? f.y : f.x;
  const float along = horizontal ? f.x : f.y;
  const float t = fract(across * float(threads));
  const float d = math::abs(t - 0.5f) * 2.0f;
  const float width = 1.0f - (horizontal ? gap.y : gap.x);
  const float end_width = 1.0f - (horizontal ? gap.x : gap.y);
  const float pixel = s.pixel * float(math::max(tiles_x, tiles_y)) * 2.0f * float(threads);
  const float bar = fill(width - d, pixel) * fill(end_width - math::abs(along - 0.5f) * 2.0f, pixel);
  const float dome = std::sqrt(math::max(0.0f, 1.0f - (d / math::max(width, 1e-3f)) * (d / math::max(width, 1e-3f))));
  const float bend = math::sin(math::clamp(along, 0.0f, 1.0f) * float(M_PI));
  const float profile = std::pow(math::max(dome * (0.4f + 0.6f * bend), 0.0f), 0.3f + shape * 2.0f);
  return bar * profile;
}

static float gaussian_1(const Sample &s)
{
  const float2 f = fract(s.uv * float(math::max(1, int_of(s.p[0])))) - 0.5f;
  return std::exp(-math::length_squared(f) * 18.0f);
}

static float gaussian_2(const Sample &s)
{
  const float2 q = s.uv * float(math::max(1, int_of(s.p[0])));
  const float2 a = fract(q) - 0.5f;
  const float2 b = fract(q + 0.5f) - 0.5f;
  return math::max(std::exp(-math::length_squared(a) * 40.0f),
                   std::exp(-math::length_squared(b) * 40.0f) * 0.6f);
}

static float axial_t(const Sample &s)
{
  const float2 a(s.p[0], s.p[1]);
  const float2 b(s.p[2], s.p[3]);
  const float2 d = b - a;
  return math::dot(s.uv - a, d) / math::max(math::length_squared(d), 1e-8f);
}

static float gradient_axial(const Sample &s)
{
  return math::clamp(axial_t(s), 0.0f, 1.0f);
}

static float gradient_axial_reflected(const Sample &s)
{
  return math::clamp(1.0f - math::abs(axial_t(s) * 2.0f - 1.0f), 0.0f, 1.0f);
}

static float gradient_circular(const Sample &s)
{
  const float2 center(s.p[0], s.p[1]);
  const float2 seam = float2(s.p[2], s.p[3]) - center;
  const float2 d = s.uv - center;
  const float a = std::atan2(d.y, d.x) - std::atan2(seam.y, seam.x);
  return fract(a / float(M_PI * 2.0));
}

static float linear_position(const Sample &s, const int rotation)
{
  switch (rotation) {
    case 1:
      return s.uv.y;
    case 2:
      return 1.0f - s.uv.x;
    case 3:
      return 1.0f - s.uv.y;
    default:
      return s.uv.x;
  }
}

static float gradient_linear_1(const Sample &s)
{
  return fract(linear_position(s, int_of(s.p[1])) * float(math::max(1, int_of(s.p[0]))));
}

static float gradient_linear_2(const Sample &s)
{
  const float t = fract(linear_position(s, int_of(s.p[1])) * float(math::max(1, int_of(s.p[0]))));
  return math::sin(t * float(M_PI));
}

static float gradient_linear_3(const Sample &s)
{
  const float t = fract(linear_position(s, int_of(s.p[2])) * float(math::max(1, int_of(s.p[0]))));
  const float peak = math::clamp(s.p[1], 1e-3f, 1.0f - 1e-3f);
  return t < peak ? t / peak : (1.0f - t) / (1.0f - peak);
}

static float gradient_radial(const Sample &s)
{
  const float2 center(s.p[1], s.p[2]);
  const float radius = math::max(math::distance(float2(s.p[3], s.p[4]), center), 1e-6f);
  const float d = math::min(math::distance(s.uv, center) / radius, 1.0f);
  return int_of(s.p[0]) == 1 ? std::sqrt(1.0f - d * d) : 1.0f - d;
}

/* Position inside one tile, -1 to 1, turned and scaled. */
static float2 tile_local(const Sample &s, const int tiles, const float turns, const float scale)
{
  const float2 p = (fract(s.uv * float(tiles)) - 0.5f) * 2.0f;
  return tg::shapes::rotate(p, -turns * float(M_PI * 2.0)) / math::max(scale, 1e-4f);
}

static float polygon_1(const Sample &s)
{
  const int sides = math::clamp(int_of(s.p[0]), 3, 32);
  const float explode = s.p[1];
  const float triangle_size = math::max(s.p[2], 1e-3f);
  const float scale = s.p[3];
  const float rotation = s.p[4];
  const bool gradient = bool_of(s.p[5]);
  const bool invert = bool_of(s.p[6]);
  const int tiles = math::max(1, int_of(s.p[7]));
  /* Pushed apart slices need room, so the shape shrinks as they move out. */
  const float fit = 1.0f / (1.0f + explode * 0.3f);
  const float2 p = tile_local(s, tiles, rotation, scale * fit);
  const float pixel = s.pixel * float(tiles) * 2.0f / math::max(scale * fit, 1e-4f);

  const float step = float(M_PI * 2.0) / float(sides);
  const float a = std::atan2(p.y, p.x) - (float(M_PI) * 0.5f);
  const int k0 = int(std::floor(a / step));
  float best = 0.0f;
  for (int k = k0 - 1; k <= k0 + 1; k++) {
    const float a0 = float(k) * step + (float(M_PI) * 0.5f);
    const float2 v0(math::cos(a0), math::sin(a0));
    const float2 v1(math::cos(a0 + step), math::sin(a0 + step));
    const float2 middle = math::normalize(v0 + v1);
    const float2 centroid = (v0 + v1) / 3.0f;
    const float2 q = centroid + (p - middle * explode * 0.3f - centroid) / triangle_size;
    /* Signed distances to the three edges, positive inside. */
    const auto edge = [](const float2 from, const float2 to, const float2 x) {
      const float2 e = to - from;
      return (e.x * (x.y - from.y) - e.y * (x.x - from.x)) / math::max(math::length(e), 1e-6f);
    };
    const float d = math::min(math::min(edge(float2(0.0f), v0, q), edge(v0, v1, q)),
                              edge(v1, float2(0.0f), q));
    const float inside = fill(d, pixel / triangle_size);
    if (inside <= 0.0f) {
      continue;
    }
    float v = 1.0f;
    if (gradient) {
      const float out = math::clamp(math::dot(q, middle) / math::cos(step * 0.5f), 0.0f, 1.0f);
      v = invert ? out : 1.0f - out;
    }
    best = math::max(best, inside * v);
  }
  return best;
}

static float polygon_2(const Sample &s)
{
  const int sides = math::clamp(int_of(s.p[0]), 3, 32);
  const float2 p = tile_local(s, 1, s.p[2], s.p[1]);
  const float curve = s.p[3];
  const float gradient = s.p[4];
  const bool invert = bool_of(s.p[5]);
  const float step = float(M_PI * 2.0) / float(sides);
  const float apothem = math::cos(step * 0.5f);
  float d = -1e9f;
  for (int k = 0; k < sides; k++) {
    const float a = (float(k) + 0.5f) * step + (float(M_PI) * 0.5f);
    d = math::max(d, math::dot(p, float2(math::cos(a), math::sin(a))));
  }
  const float inside = fill(apothem - d, s.pixel * 2.0f / math::max(s.p[1], 1e-4f));
  float v = math::clamp(1.0f - d / apothem, 0.0f, 1.0f);
  v = std::pow(v, std::exp2(-curve * 2.0f));
  v = math::clamp(v / math::max(gradient, 1e-3f), 0.0f, 1.0f);
  return inside * (invert ? 1.0f - v : v);
}

static float shape(const Sample &s)
{
  const int tiles = math::max(1, int_of(s.p[0]));
  const Shape kind = Shape(math::clamp(int_of(s.p[1]), 1, 16));
  const float turns = s.p[4] + (bool_of(s.p[5]) ? 0.125f : 0.0f);
  float2 p = tile_local(s, tiles, turns, s.p[3]);
  p /= math::max(float2(s.p[6], s.p[7]), float2(1e-4f));
  return tg::shapes::shape_value(kind, p, s.p[2]);
}

static float star(const Sample &s)
{
  const float scale = s.p[0];
  const int points = math::clamp(int_of(s.p[1]), 3, 32);
  const float inner = s.p[2];
  const float2 p = tile_local(s, 1, 0.0f, scale);
  const float step = float(M_PI * 2.0) / float(points);
  /* Fold the point into the first half spike, which points straight up. */
  float a = std::atan2(p.x, p.y);
  a = math::abs(a - step * std::round(a / step));
  const float r = math::length(p);
  const float2 q(r * math::sin(a), r * math::cos(a));
  const float2 tip(0.0f, 1.0f);
  const float2 valley(inner * math::sin(step * 0.5f), inner * math::cos(step * 0.5f));
  const float2 e = valley - tip;
  const float d = (e.x * (q.y - tip.y) - e.y * (q.x - tip.x)) / math::length(e);
  return fill(-d, s.pixel * 2.0f / math::max(scale, 1e-4f));
}

static float starburst(const Sample &s)
{
  const int branches = math::clamp(int_of(s.p[0]), 3, 512);
  const float softness = s.p[1];
  const float randomness = s.p[2];
  const float size_min = s.p[3];
  const float size_max = s.p[4];
  const float variation = s.p[5];
  const int tiles = math::max(1, int_of(s.p[6]));
  const float2 p = tile_local(s, tiles, 0.0f, 1.0f);
  const float r = math::length(p);
  const float turn = fract(std::atan2(p.y, p.x) / float(M_PI * 2.0));
  const int k0 = int(std::floor(turn * float(branches)));
  const float half_width = (0.08f + softness * 0.42f) / float(branches);
  float best = 0.0f;
  for (int k = k0 - 2; k <= k0 + 2; k++) {
    const int id = ((k % branches) + branches) % branches;
    const uint32_t h = tg::hash(id, 0, s.seed);
    const float center = (float(k) + 0.5f + (tg::hash_to_float(h) - 0.5f) * randomness) /
                         float(branches);
    const float length = size_max *
                         (1.0f - variation * tg::hash_to_float(tg::hash(h)) *
                                     (1.0f - size_min / math::max(size_max, 1e-4f)));
    if (r >= length) {
      continue;
    }
    const float da = math::abs(turn - center);
    const float across = smoothstep(half_width, half_width * (1.0f - softness) * 0.5f, da);
    best = math::max(best, across * (1.0f - r / math::max(length, 1e-4f)));
  }
  return best;
}

static float stripes(const Sample &s)
{
  const int count = math::max(1, int_of(s.p[0]));
  const float width = s.p[1];
  const float softness = s.p[2];
  const int shift = int_of(s.p[3]);
  const float t = fract(s.uv.x * float(count) + s.uv.y * float(shift));
  const float d = math::abs(t - 0.5f) * 2.0f;
  const float pixel = s.pixel * float(count + math::abs(shift)) * 2.0f;
  const float soft = math::max(softness, pixel);
  return smoothstep(width + soft * 0.5f, width - soft * 0.5f, d);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Table
 * \{ */

using PatternFunction = float (*)(const Sample &s);

struct Recipe {
  /* The UI name. The node id is "TextureGraphNode" with the spaces taken out. */
  const char *name;
  const char *description;
  bool uses_seed;
  Param params[10];
  PatternFunction function;
};

static const Recipe recipes[] = {
    {"Alveolus",
     "Rounded cells in a honeycomb",
     false,
     {tiling(4),
      boolean("Gradient", true, "Soft rounded cells, off gives flat cells with gaps"),
      factor("Gap", 0.2f, "Width of the gaps between flat cells")},
     alveolus},
    {"Brick 1",
     "Bricks twice as wide as high",
     false,
     {tiling(4),
      factor("Edge Smoothness", 0.1f, "From hard to soft brick edges"),
      factor("Gap", 0.2f, "Width of the mortar between bricks")},
     brick_1},
    {"Brick 2",
     "Long thin bricks in a stepped bond",
     false,
     {tiling(4),
      factor("Edge Smoothness", 0.1f, "From hard to soft brick edges"),
      factor("Gap", 0.2f, "Width of the mortar between bricks")},
     brick_2},
    {"Brick Generator",
     "Bricks with full control over their layout and bevel",
     true,
     {integer("Bricks X", 4, 1, 64, "Bricks across the image"),
      integer("Bricks Y", 8, 1, 64, "Rows of bricks, rounded up to an even number"),
      factor("Bevel", 0.2f, "How soft the brick edges are"),
      factor("Gap", 0.1f, "Space between bricks"),
      factor("Middle Size", 0.5f, "Height of every other row"),
      factor("Height Random", 0.0f, "Random brightness per brick"),
      factor("Slope Random", 0.0f, "Random tilt per brick"),
      factor("Offset", 0.5f, "How far each row shifts from the one below")},
     brick_generator},
    {"Checker 1", "A black and white checkerboard", false, {tiling(2), rotate_45()}, checker_1},
    {"Fibers 1", "Thin strands woven over and under", false, {tiling(4)}, fibers_1},
    {"Fibers 2", "Thicker strands woven over and under", false, {tiling(4)}, fibers_2},
    {"Gaussian 1", "A grid of soft blobs", false, {tiling(4)}, gaussian_1},
    {"Gaussian 2", "Soft blobs in staggered rows", false, {tiling(4)}, gaussian_2},
    {"Gradient Axial",
     "A ramp from one point to another",
     false,
     {point("Point 1", 0.2f, 0.5f, "Where the ramp is black"),
      point("Point 2", 0.8f, 0.5f, "Where the ramp is white")},
     gradient_axial},
    {"Gradient Axial Reflected",
     "A ramp to white and back between two points",
     false,
     {point("Point 1", 0.2f, 0.5f, "Where the ramp starts black"),
      point("Point 2", 0.8f, 0.5f, "Where the ramp is black again")},
     gradient_axial_reflected},
    {"Gradient Circular",
     "A ramp that sweeps around a point",
     false,
     {point("Point 1", 0.5f, 0.5f, "The point the ramp turns around"),
      point("Point 2", 1.0f, 0.5f, "Where the ramp starts and ends")},
     gradient_circular},
    {"Gradient Linear 1",
     "A plain ramp from black to white",
     false,
     {tiling(1), menu("Rotation", 0, rotation_4_items, "Which way the ramp runs")},
     gradient_linear_1},
    {"Gradient Linear 2",
     "A rounded ramp up and down again, like a pipe",
     false,
     {tiling(1), menu("Rotation", 0, rotation_2_items, "Which way the ramp runs")},
     gradient_linear_2},
    {"Gradient Linear 3",
     "A straight ramp up and down again",
     false,
     {tiling(1),
      factor("Position", 0.5f, "Where the ramp peaks"),
      menu("Rotation", 0, rotation_2_items, "Which way the ramp runs")},
     gradient_linear_3},
    {"Gradient Radial",
     "A fade from a center out to a radius",
     false,
     {menu("Shape", 0, radial_items, "The fade's profile"),
      point("Center", 0.5f, 0.5f, "Where the fade is white"),
      point("Radius Point", 1.0f, 0.5f, "Where the fade reaches black")},
     gradient_radial},
    {"Mesh 1", "Thin bars woven over and under", false, {tiling(4), rotate_45()}, mesh_1},
    {"Mesh 2", "Wide blocks woven over and under", false, {tiling(4), rotate_45()}, mesh_2},
    {"Polygon 1",
     "A polygon made of slices that can be pulled apart",
     false,
     {integer("Sides", 6, 3, 32, "Number of sides"),
      factor("Explode", 0.0f, "Pushes the slices away from each other"),
      factor("Triangle Size", 1.0f, "Size of each slice, below one they come apart"),
      factor("Scale", 0.9f, "Size of the whole shape"),
      factor("Rotation", 0.0f, "Turn of the shape"),
      boolean("Gradient", false, "Fill the slices with a ramp"),
      boolean("Invert Gradient", false, "Turn the ramp around"),
      tiling(1)},
     polygon_1},
    {"Polygon 2",
     "A polygon filled with a ramp from the edges to the center",
     false,
     {integer("Sides", 6, 3, 32, "Number of sides"),
      factor("Scale", 0.9f, "Size of the shape"),
      factor("Rotation", 0.0f, "Turn of the shape"),
      number("Curve", 0.0f, -1.0f, 1.0f, "Rounder or sharper ramp"),
      factor("Gradient", 1.0f, "How far the ramp reaches in, zero gives a flat shape"),
      boolean("Invert Gradient", false, "Dark center, light edges")},
     polygon_2},
    {"Shape",
     "One of the built-in shapes, optionally repeated",
     false,
     {tiling(1),
      menu("Pattern", int(Shape::Disc), tg::shapes::builtin_shape_items, "The shape to draw"),
      factor("Pattern Specific", 0.0f, "Changes the shape a little, each shape in its own way"),
      factor("Scale", 1.0f, "Size of the shape"),
      factor("Angle", 0.0f, "Turn of the shape"),
      boolean("Rotate 45", false, "Turn the shape by 45 degrees"),
      point("Size", 1.0f, 1.0f, "Width and height of the shape")},
     shape},
    {"Star",
     "A star",
     false,
     {factor("Scale", 0.9f, "Size of the star"),
      integer("Points", 5, 3, 32, "Number of points"),
      factor("Inner Radius", 0.4f, "How deep the cuts between the points go")},
     star},
    {"Starburst",
     "Rays shooting out from the center",
     true,
     {integer("Branches", 24, 3, 512, "Number of rays"),
      factor("Softness", 0.3f, "How soft the rays are"),
      factor("Randomness", 0.3f, "How unevenly the rays are spread"),
      factor("Size Min", 0.5f, "Shortest ray"),
      number("Size Max", 1.0f, 0.0f, 2.0f, "Longest ray"),
      factor("Size Variation", 0.5f, "How much ray lengths vary"),
      tiling(1)},
     starburst},
    {"Stripes",
     "Parallel stripes that can lean",
     false,
     {integer("Stripes", 8, 1, 100, "Number of stripes"),
      factor("Width", 0.5f, "Thickness of the stripes"),
      factor("Softness", 0.0f, "From hard to soft stripe edges"),
      integer("Shift", 0, -20, 20, "How far the stripes lean, in whole stripes")},
     stripes},
    {"Weave 1", "Threads woven over and under", false, {tiling(4), rotate_45()}, weave_1},
    {"Weave 2",
     "A weave with stitches swapped at random",
     true,
     {tiling(4), factor("Disorder", 0.3f, "How many stitches are swapped"), rotate_45()},
     weave_2},
    {"Weave Generator",
     "A weave with control over its blocks and threads",
     false,
     {integer("Tile X", 4, 1, 20, "Blocks across"),
      integer("Tile Y", 4, 1, 20, "Blocks down"),
      factor("Shape", 0.3f, "Rounder or flatter threads"),
      integer("Weave", 2, 1, 10, "Threads in every block"),
      point("Gap", 0.1f, 0.1f, "Space between threads across and along")},
     weave_generator},
};

static constexpr int recipes_num = 27;
static_assert(sizeof(recipes) / sizeof(recipes[0]) == recipes_num);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Node
 * \{ */

static void declare_recipe(const Recipe &recipe, NodeDeclarationBuilder &b)
{
  b.add_output<decl::Float>("Image").structure_type(StructureType::Dynamic);

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
            .static_items(param.items)
            .description(param.description);
        break;
      case ParamKind::Bool:
        b.add_input<decl::Bool>(param.name)
            .default_value(param.default_value > 0.5f)
            .description(param.description);
        break;
      case ParamKind::Vector2:
        b.add_input<decl::Vector>(param.name)
            .subtype(PROP_FACTOR)
            .dimensions(2)
            .default_value({param.default_value, param.default_y})
            .min(param.min)
            .max(param.max)
            .description(param.description);
        break;
    }
  }
  if (recipe.uses_seed) {
    b.add_input<decl::Int>("Seed").default_value(0).description(
        "Change it to get a different pattern");
  }
  b.add_input<decl::Menu>("Resolution")
      .default_value(MenuValue(0))
      .static_items(tg::generator_size_items)
      .description("Size of the image this node makes");
}

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

class PatternOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Recipe &recipe = recipes[this->node().typeinfo - node_types];

    Sample sample;
    for (float &p : sample.p) {
      p = 0.0f;
    }
    sample.seed = recipe.uses_seed ?
                      uint32_t(this->get_input("Seed").get_single_value_default<int>()) :
                      0u;
    int slot = 0;
    for (const Param &param : recipe.params) {
      if (param.name == nullptr) {
        break;
      }
      const Result &input = this->get_input(param.name);
      switch (param.kind) {
        case ParamKind::Float:
          sample.p[slot++] = input.get_single_value_default<float>();
          break;
        case ParamKind::Int:
          sample.p[slot++] = float(math::clamp(
              input.get_single_value_default<int>(), int(param.min), int(param.max)));
          break;
        case ParamKind::Menu:
          sample.p[slot++] = float(input.get_single_value_default<MenuValue>().value);
          break;
        case ParamKind::Bool:
          sample.p[slot++] = input.get_single_value_default<bool>() ? 1.0f : 0.0f;
          break;
        case ParamKind::Vector2: {
          const float2 v = input.get_single_value_default<float2>();
          sample.p[slot++] = v.x;
          sample.p[slot++] = v.y;
          break;
        }
      }
    }

    const int resolution = this->get_input("Resolution").get_single_value_default<MenuValue>().value;
    const Domain domain = tg::generator_domain(this->context(), resolution);
    const int2 size = domain.data_size;
    sample.pixel = 1.0f / float(math::max(math::max(size.x, size.y), 1));

    Result &output = this->get_result("Image");
    output.allocate_texture(domain);
    parallel_for(size, [&](const int2 texel) {
      Sample s = sample;
      s.uv = (float2(texel) + 0.5f) / float2(size);
      const float v = recipe.function(s);
      output.store_pixel(texel, std::isfinite(v) ? math::clamp(v, 0.0f, 1.0f) : 0.0f);
    });
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new PatternOperation(context, node);
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

}  // namespace blender::nodes::node_texture_graph_pattern_cc
