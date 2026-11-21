/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Blurs of the Texture Graph: Anisotropic Blur, Median Filter, MLV Filter, Non-Uniform Blur,
 * Radial Blur and Slope Blur. They all wrap around the edges, so tiling images keep tiling, and
 * their sizes are in pixels of a 256 image, so a node looks the same at every size. Each works on
 * color and grayscale images alike.
 */

#include <algorithm>
#include <cfloat>

#include "BLI_math_vector.hh"
#include "BLI_vector.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"
#include "texture_graph_util.hh"

namespace blender::nodes::node_texture_graph_blur_cc {

namespace tg = texture_graph;
using namespace blender::compositor;

/* Runs `filter` on the "Image" input as a buffer of colors and writes the result. A single value
 * stays as it is, since blurring a flat color changes nothing. */
template<typename Fn> static void filter_image(NodeOperation &operation, const Fn &filter)
{
  const Result &input = operation.get_input("Image");
  Result &output = operation.get_result("Image");
  if (input.is_single_value()) {
    output.share_data(input);
    return;
  }
  const Domain domain = input.domain();
  const int2 size = domain.data_size;
  const Array<float4> pixels = tg::read_colors(input, size);
  Array<float4> result(pixels.size());
  filter(pixels.as_span(), MutableSpan<float4>(result), size);
  output.allocate_texture(domain);
  tg::write_colors(output, result, size);
}

static void declare_image(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Color>("Image").structure_type(StructureType::Dynamic);
  b.add_input<decl::Color>("Image")
      .default_value({0.5f, 0.5f, 0.5f, 1.0f})
      .hide_value()
      .structure_type(StructureType::Dynamic);
}

/* A direction from an angle in turns. */
static float2 direction(const float turns)
{
  const float a = turns * 2.0f * float(M_PI);
  return float2(math::cos(a), math::sin(a));
}

/* Averages samples along a line through each pixel, `length` pixels each way, with weights
 * falling off toward the ends. */
static void line_blur(const Span<float4> pixels,
                      MutableSpan<float4> result,
                      const int2 size,
                      const float2 dir,
                      const float length)
{
  const int steps = math::max(int(math::ceil(length)), 1);
  threading::parallel_for(IndexRange(size.y), 8, [&](const IndexRange rows) {
    for (const int y : rows) {
      for (int x = 0; x < size.x; x++) {
        const float2 center = float2(x, y) + 0.5f;
        float4 sum(0.0f);
        float weight_sum = 0.0f;
        for (int i = -steps; i <= steps; i++) {
          const float t = float(i) / float(steps);
          const float w = 1.0f - math::abs(t) * 0.9f;
          sum += tg::sample_wrapped(pixels, size, center + dir * (t * length)) * w;
          weight_sum += w;
        }
        result[int64_t(y) * size.x + x] = sum / weight_sum;
      }
    }
  });
}

}  // namespace blender::nodes::node_texture_graph_blur_cc

/* -------------------------------------------------------------------- */
/** \name Anisotropic Blur
 * \{ */

