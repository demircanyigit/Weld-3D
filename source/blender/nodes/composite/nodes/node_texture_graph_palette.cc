/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Palette nodes of the Texture Graph. A palette is an image one pixel high with one color per
 * pixel, from left to right. An ID map stores palette index N as N / 255. Quantize Color makes
 * all three from an image; Apply, Create, Modify and View Color Palette work with them.
 */

#include <algorithm>
#include <cfloat>
#include <string>
#include <numeric>

#include "BLI_math_vector.hh"
#include "BLI_vector.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"
#include "texture_graph_util.hh"

namespace blender::nodes::node_texture_graph_palette_cc {

namespace tg = texture_graph;
using namespace blender::compositor;

constexpr int max_palette_size = 256;

enum class Space : int {
  Lab = 0,
  RGB = 1,
};

inline const EnumPropertyItem space_items[] = {
    {int(Space::Lab), "LAB", 0, N_("Lab"), N_("Colors that look alike are close, for color images")},
    {int(Space::RGB), "RGB", 0, N_("RGB"), N_("Straight red, green and blue, for data images")},
    {0, nullptr, 0, nullptr, nullptr},
};

/* A color in the space distances are measured in, scaled so black to white is about 1. */
inline float3 to_space(const float3 &rgb, const Space space)
{
  if (space == Space::Lab) {
    return tg::rgb_to_lab(rgb) / 100.0f;
  }
  return rgb;
}

/* Reads the colors of a palette image. A single value is a palette of one color. */
inline Vector<float3> read_palette(const Result &palette, const int amount = 0)
{
  Vector<float3> colors;
  if (palette.is_single_value()) {
    colors.append(float4(palette.get_single_value<Color>()).xyz());
    return colors;
  }
  const int2 size = palette.domain().data_size;
  int count = math::min(size.x, max_palette_size);
  if (amount > 0) {
    count = math::min(count, amount);
  }
  for (int i = 0; i < count; i++) {
    colors.append(float4(palette.load_pixel<Color>(int2(i, 0))).xyz());
  }
  return colors;
}

inline void write_palette(Result &result, const Span<float3> colors)
{
  const int count = math::max(int(colors.size()), 1);
  result.allocate_texture(Domain(int2(count, 1)));
  for (int i = 0; i < count; i++) {
    const float3 c = i < colors.size() ? colors[i] : float3(0.0f);
    result.store_pixel(int2(i, 0), Color(float4(c, 1.0f)));
  }
}

inline void write_amount(Result &result, const int amount)
{
  if (result.should_compute()) {
    result.allocate_single_value();
    result.set_single_value(amount);
  }
}

/* The domain a palette node draws an ID map onto. */
inline Domain id_domain(const Context &context, const Result &id)
{
  return id.is_single_value() ? context.get_compositing_domain() : id.domain();
}

/* Recolors an ID map with a palette. */
inline void apply_palette(const Context &context,
                          const Result &id,
                          const Span<float3> colors,
                          Result &output)
{
  const auto color_of = [&](const float value) {
    const int index = math::min(tg::id_from_value(value), int(colors.size()) - 1);
    return Color(float4(colors[index], 1.0f));
  };
  if (id.is_single_value()) {
    output.allocate_single_value();
    output.set_single_value(color_of(id.get_single_value<float>()));
    return;
  }
  const Domain domain = id_domain(context, id);
  output.allocate_texture(domain);
  parallel_for(domain.data_size, [&](const int2 texel) {
    output.store_pixel(texel, color_of(id.load_pixel<float>(texel)));
  });
}

static void declare_palette_input(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>("Palette")
      .default_value({0.0f, 0.0f, 0.0f, 1.0f})
      .hide_value()
      .compositor_realization_mode(CompositorInputRealizationMode::None)
      .structure_type(StructureType::Dynamic)
      .description("Row of colors, as made by Quantize Color or Create Color Palette");
}

}  // namespace blender::nodes::node_texture_graph_palette_cc

/* -------------------------------------------------------------------- */
/** \name Quantize Color
 * \{ */

