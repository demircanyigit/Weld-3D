/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Texture Graph normal map filters: making normal maps from heights, putting normal maps
 * together and apart, turning them, and turning them back into heights.
 */

#include "texture_graph_filter.hh"

namespace blender::nodes::node_texture_graph_normal_map_cc {

namespace tg = texture_graph;
using namespace tg::filter;

/* Base for nodes that read normal maps. */
class NormalOperation : public FilterOperation {
 public:
  using FilterOperation::FilterOperation;

 protected:
  Array<float3> normals(const char *name, const int2 size)
  {
    const Array<float4> colors = this->colors(name, size);
    const bool directx = this->directx();
    Array<float3> n(colors.size());
    threading::parallel_for(colors.index_range(), 4096, [&](const IndexRange range) {
      for (const int64_t k : range) {
        n[k] = decode_normal(colors[k], directx);
      }
    });
    return n;
  }

  void out_normals(const char *name, const Domain &domain, const Span<float3> n)
  {
    if (!this->wants(name)) {
      return;
    }
    const bool directx = this->directx();
    Array<float4> colors(n.size());
    threading::parallel_for(n.index_range(), 4096, [&](const IndexRange range) {
      for (const int64_t k : range) {
        colors[k] = encode_normal(n[k], directx);
      }
    });
    this->out_colors(name, domain, colors);
  }
};

/* -------------------------------------------------------------------- */
/** \name Facing Normal
 * \{ */

namespace facing {

static void node_declare(NodeDeclarationBuilder &b)
{
  value_out(b, "Facing", "White where the surface faces straight out, darker as it turns away");
  normal_in(b);
  normal_format_in(b);
}

class Operation : public NormalOperation {
 public:
  using NormalOperation::NormalOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float3> n = this->normals("Normal", size);
    Array<float> out(n.size());
    for (const int64_t k : n.index_range()) {
      out[k] = math::clamp(n[k].z, 0.0f, 1.0f);
    }
    this->out_values("Facing", domain, out);
  }
};

}  // namespace facing

/** \} */

/* -------------------------------------------------------------------- */
/** \name Normals from heights
 * \{ */

namespace from_height {

static void declare_blender(NodeDeclarationBuilder &b)
{
  color_out(b, "Normal", "The normal map with the height's bumps added");
  normal_in(b, "Normal", "Normal map to add the height to");
  value_in(b, "Height", 0.5f, "Height whose bumps are added");
  float_in(b, "Intensity", 1.0f, 0.0f, 32.0f, "How strong the height's bumps are");
  normal_format_in(b);
}

static void declare_world_units(NodeDeclarationBuilder &b)
{
  color_out(b, "Normal", "Normal map of the height at its real size");
  value_in(b, "Height", 0.5f, "Height to turn into a normal map");
  float_in(b, "Surface Size", 100.0f, 0.01f, 100000.0f, "Width of the texture in centimeters");
  float_in(b, "Height Depth", 2.0f, 0.0f, 10000.0f, "Height of white in centimeters");
  normal_format_in(b);
}

static void declare_sobel(NodeDeclarationBuilder &b)
{
  color_out(b, "Normal", "Normal map of the height");
  value_in(b, "Height", 0.5f, "Height to turn into a normal map");
  float_in(b, "Intensity", 1.0f, 0.0f, 256.0f, "Strength of the normal map");
  normal_format_in(b);
}

class BlenderOperation : public NormalOperation {
 public:
  using NormalOperation::NormalOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float3> base = this->normals("Normal", size);
    const Array<float> height = this->values("Height", size);
    const float scale = this->f("Intensity") * float(math::max(size.x, size.y)) / 64.0f;
    Array<float3> out(base.size());
    each_pixel(size, [&](const int2 p, const int64_t index) {
      out[index] = rotate_from_up(base[index], height_normal(height, size, p, scale));
    });
    this->out_normals("Normal", domain, out);
  }
};

class WorldUnitsOperation : public NormalOperation {
 public:
  using NormalOperation::NormalOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float> height = this->values("Height", size);
    const float pixel_cm = math::max(this->f("Surface Size"), 1e-4f) /
                           float(math::max(size.x, size.y));
    const float scale = this->f("Height Depth") / pixel_cm;
    Array<float3> out(height.size());
    each_pixel(size, [&](const int2 p, const int64_t index) {
      out[index] = height_normal(height, size, p, scale);
    });
    this->out_normals("Normal", domain, out);
  }
};

