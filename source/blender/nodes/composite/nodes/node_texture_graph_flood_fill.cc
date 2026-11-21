/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Flood Fill finds the separate shapes of a mask and writes, in every pixel of a shape, where the
 * pixel is in the shape's bounding box and how big that box is:
 *   red, green: position inside the box, 0 to 1;
 *   blue, alpha: width and height of the box next to the image size.
 * Background pixels are all zero. The "Flood Fill to ..." nodes turn that into values per shape.
 */

#include <algorithm>

#include "BLI_map.hh"
#include "BLI_vector.hh"

#include "texture_graph_filter.hh"

namespace blender::nodes::node_texture_graph_flood_fill_cc {

namespace tg = texture_graph;
using namespace tg::filter;

/* One pixel of a flood fill image, read back. */
struct Cell {
  bool valid = false;
  float2 local;
  float2 box;
  /* Middle of the shape's box, 0 to 1. */
  float2 center;
  /* Same for every pixel of one shape. */
  int64_t key = 0;
};

static Cell read_cell(const float4 color, const int2 p, const int2 size)
{
  Cell cell;
  if (color.z <= 0.0f || color.w <= 0.0f) {
    return cell;
  }
  cell.valid = true;
  cell.local = float2(color.x, color.y);
  cell.box = float2(color.z, color.w);
  float2 center = uv_of(p, size) + (0.5f - cell.local) * cell.box;
  center -= math::floor(center);
  cell.center = center;
  /* Box middles fall on whole or half pixels. */
  const int2 half_pixels = int2(math::round(center * float2(size) * 2.0f));
  const int2 wrapped = tg::wrap_texel(half_pixels, size * 2);
  cell.key = int64_t(wrapped.y) * (size.x * 2) + wrapped.x;
  return cell;
}

static void declare_fill_input(NodeDeclarationBuilder &b)
{
  color_in(b, "Flood Fill", float4(0.0f), "Output of a Flood Fill node");
}

/* -------------------------------------------------------------------- */
/** \name Flood Fill
 * \{ */

namespace flood_fill {

static void node_declare(NodeDeclarationBuilder &b)
{
  color_out(b, "Flood Fill", "Position in the shape's box, and the box's size, per pixel");
  value_in(b, "Mask", 0.0f, "Shapes to find, white is inside");
  bool_in(b, "Wrap", true, "Shapes that cross an edge go on at the other side");
  bool_in(b, "Diagonal", false, "Pixels touching at a corner belong to the same shape");
}

class Operation : public FilterOperation {
 public:
  using FilterOperation::FilterOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<float> mask = this->values("Mask", size);
    const bool wrap = this->b("Wrap");
    const bool diagonal = this->b("Diagonal");
    const int64_t count = mask.size();
    Array<int> label(count, -1);
    /* Position of each pixel as reached from its shape's first pixel, not wrapped. */
    Array<int2> unwrapped(count);
    Vector<int2> box_min, box_max;
    Vector<int64_t> queue;
    const int2 neighbors[8] = {
        {1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
    const int neighbor_count = diagonal ? 8 : 4;
    for (int64_t start = 0; start < count; start++) {
      if (mask[start] < 0.5f || label[start] >= 0) {
        continue;
      }
      const int id = int(box_min.size());
      const int2 p0(int(start % size.x), int(start / size.x));
      int2 lo = p0, hi = p0;
      label[start] = id;
      unwrapped[start] = p0;
      queue.clear();
      queue.append(start);
      for (int64_t q = 0; q < queue.size(); q++) {
        const int64_t i = queue[q];
        const int2 u = unwrapped[i];
        for (int n = 0; n < neighbor_count; n++) {
          const int2 next_u = u + neighbors[n];
          int2 next = next_u;
          if (wrap) {
            next = tg::wrap_texel(next, size);
          }
          else if (next.x < 0 || next.y < 0 || next.x >= size.x || next.y >= size.y) {
            continue;
          }
          const int64_t j = int64_t(next.y) * size.x + next.x;
          if (label[j] >= 0 || mask[j] < 0.5f) {
            continue;
          }
          label[j] = id;
          unwrapped[j] = next_u;
          lo = math::min(lo, next_u);
          hi = math::max(hi, next_u);
          queue.append(j);
        }
      }
      box_min.append(lo);
      box_max.append(hi);
    }
    Array<float4> out(count);
    threading::parallel_for(IndexRange(count), 4096, [&](const IndexRange range) {
      for (const int64_t i : range) {
        if (label[i] < 0) {
          out[i] = float4(0.0f);
          continue;
        }
        const int2 lo = box_min[label[i]];
        const int2 extent = box_max[label[i]] - lo + 1;
        const float2 local = (float2(unwrapped[i] - lo) + 0.5f) / float2(extent);
        out[i] = float4(local.x, local.y, float(extent.x) / size.x, float(extent.y) / size.y);
      }
    });
    this->out_colors("Flood Fill", domain, out);
  }
};

}  // namespace flood_fill

/** \} */

/* -------------------------------------------------------------------- */
/** \name Conversions
 * \{ */

/* Base for the nodes reading a flood fill image. */
class CellOperation : public FilterOperation {
 public:
  using FilterOperation::FilterOperation;

