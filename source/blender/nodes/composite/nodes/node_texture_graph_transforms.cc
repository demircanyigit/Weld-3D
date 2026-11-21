/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Texture Graph transforms: polar mapping, cropping, cloning, noise upscaling, tile-safe and
 * non-square transforms, skew, symmetry, trapezoid, and the two Make It Tile filters.
 */

#include "texture_graph_filter.hh"

namespace blender::nodes::node_texture_graph_transforms_cc {

namespace tg = texture_graph;
using namespace tg::filter;

/* Base for nodes that move the pixels of one color image. */
class MoveOperation : public FilterOperation {
 public:
  using FilterOperation::FilterOperation;

 protected:
  /* Fills the output with the image sampled where `fn(uv)` says, in 0 to 1. */
  template<typename Fn> void remap(const Fn &fn, const bool wrap = true)
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float4> image = this->colors("Image", size);
    Array<float4> out(image.size());
    each_pixel(size, [&](const int2 p, const int64_t index) {
      const float2 uv = fn(uv_of(p, size), p, index);
      out[index] = wrap ? sample_uv(image.as_span(), size, uv) :
                          sample_uv_clamped(image.as_span(), size, uv);
    });
    this->out_colors("Image", domain, out);
  }
};

static void image_io(NodeDeclarationBuilder &b, const char *description = "Image to move")
{
  color_out(b, "Image");
  color_in(b, "Image", float4(0.5f, 0.5f, 0.5f, 1.0f), description);
}

static float fract(const float x)
{
  return x - math::floor(x);
}

/* -------------------------------------------------------------------- */
/** \name Polar
 * \{ */

namespace polar {

static void declare_to_polar(NodeDeclarationBuilder &b)
{
  image_io(b, "Image to unroll around its middle");
  angle_in(b, "Rotation", 0.0f, "Where around the middle the unrolled image starts");
}

static void declare_to_cartesian(NodeDeclarationBuilder &b)
{
  image_io(b, "Image to roll up around the middle, its left edge meets its right edge");
  angle_in(b, "Rotation", 0.0f, "Turns the rolled up image");
}

class ToPolarOperation : public MoveOperation {
 public:
  using MoveOperation::MoveOperation;

  void execute() override
  {
    const float rotation = this->f("Rotation");
    this->remap([&](const float2 uv, int2, int64_t) {
      const float angle = uv.x * float(M_PI * 2.0) + rotation;
      return float2(0.5f) + float2(math::cos(angle), math::sin(angle)) * uv.y * 0.5f;
    });
  }
};

class ToCartesianOperation : public MoveOperation {
 public:
  using MoveOperation::MoveOperation;

  void execute() override
  {
    const float rotation = this->f("Rotation");
    this->remap(
        [&](const float2 uv, int2, int64_t) {
          const float2 d = uv - 0.5f;
          const float angle = std::atan2(d.y, d.x) - rotation;
          const float u = fract(angle / float(M_PI * 2.0));
          return float2(u, math::min(math::length(d) * 2.0f, 1.0f));
        },
        true);
  }
};

}  // namespace polar

/** \} */

/* -------------------------------------------------------------------- */
/** \name Auto Crop
 * \{ */