namespace blender::nodes::node_texture_graph_quantize_color_cc {

namespace tg = texture_graph;
namespace pl = node_texture_graph_palette_cc;
using namespace blender::compositor;

enum class Dither : int {
  Bayer = 0,
  Noise = 1,
};

static const EnumPropertyItem dither_items[] = {
    {int(Dither::Bayer), "BAYER", 0, N_("Bayer"), N_("Even, regular dots")},
    {int(Dither::Noise), "NOISE", 0, N_("Noise"), N_("Random dots")},
    {0, nullptr, 0, nullptr, nullptr},
};

enum class Sort : int {
  Luminance = 0,
  Hue = 1,
  Usage = 2,
};

static const EnumPropertyItem sort_items[] = {
    {int(Sort::Luminance), "LUMINANCE", 0, N_("Brightness"), N_("Dark to bright")},
    {int(Sort::Hue), "HUE", 0, N_("Hue"), N_("Around the color wheel")},
    {int(Sort::Usage), "USAGE", 0, N_("Usage"), N_("Most used first")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Color>("Image");
  b.add_output<decl::Float>("ID").description("Palette index of each pixel, index N is N / 255");
  b.add_output<decl::Color>("Palette").description("Row of the colors left");
  b.add_output<decl::Int>("Amount").description("Number of colors in the palette");

  b.add_input<decl::Color>("Image").default_value({0.5f, 0.5f, 0.5f, 1.0f}).hide_value();
  b.add_input<decl::Int>("Colors").default_value(8).min(2).max(256).description(
      "Most colors to keep, fewer when the image has fewer");
  b.add_input<decl::Float>("Smoothing").default_value(0.0f).min(0.0f).max(16.0f).description(
      "Blur before picking colors, so regions form cleaner shapes");
  b.add_input<decl::Float>("Dithering").default_value(0.0f).min(0.0f).max(1.0f).subtype(
      PROP_FACTOR).description("Mix the colors left in a dot pattern to fake the lost shades");
  b.add_input<decl::Menu>("Pattern")
      .default_value(MenuValue(Dither::Bayer))
      .static_items(dither_items)
      .description("Dot pattern of the dithering");
  b.add_input<decl::Bool>("Ignore Alpha").default_value(false).description(
      "Pick colors from transparent areas too");
  b.add_input<decl::Menu>("Space")
      .default_value(MenuValue(pl::Space::Lab))
      .static_items(pl::space_items)
      .description("How colors are compared");
  b.add_input<decl::Menu>("Sort")
      .default_value(MenuValue(Sort::Luminance))
      .static_items(sort_items)
      .description("Order of the palette and the IDs");
}

/* A box of colors for median cut. */
struct Box {
  int64_t start;
  int64_t end;
  int axis;
  float extent;
};

static Box make_box(const Span<float3> points, MutableSpan<int64_t> order, const int64_t start,
                    const int64_t end)
{
  float3 low(FLT_MAX);
  float3 high(-FLT_MAX);
  for (int64_t i = start; i < end; i++) {
    low = math::min(low, points[order[i]]);
    high = math::max(high, points[order[i]]);
  }
  const float3 extent = high - low;
  int axis = 0;
  if (extent.y > extent[axis]) {
    axis = 1;
  }
  if (extent.z > extent[axis]) {
    axis = 2;
  }
  return {start, end, axis, end - start > 1 ? extent[axis] : 0.0f};
}

/* 4 x 4 Bayer threshold, from 0 to 1. */
static float bayer(const int2 texel)
{
  static const int matrix[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
  return (float(matrix[(texel.y & 3) * 4 + (texel.x & 3)]) + 0.5f) / 16.0f;
}

class QuantizeColorOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &input = this->get_input("Image");
    const int wanted = math::clamp(this->get_input("Colors").get_single_value_default<int>(), 2,
                                   pl::max_palette_size);
    const float smoothing = this->get_input("Smoothing").get_single_value_default<float>();
    const float dithering = this->get_input("Dithering").get_single_value_default<float>();
    const Dither pattern = Dither(
        this->get_input("Pattern").get_single_value_default<MenuValue>().value);
    const bool ignore_alpha = this->get_input("Ignore Alpha").get_single_value_default<bool>();
    const pl::Space space = pl::Space(
        this->get_input("Space").get_single_value_default<MenuValue>().value);
    const Sort sort = Sort(this->get_input("Sort").get_single_value_default<MenuValue>().value);

    const bool single = input.is_single_value();
    const Domain domain = single ? Domain(int2(1)) : input.domain();
    const int2 size = domain.data_size;
    const Array<float4> pixels = tg::read_colors(input, size);
    Array<float4> smoothed(pixels);
    tg::blur_wrapped<float4>(smoothed, size, tg::scaled_radius(smoothing, size));

    /* Pick colors from at most about 64k pixels. */
    const int64_t count = smoothed.size();
    const int64_t step = math::max<int64_t>(1, count / 65536);
    Vector<float3> points;
    Vector<float3> point_rgb;
    for (int64_t i = 0; i < count; i += step) {
      if (!ignore_alpha && smoothed[i].w < 0.5f) {
        continue;
      }
      point_rgb.append(smoothed[i].xyz());
      points.append(pl::to_space(smoothed[i].xyz(), space));
    }
    if (points.is_empty()) {
      /* Nothing opaque, use everything. */
      for (int64_t i = 0; i < count; i += step) {
        point_rgb.append(smoothed[i].xyz());
        points.append(pl::to_space(smoothed[i].xyz(), space));
      }
    }

    /* Median cut: keep splitting the box with the widest spread at its median. */
    Array<int64_t> order(points.size());
    std::iota(order.begin(), order.end(), 0);
    Vector<Box> boxes;
    boxes.append(make_box(points, order, 0, points.size()));
    while (boxes.size() < wanted) {
      int widest = 0;
      for (const int i : boxes.index_range()) {
        if (boxes[i].extent > boxes[widest].extent) {
          widest = i;
        }
      }
      const Box box = boxes[widest];
      if (box.extent <= 1e-5f) {
        break;
      }
      const int64_t middle = (box.start + box.end) / 2;
      std::nth_element(order.begin() + box.start,
                       order.begin() + middle,
                       order.begin() + box.end,
                       [&](const int64_t a, const int64_t b) {
                         return points[a][box.axis] < points[b][box.axis];
                       });
      boxes[widest] = make_box(points, order, box.start, middle);
      boxes.append(make_box(points, order, middle, box.end));
    }

    struct Entry {
      float3 rgb;
      float uses;
    };
    Vector<Entry> entries;
    for (const Box &box : boxes) {
      float3 sum(0.0f);
      for (int64_t i = box.start; i < box.end; i++) {
        sum += point_rgb[order[i]];
      }
      entries.append({sum / float(math::max<int64_t>(box.end - box.start, 1)),
                      float(box.end - box.start)});
    }
    switch (sort) {
      case Sort::Luminance:
        std::stable_sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
          return tg::luminance(a.rgb) < tg::luminance(b.rgb);
        });
        break;
      case Sort::Hue:
        std::stable_sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
          const float3 ha = tg::rgb_to_hsv(a.rgb);
          const float3 hb = tg::rgb_to_hsv(b.rgb);
          /* Grays first, then around the wheel. */
          const float ka = ha.y < 0.05f ? -1.0f + ha.z * 0.5f : ha.x;
          const float kb = hb.y < 0.05f ? -1.0f + hb.z * 0.5f : hb.x;
          return ka < kb;
        });
        break;
      case Sort::Usage:
        std::stable_sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
          return a.uses > b.uses;
        });
        break;
    }
    Vector<float3> palette;
    Vector<float3> palette_space;
    for (const Entry &entry : entries) {
      palette.append(entry.rgb);
      palette_space.append(pl::to_space(entry.rgb, space));
    }
    const int amount = int(palette.size());
    /* Dithering pushes colors about one palette step around. */
    const float dither_spread = dithering / std::cbrt(float(amount));

    Array<int> ids(count);
    threading::parallel_for(IndexRange(size.y), 8, [&](const IndexRange rows) {
      for (const int y : rows) {
        for (int x = 0; x < size.x; x++) {
          const int64_t i = int64_t(y) * size.x + x;
          float3 rgb = smoothed[i].xyz();
          if (dither_spread > 0.0f) {
            const float t = pattern == Dither::Bayer ?
                                bayer(int2(x, y)) :
                                tg::hash_to_float(tg::hash(x, y, 7u));
            rgb += (t - 0.5f) * dither_spread;
          }
          const float3 p = pl::to_space(rgb, space);
          int best = 0;
          float best_distance = FLT_MAX;
          for (int j = 0; j < amount; j++) {
            const float d = math::distance_squared(p, palette_space[j]);
            if (d < best_distance) {
              best_distance = d;
              best = j;
            }
          }
          ids[i] = best;
        }
      }
    });

    Result &image_result = this->get_result("Image");
    Result &id_result = this->get_result("ID");
    if (single) {
      if (image_result.should_compute()) {
        image_result.allocate_single_value();
        image_result.set_single_value(Color(float4(palette[ids[0]], pixels[0].w)));
      }
      if (id_result.should_compute()) {
        id_result.allocate_single_value();
        id_result.set_single_value(tg::id_to_value(ids[0]));
      }
    }
    else {
      if (image_result.should_compute()) {
        image_result.allocate_texture(domain);
        parallel_for(size, [&](const int2 texel) {
          const int64_t i = int64_t(texel.y) * size.x + texel.x;
          image_result.store_pixel(texel, Color(float4(palette[ids[i]], pixels[i].w)));
        });
      }
      if (id_result.should_compute()) {
        id_result.allocate_texture(domain);
        parallel_for(size, [&](const int2 texel) {
          id_result.store_pixel(texel, tg::id_to_value(ids[int64_t(texel.y) * size.x + texel.x]));
        });
      }
    }
    Result &palette_result = this->get_result("Palette");
    if (palette_result.should_compute()) {
      pl::write_palette(palette_result, palette);
    }
    pl::write_amount(this->get_result("Amount"), amount);
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new QuantizeColorOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeQuantizeColor");
  ntype.ui_name = "Quantize Color";
  ntype.ui_description = "Reduce an image to a few colors, with an ID map and a palette";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_QUANTIZE_COLOR";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_quantize_color_cc