class SobelOperation : public NormalOperation {
 public:
  using NormalOperation::NormalOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float> height = this->values("Height", size);
    const float scale = this->f("Intensity") * float(math::max(size.x, size.y)) / 64.0f;
    Array<float3> out(height.size());
    each_pixel(size, [&](const int2 p, const int64_t index) {
      out[index] = height_normal(height, size, p, scale);
    });
    this->out_normals("Normal", domain, out);
  }
};

}  // namespace from_height

/** \} */

/* -------------------------------------------------------------------- */
/** \name Blend, Combine, Uncombine
 * \{ */

namespace combine {

static void declare_blend(NodeDeclarationBuilder &b)
{
  color_out(b, "Normal");
  normal_in(b, "Foreground", "Normal map laid over the background");
  normal_in(b, "Background", "Normal map underneath");
  value_in(b, "Mask", 1.0f, "Where the foreground shows");
  factor_in(b, "Opacity", 1.0f, "How much of the foreground shows");
  normal_format_in(b);
}

static void declare_combine(NodeDeclarationBuilder &b)
{
  color_out(b, "Normal", "The detail laid on the bends of the base");
  normal_in(b, "Base", "Large shapes");
  normal_in(b, "Detail", "Small bumps that follow the base");
  normal_format_in(b);
}

static void declare_uncombine(NodeDeclarationBuilder &b)
{
  color_out(b, "Detail", "What is left after taking the base out");
  normal_in(b, "Normal", "A combined normal map");
  normal_in(b, "Base", "The base it was combined from");
  normal_format_in(b);
}

class BlendOperation : public NormalOperation {
 public:
  using NormalOperation::NormalOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float3> fg = this->normals("Foreground", size);
    const Array<float3> bg = this->normals("Background", size);
    const Array<float> mask = this->values("Mask", size);
    const float opacity = this->f("Opacity");
    Array<float3> out(fg.size());
    for (const int64_t k : fg.index_range()) {
      out[k] = math::normalize(math::interpolate(bg[k], fg[k], math::clamp(mask[k] * opacity, 0.0f, 1.0f)) +
                               float3(0.0f, 0.0f, 1e-6f));
    }
    this->out_normals("Normal", domain, out);
  }
};

template<bool undo> class CombineOperation : public NormalOperation {
 public:
  using NormalOperation::NormalOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float3> base = this->normals("Base", size);
    const Array<float3> other = this->normals(undo ? "Normal" : "Detail", size);
    Array<float3> out(base.size());
    for (const int64_t k : base.index_range()) {
      out[k] = undo ? rotate_to_up(base[k], other[k]) : rotate_from_up(base[k], other[k]);
    }
    this->out_normals(undo ? "Detail" : "Normal", domain, out);
  }
};

}  // namespace combine

/** \} */

/* -------------------------------------------------------------------- */
/** \name Invert, Normalize, Transform, Vector Rotation
 * \{ */

namespace edit {

static void declare_invert(NodeDeclarationBuilder &b)
{
  color_out(b, "Normal");
  normal_in(b);
  bool_in(b, "Invert Red", false, "Flip left and right");
  bool_in(b, "Invert Green", true, "Flip up and down, to switch between OpenGL and DirectX");
}

static void declare_normalize(NodeDeclarationBuilder &b)
{
  color_out(b, "Normal", "The normal map with every vector made one long");
  normal_in(b);
}

static void declare_transform(NodeDeclarationBuilder &b)
{
  color_out(b, "Normal");
  normal_in(b);
  angle_in(b, "Rotation", 0.0f, "Turns the image and its normals together");
  float_in(b, "Scale", 1.0f, 0.01f, 64.0f, "Size of the image, below 1 repeats it");
  vector2_in(b, "Offset", float2(0.0f), -1.0f, 1.0f, "Slides the image");
  normal_format_in(b);
}

static void declare_vector_rotation(NodeDeclarationBuilder &b)
{
  color_out(b, "Normal");
  normal_in(b);
  value_in(b, "Rotation Map", 1.0f, "How far each pixel's normal turns");
  angle_in(b, "Rotation", float(M_PI) * 0.5f, "Turn at white in the rotation map");
  normal_format_in(b);
}

class InvertOperation : public FilterOperation {
 public:
  using FilterOperation::FilterOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    Array<float4> c = this->colors("Normal", size);
    const bool red = this->b("Invert Red");
    const bool green = this->b("Invert Green");
    for (float4 &v : c) {
      if (red) {
        v.x = 1.0f - v.x;
      }
      if (green) {
        v.y = 1.0f - v.y;
      }
    }
    this->out_colors("Normal", domain, c);
  }
};