namespace auto_crop {

static const EnumPropertyItem source_items[] = {
    {0, "ALPHA", 0, N_("Alpha"), N_("Keep the pixels that are not see-through")},
    {1, "LUMINANCE", 0, N_("Luminance"), N_("Keep the pixels brighter than the threshold")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  image_io(b, "Image to crop to what it shows");
  menu_in(b, "Source", source_items, 0, "What tells the content from the background");
  factor_in(b, "Threshold", 0.01f, "Values above this count as content");
  factor_in(b, "Padding", 0.0f, "Room left around the content");
  bool_in(b, "Keep Aspect", true, "Scale the content the same in both directions");
}

class Operation : public FilterOperation {
 public:
  using FilterOperation::FilterOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float4> image = this->colors("Image", size);
    const bool alpha = this->m("Source") == 0;
    const float threshold = this->f("Threshold");
    int2 lo(size), hi(-1);
    for (int y = 0; y < size.y; y++) {
      for (int x = 0; x < size.x; x++) {
        const float4 c = image[int64_t(y) * size.x + x];
        const float v = alpha ? c.w : luminance(c.xyz());
        if (v > threshold) {
          lo = math::min(lo, int2(x, y));
          hi = math::max(hi, int2(x, y));
        }
      }
    }
    if (hi.x < 0) {
      this->out_colors("Image", domain, image);
      return;
    }
    float2 box_lo = float2(lo) / float2(size);
    float2 box_size = float2(hi - lo + 1) / float2(size);
    if (this->b("Keep Aspect")) {
      const float2 pixels = box_size * float2(size);
      const float side = math::max(pixels.x / size.x, pixels.y / size.y);
      const float2 grown = float2(side) - box_size;
      box_lo -= grown * 0.5f;
      box_size = float2(side);
    }
    const float padding = this->f("Padding");
    box_lo -= box_size * padding;
    box_size *= 1.0f + padding * 2.0f;
    Array<float4> out(image.size());
    each_pixel(size, [&](const int2 p, const int64_t index) {
      const float2 uv = box_lo + uv_of(p, size) * box_size;
      if (uv.x < 0.0f || uv.y < 0.0f || uv.x > 1.0f || uv.y > 1.0f) {
        out[index] = float4(0.0f);
        return;
      }
      out[index] = sample_uv_clamped(image.as_span(), size, uv);
    });
    this->out_colors("Image", domain, out);
  }
};

}  // namespace auto_crop

/** \} */

/* -------------------------------------------------------------------- */
/** \name Clone
 * \{ */

namespace clone {

static void node_declare(NodeDeclarationBuilder &b)
{
  image_io(b, "Image to copy a patch within");
  vector2_in(b, "Source", float2(0.3f), 0.0f, 1.0f, "Middle of the patch to copy");
  vector2_in(b, "Target", float2(0.7f), 0.0f, 1.0f, "Where the patch is placed");
  float_in(b, "Radius", 0.1f, 0.0f, 1.0f, "Size of the patch, next to the image width");
  factor_in(b, "Softness", 0.5f, "How softly the patch fades into the image");
}

class Operation : public FilterOperation {
 public:
  using FilterOperation::FilterOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float4> image = this->colors("Image", size);
    const float2 source = this->v("Source");
    const float2 target = this->v("Target");
    const float radius = math::max(this->f("Radius"), 1e-4f);
    const float softness = this->f("Softness");
    const float aspect = float(size.x) / float(math::max(size.y, 1));
    Array<float4> out(image.size());
    each_pixel(size, [&](const int2 p, const int64_t index) {
      const float2 uv = uv_of(p, size);
      float2 d = tg::wrapped_delta(uv - target, float2(1.0f));
      const float2 from = source + d;
      d.x *= aspect;
      const float r = math::length(d) / radius;
      const float w = 1.0f - smooth01(1.0f - softness, 1.0f, r);
      out[index] = w > 0.0f ? math::interpolate(image[index],
                                                sample_uv(image.as_span(), size, from), w) :
                              image[index];
    });
    this->out_colors("Image", domain, out);
  }
};

}  // namespace clone

/** \} */

/* -------------------------------------------------------------------- */
/** \name Noise Upscale
 * \{ */

namespace upscale {

static void node_declare(NodeDeclarationBuilder &b)
{
  image_io(b, "A tiling noise to make finer");
  int_in(b, "Scale", 2, 2, 8, "How many times finer the noise gets");
  seed_in(b);
}

/* Layers of the image, each repeated `scale` times, turned by quarter turns, moved and maybe
 * mirrored; they are averaged and their contrast restored so the repeats hide each other. */
template<int layers, bool mirror> class Operation : public FilterOperation {
 public:
  using FilterOperation::FilterOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float4> image = this->colors("Image", size);
    const float scale = float(math::max(this->i("Scale"), 1));
    const uint32_t seed = this->seed();
    float4 mean(0.0f);
    for (const float4 &c : image) {
      mean += c;
    }
    mean /= float(math::max(int64_t(1), image.size()));
    float2 offsets[layers];
    int turns[layers];
    bool flips[layers];
    for (int l = 0; l < layers; l++) {
      const uint32_t h = tg::hash(l, 17, seed);
      offsets[l] = float2(tg::hash_to_float(h), tg::hash_to_float(tg::hash(h)));
      turns[l] = int(tg::hash(h ^ 0x9e37u) % 4u);
      flips[l] = mirror && (tg::hash(h ^ 0x51u) & 1u);
    }
    const float boost = math::sqrt(float(layers));
    Array<float4> out(image.size());
    each_pixel(size, [&](const int2 p, const int64_t index) {
      const float2 uv = uv_of(p, size) * scale;
      float4 sum(0.0f);
      for (int l = 0; l < layers; l++) {
        float2 q = uv;
        for (int t = 0; t < turns[l]; t++) {
          q = float2(-q.y, q.x);
        }
        if (flips[l]) {
          q.x = -q.x;
        }
        sum += sample_uv(image.as_span(), size, q + offsets[l]);
      }
      const float4 average = sum / float(layers);
      out[index] = math::clamp(mean + (average - mean) * boost, float4(0.0f), float4(1.0f));
    });
    this->out_colors("Image", domain, out);
  }
};

}  // namespace upscale