 protected:
  Array<Cell> cells(const int2 size)
  {
    const Array<float4> fill = this->colors("Flood Fill", size);
    Array<Cell> cells(fill.size());
    each_pixel(size, [&](const int2 p, const int64_t index) {
      cells[index] = read_cell(fill[index], p, size);
    });
    return cells;
  }

  static float random(const Cell &cell, const uint32_t seed, const uint32_t salt = 0)
  {
    return tg::hash_to_float(tg::hash(uint32_t(cell.key), uint32_t(cell.key >> 32) ^ salt, seed));
  }
};

namespace mapper {

static void node_declare(NodeDeclarationBuilder &b)
{
  color_out(b, "Image", "The pattern fitted into every shape");
  declare_fill_input(b);
  color_in(b, "Pattern", float4(1.0f), "Image placed in the box of each shape");
  float_in(b, "Scale", 1.0f, 0.01f, 10.0f, "Size of the pattern in its box");
  factor_in(b, "Rotation Random", 0.0f, "Turns each shape's pattern by a random amount");
  bool_in(b, "Mask", true, "Show the pattern only inside the shapes");
  seed_in(b);
}

class Operation : public CellOperation {
 public:
  using CellOperation::CellOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<Cell> cells = this->cells(size);
    const Array<float4> pattern = this->colors("Pattern", size);
    const float scale = math::max(this->f("Scale"), 1e-3f);
    const float rotation = this->f("Rotation Random");
    const uint32_t seed = this->seed();
    Array<float4> out(cells.size());
    each_pixel(size, [&](const int2 /*p*/, const int64_t index) {
      const Cell &cell = cells[index];
      if (!cell.valid) {
        out[index] = float4(0.0f);
        return;
      }
      const float angle = (random(cell, seed) - 0.5f) * rotation * float(M_PI * 2.0);
      const float2 uv = rotate2(cell.local - 0.5f, angle) / scale + 0.5f;
      out[index] = sample_uv_clamped(pattern.as_span(), size, uv);
    });
    this->out_colors("Image", domain, out);
  }
};

}  // namespace mapper

namespace bbox_size {

static const EnumPropertyItem measure_items[] = {
    {0, "LARGEST", 0, N_("Largest"), N_("The longer side of the box")},
    {1, "WIDTH", 0, N_("Width"), N_("The width of the box")},
    {2, "HEIGHT", 0, N_("Height"), N_("The height of the box")},
    {3, "AREA", 0, N_("Area"), N_("Square root of the box's area")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  value_out(b, "Size", "Size of each shape's box, next to the image size");
  declare_fill_input(b);
  menu_in(b, "Measure", measure_items, 0, "Which size of the box to give");
}

class Operation : public CellOperation {
 public:
  using CellOperation::CellOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<Cell> cells = this->cells(size);
    const int measure = this->m("Measure");
    Array<float> out(cells.size());
    for (const int64_t k : cells.index_range()) {
      const Cell &c = cells[k];
      if (!c.valid) {
        out[k] = 0.0f;
        continue;
      }
      switch (measure) {
        case 1:
          out[k] = c.box.x;
          break;
        case 2:
          out[k] = c.box.y;
          break;
        case 3:
          out[k] = math::sqrt(c.box.x * c.box.y);
          break;
        default:
          out[k] = math::max(c.box.x, c.box.y);
          break;
      }
    }
    this->out_values("Size", domain, out);
  }
};

}  // namespace bbox_size

namespace gradient {

static void node_declare(NodeDeclarationBuilder &b)
{
  value_out(b, "Gradient", "A black to white ramp across each shape");
  declare_fill_input(b);
  factor_in(b, "Angle", 0.0f, "Direction of the ramp, in turns");
  factor_in(b, "Angle Random", 0.0f, "Turns each shape's ramp by a random amount");
  seed_in(b);
}

class Operation : public CellOperation {
 public:
  using CellOperation::CellOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<Cell> cells = this->cells(size);
    const float angle = this->f("Angle");
    const float random_amount = this->f("Angle Random");
    const uint32_t seed = this->seed();
    Array<float> out(cells.size());
    each_pixel(size, [&](const int2 /*p*/, const int64_t index) {
      const Cell &c = cells[index];
      if (!c.valid) {
        out[index] = 0.0f;
        return;
      }
      const float turns = angle + (random(c, seed) - 0.5f) * random_amount;
      const float a = turns * float(M_PI * 2.0);
      const float2 dir(math::cos(a), math::sin(a));
      const float2 box = c.box * float2(size);
      const float2 pos = (c.local - 0.5f) * box;
      const float reach = math::abs(dir.x) * box.x * 0.5f + math::abs(dir.y) * box.y * 0.5f;
      out[index] = math::clamp(math::dot(pos, dir) / math::max(reach, 1e-6f) * 0.5f + 0.5f, 0.0f, 1.0f);
    });
    this->out_values("Gradient", domain, out);
  }
};

}  // namespace gradient

