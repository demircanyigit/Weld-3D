/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Texture Graph Edge Detect: every flat area of the image becomes a white shape, with a sharp
 * black gap wherever two areas of different gray meet. Roundness rounds the corners of the
 * shapes. Non Uniform Edge Detect gives each shape its own gap width.
 */

#include "BLI_vector.hh"

#include "texture_graph_filter.hh"

namespace blender::nodes::node_texture_graph_edge_detect_cc {

namespace tg = texture_graph;
using namespace tg::filter;

static void declare_common(NodeDeclarationBuilder &b)
{
  value_out(b, "Edges", "White shapes with black gaps between them");
  value_in(b, "Image", 0.0f, "Image to find edges in");
  float_in(b, "Width", 2.0f, 0.0f, 32.0f, "Width of the gaps between shapes");
  float_in(b, "Roundness", 4.0f, 0.0f, 32.0f, "Rounds the corners of the shapes");
  factor_in(b, "Tolerance", 0.0f, "How much the gray must change to make an edge");
}

static void node_declare(NodeDeclarationBuilder &b)
{
  declare_common(b);
  bool_in(b, "Invert", false, "White lines on black");
}

static void node_declare_non_uniform(NodeDeclarationBuilder &b)
{
  declare_common(b);
  factor_in(b, "Variation", 0.75f, "How much the gap changes from shape to shape");
  value_in(b, "Width Map", 1.0f, "Multiplies the width, connect an image to set it by place");
  seed_in(b);
  bool_in(b, "Invert", false, "White lines on black");
}

static bool differ(const float a, const float b, const float tolerance)
{
  return math::abs(a - b) > tolerance;
}

/* Pixels that touch a pixel of another gray. */
static Array<bool> edge_pixels(const Span<float> image, const int2 size, const float tolerance)
{
  Array<bool> seeds(image.size(), false);
  for (int y = 0; y < size.y; y++) {
    for (int x = 0; x < size.x; x++) {
      const int64_t k = int64_t(y) * size.x + x;
      const int64_t right = int64_t(y) * size.x + (x + 1) % size.x;
      const int64_t down = int64_t((y + 1) % size.y) * size.x + x;
      if (differ(image[k], image[right], tolerance)) {
        seeds[k] = seeds[right] = true;
      }
      if (differ(image[k], image[down], tolerance)) {
        seeds[k] = seeds[down] = true;
      }
    }
  }
  return seeds;
}

/* A number for each flat area, so each shape can be told apart. */
static Array<int> label_shapes(const Span<float> image, const int2 size, const float tolerance)
{
  Array<int> labels(image.size(), -1);
  Vector<int64_t> stack;
  int next = 0;
  for (int64_t start = 0; start < image.size(); start++) {
    if (labels[start] >= 0) {
      continue;
    }
    labels[start] = next;
    stack.append(start);
    while (!stack.is_empty()) {
      const int64_t k = stack.pop_last();
      const int x = int(k % size.x), y = int(k / size.x);
      const int64_t around[4] = {int64_t(y) * size.x + (x + 1) % size.x,
                                 int64_t(y) * size.x + (x + size.x - 1) % size.x,
                                 int64_t((y + 1) % size.y) * size.x + x,
                                 int64_t((y + size.y - 1) % size.y) * size.x + x};
      for (const int64_t n : around) {
        if (labels[n] < 0 && !differ(image[k], image[n], tolerance)) {
          labels[n] = next;
          stack.append(n);
        }
      }
    }
    next++;
  }
  return labels;
}

static float distance_to(const Span<int2> nearest,
                         const int2 p,
                         const int64_t index,
                         const int2 size)
{
  const int2 n = nearest[index];
  if (n.x < 0) {
    return float(math::max(size.x, size.y));
  }
  return math::length(tg::wrapped_delta(float2(n - p), float2(size)));
}

class Operation : public FilterOperation {
 public:
  using FilterOperation::FilterOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float> image = this->values("Image", size);
    const float width = tg::scaled_radius(math::max(this->f("Width"), 0.0f), size);
    const float roundness = tg::scaled_radius(math::max(this->f("Roundness"), 0.0f), size);
    const float tolerance = math::max(this->f("Tolerance"), 1e-4f);
    const bool invert = this->b("Invert");
    const bool non_uniform = STREQ(this->node().idname,
                                   "TextureGraphNodeNonUniformEdgeDetect");

    /* Gap width at each pixel, the same over a whole shape unless a width map changes it. */
    Array<float> widths(image.size(), width);
    if (non_uniform) {
      const float variation = this->f("Variation");
      const uint32_t seed = uint32_t(this->i("Seed"));
      const Array<float> width_map = this->values("Width Map", size);
      const Array<int> labels = label_shapes(image, size, tolerance);
      threading::parallel_for(widths.index_range(), 4096, [&](const IndexRange range) {
        for (const int64_t i : range) {
          const float random = tg::hash_to_float(tg::hash(labels[i], 0x2f1, seed));
          widths[i] = width * (1.0f - variation * random) * math::max(width_map[i], 0.0f);
        }
      });
    }

    /* Distance from each pixel's center to the nearest edge, which lies half a pixel past the
     * center of the nearest edge pixel. */
    const Array<bool> seeds = edge_pixels(image, size, tolerance);
    const Array<int2> nearest = nearest_seeds(seeds, size, true);
    Array<float> to_edge(image.size());
    each_pixel(size, [&](const int2 p, const int64_t index) {
      to_edge[index] = distance_to(nearest, p, index, size) + 0.5f;
    });

    /* Rounding: shrink the shapes by the roundness, then grow them back by the same amount.
     * Straight sides come back where they were, corners come back round. */
    Array<float> to_core;
    if (roundness > 0.0f) {
      Array<bool> core(image.size());
      for (const int64_t i : core.index_range()) {
        core[i] = to_edge[i] >= widths[i] * 0.5f + roundness;
      }
      const Array<int2> nearest_core = nearest_seeds(core, size, true);
      to_core = Array<float>(image.size());
      each_pixel(size, [&](const int2 p, const int64_t index) {
        to_core[index] = distance_to(nearest_core, p, index, size);
      });
    }

    Array<float> out(image.size());
    threading::parallel_for(out.index_range(), 4096, [&](const IndexRange range) {
      for (const int64_t i : range) {
        float shape = math::clamp(to_edge[i] - widths[i] * 0.5f + 0.5f, 0.0f, 1.0f);
        if (roundness > 0.0f) {
          shape = math::min(shape, math::clamp(roundness + 0.5f - to_core[i], 0.0f, 1.0f));
        }
        out[i] = invert ? 1.0f - shape : shape;
      }
    });
    this->out_values("Edges", domain, out);
  }
};

static void node_register()
{
  static bke::bNodeType types[2];
  register_filter(types[0],
                  "TextureGraphNodeEdgeDetect",
                  "Edge Detect",
                  "White shapes with sharp black gaps wherever the gray changes",
                  "TEXTURE_GRAPH_EDGE_DETECT",
                  NODE_CLASS_OP_FILTER,
                  node_declare,
                  make_operation<Operation>);
  register_filter(types[1],
                  "TextureGraphNodeNonUniformEdgeDetect",
                  "Non Uniform Edge Detect",
                  "Edge Detect with a different gap width for each shape",
                  "TEXTURE_GRAPH_NON_UNIFORM_EDGE_DETECT",
                  NODE_CLASS_OP_FILTER,
                  node_declare_non_uniform,
                  make_operation<Operation>);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_edge_detect_cc