/** \} */

/* -------------------------------------------------------------------- */
/** \name Safe, Non-Square, Non-Uniform Rotation
 * \{ */

namespace transform {

static void declare_safe(NodeDeclarationBuilder &b)
{
  image_io(b, "Tiling image to move, it keeps tiling");
  int_in(b, "Tile", 1, 1, 32, "Number of repeats across the image");
  float_in(b, "Rotation", 0.0f, -1.0f, 1.0f,
           "Turn in turns, snapped to the nearest angle that keeps the image tiling");
  vector2_in(b, "Offset", float2(0.0f), -1.0f, 1.0f, "Slides the image");
  bool_in(b, "Mirror X", false, "Flip left and right");
  bool_in(b, "Mirror Y", false, "Flip up and down");
}

static void declare_non_square(NodeDeclarationBuilder &b)
{
  image_io(b);
  int_in(b, "Tiling X", 1, 1, 64, "Repeats across");
  int_in(b, "Tiling Y", 1, 1, 64, "Repeats up and down");
  angle_in(b, "Rotation", 0.0f, "Turns the image");
  vector2_in(b, "Offset", float2(0.0f), -1.0f, 1.0f, "Slides the image");
}

static void declare_non_uniform_rotation(NodeDeclarationBuilder &b)
{
  image_io(b, "Image to turn");
  value_in(b, "Rotation Map", 1.0f, "How far each pixel turns, white turns the most");
  angle_in(b, "Rotation", float(M_PI) * 0.25f, "Turn at white in the rotation map");
  vector2_in(b, "Center", float2(0.5f), 0.0f, 1.0f, "Point the image turns around");
}

class SafeOperation : public MoveOperation {
 public:
  using MoveOperation::MoveOperation;

  void execute() override
  {
    const float tile = float(math::max(this->i("Tile"), 1));
    const float turns = this->f("Rotation");
    const float2 offset = this->v("Offset");
    const bool mirror_x = this->b("Mirror X");
    const bool mirror_y = this->b("Mirror Y");
    /* Whole number steps keep the image tiling: find the one closest to the angle. */
    const float angle = turns * float(M_PI * 2.0);
    int2 best(1, 0);
    float best_error = 1e9f;
    for (int a = -3; a <= 3; a++) {
      for (int b = -3; b <= 3; b++) {
        if (a == 0 && b == 0) {
          continue;
        }
        float error = math::abs(std::remainder(std::atan2(float(b), float(a)) - angle,
                                               float(M_PI * 2.0)));
        /* Shorter steps first, they repeat less. */
        error += float(a * a + b * b) * 1e-4f;
        if (error < best_error) {
          best_error = error;
          best = int2(a, b);
        }
      }
    }
    this->remap([&](float2 uv, int2, int64_t) {
      uv -= offset;
      if (mirror_x) {
        uv.x = 1.0f - uv.x;
      }
      if (mirror_y) {
        uv.y = 1.0f - uv.y;
      }
      const float2 c = uv - 0.5f;
      const float2 r(c.x * float(best.x) + c.y * float(best.y),
                     -c.x * float(best.y) + c.y * float(best.x));
      return r * tile + 0.5f;
    });
  }
};

class NonSquareOperation : public MoveOperation {
 public:
  using MoveOperation::MoveOperation;