/** \} */

/* -------------------------------------------------------------------- */
/** \name Apply Color Palette
 * \{ */

namespace blender::nodes::node_texture_graph_apply_palette_cc {

namespace pl = node_texture_graph_palette_cc;
using namespace blender::compositor;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Color>("Image").structure_type(StructureType::Dynamic);
  b.add_input<decl::Float>("ID").default_value(0.0f).hide_value().structure_type(
      StructureType::Dynamic);
  pl::declare_palette_input(b);
}

class ApplyPaletteOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Vector<float3> colors = pl::read_palette(this->get_input("Palette"));
    pl::apply_palette(this->context(), this->get_input("ID"), colors, this->get_result("Image"));
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new ApplyPaletteOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeApplyPalette");
  ntype.ui_name = "Apply Color Palette";
  ntype.ui_description = "Color an ID map with the colors of a palette";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_APPLY_PALETTE";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_apply_palette_cc

/** \} */

/* -------------------------------------------------------------------- */
/** \name Create Color Palette
 * \{ */

namespace blender::nodes::node_texture_graph_create_palette_cc {

namespace pl = node_texture_graph_palette_cc;
using namespace blender::compositor;

constexpr int color_slots = 16;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Color>("Palette");
  b.add_output<decl::Int>("Amount").description("Number of colors in the palette");