namespace to_color {

static void node_declare(NodeDeclarationBuilder &b)
{
  color_out(b, "Image", "Each shape filled with the color under its middle");
  declare_fill_input(b);
  color_in(b, "Image", float4(1.0f), "Colors or values to fill the shapes with");
}

class Operation : public CellOperation {
 public:
  using CellOperation::CellOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<Cell> cells = this->cells(size);
    const Array<float4> image = this->colors("Image", size);
    Array<float4> out(cells.size());
    each_pixel(size, [&](const int2 /*p*/, const int64_t index) {
      const Cell &c = cells[index];
      out[index] = c.valid ? sample_uv(image.as_span(), size, c.center) : float4(0.0f);
    });
    this->out_colors("Image", domain, out);
  }
};

}  // namespace to_color

namespace to_index {

static void node_declare(NodeDeclarationBuilder &b)
{
  value_out(b, "Index", "Number of each shape, bottom to top, divided by the shape count");
  declare_fill_input(b);
}

class Operation : public CellOperation {
 public:
  using CellOperation::CellOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<Cell> cells = this->cells(size);
    Vector<int64_t> keys;
    Map<int64_t, int> index_of;
    for (const Cell &c : cells) {
      if (c.valid && !index_of.contains(c.key)) {
        index_of.add(c.key, 0);
        keys.append(c.key);
      }
    }
    std::sort(keys.begin(), keys.end());
    for (const int64_t i : keys.index_range()) {
      index_of.lookup(keys[i]) = int(i);
    }
    const float count = float(math::max(int(keys.size()), 1));
    Array<float> out(cells.size());
    for (const int64_t k : cells.index_range()) {
      out[k] = cells[k].valid ? float(index_of.lookup(cells[k].key) + 1) / count : 0.0f;
    }
    this->out_values("Index", domain, out);
  }
};

}  // namespace to_index

namespace to_position {

static void node_declare(NodeDeclarationBuilder &b)
{
  color_out(b, "Position", "Middle of each shape's box in red and green");
  declare_fill_input(b);
}

class Operation : public CellOperation {
 public:
  using CellOperation::CellOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<Cell> cells = this->cells(size);
    Array<float4> out(cells.size());
    for (const int64_t k : cells.index_range()) {
      const Cell &c = cells[k];
      out[k] = c.valid ? float4(c.center.x, c.center.y, 0.0f, 1.0f) : float4(0.0f);
    }
    this->out_colors("Position", domain, out);
  }
};

}  // namespace to_position