  void execute() override
  {
    const float2 tiling(float(math::max(this->i("Tiling X"), 1)),
                        float(math::max(this->i("Tiling Y"), 1)));
    const float rotation = this->f("Rotation");
    const float2 offset = this->v("Offset");
    Domain domain = this->compute_domain();
    const float2 size = float2(math::max(domain.data_size, int2(1)));
    const float aspect = size.x / size.y;
    this->remap([&](const float2 uv, int2, int64_t) {
      float2 c = uv - 0.5f - offset;
      c.x *= aspect;
      c = rotate2(c, -rotation);
      c.x /= aspect;
      return c * tiling + 0.5f;
    });
  }
};

class NonUniformRotationOperation : public FilterOperation {
 public:
  using FilterOperation::FilterOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float4> image = this->colors("Image", size);
    const Array<float> map = this->values("Rotation Map", size);
    const float rotation = this->f("Rotation");
    const float2 center = this->v("Center");
    const float aspect = float(size.x) / float(math::max(size.y, 1));
    Array<float4> out(image.size());
    each_pixel(size, [&](const int2 p, const int64_t index) {
      float2 d = uv_of(p, size) - center;
      d.x *= aspect;
      d = rotate2(d, -map[index] * rotation);
      d.x /= aspect;
      out[index] = sample_uv(image.as_span(), size, center + d);
    });
    this->out_colors("Image", domain, out);
  }
};

}  // namespace transform

/** \} */

/* -------------------------------------------------------------------- */
/** \name Skew, Symmetry, Symmetry Slice, Trapezoid
 * \{ */