class NormalizeOperation : public FilterOperation {
 public:
  using FilterOperation::FilterOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    Array<float4> c = this->colors("Normal", size);
    for (float4 &v : c) {
      v = encode_normal(decode_normal(v, false), false);
    }
    this->out_colors("Normal", domain, c);
  }
};

class TransformOperation : public NormalOperation {
 public:
  using NormalOperation::NormalOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float3> n = this->normals("Normal", size);
    const float rotation = this->f("Rotation");
    const float scale = math::max(this->f("Scale"), 1e-3f);
    const float2 offset = this->v("Offset");
    Array<float3> out(n.size());
    each_pixel(size, [&](const int2 p, const int64_t index) {
      const float2 uv = rotate2(uv_of(p, size) - 0.5f - offset, -rotation) / scale + 0.5f;
      const float3 s = math::normalize(sample_uv(n.as_span(), size, uv));
      const float2 xy = rotate2(float2(s.x, s.y), rotation);
      out[index] = float3(xy.x, xy.y, s.z);
    });
    this->out_normals("Normal", domain, out);
  }
};

class VectorRotationOperation : public NormalOperation {
 public:
  using NormalOperation::NormalOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float3> n = this->normals("Normal", size);
    const Array<float> map = this->values("Rotation Map", size);
    const float rotation = this->f("Rotation");
    Array<float3> out(n.size());
    for (const int64_t k : n.index_range()) {
      const float2 xy = rotate2(float2(n[k].x, n[k].y), map[k] * rotation);
      out[k] = float3(xy.x, xy.y, n[k].z);
    }
    this->out_normals("Normal", domain, out);
  }
};

}  // namespace edit

/** \} */

/* -------------------------------------------------------------------- */
/** \name Normal to Height
 * \{ */