  pl::declare_palette_input(b);
  b.add_input<decl::Int>("Palette Amount")
      .default_value(0)
      .min(0)
      .max(256)
      .description("Colors in the connected palette, 0 uses its whole width");
  b.add_input<decl::Int>("Count").default_value(4).min(1).max(color_slots).description(
      "How many of the colors below to add");
  PanelDeclarationBuilder &panel = b.add_panel("Colors");
  for (int i = 0; i < color_slots; i++) {
    /* A spread of hues, so a fresh node shows a usable palette. */
    const float hue = float(i) / float(color_slots);
    const float3 rgb = texture_graph::hsv_to_rgb(float3(hue, 0.6f, 0.8f));
    panel.add_input<decl::Color>("Color " + std::to_string(i + 1))
        .default_value({rgb.x, rgb.y, rgb.z, 1.0f});
  }
}

class CreatePaletteOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &palette = this->get_input("Palette");
    const int amount_in = this->get_input("Palette Amount").get_single_value_default<int>();
    const int count = math::clamp(this->get_input("Count").get_single_value_default<int>(), 1,
                                  color_slots);
    Vector<float3> colors;
    if (!palette.is_single_value()) {
      colors = pl::read_palette(palette, amount_in);
    }
    for (int i = 0; i < count && colors.size() < pl::max_palette_size; i++) {
      const Result &color = this->get_input("Color " + std::to_string(i + 1));
      colors.append(float4(color.get_single_value_default<Color>()).xyz());
    }
    Result &palette_result = this->get_result("Palette");
    if (palette_result.should_compute()) {
      pl::write_palette(palette_result, colors);
    }
    pl::write_amount(this->get_result("Amount"), int(colors.size()));
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new CreatePaletteOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeCreatePalette");
  ntype.ui_name = "Create Color Palette";
  ntype.ui_description = "Make a palette from up to 16 colors, or add them to another palette";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_CREATE_PALETTE";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_create_palette_cc