namespace shape {

static const EnumPropertyItem axis_items[] = {
    {0, "HORIZONTAL", 0, N_("Horizontal"), N_("Rows slide sideways")},
    {1, "VERTICAL", 0, N_("Vertical"), N_("Columns slide up and down")},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem symmetry_items[] = {
    {0, "HORIZONTAL", 0, N_("Left to Right"), N_("The left half is mirrored onto the right")},
    {1, "VERTICAL", 0, N_("Bottom to Top"), N_("The bottom half is mirrored onto the top")},
    {2, "BOTH", 0, N_("Both"), N_("The bottom left quarter is mirrored onto the rest")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void declare_skew(NodeDeclarationBuilder &b)
{
  image_io(b, "Image to slant");
  menu_in(b, "Axis", axis_items, 0, "Which way the image slants");
  float_in(b, "Amount", 0.5f, -4.0f, 4.0f, "How far the far edge slides, next to the image size");
  bool_in(b, "Tile Safe", true, "Round the amount to whole steps so the image keeps tiling");
}

static void declare_symmetry(NodeDeclarationBuilder &b)
{
  image_io(b, "Image to mirror");
  menu_in(b, "Mode", symmetry_items, 0, "Which part is mirrored onto which");
  bool_in(b, "Flip Side", false, "Keep the other half instead");
  factor_in(b, "Position", 0.5f, "Where the mirror line is");
}

static void declare_slice(NodeDeclarationBuilder &b)
{
  image_io(b, "Image to cut into slices around the middle");
  int_in(b, "Slices", 6, 2, 64, "Number of slices around the middle");
  angle_in(b, "Angle", 0.0f, "Turns the slices");
  bool_in(b, "Mirror", true, "Every other slice is mirrored, so the slices meet without seams");
}

static void declare_trapezoid(NodeDeclarationBuilder &b)
{
  image_io(b, "Image to narrow at one end");
  float_in(b, "Top Stretch", 0.5f, 0.0f, 8.0f, "Width of the top, 1 keeps it as it is");
  float_in(b, "Bottom Stretch", 1.0f, 0.0f, 8.0f, "Width of the bottom, 1 keeps it as it is");
  bool_in(b, "Tile", false, "Repeat the image beside itself instead of showing black");
}

class SkewOperation : public MoveOperation {
 public:
  using MoveOperation::MoveOperation;

  void execute() override
  {
    const bool vertical = this->m("Axis") == 1;
    float amount = this->f("Amount");
    if (this->b("Tile Safe")) {
      amount = math::round(amount);
    }
    this->remap([&](float2 uv, int2, int64_t) {
      if (vertical) {
        uv.y -= amount * (uv.x - 0.5f);
      }
      else {
        uv.x -= amount * (uv.y - 0.5f);
      }
      return uv;
    });
  }
};

class SymmetryOperation : public MoveOperation {
 public:
  using MoveOperation::MoveOperation;

  void execute() override
  {
    const int mode = this->m("Mode");
    const bool flip = this->b("Flip Side");
    const float position = this->f("Position");
    const auto fold = [&](const float x) {
      if (flip ? x < position : x > position) {
        return 2.0f * position - x;
      }
      return x;
    };
    this->remap([&](float2 uv, int2, int64_t) {
      if (mode == 0 || mode == 2) {
        uv.x = fold(uv.x);
      }
      if (mode == 1 || mode == 2) {
        uv.y = fold(uv.y);
      }
      return uv;
    });
  }
};

class SliceOperation : public MoveOperation {
 public:
  using MoveOperation::MoveOperation;

  void execute() override
  {
    const int slices = math::max(this->i("Slices"), 1);
    const float angle = this->f("Angle");
    const bool mirror = this->b("Mirror");
    const float width = float(M_PI * 2.0) / float(slices);
    this->remap([&](const float2 uv, int2, int64_t) {
      const float2 d = uv - 0.5f;
      float a = std::atan2(d.y, d.x) - angle;
      a -= math::floor(a / width) * width;
      if (mirror) {
        const int index = int(math::floor((std::atan2(d.y, d.x) - angle) / width));
        if (index % 2 != 0) {
          a = width - a;
        }
      }
      a += angle;
      return float2(0.5f) + float2(math::cos(a), math::sin(a)) * math::length(d);
    });
  }
};

class TrapezoidOperation : public FilterOperation {
 public:
  using FilterOperation::FilterOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float4> image = this->colors("Image", size);
    const float top = this->f("Top Stretch");
    const float bottom = this->f("Bottom Stretch");
    const bool tile = this->b("Tile");
    Array<float4> out(image.size());
    each_pixel(size, [&](const int2 p, const int64_t index) {
      const float2 uv = uv_of(p, size);
      const float stretch = math::interpolate(bottom, top, uv.y);
      if (stretch <= 1e-5f) {
        out[index] = float4(0.0f);
        return;
      }
      const float u = 0.5f + (uv.x - 0.5f) / stretch;
      if (!tile && (u < 0.0f || u > 1.0f)) {
        out[index] = float4(0.0f);
        return;
      }
      out[index] = sample_uv(image.as_span(), size, float2(u, uv.y));
    });
    this->out_colors("Image", domain, out);
  }
};

}  // namespace shape

/** \} */

/* -------------------------------------------------------------------- */
/** \name Make It Tile
 * \{ */

namespace make_tile {

static void declare_patch(NodeDeclarationBuilder &b)
{
  color_out(b, "Image", "A tiling image made of soft patches of the input");
  color_in(b, "Image", float4(0.5f, 0.5f, 0.5f, 1.0f), "Image that does not tile");
  int_in(b, "Patches", 4, 1, 32, "Number of patches across the image");
  float_in(b, "Patch Size", 1.0f, 0.5f, 3.0f, "Size of each patch next to the space it has");
  factor_in(b, "Softness", 0.5f, "How softly the patches fade into each other");
  factor_in(b, "Rotation Random", 0.0f, "Turns each patch by a random amount");
  factor_in(b, "Source Area", 0.8f, "How much of the input the patches are taken from");
  seed_in(b);
}

static void declare_photo(NodeDeclarationBuilder &b)
{
  color_out(b, "Image", "The image blended with a half-shifted copy of itself, so it tiles");
  color_in(b, "Image", float4(0.5f, 0.5f, 0.5f, 1.0f), "Image that does not tile");
  factor_in(b, "Blend Width X", 0.25f, "Width of the blend across the left and right edges");
  factor_in(b, "Blend Width Y", 0.25f, "Width of the blend across the top and bottom edges");
}

class PatchOperation : public FilterOperation {
 public:
  using FilterOperation::FilterOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float4> image = this->colors("Image", size);
    const int patches = math::max(this->i("Patches"), 1);
    const float patch_size = this->f("Patch Size");
    const float softness = math::max(this->f("Softness"), 0.02f);
    const float rotation = this->f("Rotation Random");
    const float area = math::clamp(this->f("Source Area"), 0.05f, 1.0f);
    const uint32_t seed = this->seed();
    const float cell = 1.0f / float(patches);
    const float reach = cell * patch_size * 0.75f;
    const int span = int(math::ceil(patch_size)) + 1;
    Array<float4> out(image.size());
    each_pixel(size, [&](const int2 p, const int64_t index) {
      const float2 uv = uv_of(p, size);
      const int2 base = int2(math::floor(uv / cell));
      float4 sum(0.0f);
      float weight = 0.0f;
      float4 nearest(0.0f);
      float nearest_d = 1e9f;
      for (int dy = -span; dy <= span; dy++) {
        for (int dx = -span; dx <= span; dx++) {
          const int2 id = base + int2(dx, dy);
          const int2 w = tg::wrap_texel(id, int2(patches));
          const uint32_t h = tg::hash(w.x, w.y, seed);
          const float r1 = tg::hash_to_float(h);
          const float r2 = tg::hash_to_float(tg::hash(h ^ 1u));
          const float r3 = tg::hash_to_float(tg::hash(h ^ 2u));
          const float r4 = tg::hash_to_float(tg::hash(h ^ 3u));
          const float r5 = tg::hash_to_float(tg::hash(h ^ 4u));
          const float2 center = (float2(id) + 0.5f + (float2(r1, r2) - 0.5f) * 0.6f) * cell;
          const float2 d = uv - center;
          const float dist = math::length(d) / reach;
          const float2 source = 0.5f + (float2(r3, r4) - 0.5f) * (area - reach * 2.0f);
          const float4 color = sample_uv_clamped(
              image.as_span(), size, source + rotate2(d, (r5 - 0.5f) * rotation * float(M_PI * 2.0)));
          if (dist < nearest_d) {
            nearest_d = dist;
            nearest = color;
          }
          const float wgt = 1.0f - smooth01(1.0f - softness, 1.0f, dist);
          if (wgt > 0.0f) {
            sum += color * wgt;
            weight += wgt;
          }
        }
      }
      out[index] = weight > 1e-4f ? sum / weight : nearest;
    });
    this->out_colors("Image", domain, out);
  }
};

class PhotoOperation : public FilterOperation {
 public:
  using FilterOperation::FilterOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float4> image = this->colors("Image", size);
    const float2 blend = math::max(float2(this->f("Blend Width X"), this->f("Blend Width Y")),
                                   float2(1e-3f));
    Array<float4> out(image.size());
    each_pixel(size, [&](const int2 p, const int64_t index) {
      const float2 uv = uv_of(p, size);
      /* 1 in the middle of the image, 0 at its edges where it does not tile. */
      const float2 edge = math::min(uv, 1.0f - uv);
      const float wx = smooth01(0.0f, blend.x, edge.x);
      const float wy = smooth01(0.0f, blend.y, edge.y);
      const float4 shifted_x = sample_uv(image.as_span(), size, uv + float2(0.5f, 0.0f));
      const float4 shifted_y = sample_uv(image.as_span(), size, uv + float2(0.0f, 0.5f));
      const float4 shifted_xy = sample_uv(image.as_span(), size, uv + float2(0.5f));
      const float4 row = math::interpolate(shifted_x, image[index], wx);
      const float4 row_shifted = math::interpolate(shifted_xy, shifted_y, wx);
      out[index] = math::interpolate(row_shifted, row, wy);
    });
    this->out_colors("Image", domain, out);
  }
};

}  // namespace make_tile