namespace to_height {

/* Solves for a height whose slopes are `slope`, wrapping at the edges: first on a half size
 * copy, then a number of relaxation passes at this size. */
static Array<float> integrate(const Span<float2> slope, const int2 size, const int passes)
{
  const int64_t count = int64_t(size.x) * size.y;
  Array<float> height(count, 0.0f);
  if (size.x >= 8 && size.y >= 8) {
    const int2 half = size / 2;
    Array<float2> coarse(int64_t(half.x) * half.y, float2(0.0f));
    for (int y = 0; y < half.y * 2; y++) {
      for (int x = 0; x < half.x * 2; x++) {
        /* Slopes double on a grid of pixels twice as big. */
        coarse[int64_t(y / 2) * half.x + x / 2] += slope[int64_t(y) * size.x + x] * 0.5f;
      }
    }
    const Array<float> coarse_height = integrate(coarse, half, passes);
    each_pixel(size, [&](const int2 p, const int64_t index) {
      const float2 at = (float2(p) + 0.5f) * float2(half) / float2(size);
      height[index] = sample_wrapped(coarse_height.as_span(), half, at) * 2.0f;
    });
  }
  /* Divergence of the slopes, with backward differences to match the forward ones below. */
  Array<float> divergence(count);
  each_pixel(size, [&](const int2 p, const int64_t index) {
    const int2 l = tg::wrap_texel(p - int2(1, 0), size);
    const int2 d = tg::wrap_texel(p - int2(0, 1), size);
    divergence[index] = slope[index].x - slope[int64_t(l.y) * size.x + l.x].x + slope[index].y -
                        slope[int64_t(d.y) * size.x + d.x].y;
  });
  Array<float> next(count);
  for (int pass = 0; pass < passes; pass++) {
    each_pixel(size, [&](const int2 p, const int64_t index) {
      const auto at = [&](const int dx, const int dy) {
        const int2 w = tg::wrap_texel(p + int2(dx, dy), size);
        return height[int64_t(w.y) * size.x + w.x];
      };
      const float average = (at(1, 0) + at(-1, 0) + at(0, 1) + at(0, -1) - divergence[index]) *
                            0.25f;
      /* Over-relax a little to converge faster. */
      next[index] = height[index] + (average - height[index]) * 1.6f;
    });
    std::swap(height, next);
  }
  return height;
}

static void declare_common(NodeDeclarationBuilder &b)
{
  value_out(b, "Height", "Height whose bumps make the normal map");
  normal_in(b);
  factor_in(b, "Height Depth", 1.0f, "Contrast of the height");
  normal_format_in(b);
}

static void declare_plain(NodeDeclarationBuilder &b)
{
  declare_common(b);
}

static void declare_hq(NodeDeclarationBuilder &b)
{
  declare_common(b);
  int_in(b, "Quality", 4, 1, 8, "More passes give smoother large shapes, but take longer");
}

template<bool hq> class Operation : public NormalOperation {
 public:
  using NormalOperation::NormalOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float3> n = this->normals("Normal", size);
    Array<float2> slope(n.size());
    for (const int64_t k : n.index_range()) {
      const float z = math::max(n[k].z, 0.1f);
      slope[k] = float2(-n[k].x / z, -n[k].y / z);
    }
    const int passes = hq ? 30 * math::max(this->i("Quality"), 1) : 20;
    const Array<float> height = integrate(slope, size, passes);
    float lo = 1e30f, hi = -1e30f;
    for (const float h : height) {
      lo = math::min(lo, h);
      hi = math::max(hi, h);
    }
    const float depth = this->f("Height Depth");
    Array<float> out(height.size());
    const float range = math::max(hi - lo, 1e-6f);
    for (const int64_t k : height.index_range()) {
      out[k] = 0.5f + ((height[k] - lo) / range - 0.5f) * depth;
    }
    this->out_values("Height", domain, out);
  }
};

}  // namespace to_height

/** \} */

/* -------------------------------------------------------------------- */
/** \name Bent Normal
 * \{ */

namespace bent {

static void node_declare(NodeDeclarationBuilder &b)
{
  color_out(b, "Bent Normal", "The normal turned toward the open sky");
  value_in(b, "Height", 0.5f, "Height that blocks the sky");
  normal_in(b, "Normal", "Normal map to bend, flat when not connected");
  float_in(b, "Height Depth", 0.1f, 0.0f, 1.0f, "How deep the height is, next to the image width");
  float_in(b, "Radius", 0.15f, 0.0f, 1.0f, "How far to look for occluders, next to the image width");
  int_in(b, "Quality", 8, 4, 32, "Number of directions to look in");
  normal_format_in(b);
}

class Operation : public NormalOperation {
 public:
  using NormalOperation::NormalOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float> height = this->values("Height", size);
    const Array<float3> normal = this->normals("Normal", size);
    const float width = float(math::max(size.x, size.y));
    const float depth = this->f("Height Depth") * width;
    const float radius = math::max(this->f("Radius") * width, 1.0f);
    const int directions = math::max(this->i("Quality"), 1);
    Array<float3> out(height.size());
    each_pixel(size, [&](const int2 p, const int64_t index) {
      float3 sum(0.0f);
      horizons(height, size, p, depth, radius, directions, [&](const float2 dir, const float h) {
        /* Middle of the open part of the sky in this direction. */
        const float low = std::asin(math::clamp(h, 0.0f, 1.0f));
        const float mid = (low + float(M_PI) * 0.5f) * 0.5f;
        sum += float3(dir.x * math::cos(mid), dir.y * math::cos(mid), math::sin(mid));
      });
      const float3 bent = math::normalize(sum);
      out[index] = rotate_from_up(normal[index], bent);
    });
    this->out_normals("Bent Normal", domain, out);
  }
};

}  // namespace bent

/** \} */