namespace to_random {

static void declare_color(NodeDeclarationBuilder &b)
{
  color_out(b, "Image", "A random color for each shape");
  declare_fill_input(b);
  seed_in(b);
}

static void declare_gray(NodeDeclarationBuilder &b)
{
  value_out(b, "Image", "A random gray for each shape");
  declare_fill_input(b);
  seed_in(b);
}

class ColorOperation : public CellOperation {
 public:
  using CellOperation::CellOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<Cell> cells = this->cells(size);
    const uint32_t seed = this->seed();
    Array<float4> out(cells.size());
    for (const int64_t k : cells.index_range()) {
      const Cell &c = cells[k];
      out[k] = c.valid ? float4(random(c, seed, 1), random(c, seed, 2), random(c, seed, 3), 1.0f) :
                         float4(0.0f);
    }
    this->out_colors("Image", domain, out);
  }
};

class GrayOperation : public CellOperation {
 public:
  using CellOperation::CellOperation;

  void execute() override
  {
    const Domain domain = this->compute_domain();
    const int2 size = domain.data_size;
    const Array<Cell> cells = this->cells(size);
    const uint32_t seed = this->seed();
    Array<float> out(cells.size());
    for (const int64_t k : cells.index_range()) {
      out[k] = cells[k].valid ? random(cells[k], seed) : 0.0f;
    }
    this->out_values("Image", domain, out);
  }
};

}  // namespace to_random

/** \} */

static void node_register()
{
  static bke::bNodeType types[10];
  int k = 0;
  register_filter(types[k++], "TextureGraphNodeFloodFill", "Flood Fill",
                  "Find the separate shapes of a mask and their boxes",
                  "TEXTURE_GRAPH_FLOOD_FILL", NODE_CLASS_OP_FILTER, flood_fill::node_declare,
                  make_operation<flood_fill::Operation>);
  register_filter(types[k++], "TextureGraphNodeFloodFillMapper", "Flood Fill Mapper",
                  "Fit a pattern into the box of every shape", "TEXTURE_GRAPH_FLOOD_FILL_MAPPER",
                  NODE_CLASS_OP_FILTER, mapper::node_declare, make_operation<mapper::Operation>);
  register_filter(types[k++], "TextureGraphNodeFloodFillToBBoxSize", "Flood Fill to BBox Size",
                  "Fill each shape with the size of its box",
                  "TEXTURE_GRAPH_FLOOD_FILL_TO_BBOX_SIZE", NODE_CLASS_OP_FILTER,
                  bbox_size::node_declare, make_operation<bbox_size::Operation>);
  register_filter(types[k++], "TextureGraphNodeFloodFillToGradient", "Flood Fill to Gradient",
                  "Fill each shape with a ramp", "TEXTURE_GRAPH_FLOOD_FILL_TO_GRADIENT",
                  NODE_CLASS_OP_FILTER, gradient::node_declare,
                  make_operation<gradient::Operation>);
  register_filter(types[k++], "TextureGraphNodeFloodFillToColor", "Flood Fill to Color",
                  "Fill each shape with the color under its middle",
                  "TEXTURE_GRAPH_FLOOD_FILL_TO_COLOR", NODE_CLASS_OP_FILTER,
                  to_color::node_declare, make_operation<to_color::Operation>);
  register_filter(types[k++], "TextureGraphNodeFloodFillToIndex", "Flood Fill to Index",
                  "Number the shapes from bottom to top", "TEXTURE_GRAPH_FLOOD_FILL_TO_INDEX",
                  NODE_CLASS_OP_FILTER, to_index::node_declare,
                  make_operation<to_index::Operation>);
  register_filter(types[k++], "TextureGraphNodeFloodFillToPosition", "Flood Fill to Position",
                  "Fill each shape with the position of its middle",
                  "TEXTURE_GRAPH_FLOOD_FILL_TO_POSITION", NODE_CLASS_OP_FILTER,
                  to_position::node_declare, make_operation<to_position::Operation>);
  register_filter(types[k++], "TextureGraphNodeFloodFillToRandomColor",
                  "Flood Fill to Random Color", "Fill each shape with a random color",
                  "TEXTURE_GRAPH_FLOOD_FILL_TO_RANDOM_COLOR", NODE_CLASS_OP_FILTER,
                  to_random::declare_color, make_operation<to_random::ColorOperation>);
  register_filter(types[k++], "TextureGraphNodeFloodFillToRandomGrayscale",
                  "Flood Fill to Random Grayscale", "Fill each shape with a random gray",
                  "TEXTURE_GRAPH_FLOOD_FILL_TO_RANDOM_GRAYSCALE", NODE_CLASS_OP_FILTER,
                  to_random::declare_gray, make_operation<to_random::GrayOperation>);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_flood_fill_cc
