/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Frequency filters of the Texture Graph: Highpass, Height Map Frequencies Mapper, Lighting
 * Cancel High Frequencies, Lighting Cancel Low Frequencies and Luminance Highpass. Each splits an
 * image into its broad shapes (a blur) and its fine detail (what the blur takes away). The blurs
 * wrap around the edges, so tiling images keep tiling. Radii are in pixels of a 256 image, so a
 * node looks the same at every size.
 */

#include "BLI_math_vector.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"
#include "texture_graph_util.hh"

namespace blender::nodes::node_texture_graph_frequency_cc {

namespace tg = texture_graph;
using namespace blender::compositor;

/* A whole-image color filter: reads the input into a buffer, lets `filter` change it, and writes
 * it out. A single value input is treated as a one pixel image and gives a single value. */
template<typename Fn>
static void filter_colors(const Result &input, Result &output, const Fn &filter)
{
  const bool single = input.is_single_value();
  const int2 size = single ? int2(1) : input.domain().data_size;
  Array<float4> pixels = tg::read_colors(input, size);
  filter(MutableSpan<float4>(pixels), size);
  if (single) {
    output.allocate_single_value();
    output.set_single_value(Color(pixels[0]));
    return;
  }
  output.allocate_texture(input.domain());
  tg::write_colors(output, pixels, size);
}

static float radius_of(NodeOperation &operation, const char *name, const int2 size)
{
  return tg::scaled_radius(operation.get_input(name).get_single_value_default<float>(), size);
}

}  // namespace blender::nodes::node_texture_graph_frequency_cc

/* -------------------------------------------------------------------- */
/** \name Highpass
 * \{ */

namespace blender::nodes::node_texture_graph_highpass_cc {

namespace tg = texture_graph;
namespace fq = node_texture_graph_frequency_cc;
using namespace blender::compositor;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Color>("Image").structure_type(StructureType::Dynamic);
  b.add_input<decl::Color>("Image")
      .default_value({0.5f, 0.5f, 0.5f, 1.0f})
      .hide_value()
      .structure_type(StructureType::Dynamic);
  b.add_input<decl::Float>("Radius").default_value(8.0f).min(0.0f).max(64.0f).description(
      "Size of the shapes that get flattened, larger keeps less");
}

class HighpassOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    fq::filter_colors(this->get_input("Image"),
                      this->get_result("Image"),
                      [&](MutableSpan<float4> pixels, const int2 size) {
                        Array<float4> blurred(pixels.as_span());
                        tg::blur_wrapped<float4>(blurred, size, fq::radius_of(*this, "Radius", size));
                        for (const int64_t i : pixels.index_range()) {
                          const float alpha = pixels[i].w;
                          pixels[i] = pixels[i] - blurred[i] + 0.5f;
                          pixels[i].w = alpha;
                        }
                      });
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new HighpassOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeHighpass");
  ntype.ui_name = "Highpass";
  ntype.ui_description = "Keep the fine detail and flatten the broad shapes to middle gray";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_HIGHPASS";
  ntype.nclass = NODE_CLASS_OP_FILTER;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_highpass_cc

/** \} */

/* -------------------------------------------------------------------- */
/** \name Height Map Frequencies Mapper
 * \{ */

namespace blender::nodes::node_texture_graph_height_frequencies_cc {

namespace tg = texture_graph;
namespace fq = node_texture_graph_frequency_cc;
using namespace blender::compositor;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Float>("Displacement")
      .structure_type(StructureType::Dynamic)
      .description("The broad shapes");
  b.add_output<decl::Float>("Detail")
      .structure_type(StructureType::Dynamic)
      .description("The fine detail around middle gray");
  b.add_input<decl::Float>("Height").default_value(0.5f).hide_value().structure_type(
      StructureType::Dynamic);
  b.add_input<decl::Float>("Relief").default_value(16.0f).min(0.0f).max(32.0f).description(
      "Shapes smaller than this go to the detail");
}

class HeightFrequenciesOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &input = this->get_input("Height");
    Result &displacement = this->get_result("Displacement");
    Result &detail = this->get_result("Detail");
    if (input.is_single_value()) {
      if (displacement.should_compute()) {
        displacement.allocate_single_value();
        displacement.set_single_value(input.get_single_value<float>());
      }
      if (detail.should_compute()) {
        detail.allocate_single_value();
        detail.set_single_value(0.5f);
      }
      return;
    }
    const Domain domain = input.domain();
    const int2 size = domain.data_size;
    Array<float> height = tg::read_values(input, size);
    Array<float> blurred(height);
    tg::blur_wrapped<float>(blurred, size, fq::radius_of(*this, "Relief", size));
    if (displacement.should_compute()) {
      displacement.allocate_texture(domain);
      tg::write_values(displacement, blurred, size);
    }
    if (detail.should_compute()) {
      for (const int64_t i : height.index_range()) {
        height[i] = height[i] - blurred[i] + 0.5f;
      }
      detail.allocate_texture(domain);
      tg::write_values(detail, height, size);
    }
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new HeightFrequenciesOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeHeightFrequencies");
  ntype.ui_name = "Height Map Frequencies Mapper";
  ntype.ui_description = "Split a height map into broad shapes and fine detail";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_HEIGHT_FREQUENCIES";
  ntype.nclass = NODE_CLASS_OP_FILTER;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_height_frequencies_cc

/** \} */

/* -------------------------------------------------------------------- */
/** \name Lighting Cancel High Frequencies
 * \{ */