static void node_register()
{
  static bke::bNodeType types[14];
  int k = 0;
  const short nclass = NODE_CLASS_OP_VECTOR;
  register_filter(types[k++], "TextureGraphNodeFacingNormal", "Facing Normal",
                  "How much a normal map faces straight out", "TEXTURE_GRAPH_FACING_NORMAL", nclass,
                  facing::node_declare, make_operation<facing::Operation>);
  register_filter(types[k++], "TextureGraphNodeHeightNormalBlender", "Height Normal Blender",
                  "Add the bumps of a height to a normal map",
                  "TEXTURE_GRAPH_HEIGHT_NORMAL_BLENDER", nclass, from_height::declare_blender,
                  make_operation<from_height::BlenderOperation>);
  register_filter(types[k++], "TextureGraphNodeHeightToNormalWorldUnits",
                  "Height to Normal World Units",
                  "Make a normal map from a height of a real size",
                  "TEXTURE_GRAPH_HEIGHT_TO_NORMAL_WORLD_UNITS", nclass,
                  from_height::declare_world_units, make_operation<from_height::WorldUnitsOperation>);
  register_filter(types[k++], "TextureGraphNodeNormalBlend", "Normal Blend",
                  "Mix two normal maps through a mask", "TEXTURE_GRAPH_NORMAL_BLEND", nclass,
                  combine::declare_blend, make_operation<combine::BlendOperation>);
  register_filter(types[k++], "TextureGraphNodeNormalCombine", "Normal Combine",
                  "Lay a detail normal map on the bends of a base one",
                  "TEXTURE_GRAPH_NORMAL_COMBINE", nclass, combine::declare_combine,
                  make_operation<combine::CombineOperation<false>>);
  register_filter(types[k++], "TextureGraphNodeNormalInvert", "Normal Invert",
                  "Flip the red or green of a normal map", "TEXTURE_GRAPH_NORMAL_INVERT", nclass,
                  edit::declare_invert, make_operation<edit::InvertOperation>);
  register_filter(types[k++], "TextureGraphNodeNormalNormalize", "Normal Normalize",
                  "Make every vector of a normal map one long", "TEXTURE_GRAPH_NORMAL_NORMALIZE",
                  nclass, edit::declare_normalize, make_operation<edit::NormalizeOperation>);
  register_filter(types[k++], "TextureGraphNodeNormalSobel", "Normal Sobel",
                  "Make a normal map from a height with a Sobel filter",
                  "TEXTURE_GRAPH_NORMAL_SOBEL", nclass, from_height::declare_sobel,
                  make_operation<from_height::SobelOperation>);
  register_filter(types[k++], "TextureGraphNodeNormalToHeight", "Normal to Height",
                  "Rebuild a height from a normal map", "TEXTURE_GRAPH_NORMAL_TO_HEIGHT", nclass,
                  to_height::declare_plain, make_operation<to_height::Operation<false>>);
  register_filter(types[k++], "TextureGraphNodeNormalToHeightHQ", "Normal to Height HQ",
                  "Rebuild a height from a normal map, with more passes for large shapes",
                  "TEXTURE_GRAPH_NORMAL_TO_HEIGHT_HQ", nclass, to_height::declare_hq,
                  make_operation<to_height::Operation<true>>);
  register_filter(types[k++], "TextureGraphNodeNormalTransform", "Normal Transform",
                  "Turn, scale and slide a normal map, turning its normals with it",
                  "TEXTURE_GRAPH_NORMAL_TRANSFORM", nclass, edit::declare_transform,
                  make_operation<edit::TransformOperation>);
  register_filter(types[k++], "TextureGraphNodeNormalVectorRotation", "Normal Vector Rotation",
                  "Turn the normals of a normal map by a map, without moving the image",
                  "TEXTURE_GRAPH_NORMAL_VECTOR_ROTATION", nclass, edit::declare_vector_rotation,
                  make_operation<edit::VectorRotationOperation>);
  register_filter(types[k++], "TextureGraphNodeBentNormal", "Bent Normal",
                  "Turn a normal map toward the open sky over a height",
                  "TEXTURE_GRAPH_BENT_NORMAL", nclass, bent::node_declare,
                  make_operation<bent::Operation>);
  register_filter(types[k++], "TextureGraphNodeNormalUncombine", "Normal Uncombine",
                  "Take a base normal map back out of a combined one",
                  "TEXTURE_GRAPH_NORMAL_UNCOMBINE", nclass, combine::declare_uncombine,
                  make_operation<combine::CombineOperation<true>>);
  BLI_assert(k == 14);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_normal_map_cc