/** \} */

/* -------------------------------------------------------------------- */
/** \name Modify Color Palette
 * \{ */

namespace blender::nodes::node_texture_graph_modify_palette_cc {

namespace tg = texture_graph;
namespace pl = node_texture_graph_palette_cc;
using namespace blender::compositor;

enum class Select : int {
  Index = 0,
  Position = 1,
};

static const EnumPropertyItem select_items[] = {
    {int(Select::Index), "INDEX", 0, N_("Index"), N_("Pick the color by its place in the palette")},
    {int(Select::Position), "POSITION", 0, N_("Position"),
     N_("Pick the color found at a place on the ID map")},
    {0, nullptr, 0, nullptr, nullptr},
};

enum class Change : int {
  Replace = 0,
  HSV = 1,
};

static const EnumPropertyItem change_items[] = {
    {int(Change::Replace), "REPLACE", 0, N_("Replace"), N_("Swap in a new color")},
    {int(Change::HSV), "HSV", 0, N_("HSV"), N_("Shift its hue, saturation and value")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Color>("Image");
  b.add_output<decl::Color>("Palette");

  b.add_input<decl::Float>("ID").default_value(0.0f).hide_value().structure_type(
      StructureType::Dynamic);
  pl::declare_palette_input(b);
  b.add_input<decl::Menu>("Select")
      .default_value(MenuValue(Select::Index))
      .static_items(select_items)
      .description("How the color to change is picked");
  b.add_input<decl::Int>("Index").default_value(0).min(0).max(255).usage_by_menu(
      "Select", int(Select::Index));
  b.add_input<decl::Vector>("Position")
      .dimensions(2)
      .default_value({0.5f, 0.5f})
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .usage_by_menu("Select", int(Select::Position));
  b.add_input<decl::Float>("Spread").default_value(0.0f).min(0.0f).max(1.0f).subtype(
      PROP_FACTOR).description("Also change palette colors this close to the picked one");
  b.add_input<decl::Float>("Contrast").default_value(0.5f).min(0.0f).max(1.0f).subtype(
      PROP_FACTOR).description("Soft falloff of the spread at 0, a hard edge at 1");
  b.add_input<decl::Menu>("Space")
      .default_value(MenuValue(pl::Space::Lab))
      .static_items(pl::space_items)
      .description("How close colors are measured");
  b.add_input<decl::Menu>("Change")
      .default_value(MenuValue(Change::Replace))
      .static_items(change_items)
      .optional_label();
  b.add_input<decl::Color>("Color")
      .default_value({0.8f, 0.2f, 0.2f, 1.0f})
      .usage_by_menu("Change", int(Change::Replace));
  b.add_input<decl::Vector>("HSV")
      .default_value({0.0f, 0.0f, 0.0f})
      .min(-1.0f)
      .max(1.0f)
      .usage_by_menu("Change", int(Change::HSV))
      .description("Hue, saturation and value shifts");
  b.add_input<decl::Float>("Opacity").default_value(1.0f).min(0.0f).max(1.0f).subtype(
      PROP_FACTOR);
}

class ModifyPaletteOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &id = this->get_input("ID");
    Vector<float3> colors = pl::read_palette(this->get_input("Palette"));
    const int count = int(colors.size());
    const Select select = Select(
        this->get_input("Select").get_single_value_default<MenuValue>().value);
    const float spread = this->get_input("Spread").get_single_value_default<float>();
    const float contrast = this->get_input("Contrast").get_single_value_default<float>();
    const pl::Space space = pl::Space(
        this->get_input("Space").get_single_value_default<MenuValue>().value);
    const Change change = Change(
        this->get_input("Change").get_single_value_default<MenuValue>().value);
    const float3 replacement = float4(
        this->get_input("Color").get_single_value_default<Color>()).xyz();
    const float3 shift = this->get_input("HSV").get_single_value_default<float3>();
    const float opacity = this->get_input("Opacity").get_single_value_default<float>();

    int target;
    if (select == Select::Index) {
      target = this->get_input("Index").get_single_value_default<int>();
    }
    else {
      const float2 position = this->get_input("Position").get_single_value_default<float2>();
      if (id.is_single_value()) {
        target = tg::id_from_value(id.get_single_value<float>());
      }
      else {
        const int2 size = id.domain().data_size;
        const int2 texel = math::clamp(int2(position * float2(size)), int2(0), size - 1);
        target = tg::id_from_value(id.load_pixel<float>(texel));
      }
    }
    target = math::clamp(target, 0, count - 1);

    const float3 picked = pl::to_space(colors[target], space);
    Vector<float3> modified = colors;
    for (int j = 0; j < count; j++) {
      float weight;
      if (j == target) {
        weight = 1.0f;
      }
      else if (spread <= 1e-6f) {
        weight = 0.0f;
      }
      else {
        const float x = math::distance(pl::to_space(colors[j], space), picked) / spread;
        weight = contrast >= 1.0f - 1e-4f ? (x <= 1.0f ? 1.0f : 0.0f) :
                                            math::clamp((1.0f - x) / (1.0f - contrast), 0.0f, 1.0f);
      }
      float3 changed;
      if (change == Change::Replace) {
        changed = replacement;
      }
      else {
        float3 hsv = tg::rgb_to_hsv(colors[j]);
        hsv.x += shift.x;
        hsv.y = math::clamp(hsv.y + shift.y, 0.0f, 1.0f);
        hsv.z = math::max(hsv.z + shift.z, 0.0f);
        changed = tg::hsv_to_rgb(hsv);
      }
      modified[j] = math::interpolate(colors[j], changed, weight * opacity);
    }

    Result &image_result = this->get_result("Image");
    if (image_result.should_compute()) {
      pl::apply_palette(this->context(), id, modified, image_result);
    }
    Result &palette_result = this->get_result("Palette");
    if (palette_result.should_compute()) {
      pl::write_palette(palette_result, modified);
    }
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new ModifyPaletteOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeModifyPalette");
  ntype.ui_name = "Modify Color Palette";
  ntype.ui_description = "Change one palette color, and the ones close to it";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_MODIFY_PALETTE";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_modify_palette_cc

/** \} */

/* -------------------------------------------------------------------- */
/** \name View Color Palette
 * \{ */

namespace blender::nodes::node_texture_graph_view_palette_cc {

namespace pl = node_texture_graph_palette_cc;
using namespace blender::compositor;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Color>("Image");
  pl::declare_palette_input(b);
  b.add_input<decl::Int>("Amount").default_value(0).min(0).max(256).description(
      "Colors in the palette, 0 uses its whole width");
}

class ViewPaletteOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Vector<float3> colors = pl::read_palette(
        this->get_input("Palette"), this->get_input("Amount").get_single_value_default<int>());
    const int count = int(colors.size());
    const int columns = int(math::ceil(math::sqrt(float(count))));
    const int rows = (count + columns - 1) / columns;

    const Domain domain = this->context().get_compositing_domain();
    const int2 size = domain.data_size;
    Result &output = this->get_result("Image");
    output.allocate_texture(domain);
    parallel_for(size, [&](const int2 texel) {
      const int column = texel.x * columns / size.x;
      /* Fill from the top row down, like text. */
      const int row = (size.y - 1 - texel.y) * rows / size.y;
      const int index = row * columns + column;
      const float4 color = index < count ? float4(colors[index], 1.0f) :
                                           float4(0.1f, 0.1f, 0.1f, 1.0f);
      output.store_pixel(texel, Color(color));
    });
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new ViewPaletteOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;
  tg_node_type_base(&ntype, "TextureGraphNodeViewPalette");
  ntype.ui_name = "View Color Palette";
  ntype.ui_description = "Show a palette as a grid of color swatches";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_VIEW_PALETTE";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_view_palette_cc

/** \} */