namespace blender::nodes::node_texture_graph_lighting_cancel_high_cc {

namespace tg = texture_graph;
namespace fq = node_texture_graph_frequency_cc;
using namespace blender::compositor;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Color>("Image").structure_type(StructureType::Dynamic);
  b.add_input<decl::Color>("Image")
      .default_value({0.5f, 0.5f, 0.5f, 1.0f})
      .hide_value()
      .structure_type(StructureType::Dynamic);
  b.add_input<decl::Float>("Intensity").default_value(1.0f).min(0.0f).max(1.0f).subtype(
      PROP_FACTOR);
  b.add_input<decl::Float>("Radius").default_value(2.0f).min(0.0f).max(10.0f).description(
      "Size of the shading details that get removed");
}

class LightingCancelHighOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const float intensity = this->get_input("Intensity").get_single_value_default<float>();
    fq::filter_colors(
        this->get_input("Image"),
        this->get_result("Image"),
        [&](MutableSpan<float4> pixels, const int2 size) {
          Array<float4> blurred(pixels.as_span());
          tg::blur_wrapped<float4>(blurred, size, fq::radius_of(*this, "Radius", size));
          for (const int64_t i : pixels.index_range()) {
            /* Keep each pixel's color, give it the smoothed brightness. */
            const float own = tg::luminance(pixels[i].xyz());
            const float smooth = tg::luminance(blurred[i].xyz());
            const float ratio = own > 1e-5f ? smooth / own : 1.0f;
            const float3 rgb = pixels[i].xyz() * math::interpolate(1.0f, ratio, intensity);
            pixels[i] = float4(rgb, pixels[i].w);
          }
        });
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new LightingCancelHighOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeLightingCancelHigh");
  ntype.ui_name = "Lighting Cancel High Frequencies";
  ntype.ui_description = "Remove small shading details while keeping the colors";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_LIGHTING_CANCEL_HIGH";
  ntype.nclass = NODE_CLASS_OP_FILTER;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_lighting_cancel_high_cc

/** \} */

/* -------------------------------------------------------------------- */
/** \name Lighting Cancel Low Frequencies
 * \{ */

namespace blender::nodes::node_texture_graph_lighting_cancel_low_cc {

namespace tg = texture_graph;
namespace fq = node_texture_graph_frequency_cc;
using namespace blender::compositor;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Color>("Image").structure_type(StructureType::Dynamic);
  b.add_input<decl::Color>("Image")
      .default_value({0.5f, 0.5f, 0.5f, 1.0f})
      .hide_value()
      .structure_type(StructureType::Dynamic);
  b.add_input<decl::Float>("Radius").default_value(16.0f).min(0.0f).max(64.0f).description(
      "Size of the light and shadow areas that get evened out");
}

class LightingCancelLowOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    fq::filter_colors(
        this->get_input("Image"),
        this->get_result("Image"),
        [&](MutableSpan<float4> pixels, const int2 size) {
          Array<float4> blurred(pixels.as_span());
          tg::blur_wrapped<float4>(blurred, size, fq::radius_of(*this, "Radius", size));
          double sum = 0.0;
          for (const float4 &p : pixels) {
            sum += tg::luminance(p.xyz());
          }
          const float mean = float(sum / double(math::max<int64_t>(pixels.size(), 1)));
          for (const int64_t i : pixels.index_range()) {
            /* Dividing by the broad brightness keeps the saturation. */
            const float broad = math::max(tg::luminance(blurred[i].xyz()), 1e-4f);
            pixels[i] = float4(pixels[i].xyz() * (mean / broad), pixels[i].w);
          }
        });
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new LightingCancelLowOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeLightingCancelLow");
  ntype.ui_name = "Lighting Cancel Low Frequencies";
  ntype.ui_description = "Even out broad light and shadow while keeping the colors";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_LIGHTING_CANCEL_LOW";
  ntype.nclass = NODE_CLASS_OP_FILTER;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_lighting_cancel_low_cc

/** \} */

/* -------------------------------------------------------------------- */
/** \name Luminance Highpass
 * \{ */

namespace blender::nodes::node_texture_graph_luminance_highpass_cc {

namespace tg = texture_graph;
namespace fq = node_texture_graph_frequency_cc;
using namespace blender::compositor;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Color>("Image").structure_type(StructureType::Dynamic);
  b.add_input<decl::Color>("Image")
      .default_value({0.5f, 0.5f, 0.5f, 1.0f})
      .hide_value()
      .structure_type(StructureType::Dynamic);
  b.add_input<decl::Float>("Radius").default_value(16.0f).min(0.0f).max(64.0f).description(
      "Size of the lighting that gets removed, chain several for more than one size");
}

class LuminanceHighpassOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    fq::filter_colors(
        this->get_input("Image"),
        this->get_result("Image"),
        [&](MutableSpan<float4> pixels, const int2 size) {
          Array<float> broad(pixels.size());
          double sum = 0.0;
          for (const int64_t i : pixels.index_range()) {
            broad[i] = tg::luminance(pixels[i].xyz());
            sum += broad[i];
          }
          const float mean = float(sum / double(math::max<int64_t>(pixels.size(), 1)));
          tg::blur_wrapped<float>(broad, size, fq::radius_of(*this, "Radius", size));
          for (const int64_t i : pixels.index_range()) {
            /* Moving all channels by the same amount only changes the brightness. */
            pixels[i] = float4(pixels[i].xyz() + (mean - broad[i]), pixels[i].w);
          }
        });
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new LuminanceHighpassOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeLuminanceHighpass");
  ntype.ui_name = "Luminance Highpass";
  ntype.ui_description = "Remove baked lighting from the brightness only, keeping the colors";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_LUMINANCE_HIGHPASS";
  ntype.nclass = NODE_CLASS_OP_FILTER;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_luminance_highpass_cc

/** \} */