/** \} */

static void node_register()
{
  static bke::bNodeType types[17];
  int k = 0;
  const short nclass = NODE_CLASS_DISTORT;
  register_filter(types[k++], "TextureGraphNodeCartesianToPolar", "Cartesian to Polar",
                  "Unroll an image around its middle into rows", "TEXTURE_GRAPH_CARTESIAN_TO_POLAR",
                  nclass, polar::declare_to_polar, make_operation<polar::ToPolarOperation>);
  register_filter(types[k++], "TextureGraphNodePolarToCartesian", "Polar to Cartesian",
                  "Roll an image up around the middle", "TEXTURE_GRAPH_POLAR_TO_CARTESIAN", nclass,
                  polar::declare_to_cartesian, make_operation<polar::ToCartesianOperation>);
  register_filter(types[k++], "TextureGraphNodeAutoCrop", "Auto Crop",
                  "Crop an image to what it shows and fill the frame with it",
                  "TEXTURE_GRAPH_AUTO_CROP", nclass, auto_crop::node_declare,
                  make_operation<auto_crop::Operation>);
  register_filter(types[k++], "TextureGraphNodeClone", "Clone",
                  "Copy a round patch of an image to another place", "TEXTURE_GRAPH_CLONE", nclass,
                  clone::node_declare, make_operation<clone::Operation>);
  register_filter(types[k++], "TextureGraphNodeNoiseUpscale1", "Noise Upscale 1",
                  "Make a tiling noise finer from two turned copies", "TEXTURE_GRAPH_NOISE_UPSCALE_1",
                  nclass, upscale::node_declare, make_operation<upscale::Operation<2, false>>);
  register_filter(types[k++], "TextureGraphNodeNoiseUpscale2", "Noise Upscale 2",
                  "Make a tiling noise finer from three turned copies",
                  "TEXTURE_GRAPH_NOISE_UPSCALE_2", nclass, upscale::node_declare,
                  make_operation<upscale::Operation<3, false>>);
  register_filter(types[k++], "TextureGraphNodeNoiseUpscale3", "Noise Upscale 3",
                  "Make a tiling noise finer from four turned and mirrored copies",
                  "TEXTURE_GRAPH_NOISE_UPSCALE_3", nclass, upscale::node_declare,
                  make_operation<upscale::Operation<4, true>>);
  register_filter(types[k++], "TextureGraphNodeSafeTransform", "Safe Transform",
                  "Repeat, turn and slide a tiling image so it keeps tiling",
                  "TEXTURE_GRAPH_SAFE_TRANSFORM", nclass, transform::declare_safe,
                  make_operation<transform::SafeOperation>);
  register_filter(types[k++], "TextureGraphNodeNonSquareTransform", "Non-Square Transform",
                  "Repeat, turn and slide an image that is not square",
                  "TEXTURE_GRAPH_NON_SQUARE_TRANSFORM", nclass, transform::declare_non_square,
                  make_operation<transform::NonSquareOperation>);
  register_filter(types[k++], "TextureGraphNodeNonUniformRotation", "Non-Uniform Rotation",
                  "Turn each pixel of an image by its own amount from a map",
                  "TEXTURE_GRAPH_NON_UNIFORM_ROTATION", nclass,
                  transform::declare_non_uniform_rotation,
                  make_operation<transform::NonUniformRotationOperation>);
  register_filter(types[k++], "TextureGraphNodeSkew", "Skew", "Slant an image sideways or up",
                  "TEXTURE_GRAPH_SKEW", nclass, shape::declare_skew,
                  make_operation<shape::SkewOperation>);
  register_filter(types[k++], "TextureGraphNodeSymmetry", "Symmetry",
                  "Mirror one half of an image onto the other", "TEXTURE_GRAPH_SYMMETRY", nclass,
                  shape::declare_symmetry, make_operation<shape::SymmetryOperation>);
  register_filter(types[k++], "TextureGraphNodeSymmetrySlice", "Symmetry Slice",
                  "Repeat one slice of an image around its middle",
                  "TEXTURE_GRAPH_SYMMETRY_SLICE", nclass, shape::declare_slice,
                  make_operation<shape::SliceOperation>);
  register_filter(types[k++], "TextureGraphNodeTrapezoidTransform", "Trapezoid Transform",
                  "Narrow an image toward its top or bottom",
                  "TEXTURE_GRAPH_TRAPEZOID_TRANSFORM", nclass, shape::declare_trapezoid,
                  make_operation<shape::TrapezoidOperation>);
  register_filter(types[k++], "TextureGraphNodeMakeItTilePatch", "Make It Tile Patch",
                  "Make a tiling image from soft patches of one that does not tile",
                  "TEXTURE_GRAPH_MAKE_IT_TILE_PATCH", nclass, make_tile::declare_patch,
                  make_operation<make_tile::PatchOperation>);
  register_filter(types[k++], "TextureGraphNodeMakeItTilePhoto", "Make It Tile Photo",
                  "Make a photo tile by blending its edges with a shifted copy",
                  "TEXTURE_GRAPH_MAKE_IT_TILE_PHOTO", nclass, make_tile::declare_photo,
                  make_operation<make_tile::PhotoOperation>);
  BLI_assert(k == 16);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_transforms_cc