namespace blender::nodes::node_texture_graph_anisotropic_blur_cc {

namespace tg = texture_graph;
namespace bl = node_texture_graph_blur_cc;
using namespace blender::compositor;

static void node_declare(NodeDeclarationBuilder &b)
{
  bl::declare_image(b);
  b.add_input<decl::Float>("Intensity").default_value(4.0f).min(0.0f).max(16.0f).description(
      "How far the blur reaches");
  b.add_input<decl::Float>("Anisotropy").default_value(0.8f).min(0.0f).max(1.0f).subtype(
      PROP_FACTOR).description("Round blur at 0, a streak along the angle at 1");
  b.add_input<decl::Float>("Angle").default_value(0.0f).min(0.0f).max(1.0f).subtype(
      PROP_FACTOR).description("Direction of the streak, in turns");
}

class AnisotropicBlurOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const float intensity = this->get_input("Intensity").get_single_value_default<float>();
    const float anisotropy = math::clamp(
        this->get_input("Anisotropy").get_single_value_default<float>(), 0.0f, 1.0f);
    const float angle = this->get_input("Angle").get_single_value_default<float>();
    bl::filter_image(*this, [&](const Span<float4> pixels, MutableSpan<float4> result, const int2 size) {
      const float radius = tg::scaled_radius(intensity, size);
      /* A round blur for the short side, then a streak for the rest of the long side. */
      Array<float4> smooth(pixels);
      tg::blur_wrapped<float4>(smooth, size, radius * (1.0f - anisotropy));
      const float streak = radius * math::sqrt(math::max(1.0f - math::square(1.0f - anisotropy), 0.0f));
      if (streak < 0.5f) {
        result.copy_from(smooth);
        return;
      }
      bl::line_blur(smooth, result, size, bl::direction(angle), streak);
    });
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new AnisotropicBlurOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeAnisotropicBlur");
  ntype.ui_name = "Anisotropic Blur";
  ntype.ui_description = "Blur stretched along one direction, like motion";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_ANISOTROPIC_BLUR";
  ntype.nclass = NODE_CLASS_OP_FILTER;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_anisotropic_blur_cc

/** \} */

/* -------------------------------------------------------------------- */
/** \name Median Filter
 * \{ */

namespace blender::nodes::node_texture_graph_median_filter_cc {

namespace tg = texture_graph;
namespace bl = node_texture_graph_blur_cc;
using namespace blender::compositor;

enum class Type : int {
  Median = 0,
  Robust = 1,
};

static const EnumPropertyItem type_items[] = {
    {int(Type::Median), "MEDIAN", 0, N_("Median"), N_("The middle value of the neighbors")},
    {int(Type::Robust), "ROBUST", 0, N_("Robust"),
     N_("The middle value of the neighbors closest to the median, smooths more")},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem kernel_items[] = {
    {3, "3", 0, N_("3 x 3"), ""},
    {5, "5", 0, N_("5 x 5"), ""},
    {7, "7", 0, N_("7 x 7"), ""},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  bl::declare_image(b);
  b.add_input<decl::Menu>("Kernel")
      .default_value(MenuValue(3))
      .static_items(kernel_items)
      .description("Size of the square of neighbors looked at, in pixels");
  b.add_input<decl::Menu>("Type")
      .default_value(MenuValue(Type::Median))
      .static_items(type_items)
      .optional_label();
  b.add_input<decl::Bool>("Affect Alpha").default_value(false).description(
      "Filter the alpha too, off keeps it as it is");
}

/* Middle value of a small list, reordering it. */
static float median_of(MutableSpan<float> values)
{
  const int64_t middle = values.size() / 2;
  std::nth_element(values.begin(), values.begin() + middle, values.end());
  return values[middle];
}

class MedianFilterOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const int kernel = math::clamp(
        this->get_input("Kernel").get_single_value_default<MenuValue>().value, 3, 7);
    const Type type = Type(this->get_input("Type").get_single_value_default<MenuValue>().value);
    const bool affect_alpha = this->get_input("Affect Alpha").get_single_value_default<bool>();
    const int r = kernel / 2;
    const int count = kernel * kernel;
    bl::filter_image(*this, [&](const Span<float4> pixels, MutableSpan<float4> result, const int2 size) {
      threading::parallel_for(IndexRange(size.y), 8, [&](const IndexRange rows) {
        Array<float> values(count);
        Array<float> deviations(count);
        Array<float> kept(count);
        for (const int y : rows) {
          for (int x = 0; x < size.x; x++) {
            const int64_t index = int64_t(y) * size.x + x;
            float4 out = pixels[index];
            const int channels = affect_alpha ? 4 : 3;
            for (int ch = 0; ch < channels; ch++) {
              int n = 0;
              for (int dy = -r; dy <= r; dy++) {
                for (int dx = -r; dx <= r; dx++) {
                  const int2 w = tg::wrap_texel(int2(x + dx, y + dy), size);
                  values[n++] = pixels[int64_t(w.y) * size.x + w.x][ch];
                }
              }
              const float median = median_of(values);
              if (type == Type::Median) {
                out[ch] = median;
                continue;
              }
              /* Keep the values no further from the median than the median distance. */
              for (int i = 0; i < count; i++) {
                deviations[i] = math::abs(values[i] - median);
              }
              const float spread = median_of(deviations.as_mutable_span());
              int k = 0;
              for (int i = 0; i < count; i++) {
                if (math::abs(values[i] - median) <= spread) {
                  kept[k++] = values[i];
                }
              }
              out[ch] = k > 0 ? median_of(kept.as_mutable_span().take_front(k)) : median;
            }
            result[index] = out;
          }
        }
      });
    });
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new MedianFilterOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeMedianFilter");
  ntype.ui_name = "Median Filter";
  ntype.ui_description = "Remove specks and noise while keeping edges sharp";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_MEDIAN_FILTER";
  ntype.nclass = NODE_CLASS_OP_FILTER;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_median_filter_cc

/** \} */

/* -------------------------------------------------------------------- */
/** \name MLV Filter
 * \{ */

namespace blender::nodes::node_texture_graph_mlv_filter_cc {

namespace tg = texture_graph;
namespace bl = node_texture_graph_blur_cc;
using namespace blender::compositor;

enum class Criterion : int {
  Variance = 0,
  Relative = 1,
};

static const EnumPropertyItem criterion_items[] = {
    {int(Criterion::Variance), "VARIANCE", 0, N_("Variance"),
     N_("Pick the area whose values spread the least")},
    {int(Criterion::Relative), "RELATIVE", 0, N_("Relative Variance"),
     N_("Spread compared to brightness, bright areas count as calmer")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  bl::declare_image(b);
  b.add_input<decl::Float>("Intensity").default_value(3.0f).min(0.0f).max(16.0f).description(
      "Size of the areas that get flattened");
  b.add_input<decl::Float>("Smoothness").default_value(0.2f).min(0.0f).max(1.0f).subtype(
      PROP_FACTOR).description("Mix the neighboring areas more, for rounder shapes and less stepping");
  b.add_input<decl::Menu>("Criterion")
      .default_value(MenuValue(Criterion::Variance))
      .static_items(criterion_items)
      .description("How the calmest area is picked");
  b.add_input<decl::Bool>("Gaussian").default_value(false).description(
      "Weigh the areas softly toward their middle, giving smoother results");
  b.add_input<decl::Int>("Iterations").default_value(1).min(1).max(8).description(
      "Run the filter again on its own result");
  b.add_input<decl::Bool>("Affect Alpha").default_value(false).description(
      "Filter the alpha too, off keeps it as it is");
}

class MLVFilterOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const float intensity = this->get_input("Intensity").get_single_value_default<float>();
    const float smoothness = math::clamp(
        this->get_input("Smoothness").get_single_value_default<float>(), 0.0f, 1.0f);
    const Criterion criterion = Criterion(
        this->get_input("Criterion").get_single_value_default<MenuValue>().value);
    const bool gaussian = this->get_input("Gaussian").get_single_value_default<bool>();
    const int iterations = math::clamp(
        this->get_input("Iterations").get_single_value_default<int>(), 1, 8);
    const bool affect_alpha = this->get_input("Affect Alpha").get_single_value_default<bool>();

    bl::filter_image(*this, [&](const Span<float4> pixels, MutableSpan<float4> result, const int2 size) {
      const int half = int(math::round(tg::scaled_radius(intensity, size) * 0.5f));
      result.copy_from(pixels);
      if (half <= 0) {
        return;
      }
      /* Higher powers pick the calmest area more strictly. */
      const float power = 1.0f + (1.0f - smoothness) * 15.0f;
      for (int pass = 0; pass < iterations; pass++) {
        /* Means of color, brightness and squared brightness over windows centered on each pixel.
         * The four areas around a pixel are those windows moved by half their size. */
        Array<float4> mean(result.as_span());
        Array<float2> moments(result.size());
        for (const int64_t i : result.index_range()) {
          const float l = tg::luminance(result[i].xyz());
          moments[i] = float2(l, l * l);
        }
        const auto blur = [&](auto &data) {
          if (gaussian) {
            tg::blur_wrapped(data.as_mutable_span(), size, float(half) * 2.0f);
          }
          else {
            tg::box_blur_axis(data.as_mutable_span(), size, half, 0);
            tg::box_blur_axis(data.as_mutable_span(), size, half, 1);
          }
        };
        blur(mean);
        blur(moments);

        const Array<float4> source(result.as_span());
        threading::parallel_for(IndexRange(size.y), 16, [&](const IndexRange rows) {
          for (const int y : rows) {
            for (int x = 0; x < size.x; x++) {
              float4 sum(0.0f);
              float weight_sum = 0.0f;
              float lowest = FLT_MAX;
              float measures[4];
              int64_t indices[4];
              int k = 0;
              for (const int dy : {-half, half}) {
                for (const int dx : {-half, half}) {
                  const int2 w = tg::wrap_texel(int2(x + dx, y + dy), size);
                  const int64_t i = int64_t(w.y) * size.x + w.x;
                  const float variance = math::max(moments[i].y - moments[i].x * moments[i].x, 0.0f);
                  float measure = variance;
                  if (criterion == Criterion::Relative) {
                    measure = math::sqrt(variance) / math::max(moments[i].x, 1e-3f);
                  }
                  measures[k] = measure;
                  indices[k] = i;
                  lowest = math::min(lowest, measure);
                  k++;
                }
              }
              for (k = 0; k < 4; k++) {
                const float w = math::pow((lowest + 1e-6f) / (measures[k] + 1e-6f), power);
                sum += mean[indices[k]] * w;
                weight_sum += w;
              }
              const int64_t index = int64_t(y) * size.x + x;
              float4 out = sum / weight_sum;
              if (!affect_alpha) {
                out.w = source[index].w;
              }
              result[index] = out;
            }
          }
        });
      }
    });
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new MLVFilterOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeMLVFilter");
  ntype.ui_name = "MLV Filter";
  ntype.ui_description = "Flatten areas of similar pixels while keeping the edges between them sharp";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_MLV_FILTER";
  ntype.nclass = NODE_CLASS_OP_FILTER;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_mlv_filter_cc

/** \} */

/* -------------------------------------------------------------------- */
/** \name Non-Uniform Blur
 * \{ */

namespace blender::nodes::node_texture_graph_non_uniform_blur_cc {

namespace tg = texture_graph;
namespace bl = node_texture_graph_blur_cc;
using namespace blender::compositor;

static void node_declare(NodeDeclarationBuilder &b)
{
  bl::declare_image(b);
  b.add_input<decl::Float>("Blur Map")
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("How much each pixel is blurred, black stays sharp");
  b.add_input<decl::Float>("Intensity").default_value(8.0f).min(0.0f).max(50.0f).description(
      "How far the blur reaches where the map is white");
  b.add_input<decl::Float>("Anisotropy").default_value(0.0f).min(0.0f).max(1.0f).subtype(
      PROP_FACTOR).description("Squeeze the blur into a streak along the angle");
  b.add_input<decl::Float>("Asymmetry").default_value(0.0f).min(0.0f).max(1.0f).subtype(
      PROP_FACTOR).description("Push the blur toward one side, along the angle");
  b.add_input<decl::Float>("Angle").default_value(0.0f).min(0.0f).max(1.0f).subtype(
      PROP_FACTOR).description("Direction of the streak and the push, in turns");
  b.add_input<decl::Int>("Samples").default_value(4).min(1).max(16).description(
      "Samples along each blade, more is smoother and slower");
  b.add_input<decl::Int>("Blades").default_value(5).min(1).max(9).description(
      "Directions sampled around each pixel, more is smoother and slower");
}

class NonUniformBlurOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &map = this->get_input("Blur Map");
    const float intensity = this->get_input("Intensity").get_single_value_default<float>();
    const float anisotropy = math::clamp(
        this->get_input("Anisotropy").get_single_value_default<float>(), 0.0f, 1.0f);
    const float asymmetry = math::clamp(
        this->get_input("Asymmetry").get_single_value_default<float>(), 0.0f, 1.0f);
    const float2 along = bl::direction(this->get_input("Angle").get_single_value_default<float>());
    const float2 across(-along.y, along.x);
    const int samples = math::clamp(this->get_input("Samples").get_single_value_default<int>(), 1, 16);
    const int blades = math::clamp(this->get_input("Blades").get_single_value_default<int>(), 1, 9);

    bl::filter_image(*this, [&](const Span<float4> pixels, MutableSpan<float4> result, const int2 size) {
      const Array<float> amounts = tg::read_values(map, size);
      const float radius = tg::scaled_radius(intensity, size);
      /* Sample offsets on a unit disc, the same for every pixel. */
      Vector<float2> offsets;
      for (int blade = 0; blade < blades; blade++) {
        for (int s = 0; s < samples; s++) {
          const float a = (float(blade) + float(s) * 0.618034f) / float(blades) * 2.0f * float(M_PI);
          const float d = math::sqrt((float(s) + 0.5f) / float(samples));
          float2 o = float2(math::cos(a), math::sin(a)) * d;
          /* Squeeze across the angle, then shift along it. */
          const float u = math::dot(o, along) + asymmetry * d;
          const float v = math::dot(o, across) * (1.0f - anisotropy);
          offsets.append(along * u + across * v);
        }
      }
      threading::parallel_for(IndexRange(size.y), 8, [&](const IndexRange rows) {
        for (const int y : rows) {
          for (int x = 0; x < size.x; x++) {
            const int64_t index = int64_t(y) * size.x + x;
            const float r = radius * math::clamp(amounts[index], 0.0f, 1.0f);
            if (r < 0.25f) {
              result[index] = pixels[index];
              continue;
            }
            const float2 center = float2(x, y) + 0.5f;
            float4 sum = pixels[index];
            for (const float2 &o : offsets) {
              sum += tg::sample_wrapped(pixels, size, center + o * r);
            }
            result[index] = sum / float(offsets.size() + 1);
          }
        }
      });
    });
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new NonUniformBlurOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeNonUniformBlur");
  ntype.ui_name = "Non-Uniform Blur";
  ntype.ui_description = "Blur that is stronger where a map is brighter";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_NON_UNIFORM_BLUR";
  ntype.nclass = NODE_CLASS_OP_FILTER;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_non_uniform_blur_cc

/** \} */

/* -------------------------------------------------------------------- */
/** \name Radial Blur
 * \{ */

namespace blender::nodes::node_texture_graph_radial_blur_cc {

namespace tg = texture_graph;
namespace bl = node_texture_graph_blur_cc;
using namespace blender::compositor;

static void node_declare(NodeDeclarationBuilder &b)
{
  bl::declare_image(b);
  b.add_input<decl::Float>("Angle").default_value(0.05f).min(0.0f).max(0.5f).description(
      "How far around the center the blur sweeps, in turns");
  b.add_input<decl::Vector>("Center")
      .dimensions(2)
      .default_value({0.5f, 0.5f})
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Point the image spins around");
  b.add_input<decl::Int>("Samples").default_value(32).min(1).max(128).description(
      "Samples along each arc, more is smoother and slower");
}

class RadialBlurOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const float angle = this->get_input("Angle").get_single_value_default<float>();
    const float2 center_uv = this->get_input("Center").get_single_value_default<float2>();
    const int samples = math::clamp(this->get_input("Samples").get_single_value_default<int>(), 1,
                                    128);
    bl::filter_image(*this, [&](const Span<float4> pixels, MutableSpan<float4> result, const int2 size) {
      const float2 center = center_uv * float2(size);
      const float sweep = angle * 2.0f * float(M_PI);
      /* Rotations spread evenly over the sweep, centered on no rotation. */
      Vector<float2> turns;
      for (int i = 0; i < samples; i++) {
        const float t = samples == 1 ? 0.0f : (float(i) / float(samples - 1) - 0.5f) * sweep;
        turns.append(float2(math::cos(t), math::sin(t)));
      }
      threading::parallel_for(IndexRange(size.y), 8, [&](const IndexRange rows) {
        for (const int y : rows) {
          for (int x = 0; x < size.x; x++) {
            const float2 d = float2(x, y) + 0.5f - center;
            float4 sum(0.0f);
            for (const float2 &cs : turns) {
              const float2 p = center + float2(d.x * cs.x - d.y * cs.y, d.x * cs.y + d.y * cs.x);
              sum += tg::sample_wrapped(pixels, size, p);
            }
            result[int64_t(y) * size.x + x] = sum / float(turns.size());
          }
        }
      });
    });
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new RadialBlurOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeRadialBlur");
  ntype.ui_name = "Radial Blur";
  ntype.ui_description = "Blur the image as if it spun around a point";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_RADIAL_BLUR";
  ntype.nclass = NODE_CLASS_OP_FILTER;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_radial_blur_cc

/** \} */

/* -------------------------------------------------------------------- */
/** \name Slope Blur
 * \{ */

namespace blender::nodes::node_texture_graph_slope_blur_cc {

namespace tg = texture_graph;
namespace bl = node_texture_graph_blur_cc;
using namespace blender::compositor;

enum class Mode : int {
  Blur = 0,
  Min = 1,
  Max = 2,
};

static const EnumPropertyItem mode_items[] = {
    {int(Mode::Blur), "BLUR", 0, N_("Blur"), N_("Average along the slopes")},
    {int(Mode::Min), "MIN", 0, N_("Min"), N_("Darkest along the slopes, eats into bright areas")},
    {int(Mode::Max), "MAX", 0, N_("Max"), N_("Brightest along the slopes, spreads bright areas")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  bl::declare_image(b);
  b.add_input<decl::Float>("Slope").default_value(0.5f).hide_value().description(
      "Height map whose slopes set where the image runs, smooth gradients work best");
  b.add_input<decl::Float>("Intensity").default_value(4.0f).min(0.0f).max(16.0f).description(
      "How far the image runs along the slopes");
  b.add_input<decl::Int>("Samples").default_value(8).min(1).max(32).description(
      "Steps along the slopes, more is smoother and slower");
  b.add_input<decl::Menu>("Mode")
      .default_value(MenuValue(Mode::Blur))
      .static_items(mode_items)
      .optional_label();
}

class SlopeBlurOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &slope_input = this->get_input("Slope");
    const float intensity = this->get_input("Intensity").get_single_value_default<float>();
    const int samples = math::clamp(this->get_input("Samples").get_single_value_default<int>(), 1,
                                    32);
    const Mode mode = Mode(this->get_input("Mode").get_single_value_default<MenuValue>().value);

    bl::filter_image(*this, [&](const Span<float4> pixels, MutableSpan<float4> result, const int2 size) {
      const Array<float2> slopes = tg::slope_field(tg::read_values(slope_input, size), size);
      const float distance = tg::scaled_radius(intensity, size);
      const float step = distance / float(samples);
      /* Slope in height change across the whole image: a full ramp is 1. */
      const float scale = float(size.x);
      threading::parallel_for(IndexRange(size.y), 8, [&](const IndexRange rows) {
        for (const int y : rows) {
          for (int x = 0; x < size.x; x++) {
            const int64_t index = int64_t(y) * size.x + x;
            float2 p = float2(x, y) + 0.5f;
            float4 sum = pixels[index];
            float4 low = sum;
            float4 high = sum;
            for (int i = 0; i < samples; i++) {
              /* Follow the slope uphill, so what lies higher runs down onto this pixel. */
              const float2 g = tg::sample_wrapped(slopes.as_span(), size, p) * scale;
              const float length = math::length(g);
              if (length > 1e-6f) {
                const float strength = math::min(length / 4.0f, 1.0f);
                p += g / length * (strength * step);
              }
              const float4 c = tg::sample_wrapped(pixels, size, p);
              sum += c;
              low = math::min(low, c);
              high = math::max(high, c);
            }
            switch (mode) {
              case Mode::Blur:
                result[index] = sum / float(samples + 1);
                break;
              case Mode::Min:
                result[index] = low;
                break;
              case Mode::Max:
                result[index] = high;
                break;
            }
          }
        }
      });
    });
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new SlopeBlurOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeSlopeBlur");
  ntype.ui_name = "Slope Blur";
  ntype.ui_description = "Smear an image along the slopes of a height map, for drips and wear";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_SLOPE_BLUR";
  ntype.nclass = NODE_CLASS_OP_FILTER;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_slope_blur_cc

/** \} */
