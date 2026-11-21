/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Distance: a gradient that falls off with the distance to the white parts of a mask, plus the
 * color of the nearest white part. Distances are measured on the tiling image, so shapes near an
 * edge also spread across to the other side.
 *
 * Uses jump flooding: every pixel keeps the nearest mask pixel it knows of and checks what its
 * neighbors know at halving steps.
 */

#include <limits>

#include "BLI_array.hh"
#include "BLI_math_vector.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"
#include "texture_graph_util.hh"

namespace blender::nodes::node_texture_graph_distance_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Float>("Mask")
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .hide_value()
      .structure_type(StructureType::Dynamic)
      .description("Distances are measured from the white parts of this mask");
  b.add_output<decl::Float>("Distance")
      .structure_type(StructureType::Dynamic)
      .align_with_previous()
      .description("White on the mask, fading to black at the set distance");
  b.add_input<decl::Color>("Source")
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .hide_value()
      .structure_type(StructureType::Dynamic)
      .description("Colors to spread out from the white parts of the mask");
  b.add_output<decl::Color>("Color")
      .structure_type(StructureType::Dynamic)
      .align_with_previous()
      .description("Every pixel takes the source color of its nearest white mask pixel");

  b.add_input<decl::Float>("Max Distance")
      .default_value(0.1f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Distance at which the gradient reaches black, as a part of the image size");
}

using namespace blender::compositor;

class DistanceOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &mask = this->get_input("Mask");
    const Result &source = this->get_input("Source");
    Result &distance_result = this->get_result("Distance");
    Result &color_result = this->get_result("Color");

    if (mask.is_single_value()) {
      const bool inside = mask.get_single_value<float>() > 0.5f;
      if (distance_result.should_compute()) {
        distance_result.allocate_single_value();
        distance_result.set_single_value(inside ? 1.0f : 0.0f);
      }
      if (color_result.should_compute()) {
        if (inside) {
          color_result.share_data(source);
        }
        else {
          color_result.allocate_single_value();
          color_result.set_single_value(Color(float4(0.0f)));
        }
      }
      return;
    }

    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const float2 size_f = float2(size);
    const float max_distance = math::max(
        this->get_input("Max Distance").get_single_value_default<float>() *
            float(math::max(size.x, size.y)),
        1e-6f);

    /* Nearest known mask pixel of each pixel, or -1 if none is known yet. */
    const int64_t pixel_count = int64_t(size.x) * size.y;
    Array<int2> nearest(pixel_count);
    Array<int2> next(pixel_count);
    const auto index = [&](const int2 texel) { return int64_t(texel.y) * size.x + texel.x; };

    parallel_for(size, [&](const int2 texel) {
      nearest[index(texel)] = mask.load_pixel<float>(texel) > 0.5f ? texel : int2(-1);
    });

    const auto distance_squared = [&](const int2 a, const int2 b) {
      return math::length_squared(texture_graph::wrapped_delta(float2(a - b), size_f));
    };

    /* Halving steps, then one more step of 1 pixel to clean up the few wrong picks. */
    Vector<int> steps;
    for (int step = math::max(size.x, size.y) / 2; step >= 1; step /= 2) {
      steps.append(step);
    }
    steps.append(1);

    for (const int step : steps) {
      parallel_for(size, [&](const int2 texel) {
        int2 best = nearest[index(texel)];
        float best_distance = best.x < 0 ? std::numeric_limits<float>::max() :
                                           distance_squared(texel, best);
        for (int y = -1; y <= 1; y++) {
          for (int x = -1; x <= 1; x++) {
            const int2 neighbor = texture_graph::wrap_texel(texel + int2(x, y) * step, size);
            const int2 candidate = nearest[index(neighbor)];
            if (candidate.x < 0) {
              continue;
            }
            const float candidate_distance = distance_squared(texel, candidate);
            if (candidate_distance < best_distance) {
              best_distance = candidate_distance;
              best = candidate;
            }
          }
        }
        next[index(texel)] = best;
      });
      std::swap(nearest, next);
    }

    if (distance_result.should_compute()) {
      distance_result.allocate_texture(domain);
      parallel_for(size, [&](const int2 texel) {
        const int2 seed = nearest[index(texel)];
        float value = 0.0f;
        if (seed.x >= 0) {
          const float distance = math::sqrt(distance_squared(texel, seed));
          value = math::clamp(1.0f - distance / max_distance, 0.0f, 1.0f);
        }
        distance_result.store_pixel(texel, value);
      });
    }

    if (color_result.should_compute()) {
      color_result.allocate_texture(domain);
      parallel_for(size, [&](const int2 texel) {
        const int2 seed = nearest[index(texel)];
        color_result.store_pixel(texel,
                                 seed.x >= 0 ? source.load_pixel<Color, true>(seed) :
                                               Color(float4(0.0f)));
      });
    }
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new DistanceOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;

  tg_node_type_base(&ntype, "TextureGraphNodeDistance");
  ntype.ui_name = "Distance";
  ntype.ui_description =
      "Make a gradient around the white parts of a mask and spread colors out from them";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_DISTANCE";
  ntype.nclass = NODE_CLASS_OP_FILTER;
  ntype.declare = node_declare;
  ntype.flag |= NODE_PREVIEW;
  ntype.get_compositor_operation = get_compositor_operation;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_distance_cc
